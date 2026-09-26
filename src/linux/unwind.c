/**
 * @file linux/unwind.c
 * Module table from `/proc/<pid>/maps`, ELF build ids, and a
 * frame-pointer walk over the copied stack.
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

static void
context_regs(const ucontext_t* uc, uint64_t* pc, uint64_t* sp, uint64_t* fp) {
#if defined(__x86_64__)
	*pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
	*sp = (uint64_t)uc->uc_mcontext.gregs[REG_RSP];
	*fp = (uint64_t)uc->uc_mcontext.gregs[REG_RBP];
#elif defined(__aarch64__)
	*pc = uc->uc_mcontext.pc;
	*sp = uc->uc_mcontext.sp;
	*fp = uc->uc_mcontext.regs[29];
#else
#error "unsupported architecture"
#endif
}

static void
add_frame(cw_crash_info_t* info, uint64_t addr) {
	if (info->frame_count >= CW_MAX_FRAMES) {
		return;
	}
	cw_frame_t* fr = &info->frames[info->frame_count++];
	fr->module = -1;
	fr->offset = addr;
	for (int i = 0; i < map_count; ++i) {
		if (addr >= maps[i].start && addr < maps[i].end) {
			fr->module = maps[i].module;
			fr->offset = addr - info->modules[maps[i].module].base;
			return;
		}
	}
}

static const char*
signal_name(int signo) {
	switch (signo) {
	case SIGSEGV: return "SIGSEGV";
	case SIGBUS:  return "SIGBUS";
	case SIGFPE:  return "SIGFPE";
	case SIGILL:  return "SIGILL";
	case SIGABRT: return "SIGABRT";
	case SIGTRAP: return "SIGTRAP";
	default:      return "SIGNAL";
	}
}

bool
cw_unwind(pid_t pid, const cw_crash_t* crash, cw_crash_info_t* out) {
	read_maps(pid, out);
	for (int i = 0; i < out->module_count; ++i) {
		read_build_id(out->modules[i].path, out->modules[i].build_id, sizeof(out->modules[i].build_id));
	}

	snprintf(out->type, sizeof(out->type), "%s", signal_name(crash->signo));
	out->fault_addr = (uint64_t)(uintptr_t)crash->si.si_addr;
	out->tid = (uint32_t)crash->tid;
	snprintf(
		out->message_raw, sizeof(out->message_raw),
		"si_code %d addr 0x%" PRIx64, crash->si.si_code, out->fault_addr
	);

	uint64_t pc;
	uint64_t sp;
	uint64_t fp;
	context_regs(&crash->uc, &pc, &sp, &fp);
	add_frame(out, pc);

	uint64_t lo = crash->sp;
	uint64_t hi = crash->sp + crash->stack_len;
	for (int depth = 0; fp >= lo && fp + 16 <= hi && depth < CW_MAX_DEPTH; ++depth) {
		uint64_t next;
		uint64_t ret;
		memcpy(&next, crash->stack + (fp - lo), sizeof(next));
		memcpy(&ret, crash->stack + (fp - lo) + 8, sizeof(ret));
		if (ret == 0) {
			break;
		}
		add_frame(out, ret - 1);
		if (next <= fp) {
			break;
		}
		fp = next;
	}
	return out->frame_count > 0;
}
