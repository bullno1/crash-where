/**
 * @file linux/unwind.c
 * Module table from `/proc/<pid>/maps`, ELF build ids, and the walk
 * over the copied stack: `.eh_frame` rules per module, frame pointers
 * where a module has none.
 */
#include <elf.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "linux/platform.h"

#define CW_MAX_MAPS  512
#define CW_NOTE_CAP  4096
#define CW_MAX_DEPTH 64

typedef struct {
	uint64_t start;
	uint64_t end;
	uint64_t offset;
	int module;                /**< Index into the module table. */
} mapping_t;

static mapping_t maps[CW_MAX_MAPS];
static int map_count;
static cw_eh_module_t eh_modules[CW_MAX_MODULES];

static const mapping_t*
find_map(uint64_t addr) {
	for (int i = 0; i < map_count; ++i) {
		if (addr >= maps[i].start && addr < maps[i].end) {
			return &maps[i];
		}
	}
	return NULL;
}

static int
find_module(const cw_crash_info_t* info, const char* path) {
	for (int i = 0; i < info->module_count; ++i) {
		if (strncmp(info->modules[i].path, path, CW_STR_CAP) == 0) {
			return i;
		}
	}
	return -1;
}

/**
 * Read the file-backed mappings of `pid` and build the module
 * table. Maps are sorted by address, so the first mapping of a path is
 * its load base.
 */
static void
read_maps(pid_t pid, cw_crash_info_t* info) {
	map_count = 0;
	info->module_count = 0;
	info->main_module = -1;

	char proc[64];
	snprintf(proc, sizeof(proc), "/proc/%d/exe", (int)pid);
	char exe[CW_STR_CAP];
	ssize_t exe_len = readlink(proc, exe, sizeof(exe) - 1);
	exe[exe_len > 0 ? exe_len : 0] = '\0';

	snprintf(proc, sizeof(proc), "/proc/%d/maps", (int)pid);
	FILE* f = fopen(proc, "r");
	if (f == NULL) {
		return;
	}
	char line[CW_STR_CAP + 128];
	while (map_count < CW_MAX_MAPS && fgets(line, sizeof(line), f) != NULL) {
		unsigned long start;
		unsigned long end;
		unsigned long offset;
		unsigned long inode;
		unsigned dmaj;
		unsigned dmin;
		char perms[8];
		int consumed = 0;
		if (sscanf(
			line, "%lx-%lx %7s %lx %x:%x %lu %n",
			&start, &end, perms, &offset, &dmaj, &dmin, &inode, &consumed
		) < 7) {
			continue;
		}
		char* path = line + consumed;
		if (*path != '/' || strncmp(path, "/memfd:", 7) == 0) {
			continue; /* anonymous, [stack], [vdso], our own region, ... */
		}
		char* nl = strchr(path, '\n');
		if (nl != NULL) {
			*nl = '\0';
		}
		int idx = find_module(info, path);
		if (idx < 0) {
			if (info->module_count >= CW_MAX_MODULES) {
				continue;
			}
			idx = info->module_count++;
			cw_module_t* m = &info->modules[idx];
			*m = (cw_module_t){ .base = start, .size = end - start };
			snprintf(m->path, sizeof(m->path), "%s", path);
			if (strcmp(path, exe) == 0) {
				info->main_module = idx;
			}
		} else {
			cw_module_t* m = &info->modules[idx];
			if (end - m->base > m->size) {
				m->size = end - m->base;
			}
		}
		maps[map_count++] = (mapping_t){
			.start = start,
			.end = end,
			.offset = offset,
			.module = idx,
		};
	}
	fclose(f);
}

/**
 * Read the GNU build id note of an ELF file into hex.
 */
static void
read_build_id(const char* path, char* out, size_t cap) {
	out[0] = '\0';
	FILE* f = fopen(path, "rb");
	if (f == NULL) {
		return;
	}
	Elf64_Ehdr eh;
	if (fread(&eh, sizeof(eh), 1, f) != 1
		|| memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0
		|| eh.e_ident[EI_CLASS] != ELFCLASS64) {
		fclose(f);
		return;
	}
	static uint8_t note[CW_NOTE_CAP];
	for (int i = 0; i < eh.e_phnum && out[0] == '\0'; ++i) {
		Elf64_Phdr ph;
		if (fseek(f, (long)(eh.e_phoff + (uint64_t)i * eh.e_phentsize), SEEK_SET) != 0
			|| fread(&ph, sizeof(ph), 1, f) != 1
			|| ph.p_type != PT_NOTE) {
			continue;
		}
		size_t len = ph.p_filesz < sizeof(note) ? (size_t)ph.p_filesz : sizeof(note);
		if (fseek(f, (long)ph.p_offset, SEEK_SET) != 0 || fread(note, 1, len, f) != len) {
			continue;
		}
		size_t pos = 0;
		while (pos + sizeof(Elf64_Nhdr) <= len) {
			Elf64_Nhdr nh;
			memcpy(&nh, note + pos, sizeof(nh));
			size_t name_off = pos + sizeof(nh);
			size_t desc_off = name_off + ((nh.n_namesz + 3u) & ~3u);
			size_t next = desc_off + ((nh.n_descsz + 3u) & ~3u);
			if (next > len) {
				break;
			}
			if (nh.n_type == NT_GNU_BUILD_ID && nh.n_namesz == 4 && memcmp(note + name_off, "GNU", 4) == 0) {
				size_t n = nh.n_descsz;
				if (n > (cap - 1) / 2) {
					n = (cap - 1) / 2;
				}
				for (size_t k = 0; k < n; ++k) {
					snprintf(out + 2 * k, 3, "%02x", note[desc_off + k]);
				}
				break;
			}
			pos = next;
		}
	}
	fclose(f);
}

/**
 * Expose the instruction pointer where rules can read it: the return
 * address column on x86_64 (the PLT rule inspects it); arm64 has no
 * such register.
 */
static void
set_pc_reg(cw_regs_t* regs, uint64_t pc) {
#if defined(__x86_64__)
	regs->regs[16] = pc;
	regs->valid |= 1u << 16;
#else
	(void)regs;
	(void)pc;
#endif
}

static void
context_regs(const ucontext_t* uc, cw_regs_t* regs, uint64_t* pc) {
	*regs = (cw_regs_t){ 0 };
#if defined(__x86_64__)
	/* DWARF order: rax rdx rcx rbx rsi rdi rbp rsp r8-r15. */
	static const int order[] = {
		REG_RAX, REG_RDX, REG_RCX, REG_RBX, REG_RSI, REG_RDI, REG_RBP, REG_RSP,
		REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15,
	};
	for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); ++i) {
		regs->regs[i] = (uint64_t)uc->uc_mcontext.gregs[order[i]];
		regs->valid |= 1u << i;
	}
	*pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
	set_pc_reg(regs, *pc);
#elif defined(__aarch64__)
	for (unsigned i = 0; i < 31; ++i) {
		regs->regs[i] = uc->uc_mcontext.regs[i];
	}
	regs->regs[CW_REG_SP] = uc->uc_mcontext.sp;
	regs->valid = 0xffffffffu;
	*pc = uc->uc_mcontext.pc;
#else
#error "unsupported architecture"
#endif
}

/**
 * Step one frame along the frame-pointer chain: the saved frame
 * pointer and return address sit at `fp` and `fp + 8`.
 */
static bool
fp_step(const cw_stack_t* stack, cw_regs_t* regs, uint64_t* ret) {
	uint64_t fp = regs->regs[CW_REG_FP];
	if ((regs->valid & (1u << CW_REG_FP)) == 0 || fp < stack->lo
		|| fp - stack->lo > stack->len || stack->len - (fp - stack->lo) < 16) {
		return false;
	}
	uint64_t next;
	memcpy(&next, stack->data + (fp - stack->lo), sizeof(next));
	memcpy(ret, stack->data + (fp - stack->lo) + 8, sizeof(*ret));
	regs->regs[CW_REG_FP] = next;
	regs->regs[CW_REG_SP] = fp + 16;
	regs->valid |= (1u << CW_REG_FP) | (1u << CW_REG_SP);
	return true;
}

static void
add_frame(cw_crash_info_t* info, uint64_t addr) {
	if (info->frame_count >= CW_MAX_FRAMES) {
		return;
	}
	cw_frame_t* fr = &info->frames[info->frame_count++];
	const mapping_t* map = find_map(addr);
	*fr = (cw_frame_t){
		.module = map != NULL ? map->module : -1,
		.offset = map != NULL ? addr - info->modules[map->module].base : addr,
	};
}

bool
cw_unwind(pid_t pid, const cw_crash_t* crash, cw_crash_info_t* out) {
	read_maps(pid, out);
	for (int i = 0; i < out->module_count; ++i) {
		read_build_id(out->modules[i].path, out->modules[i].build_id, sizeof(out->modules[i].build_id));
	}

	snprintf(out->type, sizeof(out->type), "%s", cw_signal_name(crash->signo));
	out->fault_addr = (uint64_t)(uintptr_t)crash->si.si_addr;
	out->tid = (uint32_t)crash->tid;
	snprintf(
		out->message_raw, sizeof(out->message_raw),
		"si_code %d addr 0x%" PRIx64, crash->si.si_code, out->fault_addr
	);

	cw_stack_t stack = { .data = crash->stack, .lo = crash->sp, .len = crash->stack_len };
	cw_regs_t regs;
	uint64_t pc;
	context_regs(&crash->uc, &regs, &pc);
	add_frame(out, pc);

	/* `pc` is the lookup address: the fault itself, then each return
	 * address moved back into its call unless a signal frame made it. */
	for (int depth = 0; depth < CW_MAX_DEPTH; ++depth) {
		uint64_t sp = regs.regs[CW_REG_SP];
		uint64_t ret = 0;
		bool signal_frame = false;
		bool stepped = false;
		const mapping_t* map = find_map(pc);
		if (map != NULL) {
			cw_eh_module_t* m = &eh_modules[map->module];
			if (m->state == 0) {
				cw_eh_open(m, out->modules[map->module].path, map->start, map->offset, pc);
			}
			stepped = cw_eh_step(m, &stack, pc, &regs, &ret, &signal_frame);
		}
		if (!stepped && !fp_step(&stack, &regs, &ret)) {
			break;
		}
		uint64_t next_sp = regs.regs[CW_REG_SP];
		if (ret == 0 || next_sp < sp || (next_sp == sp && depth > 0)) {
			break;
		}
		set_pc_reg(&regs, ret);
		pc = signal_frame ? ret : ret - 1;
		add_frame(out, pc);
	}

	for (int i = 0; i < out->module_count; ++i) {
		cw_eh_close(&eh_modules[i]);
	}
	return out->frame_count > 0;
}
