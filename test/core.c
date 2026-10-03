/**
 * @file core.c
 * The ELF core against libdwfl, the reader behind `eu-stack --core`:
 * reported with the executable, attached, and unwound, the crashing
 * thread's frames must match the envelope's, and the libraries must be
 * found through the link map the core carries.
 */
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <blog.h>
#include "btest.h"
#include "cw.h"
#include "scenario.h"

#ifdef TEST_HAVE_LIBDW
#include <elfutils/libdwfl.h>
#include <fcntl.h>
#include <gelf.h>
#include <unistd.h>
#endif

/* Scenario (child side) {{{ */

static volatile int sink;
static int* volatile bad_ptr = (int*)TEST_BAD_ADDRESS;

static void
record_addr(const char* key, uintptr_t addr) {
	char buf[24];
	snprintf(buf, sizeof(buf), "%" PRIxPTR, addr);
	cw_set_state(key, buf);
}

TEST_NOINLINE static void
write_null(void) {
	record_addr("ret1", TEST_RETURN_ADDRESS());
	*bad_ptr = 1;
	sink++;
}

TEST_NOINLINE static void
level_two(void) {
	record_addr("ret2", TEST_RETURN_ADDRESS());
	write_null();
	sink++;
}

TEST_NOINLINE static void
level_three(void) {
	record_addr("ret3", TEST_RETURN_ADDRESS());
	level_two();
	sink++;
}

CW_SCENARIO(core_crash) {
	cw_set_state("mode", "core_crash");
	record_addr("base", test_image_base());
	level_three();
}

/* }}} */

/* Checks (runner side) {{{ */

static btest_suite_t core = {
	.name = "core",
	.cleanup_per_test = test_run_cleanup,
};

#ifdef TEST_HAVE_LIBDW

#define MAX_FRAMES 64

typedef struct {
	uintptr_t pcs[MAX_FRAMES];
	int count;
} frames_t;

static int
frame_cb(Dwfl_Frame* state, void* arg) {
	frames_t* f = arg;
	Dwarf_Addr pc;
	bool activation;
	if (!dwfl_frame_pc(state, &pc, &activation) || f->count == MAX_FRAMES) {
		return DWARF_CB_ABORT;
	}
	/* A return address is adjusted into the call, as the envelope's frames are. */
	f->pcs[f->count++] = (uintptr_t)(activation ? pc : pc - 1);
	return DWARF_CB_OK;
}

static int
count_module(Dwfl_Module* mod, void** user, const char* name, Dwarf_Addr start, void* arg) {
	(void)mod;
	(void)user;
	(void)start;
	int* n = arg;
	++*n;
	BLOG_DEBUG("module %s", name);
	return DWARF_CB_OK;
}

BTEST(core, unwinds_with_libdwfl) {
	const test_run_t* run = RUN_SCENARIO_WITH(SCENARIO_REF(core_crash), .status = "retry");
	BTEST_ASSERT(run != NULL);
	BTEST_EXPECT(run->exit.signaled);
	BTEST_EXPECT_EQUAL("%d", run->exit.code, SIGSEGV);
	BTEST_ASSERT_EQUAL("%d", run->num_events, 1);
	yyjson_doc* ev = run->events[0];
	BTEST_ASSERT(yyjson_get_bool(test_json_get(ev, "/envelope/attachments/minidump")));

	char path[512];
	snprintf(
		path, sizeof(path), "%s/report/pending/%" PRIu64 "_c_%s_%s.dmp",
		run->dir,
		yyjson_get_uint(test_json_get(ev, "/envelope/sent_at")),
		test_json_str(ev, "/envelope/client_fp"),
		test_json_str(ev, "/envelope/report_id")
	);
	char exe[512];
	BTEST_ASSERT(test_self_path(exe, sizeof(exe)));

	elf_version(EV_CURRENT);
	int fd = open(path, O_RDONLY);
	BTEST_ASSERT_EX(fd >= 0, "cannot open %s", path);
	Elf* elf = elf_begin(fd, ELF_C_READ_MMAP, NULL);
	BTEST_ASSERT_EX(elf != NULL, "%s", elf_errmsg(-1));
	static const Dwfl_Callbacks callbacks = {
		.find_elf = dwfl_build_id_find_elf,
		.find_debuginfo = dwfl_standard_find_debuginfo,
	};
	Dwfl* dwfl = dwfl_begin(&callbacks);
	BTEST_ASSERT(dwfl != NULL);
	BTEST_ASSERT_EX(dwfl_core_file_report(dwfl, elf, exe) >= 0, "%s", dwfl_errmsg(-1));
	BTEST_ASSERT_EQUAL("%d", dwfl_report_end(dwfl, NULL, NULL), 0);
	BTEST_ASSERT_EX(dwfl_core_file_attach(dwfl, elf) > 0, "%s", dwfl_errmsg(-1));

	/* The link map in the core is what lists the libraries beside the executable. */
	int modules = 0;
	dwfl_getmodules(dwfl, count_module, &modules, 0);
	BTEST_EXPECT_RELATION("%d", modules, >=, 2);

	pid_t tid = (pid_t)yyjson_get_uint(test_json_get(ev, "/envelope/exception/thread"));
	frames_t frames = { 0 };
	int rc = dwfl_getthread_frames(dwfl, tid, frame_cb, &frames);
	BTEST_ASSERT_EX(frames.count > 0, "no frames for thread %d (rc %d: %s)", (int)tid, rc, dwfl_errmsg(-1));

	size_t num_frames = yyjson_arr_size(test_json_get(ev, "/envelope/frames"));
	BTEST_ASSERT_RELATION("%zu", num_frames, >=, 4);
	for (size_t i = 0; i < 4; ++i) {
		uintptr_t want = test_frame_addr(ev, i);
		BTEST_ASSERT_EX(want != 0, "frame %zu of the envelope is outside the executable", i);
		BTEST_EXPECT_EX(
			(int)i < frames.count && frames.pcs[i] == want,
			"frame %zu: libdwfl %" PRIxPTR ", envelope %" PRIxPTR, i, (int)i < frames.count ? frames.pcs[i] : 0, want
		);
	}

	dwfl_end(dwfl);
	elf_end(elf);
	close(fd);
}

#else

BTEST(core, unwinds_with_libdwfl) {
	BLOG_WARN("skipped: built without libdw");
}

#endif

/* }}} */
