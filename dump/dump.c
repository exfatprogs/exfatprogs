// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2021 Namjae Jeon <linkinjeon@kernel.org>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <locale.h>
#include <inttypes.h>

#include "exfat_ondisk.h"
#include "libexfat.h"
#include "exfat_dir.h"
#include "exfat_fs.h"

#define EXFAT_MIN_SECT_SIZE_BITS		9
#define EXFAT_MAX_SECT_SIZE_BITS		12
#define BITS_PER_BYTE				8
#define BITS_PER_BYTE_MASK			0x7

#define dump_field(name, fmt, ...)	\
	exfat_info("%-40s " fmt "\n", name ":", ##__VA_ARGS__)

static const unsigned char used_bit[] = {
	0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 1, 2, 2, 3,/*  0 ~  19*/
	2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 1, 2, 2, 3, 2, 3, 3, 4,/* 20 ~  39*/
	2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5,/* 40 ~  59*/
	4, 5, 5, 6, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,/* 60 ~  79*/
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 2, 3, 3, 4,/* 80 ~  99*/
	3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6,/*100 ~ 119*/
	4, 5, 5, 6, 5, 6, 6, 7, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4,/*120 ~ 139*/
	3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,/*140 ~ 159*/
	2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5,/*160 ~ 179*/
	4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 2, 3, 3, 4, 3, 4, 4, 5,/*180 ~ 199*/
	3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6,/*200 ~ 219*/
	5, 6, 6, 7, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,/*220 ~ 239*/
	4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8             /*240 ~ 255*/
};

static void usage(void)
{
	fprintf(stderr, "Usage: dump.exfat\n");
	fprintf(stderr, "\t-V | --version                        Show version\n");
	fprintf(stderr, "\t-h | --help                           Show help\n");

	exit(EXIT_FAILURE);
}

static struct option opts[] = {
	{"version",		no_argument,		NULL,	'V' },
	{"help",		no_argument,		NULL,	'h' },
	{"?",			no_argument,		NULL,	'?' },
	{NULL,			0,			NULL,	 0  }
};

static unsigned int exfat_count_used_clusters(unsigned char *bitmap,
		unsigned long long bitmap_len)
{
	unsigned int count = 0;
	unsigned long long i;

	for (i = 0; i < bitmap_len; i++)
		count += used_bit[bitmap[i]];

	return count;
}

static int exfat_read_dentry(struct exfat *exfat, struct exfat_inode *inode,
		uint8_t type, struct exfat_dentry *dentry, off_t *dentry_off)
{
	struct exfat_lookup_filter filter = {
		.in.type	= type,
		.in.dentry_count = 0,
		.in.filter	= NULL,
		.in.param	= NULL,
	};
	int retval;

	retval = exfat_lookup_dentry_set(exfat, inode, &filter);
	if (retval) {
		/* entry not found */
		if (retval == EOF) {
			dentry->type = 0;
			return 0;
		}

		return retval;
	}

	*dentry = *filter.out.dentry_set;
	*dentry_off = filter.out.dev_offset;
	free(filter.out.dentry_set);

	return 0;
}

static int exfat_show_ondisk_all_info(struct exfat_blk_dev *bd)
{
	struct pbr *ppbr;
	struct bsx64 *pbsx;
	struct exfat_dentry ed;
	unsigned int bitmap_clu;
	unsigned int total_clus, used_clus, clu_offset, root_clu;
	unsigned long long bitmap_len;
	int ret;
	char *volume_label;
	struct exfat *exfat;
	off_t off;

	exfat = exfat_alloc_exfat(bd, NULL, NULL);
	if (!exfat)
		return -ENOMEM;

	ppbr = exfat->bs;
	if (memcmp(ppbr->bpb.oem_name, "EXFAT   ", 8) != 0) {
		exfat_err("Bad fs_name in boot sector, which does not describe a valid exfat filesystem\n");
		ret = -EINVAL;
		goto free_exfat;
	}

	pbsx = &ppbr->bsx;

	if (pbsx->sect_size_bits < EXFAT_MIN_SECT_SIZE_BITS ||
	    pbsx->sect_size_bits > EXFAT_MAX_SECT_SIZE_BITS) {
		exfat_err("bogus sector size bits : %u\n",
				pbsx->sect_size_bits);
		ret = -EINVAL;
		goto free_exfat;
	}

	if (pbsx->sect_per_clus_bits > 25 - pbsx->sect_size_bits) {
		exfat_err("bogus sectors bits per cluster : %u\n",
				pbsx->sect_per_clus_bits);
		ret = -EINVAL;
		goto free_exfat;
	}

	bd->sector_size_bits = pbsx->sect_size_bits;
	bd->sector_size = 1 << pbsx->sect_size_bits;

	clu_offset = le32_to_cpu(pbsx->clu_offset);
	total_clus = le32_to_cpu(pbsx->clu_count);
	root_clu = le32_to_cpu(pbsx->root_cluster);

	exfat_info("-------------- Dump Boot sector region --------------\n");
	dump_field("Volume Length(sectors)", "%" PRIu64,
			le64_to_cpu(pbsx->vol_length));
	dump_field("FAT Offset(sector offset)", "%u",
			le32_to_cpu(pbsx->fat_offset));
	dump_field("FAT Length(sectors)", "%u",
			le32_to_cpu(pbsx->fat_length));
	dump_field("Cluster Heap Offset (sector offset)", "%u", clu_offset);
	dump_field("Cluster Count", "%u", total_clus);
	dump_field("Root Cluster (cluster offset)", "%u", root_clu);
	dump_field("Volume Serial", "0x%x", le32_to_cpu(pbsx->vol_serial));
	dump_field("Bytes per Sector", "%u", 1 << pbsx->sect_size_bits);
	dump_field("Sectors per Cluster", "%u", 1 << pbsx->sect_per_clus_bits);

	bd->cluster_size =
		1 << (pbsx->sect_per_clus_bits + pbsx->sect_size_bits);

	exfat_info("\n----------------- Dump Root entries -----------------\n");

	ret = exfat_read_dentry(exfat, exfat->root, EXFAT_VOLUME, &ed, &off);
	if (ret)
		goto free_exfat;

	if (ed.type == EXFAT_VOLUME) {
		dump_field("Volume label entry position", "0x%llx", (unsigned long long)off);
		dump_field("Volume label character count", "%u", ed.vol_char_cnt);
		volume_label = exfat_conv_volume_label(&ed);
		if (!volume_label)
			dump_field("Volume label", "%s", "<invalid>");
		else
			dump_field("Volume label", "%s", volume_label);
		free(volume_label);
	}

	ret = exfat_read_dentry(exfat, exfat->root, EXFAT_UPCASE, &ed, &off);
	if (ret)
		goto free_exfat;

	if (ed.type == EXFAT_UPCASE) {
		dump_field("Upcase table entry position", "0x%llx", (unsigned long long)off);
		dump_field("Upcase table start cluster", "%x",
				le32_to_cpu(ed.upcase_start_clu));
		dump_field("Upcase table size", "%" PRIu64,
				le64_to_cpu(ed.upcase_size));
	}

	ret = exfat_read_dentry(exfat, exfat->root, EXFAT_BITMAP, &ed, &off);
	if (ret)
		goto free_exfat;

	if (ed.type == EXFAT_BITMAP) {
		bitmap_len = le64_to_cpu(ed.bitmap_size);
		bitmap_clu = le32_to_cpu(ed.bitmap_start_clu);

		dump_field("Bitmap entry position", "0x%llx", (unsigned long long)off);
		dump_field("Bitmap start cluster", "%x", bitmap_clu);
		dump_field("Bitmap size", "%llu", bitmap_len);

		if (bitmap_len > EXFAT_BITMAP_SIZE(exfat->clus_count)) {
			exfat_err("Invalid bitmap size\n");
			ret = -EINVAL;
			goto free_exfat;
		}

		ret = exfat_read(bd->dev_fd, exfat->disk_bitmap, bitmap_len,
				exfat_c2o(exfat, bitmap_clu));
		if (ret < 0) {
			exfat_err("bitmap read failed: %d\n", errno);
			ret = -EIO;
		}

		used_clus = exfat_count_used_clusters(
				(unsigned char *)exfat->disk_bitmap,
				bitmap_len);

		exfat_info("\n---------------- Show the statistics ----------------\n");
		dump_field("Cluster size", "%u", bd->cluster_size);
		dump_field("Total Clusters", "%u", exfat->clus_count);
		dump_field("Free Clusters", "%u", exfat->clus_count - used_clus);
	}

	ret = 0;

free_exfat:
	exfat_free_exfat(exfat);

	return ret;
}

int main(int argc, char *argv[])
{
	int c;
	int ret = EXIT_FAILURE;
	struct exfat_blk_dev bd;
	struct exfat_user_input ui;
	bool version_only = false;

	init_user_input(&ui);
	ui.writeable = false;

	if (!setlocale(LC_CTYPE, ""))
		exfat_err("failed to init locale/codeset\n");

	opterr = 0;
	while ((c = getopt_long(argc, argv, "iVh", opts, NULL)) != EOF)
		switch (c) {
		case 'V':
			version_only = true;
			break;
		case '?':
		case 'h':
		default:
			usage();
	}

	show_version();
	if (version_only)
		exit(EXIT_FAILURE);

	if (argc - optind != 1)
		usage();

	ui.dev_name = argv[1];

	ret = exfat_get_blk_dev_info(&ui, &bd);
	if (ret < 0)
		goto out;

	ret = exfat_show_ondisk_all_info(&bd);
	close(bd.dev_fd);

out:
	return ret;
}
