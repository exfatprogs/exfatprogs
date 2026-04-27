/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Copyright (C) 2019 Namjae Jeon <linkinjeon@kernel.org>
 */

#ifndef _MKFS_H
#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>

#define MIN_NUM_SECTOR			(2048)
#define EXFAT_MAX_CLUSTER_SIZE		(32*1024*1024)
#define EXFAT_HEAD_ZERO_OUT		(0x10000)

#define EXFAT_GPT_ENTRY_SIZE		(128UL)
#define EXFAT_GPT_ENTRY_CNT		(128UL)
#define EXFAT_GPT_ENTRY_ARR_SIZE	(EXFAT_GPT_ENTRY_SIZE * EXFAT_GPT_ENTRY_CNT)
#define EXFAT_GPT_MIN_PART_ALIGNMENT	(1ULL * MB)
enum {
	EXFAT_GPT_DATA_REGION_P_MBR,
	EXFAT_GPT_DATA_REGION_M_HEAD,
	EXFAT_GPT_DATA_REGION_M_ARR,
	EXFAT_GPT_DATA_REGION_B_ARR,
	EXFAT_GPT_DATA_REGION_B_HEAD,
	EXFAT_GPT_DATA_REGION_CNT,
};

struct exfat_mkfs_data_region {
	off_t ofs;
	size_t len;
	void *data;
};

struct exfat_mkfs_info {
	struct {
		struct {
			unsigned long long ofs;
			unsigned long long len;
		} sec;
		struct {
			unsigned long long ofs;
			unsigned long long len;
		} byte;
		unsigned long long start_ofs;
	} target;

	unsigned int total_clu_cnt;
	unsigned int used_clu_cnt;
	unsigned long long fat_byte_off;
	unsigned long long fat_byte_len;
	unsigned long long clu_byte_off;
	unsigned long long bitmap_byte_off;
	unsigned int bitmap_byte_len;
	unsigned long long ut_byte_off;
	unsigned int ut_start_clu;
	unsigned int ut_byte_len;
	unsigned int ut_chksum;
	unsigned long long root_byte_off;
	unsigned int root_byte_len;
	unsigned int root_start_clu;
	unsigned int volume_serial;

	struct {
		void *mbr;
		struct exfat_gpt_header *head_main;
		struct exfat_gpt_header *head_backup;
		struct exfat_gpt_entry *arr;
		struct exfat_mkfs_data_region regions[EXFAT_GPT_DATA_REGION_CNT];
	} gpt;
};

struct exfat_guid {
	uint8_t b[16];
};

struct exfat_gpt_header {
	uint64_t signature;
	uint32_t revision;
	uint32_t header_size;
	uint32_t header_crc32;
	uint32_t reserved1;
	uint64_t my_lba;
	uint64_t alternate_lba;
	uint64_t first_usable_lba;
	uint64_t last_usable_lba;
	struct exfat_guid disk_guid;
	uint64_t partition_entry_lba;
	uint32_t num_partition_entries;
	uint32_t sizeof_partition_entry;
	uint32_t partition_entry_array_crc32;
} __attribute__((__packed__));
/* static_assert(sizeof(struct exfat_gpt_header) == 92); */

struct exfat_gpt_entry_attrs {
	uint64_t a; /* not used at the moment */
} __attribute__((__packed__));

struct exfat_gpt_entry {
	struct exfat_guid partition_type_guid;
	struct exfat_guid unique_partition_guid;
	uint64_t starting_lba;
	uint64_t ending_lba;
	struct exfat_gpt_entry_attrs attrs;
	uint16_t partition_name[72/sizeof(uint16_t)];
} __attribute__((__packed__));
/* static_assert(sizeof(struct exfat_gpt_entry) == 128); */

extern struct exfat_mkfs_info finfo;

int exfat_create_upcase_table(struct exfat_blk_dev *bd, struct exfat_user_input *ui);

/* excluding checksums */
#define exfat_print_gpt_header(f, t, h) \
	do {\
		f("GPT "t" header signature             : 0x%016"PRIx64"\n", h->signature);	   \
		f("GPT "t" header revision              : 0x%08"PRIx32"\n", h->revision);	   \
		f("GPT "t" header header_size           : %"PRIu32"\n", h->header_size);	   \
		f("GPT "t" header my_lba                : %"PRIu64"\n", h->my_lba);		   \
		f("GPT "t" header alternate_lba         : %"PRIu64"\n", h->alternate_lba);	   \
		f("GPT "t" header first_usable_lba      : %"PRIu64"\n", h->first_usable_lba);	   \
		f("GPT "t" header last_usable_lba       : %"PRIu64"\n", h->last_usable_lba);	   \
		exfat_print_guid(f,								   \
		  "GPT "t" header disk_guid             ", h->disk_guid.b);			   \
		f("GPT "t" header partition_entry_lba   : %"PRIu64"\n", h->partition_entry_lba);   \
		f("GPT "t" header num_partition_entries : %"PRIu32"\n", h->num_partition_entries); \
		f("GPT "t" header sizeof_partition_entry: %"PRIu32"\n", h->sizeof_partition_entry);\
	} while(0)


void exfat_zero_mkfs_data_regions(struct exfat_mkfs_data_region *r);
bool exfat_write_mkfs_data_regions(struct exfat_blk_dev *bd,
		const struct exfat_mkfs_data_region *r);
bool exfat_verify_mkfs_data_regions(struct exfat_blk_dev *bd,
		const struct exfat_mkfs_data_region *r);

/* crc.c */
uint32_t exfat_efi_crc32(const void *buf, size_t len);

bool exfat_open_rnddev(void);
void exfat_close_rnddev(void);
void exfat_gen_guid(void *out);

#endif /* !_MKFS_H */
