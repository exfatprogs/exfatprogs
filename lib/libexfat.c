// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2019 Namjae Jeon <linkinjeon@kernel.org>
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <fcntl.h>
#ifdef _POSIX_MAPPED_FILES
#include <sys/mman.h>
#endif
#include <sys/utsname.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <wchar.h>
#include <limits.h>
#include <assert.h>

#include "exfat_ondisk.h"
#include "libexfat.h"
#include "version.h"
#include "exfat_fs.h"
#include "exfat_dir.h"

unsigned int print_level  = EXFAT_INFO;
static int fd_devzero = -1;

const char *exfat_part_table_type_str[] = {
	"none",
	"MBR",
	"GPT",
};

/* Copied from dosfstools */
const char *dummy_bootcode =
	"\x0e"			/* push cs */
	"\x1f"			/* pop ds */
	"\xbe\x00\x00"		/* mov si, offset message_txt (to be filled later) */
/* write_msg: */
	"\xac"			/* lodsb */
	"\x22\xc0"		/* and al, al */
	"\x74\x0b"		/* jz key_press */
	"\x56"			/* push si */
	"\xb4\x0e"		/* mov ah, 0eh */
	"\xbb\x07\x00"		/* mov bx, 0007h */
	"\xcd\x10"		/* int 10h */
	"\x5e"			/* pop si */
	"\xeb\xf0"		/* jmp write_msg */
/* key_press: */
	"\x32\xe4"		/* xor ah, ah */
	"\xcd\x16"		/* int 16h */
	"\xcd\x19"		/* int 19h */
	"\xeb\xfe";		/* foo: jmp foo */
/* message_txt: */
#define DEFAULT_DUMMY_BOOTCODE_MSG \
	"This exFAT/GPT volume is not bootable.\r\n"\
	"Please insert a bootable disk and press any key to try again.\r\n"
const char *dummy_bootcode_msg = DEFAULT_DUMMY_BOOTCODE_MSG;
/* static_assert(sizeof(DEFAULT_DUMMY_BOOTCODE_MSG) - 1 <= EXFAT_MAX_BOOTCODE_MSGLEN); */

void exfat_bitmap_set_range(struct exfat *exfat, unsigned char *bitmap,
			    clus_t start_clus, clus_t count)
{
	clus_t clus;

	if (!exfat_heap_clus(exfat, start_clus) ||
	    !exfat_heap_clus(exfat, start_clus + count - 1))
		return;

	clus = start_clus;
	while (clus < start_clus + count) {
		exfat_bitmap_set(bitmap, clus);
		clus++;
	}
}

static int exfat_bitmap_find_bit(struct exfat *exfat, unsigned char *bmap,
				 clus_t start_clu, clus_t *next,
				 int bit)
{
	clus_t last_clu;

	last_clu = le32_to_cpu(exfat->bs->bsx.clu_count) +
		EXFAT_FIRST_CLUSTER;
	while (start_clu < last_clu) {
		if (!!exfat_bitmap_get(bmap, start_clu) == bit) {
			*next = start_clu;
			return 0;
		}
		start_clu++;
	}
	return 1;
}

int exfat_bitmap_find_zero(struct exfat *exfat, unsigned char *bmap,
			   clus_t start_clu, clus_t *next)
{
	return exfat_bitmap_find_bit(exfat, bmap,
				     start_clu, next, 0);
}

int exfat_bitmap_find_one(struct exfat *exfat, unsigned char *bmap,
			  clus_t start_clu, clus_t *next)
{
	return exfat_bitmap_find_bit(exfat, bmap,
				     start_clu, next, 1);
}

wchar_t exfat_bad_char(wchar_t w)
{
	return (w < 0x0020)
		|| (w == '*') || (w == '?') || (w == '<') || (w == '>')
		|| (w == '|') || (w == '"') || (w == ':') || (w == '/')
		|| (w == '\\');
}

void boot_calc_checksum(const unsigned char *sector, size_t size,
		bool is_boot_sec, __le32 *checksum)
{
	size_t index;

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
	printf("exfatprogs version : %s (%s)\n",
			EXFAT_PROGS_VERSION, EXFAT_PROGS_RELEASE_DATE);
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
	if (7ULL * MB > bd->size)
		ui->cluster_size = 512;
	else if (256ULL * MB >= bd->size)
		ui->cluster_size = 4 * KB;
	else if (32ULL * GB >= bd->size)
		ui->cluster_size = 32 * KB;
	else
		ui->cluster_size = 128 * KB;
}

void exfat_init_user_input(struct exfat_user_input *ui)
{
	memset(ui, 0, sizeof(struct exfat_user_input));
	ui->writeable = true;
	ui->quick = true;
	ui->discard = true;
	ui->part_table = PART_TABLE_AUTO;
	ui->bootcode_msg = dummy_bootcode_msg;
}

void exfat_deinit_user_input(struct exfat_user_input *ui)
{
	/* nothing, yet */
}

void exfat_init_blk_dev_info(struct exfat_blk_dev *bd)
{
	memset(bd, 0, sizeof(*bd));
	bd->dev_fd = -1;
}

void exfat_deinit_blk_dev_info(struct exfat_blk_dev *bd)
{
	if (bd->dev_fd >= 0) {
		close(bd->dev_fd);
		bd->dev_fd = -1;
	}
}

static size_t count_dots(const char *s, const size_t n)
{
	size_t ret = 0;

	for (size_t i = 0; s[i] != 0 && i < n; i++)
		if (s[i] == '.')
			ret++;

	return ret;
}

static FILE *exfat_fopenat_ro(const int at, const char *path)
{
	int fd;
	FILE *ret;

	fd = openat(at, path, O_RDONLY);
	if (fd < 0)
		return NULL;

	ret = fdopen(fd, "r");
	if (ret == NULL) {
		close(fd);
		return NULL;
	}

	return ret;
}

static DIR *exfat_opendirat(const int at, const char *path)
{
	int fd;
	DIR *ret;

	fd = openat(at, path, O_RDONLY|O_DIRECTORY);
	if (fd < 0)
		return NULL;

	ret = fdopendir(fd);
	if (ret == NULL) {
		close(fd);
		return NULL;
	}

	return ret;
}

/* Read an unsigned long long integer value from the text file */
static bool exfat_read_textfile_ull(const int at, const char *path, unsigned long long *out)
{
	bool ret = false;
	FILE *fp;

	fp = exfat_fopenat_ro(at, path);
	if (fp == NULL)
		return false;

	ret = fscanf(fp, "%llu", out) == 1;
	fclose(fp);

	if (!ret)
		errno = EINVAL;

	return ret;
}

static bool exfat_is_file_symlink(const int at, const char *path)
{
	struct stat st;

	return fstatat(at, path, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
		S_ISLNK(st.st_mode) != 0;
}

/*
 * The function is used to traverse sysfs only, so errors are not handled.
 * If the function should be repurposed, definitely handle errors as VFS
 * can throw EIO at your face at any given moment!
 *
 * (POSIX not addressing this can be considered the defect in the spec
 * itself)
 */
static bool exfat_dir_has_child(const int at, const char *path)
{
	const struct dirent *ent;
	bool ret = false;
	DIR *dir;

	dir = exfat_opendirat(at, path);
	if (dir == NULL)
		return false;

	while ((ent = readdir(dir)) != NULL) {
		/* Skip dots */
		switch (count_dots(ent->d_name, 2)) {
		case 1:
			if (ent->d_name[0] == '.' && ent->d_name[1] == 0)
				/* "." */
				continue;
			/* "X." ".X" */
			break;
		case 2:
			if (ent->d_name[2] == 0)
				/* ".." */
				continue;
			/* "..XXX" */
			break;
		}

		ret = true;
		break;
	}

	closedir(dir);
	return ret;
}

static int exfat_cmp_kernel_ver(const unsigned short *req)
{
	struct utsname uts;
	unsigned short v[3] = { 0, };
	unsigned long long nreq = 0, nhost = 0;
	int ret;

	ret = uname(&uts);
	if (ret)
		return -1;

	switch (count_dots(uts.release, 65)) {
	case 0:
		ret = sscanf(uts.release, "%hu", v) != 1;
		break;
	case 1:
		ret = sscanf(uts.release, "%hu.%hu", v + 0, v + 1) != 2;
		break;
	default:
		ret = sscanf(uts.release, "%hu.%hu.%hu", v + 0, v + 1, v + 2) != 3;
	}
	if (ret) {
		errno = EINVAL;
		return -1;
	}

	exfat_debug("Kernel version: %hu.%hu.%hu\n", v[0], v[1], v[2]);

	nreq  |= (unsigned long long)req[0] << 32;
	nreq  |= (unsigned long long)req[1] << 16;
	nreq  |= (unsigned long long)req[2];
	nhost |= (unsigned long long)v[0]   << 32;
	nhost |= (unsigned long long)v[1]   << 16;
	nhost |= (unsigned long long)v[2];

	return nreq > nhost ? 1 : 0;
}

int exfat_get_blk_dev_info(struct exfat_user_input *ui,
		struct exfat_blk_dev *bd)
{
	static const unsigned short MIN_KERNEL_VER[3] = { 2, 6, 0 };
	char *pathname = NULL;
	int fd, ret = -1;
	off_t blk_dev_size;
	struct stat st;
	unsigned long long blk_dev_offset = 0;

	/* Assert kernel version >= 2.6 since a lot of interfaces in /sys/dev/block/ is relied upon.
	 * Other fs utils written in pre-2.6 had to make a lot of "juggling" to figure out the
	 * characteristics of the target block device. As exFAT is developed in 2010's, we can enjoy
	 * the luxury of the relatively recent sysfs interfaces at our disposal.
	 */
	switch (exfat_cmp_kernel_ver(MIN_KERNEL_VER)) {
	case -1:
		exfat_err("failed to get kernel version.\n");
		return -1;
	case 1:
		exfat_err("pre-2.6 kernel version not supported\n");
		return -1;
	}

	/*
	 * Neat trick: the extra one character is for causing open() to
	 * ENAMETOOLONG on (strlen(pathname) >= PATH_MAX)
	 */
	pathname = calloc(PATH_MAX + 1, 1);
	if (pathname == NULL) {
		exfat_err("Cannot allocate path string: out of memory\n");
		return -1;
	}

	fd = open(ui->dev_name, ui->writeable ? O_RDWR|O_EXCL : O_RDONLY);
	if (fd < 0) {
		exfat_err("open failed : %s, %s\n", ui->dev_name,
			strerror(errno));
		goto out;
	}

	blk_dev_size = lseek(fd, 0, SEEK_END);
	if (blk_dev_size <= 0) {
		exfat_err("invalid block device size(%s)\n",
			ui->dev_name);
		goto out;
	}

	if (fstat(fd, &st) == 0 && S_ISBLK(st.st_mode)) {
		unsigned long long v;
		int dirfd;

		bd->isblk = true;

		/*
		 * Open and hold the dir to avoid the TOCTOU condition where the
		 * device goes away mid-op. O_EXCL should prevent this, but
		 * the blockdev is not opened with that flag in read-only mode.
		 */
		snprintf(pathname, PATH_MAX + 1, "/sys/dev/block/%u:%u/",
			 major(st.st_rdev), minor(st.st_rdev));
		dirfd = open(pathname, O_RDONLY|O_DIRECTORY);
		if (dirfd >= 0) {
			if (exfat_read_textfile_ull(dirfd, "start", &blk_dev_offset))
				/*
				 * Linux kernel always reports partition offset
				 * in 512-byte units, regardless of sector size
				 */
				blk_dev_offset <<= 9;

			if (exfat_read_textfile_ull(dirfd, "partition", &v) && v != 0)
				bd->ispart = true;

			/*
			 * Problems with the prior art, get_block_linux_info() in dosfstools:
			 *
			 *  - What a mess!
			 *  - The project is licenced GPLv2 whilst exfatprogs is GPLv3'd
			 *  - It's based on the assumption that only dm(device mapper) and loop are
			 *    the only "virtual" types whilst a new type of "virtual" blockdev can
			 *    be added to the kernel. Not so future proof
			 *
			 * So, we really shouldn't just copy and paste that code into exfatprogs'
			 * codebase.
			 *
			 * Our approach is to assume a block dev without a 'device' symlink is
			 * "virtual"(example at the end of this comment block). This feature has
			 * existed since pre-2.6, so we can safely rely on it:
			 *
			 * https://git.kernel.org/pub/scm/linux/kernel/git/tglx/history.git/tree/drivers/base/class.c#n429
			 *
			 * Example scan command for your comprehension:
			 */
			// find /sys/dev/block/*/ -regextype egrep -regex '.*/(subsystem|device)' -printf '%p: %l\n' | sort
			bd->isdev = exfat_is_file_symlink(dirfd, "device");
			/*
			 * Grandfather clause from dosfstools: just to be absolutely sure, overturn
			 * the ruling if the device turns out to have any child
			 *
			 * This is not wrapped around an if statement so that the code path won't be
			 * optimised out therefore always be tested.
			 */
			bd->isdev &= !exfat_dir_has_child(dirfd, "slaves");

			/*
			 * Note that this attribute is what the device advertise itself as through
			 * the underlying transport interface. It has nothing to do with the fact
			 * that the device is actually removable or not. For example, it can be 0
			 * for mmcblk.
			 */
			if (exfat_read_textfile_ull(dirfd, "removable", &v) && v != 0)
				bd->isremov = true;

			exfat_read_textfile_ull(dirfd, "alignment_offset", &bd->alignment_offset);

			close(dirfd);
		}
	}

	bd->dev_fd = fd;
	bd->offset = blk_dev_offset;
	bd->size = blk_dev_size;
	if (!ui->cluster_size)
		exfat_set_default_cluster_size(bd, ui);

	if (!ui->boundary_align)
		ui->boundary_align = DEFAULT_BOUNDARY_ALIGNMENT;

	bd->dev_sector_size = 0;
	if (ioctl(fd, BLKSSZGET, &bd->dev_sector_size) < 0)
		bd->dev_sector_size = 0;

	if (ui->sector_size)
		bd->sector_size = ui->sector_size;
	else if (bd->dev_sector_size == 0)
		bd->sector_size = DEFAULT_SECTOR_SIZE;
	else
		bd->sector_size = bd->dev_sector_size;
	bd->sector_size_bits = sector_size_bits(bd->sector_size);
	bd->num_sectors = blk_dev_size / bd->sector_size;
	bd->num_clusters = blk_dev_size / ui->cluster_size;

	exfat_debug("Block device name      : %s\n",   ui->dev_name);
	exfat_debug("Block device offset    : %llu\n", bd->offset);
	exfat_debug("Block device size      : %llu\n", bd->size);
	exfat_debug("Block sector size      : %u\n",   bd->sector_size);
	exfat_debug("Number of the sectors  : %llu\n", bd->num_sectors);
	exfat_debug("Number of the clusters : %u\n",   bd->num_clusters);
	exfat_debug("Is block device        : %d\n",   bd->isblk);
	exfat_debug("Is partition           : %d\n",   bd->ispart);
	exfat_debug("Is real device         : %d\n",   bd->isdev);
	exfat_debug("Is removable           : %d\n",   bd->isremov);

	ret = 0;
	bd->dev_fd = fd;
	fd = -1;
out:
	free(pathname);
	if (fd >= 0)
		close(fd);

	return ret;
}

ssize_t exfat_read(int fd, void *buf, size_t size_in, off_t offset)
{
	int ret;
	off_t size = size_in;

	if (size > SSIZE_MAX)
		size = SSIZE_MAX;

	ret = exfat_read2(fd, buf, &size, &offset);
	if (ret < 0)
		return ret;
	return size_in - (size_t)size;
}

int exfat_read2(int fd, void *buf, off_t *size, off_t *offset)
{
	uint8_t *m = buf;
	size_t rem;
	ssize_t size_read;

	while (*size > 0) {
		rem = (size_t)MIN(*size, SSIZE_MAX);

		size_read = *offset >= 0 ?
			pread(fd, m, rem, *offset) :
			read(fd, m, rem);
		if (size_read == 0)
			return 1;
		if (size_read < 0)
			return -errno;
		assert((size_t)size_read <= rem);

		m += size_read;
		*size -= size_read;
		if (*offset >= 0)
			*offset += size_read;
	}

	return 0;
}

bool exfat_read_full(int fd, void *buf, size_t size, off_t offset)
{
	const ssize_t ret = exfat_read(fd, buf, size, offset);

	return ret >= 0 && (size_t)ret == size;
}

ssize_t exfat_write(int fd, const void *buf, size_t size_in, off_t offset)
{
	int ret;
	off_t size = size_in;

	if (size > SSIZE_MAX)
		size = SSIZE_MAX;

	ret = exfat_write2(fd, buf, &size, &offset);
	if (ret)
		return ret;
	return size_in - (size_t)size;
}

int exfat_write2(int fd, const void *buf, off_t *size, off_t *offset)
{
	const uint8_t *m = buf;
	size_t rem;
	ssize_t size_written;

	while (*size > 0) {
		rem = (size_t)MIN(*size, SSIZE_MAX);

		size_written = *offset >= 0 ?
			pwrite(fd, m, rem, *offset) :
			write(fd, m, rem);
		if (size_written == 0) {
			/* the dark corner of POSIX: we mirror glibc's defence mechanism here */
			exfat_debug("pwrite() returned zero. This VFS is doing something fishy!");
			return -EIO;
		}
		if (size_written < 0)
			return -errno;
		assert((size_t)size_written <= rem);

		m += size_written;
		*size -= size_written;
		if (*offset >= 0)
			*offset += size_written;
	}

	return 0;
}

bool exfat_write_full(int fd, const void *buf, size_t size, off_t offset)
{
	const ssize_t ret = exfat_write(fd, buf, size, offset);

	return ret >= 0 && (size_t)ret == size;
}

int exfat_write_zero(int fd, off_t size, off_t offset)
{
	return exfat_write_zero2(fd, size, offset, 0);
}

int exfat_write_zero2(int fd, off_t size, off_t offset, size_t bs)
{
	bool mapped = false;
	const void *zm;
	int ret = 0;
	size_t iter_size;
	ssize_t wsize;

	if (bs == 0)
		bs = 4 * KB;

	zm = exfat_map_zeromem(bs, &mapped);
	if (zm == NULL)
		return -errno;

	while (size > 0) {
		iter_size = (size_t)MIN(size, SSIZE_MAX);
		iter_size = MIN(iter_size, bs);

		wsize = offset >= 0 ?
			pwrite(fd, zm, iter_size, offset) :
			write(fd, zm, iter_size);

		if (wsize <= 0) {
			ret = -EIO;
			goto out;
		}

		size -= wsize;
		if (offset >= 0)
			offset += wsize;
	}

out:
	exfat_unmap_mm(zm, bs, &mapped);
	return ret;
}

int exfat_discard_blocks(int fd, uint64_t start, uint64_t len)
{
	uint64_t range[2] = { start, len };

	if (ioctl(fd, BLKDISCARD, &range) < 0)
		return errno;
	return 0;
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
	bool ret;
	unsigned int cluster_size, sector_size;
	off_t root_clu_off;

	bs = malloc(EXFAT_MAX_SECTOR_SIZE);
	if (!bs) {
		exfat_err("failed to allocate memory\n");
		return -ENOMEM;
	}

	ret = exfat_read_full(bd->dev_fd, bs, EXFAT_MAX_SECTOR_SIZE, 0);
	if (!ret) {
		exfat_err("boot sector read failed: %d\n", errno);
		free(bs);
		return -1;
	}

	if (memcmp(bs->bpb.oem_name, "EXFAT   ", 8) != 0) {
		exfat_err("Bad fs_name in boot sector, which does not describe a valid exfat filesystem\n");
		free(bs);
		return -1;
	}

	sector_size = 1 << bs->bsx.sect_size_bits;
	cluster_size = (1 << bs->bsx.sect_per_clus_bits) * sector_size;
	root_clu_off = le32_to_cpu(bs->bsx.clu_offset) * sector_size +
		(le32_to_cpu(bs->bsx.root_cluster) - EXFAT_RESERVED_CLUSTERS) *
		cluster_size;
	free(bs);

	return root_clu_off;
}

char *exfat_conv_volume_label(struct exfat_dentry *vol_entry)
{
	char *volume_label;
	__le16 disk_label[VOLUME_LABEL_MAX_LEN];

	volume_label = malloc(VOLUME_LABEL_BUFFER_SIZE);
	if (!volume_label) {
		exfat_err("Cannot allocate volume_label: out of memory\n");
		return NULL;
	}

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

int exfat_read_volume_label(struct exfat *exfat)
{
	struct exfat_dentry *dentry;
	int err;
	__le16 disk_label[VOLUME_LABEL_MAX_LEN];
	struct exfat_lookup_filter filter = {
		.in.type = EXFAT_VOLUME,
		.in.dentry_count = 0,
		.in.filter = NULL,
	};

	err = exfat_lookup_dentry_set(exfat, exfat->root, &filter);
	if (err)
		return err;

	dentry = filter.out.dentry_set;

	if (dentry->vol_char_cnt == 0)
		goto out;

	if (dentry->vol_char_cnt > VOLUME_LABEL_MAX_LEN) {
		exfat_err("too long label. %d\n", dentry->vol_char_cnt);
		err = -EINVAL;
		goto out;
	}

	memcpy(disk_label, dentry->vol_label, sizeof(disk_label));
	if (exfat_utf16_dec(disk_label, dentry->vol_char_cnt*2,
		exfat->volume_label, sizeof(exfat->volume_label)) < 0) {
		exfat_err("failed to decode volume label\n");
		err = -EINVAL;
		goto out;
	}

	exfat_info("label: %s\n", exfat->volume_label);
out:
	free(filter.out.dentry_set);
	return err;
}

int exfat_set_volume_label(struct exfat *exfat, char *label_input)
{
	struct exfat_dentry *pvol;
	struct exfat_dentry_loc loc;
	__u16 volume_label[VOLUME_LABEL_MAX_LEN];
	int volume_label_len, dcount, err;

	struct exfat_lookup_filter filter = {
		.in.type = EXFAT_VOLUME,
		.in.dentry_count = 1,
		.in.filter = NULL,
	};

	err = exfat_lookup_dentry_set(exfat, exfat->root, &filter);
	if (!err) {
		pvol = filter.out.dentry_set;
		dcount = filter.out.dentry_count;
		memset(pvol->vol_label, 0, sizeof(pvol->vol_label));
	} else {
		pvol = calloc(1, sizeof(struct exfat_dentry));
		if (!pvol)
			return -ENOMEM;

		dcount = 1;
		pvol->type = EXFAT_VOLUME;
	}

	volume_label_len = exfat_utf16_enc(label_input,
			volume_label, sizeof(volume_label));
	if (volume_label_len < 0) {
		exfat_err("failed to encode volume label\n");
		err = -1;
		goto out;
	}

	pvol->vol_char_cnt = volume_label_len/2;
	err = exfat_check_name(volume_label, pvol->vol_char_cnt);
	if (err != pvol->vol_char_cnt) {
		exfat_err("volume label contain invalid character(%c)\n",
				le16_to_cpu(label_input[err]));
		err = -1;
		goto out;
	}

	memcpy(pvol->vol_label, volume_label, volume_label_len);

	loc.parent = exfat->root;
	loc.file_offset = filter.out.file_offset;
	loc.dev_offset = filter.out.dev_offset;
	err = exfat_add_dentry_set(exfat, &loc, pvol, dcount, false);
	exfat_info("new label: %s\n", label_input);

out:
	free(pvol);

	return err;
}

static int set_guid(__u8 *guid, const char *input)
{
	int i, j, zero_len = 0;
	int len = strlen(input);

	if (len != EXFAT_GUID_LEN * 2 && len != EXFAT_GUID_LEN * 2 + 4) {
		exfat_err("invalid format for volume guid\n");
		return -EINVAL;
	}

	for (i = 0, j = 0; i < len; i++) {
		unsigned char ch = input[i];

		if (ch >= '0' && ch <= '9')
			ch -= '0';
		else if (ch >= 'a' && ch <= 'f')
			ch -= 'a' - 0xA;
		else if (ch >= 'A' && ch <= 'F')
			ch -= 'A' - 0xA;
		else if (ch == '-' && len == EXFAT_GUID_LEN * 2 + 4 &&
			 (i == 8 || i == 13 || i == 18 || i == 23))
			continue;
		else {
			exfat_err("invalid character '%c' for volume GUID\n", ch);
			return -EINVAL;
		}

		if (j & 1)
			guid[j >> 1] |= ch;
		else
			guid[j >> 1] = ch << 4;

		j++;

		if (ch == 0)
			zero_len++;
	}

	if (zero_len == EXFAT_GUID_LEN * 2) {
		exfat_err("%s is invalid for volume GUID\n", input);
		return -EINVAL;
	}

	return 0;
}

int exfat_read_volume_guid(struct exfat *exfat)
{
	int err;
	uint16_t checksum = 0;
	struct exfat_dentry *dentry;
	struct exfat_lookup_filter filter = {
		.in.type = EXFAT_GUID,
		.in.dentry_count = 1,
		.in.filter = NULL,
	};

	err = exfat_lookup_dentry_set(exfat, exfat->root, &filter);
	if (err)
		return err;

	dentry = filter.out.dentry_set;
	exfat_calc_dentry_checksum(dentry, &checksum, true);

	if (cpu_to_le16(checksum) == dentry->dentry.guid.checksum)
		exfat_print_guid(exfat_info, "GUID", dentry->dentry.guid.guid);
	else
		exfat_info("GUID is corrupted, please delete it or set a new one\n");

	free(dentry);

	return err;
}

int __exfat_set_volume_guid(struct exfat_dentry *dentry, const char *guid)
{
	int err;
	uint16_t checksum = 0;

	memset(dentry, 0, sizeof(*dentry));
	dentry->type = EXFAT_GUID;

	err = set_guid(dentry->dentry.guid.guid, guid);
	if (err)
		return err;

	exfat_calc_dentry_checksum(dentry, &checksum, true);
	dentry->dentry.guid.checksum = cpu_to_le16(checksum);

	return 0;
}

/*
 * Create/Update/Delete GUID dentry in root directory
 *
 * create/update GUID if @guid is not NULL.
 * delete GUID if @guid is NULL.
 */
int exfat_set_volume_guid(struct exfat *exfat, const char *guid)
{
	struct exfat_dentry *dentry;
	struct exfat_dentry_loc loc;
	int err;

	struct exfat_lookup_filter filter = {
		.in.type = EXFAT_GUID,
		.in.dentry_count = 1,
		.in.filter = NULL,
	};

	err = exfat_lookup_dentry_set(exfat, exfat->root, &filter);
	if (!err) {
		/* GUID entry is found */
		dentry = filter.out.dentry_set;
	} else {
		/* no GUID to delete */
		if (guid == NULL)
			return 0;

		dentry = calloc(1, sizeof(*dentry));
		if (!dentry)
			return -ENOMEM;
	}

	if (guid) {
		/* Set GUID */
		err = __exfat_set_volume_guid(dentry, guid);
		if (err)
			goto out;
	} else {
		/* Delete GUID */
		dentry->type &= ~EXFAT_INVAL;
	}

	loc.parent = exfat->root;
	loc.file_offset = filter.out.file_offset;
	loc.dev_offset = filter.out.dev_offset;
	err = exfat_add_dentry_set(exfat, &loc, dentry, 1, false);
	if (!err) {
		if (guid)
			exfat_print_guid(exfat_info, "new GUID", dentry->dentry.guid.guid);
		else
			exfat_info("GUID is deleted\n");
	}

out:
	free(dentry);

	return err;
}

static inline void exfat_report_verify_mismatch(const uint8_t *w, const uint8_t *r,
		const off_t off, const size_t len, const char *what)
{
	for (size_t i = 0; i < len; i++) {
		if (w[i] == r[i])
			continue;

		exfat_debug("%s verify mismatch (off=%llu, written=%02X, readback=%02X)\n",
			what, (unsigned long long)off + i, w[i], r[i]);
	}
}

int exfat_check_written_data(struct exfat_blk_dev *bd,
				const void *buf, size_t len,
				off_t off, const char *what)
{
	const size_t sector = bd->sector_size;
	void *verify = NULL;
	const void *zm = NULL;
	bool zmapped = false;
	int ret = 0;
	int flags;

	assert(sector > 0);

	if (len == 0)
		return 0;

	ret = fsync(bd->dev_fd);
	if (ret)
		return ret;

	flags = fcntl(bd->dev_fd, F_GETFL);

	off_t aligned_off = off & ~((off_t)sector - 1); /* this is evil */
	off_t head = off - aligned_off;
	off_t aligned_len = ((head + len + sector - 1) / sector) * sector;

	assert(head >= 0 && head < (off_t)sector);
	assert(aligned_len > 0 && aligned_len >= (off_t)len);

	if (posix_memalign(&verify, sector, (size_t)aligned_len))
		return -errno;
	memset(verify, 0, (size_t)aligned_len);

	if (buf == NULL) {
		zm = exfat_map_zeromem(len, &zmapped);
		if (zm == NULL) {
			ret = -errno;
			goto out;
		}

		buf = zm;
	}

	fcntl(bd->dev_fd, F_SETFL, flags|O_DIRECT);
	ret = exfat_read2(bd->dev_fd, verify, &aligned_len, &aligned_off);
	if (ret) {
		exfat_debug("%s verify read failed (off=%llu, len=%llu)\n", what,
				(unsigned long long)aligned_off,
				(unsigned long long)aligned_len);
		ret = -EIO;
		goto out;
	}

	if (memcmp(buf, (const uint8_t *)verify + (size_t)head, len) != 0) {
		exfat_report_verify_mismatch(buf,
				(const uint8_t *)verify + (size_t)head, off, len, what);
		ret = -EIO;
		goto out;
	}

out:
	free(verify);
	exfat_unmap_mm(zm, len, &zmapped);
	fcntl(bd->dev_fd, F_SETFL, flags);

	return ret;
}

int exfat_open_fd_devzero(void)
{
#ifdef _POSIX_MAPPED_FILES
	if (fd_devzero < 0) {
		fd_devzero = open("/dev/zero", O_RDONLY);
		if (fd_devzero < 0)
			return -errno;
	}
#else
	return -ENOSYS;
#endif
	return 0;
}

void exfat_close_fd_devzero(void)
{
	if (fd_devzero < 0)
		return;
	close(fd_devzero);
	fd_devzero = -1;
}

static void *exfat_do_map_zerodev(const size_t len, bool *mapped, int prot, int flags)
{
	void *ret;

	if (len == 0)
		return NULL;

#ifdef _POSIX_MAPPED_FILES
	if (fd_devzero >= 0) {
		assert(prot > 0 && flags > 0);
		ret = mmap(NULL, len, prot, flags, fd_devzero, 0);

		assert(ret != NULL);
		if (ret != MAP_FAILED) {
			*mapped = true;
			return ret;
		}
	} else if (*mapped) {
		errno = EBADFD;
		return NULL;
	}
#else
	if (*mapped) {
		errno = ENOSYS;
		return NULL;
	}
#endif
	ret = calloc(len, 1);
	if (ret != NULL)
		*mapped = false;

	return ret;
}

const void *exfat_map_zeromem(const size_t len, bool *mapped)
{
	int prot = -1;
	int flags = -1;

#ifdef _POSIX_MAPPED_FILES
	prot = PROT_READ;
	flags = MAP_SHARED;
#endif
	return exfat_do_map_zerodev(len, mapped, prot, flags);
}

void *exfat_map_blankmem(const size_t len, bool *mapped)
{
	int prot = -1;
	int flags = -1;

#ifdef _POSIX_MAPPED_FILES
	prot = PROT_READ|PROT_WRITE;
	flags = MAP_PRIVATE;
#endif
	return exfat_do_map_zerodev(len, mapped, prot, flags);
}

int exfat_unmap_mm(const void *m, const size_t len, bool *mapped)
{
	if (m == NULL)
		return 0;

#ifdef _POSIX_MAPPED_FILES
	if (*mapped) {
		*mapped = false;
		if (len > 0)
			return munmap((void *)m, len);
		return 0;
	}
#else
	(void)mapped;
#endif
	free((void *)m);
	(void)len;

	return 0;
}

int exfat_read_sector(struct exfat_blk_dev *bd, void *buf, unsigned long long sec_off)
{
	const unsigned long long offset = sec_off * bd->sector_size;

	/* integer overflow check */
	assert(bd->sector_size < UINT_MAX);

	if (!exfat_read_full(bd->dev_fd, buf, bd->sector_size, offset)) {
		exfat_err("read failed, sec_off : %llu\n", sec_off);
		return -1;
	}
	return 0;
}

int exfat_write_sector(struct exfat_blk_dev *bd, void *buf, unsigned long long sec_off)
{
	const unsigned long long offset = sec_off * bd->sector_size;

	/* integer overflow check */
	assert(bd->sector_size < UINT_MAX);

	if (!exfat_write_full(bd->dev_fd, buf, bd->sector_size, offset)) {
		exfat_err("write failed, sec_off : %llu\n", sec_off);
		return -1;
	}
	return 0;
}

int exfat_write_checksum_sector(struct exfat_blk_dev *bd,
	struct exfat_user_input *ui, unsigned long long sec_off,
	unsigned int checksum, bool is_backup)
{
	__le32 *checksum_buf;
	int ret = 0;
	unsigned int i;

	sec_off += CHECKSUM_SEC_IDX;

	checksum_buf = malloc(bd->sector_size);
	if (!checksum_buf) {
		exfat_err("Cannot allocate checksum_buf: out of memory\n");
		return -1;
	}

	if (is_backup)
		sec_off += BACKUP_BOOT_SEC_IDX;

	for (i = 0; i < bd->sector_size / sizeof(int); i++)
		checksum_buf[i] = cpu_to_le32(checksum);

	ret = exfat_write_sector(bd, checksum_buf, sec_off);
	if (ret) {
		exfat_err("checksum sector write failed\n");
		goto free;
	}

	if (ui && ui->verify) {
		ret = exfat_check_written_data(bd,
				checksum_buf, bd->sector_size,
				sec_off * bd->sector_size,
				"checksum sector");
		if (ret) {
			exfat_err("checksum sector verification failed (read-back mismatch)\n");
			goto free;
		}
	}

free:
	free(checksum_buf);
	return ret;
}

int exfat_show_volume_serial(int fd)
{
	struct pbr *ppbr;
	int ret = 0;

	ppbr = malloc(EXFAT_MAX_SECTOR_SIZE);
	if (!ppbr) {
		exfat_err("Cannot allocate pbr: out of memory\n");
		return -1;
	}

	/* read main boot sector */
	if (!exfat_read_full(fd, (char *)ppbr, EXFAT_MAX_SECTOR_SIZE, 0)) {
		exfat_err("main boot sector read failed\n");
		ret = -1;
		goto free_ppbr;
	}

	if (memcmp(ppbr->bpb.oem_name, "EXFAT   ", 8) != 0) {
		exfat_err("Bad fs_name in boot sector, which does not describe a valid exfat filesystem\n");
		ret = -1;
		goto free_ppbr;
	}

	exfat_info("volume serial : 0x%x\n", le32_to_cpu(ppbr->bsx.vol_serial));

free_ppbr:
	free(ppbr);
	return ret;
}

static int exfat_update_boot_checksum(struct exfat_blk_dev *bd,
	struct exfat_user_input *ui, bool is_backup)
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

	ret = exfat_write_checksum_sector(bd, ui, 0, checksum, is_backup);

free_buf:
	free(buf);

	return ret;
}

int exfat_set_volume_serial(struct exfat_blk_dev *bd,
		struct exfat_user_input *ui)
{
	int ret;
	struct pbr *ppbr;

	ppbr = malloc(EXFAT_MAX_SECTOR_SIZE);
	if (!ppbr) {
		exfat_err("Cannot allocate pbr: out of memory\n");
		return -1;
	}

	/* read main boot sector */
	if (!exfat_read_full(bd->dev_fd, (char *)ppbr, EXFAT_MAX_SECTOR_SIZE, BOOT_SEC_IDX)) {
		exfat_err("main boot sector read failed\n");
		ret = -1;
		goto free_ppbr;
	}

	if (memcmp(ppbr->bpb.oem_name, "EXFAT   ", 8) != 0) {
		exfat_err("Bad fs_name in boot sector, which does not describe a valid exfat filesystem\n");
		ret = -1;
		goto free_ppbr;
	}

	bd->sector_size = 1 << ppbr->bsx.sect_size_bits;
	ppbr->bsx.vol_serial = cpu_to_le32(ui->volume_serial);

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

	ret = exfat_update_boot_checksum(bd, ui, 0);
	if (ret < 0) {
		exfat_err("main checksum update failed\n");
		goto free_ppbr;
	}

	ret = exfat_update_boot_checksum(bd, ui, 1);
	if (ret < 0)
		exfat_err("backup checksum update failed\n");
free_ppbr:
	free(ppbr);

	exfat_info("New volume serial : 0x%x\n", ui->volume_serial);

	return ret;
}

unsigned int exfat_clus_to_blk_dev_off(struct exfat_blk_dev *bd,
		unsigned int clu_off_sectnr, unsigned int clu)
{
	return clu_off_sectnr * bd->sector_size +
		(clu - EXFAT_RESERVED_CLUSTERS) * bd->cluster_size;
}

int exfat_get_next_clus(struct exfat *exfat, clus_t clus, clus_t *next)
{
	off_t offset;

	*next = EXFAT_EOF_CLUSTER;

	if (!exfat_heap_clus(exfat, clus))
		return -EINVAL;

	offset = (off_t)le32_to_cpu(exfat->bs->bsx.fat_offset) <<
				exfat->bs->bsx.sect_size_bits;
	offset += sizeof(clus_t) * clus;

	if (!exfat_read_full(exfat->blk_dev->dev_fd, next, sizeof(*next), offset))
		return -EIO;
	*next = le32_to_cpu(*next);
	return 0;
}

int exfat_get_clus(struct exfat *exfat, struct exfat_inode *node,
		clus_t index, clus_t *ret_clu)
{
	int ret;
	clus_t clu = node->first_clus;

	if (node->is_contiguous) {
		*ret_clu = clu + index;
		return 0;
	}

	while (index) {
		ret = exfat_get_next_clus(exfat, clu, &clu);
		if (ret)
			return ret;

		index--;
	}

	*ret_clu = clu;

	return 0;
}

int exfat_get_inode_next_clus(struct exfat *exfat, struct exfat_inode *node,
			      clus_t clus, clus_t *next)
{
	*next = EXFAT_EOF_CLUSTER;

	if (node->is_contiguous) {
		if (!exfat_heap_clus(exfat, clus))
			return -EINVAL;
		*next = clus + 1;
		return 0;
	}

	return exfat_get_next_clus(exfat, clus, next);
}

int exfat_set_fat(struct exfat *exfat, clus_t clus, clus_t next_clus)
{
	off_t offset;

	offset = le32_to_cpu(exfat->bs->bsx.fat_offset) <<
		exfat->bs->bsx.sect_size_bits;
	offset += sizeof(clus_t) * clus;

	next_clus = cpu_to_le32(next_clus);

	if (!exfat_write_full(exfat->blk_dev->dev_fd, &next_clus, sizeof(next_clus), offset))
		return -EIO;
	return 0;
}

off_t exfat_s2o(struct exfat *exfat, off_t sect)
{
	return sect << exfat->bs->bsx.sect_size_bits;
}

off_t exfat_c2o(struct exfat *exfat, unsigned int clus)
{
	assert(clus >= EXFAT_FIRST_CLUSTER);

	return exfat_s2o(exfat, le32_to_cpu(exfat->bs->bsx.clu_offset) +
				((off_t)(clus - EXFAT_FIRST_CLUSTER) <<
				 exfat->bs->bsx.sect_per_clus_bits));
}

int exfat_o2c(struct exfat *exfat, off_t device_offset,
	      unsigned int *clu, unsigned int *offset)
{
	off_t heap_offset;

	heap_offset = exfat_s2o(exfat, le32_to_cpu(exfat->bs->bsx.clu_offset));
	if (device_offset < heap_offset)
		return -ERANGE;

	*clu = (unsigned int)((device_offset - heap_offset) /
			      exfat->clus_size) + EXFAT_FIRST_CLUSTER;
	if (!exfat_heap_clus(exfat, *clu))
		return -ERANGE;
	*offset = (device_offset - heap_offset) % exfat->clus_size;
	return 0;
}

bool exfat_heap_clus(struct exfat *exfat, clus_t clus)
{
	return clus >= EXFAT_FIRST_CLUSTER &&
		(clus - EXFAT_FIRST_CLUSTER) < exfat->clus_count;
}

int exfat_root_clus_count(struct exfat *exfat)
{
	struct exfat_inode *node = exfat->root;
	clus_t clus, next;
	int clus_count = 0;

	if (!exfat_heap_clus(exfat, node->first_clus))
		return -EIO;

	clus = node->first_clus;
	do {
		if (exfat_bitmap_get(exfat->alloc_bitmap, clus))
			return -EINVAL;

		exfat_bitmap_set(exfat->alloc_bitmap, clus);

		if (exfat_get_inode_next_clus(exfat, node, clus, &next)) {
			exfat_err("ERROR: failed to read the fat entry of root");
			return -EIO;
		}

		if (next != EXFAT_EOF_CLUSTER && !exfat_heap_clus(exfat, next))
			return -EINVAL;

		clus = next;
		clus_count++;
	} while (clus != EXFAT_EOF_CLUSTER);

	node->size = clus_count * exfat->clus_size;
	return 0;
}

int read_boot_sect(struct exfat_blk_dev *bdev, struct pbr **bs)
{
	struct pbr *pbr;
	int err = 0;
	unsigned int sect_size, clu_size;

	pbr = malloc(sizeof(struct pbr));
	if (!pbr) {
		exfat_err("failed to allocate memory\n");
		return -ENOMEM;
	}

	if (!exfat_read_full(bdev->dev_fd, pbr, sizeof(*pbr), 0)) {
		exfat_err("failed to read a boot sector\n");
		err = -EIO;
		goto err;
	}

	err = -EINVAL;
	if (memcmp(pbr->bpb.oem_name, "EXFAT   ", 8) != 0) {
		exfat_err("failed to find exfat file system\n");
		goto err;
	}

	sect_size = 1 << pbr->bsx.sect_size_bits;
	clu_size = 1 << (pbr->bsx.sect_size_bits +
			 pbr->bsx.sect_per_clus_bits);

	if (sect_size < 512 || sect_size > 4 * KB) {
		exfat_err("too small or big sector size: %d\n",
			  sect_size);
		goto err;
	}

	if (clu_size < sect_size || clu_size > 32 * MB) {
		exfat_err("too small or big cluster size: %d\n",
			  clu_size);
		goto err;
	}

	*bs = pbr;
	return 0;
err:
	free(pbr);
	return err;
}

int exfat_parse_ulong(const char *s, unsigned long *out)
{
	char *endptr;

	errno = 0;

	*out = strtoul(s, &endptr, 0);

	if (errno)
		return -errno;

	if (s == endptr || *endptr != '\0')
		return -EINVAL;

	return 0;
}

static inline int check_bad_utf16_char(unsigned short w)
{
	return (w < 0x0020) || (w == '*') || (w == '?') || (w == '<') ||
		(w == '>') || (w == '|') || (w == '"') || (w == ':') ||
		(w == '/') || (w == '\\');
}

int exfat_check_name(__le16 *utf16_name, int len)
{
	int i;

	for (i = 0; i < len; i++) {
		if (check_bad_utf16_char(le16_to_cpu(utf16_name[i])))
			break;
	}

	return i;
}

int exfat_select_part_type(const struct exfat_blk_dev *bd,
		enum exfat_part_table_type *pt, const bool quiet)
{
	if (bd->isblk && !bd->ispart && bd->isdev && !bd->isremov) {
		/*
		 * If the target volume:
		 *
		 *  - is a block device AND
		 *  - is not a partition AND
		 *  - is a real device (not "virtual" like loopdev, dm, nbd or anything) AND
		 *  - hasn't got "removable" USB attribute
		 *
		 * , it needs a partition table for MS Windows to be able to recognise it. Go
		 * through fairly straightforward heuristics to determine the type of partition
		 * table suitable for this device.
		 */
		if (*pt == PART_TABLE_NONE) {
			/* The user says no... Well, we could at least warn them. */
			if (!quiet) {
				exfat_info("!!! A partition table is required for MS Windows to recognise this volume !!!\n");
				exfat_info("But one will not be created because it's been overridden with -P option.\n");
			}
			return 0;
		}

		if (*pt == PART_TABLE_AUTO) {
			if (bd->size >= 2ULL * TB)
				/*
				 * no, we don't determine based on the sector count because it
				 * doesn't matter for Windows. 4K-bs or not, it just can't do MBR on
				 * >= 2TiB devices.
				 */
				*pt = PART_TABLE_GPT;
			else
				*pt = PART_TABLE_MBR;
		}
	} else if (*pt == PART_TABLE_AUTO)
		/* For any other type of blockdev, don't worry about it */
		*pt = PART_TABLE_NONE;

	/*
	 * There's a classic off-by-one limitation common in many implementations where the size of
	 * the partition in the recursive(fake) MBR may be calculated off by one. Let's just not
	 * deal with that case by imposing the hard limit of 2 TiB, inclusive.
	 *
	 * See exfat_put_mbr_partition().
	 */
	if (bd->size >= 2ULL * TB && *pt == PART_TABLE_MBR) {
		exfat_err("Refusing to create an MBR partition table on a device that's 2 TiB or larger!\n");
		return -1;
	}

	if (bd->ispart && *pt != PART_TABLE_NONE) {
		/* bro... */
		exfat_err("Refusing to create a partition table within a partition!\n");
		return -1;
	}

	return 0;
}

static void lba_to_chs(const uint32_t lba, const uint32_t sec_per_track, const uint32_t heads,
		uint8_t *a, uint8_t *b, uint8_t *c)
{
	unsigned int head_low, head_high;

	head_low = (1 + lba % sec_per_track) & 0x3F;
	head_high = ((lba / (heads * sec_per_track)) >> 8) << 6;

	*a = (uint8_t)((lba / sec_per_track) % heads);
	*b = (uint8_t)(head_low | head_high);
	*c = (uint8_t)((lba / (heads * sec_per_track)) & 255);
}

void exfat_put_mbr_partition(const struct exfat_blk_dev *bd, void *dst,
		uint32_t serial, uint32_t offset_sector, const bool active,
		const uint8_t ptype, uint32_t chs_limit)
{
	struct pbr *out = dst;
	uint32_t heads, sec_per_track;
	uint32_t nb_secs, last_sect;

	/*
	 * Now, we're entering the fictional land of CHS.
	 * Refer to mkfs.fat.c in dosfstools in order to make sense of all this.
	 *
	 * It really doesn't matter for implementations that understand exFAT.
	 *
	 * CHS Recommendations from SD Card Part 2 File System Specification
	 * Version 3.00 (April 16, 2009)
	 */
	if (bd->num_sectors >= 67371008) { /* 32896MB */
		/* Appendix C.5.2 */
		heads = 255;
		sec_per_track = 63;
	} else if (bd->num_sectors <= 524288) { /* 256MB */
		heads = bd->num_sectors <=  32768 ? 2 :
			bd->num_sectors <=  65536 ? 4 :
			bd->num_sectors <= 262144 ? 8 : 16;
		sec_per_track = bd->num_sectors <= 4096 ? 16 : 32;
	} else {
		heads = bd->num_sectors <=  16*63*1024 ? 16 :
			bd->num_sectors <=  32*63*1024 ? 32 :
			bd->num_sectors <=  64*63*1024 ? 64 :
			bd->num_sectors <= 128*63*1024 ? 128 : 255;
		sec_per_track = 63;
	}

	/*
	 * Careful, some arches like IBM z, MIPS, older ARMv4 can't do unaligned
	 * memory access. Access byte-by-byte or use memcpy()/memset() or the
	 * process will crash with SIGBUS.
	 */

	/* 0x01B8: 32-bit disk signature */
	serial = cpu_to_le32(serial);
	memcpy(&out->mbr.disk_signature, &serial, 4);
	/* 0x01BC: copy-protected? (nope) */
	memset(&out->mbr.copy_protected, 0, 2);

	/* start with a clean slate */
	memset(&out->mbr.part_entries, 0, sizeof(out->mbr.part_entries));

	/* Partition entry #1 */
	/* is active (boot flag)? */
	if (active)
		out->mbr.part_entries[0].active = 0x80;

	/* CHS address of the first sector */
	lba_to_chs(offset_sector, sec_per_track, heads,
			&out->mbr.part_entries[0].chs_ofs[0],
			&out->mbr.part_entries[0].chs_ofs[1],
			&out->mbr.part_entries[0].chs_ofs[2]);

	/* Partition type */
	out->mbr.part_entries[0].type = ptype;

	/* Demote sectors to 32 bit for calculations, clamp to 0xFFFFFFFF as per UEFI spec */

	if (bd->num_sectors - offset_sector > UINT32_MAX)
		nb_secs = UINT32_MAX;
	else
		nb_secs = (uint32_t)(bd->num_sectors - offset_sector);
	if (bd->num_sectors > UINT32_MAX)
		last_sect = UINT32_MAX;
	else
		last_sect = (uint32_t)bd->num_sectors;

	/* CHS address of the last sector */
	if (bd->num_sectors >= sec_per_track * heads * 1024) {
		/* CHS address is too large use tuple supplied */
		chs_limit = cpu_to_le32(chs_limit);
		memcpy(&out->mbr.part_entries[0].chs_end, &chs_limit, 3);
	} else
		lba_to_chs(last_sect, sec_per_track, heads,
				&out->mbr.part_entries[0].chs_end[0],
				&out->mbr.part_entries[0].chs_end[1],
				&out->mbr.part_entries[0].chs_end[2]);


	/* LBA of the first sector */
	offset_sector = cpu_to_le32(offset_sector);
	memcpy(&out->mbr.part_entries[0].ofs_lba, &offset_sector, 4);

	/* Number of sectors */
	nb_secs = cpu_to_le32(nb_secs);
	memcpy(&out->mbr.part_entries[0].len_lba, &nb_secs, 4);
}

void exfat_put_bootstrap_code(const char *user_msg, void *dst, unsigned int code_offset)
{
	char *p = dst;
	size_t msglen = strlen(user_msg);
	uint16_t jump_ofs;

	msglen = MIN(msglen, (size_t)EXFAT_MAX_BOOTCODE_MSGLEN);

	jump_ofs = 0x7C00 + code_offset + EXFAT_DUMMY_BOOTCODE_SIZE;
	jump_ofs = cpu_to_le16(jump_ofs);

	memcpy(p, dummy_bootcode, EXFAT_DUMMY_BOOTCODE_SIZE);
	memcpy(p + EXFAT_BOOTCODE_MSG_JMP_OFFSET, &jump_ofs, 2);
	p += EXFAT_DUMMY_BOOTCODE_SIZE;
	memcpy(p, user_msg, msglen);
	p += msglen;
	if (msglen > 1 && strncmp(p - 2, "\r\n", 2) != 0) {
		/* Terminate the message with CrLF if possible */
		if (msglen + 2 <= EXFAT_MAX_BOOTCODE_MSGLEN) {
			p[0] = '\r';
			p[1] = '\n';
			p += 2;
		} else if (msglen + 1 <= EXFAT_MAX_BOOTCODE_MSGLEN) {
			p[0] = '\n';
			p += 1;
		}
	}
	/* *p = 0; */
}
