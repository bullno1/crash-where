/**
 * @file compress.c
 * The gzip the library writes, decoded by an independent inflater and
 * checked against the input and against the format's trailer.
 */
#include <stdlib.h>
#include <string.h>

#include "btest.h"
#include "internal.h"
#include "scenario.h"

static btest_suite_t compress = {
	.name = "compress",
};

/**
 * Compress, decode, and compare; the trailer must carry the input's
 * checksum and size.
 */
static void
check_round_trip(const void* in, size_t len, size_t* zipped_len) {
	size_t zlen = 0;
	uint8_t* z = cw_gzip(in, len, &zlen);
	BTEST_ASSERT(z != NULL);
	BTEST_ASSERT_RELATION("%zu", zlen, >=, (size_t)18);
	BTEST_EXPECT_EQUAL("%d", z[0], 0x1f);
	BTEST_EXPECT_EQUAL("%d", z[1], 0x8b);
	BTEST_EXPECT_EQUAL("%d", z[2], 8);
	uint32_t crc = (uint32_t)z[zlen - 8] | (uint32_t)z[zlen - 7] << 8 | (uint32_t)z[zlen - 6] << 16 | (uint32_t)z[zlen - 5] << 24;
	uint32_t size = (uint32_t)z[zlen - 4] | (uint32_t)z[zlen - 3] << 8 | (uint32_t)z[zlen - 2] << 16 | (uint32_t)z[zlen - 1] << 24;
	BTEST_EXPECT_EQUAL("%u", crc, test_crc32(in, len));
	BTEST_EXPECT_EQUAL("%u", size, (uint32_t)len);

	size_t out_len = 0;
	uint8_t* out = test_gunzip(z, zlen, &out_len);
	BTEST_ASSERT(out != NULL);
	BTEST_EXPECT_EQUAL("%zu", out_len, len);
	BTEST_EXPECT(out_len == len && (len == 0 || memcmp(out, in, len) == 0));
	free(out);
	free(z);
	*zipped_len = zlen;
}

BTEST(compress, text_round_trips_smaller) {
	static char text[8192];
	for (size_t i = 0; i < sizeof(text); ++i) {
		text[i] = "{\"frames\":[{\"module\":\"game\",\"offset\":1234}],"[i % 46];
	}
	size_t zlen;
	check_round_trip(text, sizeof(text), &zlen);
	BTEST_EXPECT_RELATION("%zu", zlen, <, sizeof(text) / 4);
}

BTEST(compress, zeros_nearly_vanish) {
	size_t len = 1024 * 1024;
	void* zeros = calloc(1, len);
	BTEST_ASSERT(zeros != NULL);
	size_t zlen;
	check_round_trip(zeros, len, &zlen);
	BTEST_EXPECT_RELATION("%zu", zlen, <, (size_t)8192);
	free(zeros);
}

BTEST(compress, noise_survives) {
	static uint8_t noise[65536];
	uint32_t x = 0x12345678u;
	for (size_t i = 0; i < sizeof(noise); ++i) {
		x = x * 1664525u + 1013904223u;
		noise[i] = (uint8_t)(x >> 24);
	}
	size_t zlen;
	check_round_trip(noise, sizeof(noise), &zlen);
	/* Stored blocks at worst: a little over the input. */
	BTEST_EXPECT_RELATION("%zu", zlen, <, sizeof(noise) + 1024);
}

BTEST(compress, empty_input) {
	size_t zlen;
	check_round_trip("", 0, &zlen);
}
