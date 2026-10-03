/**
 * @file minidump.c
 * The dump attachment: a minidump on Windows and an ELF core on Linux,
 * written beside a crash or hang report, named in the envelope,
 * uploaded when the server asks for it and deleted when it declines.
 *
 * Both formats are read with readers of their own, so the checks
 * compile everywhere and the platforms without a dump assert its absence.
 */
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "btest.h"
#include "cw.h"
#include "scenario.h"

/* Scenarios (child side) {{{ */

#define STALL_CAP_S 20
#define STALL_EXIT_UNREPORTED 4

static volatile int sink;
static int* volatile bad_ptr = (int*)TEST_BAD_ADDRESS;

/** Bytes a scenario leaves on its stack for the runner to find in the dump. */
static const uint8_t marker_bytes[16] = {
	0xc5, 0x3a, 0x9e, 0x11, 0x7d, 0x42, 0xf0, 0x66,
	0x28, 0xb3, 0x5c, 0xd9, 0x84, 0x1f, 0xa7, 0x90,
};

static void
record_addr(const char* key, uintptr_t addr) {
	char buf[24];
	snprintf(buf, sizeof(buf), "%" PRIxPTR, addr);
	cw_set_state(key, buf);
}

TEST_NOINLINE static void
write_null(void) {
	*bad_ptr = 1;
	sink++;
}

/**
 * Leave the marker on the stack, record where, and crash one call
 * deeper so the frame holding it is still live.
 */
TEST_NOINLINE static void
crash_with_marker(void) {
	volatile uint8_t marker[sizeof(marker_bytes)];
	for (size_t i = 0; i < sizeof(marker); ++i) {
		marker[i] = marker_bytes[i];
	}
	record_addr("marker", (uintptr_t)marker);
	write_null();
	sink++;
}

/**
 * Spin until the watcher has reported, which the test transport records
 * in the events file; a cap ends a stall nobody reports.
 */
static void
stall_until_reported(void) {
	char path[512];
	snprintf(path, sizeof(path), "%s/events.jsonl", getenv("CW_TEST_OUT"));
	time_t start = time(NULL);
	for (uint64_t i = 1;; ++i) {
		sink++;
		if ((i & 0xffff) != 0) {
			continue;
		}
		if (test_file_size(path) > 0) {
			break;
		}
		if (time(NULL) - start > STALL_CAP_S) {
			exit(STALL_EXIT_UNREPORTED);
		}
	}
}

CW_SCENARIO(minidump_crash) {
	cw_set_state("mode", "minidump_crash");
	crash_with_marker();
}

CW_SCENARIO(minidump_hang) {
	cw_set_state("mode", "minidump_hang");
	for (int i = 0; i < 3; ++i) {
		cw_heartbeat();
	}
	stall_until_reported();
	cw_shutdown();
}

/* }}} */

/* Dump readers (runner side) {{{ */

#define MDMP_SIGNATURE   0x504d444du
#define MDMP_HEADER_SIZE 32
#define MDMP_DIR_SIZE    12
#define MDMP_THREAD_SIZE 48
#define MDMP_THREAD_LIST 3
#define MDMP_MODULE_LIST 4
#define MDMP_EXCEPTION   6

#define ELF_PT_LOAD      1
#define ELF_PT_NOTE      4
#define ELF_NT_PRSTATUS  1
#define ELF_NT_FILE      0x46494c45u
#define ELF_PRSTATUS_SIG 12  /* `pr_cursig` */
#define ELF_PRSTATUS_PID 32  /* `pr_pid` */
#define ELF_PRSTATUS_REG 112 /* `pr_reg` */
/* Index of the stack pointer in the register set a core carries. */
#if defined(__aarch64__)
#	define ELF_REG_SP 31
#else
#	define ELF_REG_SP 19
#endif

/**
 * What the checks read from a dump of either format.
 */
typedef struct {
	uint32_t threads;        /**< Threads listed. */
	bool thread_found;       /**< The thread asked for is listed. */
	uint64_t stack_size;     /**< Bytes of that thread's stack in the dump, from its stack pointer up. */
	bool marker_in_stack;    /**< The marker bytes are at the address asked for, inside that stack. */
	bool has_exception;      /**< The dump blames a signal or exception. */
	uint32_t exception_tid;  /**< Thread it blames, or the first one listed. */
	bool has_modules;        /**< The module or file-mapping list is present. */
} minidump_t;

static uint32_t
u32(const uint8_t* p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t
u64(const uint8_t* p) {
	return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32;
}

static uint8_t*
load_file(const char* path, size_t* len) {
	FILE* f = fopen(path, "rb");
	if (f == NULL) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* data = size >= 0 ? malloc((size_t)size + 1) : NULL;
	*len = data != NULL ? fread(data, 1, (size_t)size, f) : 0;
	fclose(f);
	return data;
}

/**
 * Find thread `tid` in the thread list and, when `marker` is not 0,
 * look for the marker bytes at that address inside its stack.
 */
static void
read_thread(const uint8_t* data, size_t len, const uint8_t* list, uint32_t size, uint32_t tid, uintptr_t marker, minidump_t* out) {
	out->threads = u32(list);
	if (4 + (uint64_t)out->threads * MDMP_THREAD_SIZE > size) {
		out->threads = 0;
		return;
	}
	for (uint32_t i = 0; i < out->threads; ++i) {
		const uint8_t* t = list + 4 + i * MDMP_THREAD_SIZE;
		if (u32(t) != tid) {
			continue;
		}
		out->thread_found = true;
		uint64_t start = u64(t + 24);
		uint32_t stack_size = u32(t + 32);
		uint32_t rva = u32(t + 36);
		out->stack_size = stack_size;
		if ((uint64_t)rva + stack_size > len) {
			return;
		}
		bool inside = marker >= start && marker + sizeof(marker_bytes) <= start + stack_size;
		out->marker_in_stack = inside && memcmp(data + rva + (marker - start), marker_bytes, sizeof(marker_bytes)) == 0;
		return;
	}
}

static bool
read_minidump(const uint8_t* data, size_t len, uint32_t tid, uintptr_t marker, minidump_t* out) {
	bool ok = len >= MDMP_HEADER_SIZE;
	uint32_t count = ok ? u32(data + 8) : 0;
	uint32_t dir = ok ? u32(data + 12) : 0;
	ok = ok && (uint64_t)dir + (uint64_t)count * MDMP_DIR_SIZE <= len;
	for (uint32_t i = 0; ok && i < count; ++i) {
		const uint8_t* entry = data + dir + i * MDMP_DIR_SIZE;
		uint32_t type = u32(entry);
		uint32_t size = u32(entry + 4);
		uint32_t rva = u32(entry + 8);
		if ((uint64_t)rva + size > len) {
			ok = false;
		} else if (type == MDMP_THREAD_LIST && size >= 4) {
			read_thread(data, len, data + rva, size, tid, marker, out);
		} else if (type == MDMP_MODULE_LIST) {
			out->has_modules = true;
		} else if (type == MDMP_EXCEPTION && size >= 4) {
			out->has_exception = true;
			out->exception_tid = u32(data + rva);
		}
	}
	return ok;
}

/**
 * The loadable segment holding `[addr, addr + len)`, or `NULL`.
 */
static const uint8_t*
core_segment(const uint8_t* data, size_t len, uint64_t addr, size_t want, uint64_t* seg_end) {
	uint64_t phoff = u64(data + 32);
	uint16_t phentsize = (uint16_t)(data[54] | data[55] << 8);
	uint16_t phnum = (uint16_t)(data[56] | data[57] << 8);
	for (uint16_t i = 0; i < phnum; ++i) {
		uint64_t at = phoff + (uint64_t)i * phentsize;
		if (at + 56 > len) {
			return NULL;
		}
		const uint8_t* ph = data + at;
		uint64_t offset = u64(ph + 8);
		uint64_t vaddr = u64(ph + 16);
		uint64_t filesz = u64(ph + 32);
		if (u32(ph) == ELF_PT_LOAD && addr >= vaddr && addr + want <= vaddr + filesz && offset + filesz <= len) {
			*seg_end = vaddr + filesz;
			return data + offset + (addr - vaddr);
		}
	}
	return NULL;
}

/**
 * Walk the notes of an ELF core: one `NT_PRSTATUS` per thread, with
 * its signal, id and registers, and the file-mapping note.
 */
static bool
read_core(const uint8_t* data, size_t len, uint32_t tid, uintptr_t marker, minidump_t* out) {
	if (len < 64) {
		return false;
	}
	uint64_t phoff = u64(data + 32);
	uint16_t phentsize = (uint16_t)(data[54] | data[55] << 8);
	uint16_t phnum = (uint16_t)(data[56] | data[57] << 8);
	uint64_t sp = 0;
	for (uint16_t i = 0; i < phnum; ++i) {
		uint64_t at = phoff + (uint64_t)i * phentsize;
		if (at + 56 > len) {
			return false;
		}
		const uint8_t* ph = data + at;
		if (u32(ph) != ELF_PT_NOTE) {
			continue;
		}
		uint64_t offset = u64(ph + 8);
		uint64_t filesz = u64(ph + 32);
		if (offset + filesz > len) {
			return false;
		}
		for (uint64_t pos = 0; pos + 12 <= filesz;) {
			const uint8_t* note = data + offset + pos;
			uint32_t namesz = u32(note);
			uint32_t descsz = u32(note + 4);
			uint32_t type = u32(note + 8);
			uint64_t name_pad = ((uint64_t)namesz + 3) & ~(uint64_t)3;
			uint64_t desc_pad = ((uint64_t)descsz + 3) & ~(uint64_t)3;
			const uint8_t* desc = note + 12 + name_pad;
			if (pos + 12 + name_pad + desc_pad > filesz) {
				return false;
			}
			if (type == ELF_NT_FILE) {
				out->has_modules = true;
			} else if (type == ELF_NT_PRSTATUS && descsz >= ELF_PRSTATUS_REG + (ELF_REG_SP + 1) * 8) {
				uint32_t pid = u32(desc + ELF_PRSTATUS_PID);
				if (out->threads == 0) {
					out->exception_tid = pid;
				}
				++out->threads;
				if (pid == tid) {
					out->thread_found = true;
					out->has_exception = (desc[ELF_PRSTATUS_SIG] | desc[ELF_PRSTATUS_SIG + 1] << 8) != 0;
					sp = u64(desc + ELF_PRSTATUS_REG + ELF_REG_SP * 8);
				}
			}
			pos += 12 + name_pad + desc_pad;
		}
	}
	uint64_t end;
	if (sp != 0 && core_segment(data, len, sp, 8, &end) != NULL) {
		out->stack_size = end - sp;
	}
	const uint8_t* at = marker != 0 ? core_segment(data, len, marker, sizeof(marker_bytes), &end) : NULL;
	out->marker_in_stack = at != NULL && memcmp(at, marker_bytes, sizeof(marker_bytes)) == 0;
	return true;
}

/**
 * Read whichever dump format the file holds.
 *
 * @return `false` when `path` is neither a minidump nor an ELF core.
 */
static bool
read_dump(const char* path, uint32_t tid, uintptr_t marker, minidump_t* out) {
	*out = (minidump_t){ 0 };
	size_t len;
	uint8_t* data = load_file(path, &len);
	if (data == NULL) {
		return false;
	}
	bool ok = false;
	if (len >= 4 && u32(data) == MDMP_SIGNATURE) {
		ok = read_minidump(data, len, tid, marker, out);
	} else if (len >= 4 && memcmp(data, "\x7f" "ELF", 4) == 0) {
		ok = read_core(data, len, tid, marker, out);
	}
	free(data);
	return ok;
}

/* }}} */

/* Checks (runner side) {{{ */

static btest_suite_t minidump = {
	.name = "minidump",
	.cleanup_per_test = test_run_cleanup,
};

/**
 * Path of the dump beside the envelope of `ev`, rebuilt from fields the
 * child reported.
 */
static void
dump_path(const test_run_t* run, yyjson_doc* ev, char kind, char* out, size_t cap) {
	const char* fp = test_json_str(ev, "/envelope/client_fp");
	const char* id = test_json_str(ev, "/envelope/report_id");
	snprintf(
		out, cap, "%s/report/pending/%" PRIu64 "_%c_%s_%s.dmp",
		run->dir,
		yyjson_get_uint(test_json_get(ev, "/envelope/sent_at")),
		kind,
		fp != NULL ? fp : "",
		id != NULL ? id : ""
	);
}

/**
 * The envelope names the dump exactly where the platform writes one, and
 * the file beside it is a minidump listing the reported thread.
 */
static void
check_dump(const test_run_t* run, yyjson_doc* ev, char kind, bool exception, minidump_t* dump) {
	bool named = yyjson_get_bool(test_json_get(ev, "/envelope/attachments/minidump"));
	BTEST_EXPECT_EQUAL("%d", named, TEST_HAS_MINIDUMP);
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".dmp"), TEST_HAS_MINIDUMP);
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".tmp"), 0);
	if (!TEST_HAS_MINIDUMP) {
		return;
	}
	char path[512];
	dump_path(run, ev, kind, path, sizeof(path));
	uint32_t tid = (uint32_t)yyjson_get_uint(test_json_get(ev, "/envelope/exception/thread"));
	BTEST_ASSERT_EX(read_dump(path, tid, test_state_hex(ev, "marker"), dump), "%s is not a dump", path);
	BTEST_EXPECT_RELATION("%" PRIu32, dump->threads, >=, 1);
	BTEST_EXPECT_EX(dump->thread_found, "thread %" PRIu32 " is not in the dump", tid);
	BTEST_EXPECT_RELATION("%" PRIu64, dump->stack_size, >, 0);
	BTEST_EXPECT_EQUAL("%d", dump->has_exception, exception);
	BTEST_EXPECT(dump->has_modules);
}

BTEST(minidump, crash_writes_dump) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(minidump_crash), .status = "retry");
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->exit.code, SIGSEGV);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	yyjson_doc* ev = run->events[0];
	BTEST_EXPECT(test_event_is(ev, "report", NULL));
	BTEST_ASSERT_EQUAL("%d", test_run_pending(run, ".json"), 1);

	minidump_t dump;
	check_dump(run, ev, 'c', true, &dump);
	if (!TEST_HAS_MINIDUMP) {
		return;
	}
	uint32_t tid = (uint32_t)yyjson_get_uint(test_json_get(ev, "/envelope/exception/thread"));
	BTEST_EXPECT_EQUAL("%" PRIu32, dump.exception_tid, tid);
	BTEST_EXPECT_EX(dump.marker_in_stack, "the marker is not in the stack of thread %" PRIu32, tid);
}

BTEST(minidump, hang_writes_dump) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(minidump_hang), .status = "retry", .hang_timeout_ms = 100);
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(!run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->exit.code, 0);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	yyjson_doc* ev = run->events[0];
	BTEST_EXPECT(strcmp(test_json_str(ev, "/envelope/exception/type"), "HANG") == 0);
	BTEST_ASSERT_EQUAL("%d", test_run_pending(run, ".json"), 1);

	minidump_t dump;
	check_dump(run, ev, 'h', false, &dump);
	if (!TEST_DUMP_BLAMES_FIRST) {
		return;
	}
	/* With no exception to name it, the stalled thread comes first, so it is the one a debugger selects. */
	uint32_t tid = (uint32_t)yyjson_get_uint(test_json_get(ev, "/envelope/exception/thread"));
	BTEST_EXPECT_EQUAL("%" PRIu32, dump.exception_tid, tid);
}

/** The dump follows the envelope through the attach route, then both are gone. */
BTEST(minidump, uploaded_when_wanted) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(minidump_crash), .want_attachments = true);
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1 + TEST_HAS_MINIDUMP);
	yyjson_doc* ev = run->events[0];
	BTEST_EXPECT(test_event_is(ev, "report", NULL));
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".json"), 0);
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".dmp"), 0);
	if (!TEST_HAS_MINIDUMP) {
		return;
	}

	yyjson_doc* attach = run->events[1];
	char url[512];
	snprintf(
		url, sizeof(url),
		"http://127.0.0.1:9/v1/cw-test/attach?report=%s&name=%" PRIu64 "_c_%s_%s.dmp",
		test_json_str(ev, "/envelope/report_id"),
		yyjson_get_uint(test_json_get(ev, "/envelope/sent_at")),
		test_json_str(ev, "/envelope/client_fp"),
		test_json_str(ev, "/envelope/report_id")
	);
	BTEST_EXPECT_EX(strcmp(test_json_str(attach, "/url"), url) == 0, "attach url is %s", test_json_str(attach, "/url"));
	BTEST_EXPECT(strcmp(test_json_str(attach, "/method"), "POST") == 0);
	BTEST_EXPECT(strcmp(test_json_str(attach, "/content_type"), "application/octet-stream") == 0);
	BTEST_EXPECT(yyjson_is_null(test_json_get(attach, "/token")));
	/* The dump travels gzipped, and decodes to something at least as large as a dump header. */
	BTEST_EXPECT(strcmp(test_json_str(attach, "/content_encoding"), "gzip") == 0);
	BTEST_EXPECT(yyjson_get_bool(test_json_get(attach, "/decodable")));
	BTEST_EXPECT_RELATION("%" PRIu64, yyjson_get_uint(test_json_get(attach, "/raw_len")), >, (uint64_t)MDMP_HEADER_SIZE);
	BTEST_EXPECT_RELATION("%" PRIu64, yyjson_get_uint(test_json_get(attach, "/body_len")), <, yyjson_get_uint(test_json_get(attach, "/raw_len")));
	BTEST_EXPECT(strcmp(test_json_str(ev, "/content_encoding"), "gzip") == 0);
}

/** A declined dump is deleted with its envelope and never sent. */
BTEST(minidump, deleted_when_declined) {
	const test_run_t* run = RUN_SCENARIO(SCENARIO_REF(minidump_crash));
	BTEST_ASSERT(run != NULL);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	BTEST_EXPECT(test_event_is(run->events[0], "report", NULL));
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".json"), 0);
	BTEST_EXPECT_EQUAL("%d", test_run_pending(run, ".dmp"), 0);
}

/* }}} */
