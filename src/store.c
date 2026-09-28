/**
 * @file store.c
 * The pending store: envelopes under `<report_dir>/pending/`, described
 * by their names alone so none has to be parsed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

static const char kind_letters[] = { 'c', 'h', 'a' };

char
cw_report_kind_letter(cw_report_kind_t kind) {
	return (unsigned)kind < sizeof(kind_letters) ? kind_letters[kind] : 'c';
}

bool
cw_pending_parse(const char* name, cw_pending_t* out) {
	static const char ext[] = ".json";
	size_t len = strlen(name);
	if (len >= sizeof(out->name) || len <= sizeof(ext) || strcmp(name + len - (sizeof(ext) - 1), ext) != 0) {
		return false;
	}
	char* end;
	long long ts = strtoll(name, &end, 10);
	if (end == name || end[0] != '_' || end[1] == '\0' || end[2] != '_') {
		return false;
	}
	cw_report_kind_t kind = CW_REPORT_CRASH;
	bool known = false;
	for (size_t i = 0; i < sizeof(kind_letters); ++i) {
		if (end[1] == kind_letters[i]) {
			kind = (cw_report_kind_t)i;
			known = true;
		}
	}
	if (!known) {
		return false;
	}
	*out = (cw_pending_t){ .ts = ts, .kind = kind };
	memcpy(out->name, name, len + 1);
	return true;
}

void
cw_pending_remove(const cw_pending_t* p) {
	static const char* const sidecars[] = { ".json", ".dmp", ".log", ".snap" };
	size_t stem = strlen(p->name) - 5;
	for (size_t i = 0; i < sizeof(sidecars) / sizeof(sidecars[0]); ++i) {
		char path[CW_STR_CAP + 128];
		snprintf(path, sizeof(path), "%s/pending/%.*s%s", cw_ctx.report_dir, (int)stem, p->name, sidecars[i]);
		remove(path);
	}
}
