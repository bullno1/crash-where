/**
 * @file host.c
 * The collector object and the platform-independent probes; the
 * platform backend does the rest.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cw_host.h"
#include "backend.h"
#include "internal.h"

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#	define CW_HOST_X86 1
#	if defined(_MSC_VER)
#		include <intrin.h>
#	else
#		include <cpuid.h>
#	endif
#endif

void
cw_host_set_u64(const char* key, uint64_t value) {
	char buf[24];
	snprintf(buf, sizeof(buf), "%" PRIu64, value);
	cw_set_env(key, buf);
}

#if defined(CW_HOST_X86)

static void
cpuid_leaf(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
#	if defined(_MSC_VER)
	int t[4];
	__cpuidex(t, (int)leaf, (int)sub);
	memcpy(r, t, sizeof(t));
#	else
	__cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]);
#	endif
}

/** Enabled extended register state, for AVX and AVX-512 OS support. */
static uint64_t
xcr0(void) {
#	if defined(_MSC_VER)
	return _xgetbv(0);
#	else
	uint32_t a, d;
	__asm__ volatile("xgetbv" : "=a"(a), "=d"(d) : "c"(0));
	return ((uint64_t)d << 32) | a;
#	endif
}

void
cw_host_cpu_features(char* out, size_t cap) {
	out[0] = '\0';
	uint32_t r[4];
	cpuid_leaf(0, 0, r);
	uint32_t max_leaf = r[0];
	if (max_leaf < 1) {
		return;
	}
	cpuid_leaf(1, 0, r);
	uint32_t ecx1 = r[2];
	uint32_t ebx7 = 0;
	if (max_leaf >= 7) {
		cpuid_leaf(7, 0, r);
		ebx7 = r[1];
	}
	bool osxsave = (ecx1 & (1u << 27)) != 0;
	uint64_t x = osxsave ? xcr0() : 0;
	bool avx_os = (x & 0x6) == 0x6;
	bool avx512_os = (x & 0xe6) == 0xe6;

	const struct {
		const char* name;
		bool present;
	} features[] = {
		{ "sse3",    (ecx1 & (1u << 0)) != 0 },
		{ "ssse3",   (ecx1 & (1u << 9)) != 0 },
		{ "sse4.1",  (ecx1 & (1u << 19)) != 0 },
		{ "sse4.2",  (ecx1 & (1u << 20)) != 0 },
		{ "popcnt",  (ecx1 & (1u << 23)) != 0 },
		{ "aes",     (ecx1 & (1u << 25)) != 0 },
		{ "fma",     (ecx1 & (1u << 12)) != 0 && avx_os },
		{ "f16c",    (ecx1 & (1u << 29)) != 0 && avx_os },
		{ "avx",     (ecx1 & (1u << 28)) != 0 && avx_os },
		{ "avx2",    (ebx7 & (1u << 5)) != 0 && avx_os },
		{ "bmi1",    (ebx7 & (1u << 3)) != 0 },
		{ "bmi2",    (ebx7 & (1u << 8)) != 0 },
		{ "avx512f", (ebx7 & (1u << 16)) != 0 && avx512_os },
	};
	size_t len = 0;
	for (size_t i = 0; i < sizeof(features) / sizeof(features[0]); ++i) {
		if (!features[i].present) {
			continue;
		}
		int n = snprintf(out + len, cap - len, "%s%s", len > 0 ? " " : "", features[i].name);
		if (n < 0 || (size_t)n >= cap - len) {
			break;
		}
		len += (size_t)n;
	}
}

#else

void
cw_host_cpu_features(char* out, size_t cap) {
	(void)cap;
	out[0] = '\0';
}

#endif

/**
 * One object serves both config slots: the core knows which process
 * it is running in.
 */
static void
host_collect(void* user) {
	(void)user;
	if (cw_ctx.active) {
		cw_host_backend_init();
	} else if (cw_ctx.game_pid != 0) {
		cw_host_backend_report(cw_ctx.game_pid);
	}
}

const cw_collector_t cw_collector_host = {
	.collect = host_collect,
	.user = NULL,
};
