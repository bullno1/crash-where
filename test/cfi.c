/**
 * @file cfi.c
 * The `.eh_frame` interpreter against libdwfl: every row of every
 * executable mapping in this process, stepped from one synthetic state
 * by both, must yield the same caller.
 */
#include <blog.h>
#include "btest.h"

#ifdef TEST_HAVE_LIBDW
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <elfutils/libdwfl.h>

#include "linux/platform.h"
#endif

static btest_suite_t cfi = {
	.name = "cfi",
};

#ifdef TEST_HAVE_LIBDW

#define STACK_LEN    (256u * 1024u)
#define MAX_REPORTED 20            /* Mismatches printed in full before the rest are only counted. */

/*
 * Registers whose caller value decides the rest of a walk: the stack
 * pointer, the callee-saved set and, on arm64, the link register.
 * Caller-saved registers are meaningless across a call and the two
 * unwinders are free to disagree on them.
 */
#if defined(__x86_64__)
/**
 * DWARF register numbers from the System V AMD64 psABI, figure 3.36
 * "DWARF Register Number Mapping".
 */
#define DW_REGS(X) \
	X(RBX, 3) \
	X(RBP, 6) \
	X(RSP, 7) \
	X(R12, 12) \
	X(R13, 13) \
	X(R14, 14) \
	X(R15, 15)
#define INITIAL_REGS 17           /* rax-r15 and the return address column. */
#elif defined(__aarch64__)
/**
 * DWARF register numbers from "DWARF for the Arm 64-bit Architecture
 * (AArch64)", section 4.1 "DWARF register names".
 */
#define DW_REGS(X) \
	X(X19, 19) \
	X(X20, 20) \
	X(X21, 21) \
	X(X22, 22) \
	X(X23, 23) \
	X(X24, 24) \
	X(X25, 25) \
	X(X26, 26) \
	X(X27, 27) \
	X(X28, 28) \
	X(X29, 29) \
	X(X30, 30) \
	X(SP, 31)
#define INITIAL_REGS 32           /* x0-x30 and sp. */
#endif

typedef enum {
#define DW_REG_ENUM(NAME, NUM) DW_##NAME = NUM,
	DW_REGS(DW_REG_ENUM)
#undef DW_REG_ENUM
} dw_reg_t;

static const dw_reg_t compared[] = {
#define DW_REG_ITEM(NAME, NUM) DW_##NAME,
	DW_REGS(DW_REG_ITEM)
#undef DW_REG_ITEM
};

static const char*
dw_reg_name(int reg) {
	switch (reg) {
#define DW_REG_NAME(NAME, NUM) case DW_##NAME: return #NAME;
	DW_REGS(DW_REG_NAME)
#undef DW_REG_NAME
	default: return "?";
	}
}

/** What one unwinder found above the sampled frame. */
typedef struct {
	bool has_caller;
	uint64_t ret;
	bool exact;                /**< The frame is a signal frame, so `ret` is not a call return. */
	uint64_t regs[CW_REG_COUNT];
	uint32_t valid;
} caller_t;

/** State the libdwfl callbacks read; one synthetic thread per sample. */
typedef struct {
	cw_regs_t regs;
	uint64_t pc;
	bool served;
	int depth;
	caller_t out;
} dwfl_ctx_t;

typedef struct {
	int samples;
	int both_callers;          /**< Samples where both unwinders found a caller. */
	int mismatches;
	int cw_only[CW_REG_COUNT]; /**< Compared register known to the in-tree parser only. */
	int dw_only[CW_REG_COUNT]; /**< Compared register known to libdwfl only. */
} tally_t;

/*
 * Synthetic stack. Every word holds an aligned address inside the
 * buffer, so a rule that dereferences stack memory lands back inside
 * it and both unwinders see the same bytes wherever a chain leads.
 */
static _Alignas(16) uint8_t stack_buf[STACK_LEN];

static void
fill_stack(void) {
	uint64_t base = (uint64_t)(uintptr_t)stack_buf;
	for (size_t o = 0; o + 8 <= STACK_LEN; o += 8) {
		uint64_t v = base + ((((o / 8) * 7919u + 4096u) % (STACK_LEN - 8192u)) & ~(uint64_t)15);
		memcpy(stack_buf + o, &v, sizeof(v));
	}
}

/**
 * Registers before the step: distinct addresses inside the buffer, so
 * any register a rule uses as a base reads valid memory and a wrong
 * base shows up as a different result.
 */
static cw_regs_t
initial_regs(uint64_t pc) {
	cw_regs_t r = { 0 };
	uint64_t base = (uint64_t)(uintptr_t)stack_buf + 64u * 1024u;
	for (int i = 0; i < INITIAL_REGS; ++i) {
		r.regs[i] = base + (uint64_t)i * 1024u;
		r.valid |= 1u << i;
	}
#if defined(__x86_64__)
	r.regs[16] = pc; /* The PLT rule reads the instruction pointer. */
#else
	(void)pc;
#endif
	return r;
}

/* libdwfl side {{{ */

static int
no_debuginfo(
	Dwfl_Module* mod, void** userdata, const char* modname, Dwarf_Addr base,
	const char* file_name, const char* debuglink_file, GElf_Word debuglink_crc,
	char** debuginfo_file_name
) {
	(void)mod;
	(void)userdata;
	(void)modname;
	(void)base;
	(void)file_name;
	(void)debuglink_file;
	(void)debuglink_crc;
	(void)debuginfo_file_name;
	return -1;
}

static pid_t
next_thread(Dwfl* dwfl, void* arg, void** thread_argp) {
	(void)dwfl;
	dwfl_ctx_t* ctx = arg;
	*thread_argp = ctx;
	if (ctx->served) {
		return 0;
	}
	ctx->served = true;
	return 1; /* Any id: there is one synthetic thread. */
}

static bool
memory_read(Dwfl* dwfl, Dwarf_Addr addr, Dwarf_Word* result, void* arg) {
	(void)dwfl;
	(void)arg;
	uint64_t lo = (uint64_t)(uintptr_t)stack_buf;
	if (addr < lo || addr - lo > STACK_LEN - sizeof(*result)) {
		return false;
	}
	memcpy(result, stack_buf + (addr - lo), sizeof(*result));
	return true;
}

static bool
set_initial_registers(Dwfl_Thread* thread, void* arg) {
	dwfl_ctx_t* ctx = arg;
	if (!dwfl_thread_state_registers(thread, 0, INITIAL_REGS, ctx->regs.regs)) {
		return false;
	}
	dwfl_thread_state_register_pc(thread, ctx->pc);
	return true;
}

static int
frame_cb(Dwfl_Frame* state, void* arg) {
	dwfl_ctx_t* ctx = arg;
	if (ctx->depth++ == 0) {
		return DWARF_CB_OK; /* The sampled frame itself. */
	}
	Dwarf_Addr pc;
	bool activation;
	if (!dwfl_frame_pc(state, &pc, &activation)) {
		return DWARF_CB_ABORT;
	}
	caller_t* c = &ctx->out;
	c->has_caller = true;
	c->ret = pc;
	c->exact = activation;
	for (size_t i = 0; i < sizeof(compared) / sizeof(compared[0]); ++i) {
		int r = compared[i];
		Dwarf_Word v;
		if (dwfl_frame_reg(state, (unsigned)r, &v) == 0) {
			c->regs[r] = v;
			c->valid |= 1u << r;
		}
	}
	return DWARF_CB_ABORT;
}

static caller_t
dwfl_step(Dwfl* dwfl, dwfl_ctx_t* ctx, uint64_t pc) {
	*ctx = (dwfl_ctx_t){ .regs = initial_regs(pc), .pc = pc };
	dwfl_getthread_frames(dwfl, 1, frame_cb, ctx);
	return ctx->out;
}

/* }}} */

static caller_t
cw_step(const cw_eh_module_t* m, uint64_t pc) {
	cw_stack_t stack = {
		.data = stack_buf,
		.lo = (uint64_t)(uintptr_t)stack_buf,
		.len = STACK_LEN,
	};
	cw_regs_t regs = initial_regs(pc);
	caller_t c = { 0 };
	uint64_t ret;
	bool signal_frame;
	if (cw_eh_step(m, &stack, pc, &regs, &ret, &signal_frame) && ret != 0) {
		c.has_caller = true;
		c.ret = ret;
		c.exact = signal_frame;
		for (size_t i = 0; i < sizeof(compared) / sizeof(compared[0]); ++i) {
			int r = compared[i];
			if ((regs.valid & (1u << r)) != 0) {
				c.regs[r] = regs.regs[r];
				c.valid |= 1u << r;
			}
		}
	}
	return c;
}

static void
compare(tally_t* t, const char* path, uint64_t addr, const caller_t* cw, const caller_t* dw) {
	++t->samples;
	char why[160] = "";
	if (cw->has_caller != dw->has_caller) {
		snprintf(
			why, sizeof(why), "caller: cw %s, libdwfl %s",
			cw->has_caller ? "found" : "none", dw->has_caller ? "found" : "none"
		);
	} else if (cw->has_caller && cw->ret != dw->ret) {
		snprintf(why, sizeof(why), "return address: cw %" PRIx64 ", libdwfl %" PRIx64, cw->ret, dw->ret);
	} else if (cw->has_caller && cw->exact != dw->exact) {
		snprintf(why, sizeof(why), "signal frame: cw %d, libdwfl %d", cw->exact, dw->exact);
	} else if (cw->has_caller) {
		++t->both_callers;
		for (size_t i = 0; i < sizeof(compared) / sizeof(compared[0]) && why[0] == '\0'; ++i) {
			int r = compared[i];
			bool kc = (cw->valid & (1u << r)) != 0;
			bool kd = (dw->valid & (1u << r)) != 0;
			if (kc && kd && cw->regs[r] != dw->regs[r]) {
				snprintf(
					why, sizeof(why), "%s: cw %" PRIx64 ", libdwfl %" PRIx64,
					dw_reg_name(r), cw->regs[r], dw->regs[r]
				);
			} else if (kc && !kd) {
				++t->cw_only[r];
			} else if (kd && !kc) {
				++t->dw_only[r];
			}
		}
	}
	if (why[0] != '\0' && t->mismatches++ < MAX_REPORTED) {
		BLOG_ERROR("%s+%" PRIx64 ": %s", path, addr, why);
	}
}

/**
 * Step both unwinders from `pc` and compare. `addr` is the link-time
 * address, as a disassembler shows it.
 */
static void
sample(tally_t* t, const cw_eh_module_t* m, Dwfl* dwfl, dwfl_ctx_t* ctx, const char* path, uint64_t pc) {
	caller_t cw = cw_step(m, pc);
	caller_t dw = dwfl_step(dwfl, ctx, pc);
	compare(t, path, pc - m->bias, &cw, &dw);
}

BTEST(cfi, matches_libdwfl) {
	fill_stack();
	static const Dwfl_Callbacks callbacks = {
		.find_elf = dwfl_linux_proc_find_elf,
		.find_debuginfo = no_debuginfo,
	};
	static const Dwfl_Thread_Callbacks thread_callbacks = {
		.next_thread = next_thread,
		.memory_read = memory_read,
		.set_initial_registers = set_initial_registers,
	};
	Dwfl* dwfl = dwfl_begin(&callbacks);
	BTEST_ASSERT(dwfl != NULL);
	BTEST_ASSERT_EQUAL("%d", dwfl_linux_proc_report(dwfl, getpid()), 0);
	BTEST_ASSERT_EQUAL("%d", dwfl_report_end(dwfl, NULL, NULL), 0);
	static dwfl_ctx_t ctx;
	BTEST_ASSERT_EX(dwfl_attach_state(dwfl, NULL, getpid(), &thread_callbacks, &ctx), "%s", dwfl_errmsg(-1));

	FILE* f = fopen("/proc/self/maps", "r");
	BTEST_ASSERT(f != NULL);
	tally_t tally = { 0 };
	int modules = 0;
	char line[CW_STR_CAP + 128];
	while (fgets(line, sizeof(line), f) != NULL) {
		unsigned long start;
		unsigned long end;
		unsigned long offset;
		char perms[8];
		int consumed = 0;
		if (sscanf(line, "%lx-%lx %7s %lx %*x:%*x %*u %n", &start, &end, perms, &offset, &consumed) < 4) {
			continue;
		}
		char* path = line + consumed;
		char* nl = strchr(path, '\n');
		if (nl != NULL) {
			*nl = '\0';
		}
		if (perms[2] != 'x' || path[0] != '/') {
			continue;
		}

		Dwfl_Module* mod = dwfl_addrmodule(dwfl, start);
		Dwarf_Addr bias = 0;
		Dwarf_CFI* dw_cfi = mod != NULL ? dwfl_module_eh_cfi(mod, &bias) : NULL;
		if (dw_cfi == NULL) {
			BLOG_WARN("%s: libdwfl has no .eh_frame (%s)", path, dwfl_errmsg(-1));
			continue;
		}
		++modules;

		/* Walk the rows libdw finds and sample the first and last address of each. */
		cw_eh_module_t m = { 0 };
		int rows = 0;
		for (uint64_t pc = start; pc < end;) {
			Dwarf_Frame* frame = NULL;
			if (dwarf_cfi_addrframe(dw_cfi, pc - bias, &frame) != 0) {
				++pc;
				continue;
			}
			Dwarf_Addr row_start;
			Dwarf_Addr row_end;
			bool signal_frame;
			dwarf_frame_info(frame, &row_start, &row_end, &signal_frame);
			free(frame);
			row_start += bias;
			row_end += bias;
			if (row_end <= pc) {
				++pc;
				continue;
			}
			uint64_t first = row_start > pc ? row_start : pc;
			/* Opened with a code address: the mapping may start in padding shared with another segment. */
			if (m.state == 0 && !cw_eh_open(&m, path, start, offset, first)) {
				BTEST_EXPECT_EX(false, "%s: cw_eh_open failed", path);
				break;
			}
			if (rows == 0) {
				BLOG_DEBUG("%s: cw bias %" PRIx64 ", libdwfl bias %" PRIx64, path, m.bias, (uint64_t)bias);
			}
			sample(&tally, &m, dwfl, &ctx, path, first);
			if (row_end - 1 != first) {
				sample(&tally, &m, dwfl, &ctx, path, row_end - 1);
			}
			pc = row_end;
			++rows;
		}
		BLOG_INFO("%s: %d rows", path, rows);
		cw_eh_close(&m);
	}
	fclose(f);
	dwfl_end(dwfl);

	BTEST_EXPECT_RELATION("%d", modules, >, 0);
	BLOG_INFO("%d samples, %d with a caller from both, %d mismatches", tally.samples, tally.both_callers, tally.mismatches);
	for (size_t i = 0; i < sizeof(compared) / sizeof(compared[0]); ++i) {
		int r = compared[i];
		if (tally.cw_only[r] != 0 || tally.dw_only[r] != 0) {
			BLOG_INFO(
				"%s: known to cw only %d times, to libdwfl only %d times",
				dw_reg_name(r), tally.cw_only[r], tally.dw_only[r]
			);
		}
	}
	BTEST_EXPECT_RELATION("%d", tally.both_callers, >, tally.samples / 2);
	BTEST_EXPECT_EQUAL("%d", tally.mismatches, 0);
}

#else

BTEST(cfi, matches_libdwfl) {
	BLOG_WARN("skipped: built without libdw");
}

#endif
