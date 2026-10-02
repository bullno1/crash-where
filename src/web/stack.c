/**
 * @file web/stack.c
 * Reads what an engine prints about an error: the stack text and the message.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "web/platform.h"

static const char token[] = "wasm-function[";

/**
 * The first `wasm-function[` on a line, NULL when it holds none.
 */
static const char*
find_token(const char* line, size_t len) {
	const char* p = strstr(line, token);
	return p != NULL && p < line + len ? p : NULL;
}

/**
 * Offset of the Wasm frame on a line, 0 when the line holds none.
 */
static uint64_t
wasm_offset(const char* line, size_t len) {
	for (const char* p = find_token(line, len); p != NULL; p = find_token(p, len - (size_t)(p - line))) {
		p += sizeof(token) - 1;
		char* after;
		unsigned long index = strtoul(p, &after, 10);
		if (after == p || *after != ']') {
			continue;
		}
		p = after + 1;
		if (p[0] == ':' && p[1] == '0' && p[2] == 'x') {
			uint64_t offset = strtoull(p + 3, &after, 16);
			if (after != p + 3) {
				return offset;
			}
		} else {
			return cw_web_function_offset((uint32_t)index);
		}
	}
	return 0;
}

/**
 * Whether a line is a frame as V8 prints one, `    at ...`.
 */
static bool
v8_frame(const char* line, size_t len) {
	size_t i = 0;
	while (i < len && line[i] == ' ') {
		++i;
	}
	return len - i >= 3 && memcmp(line + i, "at ", 3) == 0;
}

/**
 * The function name of a JavaScript frame line, into `out`.
 *
 * V8: `at NAME (LOCATION)`, or `at LOCATION` for a frame without a
 * name. SpiderMonkey and JavaScriptCore: `NAME@LOCATION`, the name
 * possibly empty.
 */
static void
js_name(const char* line, size_t len, char* out, size_t cap) {
	const char* name = NULL;
	size_t name_len = 0;
	if (v8_frame(line, len)) {
		const char* p = line;
		while (*p == ' ') {
			++p;
		}
		p += 3;
		const char* paren = NULL;
		for (const char* q = p; q + 1 < line + len; ++q) {
			if (q[0] == ' ' && q[1] == '(') {
				paren = q;
				break;
			}
		}
		if (paren != NULL) {
			name = p;
			name_len = (size_t)(paren - p);
		} else {
			name = "<anonymous>";
			name_len = strlen(name);
		}
	} else {
		const char* at = memchr(line, '@', len);
		if (at == NULL) {
			name = "<unknown>";
			name_len = strlen(name);
		} else if (at == line) {
			name = "<anonymous>";
			name_len = strlen(name);
		} else {
			name = line;
			name_len = (size_t)(at - line);
		}
	}
	if (name_len >= cap) {
		name_len = cap - 1;
	}
	memcpy(out, name, name_len);
	out[name_len] = '\0';
	/* The module name is printed without its directory. */
	for (size_t i = 0; i < name_len; ++i) {
		if (out[i] == '/' || out[i] == '\\') {
			out[i] = '_';
		}
	}
}

/**
 * Index of the module named `path`, added when missing; -1 when full.
 */
static int
module_index(cw_crash_info_t* info, const char* path) {
	for (int i = 0; i < info->module_count; ++i) {
		if (strcmp(info->modules[i].path, path) == 0) {
			return i;
		}
	}
	if (info->module_count == CW_MAX_MODULES) {
		return -1;
	}
	cw_module_t* m = &info->modules[info->module_count];
	*m = (cw_module_t){ .build_id = "0" };
	snprintf(m->path, sizeof(m->path), "%s", path);
	return info->module_count++;
}

void
cw_web_parse_stack(const char* stack, cw_crash_info_t* info) {
	/* V8 opens with the error's own text; the frames come after. */
	bool v8 = false;
	for (const char* p = stack; *p != '\0'; ) {
		size_t len = strcspn(p, "\n");
		if (v8_frame(p, len)) {
			v8 = true;
			break;
		}
		p += len + (p[len] == '\n');
	}
	bool started = !v8;
	for (const char* p = stack; *p != '\0' && info->frame_count < CW_MAX_FRAMES; ) {
		size_t len = strcspn(p, "\n");
		const char* line = p;
		p += len + (p[len] == '\n');
		if (len > 0 && line[len - 1] == '\r') {
			--len;
		}
		if (!started) {
			started = v8_frame(line, len);
			if (!started) {
				continue;
			}
		}
		if (len == 0) {
			continue;
		}
		cw_frame_t* frame = &info->frames[info->frame_count];
		uint64_t offset = wasm_offset(line, len);
		if (offset != 0) {
			*frame = (cw_frame_t){ .module = 0, .offset = offset };
		} else if (find_token(line, len) != NULL) {
			/* A Wasm frame nothing can place. */
			continue;
		} else {
			char path[CW_STR_CAP];
			memcpy(path, "javascript:", 11);
			js_name(line, len, path + 11, sizeof(path) - 11);
			*frame = (cw_frame_t){ .module = module_index(info, path), .offset = 0 };
			snprintf(frame->raw, sizeof(frame->raw), "%.*s", (int)len, line);
		}
		info->frame_count++;
	}
}

void
cw_web_trap_type(const char* name, const char* message, char* out, size_t cap) {
	if (name[0] != '\0') {
		snprintf(out, cap, "%s", name);
		return;
	}
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
