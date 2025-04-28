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

#define DUMP_SCAN_DIR			(1 << 0)
#define DUMP_SCAN_DIR_RECURSIVE		(1 << 1)
#define DUMP_CLUSTER_CHAIN		(1 << 2)

#define dump_field(name, fmt, ...)	\
	exfat_info("%-40s " fmt "\n", name ":", ##__VA_ARGS__)
#define dump_dentry_field(name, fmt, ...)	\
	exfat_info("   %-30s  " fmt "\n", name ":", ##__VA_ARGS__)
#define dump_dentry_field_wrap(fmt, ...)	\
	exfat_info("   %-30s  " fmt "\n", "", ##__VA_ARGS__)

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
	fprintf(stderr, "\t-d | --dentry-set=path                Show directory entry set\n");
	fprintf(stderr, "\t-c | --cluster-chain                  Show cluster chain\n");
	fprintf(stderr,
		"\t-s | --scan-dir=dir-path              Scan and show directory entry sets\n");
	fprintf(stderr,
		"\t-r | --recursive                      Scan and show directory entry sets recursively\n");
	fprintf(stderr, "\t-V | --version                        Show version\n");
	fprintf(stderr, "\t-h | --help                           Show help\n");

	exit(EXIT_FAILURE);
}

static struct option opts[] = {
	{"dentry-set",		required_argument,	NULL,	'd' },
	{"scan-dir",		required_argument,	NULL,	's' },
	{"recursive",		no_argument,		NULL,	'r' },
	{"cluster-chain",	no_argument,		NULL,	'c' },
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

static int exfat_show_fs_info(struct exfat *exfat)
{
	struct pbr *ppbr;
	struct bsx64 *pbsx;
	struct exfat_blk_dev *bd = exfat->blk_dev;
	struct exfat_dentry ed;
	unsigned int bitmap_clu;
	unsigned int total_clus, used_clus, clu_offset, root_clu;
	unsigned long long bitmap_len;
	int ret;
	char *volume_label;
	off_t off;

	ppbr = exfat->bs;
	if (memcmp(ppbr->bpb.oem_name, "EXFAT   ", 8) != 0) {
		exfat_err("Bad fs_name in boot sector, which does not describe a valid exfat filesystem\n");
		return -EINVAL;
	}

	pbsx = &ppbr->bsx;

	if (pbsx->sect_size_bits < EXFAT_MIN_SECT_SIZE_BITS ||
	    pbsx->sect_size_bits > EXFAT_MAX_SECT_SIZE_BITS) {
		exfat_err("bogus sector size bits : %u\n",
				pbsx->sect_size_bits);
		return -EINVAL;
	}

	if (pbsx->sect_per_clus_bits > 25 - pbsx->sect_size_bits) {
		exfat_err("bogus sectors bits per cluster : %u\n",
				pbsx->sect_per_clus_bits);
		return -EINVAL;
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
		return ret;

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
		return ret;

	if (ed.type == EXFAT_UPCASE) {
		dump_field("Upcase table entry position", "0x%llx", (unsigned long long)off);
		dump_field("Upcase table start cluster", "%x",
				le32_to_cpu(ed.upcase_start_clu));
		dump_field("Upcase table size", "%" PRIu64,
				le64_to_cpu(ed.upcase_size));
	}

	ret = exfat_read_dentry(exfat, exfat->root, EXFAT_BITMAP, &ed, &off);
	if (ret)
		return ret;

	if (ed.type == EXFAT_BITMAP) {
		bitmap_len = le64_to_cpu(ed.bitmap_size);
		bitmap_clu = le32_to_cpu(ed.bitmap_start_clu);

		dump_field("Bitmap entry position", "0x%llx", (unsigned long long)off);
		dump_field("Bitmap start cluster", "%x", bitmap_clu);
		dump_field("Bitmap size", "%llu", bitmap_len);

		if (bitmap_len > EXFAT_BITMAP_SIZE(exfat->clus_count)) {
			exfat_err("Invalid bitmap size\n");
			return -EINVAL;
		}

		ret = exfat_read(bd->dev_fd, exfat->disk_bitmap, bitmap_len,
				exfat_c2o(exfat, bitmap_clu));
		if (ret < 0) {
			exfat_err("bitmap read failed: %d\n", errno);
			return -EIO;
		}

		used_clus = exfat_count_used_clusters(
				(unsigned char *)exfat->disk_bitmap,
				bitmap_len);

		exfat_info("\n---------------- Show the statistics ----------------\n");
		dump_field("Cluster size", "%u", bd->cluster_size);
		dump_field("Total Clusters", "%u", exfat->clus_count);
		dump_field("Free Clusters", "%u", exfat->clus_count - used_clus);
	}

	return 0;
}

/*
 * Get the first level file name from a given path
 *
 * Input
 *  path: The path of the file/directory.
 *  name_size: the size of 'name'.
 * Output
 *  name: the file name in the first level of the path.
 * Return
 *  The length of the path to jump to the next level.
 */
static int get_name_from_path(const char *path, char *name, size_t name_size)
{
	int i;
	int name_len = 0;
	int path_len = strlen(path);

	if (path_len == 0)
		return 0;

	for (i = 0; i <= path_len && name_len + 1 < name_size; i++, path++) {
		if (*path == '/' || *path == '\0') {
			if (name_len == 0)
				continue;

			name[name_len] = 0;
			return i;
		}

		name[name_len] = *path;
		name_len++;
	}

	name[0] = 0;
	return 0;
}

/*
 * Create a inode for a given path
 *
 * Input
 *   path: the path of the file/directory.
 * Output
 *   new: the new inode is created for the file/directory.
 *        If path is '/', it is a copy of exfat->root.
 *   dir_is_contiguous: Whether the clusters of the parent directory are
 *                      contiguous.
 * Return
 *   0 on success
 *   -error code on failure
 */
static int exfat_create_inode_by_path(struct exfat *exfat, const char *path,
		struct exfat_inode **new, bool *dir_is_contiguous)
{
	int len, ret;
	char name[PATH_MAX + 1];
	struct exfat_inode *inode;
	struct exfat_dentry *dentry_set;
	struct exfat_lookup_filter filter;
	const char *p_path = path;

	inode = exfat_alloc_inode(ATTR_SUBDIR);
	if (!inode)
		return -ENOMEM;

	*inode = *exfat->root;
	*dir_is_contiguous = inode->is_contiguous;

	do {
		if ((inode->attr & ATTR_SUBDIR) == 0 && *p_path != '\0') {
			ret = -ENOENT;
			goto free_inode;
		}

		len = get_name_from_path(p_path, name, sizeof(name));
		p_path += len;
		if (name[0] == '\0' || len == 0) {
			*new = inode;
			return 0;
		}

		ret = exfat_utf16_enc(name, inode->name, NAME_BUFFER_SIZE);
		if (ret < 0)
			goto free_inode;

		ret = exfat_lookup_file_by_utf16name(exfat, inode, inode->name,
						     &filter);
		if (ret) {
			if (ret == EOF)
				ret = -ENOENT;
			goto free_inode;
		}

		dentry_set = filter.out.dentry_set;
		if (inode->dentry_set)
			free(inode->dentry_set);
		inode->dentry_set = dentry_set;
		inode->dev_offset = filter.out.dev_offset;
		inode->dentry_count = filter.out.dentry_count;
		inode->attr = dentry_set[0].file_attr;
		inode->first_clus = le32_to_cpu(dentry_set[1].stream_start_clu);
		*dir_is_contiguous = inode->is_contiguous;
		inode->is_contiguous =
			(dentry_set[1].stream_flags & EXFAT_SF_CONTIGUOUS);
		inode->size = le64_to_cpu(dentry_set[1].stream_size);
	} while (1);

free_inode:
	exfat_free_inode(inode);

	return ret;
}

/*
 * Get the position of the next directory entry
 *
 * Input
 *   is_contiguous: Whether the cluster chain where the directory entries are
 *                  located is continuous.
 *   dentry_off:    The position of the current directory entry.
 * Output
 *   dentry_off:    The position of the next directory entry.
 * Return
 *   0 on success
 *   -error code on failure
 */
static int exfat_get_next_dentry_offset(struct exfat *exfat, bool is_contiguous,
			off_t *dentry_off)
{
	int ret;
	clus_t clu;
	unsigned int offset;

	if (is_contiguous) {
		*dentry_off += DENTRY_SIZE;
		return 0;
	}

	ret = exfat_o2c(exfat, *dentry_off, &clu, &offset);
	if (ret)
		return ret;

	if (offset + DENTRY_SIZE == exfat->clus_size) {
		ret = exfat_get_next_clus(exfat, clu, &clu);
		if (ret) {
			exfat_err("failed to get next dentry offset 0x%lx\n",
					*dentry_off);
			return ret;
		}

		if (!exfat_heap_clus(exfat, clu)) {
			exfat_err("cluster %u is not in cluster heap\n", clu);
			return -ERANGE;
		}

		*dentry_off = exfat_c2o(exfat, clu);
	} else
		*dentry_off += DENTRY_SIZE;

	return 0;
}

/*
 * Print the cluster chain in the format.
 *
 * Cluster Chain:    clu_1:nr_clu_1
 *                   clu_2:nr_clu_2
 *                   ... ...
 *                   clu_n:nr_clu_n
 *
 */
static void exfat_show_cluster_chain(struct exfat *exfat,
		struct exfat_dentry *ed)
{
	int ret = 0;
	clus_t clu, next_clu;
	clus_t count = 0, num_clus;
	bool first = true;

	clu = le32_to_cpu(ed->stream_start_clu);
	num_clus = DIV_ROUND_UP(le64_to_cpu(ed->stream_size), exfat->clus_size);
	if (clu == 0)
		return;

	while (clu != EXFAT_EOF_CLUSTER) {
		if (!exfat_heap_clus(exfat, clu)) {
			if (first)
				dump_dentry_field("Cluster Chain",
						"%u(invalid)", clu);
			else
				dump_dentry_field_wrap("%u(invalid)", clu);

			return;
		}

		if (exfat_bitmap_get(exfat->alloc_bitmap, clu)) {
			dump_dentry_field_wrap("%u(double-link)", clu);
			return;
		}

		exfat_bitmap_set(exfat->alloc_bitmap, clu);

		count++;
		if (ed->stream_flags & EXFAT_SF_CONTIGUOUS) {
			if (count == num_clus)
				next_clu = EXFAT_EOF_CLUSTER;
			else
				next_clu = clu + 1;
		} else
			ret = exfat_get_next_clus(exfat, clu, &next_clu);

		if (clu + 1 != next_clu || ret) {
			if (first)
				dump_dentry_field("Cluster chain", "%u:%u",
					clu - count + 1, count);
			else
				dump_dentry_field_wrap("%u:%u", clu - count + 1, count);
			first = false;
			count = 0;
		}

		if (ret)
			return;

		clu = next_clu;
	}
}

struct show_dentry {
	__u8 type;
	const char *type_name;
	void (*show)(struct exfat_dentry *ed, struct exfat *exfat,
			uint32_t flags);
};

static void exfat_show_file_dentry(struct exfat_dentry *ed,
		struct exfat *exfat, uint32_t flags)
{
	uint16_t checksum = calc_dentry_set_checksum(ed, ed->file_num_ext + 1);

	dump_dentry_field("SecondaryCount", "%u", ed->file_num_ext);
	if (checksum == le16_to_cpu(ed->file_checksum))
		dump_dentry_field("SetChecksum", "0x%04X", checksum);
	else
		dump_dentry_field("SetChecksum", "0x%04X(expected: 0x%04X)",
				le16_to_cpu(ed->file_checksum), checksum);
	dump_dentry_field("FileAttributes", "0x%04X", le16_to_cpu(ed->file_attr));
	dump_dentry_field("CreateTimestamp", "0x%08X", le32_to_cpu(ed->file_create_time));
	dump_dentry_field("LastModifiedTimestamp", "0x%08X", le32_to_cpu(ed->file_modify_time));
	dump_dentry_field("LastAccessedTimestamp", "0x%08X", le32_to_cpu(ed->file_access_time));
	dump_dentry_field("Create10msIncrement", "%u", ed->file_create_time_ms);
	dump_dentry_field("LastModified10msIncrement", "%u", ed->file_modify_time_ms);
	dump_dentry_field("CreateUtcOffset", "%u", ed->file_create_tz);
	dump_dentry_field("LastModifiedUtcOffset", "%u", ed->file_modify_tz);
	dump_dentry_field("LastAccessedUtcOffset", "%u", ed->file_access_tz);
}

static void exfat_show_stream_dentry(struct exfat_dentry *ed,
		struct exfat *exfat, uint32_t flags)
{
	dump_dentry_field("NameLength", "%u", ed->stream_name_len);
	dump_dentry_field("NameHash", "0x%04X", le16_to_cpu(ed->stream_name_hash));
	dump_dentry_field("ValidDataLength", "%" PRIu64, le64_to_cpu(ed->stream_valid_size));
	dump_dentry_field("FirstCluster", "%u", le32_to_cpu(ed->stream_start_clu));
	dump_dentry_field("DataLength", "%" PRIu64, le64_to_cpu(ed->stream_size));
	if (flags & DUMP_CLUSTER_CHAIN)
		exfat_show_cluster_chain(exfat, ed);
}

static void exfat_show_bytes(const char *name, unsigned char *bytes, int n)
{
	char buf[64];
	int i, len = 0;

	for (i = 0; i < n && len < sizeof(buf); i++)
		len += snprintf(buf + len, sizeof(buf) - len, "%02X", bytes[i]);

	exfat_info("%-33s  %s\n", name, buf);
}

#define dump_bytes_field(name, feild)	\
	exfat_show_bytes("   " name ":", (unsigned char *)feild, sizeof(feild))

static void exfat_show_name_dentry(struct exfat_dentry *ed,
		struct exfat *exfat, uint32_t flags)
{
	dump_bytes_field("FileName", ed->name_unicode);
}

static void exfat_show_bitmap_entry(struct exfat_dentry *ed,
		struct exfat *exfat, uint32_t flags)
{
	dump_dentry_field("BitmapFlags", "0x%02X", ed->dentry.bitmap.flags);
	dump_dentry_field("FirstCluster", "%u", le32_to_cpu(ed->bitmap_start_clu));
	dump_dentry_field("DataLength", "%" PRIu64, le64_to_cpu(ed->bitmap_size));
	if (flags & DUMP_CLUSTER_CHAIN)
		exfat_show_cluster_chain(exfat, ed);
}

static void exfat_show_upcase_entry(struct exfat_dentry *ed,
		struct exfat *exfat, uint32_t flags)
{
	dump_dentry_field("TableChecksum", "0x%08X", le32_to_cpu(ed->upcase_checksum));
	dump_dentry_field("FirstCluster", "%u", le32_to_cpu(ed->upcase_start_clu));
	dump_dentry_field("DataLength", "%" PRIu64, le64_to_cpu(ed->upcase_size));
	if (flags & DUMP_CLUSTER_CHAIN)
		exfat_show_cluster_chain(exfat, ed);
}

static void exfat_show_volume_entry(struct exfat_dentry *ed,
		struct exfat *exfat, uint32_t flags)
{
	dump_dentry_field("CharacterCount", "%u", ed->vol_char_cnt);
	dump_bytes_field("VolumeLabel", ed->vol_label);
}

static void exfat_show_guid_entry(struct exfat_dentry *ed,
		struct exfat *exfat, uint32_t flags)
{
	dump_dentry_field("SecondaryCount", "%u", ed->dentry.guid.num_ext);
	dump_dentry_field("SetChecksum", "0x%04X", le16_to_cpu(ed->dentry.guid.checksum));
	dump_dentry_field("GeneralPrimaryFlags", "0x%04X", le16_to_cpu(ed->dentry.guid.flags));
	dump_bytes_field("VolumeGuid", ed->dentry.guid.guid);
}

static void exfat_show_vendor_ext_dentry(struct exfat_dentry *ed,
		struct exfat *exfat, uint32_t flags)
{
	dump_bytes_field("VendorGuid", ed->dentry.vendor_ext.guid);
	dump_bytes_field("VendorDefined", ed->dentry.vendor_ext.vendor_defined);
}

static void exfat_show_vendor_alloc_dentry(struct exfat_dentry *ed,
		struct exfat *exfat, uint32_t flags)
{
	dump_bytes_field("VendorGuid", ed->dentry.vendor_alloc.guid);
	dump_bytes_field("VendorDefined", ed->dentry.vendor_alloc.vendor_defined);
	dump_dentry_field("FirstCluster", "%u", le32_to_cpu(ed->vendor_alloc_start_clu));
	dump_dentry_field("DataLength", "%" PRIu64, le64_to_cpu(ed->vendor_alloc_size));
	if (flags & DUMP_CLUSTER_CHAIN)
		exfat_show_cluster_chain(exfat, ed);
}

static struct show_dentry show_dentry_array[] = {
	{EXFAT_FILE, "File", exfat_show_file_dentry},
	{EXFAT_STREAM, "Stream Extension", exfat_show_stream_dentry},
	{EXFAT_NAME, "File Name", exfat_show_name_dentry},
	{EXFAT_VENDOR_EXT, "Vendor Extension", exfat_show_vendor_ext_dentry},
	{EXFAT_VENDOR_ALLOC, "Vendor Allocation", exfat_show_vendor_alloc_dentry},
	{EXFAT_BITMAP, "Allocation Bitmap", exfat_show_bitmap_entry},
	{EXFAT_UPCASE, "Up-case Table", exfat_show_upcase_entry},
	{EXFAT_VOLUME, "Volume Label", exfat_show_volume_entry},
	{EXFAT_GUID, "Volume GUID", exfat_show_guid_entry},
	{EXFAT_PADDING, "TexFAT Padding", NULL},
	{EXFAT_LAST, "Unused", NULL}
};

/*
 * Print a directory entry
 *
 * Input
 *   ed: The directory entry will be printed.
 *   index: The entry index in directory entry set.
 *   dentry_off: The position of the directory entry.
 *   flags: If DUMP_CLUSTER_CHAIN is set, the cluster chain will be printed.
 */
static void exfat_show_dentry(struct exfat *exfat, struct exfat_dentry *ed,
		unsigned int index, off_t dentry_off, uint32_t flags)
{
	int i;
	struct show_dentry *sd = NULL;

	for (i = 0; i < sizeof(show_dentry_array) / sizeof(*sd); i++) {
		if (show_dentry_array[i].type == ed->type) {
			sd = show_dentry_array + i;
			break;
		}
	}

	if (sd)
		exfat_info("%d. %s Directory Entry\n", index,
				sd->type_name);
	else if (IS_EXFAT_DELETED(ed->type))
		exfat_info("%d. Deleted Directory Entry\n", index);
	else
		exfat_info("%d. Unknown Directory Entry\n", index);

	dump_dentry_field("Position", "0x%llX", (unsigned long long)dentry_off);
	dump_dentry_field("Entrytype", "0x%02X", ed->type);

	if (IS_EXFAT_SEC(ed->type))
		dump_dentry_field("GeneralSecondaryFlags", "0x%02X", ed->stream_flags);

	if (sd && sd->show)
		sd->show(ed, exfat, flags);
}

/*
 * Print the directory entry set of the file/directory
 *
 * Input:
 *   inode: it contains a copy of the directory entry set that will be printed.
 *   dir_is_contiguous: Whether the clusters of the parent directory are
 *                      contiguous.
 *   flags: If DUMP_CLUSTER_CHAIN is set, the cluster chain will be printed.
 */
static void exfat_show_dentry_set(struct exfat *exfat,
		struct exfat_inode *inode, bool dir_is_contiguous,
		uint32_t flags)
{
	int i;
	struct exfat_dentry *ed;
	off_t dentry_off;

	ed = inode->dentry_set;
	dentry_off = inode->dev_offset;

	for (i = 0; i < inode->dentry_count; i++, ed++) {
		exfat_show_dentry(exfat, ed, i, dentry_off, flags);

		if (i < inode->dentry_count - 1)
			exfat_get_next_dentry_offset(exfat,
					dir_is_contiguous, &dentry_off);
	}
}

/*
 * Create an inode for a directory entry set
 *
 * Input:
 *   de_iter: the directory entry set in the scan buffer
 * Output:
 *   new: the newly created inode.
 * Return:
 *   0 on success
 *   -error code on failure
 */
static int exfat_create_inode(struct exfat *exfat,
		struct exfat_de_iter *de_iter, struct exfat_inode **new)
{
	int ret, i;
	struct exfat_inode *inode;
	struct exfat_dentry *dentry, *de_stream;

	exfat_de_iter_get(de_iter, 0, &dentry);
	ret = exfat_de_iter_get(de_iter, 1, &de_stream);
	if (ret)
		return ret;

	inode = exfat_alloc_inode(le16_to_cpu(dentry->file_attr));
	if (!inode)
		return -ENOMEM;

	inode->dev_offset = exfat_de_iter_device_offset(de_iter);
	inode->dentry_count = DIV_ROUND_UP(de_stream->stream_name_len,
			ENTRY_NAME_MAX) + 2;
	inode->dentry_count = MAX(dentry->file_num_ext + 1,
			inode->dentry_count);
	inode->dentry_count = MAX(inode->dentry_count, 3);
	inode->attr = le16_to_cpu(dentry->file_attr);

	inode->dentry_set = calloc(inode->dentry_count, sizeof(*dentry));
	if (!inode->dentry_set) {
		ret = -ENOMEM;
		goto free_inode;
	}

	inode->dentry_set[0] = *dentry;
	inode->dentry_set[1] = *de_stream;

	for (i = 2; i < inode->dentry_count; i++) {
		ret = exfat_de_iter_get(de_iter, i, &dentry);
		if (ret)
			goto free_inode;

		if (!IS_EXFAT_SEC(dentry->type)) {
			if (i == 2) {
				ret = -EINVAL;
				goto free_inode;
			}

			inode->dentry_count = i;
			break;
		}

		inode->dentry_set[i] = *dentry;
		if (dentry->type == EXFAT_NAME)
			memcpy(inode->name + (i - 2) * ENTRY_NAME_MAX,
					dentry->name_unicode,
					sizeof(dentry->name_unicode));
	}

	inode->first_clus = le32_to_cpu(de_stream->stream_start_clu);
	inode->is_contiguous = de_stream->stream_flags & EXFAT_SF_CONTIGUOUS;
	inode->size = le64_to_cpu(de_stream->stream_size);

	*new = inode;

	return 0;

free_inode:
	exfat_free_inode(inode);

	return ret;
}

/*
 * Scan and print the directory sets in a directory
 *
 * Input:
 *   dir: the inode of the directory.
 *   path: the path of the directory.
 *   flags: If DUMP_SCAN_DIR_RECURSIVE is set, scan and print directory entry
 *         sets recursively.
 */
static int exfat_scan_dentry_set(struct exfat *exfat, struct exfat_inode *dir,
		const char *path, uint32_t flags)
{
	int ret, dentry_count, len;
	struct buffer_desc *scan_bdesc;
	struct exfat_de_iter de_iter;
	struct exfat_dentry *dentry;
	struct exfat_inode *inode;
	char new_path[PATH_MAX];

	scan_bdesc = exfat_alloc_buffer(exfat);
	if (!scan_bdesc)
		return -ENOMEM;

	ret = exfat_de_iter_init(&de_iter, exfat, dir, scan_bdesc);
	if (ret == EOF) {
		ret = 0;
		goto free_buffer;
	} else if (ret)
		goto free_buffer;

	while (1) {
		ret = exfat_de_iter_get(&de_iter, 0, &dentry);
		if (ret == EOF) {
			ret = 0;
			goto free_buffer;
		} else if (ret)
			goto free_buffer;

		dentry_count = 1;
		if (dentry->type == EXFAT_FILE) {
			ret = exfat_create_inode(exfat, &de_iter, &inode);
			if (ret == EOF) {
				ret = 0;
				goto free_buffer;
			} else if (ret)
				goto free_buffer;

			exfat_info("-----------------------------------------------------\n");

			len = snprintf(new_path, sizeof(new_path), "%s/", path);
			if (dir->first_clus == exfat->root->first_clus)
				len = 1;

			ret = exfat_utf16_dec(inode->name, NAME_BUFFER_SIZE,
					new_path + len, sizeof(new_path) - len);
			if (ret < 0)
				snprintf(new_path + len, sizeof(new_path) - len,
						"<invalid>");

			exfat_info("Path: %s\n", new_path);
			exfat_show_dentry_set(exfat, inode, dir->is_contiguous, flags);
			if ((inode->attr & ATTR_SUBDIR) &&
			    (flags & DUMP_SCAN_DIR_RECURSIVE)) {
				ret = exfat_scan_dentry_set(exfat, inode,
						new_path, flags);
				if (ret) {
					exfat_free_inode(inode);
					goto free_buffer;
				}
			}
			dentry_count = inode->dentry_count;
			exfat_free_inode(inode);
		} else if (dentry->type & EXFAT_INVAL) {
			exfat_info("-----------------------------------------------------\n");
			exfat_info("Directory: %s\n", path);
			exfat_show_dentry(exfat, dentry, 0,
					exfat_de_iter_device_offset(&de_iter), flags);
		}

		ret = exfat_de_iter_advance(&de_iter, dentry_count);
		if (ret)
			goto free_buffer;
	}

free_buffer:
	exfat_free_buffer(exfat, scan_bdesc);

	return ret;
}

static int exfat_show_dentry_set_by_path(struct exfat *exfat, const char *path,
		uint32_t flags)
{
	int ret;
	bool dir_is_contiguous;
	struct exfat_inode *inode;

	ret = exfat_create_inode_by_path(exfat, path, &inode,
			&dir_is_contiguous);
	if (ret)
		return ret;

	if (flags & DUMP_SCAN_DIR) {
		if ((inode->attr & ATTR_SUBDIR) == 0) {
			ret = -EINVAL;
			goto out;
		}

		ret = exfat_scan_dentry_set(exfat, inode, path, flags);
		goto out;
	}

	exfat_info("\n");

	/* The root has no directory entry, only show the cluster chain */
	if (exfat->root->first_clus == inode->first_clus &&
	    (flags & DUMP_CLUSTER_CHAIN)) {
		struct exfat_dentry ed;

		memset(&ed, 0, sizeof(ed));
		ed.stream_start_clu = cpu_to_le32(inode->first_clus);
		memset(exfat->alloc_bitmap, 0, EXFAT_BITMAP_SIZE(exfat->clus_count));

		exfat_show_cluster_chain(exfat, &ed);
	} else
		exfat_show_dentry_set(exfat, inode, dir_is_contiguous, flags);

out:
	exfat_free_inode(inode);
	return ret;
}

int main(int argc, char *argv[])
{
	int c;
	int ret = EXIT_FAILURE;
	struct exfat_blk_dev bd;
	struct exfat_user_input ui;
	bool version_only = false;
	struct exfat *exfat;
	const char *path = NULL;
	uint32_t flags = 0;

	init_user_input(&ui);
	ui.writeable = false;

	if (!setlocale(LC_CTYPE, ""))
		exfat_err("failed to init locale/codeset\n");

	opterr = 0;
	while ((c = getopt_long(argc, argv, "iVhd:s:rc", opts, NULL)) != EOF)
		switch (c) {
		case 'V':
			version_only = true;
			break;
		case 'd':
			path = optarg;
			break;
		case 's':
			path = optarg;
			flags |= DUMP_SCAN_DIR;
			break;
		case 'r':
			flags |= DUMP_SCAN_DIR_RECURSIVE;
			break;
		case 'c':
			flags |= DUMP_CLUSTER_CHAIN;
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

	ui.dev_name = argv[argc - 1];

	ret = exfat_get_blk_dev_info(&ui, &bd);
	if (ret < 0)
		goto out;

	exfat = exfat_alloc_exfat(&bd, NULL, NULL);
	if (!exfat) {
		ret = -ENOMEM;
		goto close_dev_fd;
	}

	if (path)
		ret = exfat_show_dentry_set_by_path(exfat, path, flags);
	else
		ret = exfat_show_fs_info(exfat);

	exfat_free_exfat(exfat);

close_dev_fd:
	close(bd.dev_fd);

out:
	return ret;
}
