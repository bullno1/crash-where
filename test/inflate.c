/**
 * @file inflate.c
 * gzip decoding for the tests, so what the library compresses can be
 * checked byte for byte against what it was given.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "scenario.h"

/* The decoder keeps a helper its scalar path never calls. */
#if defined(__GNUC__)
#	pragma GCC diagnostic push
#	pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define SINFL_IMPLEMENTATION
#include "sinfl.h"
#if defined(__GNUC__)
#	pragma GCC diagnostic pop
#endif

uint32_t
test_crc32(const void* data, size_t len) {
	const uint8_t* p = data;
	uint32_t crc = 0xffffffffu;
	for (size_t i = 0; i < len; ++i) {
		crc ^= p[i];
		for (int k = 0; k < 8; ++k) {
			crc = (crc & 1) ? 0xedb88320u ^ (crc >> 1) : crc >> 1;
		}
	}
	return crc ^ 0xffffffffu;
}

static uint32_t
le32(const uint8_t* p) {
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

uint8_t*
test_gunzip(const void* in, size_t len, size_t* out_len) {
	const uint8_t* p = in;
	/* The library writes no optional fields, so the header is exactly ten bytes. */
	if (len < 18 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8 || p[3] != 0) {
		return NULL;
	}
	uint32_t crc = le32(p + len - 8);
	uint32_t size = le32(p + len - 4);
	uint8_t* out = malloc((size_t)size + 1);
	if (out == NULL) {
		return NULL;
	}
	int n = sinflate(out, (int)size, p + 10, (int)(len - 18));
	if (n != (int)size || test_crc32(out, size) != crc) {
		free(out);
		return NULL;
	}
	*out_len = size;
	return out;
}
