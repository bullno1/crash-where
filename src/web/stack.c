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
		/* The function index; the offset alone names the place. */
		while (*p >= '0' && *p <= '9') {
			++p;
		}
		if (p[0] != ']' || p[1] != ':' || p[2] != '0' || p[3] != 'x') {
			continue;
		}
		p += 4;
		char* end;
		unsigned long long offset = strtoull(p, &end, 16);
		if (end == p) {
			continue;
		}
		frames[count++] = (cw_frame_t){ .module = 0, .offset = offset };
		p = end;
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
