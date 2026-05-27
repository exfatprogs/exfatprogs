/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Copyright (C) 2019 Namjae Jeon <linkinjeon@kernel.org>
 */

#ifndef _LIBEXFAT_H

#include <stdbool.h>
#include <sys/types.h>
#include <wchar.h>
#include <limits.h>

typedef __u32 clus_t;

#define KB			(1024)
#define MB			(1024*1024)
#define GB			(1024UL*1024UL*1024UL)
#define TB			(1024ULL*1024ULL*1024ULL*1024ULL)

#define __round_mask(x, y) ((__typeof__(x))((y)-1))
#define round_up(x, y) ((((x)-1) | __round_mask(x, y))+1)
#define round_down(x, y) ((x) & ~__round_mask(x, y))

#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))

#define DIV_ROUND_UP(__i, __d)	(((__i) + (__d) - 1) / (__d))

#define EXFAT_MIN_NUM_SEC_VOL		(2048)
#define EXFAT_MAX_NUM_SEC_VOL		((2 << 64) - 1)

#define EXFAT_MAX_NUM_CLUSTER		(0xFFFFFFF5)

#define DEFAULT_BOUNDARY_ALIGNMENT	(1024*1024)

#define DEFAULT_SECTOR_SIZE	(512)

#define VOLUME_LABEL_BUFFER_SIZE	(VOLUME_LABEL_MAX_LEN*MB_LEN_MAX+1)

/* Upcase table macro */
#define EXFAT_UPCASE_TABLE_CHARS	(0x10000)
#define EXFAT_UPCASE_TABLE_SIZE		(5836)

/* Flags for tune.exfat and exfatlabel */
#define EXFAT_GET_VOLUME_LABEL		0x01
#define EXFAT_SET_VOLUME_LABEL		0x02
#define EXFAT_GET_VOLUME_SERIAL		0x03
#define EXFAT_SET_VOLUME_SERIAL		0x04
#define EXFAT_GET_VOLUME_GUID		0x05
#define EXFAT_SET_VOLUME_GUID		0x06

#define EXFAT_MAX_SECTOR_SIZE		4096

#define EXFAT_CLUSTER_SIZE(pbr) (1 << ((pbr)->bsx.sect_size_bits +	\
					(pbr)->bsx.sect_per_clus_bits))
#define EXFAT_SECTOR_SIZE(pbr) (1 << (pbr)->bsx.sect_size_bits)

#define EXFAT_MAX_HASH_COUNT		(UINT16_MAX + 1)

#define EXFAT_MAX_UPCASE_CHARS		(0x10000)
#define EXFAT_MAX_UPCASE_TABLE_SIZE	(EXFAT_MAX_UPCASE_CHARS * sizeof(__u16))

#define EXFAT_MBR_PART_TYPE	(0x07) /* same as HPFS/NTFS */
/* Encoded in MS's mixed endianness (first 32, 16, 16 LE, the rest BE) */
#define EXFAT_GPT_PART_TYPE	"\xA2\xA0\xD0\xEB""\xE5\xB9""\x33\x44""\x87\xC0""\x68\xB6\xB7\x26\x99\xC7"
/* #define EXFAT_GPT_PART_TYPE	"\xEB\xD0\xA0\xA2""\xB9\xE5""\x44\x33""\x87\xC0""\x68\xB6\xB7\x26\x99\xC7" */

enum {
	BOOT_SEC_IDX = 0,
	EXBOOT_SEC_IDX,
	EXBOOT_SEC_NUM = 8,
	OEM_SEC_IDX,
	RESERVED_SEC_IDX,
	CHECKSUM_SEC_IDX,
	BACKUP_BOOT_SEC_IDX,
};

struct exfat_blk_dev {
	int dev_fd;
	unsigned long long offset;
	unsigned long long alignment_offset;
	unsigned long long size;
	unsigned int sector_size;
	unsigned int sector_size_bits;
	unsigned long long num_sectors;
	unsigned int num_clusters;
	unsigned int cluster_size;
	unsigned int dev_sector_size;
	/* is the target a block device in /sys/dev/block? */
	bool isblk:1;
	/* is the target a partition in a block device? */
	bool ispart:1;
	/*
	 * is the target a block device directly associated with a real device
	 * (has "device" symlink in /sys/dev/block/MAJ:MIN/)?
	 */
	bool isdev:1;
	/*
	 * is the target device advertising itself as a "removable" device
	 * through the underlying interface?
	 */
	bool isremov:1;
};

enum exfat_part_table_type {
	PART_TABLE_AUTO = -1,
	PART_TABLE_NONE,
	PART_TABLE_MBR,
	PART_TABLE_GPT,
};

extern const char *exfat_part_table_type_str[];

#define EXFAT_DUMMY_BOOTCODE_SIZE (29)
extern const char *dummy_bootcode;
/*
 * Out of 390 bytes in BootCode field, take the length of the x86 code, one byte
 * for a null-terminator, and 70 bytes for the MBR disk signature and partition
 * entries(6 + 16 * 4).
 */
#define EXFAT_MAX_BOOTCODE_MSGLEN (390 - EXFAT_DUMMY_BOOTCODE_SIZE - 1 - 70)
/* static_assert(EXFAT_MAX_BOOTCODE_MSGLEN > 0); */
extern const char *dummy_bootcode_msg;
#define EXFAT_BOOTCODE_MSG_JMP_OFFSET (3)

struct exfat_user_input {
	const char *dev_name;
	bool writeable;
	unsigned int sector_size;
	unsigned int cluster_size;
	unsigned int sec_per_clu;
	unsigned int boundary_align;
	bool pack_bitmap;
	bool quick;
	bool force;
	bool verify;
	bool discard;
	__u16 volume_label[VOLUME_LABEL_MAX_LEN];
	int volume_label_len;
	unsigned int volume_serial;
	const char *guid;
	unsigned char *fat_table_buff;

	struct {
		const char *file;
		const unsigned char *table;
		void *m;
		size_t len;
		void (*free)(struct exfat_user_input *ui);
	} upcase;

	const char *bootcode_msg;

	enum exfat_part_table_type part_table;
};

/* Returns true if the option used or the option argument is not an empty string */
static inline bool exfat_ui_has_upcase_file(const struct exfat_user_input *ui)
{
	return ui->upcase.file != NULL && ui->upcase.file[0] != 0;
}

struct exfat;
struct exfat_inode;

#ifdef WORDS_BIGENDIAN
typedef __u8	bitmap_t;
#else
typedef __u32	bitmap_t;
#endif

#define BITS_PER	(sizeof(bitmap_t) * 8)
#define BIT_MASK(__c)	(1 << ((__c) % BITS_PER))
#define BIT_ENTRY(__c)	((__c) / BITS_PER)

#define EXFAT_BITMAP_SIZE(__c_count)	\
	(DIV_ROUND_UP(__c_count, BITS_PER) * sizeof(bitmap_t))

#define BITMAP_GET(bmap, bit)	\
	(((bitmap_t *)(bmap))[BIT_ENTRY(bit)] & BIT_MASK(bit))

#define BITMAP_SET(bmap, bit)	\
	(((bitmap_t *)(bmap))[BIT_ENTRY(bit)] |= BIT_MASK(bit))

#define BITMAP_CLEAR(bmap, bit)	\
	(((bitmap_t *)(bmap))[BIT_ENTRY(bit)] &= ~BIT_MASK(bit))

static inline bool exfat_bitmap_get(unsigned char *bmap, clus_t c)
{
	clus_t cc = c - EXFAT_FIRST_CLUSTER;

	return BITMAP_GET(bmap, cc);
}

static inline void exfat_bitmap_set(unsigned char *bmap, clus_t c)
{
	clus_t cc = c - EXFAT_FIRST_CLUSTER;

	BITMAP_SET(bmap, cc);
}

static inline void exfat_bitmap_clear(unsigned char *bmap, clus_t c)
{
	clus_t cc = c - EXFAT_FIRST_CLUSTER;
	(((bitmap_t *)(bmap))[BIT_ENTRY(cc)] &= ~BIT_MASK(cc));
}

void exfat_bitmap_set_range(struct exfat *exfat, unsigned char *bitmap,
			    clus_t start_clus, clus_t count);
int exfat_bitmap_find_zero(struct exfat *exfat, unsigned char *bmap,
			   clus_t start_clu, clus_t *next);
int exfat_bitmap_find_one(struct exfat *exfat, unsigned char *bmap,
			  clus_t start_clu, clus_t *next);

void show_version(void);

wchar_t exfat_bad_char(wchar_t w);
void boot_calc_checksum(const unsigned char *sector, size_t size,
		bool is_boot_sec, __le32 *checksum);
void exfat_init_user_input(struct exfat_user_input *ui);
void exfat_deinit_user_input(struct exfat_user_input *ui);
void exfat_init_blk_dev_info(struct exfat_blk_dev *bd);
void exfat_deinit_blk_dev_info(struct exfat_blk_dev *bd);
int exfat_get_blk_dev_info(struct exfat_user_input *ui,
		struct exfat_blk_dev *bd);
/*
 * Load the requested size of data from the file onto buf
 *
 * The function ensures that all of the requested size has been read and memory
 * at buf contains the data read from the file. If offset is a negative value,
 * read() is used. Otherwise, pread() is used to do I/O.
 *
 * If all of the size cannot be read due to an error, -errno is returned.
 * Otherwise, the number of bytes read is returned. Note that the number of
 * bytes returned may be less than the size requested.
 *
 * Note that the function will read SSIZE_MAX bytes(~2GB on 32-bit arch) at most
 * due to interface limitations. exfat_read2() has no such limitation.
 */
ssize_t exfat_read(int fd, void *buf, size_t size, off_t offset);
int exfat_read2(int fd, void *buf, off_t *size, off_t *offset);
bool exfat_read_full(int fd, void *buf, size_t size, off_t offset);
/*
 * Write the requested buffer to the file
 *
 * The function ensures that all of the requested buffer has been written to the
 * file. If offset is a negative value, write() is used. Otherwise, pwrite() is
 * used to do I/O.
 *
 * If all of the buffer data cannot be written read due to an error, -errno.
 * Otherwise, the number of bytes successfully written is returned.
 *
 * Note that the function will write SSIZE_MAX bytes(~2GB on 32-bit arch) at
 * most due to interface limitations. exfat_write2() has no such limitation.
 *
 * In case of an error, the caller cannot determine how much data has been
 * written to the file with the function. exfat_write2() may be used where such
 * info is required.
 */
ssize_t exfat_write(int fd, const void *buf, size_t size, off_t offset);
int exfat_write2(int fd, const void *buf, off_t *size, off_t *offset);
bool exfat_write_full(int fd, const void *buf, size_t size, off_t offset);
int exfat_write_zero(int fd, off_t size, off_t offset);
int exfat_write_zero2(int fd, off_t size, off_t offset, size_t bs);
int exfat_discard_blocks(int fd, uint64_t start, uint64_t len);

size_t exfat_utf16_len(const __le16 *str, size_t max_size);
ssize_t exfat_utf16_enc(const char *in_str, __u16 *out_str, size_t out_size);
ssize_t exfat_utf16_dec(const __u16 *in_str, size_t in_len,
			char *out_str, size_t out_size);
off_t exfat_get_root_entry_offset(struct exfat_blk_dev *bd);
int exfat_read_volume_label(struct exfat *exfat);
int exfat_set_volume_label(struct exfat *exfat, char *label_input);
int __exfat_set_volume_guid(struct exfat_dentry *dentry, const char *guid);
int exfat_read_volume_guid(struct exfat *exfat);
int exfat_set_volume_guid(struct exfat *exfat, const char *guid);
int exfat_read_sector(struct exfat_blk_dev *bd, void *buf, unsigned long long sec_off);
int exfat_write_sector(struct exfat_blk_dev *bd, void *buf, unsigned long long sec_off);
int exfat_write_checksum_sector(struct exfat_blk_dev *bd,
	struct exfat_user_input *ui, unsigned long long sec_off,
	unsigned int checksum, bool is_backup);
char *exfat_conv_volume_label(struct exfat_dentry *vol_entry);
int exfat_show_volume_serial(int fd);
int exfat_set_volume_serial(struct exfat_blk_dev *bd,
		struct exfat_user_input *ui);
unsigned int exfat_clus_to_blk_dev_off(struct exfat_blk_dev *bd,
		unsigned int clu_off, unsigned int clu);
int exfat_get_next_clus(struct exfat *exfat, clus_t clus, clus_t *next);
int exfat_get_inode_next_clus(struct exfat *exfat, struct exfat_inode *node,
			      clus_t clus, clus_t *next);
int exfat_get_clus(struct exfat *exfat, struct exfat_inode *node,
		clus_t index, clus_t *ret_clu);
int exfat_set_fat(struct exfat *exfat, clus_t clus, clus_t next_clus);
off_t exfat_s2o(struct exfat *exfat, off_t sect);
off_t exfat_c2o(struct exfat *exfat, unsigned int clus);
int exfat_o2c(struct exfat *exfat, off_t device_offset,
	      unsigned int *clu, unsigned int *offset);
bool exfat_heap_clus(struct exfat *exfat, clus_t clus);
int exfat_root_clus_count(struct exfat *exfat);
int read_boot_sect(struct exfat_blk_dev *bdev, struct pbr **bs);
int exfat_parse_ulong(const char *s, unsigned long *out);
int exfat_check_name(__le16 *utf16_name, int len);
/*
 * Read back from the target device to confirm the successful write.
 *
 * If buf is NULL, checks that data read back from the device is all-zero(i.e.
 * confirms successful zeroing).
 */
int exfat_check_written_data(struct exfat_blk_dev *bd, const void *buf,
				     size_t len, off_t off,
				     const char *what);
/* Opens "/dev/zero" for exfat_map_zeromem(). */
int exfat_open_fd_devzero(void);
/* Closes "/dev/zero" opened for exfat_map_zeromem(). */
void exfat_close_fd_devzero(void);
/*
 * Map read-only pages for testing memory range is all-zero in conjunction with
 * memcmp().
 *
 * If mapped is unset, fallback to calloc() if mmap() is not available on the
 * host platform or exfat_open_fd_devzero() hasn't been called. If mmap() has
 * been successful, mapped is set. NULL is returned if neither mmap() nor
 * calloc() has been possible and errno is set.
 *
 * If mapped is set, try mmap() and do not fallback to calloc. On error, NULL is
 * returned and errno is set to ENOSYS if the platform does not support mmap(),
 * EBADFD if exfat_open_fd_devzero() has been called successfully, or any other
 * errno set in mmap().
 *
 * Note that /dev/zero mapping do not count towards committed memory. See
 * vm.overcommit for detail.
 *
 * For rw version of this function, see exfat_map_blankmem().
 */
const void *exfat_map_zeromem(const size_t len, bool *mapped);
/*
 * Map read-writeable pages suitable for composing binary data containing mostly
 * zeros(e.g. allocation bitmap).
 *
 * The function has the exact same semantics as exfat_map_zeromem(), except that
 * it calls mmap() with PROT_READ|PROT_WRITE and MAP_PRIVATE.
 */
void *exfat_map_blankmem(const size_t len, bool *mapped);
/*
 * Unmap memory mapping mapped with exfat_map_zeromem() and exfat_map_blankmem().
 *
 * If mapped is set, call munmap(), clear mapped unset and return the value
 * returned from munmap(). Otherwise, call free() and return 0.
 */
int exfat_unmap_mm(const void *m, const size_t len, bool *mapped);

/*
 * Partition table related
 */

int exfat_select_part_type(const struct exfat_blk_dev *bd,
		enum exfat_part_table_type *pt, const bool quiet);
/*
 * Put one partition entry that covers the entire device. Used for both
 * protective MBR for GPT and "fake"(recursive) MBR for "fixed" devices for
 * Windows.
 *
 * For protective MBRs, unset active flag and set offset_sector to 1 as per
 * UEFI spec.
 *
 * As for chs_limit, set to
 *
 *  - 0xFFFFFE: "conventional" tuple limit (1023, 254, 63)
 *  - 0xFFFFFF: tuple limit mandated by UEFI spec (1023, 255, 63)
 *
 * Note that LBA is always clamped to 0xFFFFFFFF.
 *
 * As for ptype, set to
 *
 *  - 0x07: HPFS/NTFS/exFAT
 *  - 0xEE: GPT protective
 */
void exfat_put_mbr_partition(const struct exfat_blk_dev *bd, void *dst,
		uint32_t serial, uint32_t offset_sector, const bool active,
		const uint8_t ptype, uint32_t chs_limit);
void exfat_put_bootstrap_code(const char *user_msg, void *dst, unsigned int code_offset);

/*
 * Misc
 */

/*
 * Returns true if STDIN and STDOUT are associated with a terminal. Returns
 * false otherwise.
 *
 * To override the return value of this function, the influential env var
 * EXFAT_TTY_OVERRIDE can be set to a non-negative integral value. This is for
 * testing `mkfs.exfat -F` in xfstests and CI pipelines.
 */
bool exfat_isatty_stdio(void);

/*
 * Exfat Print
 */

extern unsigned int print_level;

#define EXFAT_ERROR	(1)
#define EXFAT_INFO	(2)
#define EXFAT_DEBUG	(3)

#define exfat_msg(level, dir, fmt, ...)					\
	do {								\
		if (print_level >= level) {				\
			fprintf(dir, fmt, ##__VA_ARGS__);		\
		}							\
	} while (0)							\

#define exfat_err(fmt, ...)	exfat_msg(EXFAT_ERROR, stderr,		\
					fmt, ##__VA_ARGS__)
#define exfat_info(fmt, ...)	exfat_msg(EXFAT_INFO, stdout,		\
					fmt, ##__VA_ARGS__)
#define exfat_debug(fmt, ...)	exfat_msg(EXFAT_DEBUG, stdout,		\
					"[%s:%4d] " fmt, __func__, 	\
					__LINE__, ##__VA_ARGS__)

#define exfat_print_guid(f, msg, guid)					\
		f("%s: %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",\
			(msg),						\
			(guid)[0], (guid)[1], (guid)[2], (guid)[3],	\
			(guid)[4], (guid)[5], (guid)[6], (guid)[7],	\
			(guid)[8], (guid)[9], (guid)[10], (guid)[11],	\
			(guid)[12], (guid)[13], (guid)[14], (guid)[15])

#endif /* !_LIBEXFAT_H */
