/**
 * @file linux/eh_frame.c
 * Call frame information from the `.eh_frame` of an ELF file on disk,
 * applied to the copied stack of a parked thread.
 */
#include <elf.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "linux/platform.h"

/* Pointer encodings (DW_EH_PE_*). */
#define PE_ABSPTR   0x00
#define PE_ULEB128  0x01
#define PE_UDATA2   0x02
#define PE_UDATA4   0x03
#define PE_UDATA8   0x04
#define PE_SLEB128  0x09
#define PE_SDATA2   0x0a
#define PE_SDATA4   0x0b
#define PE_SDATA8   0x0c
#define PE_PCREL    0x10
#define PE_DATAREL  0x30
#define PE_INDIRECT 0x80
#define PE_OMIT     0xff

/* Call frame instructions (DW_CFA_*). */
#define CFA_ADVANCE_LOC             0x40
#define CFA_OFFSET                  0x80
#define CFA_RESTORE                 0xc0
#define CFA_NOP                     0x00
#define CFA_SET_LOC                 0x01
#define CFA_ADVANCE_LOC1            0x02
#define CFA_ADVANCE_LOC2            0x03
#define CFA_ADVANCE_LOC4            0x04
#define CFA_OFFSET_EXTENDED         0x05
#define CFA_RESTORE_EXTENDED        0x06
#define CFA_UNDEFINED               0x07
#define CFA_SAME_VALUE              0x08
#define CFA_REGISTER                0x09
#define CFA_REMEMBER_STATE          0x0a
#define CFA_RESTORE_STATE           0x0b
#define CFA_DEF_CFA                 0x0c
#define CFA_DEF_CFA_REGISTER        0x0d
#define CFA_DEF_CFA_OFFSET          0x0e
#define CFA_DEF_CFA_EXPRESSION      0x0f
#define CFA_EXPRESSION              0x10
#define CFA_OFFSET_EXTENDED_SF      0x11
#define CFA_DEF_CFA_SF              0x12
#define CFA_DEF_CFA_OFFSET_SF       0x13
#define CFA_VAL_OFFSET              0x14
#define CFA_VAL_OFFSET_SF           0x15
#define CFA_VAL_EXPRESSION          0x16
#define CFA_AARCH64_NEGATE_RA_STATE 0x2d
#define CFA_GNU_ARGS_SIZE           0x2e
#define CFA_GNU_NEGATIVE_OFFSET_EXT 0x2f

/* Expression operations (DW_OP_*). */
#define OP_ADDR           0x03
#define OP_DEREF          0x06
#define OP_CONST1U        0x08
#define OP_CONST1S        0x09
#define OP_CONST2U        0x0a
#define OP_CONST2S        0x0b
#define OP_CONST4U        0x0c
#define OP_CONST4S        0x0d
#define OP_CONST8U        0x0e
#define OP_CONST8S        0x0f
#define OP_CONSTU         0x10
#define OP_CONSTS         0x11
#define OP_DUP            0x12
#define OP_DROP           0x13
#define OP_OVER           0x14
#define OP_PICK           0x15
#define OP_SWAP           0x16
#define OP_ROT            0x17
#define OP_ABS            0x19
#define OP_AND            0x1a
#define OP_DIV            0x1b
#define OP_MINUS          0x1c
#define OP_MOD            0x1d
#define OP_MUL            0x1e
#define OP_NEG            0x1f
#define OP_NOT            0x20
#define OP_OR             0x21
#define OP_PLUS           0x22
#define OP_PLUS_UCONST    0x23
#define OP_SHL            0x24
#define OP_SHR            0x25
#define OP_SHRA           0x26
#define OP_XOR            0x27
#define OP_BRA            0x28
#define OP_EQ             0x29
#define OP_GE             0x2a
#define OP_GT             0x2b
#define OP_LE             0x2c
#define OP_LT             0x2d
#define OP_NE             0x2e
#define OP_SKIP           0x2f
#define OP_LIT0           0x30
#define OP_LIT31          0x4f
#define OP_REG0           0x50
#define OP_REG31          0x6f
#define OP_BREG0          0x70
#define OP_BREG31         0x8f
#define OP_REGX           0x90
#define OP_BREGX          0x92
#define OP_DEREF_SIZE     0x94
#define OP_NOP            0x96
#define OP_CALL_FRAME_CFA 0x9c

#define EXPR_STACK_CAP  32
#define STATE_STACK_CAP 8

/** Bounded little-endian reader over the mapped file. */
typedef struct {
	const uint8_t* p;
	const uint8_t* end;
	bool err;
} cursor_t;

static uint64_t
read_u(cursor_t* c, size_t n) {
	if (c->err || (size_t)(c->end - c->p) < n) {
		c->err = true;
		return 0;
	}
	uint64_t v = 0;
	for (size_t i = 0; i < n; ++i) {
		v |= (uint64_t)c->p[i] << (8 * i);
	}
	c->p += n;
	return v;
}

static int64_t
read_s(cursor_t* c, size_t n) {
	uint64_t v = read_u(c, n);
	if (n < 8 && (v & ((uint64_t)1 << (8 * n - 1))) != 0) {
		v |= ~(uint64_t)0 << (8 * n);
	}
	return (int64_t)v;
}

static uint64_t
read_uleb(cursor_t* c) {
	uint64_t v = 0;
	for (unsigned shift = 0; shift < 64; shift += 7) {
		uint8_t b = (uint8_t)read_u(c, 1);
		v |= (uint64_t)(b & 0x7f) << shift;
		if ((b & 0x80) == 0 || c->err) {
			return v;
		}
	}
	c->err = true;
	return 0;
}

static int64_t
read_sleb(cursor_t* c) {
	uint64_t v = 0;
	unsigned shift = 0;
	uint8_t b;
	do {
		b = (uint8_t)read_u(c, 1);
		v |= (uint64_t)(b & 0x7f) << shift;
		shift += 7;
	} while ((b & 0x80) != 0 && shift < 64 && !c->err);
	if ((b & 0x80) != 0) {
		c->err = true;
		return 0;
	}
	if (shift < 64 && (b & 0x40) != 0) {
		v |= ~(uint64_t)0 << shift;
	}
	return (int64_t)v;
}

static void
skip(cursor_t* c, uint64_t n) {
	if (c->err || (uint64_t)(c->end - c->p) < n) {
		c->err = true;
		return;
	}
	c->p += n;
}

/* Address translation {{{ */

/** File byte for a link-time address, or NULL when not in a loaded segment. */
static const uint8_t*
vaddr_ptr(const cw_eh_module_t* m, uint64_t vaddr) {
	for (int i = 0; i < m->load_count; ++i) {
		const cw_eh_load_t* l = &m->load[i];
		if (vaddr >= l->vaddr && vaddr - l->vaddr < l->filesz) {
			uint64_t off = l->offset + (vaddr - l->vaddr);
			return off < m->file_len ? m->file + off : NULL;
		}
	}
	return NULL;
}

/** Link-time address of a file byte, or 0 when not in a loaded segment. */
static uint64_t
ptr_vaddr(const cw_eh_module_t* m, const uint8_t* p) {
	uint64_t off = (uint64_t)(p - m->file);
	for (int i = 0; i < m->load_count; ++i) {
		const cw_eh_load_t* l = &m->load[i];
		if (off >= l->offset && off - l->offset < l->filesz) {
			return l->vaddr + (off - l->offset);
		}
	}
	return 0;
}

/** Read a DW_EH_PE encoded value as a link-time address. */
static uint64_t
read_encoded(cursor_t* c, uint8_t enc, const cw_eh_module_t* m) {
	if (enc == PE_OMIT) {
		c->err = true;
		return 0;
	}
	uint64_t field = ptr_vaddr(m, c->p);
	uint64_t v;
	switch (enc & 0x0f) {
	case PE_ABSPTR:  v = read_u(c, 8); break;
	case PE_ULEB128: v = read_uleb(c); break;
	case PE_UDATA2:  v = read_u(c, 2); break;
	case PE_UDATA4:  v = read_u(c, 4); break;
	case PE_UDATA8:  v = read_u(c, 8); break;
	case PE_SLEB128: v = (uint64_t)read_sleb(c); break;
	case PE_SDATA2:  v = (uint64_t)read_s(c, 2); break;
	case PE_SDATA4:  v = (uint64_t)read_s(c, 4); break;
	case PE_SDATA8:  v = (uint64_t)read_s(c, 8); break;
	default:
		c->err = true;
		return 0;
	}
	switch (enc & 0x70) {
	case PE_ABSPTR:  break;
	case PE_PCREL:   v += field; break;
	case PE_DATAREL: v += m->hdr_vaddr; break;
	default:
		c->err = true;
		return 0;
	}
	if ((enc & PE_INDIRECT) != 0) {
		c->err = true; /* Needs the game's memory. */
		return 0;
	}
	return v;
}

/** Byte width of a fixed-size encoding, or 0 for a variable one. */
static size_t
encoded_size(uint8_t enc) {
	switch (enc & 0x0f) {
	case PE_ABSPTR: case PE_UDATA8: case PE_SDATA8: return 8;
	case PE_UDATA4: case PE_SDATA4:                 return 4;
	case PE_UDATA2: case PE_SDATA2:                 return 2;
	default:                                        return 0;
	}
}

static bool
stack_read(const cw_stack_t* s, uint64_t addr, size_t n, uint64_t* out) {
	if (addr < s->lo || addr - s->lo > s->len || s->len - (addr - s->lo) < n) {
		return false;
	}
	cursor_t c = { .p = s->data + (addr - s->lo), .end = s->data + s->len };
	*out = read_u(&c, n);
	return true;
}

/* }}} */

/* CIE and FDE {{{ */

typedef struct {
	uint64_t code_align;
	int64_t data_align;
	uint64_t ra_reg;
	uint8_t fde_enc;
	uint8_t lsda_enc;
	bool has_z;
	bool signal_frame;
	const uint8_t* instr;
	const uint8_t* instr_end;
} cie_t;

typedef struct {
	uint64_t pc_begin;
	uint64_t pc_range;
	const uint8_t* instr;
	const uint8_t* instr_end;
} fde_t;

/**
 * Read the length of a CIE or FDE at `p` and bound `c` to its body.
 *
 * @return `false` for a terminator or a 64-bit DWARF record.
 */
static bool
open_record(const cw_eh_module_t* m, const uint8_t* p, cursor_t* c) {
	if (p < m->file || p >= m->file + m->file_len) {
		return false;
	}
	*c = (cursor_t){ .p = p, .end = m->file + m->file_len };
	uint64_t len = read_u(c, 4);
	if (c->err || len == 0 || len == 0xffffffff || (uint64_t)(c->end - c->p) < len) {
		return false;
	}
	c->end = c->p + len;
	return true;
}

static bool
parse_cie(const cw_eh_module_t* m, const uint8_t* p, cie_t* cie) {
	cursor_t c;
	if (!open_record(m, p, &c) || read_u(&c, 4) != 0) {
		return false;
	}
	uint64_t version = read_u(&c, 1);
	if (version != 1 && version != 3 && version != 4) {
		return false;
	}
	const char* aug = (const char*)c.p;
	while (read_u(&c, 1) != 0 && !c.err) {
	}
	if (c.err) {
		return false;
	}
	if (version == 4) {
		if (read_u(&c, 1) != 8 || read_u(&c, 1) != 0) {
			return false; /* address_size, segment_size */
		}
	}
	*cie = (cie_t){
		.code_align = read_uleb(&c),
		.data_align = read_sleb(&c),
		.ra_reg = version == 1 ? read_u(&c, 1) : read_uleb(&c),
		.fde_enc = PE_ABSPTR,
		.lsda_enc = PE_OMIT,
	};
	if (aug[0] == 'z') {
		cie->has_z = true;
		uint64_t aug_len = read_uleb(&c);
		const uint8_t* aug_end = c.p + aug_len;
		if (c.err || aug_len > (uint64_t)(c.end - c.p)) {
			return false;
		}
		for (const char* a = aug + 1; *a != '\0' && !c.err; ++a) {
			switch (*a) {
			case 'L':
				cie->lsda_enc = (uint8_t)read_u(&c, 1);
				break;
			case 'P': {
				uint8_t enc = (uint8_t)read_u(&c, 1);
				size_t n = encoded_size(enc);
				if (n != 0) {
					skip(&c, n);
				} else if ((enc & 0x0f) == PE_ULEB128) {
					read_uleb(&c);
				} else {
					read_sleb(&c);
				}
				break;
			}
			case 'R':
				cie->fde_enc = (uint8_t)read_u(&c, 1);
				break;
			case 'S':
				cie->signal_frame = true;
				break;
			default:
				break; /* Unknown letters are skipped with the rest of the data. */
			}
		}
		c.p = aug_end;
	} else if (aug[0] != '\0') {
		return false;
	}
	if (c.err) {
		return false;
	}
	cie->instr = c.p;
	cie->instr_end = c.end;
	return true;
}

/** Parse the FDE at `p` and the CIE it points to. */
static bool
parse_fde(const cw_eh_module_t* m, const uint8_t* p, cie_t* cie, fde_t* fde) {
	cursor_t c;
	if (!open_record(m, p, &c)) {
		return false;
	}
	const uint8_t* id_field = c.p;
	uint64_t cie_ptr = read_u(&c, 4);
	if (c.err || cie_ptr == 0 || cie_ptr > (uint64_t)(id_field - m->file)) {
		return false;
	}
	if (!parse_cie(m, id_field - cie_ptr, cie)) {
		return false;
	}
	*fde = (fde_t){
		.pc_begin = read_encoded(&c, cie->fde_enc, m),
		.pc_range = read_encoded(&c, cie->fde_enc & 0x0f, m),
	};
	if (cie->has_z) {
		skip(&c, read_uleb(&c));
	}
	if (c.err) {
		return false;
	}
	fde->instr = c.p;
	fde->instr_end = c.end;
	return true;
}

/**
 * Find the FDE covering the link-time address `pc` through the sorted
 * header table, or by scanning the section when there is none.
 */
static bool
find_fde(const cw_eh_module_t* m, uint64_t pc, cie_t* cie, fde_t* fde) {
	if (m->table != NULL) {
		size_t width = encoded_size(m->table_enc);
		uint64_t lo = 0;
		uint64_t hi = m->table_count;
		uint64_t fde_addr = 0;
		while (lo < hi) {
			uint64_t mid = lo + (hi - lo) / 2;
			cursor_t c = { .p = m->table + mid * 2 * width, .end = m->file + m->file_len };
			uint64_t loc = read_encoded(&c, m->table_enc, m);
			uint64_t addr = read_encoded(&c, m->table_enc, m);
			if (c.err) {
				return false;
			}
			if (loc <= pc) {
				fde_addr = addr;
				lo = mid + 1;
			} else {
				hi = mid;
			}
		}
		if (fde_addr == 0 || !parse_fde(m, vaddr_ptr(m, fde_addr), cie, fde)) {
			return false;
		}
		return pc >= fde->pc_begin && pc - fde->pc_begin < fde->pc_range;
	}

	const uint8_t* p = m->eh_frame;
	while (p != NULL && p + 8 <= m->eh_frame_end) {
		cursor_t c;
		if (!open_record(m, p, &c)) {
			return false;
		}
		const uint8_t* next = c.end;
		if (read_u(&c, 4) != 0 && parse_fde(m, p, cie, fde)
			&& pc >= fde->pc_begin && pc - fde->pc_begin < fde->pc_range) {
			return true;
		}
		p = next;
	}
	return false;
}

/* }}} */

/* Expressions {{{ */

/**
 * Evaluate a DWARF expression. `initial` is pushed first when not
 * NULL; that is the CFA for register rules.
 */
static bool
eval_expr(
	const cw_eh_module_t* m, const uint8_t* expr, size_t len,
	const cw_regs_t* regs, const cw_stack_t* stack,
	const uint64_t* initial, uint64_t* result
) {
	uint64_t st[EXPR_STACK_CAP];
	int sp = 0;
	if (initial != NULL) {
		st[sp++] = *initial;
	}
	cursor_t c = { .p = expr, .end = expr + len };
	while (c.p < c.end && !c.err) {
		uint8_t op = (uint8_t)read_u(&c, 1);
		uint64_t v;
		if (op >= OP_LIT0 && op <= OP_LIT31) {
			v = op - OP_LIT0;
		} else if (op >= OP_REG0 && op <= OP_REG31) {
			unsigned r = op - OP_REG0;
			if ((regs->valid & (1u << r)) == 0) {
				return false;
			}
			v = regs->regs[r];
		} else if (op >= OP_BREG0 && op <= OP_BREG31) {
			unsigned r = op - OP_BREG0;
			if ((regs->valid & (1u << r)) == 0) {
				return false;
			}
			v = regs->regs[r] + (uint64_t)read_sleb(&c);
		} else {
			switch (op) {
			case OP_ADDR:        v = read_u(&c, 8) + m->bias; break;
			case OP_CONST1U:     v = read_u(&c, 1); break;
			case OP_CONST1S:     v = (uint64_t)read_s(&c, 1); break;
			case OP_CONST2U:     v = read_u(&c, 2); break;
			case OP_CONST2S:     v = (uint64_t)read_s(&c, 2); break;
			case OP_CONST4U:     v = read_u(&c, 4); break;
			case OP_CONST4S:     v = (uint64_t)read_s(&c, 4); break;
			case OP_CONST8U:     v = read_u(&c, 8); break;
			case OP_CONST8S:     v = (uint64_t)read_s(&c, 8); break;
			case OP_CONSTU:      v = read_uleb(&c); break;
			case OP_CONSTS:      v = (uint64_t)read_sleb(&c); break;
			case OP_REGX:
			case OP_BREGX: {
				uint64_t r = read_uleb(&c);
				if (r >= CW_REG_COUNT || (regs->valid & (1u << r)) == 0) {
					return false;
				}
				v = regs->regs[r] + (op == OP_BREGX ? (uint64_t)read_sleb(&c) : 0);
				break;
			}
			case OP_CALL_FRAME_CFA:
				if (initial == NULL) {
					return false;
				}
				v = *initial;
				break;
			case OP_NOP:
				continue;
			case OP_DUP:
				if (sp < 1) {
					return false;
				}
				v = st[sp - 1];
				break;
			case OP_DROP:
				if (sp < 1) {
					return false;
				}
				--sp;
				continue;
			case OP_OVER:
				if (sp < 2) {
					return false;
				}
				v = st[sp - 2];
				break;
			case OP_PICK: {
				uint64_t idx = read_u(&c, 1);
				if (idx >= (uint64_t)sp) {
					return false;
				}
				v = st[sp - 1 - idx];
				break;
			}
			case OP_SWAP:
				if (sp < 2) {
					return false;
				}
				v = st[sp - 1];
				st[sp - 1] = st[sp - 2];
				st[sp - 2] = v;
				continue;
			case OP_ROT:
				if (sp < 3) {
					return false;
				}
				v = st[sp - 1];
				st[sp - 1] = st[sp - 2];
				st[sp - 2] = st[sp - 3];
				st[sp - 3] = v;
				continue;
			case OP_DEREF:
			case OP_DEREF_SIZE: {
				if (sp < 1) {
					return false;
				}
				size_t n = op == OP_DEREF ? 8 : (size_t)read_u(&c, 1);
				if (n == 0 || n > 8 || !stack_read(stack, st[sp - 1], n, &v)) {
					return false;
				}
				--sp;
				break;
			}
			case OP_ABS:
			case OP_NEG:
			case OP_NOT:
			case OP_PLUS_UCONST:
				if (sp < 1) {
					return false;
				}
				v = st[--sp];
				if (op == OP_ABS) {
					v = (int64_t)v < 0 ? (uint64_t)-(int64_t)v : v;
				} else if (op == OP_NEG) {
					v = (uint64_t)-(int64_t)v;
				} else if (op == OP_NOT) {
					v = ~v;
				} else {
					v += read_uleb(&c);
				}
				break;
			case OP_BRA:
			case OP_SKIP: {
				int64_t off = read_s(&c, 2);
				if (op == OP_BRA) {
					if (sp < 1) {
						return false;
					}
					if (st[--sp] == 0) {
						continue;
					}
				}
				const uint8_t* target = c.p + off;
				if (c.err || target < expr || target > c.end) {
					return false;
				}
				c.p = target;
				continue;
			}
			default: {
				if (op < OP_AND || op > OP_NE || op == OP_BRA || sp < 2) {
					return false;
				}
				uint64_t b = st[--sp];
				uint64_t a = st[--sp];
				switch (op) {
				case OP_AND:   v = a & b; break;
				case OP_DIV:   v = b == 0 ? 0 : (uint64_t)((int64_t)a / (int64_t)b); break;
				case OP_MINUS: v = a - b; break;
				case OP_MOD:   v = b == 0 ? 0 : a % b; break;
				case OP_MUL:   v = a * b; break;
				case OP_OR:    v = a | b; break;
				case OP_PLUS:  v = a + b; break;
				case OP_SHL:   v = b < 64 ? a << b : 0; break;
				case OP_SHR:   v = b < 64 ? a >> b : 0; break;
				case OP_SHRA:  v = b < 64 ? (uint64_t)((int64_t)a >> b) : 0; break;
				case OP_XOR:   v = a ^ b; break;
				case OP_EQ:    v = a == b; break;
				case OP_GE:    v = (int64_t)a >= (int64_t)b; break;
				case OP_GT:    v = (int64_t)a > (int64_t)b; break;
				case OP_LE:    v = (int64_t)a <= (int64_t)b; break;
				case OP_LT:    v = (int64_t)a < (int64_t)b; break;
				case OP_NE:    v = a != b; break;
				default:       return false;
				}
				break;
			}
			}
		}
		if (c.err || sp >= EXPR_STACK_CAP) {
			return false;
		}
		st[sp++] = v;
	}
	if (c.err || sp < 1) {
		return false;
	}
	*result = st[sp - 1];
	return true;
}

/* }}} */

/* Rules {{{ */

typedef enum {
	RULE_SAME,                 /**< Unchanged by this frame; the default. */
	RULE_UNDEFINED,
	RULE_OFFSET,               /**< Saved at CFA + `value`. */
	RULE_VAL_OFFSET,           /**< Equals CFA + `value`. */
	RULE_REGISTER,             /**< Held in register `value`. */
	RULE_EXPR,                 /**< Saved at the address the expression yields. */
	RULE_VAL_EXPR,             /**< Equals what the expression yields. */
} rule_kind_t;

typedef struct {
	rule_kind_t kind;
	int64_t value;
	const uint8_t* expr;
	size_t expr_len;
} rule_t;

typedef struct {
	rule_t regs[CW_REG_COUNT];
	uint64_t cfa_reg;
	int64_t cfa_offset;
	const uint8_t* cfa_expr;   /**< When not NULL, the CFA is this expression. */
	size_t cfa_expr_len;
	bool ra_signed;            /**< arm64 pointer authentication is active on the return address. */
} rules_t;

static void
set_rule(rules_t* rules, uint64_t reg, rule_t rule) {
	if (reg < CW_REG_COUNT) {
		rules->regs[reg] = rule;
	}
}

/**
 * Run call frame instructions until they pass `target`, updating
 * `rules`. `initial` supplies the rules DW_CFA_restore returns to and
 * is NULL for the CIE's own instructions.
 */
static bool
run_cfi(
	const uint8_t* instr, const uint8_t* instr_end, const cie_t* cie, const cw_eh_module_t* m,
	uint64_t loc, uint64_t target, rules_t* rules, const rules_t* initial
) {
	cursor_t c = { .p = instr, .end = instr_end };
	rules_t saved[STATE_STACK_CAP];
	int depth = 0;
	while (c.p < c.end && !c.err) {
		uint8_t op = (uint8_t)read_u(&c, 1);
		uint8_t high = op & 0xc0;
		uint64_t low = op & 0x3f;
		if (high == CFA_ADVANCE_LOC) {
			loc += low * cie->code_align;
			if (loc > target) {
				break;
			}
			continue;
		}
		if (high == CFA_OFFSET) {
			set_rule(rules, low, (rule_t){ .kind = RULE_OFFSET, .value = (int64_t)read_uleb(&c) * cie->data_align });
			continue;
		}
		if (high == CFA_RESTORE) {
			set_rule(rules, low, initial != NULL && low < CW_REG_COUNT ? initial->regs[low] : (rule_t){ .kind = RULE_SAME });
			continue;
		}
		switch (op) {
		case CFA_NOP:
			break;
		case CFA_SET_LOC:
			loc = read_encoded(&c, cie->fde_enc, m);
			break;
		case CFA_ADVANCE_LOC1:
			loc += read_u(&c, 1) * cie->code_align;
			break;
		case CFA_ADVANCE_LOC2:
			loc += read_u(&c, 2) * cie->code_align;
			break;
		case CFA_ADVANCE_LOC4:
			loc += read_u(&c, 4) * cie->code_align;
			break;
		case CFA_OFFSET_EXTENDED: {
			uint64_t reg = read_uleb(&c);
			set_rule(rules, reg, (rule_t){ .kind = RULE_OFFSET, .value = (int64_t)read_uleb(&c) * cie->data_align });
			break;
		}
		case CFA_OFFSET_EXTENDED_SF: {
			uint64_t reg = read_uleb(&c);
			set_rule(rules, reg, (rule_t){ .kind = RULE_OFFSET, .value = read_sleb(&c) * cie->data_align });
			break;
		}
		case CFA_GNU_NEGATIVE_OFFSET_EXT: {
			uint64_t reg = read_uleb(&c);
			set_rule(rules, reg, (rule_t){ .kind = RULE_OFFSET, .value = -(int64_t)read_uleb(&c) * cie->data_align });
			break;
		}
		case CFA_RESTORE_EXTENDED: {
			uint64_t reg = read_uleb(&c);
			set_rule(rules, reg, initial != NULL && reg < CW_REG_COUNT ? initial->regs[reg] : (rule_t){ .kind = RULE_SAME });
			break;
		}
		case CFA_UNDEFINED:
			set_rule(rules, read_uleb(&c), (rule_t){ .kind = RULE_UNDEFINED });
			break;
		case CFA_SAME_VALUE:
			set_rule(rules, read_uleb(&c), (rule_t){ .kind = RULE_SAME });
			break;
		case CFA_REGISTER: {
			uint64_t reg = read_uleb(&c);
			set_rule(rules, reg, (rule_t){ .kind = RULE_REGISTER, .value = (int64_t)read_uleb(&c) });
			break;
		}
		case CFA_REMEMBER_STATE:
			if (depth >= STATE_STACK_CAP) {
				return false;
			}
			saved[depth++] = *rules;
			break;
		case CFA_RESTORE_STATE:
			if (depth == 0) {
				return false;
			}
			*rules = saved[--depth];
			break;
		case CFA_DEF_CFA:
			rules->cfa_reg = read_uleb(&c);
			rules->cfa_offset = (int64_t)read_uleb(&c);
			rules->cfa_expr = NULL;
			break;
		case CFA_DEF_CFA_SF:
			rules->cfa_reg = read_uleb(&c);
			rules->cfa_offset = read_sleb(&c) * cie->data_align;
			rules->cfa_expr = NULL;
			break;
		case CFA_DEF_CFA_REGISTER:
			rules->cfa_reg = read_uleb(&c);
			rules->cfa_expr = NULL;
			break;
		case CFA_DEF_CFA_OFFSET:
			rules->cfa_offset = (int64_t)read_uleb(&c);
			break;
		case CFA_DEF_CFA_OFFSET_SF:
			rules->cfa_offset = read_sleb(&c) * cie->data_align;
			break;
		case CFA_DEF_CFA_EXPRESSION: {
			uint64_t len = read_uleb(&c);
			rules->cfa_expr = c.p;
			rules->cfa_expr_len = (size_t)len;
			skip(&c, len);
			break;
		}
		case CFA_EXPRESSION:
		case CFA_VAL_EXPRESSION: {
			uint64_t reg = read_uleb(&c);
			uint64_t len = read_uleb(&c);
			set_rule(rules, reg, (rule_t){
				.kind = op == CFA_EXPRESSION ? RULE_EXPR : RULE_VAL_EXPR,
				.expr = c.p,
				.expr_len = (size_t)len,
			});
			skip(&c, len);
			break;
		}
		case CFA_VAL_OFFSET: {
			uint64_t reg = read_uleb(&c);
			set_rule(rules, reg, (rule_t){ .kind = RULE_VAL_OFFSET, .value = (int64_t)read_uleb(&c) * cie->data_align });
			break;
		}
		case CFA_VAL_OFFSET_SF: {
			uint64_t reg = read_uleb(&c);
			set_rule(rules, reg, (rule_t){ .kind = RULE_VAL_OFFSET, .value = read_sleb(&c) * cie->data_align });
			break;
		}
		case CFA_AARCH64_NEGATE_RA_STATE:
			rules->ra_signed = !rules->ra_signed;
			break;
		case CFA_GNU_ARGS_SIZE:
			read_uleb(&c);
			break;
		default:
			return false;
		}
		if (loc > target) {
			break;
		}
	}
	return !c.err;
}

/* }}} */

bool
cw_eh_open(cw_eh_module_t* m, const char* path, uint64_t map_start, uint64_t map_offset, uint64_t pc) {
	*m = (cw_eh_module_t){ .state = -1 };
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	struct stat st;
	if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
		close(fd);
		return false;
	}
	void* map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED) {
		return false;
	}
	m->file = map;
	m->file_len = (size_t)st.st_size;

	Elf64_Ehdr eh;
	memcpy(&eh, m->file, sizeof(eh));
	if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64
		|| eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_phentsize != sizeof(Elf64_Phdr)
		|| eh.e_phoff > m->file_len || (uint64_t)eh.e_phnum * sizeof(Elf64_Phdr) > m->file_len - eh.e_phoff) {
		return false;
	}
	uint64_t hdr_off = 0;
	uint64_t hdr_len = 0;
	for (int i = 0; i < eh.e_phnum; ++i) {
		Elf64_Phdr ph;
		memcpy(&ph, m->file + eh.e_phoff + (uint64_t)i * sizeof(ph), sizeof(ph));
		if (ph.p_type == PT_LOAD && m->load_count < CW_EH_MAX_LOAD) {
			m->load[m->load_count++] = (cw_eh_load_t){
				.vaddr = ph.p_vaddr,
				.offset = ph.p_offset,
				.filesz = ph.p_filesz,
			};
		} else if (ph.p_type == PT_GNU_EH_FRAME) {
			hdr_off = ph.p_offset;
			hdr_len = ph.p_filesz;
			m->hdr_vaddr = ph.p_vaddr;
		}
	}

	/* The load bias comes from the segment that holds the mapping's file offset of pc. */
	uint64_t pc_off = pc - map_start + map_offset;
	bool found = false;
	for (int i = 0; i < m->load_count && !found; ++i) {
		const cw_eh_load_t* l = &m->load[i];
		if (pc_off >= l->offset && pc_off - l->offset < l->filesz) {
			m->bias = pc - (l->vaddr + (pc_off - l->offset));
			found = true;
		}
	}
	if (!found) {
		return false;
	}

	if (hdr_len >= 4 && hdr_off < m->file_len && hdr_len <= m->file_len - hdr_off) {
		cursor_t c = { .p = m->file + hdr_off, .end = m->file + hdr_off + hdr_len };
		uint64_t version = read_u(&c, 1);
		uint8_t eh_frame_enc = (uint8_t)read_u(&c, 1);
		uint8_t count_enc = (uint8_t)read_u(&c, 1);
		uint8_t table_enc = (uint8_t)read_u(&c, 1);
		if (version == 1 && eh_frame_enc != PE_OMIT) {
			uint64_t eh_frame_vaddr = read_encoded(&c, eh_frame_enc, m);
			m->eh_frame = c.err ? NULL : vaddr_ptr(m, eh_frame_vaddr);
			if (count_enc != PE_OMIT && table_enc != PE_OMIT && encoded_size(table_enc) != 0) {
				uint64_t count = read_encoded(&c, count_enc, m);
				if (!c.err && count > 0 && count <= (uint64_t)(c.end - c.p) / (2 * encoded_size(table_enc))) {
					m->table = c.p;
					m->table_count = count;
					m->table_enc = table_enc;
				}
			}
		}
	}

	/* Without a search table the section is scanned; its bounds come from
	 * the section header, or up to the end of the segment when stripped. */
	if (m->table == NULL) {
		if (eh.e_shentsize == sizeof(Elf64_Shdr) && eh.e_shstrndx < eh.e_shnum
			&& eh.e_shoff <= m->file_len && (uint64_t)eh.e_shnum * sizeof(Elf64_Shdr) <= m->file_len - eh.e_shoff) {
			Elf64_Shdr strtab;
			memcpy(&strtab, m->file + eh.e_shoff + (uint64_t)eh.e_shstrndx * sizeof(strtab), sizeof(strtab));
			for (int i = 0; i < eh.e_shnum && m->eh_frame_end == NULL; ++i) {
				Elf64_Shdr sh;
				memcpy(&sh, m->file + eh.e_shoff + (uint64_t)i * sizeof(sh), sizeof(sh));
				uint64_t name_off = strtab.sh_offset + sh.sh_name;
				if (sh.sh_type == SHT_PROGBITS && strtab.sh_offset < m->file_len && sh.sh_name < m->file_len - strtab.sh_offset
					&& strncmp((const char*)m->file + name_off, ".eh_frame", m->file_len - name_off) == 0
					&& sh.sh_offset < m->file_len && sh.sh_size <= m->file_len - sh.sh_offset) {
					m->eh_frame = m->file + sh.sh_offset;
					m->eh_frame_end = m->eh_frame + sh.sh_size;
				}
			}
		}
		if (m->eh_frame == NULL) {
			return false;
		}
		if (m->eh_frame_end == NULL) {
			uint64_t off = (uint64_t)(m->eh_frame - m->file);
			for (int i = 0; i < m->load_count; ++i) {
				const cw_eh_load_t* l = &m->load[i];
				if (off >= l->offset && off - l->offset < l->filesz) {
					m->eh_frame_end = m->file + l->offset + l->filesz;
				}
			}
		}
		if (m->eh_frame_end == NULL) {
			return false;
		}
	}
	m->state = 1;
	return true;
}

void
cw_eh_close(cw_eh_module_t* m) {
	if (m->file != NULL) {
		munmap((void*)m->file, m->file_len);
	}
	*m = (cw_eh_module_t){ 0 };
}

bool
cw_eh_step(
	const cw_eh_module_t* m, const cw_stack_t* stack,
	uint64_t pc, cw_regs_t* regs, uint64_t* ret, bool* signal_frame
) {
	if (m->state != 1) {
		return false;
	}
	cie_t cie;
	fde_t fde;
	uint64_t link_pc = pc - m->bias;
	if (!find_fde(m, link_pc, &cie, &fde) || cie.ra_reg >= CW_REG_COUNT) {
		return false;
	}

	rules_t initial = { .cfa_reg = CW_REG_COUNT };
	if (!run_cfi(cie.instr, cie.instr_end, &cie, m, 0, UINT64_MAX, &initial, NULL)) {
		return false;
	}
	rules_t rules = initial;
	if (!run_cfi(fde.instr, fde.instr_end, &cie, m, fde.pc_begin, link_pc, &rules, &initial)) {
		return false;
	}

	uint64_t cfa;
	if (rules.cfa_expr != NULL) {
		if (!eval_expr(m, rules.cfa_expr, rules.cfa_expr_len, regs, stack, NULL, &cfa)) {
			return false;
		}
	} else {
		if (rules.cfa_reg >= CW_REG_COUNT || (regs->valid & (1u << rules.cfa_reg)) == 0) {
			return false;
		}
		cfa = regs->regs[rules.cfa_reg] + (uint64_t)rules.cfa_offset;
	}

	cw_regs_t out = *regs;
	for (unsigned r = 0; r < CW_REG_COUNT; ++r) {
		const rule_t* rule = &rules.regs[r];
		uint64_t v = 0;
		bool known = true;
		switch (rule->kind) {
		case RULE_SAME:
			continue;
		case RULE_UNDEFINED:
			known = false;
			break;
		case RULE_OFFSET:
			known = stack_read(stack, cfa + (uint64_t)rule->value, 8, &v);
			break;
		case RULE_VAL_OFFSET:
			v = cfa + (uint64_t)rule->value;
			break;
		case RULE_REGISTER:
			known = rule->value >= 0 && rule->value < CW_REG_COUNT
				&& (regs->valid & (1u << rule->value)) != 0;
			v = known ? regs->regs[rule->value] : 0;
			break;
		case RULE_EXPR:
			known = eval_expr(m, rule->expr, rule->expr_len, regs, stack, &cfa, &v)
				&& stack_read(stack, v, 8, &v);
			break;
		case RULE_VAL_EXPR:
			known = eval_expr(m, rule->expr, rule->expr_len, regs, stack, &cfa, &v);
			break;
		}
		if (known) {
			out.regs[r] = v;
			out.valid |= 1u << r;
		} else {
			out.valid &= ~(1u << r);
		}
	}
	if (rules.regs[CW_REG_SP].kind == RULE_SAME) {
		out.regs[CW_REG_SP] = cfa;
		out.valid |= 1u << CW_REG_SP;
	}

	if (rules.regs[cie.ra_reg].kind == RULE_UNDEFINED) {
		*ret = 0; /* Outermost frame. */
	} else {
		if ((out.valid & (1u << cie.ra_reg)) == 0) {
			return false;
		}
		*ret = out.regs[cie.ra_reg];
		if (rules.ra_signed) {
			*ret &= 0x0000ffffffffffffull; /* Strip the authentication code. */
		}
	}
	*signal_frame = cie.signal_frame;
	*regs = out;
	return true;
}
