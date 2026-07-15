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

int main(int argc, char *argv[])
{
	test_count_used_clusters_0();
	test_count_used_clusters_1();

	return 0;
}
