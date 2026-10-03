/**
 * @file compress.c
 * gzip for request bodies: a vendored deflate encoder inside the
 * ten-byte header and the CRC-32 trailer the format asks for.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

/*
 * The encoder is compiled with internal linkage so nothing of it leaks
 * from the library; its zlib variant then goes unused, and it leaves a
 * parameter unused, both of which the warning level here flags.
 */
#if defined(_MSC_VER)
#	pragma warning(push)
#	pragma warning(disable : 4100 4505)
#elif defined(__GNUC__)
#	pragma GCC diagnostic push
#	pragma GCC diagnostic ignored "-Wunused-function"
#	pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#define SDEFL_API static
#define SDEFL_IMPLEMENTATION
#include "vendor/sdefl.h"
#if defined(_MSC_VER)
#	pragma warning(pop)
#elif defined(__GNUC__)
#	pragma GCC diagnostic pop
#endif

#define CW_GZIP_HEADER  10
#define CW_GZIP_TRAILER 8
/** Largest input the encoder takes; well above any body the library sends. */
#define CW_GZIP_MAX     (256u * 1024u * 1024u)

static uint32_t
crc32(const uint8_t* p, size_t len) {
	static uint32_t table[256];
	if (table[1] == 0) {
		for (uint32_t i = 0; i < 256; ++i) {
			uint32_t c = i;
			for (int k = 0; k < 8; ++k) {
				c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
			}
			table[i] = c;
		}
	}
	uint32_t crc = 0xffffffffu;
	for (size_t i = 0; i < len; ++i) {
		crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
	}
	return crc ^ 0xffffffffu;
}

static void
put_le32(uint8_t* p, uint32_t v) {
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

void*
cw_gzip(const void* in, size_t len, size_t* out_len) {
	if (len > CW_GZIP_MAX) {
		return NULL;
	}
	/* The encoder's state is about a megabyte; it lives on the heap for the call. */
	struct sdefl* state = calloc(1, sizeof(*state));
	size_t cap = CW_GZIP_HEADER + (size_t)sdefl_bound((int)len) + CW_GZIP_TRAILER;
	uint8_t* out = malloc(cap);
	if (state == NULL || out == NULL) {
		free(state);
		free(out);
		return NULL;
	}
	static const uint8_t header[CW_GZIP_HEADER] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255 };
	memcpy(out, header, sizeof(header));
	int n = sdeflate(state, out + CW_GZIP_HEADER, in, (int)len, SDEFL_LVL_DEF);
	free(state);
	if (n < 0) {
		free(out);
		return NULL;
	}
	uint8_t* trailer = out + CW_GZIP_HEADER + n;
	put_le32(trailer, crc32(in, len));
	put_le32(trailer + 4, (uint32_t)len);
	*out_len = CW_GZIP_HEADER + (size_t)n + CW_GZIP_TRAILER;
	return out;
}
