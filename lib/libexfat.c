// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2019 Namjae Jeon <linkinjeon@kernel.org>
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <wchar.h>
#include <limits.h>

#include "exfat_ondisk.h"
#include "libexfat.h"
#include "version.h"

#define BITS_PER_LONG		(sizeof(long) * CHAR_BIT)

#ifdef WORDS_BIGENDIAN
#define BITOP_LE_SWIZZLE	((BITS_PER_LONG - 1) & ~0x7)
#else
#define BITOP_LE_SWIZZLE        0
#endif

#define BIT_MASK(nr)            (1UL << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr)            ((nr) / BITS_PER_LONG)

unsigned int print_level  = EXFAT_INFO;

static inline void set_bit(int nr, void *addr)
{
	unsigned long mask = BIT_MASK(nr);
	unsigned long *p = ((unsigned long *)addr) + BIT_WORD(nr);

	*p  |= mask;
}

static inline void clear_bit(int nr, void *addr)
{
	unsigned long mask = BIT_MASK(nr);
	unsigned long *p = ((unsigned long *)addr) + BIT_WORD(nr);

	*p &= ~mask;
}

static inline void set_bit_le(int nr, void *addr)
{
	set_bit(nr ^ BITOP_LE_SWIZZLE, addr);
}

static inline void clear_bit_le(int nr, void *addr)
{
	clear_bit(nr ^ BITOP_LE_SWIZZLE, addr);
}

void exfat_set_bit(struct exfat_blk_dev *bd, char *bitmap,
		unsigned int clu)
{
	int b;

	b = clu & ((bd->sector_size << 3) - 1);

	set_bit_le(b, bitmap);
}

void exfat_clear_bit(struct exfat_blk_dev *bd, char *bitmap,
		unsigned int clu)
{
	int b;

	b = clu & ((bd->sector_size << 3) - 1);

	clear_bit_le(b, bitmap);
}

wchar_t exfat_bad_char(wchar_t w)
{
	return (w < 0x0020)
		|| (w == '*') || (w == '?') || (w == '<') || (w == '>')
		|| (w == '|') || (w == '"') || (w == ':') || (w == '/')
		|| (w == '\\');
}

void boot_calc_checksum(unsigned char *sector, unsigned short size,
		bool is_boot_sec, __le32 *checksum)
{
	unsigned int index;

	if (is_boot_sec) {
		for (index = 0; index < size; index++) {
			if ((index == 106) || (index == 107) || (index == 112))
				continue;
			*checksum = ((*checksum & 1) ? 0x80000000 : 0) +
				(*checksum >> 1) + sector[index];
		}
	} else {
		for (index = 0; index < size; index++) {
			*checksum = ((*checksum & 1) ? 0x80000000 : 0) +
				(*checksum >> 1) + sector[index];
		}
	}
}

void show_version(void)
{
	printf("exfatprogs version : %s\n", EXFAT_PROGS_VERSION);
}

static inline unsigned int sector_size_bits(unsigned int size)
{
	unsigned int bits = 8;

	do {
		bits++;
		size >>= 1;
	} while (size > 256);

	return bits;
}

static void exfat_set_default_cluster_size(struct exfat_blk_dev *bd,
		struct exfat_user_input *ui)
{
	if (256 * MB >= bd->size)
		ui->cluster_size = 4 * KB;
	else if (32 * GB >= bd->size)
		ui->cluster_size = 32 * KB;
	else
		ui->cluster_size = 128 * KB;
}

void init_user_input(struct exfat_user_input *ui)
{
	memset(ui, 0, sizeof(struct exfat_user_input));
	ui->writeable = true;
	ui->quick = true;
}

int exfat_get_blk_dev_info(struct exfat_user_input *ui,
		struct exfat_blk_dev *bd)
{
	int fd, ret = -1;
	off_t blk_dev_size;
	struct stat st;
	unsigned long long blk_dev_offset = 0;

	fd = open(ui->dev_name, ui->writeable ? O_RDWR|O_EXCL : O_RDONLY);
	if (fd < 0) {
		exfat_err("open failed : %s, %s\n", ui->dev_name,
			strerror(errno));
		return -1;
	}
	blk_dev_size = lseek(fd, 0, SEEK_END);
	if (blk_dev_size <= 0) {
		exfat_err("invalid block device size(%s)\n",
			ui->dev_name);
		ret = blk_dev_size;
		close(fd);
		goto out;
	}

	if (fstat(fd, &st) == 0 && S_ISBLK(st.st_mode)) {
		char pathname[sizeof("/sys/dev/block/4294967295:4294967295/start")];
		FILE *fp;

		snprintf(pathname, sizeof(pathname), "/sys/dev/block/%u:%u/start",
			major(st.st_rdev), minor(st.st_rdev));
		fp = fopen(pathname, "r");
		if (fp != NULL) {
			if (fscanf(fp, "%llu", &blk_dev_offset) == 1) {
				/*
				 * Linux kernel always reports partition offset
				 * in 512-byte units, regardless of sector size
				 */
				blk_dev_offset <<= 9;
			}
			fclose(fp);
		}
	}

	bd->dev_fd = fd;
	bd->offset = blk_dev_offset;
	bd->size = blk_dev_size;
	if (!ui->cluster_size)
		exfat_set_default_cluster_size(bd, ui);

	if (!ui->boundary_align)
		ui->boundary_align = DEFAULT_BOUNDARY_ALIGNMENT;

	if (ioctl(fd, BLKSSZGET, &bd->sector_size) < 0)
		bd->sector_size = DEFAULT_SECTOR_SIZE;
	bd->sector_size_bits = sector_size_bits(bd->sector_size);
	bd->num_sectors = blk_dev_size / DEFAULT_SECTOR_SIZE;
	bd->num_clusters = blk_dev_size / ui->cluster_size;

	exfat_debug("Block device name : %s\n", ui->dev_name);
	exfat_debug("Block device offset : %llu\n", bd->offset);
	exfat_debug("Block device size : %llu\n", bd->size);
	exfat_debug("Block sector size : %u\n", bd->sector_size);
	exfat_debug("Number of the sectors : %llu\n",
		bd->num_sectors);
	exfat_debug("Number of the clusters : %u\n",
		bd->num_clusters);

	ret = 0;
	bd->dev_fd = fd;
out:
	return ret;
}

ssize_t exfat_read(int fd, void *buf, size_t size, off_t offset)
{
	return pread(fd, buf, size, offset);
}

ssize_t exfat_write(int fd, void *buf, size_t size, off_t offset)
{
	return pwrite(fd, buf, size, offset);
}

size_t exfat_utf16_len(const __le16 *str, size_t max_size)
{
	size_t i = 0;

	while (le16_to_cpu(str[i]) && i < max_size)
		i++;
	return i;
}

ssize_t exfat_utf16_enc(const char *in_str, __u16 *out_str, size_t out_size)
{
	size_t mbs_len, out_len, i;
	wchar_t *wcs;

	mbs_len = mbstowcs(NULL, in_str, 0);
	if (mbs_len == (size_t)-1) {
		if (errno == EINVAL || errno == EILSEQ)
			exfat_err("invalid character sequence in current locale\n");
		return -errno;
	}

	wcs = calloc(mbs_len+1, sizeof(wchar_t));
	if (!wcs)
		return -ENOMEM;

	/* First convert multibyte char* string to wchar_t* string */
	if (mbstowcs(wcs, in_str, mbs_len+1) == (size_t)-1) {
		if (errno == EINVAL || errno == EILSEQ)
			exfat_err("invalid character sequence in current locale\n");
		free(wcs);
		return -errno;
	}

	/* Convert wchar_t* string (sequence of code points) to UTF-16 string */
	for (i = 0, out_len = 0; i < mbs_len; i++) {
		if (2*(out_len+1) > out_size ||
		    (wcs[i] >= 0x10000 && 2*(out_len+2) > out_size)) {
			exfat_err("input string is too long\n");
			free(wcs);
			return -E2BIG;
		}

		/* Encode code point above Plane0 as UTF-16 surrogate pair */
		if (wcs[i] >= 0x10000) {
			out_str[out_len++] =
			  cpu_to_le16(((wcs[i] - 0x10000) >> 10) + 0xD800);
			wcs[i] = ((wcs[i] - 0x10000) & 0x3FF) + 0xDC00;
		}

		out_str[out_len++] = cpu_to_le16(wcs[i]);
	}

	free(wcs);
	return 2*out_len;
}

ssize_t exfat_utf16_dec(const __u16 *in_str, size_t in_len,
			char *out_str, size_t out_size)
{
	size_t wcs_len, out_len, c_len, i;
	char c_str[MB_LEN_MAX];
	wchar_t *wcs;
	mbstate_t ps;
	wchar_t w;

	wcs = calloc(in_len/2+1, sizeof(wchar_t));
	if (!wcs)
		return -ENOMEM;

	/* First convert UTF-16 string to wchar_t* string */
	for (i = 0, wcs_len = 0; i < in_len/2; i++, wcs_len++) {
		wcs[wcs_len] = le16_to_cpu(in_str[i]);
		/*
		 * If wchar_t can store code point above Plane0
		 * then unpack UTF-16 surrogate pair to code point
		 */
#if WCHAR_MAX >= 0x10FFFF
		if (wcs[wcs_len] >= 0xD800 && wcs[wcs_len] <= 0xDBFF &&
		    i+1 < in_len/2) {
			w = le16_to_cpu(in_str[i+1]);
			if (w >= 0xDC00 && w <= 0xDFFF) {
				wcs[wcs_len] = 0x10000 +
					       ((wcs[wcs_len] - 0xD800) << 10) +
					       (w - 0xDC00);
				i++;
			}
		}
#endif
	}

	memset(&ps, 0, sizeof(ps));

	/* And then convert wchar_t* string to multibyte char* string */
	for (i = 0, out_len = 0, c_len = 0; i <= wcs_len; i++) {
		c_len = wcrtomb(c_str, wcs[i], &ps);
		/*
		 * If character is non-representable in current locale then
		 * try to store it as Unicode replacement code point U+FFFD
		 */
		if (c_len == (size_t)-1 && errno == EILSEQ)
			c_len = wcrtomb(c_str, 0xFFFD, &ps);
		/* If U+FFFD is also non-representable, try question mark */
		if (c_len == (size_t)-1 && errno == EILSEQ)
			c_len = wcrtomb(c_str, L'?', &ps);
		/* If also (7bit) question mark fails then we cannot do more */
		if (c_len == (size_t)-1) {
			exfat_err("invalid UTF-16 sequence\n");
			free(wcs);
			return -errno;
		}
		if (out_len+c_len > out_size) {
			exfat_err("input string is too long\n");
			free(wcs);
			return -E2BIG;
		}
		memcpy(out_str+out_len, c_str, c_len);
		out_len += c_len;
	}

	free(wcs);

	/* Last iteration of above loop should have produced null byte */
	if (c_len == 0 || out_str[out_len-1] != 0) {
		exfat_err("invalid UTF-16 sequence\n");
		return -errno;
	}

	return out_len-1;
}

off_t exfat_get_root_entry_offset(struct exfat_blk_dev *bd)
{
	struct pbr *bs;
	int nbytes;
	unsigned int cluster_size;
	off_t root_clu_off;

	bs = (struct pbr *)malloc(sizeof(struct pbr));
	if (!bs) {
		exfat_err("failed to allocate memory\n");
		return -ENOMEM;
	}

	nbytes = exfat_read(bd->dev_fd, bs, sizeof(struct pbr), 0);
	if (nbytes != sizeof(struct pbr)) {
		exfat_err("boot sector read failed: %d\n", errno);
		free(bs);
		return -1;
	}

	cluster_size = (1 << bs->bsx.sect_per_clus_bits) * bd->sector_size;
	root_clu_off = le32_to_cpu(bs->bsx.clu_offset) * bd->sector_size +
		le32_to_cpu(bs->bsx.root_cluster - EXFAT_RESERVED_CLUSTERS)
		* cluster_size;
	free(bs);

	return root_clu_off;
}

char *exfat_conv_volume_label(struct exfat_dentry *vol_entry)
{
	char *volume_label;
	__le16 disk_label[VOLUME_LABEL_MAX_LEN];

	volume_label = malloc(VOLUME_LABEL_BUFFER_SIZE);
	if (!volume_label)
		return NULL;

	memcpy(disk_label, vol_entry->vol_label, sizeof(disk_label));
	memset(volume_label, 0, VOLUME_LABEL_BUFFER_SIZE);
	if (exfat_utf16_dec(disk_label, vol_entry->vol_char_cnt*2,
		volume_label, VOLUME_LABEL_BUFFER_SIZE) < 0) {
		exfat_err("failed to decode volume label\n");
		free(volume_label);
		return NULL;
	}

	return volume_label;
}

int exfat_show_volume_label(struct exfat_blk_dev *bd, off_t root_clu_off)
{
	struct exfat_dentry *vol_entry;
	char *volume_label;
	int nbytes;

	vol_entry = malloc(sizeof(struct exfat_dentry));
	if (!vol_entry) {
		exfat_err("failed to allocate memory\n");
		return -ENOMEM;
	}

	nbytes = exfat_read(bd->dev_fd, vol_entry,
		sizeof(struct exfat_dentry), root_clu_off);
	if (nbytes != sizeof(struct exfat_dentry)) {
		exfat_err("volume entry read failed: %d\n", errno);
		free(vol_entry);
		return -1;
	}

	volume_label = exfat_conv_volume_label(vol_entry);
	if (!volume_label) {
		free(vol_entry);
		return -EINVAL;
	}

	exfat_info("label: %s\n", volume_label);

	free(volume_label);
	free(vol_entry);
	return 0;
}

int exfat_set_volume_label(struct exfat_blk_dev *bd,
		char *label_input, off_t root_clu_off)
{
	struct exfat_dentry vol;
	int nbytes;
	__u16 volume_label[VOLUME_LABEL_MAX_LEN];
	int volume_label_len;

	volume_label_len = exfat_utf16_enc(label_input,
			volume_label, sizeof(volume_label));
	if (volume_label_len < 0) {
		exfat_err("failed to encode volume label\n");
		return -1;
	}

	vol.type = EXFAT_VOLUME;
	memset(vol.vol_label, 0, sizeof(vol.vol_label));
	memcpy(vol.vol_label, volume_label, volume_label_len);
	vol.vol_char_cnt = volume_label_len/2;

	nbytes = exfat_write(bd->dev_fd, &vol, sizeof(struct exfat_dentry),
			root_clu_off);
	if (nbytes != sizeof(struct exfat_dentry)) {
		exfat_err("volume entry write failed: %d\n", errno);
		return -1;
	}
	fsync(bd->dev_fd);

	exfat_info("new label: %s\n", label_input);
	return 0;
}

int exfat_read_sector(struct exfat_blk_dev *bd, void *buf, unsigned int sec_off)
{
	int ret;
	unsigned long long offset = sec_off * bd->sector_size;

	lseek(bd->dev_fd, offset, SEEK_SET);
	ret = read(bd->dev_fd, buf, bd->sector_size);
	if (ret < 0) {
		exfat_err("read failed, sec_off : %u\n", sec_off);
		return -1;
	}
	return 0;
}

int exfat_write_sector(struct exfat_blk_dev *bd, void *buf,
		unsigned int sec_off)
{
	int bytes;
	unsigned long long offset = sec_off * bd->sector_size;

	lseek(bd->dev_fd, offset, SEEK_SET);
	bytes = write(bd->dev_fd, buf, bd->sector_size);
	if (bytes != (int)bd->sector_size) {
		exfat_err("write failed, sec_off : %u, bytes : %d\n", sec_off,
			bytes);
		return -1;
	}
	return 0;
}

int exfat_write_checksum_sector(struct exfat_blk_dev *bd,
		unsigned int checksum, bool is_backup)
{
	__le32 *checksum_buf;
	int ret = 0;
	unsigned int i;
	unsigned int sec_idx = CHECKSUM_SEC_IDX;

	checksum_buf = malloc(bd->sector_size);
	if (!checksum_buf)
		return -1;

	if (is_backup)
		sec_idx += BACKUP_BOOT_SEC_IDX;

	for (i = 0; i < bd->sector_size / sizeof(int); i++)
		checksum_buf[i] = cpu_to_le32(checksum);

	ret = exfat_write_sector(bd, checksum_buf, sec_idx);
	if (ret) {
		exfat_err("checksum sector write failed\n");
		goto free;
	}

free:
	free(checksum_buf);
	return ret;
}

int exfat_show_volume_serial(struct exfat_blk_dev *bd,
		struct exfat_user_input *ui)
{
	struct pbr *ppbr;
	int ret;

	ppbr = malloc(bd->sector_size);
	if (!ppbr) {
		exfat_err("Cannot allocate pbr: out of memory\n");
		return -1;
	}

	/* read main boot sector */
	ret = exfat_read_sector(bd, (char *)ppbr, BOOT_SEC_IDX);
	if (ret < 0) {
		exfat_err("main boot sector read failed\n");
		ret = -1;
		goto free_ppbr;
	}

	exfat_info("volume serial : 0x%x\n", ppbr->bsx.vol_serial);

free_ppbr:
	free(ppbr);
	return ret;
}

static int exfat_update_boot_checksum(struct exfat_blk_dev *bd, bool is_backup)
{
	unsigned int checksum = 0;
	int ret, sec_idx, backup_sec_idx = 0;
	unsigned char *buf;

	buf = malloc(bd->sector_size);
	if (!buf) {
		exfat_err("Cannot allocate pbr: out of memory\n");
		return -1;
	}

	if (is_backup)
		backup_sec_idx = BACKUP_BOOT_SEC_IDX;

	for (sec_idx = BOOT_SEC_IDX; sec_idx < CHECKSUM_SEC_IDX; sec_idx++) {
		bool is_boot_sec = false;

		ret = exfat_read_sector(bd, buf, sec_idx + backup_sec_idx);
		if (ret < 0) {
			exfat_err("sector(%d) read failed\n", sec_idx);
			ret = -1;
			goto free_buf;
		}

		if (sec_idx == BOOT_SEC_IDX)
			is_boot_sec = true;

		boot_calc_checksum(buf, bd->sector_size, is_boot_sec,
			&checksum);
	}

	ret = exfat_write_checksum_sector(bd, checksum, is_backup);

free_buf:
	free(buf);

	return ret;
}

int exfat_set_volume_serial(struct exfat_blk_dev *bd,
		struct exfat_user_input *ui)
{
	int ret;
	struct pbr *ppbr;

	ppbr = malloc(bd->sector_size);
	if (!ppbr) {
		exfat_err("Cannot allocate pbr: out of memory\n");
		return -1;
	}

	/* read main boot sector */
	ret = exfat_read_sector(bd, (char *)ppbr, BOOT_SEC_IDX);
	if (ret < 0) {
		exfat_err("main boot sector read failed\n");
		ret = -1;
		goto free_ppbr;
	}

	ppbr->bsx.vol_serial = ui->volume_serial;

	/* update main boot sector */
	ret = exfat_write_sector(bd, (char *)ppbr, BOOT_SEC_IDX);
	if (ret < 0) {
		exfat_err("main boot sector write failed\n");
		ret = -1;
		goto free_ppbr;
	}

	/* update backup boot sector */
	ret = exfat_write_sector(bd, (char *)ppbr, BACKUP_BOOT_SEC_IDX);
	if (ret < 0) {
		exfat_err("backup boot sector write failed\n");
		ret = -1;
		goto free_ppbr;
	}

	ret = exfat_update_boot_checksum(bd, 0);
	if (ret < 0) {
		exfat_err("main checksum update failed\n");
		goto free_ppbr;
	}

	ret = exfat_update_boot_checksum(bd, 1);
	if (ret < 0)
		exfat_err("backup checksum update failed\n");
free_ppbr:
	free(ppbr);

	exfat_info("New volume serial : 0x%x\n", ui->volume_serial);

	return ret;
}
