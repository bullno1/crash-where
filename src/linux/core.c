/**
 * @file linux/core.c
 * A small ELF core of the game, written by the watcher: the registers
 * and used stack of every thread, what the stacks point at, the
 * executable's data, and the loader's link map, so a debugger opens it
 * like a kernel core, without the heap.
 */
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <inttypes.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/procfs.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include "linux/platform.h"

#define CW_CORE_MAX_THREADS  256
#define CW_CORE_MAX_MAPS     1024
#define CW_CORE_MAX_RANGES   4096
#define CW_CORE_STACK_CAP    (1024u * 1024u)       /**< Per thread, from its stack pointer up. */
#define CW_CORE_INDIRECT_CAP (2u * 1024u * 1024u)  /**< All windows around pointers found on stacks. */
#define CW_CORE_DATA_CAP     (16u * 1024u * 1024u) /**< The executable's writable mappings. */
#define CW_CORE_BEFORE       256                   /**< Window around a pointer found on a stack. */
#define CW_CORE_AFTER        1024
#define CW_CORE_CHUNK        (64u * 1024u)
#define CW_CORE_MAX_DYN      512
#define CW_CORE_MAX_LINKS    512

#ifndef NT_SIGINFO
#	define NT_SIGINFO 0x53494749
#endif
#ifndef NT_FILE
#	define NT_FILE 0x46494c45
#endif

#if defined(__x86_64__)
typedef struct user_fpregs_struct cw_fpregs_t;
#	define CW_CORE_MACHINE EM_X86_64
#elif defined(__aarch64__)
typedef struct user_fpsimd_state cw_fpregs_t;
#	define CW_CORE_MACHINE EM_AARCH64
#else
#	error "unsupported architecture"
#endif

_Static_assert(sizeof(elf_gregset_t) == sizeof(struct user_regs_struct), "prstatus registers are the ptrace register set");

/**
 * One line of `/proc/<pid>/maps`.
 */
typedef struct {
	uint64_t start;
	uint64_t end;
	uint64_t offset;
	char perms[5];
	char path[CW_STR_CAP];
} cw_map_t;

/**
 * One thread of the game, stopped under ptrace while the core is written.
 */
typedef struct {
	pid_t tid;
	bool seized;
	int sig;                   /**< Signal that arrived first, delivered on detach. */
	bool have_fp;
	struct user_regs_struct regs;
	cw_fpregs_t fp;
} cw_core_thread_t;

typedef struct {
	uint64_t start;
	uint64_t end;
} cw_range_t;

/**
 * Everything gathered before the file is laid out.
 */
typedef struct {
	pid_t game;
	uint64_t page;
	cw_map_t maps[CW_CORE_MAX_MAPS];
	int map_count;
	cw_core_thread_t threads[CW_CORE_MAX_THREADS];
	int thread_count;
	cw_range_t ranges[CW_CORE_MAX_RANGES];
	int range_count;
	cw_range_t stacks[CW_CORE_MAX_THREADS]; /**< The ranges that are stacks; pointers into them are not followed. */
	int stack_count;
	uint64_t indirect_bytes;
	uint8_t auxv[4096];
	size_t auxv_len;
	uint8_t chunk[CW_CORE_CHUNK];
} cw_core_t;

/**
 * Growable note buffer.
 */
typedef struct {
	uint8_t* data;
	size_t len;
	size_t cap;
} cw_notes_t;

/* Reading the game {{{ */

/**
 * @return Bytes read, which stop at the first unreadable page; 0 when
 *         nothing could be read.
 */
static size_t
read_mem(pid_t game, uint64_t addr, void* buf, size_t len) {
	struct iovec local = { .iov_base = buf, .iov_len = len };
	struct iovec remote = { .iov_base = (void*)(uintptr_t)addr, .iov_len = len };
	ssize_t n = process_vm_readv(game, &local, 1, &remote, 1, 0);
	return n > 0 ? (size_t)n : 0;
}

static bool
read_maps(cw_core_t* core) {
	char proc[64];
	snprintf(proc, sizeof(proc), "/proc/%d/maps", (int)core->game);
	FILE* f = fopen(proc, "r");
	if (f == NULL) {
		return false;
	}
	core->map_count = 0;
	char line[CW_STR_CAP + 128];
	while (core->map_count < CW_CORE_MAX_MAPS && fgets(line, sizeof(line), f) != NULL) {
		cw_map_t* m = &core->maps[core->map_count];
		unsigned long start;
		unsigned long end;
		unsigned long offset;
		char perms[5];
		int consumed = 0;
		if (sscanf(line, "%lx-%lx %4s %lx %*x:%*x %*u %n", &start, &end, perms, &offset, &consumed) < 4) {
			continue;
		}
		*m = (cw_map_t){ .start = start, .end = end, .offset = offset };
		memcpy(m->perms, perms, sizeof(perms));
		const char* path = line + consumed;
		size_t len = strcspn(path, "\n");
		snprintf(m->path, sizeof(m->path), "%.*s", (int)len, path);
		++core->map_count;
	}
	fclose(f);
	return core->map_count > 0;
}

/**
 * The mapping holding `addr`, or `NULL`.
 */
static const cw_map_t*
find_map(const cw_core_t* core, uint64_t addr) {
	int lo = 0;
	int hi = core->map_count;
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		const cw_map_t* m = &core->maps[mid];
		if (addr < m->start) {
			hi = mid;
		} else if (addr >= m->end) {
			lo = mid + 1;
		} else {
			return m;
		}
	}
	return NULL;
}

static bool
readable(const cw_map_t* m) {
	return m != NULL && m->perms[0] == 'r';
}

static void
read_auxv(cw_core_t* core) {
	char proc[64];
	snprintf(proc, sizeof(proc), "/proc/%d/auxv", (int)core->game);
	FILE* f = fopen(proc, "rb");
	core->auxv_len = 0;
	if (f != NULL) {
		core->auxv_len = fread(core->auxv, 1, sizeof(core->auxv), f);
		fclose(f);
	}
}

static uint64_t
auxv_get(const cw_core_t* core, uint64_t type) {
	for (size_t i = 0; i + 2 * sizeof(uint64_t) <= core->auxv_len; i += 2 * sizeof(uint64_t)) {
		uint64_t key;
		uint64_t value;
		memcpy(&key, core->auxv + i, sizeof(key));
		memcpy(&value, core->auxv + i + sizeof(key), sizeof(value));
		if (key == type) {
			return value;
		}
	}
	return 0;
}

/* }}} */

/* Choosing memory {{{ */

/**
 * Queue `[start, end)`, widened to whole pages: readers expect the
 * page-granular segments a kernel core has, and a page is never split
 * between mappings.
 */
static void
add_range(cw_core_t* core, uint64_t start, uint64_t end) {
	start &= ~(core->page - 1);
	end = (end + core->page - 1) & ~(core->page - 1);
	if (start < end && core->range_count < CW_CORE_MAX_RANGES) {
		core->ranges[core->range_count++] = (cw_range_t){ start, end };
	}
}

static bool
in_stack(const cw_core_t* core, uint64_t addr) {
	for (int i = 0; i < core->stack_count; ++i) {
		if (addr >= core->stacks[i].start && addr < core->stacks[i].end) {
			return true;
		}
	}
	return false;
}

/**
 * The used part of the stack a thread's stack pointer is on, from the
 * pointer to the end of its mapping. After an overflow the pointer is
 * in the guard, so the search steps up a few pages.
 */
static bool
add_stack(cw_core_t* core, uint64_t sp) {
	uint64_t start = sp;
	const cw_map_t* m = NULL;
	for (int attempt = 0; attempt < 4 && !readable(m); ++attempt) {
		m = find_map(core, start);
		if (!readable(m)) {
			start = (start + core->page) & ~(core->page - 1);
		}
	}
	if (!readable(m)) {
		return false;
	}
	uint64_t end = m->end - start > CW_CORE_STACK_CAP ? start + CW_CORE_STACK_CAP : m->end;
	add_range(core, start, end);
	if (core->stack_count < CW_CORE_MAX_THREADS) {
		core->stacks[core->stack_count++] = (cw_range_t){ start, end };
	}
	return true;
}

/**
 * Follow every word of a stack that points into memory a debugger
 * cannot get from a file, one hop, with a window around the target.
 * Code and read-only data are left out: the file on disk has them.
 */
static void
scan_stack(cw_core_t* core, const cw_crash_t* crash, cw_range_t stack) {
	for (uint64_t at = stack.start; at < stack.end && core->indirect_bytes < CW_CORE_INDIRECT_CAP;) {
		size_t want = stack.end - at > CW_CORE_CHUNK ? CW_CORE_CHUNK : (size_t)(stack.end - at);
		size_t got = read_mem(core->game, at, core->chunk, want);
		if (got == 0 && crash != NULL && at >= crash->sp && at < crash->sp + crash->stack_len) {
			got = crash->sp + crash->stack_len - at;
			got = got > want ? want : got;
			memcpy(core->chunk, crash->stack + (at - crash->sp), got);
		}
		if (got < sizeof(uint64_t)) {
			break;
		}
		for (size_t i = 0; i + sizeof(uint64_t) <= got; i += sizeof(uint64_t)) {
			uint64_t p;
			memcpy(&p, core->chunk + i, sizeof(p));
			const cw_map_t* m = find_map(core, p);
			if (!readable(m) || in_stack(core, p) || (m->path[0] == '/' && m->perms[1] != 'w')) {
				continue;
			}
			uint64_t lo = p - m->start > CW_CORE_BEFORE ? p - CW_CORE_BEFORE : m->start;
			uint64_t hi = m->end - p > CW_CORE_AFTER ? p + CW_CORE_AFTER : m->end;
			add_range(core, lo, hi);
			core->indirect_bytes += hi - lo;
		}
		at += got;
	}
}

/**
 * The executable's writable mappings, including the anonymous tail of
 * its `.bss`, so its globals are visible. Too large a data segment is
 * left out rather than blow the attachment budget.
 */
static void
add_exe_data(cw_core_t* core) {
	char link[64];
	char exe[CW_STR_CAP];
	snprintf(link, sizeof(link), "/proc/%d/exe", (int)core->game);
	ssize_t n = readlink(link, exe, sizeof(exe) - 1);
	if (n <= 0) {
		return;
	}
	exe[n] = '\0';
	int first = core->range_count;
	uint64_t bytes = 0;
	uint64_t prev_end = 0;
	for (int i = 0; i < core->map_count; ++i) {
		const cw_map_t* m = &core->maps[i];
		bool own = strcmp(m->path, exe) == 0 || (m->path[0] == '\0' && m->start == prev_end);
		if (own && m->perms[0] == 'r' && m->perms[1] == 'w') {
			add_range(core, m->start, m->end);
			bytes += m->end - m->start;
			prev_end = m->end;
		} else {
			prev_end = strcmp(m->path, exe) == 0 ? m->end : 0;
		}
	}
	if (bytes > CW_CORE_DATA_CAP) {
		cw_log(CW_LOG_WARN, "executable data is %" PRIu64 " MB, left out of the core", bytes >> 20);
		core->range_count = first;
	}
}

/**
 * The executable's dynamic section, as patched by the loader, and the
 * loader's list of what it mapped, so a debugger finds the shared
 * libraries: `_r_debug` and every link map entry with its name.
 */
static void
add_link_map(cw_core_t* core) {
	uint64_t phdr = auxv_get(core, AT_PHDR);
	uint64_t phnum = auxv_get(core, AT_PHNUM);
	if (phdr == 0 || phnum == 0 || phnum > 64) {
		return;
	}
	Elf64_Phdr ph[64];
	if (read_mem(core->game, phdr, ph, phnum * sizeof(Elf64_Phdr)) != phnum * sizeof(Elf64_Phdr)) {
		return;
	}
	uint64_t bias = 0;
	uint64_t dynamic = 0;
	uint64_t dynamic_size = 0;
	for (uint64_t i = 0; i < phnum; ++i) {
		if (ph[i].p_type == PT_PHDR) {
			bias = phdr - ph[i].p_vaddr;
		} else if (ph[i].p_type == PT_DYNAMIC) {
			dynamic = ph[i].p_vaddr;
			dynamic_size = ph[i].p_memsz;
		}
	}
	if (dynamic == 0) {
		return;
	}
	dynamic += bias;
	/* The whole segment: a reader may ask for every byte the header promises. */
	add_range(core, dynamic, dynamic + dynamic_size);

	Elf64_Dyn dyn[CW_CORE_MAX_DYN];
	size_t got = read_mem(core->game, dynamic, dyn, sizeof(dyn));
	uint64_t r_debug_addr = 0;
	for (size_t i = 0; (i + 1) * sizeof(Elf64_Dyn) <= got && dyn[i].d_tag != DT_NULL; ++i) {
		if (dyn[i].d_tag == DT_DEBUG) {
			r_debug_addr = dyn[i].d_un.d_ptr;
		}
	}
	if (r_debug_addr == 0) {
		return;
	}

	struct r_debug rd;
	if (read_mem(core->game, r_debug_addr, &rd, sizeof(rd)) != sizeof(rd)) {
		return;
	}
	add_range(core, r_debug_addr, r_debug_addr + sizeof(rd));
	uint64_t lm = (uint64_t)(uintptr_t)rd.r_map;
	for (int i = 0; i < CW_CORE_MAX_LINKS && lm != 0; ++i) {
		struct link_map entry;
		if (read_mem(core->game, lm, &entry, sizeof(entry)) != sizeof(entry)) {
			break;
		}
		add_range(core, lm, lm + sizeof(entry));
		uint64_t name = (uint64_t)(uintptr_t)entry.l_name;
		if (name != 0) {
			char text[CW_STR_CAP];
			size_t len = read_mem(core->game, name, text, sizeof(text));
			add_range(core, name, name + strnlen(text, len) + 1);
		}
		lm = (uint64_t)(uintptr_t)entry.l_next;
	}
}

static void
add_vdso(cw_core_t* core) {
	for (int i = 0; i < core->map_count; ++i) {
		if (strcmp(core->maps[i].path, "[vdso]") == 0) {
			add_range(core, core->maps[i].start, core->maps[i].end);
		}
	}
}

static int
range_cmp(const void* a, const void* b) {
	const cw_range_t* x = a;
	const cw_range_t* y = b;
	return x->start < y->start ? -1 : x->start > y->start ? 1 : 0;
}

/**
 * Sort the ranges and fold overlapping or touching ones together.
 */
static void
merge_ranges(cw_core_t* core) {
	qsort(core->ranges, (size_t)core->range_count, sizeof(cw_range_t), range_cmp);
	int n = 0;
	for (int i = 0; i < core->range_count; ++i) {
		if (n > 0 && core->ranges[i].start <= core->ranges[n - 1].end) {
			if (core->ranges[i].end > core->ranges[n - 1].end) {
				core->ranges[n - 1].end = core->ranges[i].end;
			}
		} else {
			core->ranges[n++] = core->ranges[i];
		}
	}
	core->range_count = n;
}

/* }}} */

/* Threads {{{ */

/**
 * Stop every thread of the game and take its registers. A thread that
 * cannot be seized is left out.
 */
static void
seize_threads(cw_core_t* core) {
	char proc[64];
	snprintf(proc, sizeof(proc), "/proc/%d/task", (int)core->game);
	DIR* dir = opendir(proc);
	if (dir == NULL) {
		return;
	}
	for (struct dirent* e = readdir(dir); e != NULL && core->thread_count < CW_CORE_MAX_THREADS; e = readdir(dir)) {
		char* end;
		long tid = strtol(e->d_name, &end, 10);
		if (end == e->d_name || *end != '\0') {
			continue;
		}
		cw_core_thread_t* t = &core->threads[core->thread_count];
		*t = (cw_core_thread_t){ .tid = (pid_t)tid };
		if (ptrace(PTRACE_SEIZE, t->tid, NULL, NULL) != 0 || ptrace(PTRACE_INTERRUPT, t->tid, NULL, NULL) != 0) {
			cw_log(CW_LOG_DEBUG, "cannot stop thread %ld for the core (%s)", tid, strerror(errno));
			continue;
		}
		int status;
		pid_t w;
		while ((w = waitpid(t->tid, &status, __WALL)) < 0 && errno == EINTR) {
		}
		if (w != t->tid || !WIFSTOPPED(status)) {
			continue;
		}
		t->seized = true;
		t->sig = (status >> 16) == PTRACE_EVENT_STOP ? 0 : WSTOPSIG(status);
		struct iovec iov = { .iov_base = &t->regs, .iov_len = sizeof(t->regs) };
		if (ptrace(PTRACE_GETREGSET, t->tid, (void*)NT_PRSTATUS, &iov) != 0) {
			ptrace(PTRACE_DETACH, t->tid, NULL, (void*)(intptr_t)t->sig);
			t->seized = false;
			continue;
		}
		iov = (struct iovec){ .iov_base = &t->fp, .iov_len = sizeof(t->fp) };
		t->have_fp = ptrace(PTRACE_GETREGSET, t->tid, (void*)NT_PRFPREG, &iov) == 0;
		++core->thread_count;
	}
	closedir(dir);
}

static void
release_threads(cw_core_t* core) {
	for (int i = 0; i < core->thread_count; ++i) {
		cw_core_thread_t* t = &core->threads[i];
		if (t->seized) {
			ptrace(PTRACE_DETACH, t->tid, NULL, (void*)(intptr_t)t->sig);
			t->seized = false;
		}
	}
}

/**
 * Overlay the registers a thread had when it faulted, from the context
 * the handler saved, so the thread is seen at the fault and not parked
 * in the handler.
 */
static void
context_to_regs(const ucontext_t* uc, struct user_regs_struct* r) {
#if defined(__x86_64__)
	const greg_t* g = uc->uc_mcontext.gregs;
	r->r15 = (unsigned long long)g[REG_R15];
	r->r14 = (unsigned long long)g[REG_R14];
	r->r13 = (unsigned long long)g[REG_R13];
	r->r12 = (unsigned long long)g[REG_R12];
	r->rbp = (unsigned long long)g[REG_RBP];
	r->rbx = (unsigned long long)g[REG_RBX];
	r->r11 = (unsigned long long)g[REG_R11];
	r->r10 = (unsigned long long)g[REG_R10];
	r->r9 = (unsigned long long)g[REG_R9];
	r->r8 = (unsigned long long)g[REG_R8];
	r->rax = (unsigned long long)g[REG_RAX];
	r->rcx = (unsigned long long)g[REG_RCX];
	r->rdx = (unsigned long long)g[REG_RDX];
	r->rsi = (unsigned long long)g[REG_RSI];
	r->rdi = (unsigned long long)g[REG_RDI];
	r->rip = (unsigned long long)g[REG_RIP];
	r->eflags = (unsigned long long)g[REG_EFL];
	r->rsp = (unsigned long long)g[REG_RSP];
#elif defined(__aarch64__)
	for (int i = 0; i < 31; ++i) {
		r->regs[i] = uc->uc_mcontext.regs[i];
	}
	r->sp = uc->uc_mcontext.sp;
	r->pc = uc->uc_mcontext.pc;
	r->pstate = uc->uc_mcontext.pstate;
#endif
}

/**
 * Put the blamed thread first, which makes it the one a debugger
 * selects, and give it the fault-time registers when there are any.
 */
static void
blame_thread(cw_core_t* core, pid_t tid, const cw_crash_t* crash) {
	int at = -1;
	for (int i = 0; i < core->thread_count && at < 0; ++i) {
		if (core->threads[i].tid == tid) {
			at = i;
		}
	}
	if (at < 0) {
		if (crash == NULL || core->thread_count >= CW_CORE_MAX_THREADS) {
			return;
		}
		/* Not stoppable, but the handler's copy still describes it. */
		at = core->thread_count++;
		core->threads[at] = (cw_core_thread_t){ .tid = tid };
	}
	if (at != 0) {
		cw_core_thread_t tmp = core->threads[0];
		core->threads[0] = core->threads[at];
		core->threads[at] = tmp;
	}
	if (crash == NULL) {
		return;
	}
	cw_core_thread_t* t = &core->threads[0];
	context_to_regs(&crash->uc, &t->regs);
#if defined(__x86_64__)
	/* The vector registers at the fault are on the signal frame, which the parked thread still holds. */
	uint64_t fp = (uint64_t)(uintptr_t)crash->uc.uc_mcontext.fpregs;
	cw_fpregs_t saved;
	if (fp != 0 && read_mem(core->game, fp, &saved, sizeof(saved)) == sizeof(saved)) {
		t->fp = saved;
		t->have_fp = true;
	}
#endif
}

static uint64_t
regs_sp(const struct user_regs_struct* r) {
#if defined(__x86_64__)
	return r->rsp;
#else
	return r->sp;
#endif
}

/* }}} */

/* Writing {{{ */

static bool
notes_reserve(cw_notes_t* n, size_t extra) {
	if (n->len + extra <= n->cap) {
		return true;
	}
	size_t cap = n->cap == 0 ? 64 * 1024 : n->cap;
	while (cap < n->len + extra) {
		cap *= 2;
	}
	uint8_t* data = realloc(n->data, cap);
	if (data == NULL) {
		return false;
	}
	n->data = data;
	n->cap = cap;
	return true;
}

static bool
notes_add(cw_notes_t* n, uint32_t type, const void* desc, size_t desc_len) {
	static const char name[] = "CORE";
	size_t name_len = (sizeof(name) + 3) & ~(size_t)3;
	size_t padded = (desc_len + 3) & ~(size_t)3;
	if (!notes_reserve(n, sizeof(Elf64_Nhdr) + name_len + padded)) {
		return false;
	}
	Elf64_Nhdr hdr = { .n_namesz = sizeof(name), .n_descsz = (Elf64_Word)desc_len, .n_type = type };
	memcpy(n->data + n->len, &hdr, sizeof(hdr));
	n->len += sizeof(hdr);
	memset(n->data + n->len, 0, name_len);
	memcpy(n->data + n->len, name, sizeof(name));
	n->len += name_len;
	memset(n->data + n->len, 0, padded);
	memcpy(n->data + n->len, desc, desc_len);
	n->len += padded;
	return true;
}

/**
 * The process note: name and arguments, which a debugger prints as
 * "Core was generated by".
 */
static bool
notes_add_psinfo(cw_notes_t* n, pid_t game) {
	struct elf_prpsinfo info = { .pr_state = 0, .pr_sname = 'R', .pr_pid = game, .pr_ppid = getppid() };
	char proc[64];
	snprintf(proc, sizeof(proc), "/proc/%d/comm", (int)game);
	FILE* f = fopen(proc, "r");
	if (f != NULL) {
		fread(info.pr_fname, 1, sizeof(info.pr_fname) - 1, f);
		fclose(f);
		info.pr_fname[strcspn(info.pr_fname, "\n")] = '\0';
	}
	snprintf(proc, sizeof(proc), "/proc/%d/cmdline", (int)game);
	f = fopen(proc, "rb");
	if (f != NULL) {
		size_t len = fread(info.pr_psargs, 1, sizeof(info.pr_psargs) - 1, f);
		fclose(f);
		for (size_t i = 0; i + 1 < len; ++i) {
			if (info.pr_psargs[i] == '\0') {
				info.pr_psargs[i] = ' ';
			}
		}
	}
	return notes_add(n, NT_PRPSINFO, &info, sizeof(info));
}

static bool
notes_add_thread(cw_notes_t* n, const cw_core_thread_t* t, pid_t game, int signo) {
	struct elf_prstatus st = {
		.pr_info = { .si_signo = signo },
		.pr_cursig = (short)signo,
		.pr_pid = t->tid,
		.pr_ppid = game,
		.pr_pgrp = game,
		.pr_fpvalid = t->have_fp,
	};
	memcpy(&st.pr_reg, &t->regs, sizeof(st.pr_reg));
	bool ok = notes_add(n, NT_PRSTATUS, &st, sizeof(st));
	if (ok && t->have_fp) {
		ok = notes_add(n, NT_FPREGSET, &t->fp, sizeof(t->fp));
	}
	return ok;
}

/**
 * The file-backed mappings, which a debugger uses to find the
 * executable and the libraries on disk.
 */
static bool
notes_add_files(cw_notes_t* n, const cw_core_t* core) {
	long page = sysconf(_SC_PAGESIZE);
	size_t count = 0;
	size_t names = 0;
	for (int i = 0; i < core->map_count; ++i) {
		if (core->maps[i].path[0] == '/') {
			++count;
			names += strlen(core->maps[i].path) + 1;
		}
	}
	size_t len = 2 * sizeof(uint64_t) + count * 3 * sizeof(uint64_t) + names;
	uint8_t* desc = malloc(len);
	if (desc == NULL) {
		return false;
	}
	uint64_t* words = (uint64_t*)desc;
	words[0] = count;
	words[1] = (uint64_t)page;
	char* name = (char*)(words + 2 + count * 3);
	size_t k = 0;
	for (int i = 0; i < core->map_count; ++i) {
		const cw_map_t* m = &core->maps[i];
		if (m->path[0] != '/') {
			continue;
		}
		words[2 + k * 3] = m->start;
		words[3 + k * 3] = m->end;
		words[4 + k * 3] = m->offset / (uint64_t)page;
		size_t path_len = strlen(m->path) + 1;
		memcpy(name, m->path, path_len);
		name += path_len;
		++k;
	}
	bool ok = notes_add(n, NT_FILE, desc, len);
	free(desc);
	return ok;
}

/**
 * Copy one range out of the game into the file, falling back on the
 * handler's stack copy where the game cannot be read, and on zeros
 * where neither has the bytes, so every segment keeps its size.
 */
static bool
write_range(FILE* f, cw_core_t* core, const cw_crash_t* crash, cw_range_t r) {
	for (uint64_t at = r.start; at < r.end;) {
		size_t want = r.end - at > CW_CORE_CHUNK ? CW_CORE_CHUNK : (size_t)(r.end - at);
		size_t got = read_mem(core->game, at, core->chunk, want);
		if (got == 0 && crash != NULL && at >= crash->sp && at < crash->sp + crash->stack_len) {
			got = crash->sp + crash->stack_len - at;
			got = got > want ? want : got;
			memcpy(core->chunk, crash->stack + (at - crash->sp), got);
		}
		if (got < want) {
			memset(core->chunk + got, 0, want - got);
		}
		if (fwrite(core->chunk, 1, want, f) != want) {
			return false;
		}
		at += want;
	}
	return true;
}

static bool
write_zeros(FILE* f, uint64_t count) {
	static const uint8_t zeros[4096];
	while (count > 0) {
		size_t n = count > sizeof(zeros) ? sizeof(zeros) : (size_t)count;
		if (fwrite(zeros, 1, n, f) != n) {
			return false;
		}
		count -= n;
	}
	return true;
}

static bool
write_core(FILE* f, cw_core_t* core, const cw_crash_t* crash, const cw_notes_t* notes) {
	uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
	size_t phnum = 1 + (size_t)core->range_count;
	uint64_t notes_off = sizeof(Elf64_Ehdr) + phnum * sizeof(Elf64_Phdr);
	uint64_t data_off = (notes_off + notes->len + page - 1) & ~(page - 1);

	Elf64_Ehdr eh = {
		.e_ident = { ELFMAG0, ELFMAG1, ELFMAG2, ELFMAG3, ELFCLASS64, ELFDATA2LSB, EV_CURRENT, ELFOSABI_NONE },
		.e_type = ET_CORE,
		.e_machine = CW_CORE_MACHINE,
		.e_version = EV_CURRENT,
		.e_phoff = sizeof(Elf64_Ehdr),
		.e_ehsize = sizeof(Elf64_Ehdr),
		.e_phentsize = sizeof(Elf64_Phdr),
		.e_phnum = (Elf64_Half)phnum,
	};
	if (fwrite(&eh, sizeof(eh), 1, f) != 1) {
		return false;
	}
	Elf64_Phdr ph = {
		.p_type = PT_NOTE,
		.p_offset = notes_off,
		.p_filesz = notes->len,
		.p_align = 4,
	};
	if (fwrite(&ph, sizeof(ph), 1, f) != 1) {
		return false;
	}
	uint64_t off = data_off;
	for (int i = 0; i < core->range_count; ++i) {
		const cw_range_t* r = &core->ranges[i];
		ph = (Elf64_Phdr){
			.p_type = PT_LOAD,
			.p_flags = PF_R | PF_W,
			.p_offset = off,
			.p_vaddr = r->start,
			.p_filesz = r->end - r->start,
			.p_memsz = r->end - r->start,
			.p_align = page,
		};
		if (fwrite(&ph, sizeof(ph), 1, f) != 1) {
			return false;
		}
		off = (off + ph.p_filesz + page - 1) & ~(page - 1);
	}
	if (fwrite(notes->data, 1, notes->len, f) != notes->len) {
		return false;
	}
	if (!write_zeros(f, data_off - notes_off - notes->len)) {
		return false;
	}
	for (int i = 0; i < core->range_count; ++i) {
		const cw_range_t* r = &core->ranges[i];
		uint64_t len = r->end - r->start;
		uint64_t padded = (len + page - 1) & ~(page - 1);
		if (!write_range(f, core, crash, *r) || !write_zeros(f, padded - len)) {
			return false;
		}
	}
	return true;
}

/* }}} */

bool
cw_write_core(pid_t game, pid_t tid, const cw_crash_t* crash, char* out, size_t cap) {
	int n = snprintf(out, cap, "%s/pending/%d.dmp.tmp", cw_ctx.report_dir, (int)getpid());
	if (n <= 0 || (size_t)n >= cap) {
		out[0] = '\0';
		return false;
	}

	static cw_core_t core;
	core = (cw_core_t){ .game = game, .page = (uint64_t)sysconf(_SC_PAGESIZE) };
	if (!read_maps(&core)) {
		cw_log(CW_LOG_WARN, "cannot read the game's mappings, no core written");
		out[0] = '\0';
		return false;
	}
	read_auxv(&core);
	seize_threads(&core);
	blame_thread(&core, tid, crash);
	if (core.thread_count == 0) {
		cw_log(CW_LOG_WARN, "no thread of the game could be read, no core written");
		out[0] = '\0';
		return false;
	}

	/* Stacks first: they are the ranges the scan must not follow pointers into. */
	for (int i = 0; i < core.thread_count; ++i) {
		add_stack(&core, regs_sp(&core.threads[i].regs));
	}
	int stacks = core.stack_count;
	for (int i = 0; i < stacks; ++i) {
		scan_stack(&core, i == 0 ? crash : NULL, core.stacks[i]);
	}
	add_exe_data(&core);
	add_link_map(&core);
	add_vdso(&core);
	merge_ranges(&core);

	int signo = crash != NULL ? crash->signo : 0;
	cw_notes_t notes = { 0 };
	bool ok = notes_add_psinfo(&notes, game);
	for (int i = 0; ok && i < core.thread_count; ++i) {
		ok = notes_add_thread(&notes, &core.threads[i], game, i == 0 ? signo : 0);
	}
	if (ok && crash != NULL) {
		ok = notes_add(&notes, NT_SIGINFO, &crash->si, sizeof(crash->si));
	}
	if (ok && core.auxv_len > 0) {
		ok = notes_add(&notes, NT_AUXV, core.auxv, core.auxv_len);
	}
	ok = ok && notes_add_files(&notes, &core);

	FILE* f = ok ? fopen(out, "wb") : NULL;
	if (f == NULL) {
		cw_log(CW_LOG_WARN, "cannot create %s (%s)", out, strerror(errno));
		ok = false;
	} else {
		ok = write_core(f, &core, crash, &notes);
		ok = fclose(f) == 0 && ok;
	}
	release_threads(&core);
	free(notes.data);
	if (!ok) {
		cw_log(CW_LOG_WARN, "cannot write the core to %s", out);
		cw_platform_remove(out);
		out[0] = '\0';
		return false;
	}
	cw_log(CW_LOG_DEBUG, "core holds %d threads and %d ranges", core.thread_count, core.range_count);
	return true;
}
