// SPDX-License-Identifier: GPL-2.0-or-later
#include "exfat_ondisk.h"
#include "libexfat.h"

#include <assert.h>

#define SET_SWVER(o, a, b, c)		\
	do {				\
		(o)[0] = (a);		\
		(o)[1] = (b);		\
		(o)[2] = (c);		\
	} while (0)

static void test_swver_cmp(void)
{
	unsigned short first[3], second[3];
#define CMP_VER(a, b, c, CMP, d, e, f)\
	do {								\
		SET_SWVER(first, (a), (b), (c));			\
		SET_SWVER(second, (d), (e), (f));			\
		assert(exfat_cmp_swver(first, second) CMP 0);		\
	} while (0)

	CMP_VER(0, 0, 0, ==, 0, 0, 0);
	CMP_VER(2, 6, 0, ==, 2, 6, 0);
	CMP_VER(1, 2, 3, ==, 1, 2, 3);

	CMP_VER(2, 6, 0, >, 2, 0, 0);
	CMP_VER(2, 6, 0, >, 2, 4, 0);
	CMP_VER(2, 6, 0, >, 2, 4, 999);

	CMP_VER(0, 0, 1, <, 65535, 65535, 65535);
	CMP_VER(2, 6, 0, <, 2, 6, 1);
	CMP_VER(2, 6, 0, <, 3, 0, 0);
	CMP_VER(2, 6, 0, <, 7, 0, 0);
	CMP_VER(2, 6, 0, <, 7, 2, 0);
	CMP_VER(2, 6, 0, <, 7, 1, 8);
	CMP_VER(2, 6, 0, <, 6, 18, 44);
	CMP_VER(2, 6, 0, <, 6, 12, 103);
	CMP_VER(2, 6, 0, <, 6, 6, 151);
	CMP_VER(2, 6, 0, <, 6, 1, 182);
	CMP_VER(2, 6, 0, <, 5, 15, 215);
	CMP_VER(2, 6, 0, <, 5, 10, 0);
}

static void test_swver_parse(void)
{
	unsigned short ver[3];
	int ret;
#define EXPECT_VER(s, a, b, c)\
	do {									\
		ret = exfat_parse_swver((s), ver);				\
		assert(ret == 0);						\
		assert((a) == ver[0] && (b) == ver[1] && (c) == ver[2]);	\
	} while (0)

	assert(exfat_parse_swver("", ver));
	assert(exfat_parse_swver("asd", ver));
	assert(exfat_parse_swver("asd-1.2.3", ver));
	assert(exfat_parse_swver("0.0.0", ver) == 0);
	assert(exfat_parse_swver("7.1.8-200.fc44.x86_64", ver) == 0);
	assert(exfat_parse_swver("2.6.0", ver) == 0);
	assert(exfat_parse_swver("2.6.0aaaa", ver) == 0);

	EXPECT_VER("6.1.157-android14-11-gbd23337e42e7-ab14791245", 6, 1, 157);
	EXPECT_VER("5.4.242-32179049-abG990EXXSNHZF5", 5, 4, 242);
	EXPECT_VER("6.6.77-8.el10.altarch.aarch64+64k", 6, 6, 77);
	EXPECT_VER("7.1.8-200.fc44.x86_64", 7, 1, 8);
	EXPECT_VER("7.0.8-200.fc44.x86_64", 7, 0, 8);
	EXPECT_VER("6.17.0-1019-aws", 6, 17, 0);
	EXPECT_VER("6.18.39-0-virt", 6, 18, 39);
	EXPECT_VER("0.18.39-0-virt", 0, 18, 39);
	EXPECT_VER("6.0.39-0-virt", 6, 0, 39);
	EXPECT_VER("2foo.6", 2, 0, 0);
	EXPECT_VER("2.6foo.3", 2, 6, 0);
	EXPECT_VER("2.", 2, 0, 0);
	EXPECT_VER("2.6.", 2, 6, 0);
	EXPECT_VER("0", 0, 0, 0);
	EXPECT_VER("42", 42, 0, 0);

	/*
	 * Note that the following case doesn't happen on Linux because the
	 * Linux kernel always returns a version in three numbers:
	 *
	 *   "7.0.0" not "7" (although the tarball would be named "7.0")
	 *   "7.1.0" not "7.1" (although the tarball would be named "7.1")
	 *
	 * The following test is only for correctness in implementation so that
	 * there's no confusion in the future with values from other software.
	 */
	EXPECT_VER("6.77-8.el10.altarch.aarch64+64k", 6, 77, 0);
#undef EXPECT_VER
}

int main(void)
{
	test_swver_cmp();
	test_swver_parse();

	return 0;
}
