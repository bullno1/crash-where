/**
 * @file web/stack.c
 * Reads what an engine prints about a trap: the stack text and the message.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "web/platform.h"

int
cw_web_parse_stack(const char* stack, cw_frame_t* frames, int cap) {
	static const char token[] = "wasm-function[";
	int count = 0;
	const char* p = stack;
	while (count < cap && (p = strstr(p, token)) != NULL) {
		p += sizeof(token) - 1;
		char* end;
		unsigned long index = strtoul(p, &end, 10);
		if (end == p || *end != ']') {
			continue;
		}
		p = end + 1;
		unsigned long long offset;
		if (p[0] == ':' && p[1] == '0' && p[2] == 'x') {
			offset = strtoull(p + 3, &end, 16);
			if (end == p + 3) {
				continue;
			}
			p = end;
		} else {
			offset = cw_web_function_offset((uint32_t)index);
			if (offset == 0) {
				continue;
			}
		}
		frames[count++] = (cw_frame_t){ .module = 0, .offset = offset };
	}
	return count;
}

void
cw_web_trap_type(const char* message, char* out, size_t cap) {
	/* First match wins, so the narrower wording comes first. */
	static const struct {
		const char* text;
		const char* type;
	} known[] = {
		{ "Aborted", "ABORT" },
		{ "signature mismatch", "BAD_CALL" },
		{ "unreachable", "UNREACHABLE" },
		{ "divide by zero", "DIVIDE_BY_ZERO" },
		{ "out of bounds", "OUT_OF_BOUNDS" },
	};
	const char* type = "TRAP";
	for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
		if (strstr(message, known[i].text) != NULL) {
			type = known[i].type;
			break;
		}
	}
	snprintf(out, cap, "%s", type);
}
