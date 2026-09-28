/*
 * Call frame information that compilers never emit, for the
 * differential unwinder test. Each function is one `ret` under an FDE
 * written by hand; the code never runs, the rows are only looked up.
 *
 * Opcode bytes follow DWARF 5 tables 7.9 (DW_OP) and 7.29 (DW_CFA).
 * The assembler's CIE has code alignment 1, data alignment -8 and the
 * return address at CFA - 8, so factored offsets are in units of -8.
 */

#define FIXTURE(NAME) \
	.globl NAME; \
	.type NAME, %function; \
	NAME:; \
	.cfi_startproc

#define END(NAME) \
	.cfi_endproc; \
	.size NAME, . - NAME

	.text
	.globl cfi_fixtures_begin
cfi_fixtures_begin:

#if defined(__x86_64__)

/* DW_CFA_offset_extended, _sf, GNU_negative_offset_extended, val_offset, _sf. */
FIXTURE(cfi_fixture_offsets)
	.cfi_escape 0x05, 3, 2             /* rbx at CFA - 16 */
	.cfi_escape 0x11, 12, 0x7d         /* r12 at CFA + 24 */
	.cfi_escape 0x2f, 13, 2            /* r13 at CFA + 16 */
	.cfi_escape 0x14, 14, 4            /* r14 = CFA - 32 */
	.cfi_escape 0x15, 15, 0x7c         /* r15 = CFA + 32, signed form */
	ret
END(cfi_fixture_offsets)

/* DW_CFA_same_value and DW_CFA_restore_extended undo earlier rules. */
FIXTURE(cfi_fixture_restore)
	.cfi_offset %rbx, -16
	.cfi_offset %r12, -24
	.cfi_same_value %rbx
	.cfi_escape 0x06, 12               /* restore_extended r12 */
	ret
END(cfi_fixture_restore)

/* DW_CFA_def_cfa_sf and DW_CFA_def_cfa_offset_sf. */
FIXTURE(cfi_fixture_cfa_sf)
	.cfi_escape 0x12, 7, 0x7e          /* CFA = rsp + 16 */
	.cfi_escape 0x13, 0x7d             /* CFA = rsp + 24 */
	ret
END(cfi_fixture_cfa_sf)

/* Every constant form and arithmetic operation; the value ends as 1. */
FIXTURE(cfi_fixture_expr_arith)
	.cfi_escape 0x16, 3, 60,           /* rbx = val_expression of 60 bytes */ \
		0x08, 5,                       /* const1u 5 */ \
		0x09, 0xfd, 0x22,              /* const1s -3; plus -> 2 */ \
		0x0a, 0x00, 0x01, 0x1e,        /* const2u 256; mul -> 512 */ \
		0x0b, 0xff, 0xff, 0x1c,        /* const2s -1; minus -> 513 */ \
		0x0c, 0x10, 0, 0, 0, 0x21,     /* const4u 16; or -> 529 */ \
		0x0d, 0xfe, 0xff, 0xff, 0xff, 0x1a, /* const4s -2; and -> 528 */ \
		0x0e, 3, 0, 0, 0, 0, 0, 0, 0, 0x1d, /* const8u 3; mod -> 0 */ \
		0x0f, 0xf9, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x1c, /* const8s -7; minus -> 7 */ \
		0x10, 9, 0x27,                 /* constu 9; xor -> 14 */ \
		0x11, 0x7c, 0x1b,              /* consts -4; div -> -3 */ \
		0x19,                          /* abs -> 3 */ \
		0x31, 0x24,                    /* lit1; shl -> 6 */ \
		0x31, 0x25,                    /* lit1; shr -> 3 */ \
		0x1f,                          /* neg -> -3 */ \
		0x31, 0x26,                    /* lit1; shra -> -2 */ \
		0x20                           /* not -> 1 */
	ret
END(cfi_fixture_expr_arith)

/* Stack manipulation; the value ends as 9. */
FIXTURE(cfi_fixture_expr_stack)
	.cfi_escape 0x16, 12, 15,          /* r12 = val_expression of 15 bytes */ \
		0x31, 0x32, 0x33,              /* lit1 lit2 lit3 */ \
		0x17,                          /* rot -> 1 3 2 */ \
		0x16,                          /* swap -> 1 2 3 */ \
		0x14,                          /* over -> 1 2 3 2 */ \
		0x15, 3,                       /* pick 3 -> 1 2 3 2 1 */ \
		0x12, 0x13,                    /* dup; drop */ \
		0x96,                          /* nop */ \
		0x22, 0x22, 0x22, 0x22         /* plus x4 -> 9 */
	ret
END(cfi_fixture_expr_stack)

/* Comparisons and branches; the value ends as 17. */
FIXTURE(cfi_fixture_expr_branch)
	.cfi_escape 0x16, 13, 26,          /* r13 = val_expression of 26 bytes */ \
		0x35, 0x33, 0x2b,              /* lit5 lit3 gt -> 1 */ \
		0x32, 0x32, 0x2c,              /* lit2 lit2 le -> 1 1 */ \
		0x22,                          /* plus -> 2 */ \
		0x31, 0x2d,                    /* lit1 lt -> 0 */ \
		0x30, 0x29,                    /* lit0 eq -> 1 */ \
		0x31, 0x2e,                    /* lit1 ne -> 0 */ \
		0x31, 0x28, 0x01, 0x00,        /* lit1; bra +1 over lit7 */ \
		0x37, 0x39,                    /* lit7 (skipped) lit9 -> 0 9 */ \
		0x2f, 0x01, 0x00,              /* skip +1 over lit7 */ \
		0x37, 0x38,                    /* lit7 (skipped) lit8 -> 0 9 8 */ \
		0x22, 0x22                     /* plus plus -> 17 */
	ret
END(cfi_fixture_expr_branch)

/* Register operands, sized dereference, and an absolute address that is dropped. */
FIXTURE(cfi_fixture_expr_regs)
	.cfi_escape 0x10, 14, 3,           /* r14 saved at the address: */ \
		0x92, 7, 16                    /* bregx rsp + 16 */
	.cfi_escape 0x16, 15, 18,          /* r15 = val_expression of 18 bytes */ \
		0x92, 7, 24,                   /* bregx rsp + 24 */ \
		0x94, 4,                       /* deref_size 4 */ \
		0x90, 3,                       /* regx rbx */ \
		0x22,                          /* plus */ \
		0x03, 0x00, 0x10, 0, 0, 0, 0, 0, 0, /* addr 0x1000 */ \
		0x13                           /* drop */
	ret
END(cfi_fixture_expr_regs)

/* A signal frame CIE. */
FIXTURE(cfi_fixture_signal)
	.cfi_signal_frame
	.cfi_def_cfa_offset 16
	ret
END(cfi_fixture_signal)

#elif defined(__aarch64__)

/* DW_CFA_offset_extended, _sf, val_offset, _sf, same_value, restore_extended. */
FIXTURE(cfi_fixture_offsets)
	.cfi_escape 0x05, 19, 2            /* x19 at CFA - 16 */
	.cfi_escape 0x11, 20, 0x7d         /* x20 at CFA + 24 */
	.cfi_escape 0x14, 21, 4            /* x21 = CFA - 32 */
	.cfi_escape 0x15, 22, 0x7c         /* x22 = CFA + 32, signed form */
	.cfi_offset x23, -40
	.cfi_same_value x23
	.cfi_offset x24, -48
	.cfi_escape 0x06, 24               /* restore_extended x24 */
	ret
END(cfi_fixture_offsets)

/* DW_CFA_def_cfa_sf and DW_CFA_def_cfa_offset_sf. */
FIXTURE(cfi_fixture_cfa_sf)
	.cfi_escape 0x12, 31, 0x7e         /* CFA = sp + 16 */
	.cfi_escape 0x13, 0x7d             /* CFA = sp + 24 */
	.cfi_offset x30, -8
	ret
END(cfi_fixture_cfa_sf)

/* Every constant form and arithmetic operation; the value ends as 1. */
FIXTURE(cfi_fixture_expr_arith)
	.cfi_escape 0x16, 19, 60, \
		0x08, 5, \
		0x09, 0xfd, 0x22, \
		0x0a, 0x00, 0x01, 0x1e, \
		0x0b, 0xff, 0xff, 0x1c, \
		0x0c, 0x10, 0, 0, 0, 0x21, \
		0x0d, 0xfe, 0xff, 0xff, 0xff, 0x1a, \
		0x0e, 3, 0, 0, 0, 0, 0, 0, 0, 0x1d, \
		0x0f, 0xf9, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x1c, \
		0x10, 9, 0x27, \
		0x11, 0x7c, 0x1b, \
		0x19, \
		0x31, 0x24, \
		0x31, 0x25, \
		0x1f, \
		0x31, 0x26, \
		0x20
	ret
END(cfi_fixture_expr_arith)

/* Stack manipulation, comparisons and branches; the value ends as 26. */
FIXTURE(cfi_fixture_expr_stack)
	.cfi_escape 0x16, 20, 42, \
		0x31, 0x32, 0x33, 0x17, 0x16, 0x14, 0x15, 3, 0x12, 0x13, 0x96, \
		0x22, 0x22, 0x22, 0x22, \
		0x35, 0x33, 0x2b, 0x32, 0x32, 0x2c, 0x22, 0x31, 0x2d, 0x30, 0x29, 0x31, 0x2e, \
		0x31, 0x28, 0x01, 0x00, 0x37, 0x39, 0x2f, 0x01, 0x00, 0x37, 0x38, \
		0x22, 0x22, 0x22
	ret
END(cfi_fixture_expr_stack)

/* Register operands, sized dereference, a dropped absolute address. */
FIXTURE(cfi_fixture_expr_regs)
	.cfi_escape 0x10, 21, 3, 0x92, 31, 16
	.cfi_escape 0x16, 22, 18, \
		0x92, 31, 24, 0x94, 4, 0x90, 19, 0x22, \
		0x03, 0x00, 0x10, 0, 0, 0, 0, 0, 0, 0x13
	ret
END(cfi_fixture_expr_regs)

/* Pointer authentication state toggled on, then a signal frame. */
FIXTURE(cfi_fixture_ra_state)
	.cfi_negate_ra_state
	.cfi_def_cfa_offset 16
	.cfi_offset x30, -8
	ret
END(cfi_fixture_ra_state)

FIXTURE(cfi_fixture_signal)
	.cfi_signal_frame
	.cfi_def_cfa_offset 16
	.cfi_offset x30, -8
	ret
END(cfi_fixture_signal)

#endif

	.globl cfi_fixtures_end
cfi_fixtures_end:

	.section .note.GNU-stack, "", %progbits
