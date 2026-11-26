// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2026 Shaopei Zhu <shaopei_1012@163.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/types.h>
#include <linux/ioctl.h>

#include "exfat_ondisk.h"
#include "libexfat.h"

/* From <linux/exfat.h>, which may not be installed yet. */
#ifndef EXFAT_IOC_COMBINE
struct exfat_combine_args {
	__s32 src_fd;
	__u32 flags;
};

#define EXFAT_IOC_COMBINE	_IOW('r', 0x20, struct exfat_combine_args)
#endif

static void usage(void)
{
	fprintf(stderr, "Usage: exfatcombine [-v] DST SRC\n");
	fprintf(stderr, "Append the clusters of SRC to DST without copying data,\n");
	fprintf(stderr, "then remove SRC.\n");
	fprintf(stderr, "If DST is not cluster aligned, it is padded with zeroes\n");
	fprintf(stderr, "up to the next cluster boundary first.\n\n");
	fprintf(stderr, "\t-v | --verbose  Print where the data of SRC was placed\n");
	fprintf(stderr, "\t-V | --version  Show version\n");
	fprintf(stderr, "\t-h | --help     Show help\n");

	exit(EXIT_FAILURE);
}

static struct option opts[] = {
	{"verbose",	no_argument,	NULL,	'v' },
	{"version",	no_argument,	NULL,	'V' },
	{"help",	no_argument,	NULL,	'h' },
	{"?",		no_argument,	NULL,	'?' },
	{NULL,		0,		NULL,	 0  }
};

int main(int argc, char *argv[])
{
	struct exfat_combine_args args = { 0 };
	const char *dst_path, *src_path;
	bool verbose = false;
	struct stat dst_st, src_st;
	int c, dst_fd, src_fd;
	int ret = EXIT_FAILURE;
	uint64_t offset;

	setlocale(LC_ALL, "");

	while ((c = getopt_long(argc, argv, "vVh?", opts, NULL)) != EOF) {
		switch (c) {
		case 'v':
			verbose = true;
			break;
		case 'V':
			show_version(stdout);
			exit(EXIT_SUCCESS);
		case 'h':
		case '?':
		default:
			usage();
		}
	}

	if (argc - optind != 2)
		usage();
	dst_path = argv[optind];
	src_path = argv[optind + 1];

	dst_fd = open(dst_path, O_WRONLY);
	if (dst_fd < 0) {
		exfat_err("open %s: %s\n", dst_path, strerror(errno));
		return EXIT_FAILURE;
	}

	/*
	 * The kernel requires SRC to be open for reading and writing, and
	 * leaves it as an empty file once its clusters belong to DST.
	 */
	src_fd = open(src_path, O_RDWR);
	if (src_fd < 0) {
		exfat_err("open %s: %s\n", src_path, strerror(errno));
		goto close_dst;
	}

	if (fstat(dst_fd, &dst_st) || fstat(src_fd, &src_st)) {
		exfat_err("stat: %s\n", strerror(errno));
		goto close_src;
	}

	/* st_blksize is the cluster size on exFAT. */
	offset = dst_st.st_size;
	if (src_st.st_size && dst_st.st_blksize)
		offset = (offset + dst_st.st_blksize - 1) /
			dst_st.st_blksize * dst_st.st_blksize;

	args.src_fd = src_fd;
	if (ioctl(dst_fd, EXFAT_IOC_COMBINE, &args)) {
		if (errno == ENOTTY)
			exfat_err("%s: not supported by this kernel or filesystem\n",
				  dst_path);
		else
			exfat_err("combine %s into %s: %s\n", src_path, dst_path,
				  strerror(errno));
		goto close_src;
	}

	if (verbose)
		printf("%s: appended %llu bytes at offset %llu, %llu zero bytes of padding\n",
		       dst_path, (unsigned long long)src_st.st_size,
		       (unsigned long long)offset,
		       (unsigned long long)(offset - dst_st.st_size));

	/* SRC is now empty; remove it. */
	if (unlink(src_path)) {
		exfat_err("remove %s: %s\n", src_path, strerror(errno));
		goto close_src;
	}

	ret = EXIT_SUCCESS;
close_src:
	close(src_fd);
close_dst:
	close(dst_fd);
	return ret;
}
