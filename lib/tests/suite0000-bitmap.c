// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include "exfat_ondisk.h"
#include "libexfat.h"

/* exfat_count_used_clusters() out of bounds checks. */
static void test_count_used_clusters_0(void)
{
	static bitmap_t bm[2];
	const static unsigned int tot_clus = sizeof(bm) / sizeof(bm[0]) * BITS_PER;
	unsigned int ret;

	memset(&bm, 0xFF, sizeof(bm));

	/* This is no-op. */
	ret = exfat_count_used_clusters(&bm, 0, tot_clus);
	assert(ret == 0);

	/* normal use cases */

	ret = exfat_count_used_clusters(&bm, sizeof(bm), tot_clus);
	assert(ret == tot_clus);

	for (unsigned int i = 0; i <= tot_clus; i++) {
		ret = exfat_count_used_clusters(&bm, sizeof(bm), i);
		assert(ret == i);
	}

	/* alloc len < content len */
	for (unsigned int i = tot_clus + 1; i < tot_clus + 10; i++) {
		ret = exfat_count_used_clusters(&bm, sizeof(bm), i);
		assert(ret == tot_clus);
	}
}

/* Tests if exfat_count_used_clusters() doesn't count the garbage ones at the end. */
static void test_count_used_clusters_1(void)
{
	static const unsigned int END = 0x1000;
	bool seen_garbage = false;

	for (unsigned int clus_cnt = 1; clus_cnt < END; clus_cnt++) {
		const size_t bm_len = DIV_ROUND_UP(clus_cnt, 8);
		const size_t bm_size = EXFAT_BITMAP_SIZE(clus_cnt);
		unsigned char *m = malloc(bm_size);
		unsigned int ret;

		assert(bm_len <= bm_size);

		/* All ones. */
		memset(m, UCHAR_MAX, bm_size);
		ret = exfat_count_used_clusters(m, bm_len, clus_cnt);
		assert(ret == clus_cnt);

		/* All valid bits zeros, all garbage ones. */
		for (clus_t i = 0, j = EXFAT_FIRST_CLUSTER; i < clus_cnt; i++, j++)
			exfat_bitmap_clear(m, j);
		ret = exfat_count_used_clusters(m, bm_len, clus_cnt);
		assert(ret == 0);

		/* Make sure this test is valid(garbage should still be there). */
		if (m[bm_len - 1] != 0)
			seen_garbage = true;

		free(m);
	}

	assert(seen_garbage);
}

/* Tests bitmap manipulation functions. */
static void test_bitmap_func(void)
{
	static const clus_t ARR[] = {
		1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 1023,
		1024, 1025, 1026, 1027, 1028, 1029, 1030, 1031, 1032, 1032,
		65543, 65544,
		0
	};

	for (const clus_t *cc = ARR; *cc != 0; cc++) {
		const size_t bm_len = DIV_ROUND_UP(*cc, 8);
		const size_t bm_size = EXFAT_BITMAP_SIZE(*cc);
		void *m = calloc(bm_size, 1);
		unsigned int clus_cnt;

		assert(bm_len > 0 && bm_len <= bm_size);

		/* Fault the pages just for the sake of it. */
		clus_cnt = exfat_count_used_clusters(m, bm_len, *cc);
		assert(clus_cnt == 0);

		/* 1x1 */
		for (clus_t i = 0, j = EXFAT_FIRST_CLUSTER; i < *cc; i++, j++) {
			bool b;

			/* Test set(). */
			exfat_bitmap_set(m, j);
			b = exfat_bitmap_get(m, j);
			clus_cnt = exfat_count_used_clusters(m, bm_len, *cc);
			assert(b);
			assert(clus_cnt == 1);

			/* Test clear(). */
			exfat_bitmap_clear(m, j);
			clus_cnt = exfat_count_used_clusters(m, bm_len, *cc);
			b = exfat_bitmap_get(m, j);
			assert(!b);
			assert(clus_cnt == 0);
		}

		/* SxS */
		for (clus_t i = 0, j = EXFAT_FIRST_CLUSTER; i < *cc - 1; i++, j++) {
			bool b1, b2;

			/* Test set(). */
			exfat_bitmap_set(m, j);
			b1 = exfat_bitmap_get(m, j);
			b2 = exfat_bitmap_get(m, j + 1);
			assert(b1 && !b2);
			clus_cnt = exfat_count_used_clusters(m, bm_len, *cc);
			assert(clus_cnt == 1);

			exfat_bitmap_set(m, j + 1);
			b1 = exfat_bitmap_get(m, j);
			b2 = exfat_bitmap_get(m, j + 1);
			assert(b1 && b2);
			clus_cnt = exfat_count_used_clusters(m, bm_len, *cc);
			assert(clus_cnt == 2);

			/* Test clear(). */
			exfat_bitmap_clear(m, j);
			b1 = exfat_bitmap_get(m, j);
			b2 = exfat_bitmap_get(m, j + 1);
			assert(!b1 && b2);
			clus_cnt = exfat_count_used_clusters(m, bm_len, *cc);
			assert(clus_cnt == 1);

			exfat_bitmap_clear(m, j + 1);
			b1 = exfat_bitmap_get(m, j);
			b2 = exfat_bitmap_get(m, j + 1);
			assert(!b1 && !b2);
			clus_cnt = exfat_count_used_clusters(m, bm_len, *cc);
			assert(clus_cnt == 0);
		}

		free(m);
	}
}

/*
 * Data-driven test of bitmap manipulation functions against endianness confusion
 *
 * This doesn't really mean anything on LE machines as the on-disk format is
 * already in LE. Make sure this is run on BE machines.
 */
static void test_bitmap_endian(void)
{
	static const unsigned char EXPECTED_1[] = {
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	static const unsigned char EXPECTED_2[] = {
		0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
	};
	static const unsigned char EXPECTED_3[] = {
		0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
	};
	unsigned char buf[sizeof(EXPECTED_1)];
	int ret;

	assert(sizeof(buf) % sizeof(bitmap_t) == 0);
	assert(sizeof(buf) == sizeof(EXPECTED_1));
	assert(sizeof(buf) == sizeof(EXPECTED_2));
	assert(sizeof(buf) == sizeof(EXPECTED_3));

	memset(buf, 0, sizeof(buf));
	exfat_bitmap_set(buf, EXFAT_FIRST_CLUSTER + 0);
	ret = memcmp(buf, EXPECTED_1, sizeof(buf));
	assert(ret == 0);

	memset(buf, 0, sizeof(buf));
	exfat_bitmap_set(buf, EXFAT_FIRST_CLUSTER + 32);
	ret = memcmp(buf, EXPECTED_2, sizeof(buf));
	assert(ret == 0);

	memset(buf, 0, sizeof(buf));
	exfat_bitmap_set(buf, EXFAT_FIRST_CLUSTER + 0);
	exfat_bitmap_set(buf, EXFAT_FIRST_CLUSTER + 32);
	ret = memcmp(buf, EXPECTED_3, sizeof(buf));
	assert(ret == 0);
}

/* Tests bitmap size calculation near the maximum cluster count. */
static void test_bitmap_size(void)
{
	const clus_t clus_cnt = EXFAT_MAX_NUM_CLUSTER - 1;
	const size_t expected =
		DIV_ROUND_UP((uint64_t)clus_cnt, BITS_PER) *
		sizeof(bitmap_t);
	const size_t actual = EXFAT_BITMAP_SIZE(clus_cnt);

	assert(expected != 0);
	assert(actual == expected);
}

int main(int argc, char *argv[])
{
	test_bitmap_size();
	test_count_used_clusters_0();
	test_count_used_clusters_1();
	test_bitmap_func();
	test_bitmap_endian();

	return 0;
}
