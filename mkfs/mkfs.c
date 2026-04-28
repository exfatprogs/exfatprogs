// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2019 Namjae Jeon <linkinjeon@kernel.org>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <errno.h>
#include <locale.h>
#include <time.h>
#include <assert.h>
#ifdef _POSIX_MAPPED_FILES
#include <sys/mman.h>
#endif

#include "exfat_ondisk.h"
#include "libexfat.h"
#include "mkfs.h"
#include "upcase_table.h"

struct exfat_mkfs_info finfo;
static int fd_rnddev = -1;

/* random serial generator based on current time */
static unsigned int get_new_serial(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_REALTIME, &ts)) {
		/* set 0000-0000 on error */
		ts.tv_sec = 0;
		ts.tv_nsec = 0;
	}

	return (unsigned int)(ts.tv_nsec << 12 | ts.tv_sec);
}

static inline unsigned long long gpt_backup_array_byte_offset(const struct exfat_blk_dev *bd,
		const unsigned long long arr_size)
{
	unsigned long long backup_arr_ofs;

	backup_arr_ofs = bd->size - bd->sector_size - arr_size;
	return round_down(backup_arr_ofs, (unsigned long long)bd->sector_size);
}

static void gpt_entry_arr_to_le(struct exfat_gpt_entry *e)
{
	e->starting_lba = cpu_to_le64(e->starting_lba);
	e->ending_lba = cpu_to_le64(e->ending_lba);
}

static void gpt_header_to_le(struct exfat_gpt_header *h)
{
	h->signature = cpu_to_le64(h->signature);
	h->revision = cpu_to_le32(h->revision);
	h->header_size = cpu_to_le32(h->header_size);
	h->my_lba = cpu_to_le64(h->my_lba);
	h->alternate_lba = cpu_to_le64(h->alternate_lba);
	h->first_usable_lba = cpu_to_le64(h->first_usable_lba);
	h->last_usable_lba = cpu_to_le64(h->last_usable_lba);
	h->partition_entry_lba = cpu_to_le64(h->partition_entry_lba);
	h->num_partition_entries = cpu_to_le32(h->num_partition_entries);
	h->sizeof_partition_entry = cpu_to_le32(h->sizeof_partition_entry);
}

static void gpt_entry_arr_to_cpu(struct exfat_gpt_entry *e)
{
	e->starting_lba = le64_to_cpu(e->starting_lba);
	e->ending_lba = le64_to_cpu(e->ending_lba);
}

static void gpt_header_to_cpu(struct exfat_gpt_header *h)
{
	h->signature = le64_to_cpu(h->signature);
	h->revision = le32_to_cpu(h->revision);
	h->header_size = le32_to_cpu(h->header_size);
	h->my_lba = le64_to_cpu(h->my_lba);
	h->alternate_lba = le64_to_cpu(h->alternate_lba);
	h->first_usable_lba = le64_to_cpu(h->first_usable_lba);
	h->last_usable_lba = le64_to_cpu(h->last_usable_lba);
	h->partition_entry_lba = le64_to_cpu(h->partition_entry_lba);
	h->num_partition_entries = le32_to_cpu(h->num_partition_entries);
	h->sizeof_partition_entry = le32_to_cpu(h->sizeof_partition_entry);
}

/*
 * Here's the deal:
 *
 *  1. Author GPT structures in memory
 *  2. Check if there's enough sectors for GPT. Whether there would be enough
 *     space for the exFAT volume after the fact should be handled elsewhere,
 *     not here
 *  3. Let her rip: write the data
 *  4. Adjust parameters so that the exFAT creation process takes place in the
 *     new GPT partition
 *
 * Rollback:
 *
 * In case of exFAT creation failure, the caller SHOULD wipe out the regions of
 * device the GPT structures have already been written to. If the underlying
 * device or the file system supports TRIM or sparse extents, the block may end
 * up being allocated. Hopefully, the underlying  device or the VFS is smart
 * enough to not actually allocate blocks for all-zero data which is almost
 * always the case for SSDs and SMR HDDs. Corner cases would be not-so-smart
 * SD cards, though.
 */
static int build_gpt(struct exfat_blk_dev *bd, const struct exfat_user_input *ui)
{
	const unsigned long long sector = bd->sector_size;
	int ret = 0;
	unsigned long long req_size = 0;
	unsigned long long first_usable, first_aligned, last_usable, last_aligned, part_sec_cnt;
	uint16_t serial;
	uint8_t *mbr = calloc(1, sector);
	struct exfat_gpt_header *head_main = calloc(1, bd->sector_size);
	struct exfat_gpt_header *head_backup = calloc(1, bd->sector_size);
	struct exfat_gpt_entry *arr = calloc(EXFAT_GPT_ENTRY_SIZE, EXFAT_GPT_ENTRY_CNT);
	struct exfat_mkfs_data_region regions[EXFAT_GPT_DATA_REGION_CNT] = {
		{
			/* LBA 0: Protective MBR */
			.ofs = sector * 0,
			.len = sector,
			.data = mbr,
		},
		{
			/* LBA 1: GPT header */
			.ofs = sector * 1,
			.len = sector,
			.data = head_main,
		},
		{
			/* LBA 2+: GPT partition entry array */
			.ofs = sector * 2,
			.len = EXFAT_GPT_ENTRY_ARR_SIZE,
			.data = arr,
		},
		{
			/* ~ LBA n-1: Backup GPT partition entry array */
			.ofs = gpt_backup_array_byte_offset(bd, EXFAT_GPT_ENTRY_ARR_SIZE),
			.len = EXFAT_GPT_ENTRY_ARR_SIZE,
			.data = arr,
		},
		{
			/* LBA n: Backup GPT header */
			.ofs = bd->size - sector,
			.len = sector,
			.data = head_backup,
		},
	};

	/* runtime safety checks */

	assert(sizeof(struct exfat_gpt_header) <= sector);
	assert(sizeof(struct exfat_gpt_entry) % 128 == 0); /* Mandated by EFI spec */
	assert(sizeof(struct exfat_gpt_entry) <= EXFAT_GPT_ENTRY_SIZE);
	assert(EXFAT_GPT_ENTRY_ARR_SIZE >= 16384); /* Mandated by EFI spec */

	/* raise memory error */
	if (mbr == NULL || head_main == NULL || head_backup == NULL || arr == NULL)
		goto memerr;

	/*
	 * 1 MiB partition alignment is recommended by UEFI and Windows userland tools can only
	 * do 1 MiB partition bounds.
	 */
	if (EXFAT_GPT_MIN_PART_ALIGNMENT > ui->boundary_align) {
		exfat_err("Boundary unit of 1 MiB or larger is required for GPT\n");
		ret = -EINVAL;
		goto out;
	}
	/*
	 * Some weird and old SCSI and RAID devices might have optimal offset for I/O. If the
	 * start offset is not at a 1 MiB boundary, the device generally cannot hold GPT. This is
	 * extremely rare, but if it actually happens, the user should really do something about it
	 * and a file system util shouldn't force to make it work somehow.
	 */
	if (bd->alignment_offset % ui->boundary_align != 0) {
		exfat_err("Refusing to create GPT on target with device alignment offset(%llu)\n"
			  "not aligned to boundary unit.\n", bd->alignment_offset);
		ret = -EINVAL;
		goto out;
	}

	/* check if the device is big enough to hold the structure */

	for (size_t i = 0; i < EXFAT_GPT_DATA_REGION_CNT; i++)
		req_size += regions[i].len;
	if (req_size >= bd->size) {
		exfat_err("Target too small to hold GPT structures(cap: %llu, required: %llu)\n",
			  bd->size, req_size);
		ret = -ENOSPC;
		goto out;
	}

	/* pre-calc */

	first_usable = first_aligned = regions[EXFAT_GPT_DATA_REGION_M_ARR].ofs +
					regions[EXFAT_GPT_DATA_REGION_M_ARR].len;
	first_usable = round_up(first_usable, sector) / sector;
	first_aligned = round_up(first_aligned, ui->boundary_align) / sector;
	last_usable = last_aligned = regions[EXFAT_GPT_DATA_REGION_B_ARR].ofs - 1;
	last_usable = round_down(last_usable, sector) / sector;
	last_aligned = round_down(last_aligned, ui->boundary_align) / sector - 1;

	part_sec_cnt = last_aligned - first_aligned + 1;
	if (first_aligned > last_aligned || part_sec_cnt < MIN_NUM_SECTOR) {
		exfat_err("GPT alignment issue(first_aligned=%llu, last_aligned=%llu):\n"
			  "target might be too small(minimum number of sectors required: %d)\n",
			  first_aligned, last_aligned, (int)MIN_NUM_SECTOR);
		ret = -ENOSPC;
		goto out;
	}

	/* fill in the data */

	exfat_open_rnddev(); /* Some good entropy, please. */

	/* The first GPT entry */

	memcpy(&arr[0].partition_type_guid, EXFAT_GPT_PART_TYPE, 16);
	memcpy(&serial, &arr[0].partition_type_guid, 2);
	exfat_gen_guid(&arr[0].unique_partition_guid);
	arr[0].starting_lba = first_aligned;
	arr[0].ending_lba = last_aligned;

	/* Protective MBR */
	exfat_put_bootstrap_code(ui->bootcode_msg, mbr, 0);
	exfat_put_mbr_partition(bd, mbr, serial, 1, false, 0xEE, 0xFFFFFF);
	mbr[510] = 0x55;
	mbr[511] = 0xAA;

	exfat_print_guid(exfat_debug,
			 "GPT entry partition_type_guid  ",
			 arr[0].partition_type_guid.b);
	exfat_print_guid(exfat_debug,
			 "GPT entry unique_partition_guid",
			 arr[0].unique_partition_guid.b);
	exfat_debug(	 "GPT entry starting_lba         : %"PRIu64"\n", arr[0].starting_lba);
	exfat_debug(	 "GPT entry ending_lba           : %"PRIu64"\n", arr[0].ending_lba);

	for (size_t i = 0; i < EXFAT_GPT_ENTRY_CNT; i++)
		gpt_entry_arr_to_le(&arr[i]);

	/* main header  */

	head_main->signature = 0x5452415020494645;
	head_main->revision = 0x00010000;
	head_main->header_size = sizeof(*head_main);
	head_main->my_lba = 1;
	head_main->alternate_lba = bd->num_sectors - 1;
	head_main->first_usable_lba = first_usable;
	head_main->last_usable_lba = last_usable;
	exfat_gen_guid(&head_main->disk_guid);
	head_main->partition_entry_lba = regions[EXFAT_GPT_DATA_REGION_M_ARR].ofs / sector;
	head_main->num_partition_entries = EXFAT_GPT_ENTRY_CNT;
	head_main->sizeof_partition_entry = EXFAT_GPT_ENTRY_SIZE;
	head_main->partition_entry_array_crc32 = 0;
	head_main->partition_entry_array_crc32 = exfat_efi_crc32(arr, EXFAT_GPT_ENTRY_ARR_SIZE);
	exfat_print_gpt_header(exfat_debug, "main", head_main);
	exfat_debug("GPT header partition_entry_array_crc32: 0x%08"PRIx32"\n",
			head_main->partition_entry_array_crc32);
	head_main->partition_entry_array_crc32 =
			cpu_to_le32(head_main->partition_entry_array_crc32);

	/* backup header */

	*head_backup = *head_main;
	head_backup->my_lba = head_main->alternate_lba;
	head_backup->alternate_lba = head_main->my_lba;
	head_backup->partition_entry_lba = regions[EXFAT_GPT_DATA_REGION_B_ARR].ofs / sector;

	exfat_print_gpt_header(exfat_debug, "backup", head_backup);

	/* rest of endian conv and checksums */

	gpt_header_to_le(head_main);
	gpt_header_to_le(head_backup);
	head_main->header_crc32 = 0;
	head_main->header_crc32 = exfat_efi_crc32(head_main, sizeof(*head_main));
	head_backup->header_crc32 = 0;
	head_backup->header_crc32 = exfat_efi_crc32(head_backup, sizeof(*head_backup));
	exfat_debug("GPT main   header header_crc32: 0x%08"PRIx32"\n", head_main->header_crc32);
	exfat_debug("GPT backup header header_crc32: 0x%08"PRIx32"\n", head_backup->header_crc32);
	head_main->header_crc32 = cpu_to_le32(head_main->header_crc32);
	head_backup->header_crc32 = cpu_to_le32(head_backup->header_crc32);

	for (size_t i = 0; i < EXFAT_GPT_DATA_REGION_CNT; i++)
		exfat_debug("GPT region[%zu]: %llu, %llu\n", i,
				(unsigned long long)regions[i].ofs, (unsigned long long)regions[i].len);

	/*
	 * now that the checksum calculations are done, flip'em back to host
	 * endian so that they can be used later on
	 */

	for (size_t i = 0; i < EXFAT_GPT_ENTRY_CNT; i++)
		gpt_entry_arr_to_cpu(&arr[i]);
	gpt_header_to_cpu(head_main);
	gpt_header_to_cpu(head_backup);

	/* transfer ownership */

	memcpy(finfo.gpt.regions, regions, sizeof(regions));
	memset(regions, 0, sizeof(regions));
	finfo.gpt.mbr = mbr;
	finfo.gpt.head_main = head_main;
	finfo.gpt.head_backup = head_backup;
	finfo.gpt.arr = arr;
	mbr = NULL;
	head_main = head_backup = NULL;
	arr = NULL;
	goto out;
memerr:
	ret = -ENOMEM;
	exfat_err("Could not allocate GPT: out of memory\n");
out:
	free(mbr);
	free(head_main);
	free(head_backup);
	free(arr);

	return ret;
}

static void exfat_setup_boot_sector(struct pbr *ppbr,
		struct exfat_blk_dev *bd, struct exfat_user_input *ui)
{
	struct bpb64 *pbpb = &ppbr->bpb;
	struct bsx64 *pbsx = &ppbr->bsx;
	unsigned int i;

	/* Fill exfat BIOS parameter block */
	pbpb->jmp_boot[0] = 0xeb; /* jmp short, relative */
	pbpb->jmp_boot[1] = 0x76; /* by 118 (118 + 2 == BootCode ofs) */
	pbpb->jmp_boot[2] = 0x90; /* nop */
	memcpy(pbpb->oem_name, "EXFAT   ", 8);
	memset(pbpb->res_zero, 0, 53);

	/* Alignment guarantees */
	assert(finfo.target.start_ofs % bd->sector_size == 0);
	assert(finfo.fat_byte_off % bd->sector_size == 0);
	assert(finfo.fat_byte_len % bd->sector_size == 0);
	assert(finfo.clu_byte_off % bd->sector_size == 0);

	/* Fill exfat extend BIOS parameter block */
	pbsx->vol_offset = cpu_to_le64(finfo.target.start_ofs / bd->sector_size);
	pbsx->vol_length = cpu_to_le64(finfo.target.sec.len);
	pbsx->fat_offset = cpu_to_le32(finfo.fat_byte_off / bd->sector_size);
	pbsx->fat_length = cpu_to_le32(finfo.fat_byte_len / bd->sector_size);
	pbsx->clu_offset = cpu_to_le32(finfo.clu_byte_off / bd->sector_size);
	pbsx->clu_count = cpu_to_le32(finfo.total_clu_cnt);
	pbsx->root_cluster = cpu_to_le32(finfo.root_start_clu);
	pbsx->vol_serial = cpu_to_le32(finfo.volume_serial);
	pbsx->vol_flags = 0;
	pbsx->sect_size_bits = bd->sector_size_bits;
	pbsx->sect_per_clus_bits = 0;
	/* Compute base 2 logarithm of ui->cluster_size / bd->sector_size */
	for (i = ui->cluster_size / bd->sector_size; i > 1; i /= 2)
		pbsx->sect_per_clus_bits++;
	pbsx->num_fats = 1;
	/* fs_version[0] : minor and fs_version[1] : major */
	pbsx->fs_version[0] = 0;
	pbsx->fs_version[1] = 1;
	pbsx->phy_drv_no = 0x80;
	memset(pbsx->reserved2, 0, 7);

	memset(ppbr->boot_code, 0, 390);
	/* Offset to exFAT bootcode: 120 */
	/* Offset to MBR disk signature: 440 */
	/* Offset to 1st MBR partition entry: 446 */
	exfat_put_bootstrap_code(ui->bootcode_msg, ppbr->boot_code,
			(char *)&ppbr->boot_code - (char *)ppbr);
	if (ui->part_table == PART_TABLE_MBR)
		exfat_put_mbr_partition(bd, ppbr, finfo.volume_serial, 0, true,
					EXFAT_MBR_PART_TYPE, 0xFFFFFE);
	ppbr->signature = cpu_to_le16(PBR_SIGNATURE);

	exfat_debug("Volume Offset(sectors) : %" PRIu64 "\n",
		le64_to_cpu(pbsx->vol_offset));
	exfat_debug("Volume Length(sectors) : %" PRIu64 "\n",
		le64_to_cpu(pbsx->vol_length));
	exfat_debug("FAT Offset(sector offset) : %u\n",
		le32_to_cpu(pbsx->fat_offset));
	exfat_debug("FAT Length(sectors) : %u\n",
		le32_to_cpu(pbsx->fat_length));
	exfat_debug("Cluster Heap Offset (sector offset) : %u\n",
		le32_to_cpu(pbsx->clu_offset));
	exfat_debug("Cluster Count : %u\n",
		le32_to_cpu(pbsx->clu_count));
	exfat_debug("Root Cluster (cluster offset) : %u\n",
		le32_to_cpu(pbsx->root_cluster));
	exfat_debug("Volume Serial : 0x%x\n", le32_to_cpu(pbsx->vol_serial));
	exfat_debug("Sector Size Bits : %u\n",
		pbsx->sect_size_bits);
	exfat_debug("Sector per Cluster bits : %u\n",
		pbsx->sect_per_clus_bits);
}

static int exfat_write_boot_sector(struct exfat_blk_dev *bd,
		struct exfat_user_input *ui, unsigned int *checksum,
		bool is_backup)
{
	struct pbr *ppbr;
	unsigned long long sec_idx = finfo.target.sec.ofs + BOOT_SEC_IDX;
	int ret = 0;

	if (is_backup)
		sec_idx += BACKUP_BOOT_SEC_IDX;

	ppbr = malloc(bd->sector_size);
	if (!ppbr) {
		exfat_err("Cannot allocate pbr: out of memory\n");
		return -1;
	}
	memset(ppbr, 0, bd->sector_size);

	exfat_setup_boot_sector(ppbr, bd, ui);

	/* write main boot sector */
	ret = exfat_write_sector(bd, ppbr, sec_idx);
	if (ret < 0) {
		exfat_err("main boot sector write failed\n");
		ret = -1;
		goto free_ppbr;
	}

	if (ui->verify) {
		ret = exfat_check_written_data(bd,
				ppbr, bd->sector_size,
				sec_idx * bd->sector_size,
				"boot sector");
		if (ret) {
			exfat_err("boot sector verification failed (read-back mismatch)\n");
			goto free_ppbr;
		}
	}

	boot_calc_checksum((unsigned char *)ppbr, bd->sector_size,
		true, checksum);

free_ppbr:
	free(ppbr);
	return ret;
}

static int exfat_write_extended_boot_sectors(struct exfat_blk_dev *bd,
		struct exfat_user_input *ui, unsigned int *checksum,
		bool is_backup)
{
	char *peb;
	__le16 *peb_signature;
	int ret = 0;
	int i;
	unsigned long long sec_idx = finfo.target.sec.ofs + EXBOOT_SEC_IDX;

	peb = malloc(bd->sector_size);
	if (!peb) {
		exfat_err("Cannot allocate peb: out of memory\n");
		return -1;
	}

	if (is_backup)
		sec_idx += BACKUP_BOOT_SEC_IDX;

	memset(peb, 0, bd->sector_size);
	peb_signature = (__le16*) (peb + bd->sector_size - 2);
	*peb_signature = cpu_to_le16(PBR_SIGNATURE);
	for (i = 0; i < EXBOOT_SEC_NUM; i++) {
		if (exfat_write_sector(bd, peb, sec_idx++)) {
			exfat_err("extended boot sector write failed\n");
			ret = -1;
			goto free_peb;
		}

		if (ui->verify) {
			ret = exfat_check_written_data(bd,
					peb, bd->sector_size,
					(sec_idx - 1) * bd->sector_size,
					"extended boot sector");
			if (ret) {
				exfat_err("extended boot sector verification failed (read-back mismatch)\n");
				goto free_peb;
			}
		}

		boot_calc_checksum((unsigned char *) peb, bd->sector_size,
			false, checksum);
	}

free_peb:
	free(peb);
	return ret;
}

static int exfat_write_oem_sector(struct exfat_blk_dev *bd,
		struct exfat_user_input *ui, unsigned int *checksum,
		bool is_backup)
{
	char *oem;
	int ret = 0;
	unsigned long long sec_idx = finfo.target.sec.ofs + OEM_SEC_IDX;

	oem = calloc(1, bd->sector_size);
	if (!oem)
		return -1;

	if (is_backup)
		sec_idx += BACKUP_BOOT_SEC_IDX;

	ret = exfat_write_sector(bd, oem, sec_idx);
	if (ret) {
		exfat_err("oem sector write failed\n");
		ret = -1;
		goto free_oem;
	}

	if (ui->verify) {
		ret = exfat_check_written_data(bd,
				oem, bd->sector_size,
				sec_idx * bd->sector_size,
				"oem sector");
		if (ret) {
			exfat_err("oem sector verification failed (read-back mismatch)\n");
			goto free_oem;
		}
	}

	boot_calc_checksum((unsigned char *)oem, bd->sector_size, false,
		checksum);

	/* Reuse zeroed out oem sector for reserved sector */
	ret = exfat_write_sector(bd, oem, sec_idx + 1);
	if (ret) {
		exfat_err("reserved sector write failed\n");
		ret = -1;
		goto free_oem;
	}

	if (ui->verify) {
		ret = exfat_check_written_data(bd,
				oem, bd->sector_size,
				(sec_idx + 1) * bd->sector_size,
				"reserved sector");
		if (ret) {
			exfat_err("reserved sector verification failed (read-back mismatch)\n");
			goto free_oem;
		}
	}

	boot_calc_checksum((unsigned char *)oem, bd->sector_size, false,
		checksum);

free_oem:
	free(oem);
	return ret;
}

static int exfat_create_volume_boot_record(struct exfat_blk_dev *bd,
		struct exfat_user_input *ui, bool is_backup)
{
	unsigned int checksum = 0;
	int ret;

	ret = exfat_write_boot_sector(bd, ui, &checksum, is_backup);
	if (ret)
		return ret;
	ret = exfat_write_extended_boot_sectors(bd, ui, &checksum, is_backup);
	if (ret)
		return ret;
	ret = exfat_write_oem_sector(bd, ui, &checksum, is_backup);
	if (ret)
		return ret;

	return exfat_write_checksum_sector(bd, ui, finfo.target.sec.ofs,
					   checksum, is_backup);
}

static int write_fat_entry(struct exfat_user_input *ui, int fd,
		__le32 clu, unsigned long long offset)
{
	off_t fat_entry_offset = finfo.target.byte.ofs + finfo.fat_byte_off +
				(offset * sizeof(__le32));

	if (!exfat_write_full(fd, (__u8 *) &clu, sizeof(__le32), fat_entry_offset)) {
		exfat_err("write failed, offset : %llu, clu : %x\n",
			offset, clu);
		return -1;
	}

	if (ui->verify) {
		memcpy(ui->fat_table_buff + offset * sizeof(__le32),
			(__u8 *)&clu, sizeof(__le32));
	}

	return 0;
}

static int write_fat_entries(struct exfat_user_input *ui, int fd,
		unsigned int clu, unsigned int length)
{
	int ret;
	unsigned int count;

	count = clu + round_up(length, ui->cluster_size) / ui->cluster_size;

	for (; clu < count - 1; clu++) {
		ret = write_fat_entry(ui, fd, cpu_to_le32(clu + 1), clu);
		if (ret)
			return ret;
	}

	ret = write_fat_entry(ui, fd, cpu_to_le32(EXFAT_EOF_CLUSTER), clu);
	if (ret)
		return ret;

	return clu;
}

static int exfat_create_fat_table(struct exfat_blk_dev *bd,
		struct exfat_user_input *ui)
{
	int ret, clu;
	unsigned int fat_table_entries = 0;

	if (ui->verify) {
		fat_table_entries =
			EXFAT_FIRST_CLUSTER +
			DIV_ROUND_UP(finfo.bitmap_byte_len, ui->cluster_size) +
			DIV_ROUND_UP(finfo.ut_byte_len, ui->cluster_size) +
			DIV_ROUND_UP(finfo.root_byte_len, ui->cluster_size);

		ui->fat_table_buff = calloc(fat_table_entries, sizeof(__le32));
		if (!ui->fat_table_buff)
			return -ENOMEM;
	}

	/* fat entry 0 should be media type field(0xF8) */
	ret = write_fat_entry(ui, bd->dev_fd, cpu_to_le32(0xfffffff8), 0);
	if (ret) {
		exfat_err("fat 0 entry write failed\n");
		goto free_fat_table_buff;
	}

	/* fat entry 1 is historical precedence(0xFFFFFFFF) */
	ret = write_fat_entry(ui, bd->dev_fd, cpu_to_le32(0xffffffff), 1);
	if (ret) {
		exfat_err("fat 1 entry write failed\n");
		goto free_fat_table_buff;
	}

	/* write bitmap entries */
	clu = write_fat_entries(ui, bd->dev_fd, EXFAT_FIRST_CLUSTER,
		finfo.bitmap_byte_len);
	if (clu < 0) {
		ret = clu;
		goto free_fat_table_buff;
	}

	/* write upcase table entries */
	clu = write_fat_entries(ui, bd->dev_fd, clu + 1, finfo.ut_byte_len);
	if (clu < 0) {
		ret = clu;
		goto free_fat_table_buff;
	}

	/* write root directory entries */
	clu = write_fat_entries(ui, bd->dev_fd, clu + 1, finfo.root_byte_len);
	if (clu < 0) {
		ret = clu;
		goto free_fat_table_buff;
	}

	finfo.used_clu_cnt = clu + 1 - EXFAT_FIRST_CLUSTER;
	exfat_debug("Total used cluster count : %d\n", finfo.used_clu_cnt);

	if (ui->verify) {
		ret = exfat_check_written_data(bd, ui->fat_table_buff,
				fat_table_entries * sizeof(__le32),
				finfo.target.byte.ofs + finfo.fat_byte_off,
				"fat table");
		if (ret)
			exfat_err("fat table verification failed (read-back mismatch)\n");
	}

free_fat_table_buff:
	if (ui->verify)
		free(ui->fat_table_buff);
	return ret;
}

static int exfat_create_bitmap(struct exfat_blk_dev *bd,
		struct exfat_user_input *ui)
{
	char *bitmap;
	unsigned int full_bytes, rem_bits;
	int ret = 0;
	bool mapped = false;

	bitmap = exfat_map_blankmem(finfo.bitmap_byte_len, &mapped);
	if (bitmap == NULL) {
		exfat_err("Cannot allocate bitmap: out of memory\n");
		return -1;
	}
#if defined(__linux__) || defined(__FreeBSD__)
	/*
	 * Demand paging off /dev/zero is extremely a Linux/FreeBSD thing.
	 * This code path is not tested at the time of writing because
	 * exfatprogs is only supported on Linux(might work on FreeBSD
	 * on top of the compat ABI mode).
	 */
	if (!mapped)
		exfat_info("mmap() for bitmap failed: errno=%d. Falling back to allocating memory.\n"
			   "Up to %u bytes of memory may be required.\n",
			   errno, finfo.bitmap_byte_len);
#endif

	full_bytes = finfo.used_clu_cnt / 8;
	rem_bits = finfo.used_clu_cnt % 8;

	memset(bitmap, 0xff, full_bytes);

	if (rem_bits != 0)
		bitmap[full_bytes] = (1 << rem_bits) - 1;

	if (!exfat_write_full(bd->dev_fd, bitmap, finfo.bitmap_byte_len,
			finfo.target.byte.ofs + finfo.bitmap_byte_off)) {
		exfat_err("write failed, bitmap_len : %d\n", finfo.bitmap_byte_len);
		ret = -1;
		goto out;
	}

	if (ui->verify) {
		ret = exfat_check_written_data(bd, bitmap, finfo.bitmap_byte_len,
				finfo.target.byte.ofs + finfo.bitmap_byte_off,
				"bitmap");
		if (ret) {
			exfat_err("bitmap verification failed (read-back mismatch)\n");
			goto out;
		}
	}

out:
	exfat_unmap_mm(bitmap, finfo.bitmap_byte_len, &mapped);
	return ret;
}

static int exfat_create_root_dir(struct exfat_blk_dev *bd,
		struct exfat_user_input *ui)
{
	struct exfat_dentry ed[4] = {0};
	int dentries_len = sizeof(ed);
	int ret;

	ret = exfat_write_zero2(bd->dev_fd, ui->cluster_size,
			finfo.target.byte.ofs + finfo.root_byte_off, ui->cluster_size);
        if (ret) {
                exfat_err("zero out write failed for root dir (errno : %d)\n",
				errno);
                return ret;
        }
	if (ui->verify) {
		ret = exfat_check_written_data(bd, NULL, ui->cluster_size,
				finfo.target.byte.ofs + finfo.root_byte_off,
				"zero out root dir");
		if (ret) {
			exfat_err("root dir zeroing out verification failed (read-back mismatch)\n");
			return ret;
		}
	}

	/* Set volume label entry */
	ed[0].type = EXFAT_VOLUME;
	memset(ed[0].vol_label, 0, 22);
	memcpy(ed[0].vol_label, ui->volume_label, ui->volume_label_len);
	ed[0].vol_char_cnt = ui->volume_label_len/2;

	/* Set volume GUID entry */
	if (ui->guid) {
		if (__exfat_set_volume_guid(&ed[1], ui->guid))
			return -1;
	} else {
		/*
		 * Since a single empty entry cannot be allocated for a
		 * file, this can reserve the entry for volume GUID.
		 */
		ed[1].type = EXFAT_GUID & ~EXFAT_INVAL;
	}

	/* Set bitmap entry */
	ed[2].type = EXFAT_BITMAP;
	ed[2].bitmap_flags = 0;
	ed[2].bitmap_start_clu = cpu_to_le32(EXFAT_FIRST_CLUSTER);
	ed[2].bitmap_size = cpu_to_le64(finfo.bitmap_byte_len);

	/* Set upcase table entry */
	if (finfo.ut_byte_len) {
		ed[3].type = EXFAT_UPCASE;
		ed[3].upcase_checksum = cpu_to_le32(finfo.ut_chksum);
		ed[3].upcase_start_clu = cpu_to_le32(finfo.ut_start_clu);
		ed[3].upcase_size = cpu_to_le64(finfo.ut_byte_len);
	}

	if (!exfat_write_full(bd->dev_fd, ed, dentries_len,
			finfo.target.byte.ofs + finfo.root_byte_off)) {
		exfat_err("write failed, dentries_len : %d\n", dentries_len);
		return -1;
	}

	if (ui->verify) {
		ret = exfat_check_written_data(bd, ed, dentries_len,
				finfo.target.byte.ofs + finfo.root_byte_off,
				"root directory");
		if (ret) {
			exfat_err("root directory verification failed (read-back mismatch)\n");
			return ret;
		}

	}
	return 0;
}

static void usage(void)
{
	fputs("Usage: mkfs.exfat\n"
		"\t-L | --volume-label=label                              Set volume label\n"
		"\t-U | --volume-guid=guid                                Set volume GUID\n"
		"\t-s | --sector-size=size(or suffixed by 'K')            Specify sector size\n"
		"\t-c | --cluster-size=size(or suffixed by 'K' or 'M')    Specify cluster size\n"
		"\t-b | --boundary-align=size(or suffixed by 'K' or 'M')  Specify boundary alignment\n"
		"\t     --pack-bitmap                                     Move bitmap into FAT segment\n"
		"\t     --upcase=file                                     Specify up-case table binary file\n"
		"\t     --bootcode-msg=message                            Specify custom message in MBR bootstrap code\n"
		"\t-P | --partition-table=auto|none|mbr|gpt               Specify partition table\n"
		"\t-f | --full-format                                     Full format\n"
		"\t-C | --check-written                                   Verify written filesystem metadata by read-back\n"
		"\t-K | --no-discard                                      Do not discard blocks\n"
		"\t-V | --version                                         Show version\n"
		"\t-q | --quiet                                           Print only errors\n"
		"\t-v | --verbose                                         Print debug\n"
		"\t-h | --help                                            Show help\n",
		stderr);

	exit(EXIT_FAILURE);
}

#define PACK_BITMAP	(CHAR_MAX + 1)
#define UPCASE_FILE	(CHAR_MAX + 2)
#define BOOTCODE_MSG	(CHAR_MAX + 3)

static const struct option opts[] = {
	{"volume-label",	required_argument,	NULL,	'L' },
	{"volume-guid",		required_argument,	NULL,	'U' },
	{"sector-size",		required_argument,	NULL,	's' },
	{"cluster-size",	required_argument,	NULL,	'c' },
	{"boundary-align",	required_argument,	NULL,	'b' },
	{"upcase",		required_argument,	NULL,	UPCASE_FILE },
	{"bootcode-msg",	required_argument,	NULL,	BOOTCODE_MSG },
	{"pack-bitmap",		no_argument,		NULL,	PACK_BITMAP },
	{"full-format",		no_argument,		NULL,	'f' },
	{"check-written",	no_argument,		NULL,	'C' },
	{"no-discard",		no_argument,		NULL,	'K' },
	{"partition-table",	no_argument,		NULL,	'P' },
	{"version",		no_argument,		NULL,	'V' },
	{"quiet",		no_argument,		NULL,	'q' },
	{"verbose",		no_argument,		NULL,	'v' },
	{"help",		no_argument,		NULL,	'h' },
	{"?",			no_argument,		NULL,	'?' },
	{NULL,			0,			NULL,	 0  }
};

/*
 * Moves the bitmap to just before the alignment boundary if there is space
 * between the boundary and the end of the FAT. This may allow the FAT and the
 * bitmap to share the same allocation unit on flash media, thereby improving
 * performance and endurance.
 */
static int exfat_pack_bitmap(const struct exfat_user_input *ui)
{
	unsigned long long fat_byte_end = finfo.fat_byte_off + finfo.fat_byte_len;
	unsigned int bitmap_byte_len = finfo.bitmap_byte_len,
		bitmap_clu_len = round_up(bitmap_byte_len, ui->cluster_size),
		bitmap_clu_cnt, total_clu_cnt, new_bitmap_clu_len;

	for (;;) {
		bitmap_clu_cnt = bitmap_clu_len / ui->cluster_size;
		if (finfo.clu_byte_off - bitmap_clu_len < fat_byte_end ||
				finfo.total_clu_cnt > EXFAT_MAX_NUM_CLUSTER -
					bitmap_clu_cnt)
			return -1;
		total_clu_cnt = finfo.total_clu_cnt + bitmap_clu_cnt;
		bitmap_byte_len = round_up(total_clu_cnt, 8) / 8;
		new_bitmap_clu_len = round_up(bitmap_byte_len, ui->cluster_size);
		if (new_bitmap_clu_len == bitmap_clu_len) {
			finfo.clu_byte_off -= bitmap_clu_len;
			finfo.total_clu_cnt = total_clu_cnt;
			finfo.bitmap_byte_off -= bitmap_clu_len;
			finfo.bitmap_byte_len = bitmap_byte_len;
			return 0;
		}
		bitmap_clu_len = new_bitmap_clu_len;
	}
}

#ifdef _POSIX_MAPPED_FILES
static void exfat_unmap_upcase_m(struct exfat_user_input *ui)
{
	munmap(ui->upcase.m, ui->upcase.len);
}
#endif

static void exfat_free_upcase_m(struct exfat_user_input *ui)
{
	free(ui->upcase.m);
}

static int exfat_load_upcase(struct exfat_user_input *ui)
{
	int fd, ret = 0;
	uint8_t *m = NULL;
	void *nm;
	off_t len;
	size_t size = 0;
	ssize_t rlen;

	if (!exfat_ui_has_upcase_file(ui)) {
		/* set defaults and return */
		ui->upcase.table = default_upcase_table;
		ui->upcase.len = EXFAT_UPCASE_TABLE_SIZE;
		return 0;
	}

	fd = open(ui->upcase.file, O_RDONLY);
	if (fd < 0) {
		ret = -errno;
		goto out;
	}

	len = lseek(fd, 0, SEEK_END);
	if (len == 0) {
		/* An empty file given */
		ui->upcase.table = NULL;
		ui->upcase.len = 0;
		goto out;
	}
	if (len > (off_t)EXFAT_MAX_UPCASE_TABLE_SIZE) {
		ret = -EFBIG;
		goto out;
	}

#ifdef _POSIX_MAPPED_FILES
	if (len > 0) {
		/*
		 * The file is seekable and the size of the file is known. Try mmap() to save some
		 * memory (a full upcase table binary file can range up to 131KB)
		 */
		m = mmap(NULL, (size_t)len, PROT_READ, MAP_SHARED, fd, 0);
		assert(m != NULL);
		if (m == MAP_FAILED)
			m = NULL;
		else {
			ui->upcase.table = ui->upcase.m = m;
			ui->upcase.len = (size_t)len;
			ui->upcase.free = exfat_unmap_upcase_m;
			goto out;
		}
	}
#endif
	/* Good-old I/O loop fallback */

	size = len > 0 ? (size_t)len : (EXFAT_MAX_UPCASE_TABLE_SIZE + 1);
	m = malloc(size);
	if (m == NULL)
		goto nomem;

	rlen = exfat_read(fd, m, size, 0);
	if (rlen < 0) {
		ret = -errno;
		goto free;
	} else if ((size_t)rlen > EXFAT_MAX_UPCASE_TABLE_SIZE) {
		ret = -EFBIG;
		goto free;
	}

	/* shrink to fit before returning, ignoring errors */
	if (rlen == 0) {
		free(m);
		m = NULL;
	} else if ((size_t)rlen != size) {
		nm = realloc(m, rlen);
		if (nm != NULL)
			m = nm;
	}

	ui->upcase.table = ui->upcase.m = m;
	ui->upcase.len = rlen;
	ui->upcase.free = exfat_free_upcase_m;
	goto out;

nomem:
	ret = -ENOMEM;
free:
	free(m);
out:
	if (fd >= 0)
		close(fd);
	if (ret < 0)
		exfat_err("%s: %s\n", ui->upcase.file, strerror(-ret));
	if (ret == 0) {
		if (ui->upcase.len == 0)
			exfat_info("!!! an empty upcase table(0 bytes) is loaded !!!\n"
				   "!!! Implementations will treat the volume as damaged !!!\n");
		else
			exfat_info("!!! --upcase option is used !!!\n"
				   "!!! mkfs.exfat does not check the validity of the file !!!\n"
				   "!!! Use at your own risk !!!\n");
	}

	return ret;
}

static void exfat_free_upcase(struct exfat_user_input *ui)
{
	if (ui->upcase.free)
		ui->upcase.free(ui);
	ui->upcase.table = NULL;
	ui->upcase.m = NULL;
	ui->upcase.free = NULL;
	ui->upcase.len = 0;
}

static int exfat_build_mkfs_info(struct exfat_blk_dev *bd, struct exfat_user_input *ui,
		const unsigned long long sec_ofs, const unsigned long long sec_len,
		const unsigned long long start_ofs)
{
	unsigned long long total_clu_cnt;
	unsigned long long max_clusters;
	int clu_len;
	int num_fats = 1;

	finfo.target.sec.ofs = sec_ofs;
	finfo.target.sec.len = sec_len;
	finfo.target.byte.ofs = sec_ofs * bd->sector_size;
	finfo.target.byte.len = sec_len * bd->sector_size;
	finfo.target.start_ofs = start_ofs;

	if (ui->cluster_size < bd->sector_size) {
		exfat_err("cluster size (%u bytes) is smaller than sector size (%u bytes)\n",
			  ui->cluster_size, bd->sector_size);
		return -1;
	}
	if (ui->boundary_align < bd->sector_size) {
		exfat_err("boundary alignment is too small (min %d)\n",
				bd->sector_size);
		return -1;
	}

	finfo.fat_byte_off = round_up(start_ofs + 24 * bd->sector_size,
			ui->boundary_align) - start_ofs;
	max_clusters = (finfo.target.byte.len - finfo.fat_byte_off - 8 * num_fats - 1) /
		(ui->cluster_size + 4 * num_fats) + 1;
	finfo.fat_byte_len = round_up((max_clusters + 2) * 4,
			(unsigned long long)bd->sector_size);
	/* Prevent integer overflow when computing the FAT length */
	if (finfo.fat_byte_len / bd->sector_size > UINT_MAX) {
		exfat_err("cluster size (%u bytes) is too small\n", ui->cluster_size);
		return -1;
	}
	finfo.clu_byte_off = round_up(start_ofs + finfo.fat_byte_off +
			finfo.fat_byte_len * num_fats,
				(unsigned long long)ui->boundary_align) - start_ofs;
	if (finfo.target.byte.len <= finfo.clu_byte_off) {
		exfat_err("boundary alignment is too big\n");
		return -1;
	}
	total_clu_cnt = (finfo.target.byte.len - finfo.clu_byte_off) / ui->cluster_size;
	if (total_clu_cnt > EXFAT_MAX_NUM_CLUSTER) {
		exfat_err("cluster size is too small\n");
		return -1;
	}
	finfo.total_clu_cnt = (unsigned int) total_clu_cnt;

	finfo.bitmap_byte_off = finfo.clu_byte_off;
	finfo.bitmap_byte_len = round_up(finfo.total_clu_cnt, 8) / 8;
	if (ui->pack_bitmap)
		exfat_pack_bitmap(ui);
	clu_len = round_up(finfo.bitmap_byte_len,
			(unsigned long long)ui->cluster_size);

	finfo.ut_start_clu = EXFAT_FIRST_CLUSTER + clu_len / ui->cluster_size;
	finfo.ut_byte_off = finfo.bitmap_byte_off + clu_len;
	finfo.ut_byte_len = ui->upcase.len;
	boot_calc_checksum(ui->upcase.table, ui->upcase.len, false, &finfo.ut_chksum);
	if (ui->upcase.table == default_upcase_table)
		assert(finfo.ut_chksum == EXFAT_UPCASE_TABLE_CHKSUM &&
			finfo.ut_byte_len == EXFAT_UPCASE_TABLE_SIZE);
	clu_len = round_up(finfo.ut_byte_len,
			(unsigned long long)ui->cluster_size);

	finfo.root_start_clu = finfo.ut_start_clu + clu_len / ui->cluster_size;
	finfo.root_byte_off = finfo.ut_byte_off + clu_len;
	finfo.root_byte_len = sizeof(struct exfat_dentry) * 3;
	finfo.volume_serial = get_new_serial();

	return 0;
}

static int exfat_zero_out_disk(struct exfat_blk_dev *bd,
		struct exfat_user_input *ui)
{
	int ret = 0;
	bool mapped = false;
	const void *zm = NULL;
	const size_t iosize = ui->cluster_size;
	unsigned long long target_zerolen;

	assert(iosize > 0);

	if (ui->quick)
		target_zerolen = MIN(bd->size, EXFAT_HEAD_ZERO_OUT);
	else
		target_zerolen = bd->size;

	exfat_info("Zeroing out %llu bytes: ", target_zerolen);
	fflush(stdout);

	ret = exfat_write_zero2(bd->dev_fd, target_zerolen, 0, iosize);
	if (ret)
		goto out;

	if (ui->verify) {
		off_t ofs = 0, rem = target_zerolen;

		zm = exfat_map_zeromem(iosize, &mapped);
		if (zm == NULL)
			goto out;

		while (rem > 0) {
			const off_t vs = MIN((off_t)iosize, rem);

			ret = exfat_check_written_data(bd, zm, (size_t)vs, ofs, "zero out");
			if (ret) {
				exfat_err("disk zeroing out verification failed (read-back mismatch)\n");
				goto out;
			}

			ofs += vs;
			rem -= vs;
		}
	}

out:
	exfat_unmap_mm(zm, iosize, &mapped);

	if (ret)
		exfat_err("write failed(errno : %d)\n", errno);
	else {
		exfat_info("done\n");
		exfat_debug("zero out written size : %llu\n", target_zerolen);
	}

	return ret;
}

static void exfat_discard_dev(struct exfat_blk_dev *bd,
		struct exfat_user_input *ui)
{
	uint64_t offset = 0;
	uint64_t tmp_step;
	int err;
	/* Discard the device 2G at a time */
	const uint64_t step = 2ULL << 30;
	const uint64_t count = bd->num_sectors * bd->sector_size;

	if (!ui->discard || !bd->isblk) {
		exfat_debug("no-discard requested or the device is a file\n");
		return;
	}

	/*
	 * The block discarding happens in smaller batches so it can be
	 * interrupted prematurely
	 */
	while (offset < count) {
		tmp_step = count - offset;
		if (step < tmp_step)
			tmp_step = step;

		err = exfat_discard_blocks(bd->dev_fd, offset, tmp_step);
		/*
		 * We intentionally ignore errors from the discard ioctl. It is
		 * not necessary for the mkfs functionality but just an
		 * optimization. However we should stop on error.
		 */
		if (err == 0) {
			if (offset == 0) {
				exfat_info("Discarding blocks: ");
				exfat_debug("BLKDISCARD: ");
			}
			exfat_debug("%"PRIu64"-%"PRIu64" ", offset, offset + tmp_step);
			fflush(stdout);
		} else {
			exfat_debug("BLKDISCARD: %s\n", strerror(err));
			if (offset > 0)
				exfat_info("\n");
			return;
		}

		offset += tmp_step;
	}
	if (offset > 0)
		exfat_info("done\n");
}

static int make_exfat(struct exfat_blk_dev *bd, struct exfat_user_input *ui)
{
	int ret;

	exfat_info("Creating exFAT filesystem(%s, cluster size=%u)\n\n",
		ui->dev_name, ui->cluster_size);

	exfat_info("Writing volume boot record: ");
	ret = exfat_create_volume_boot_record(bd, ui, false);
	exfat_info("%s\n", ret ? "failed" : "done");
	if (ret)
		return ret;

	exfat_info("Writing backup volume boot record: ");
	/* backup sector */
	ret = exfat_create_volume_boot_record(bd, ui, true);
	exfat_info("%s\n", ret ? "failed" : "done");
	if (ret)
		return ret;

	exfat_info("Fat table creation: ");
	ret = exfat_create_fat_table(bd, ui);
	exfat_info("%s\n", ret ? "failed" : "done");
	if (ret)
		return ret;

	exfat_info("Allocation bitmap creation: ");
	ret = exfat_create_bitmap(bd, ui);
	exfat_info("%s\n", ret ? "failed" : "done");
	if (ret)
		return ret;

	exfat_info("Upcase table creation: ");
	ret = exfat_create_upcase_table(bd, ui);
	exfat_info("%s\n", ret ? "failed" : "done");
	if (ret)
		return ret;

	exfat_info("Writing root directory entry: ");
	ret = exfat_create_root_dir(bd, ui);
	exfat_info("%s\n", ret ? "failed" : "done");
	if (ret)
		return ret;

	/* dump finfo */

	exfat_debug("Target sector offset:         %llu\n",      finfo.target.sec.ofs);
	exfat_debug("Target sector length:         %llu\n",      finfo.target.sec.len);
	exfat_debug("Target byte offset:           %llu\n",      finfo.target.byte.ofs);
	exfat_debug("Target byte length:           %llu\n",      finfo.target.byte.len);
	exfat_debug("Number of all clusters:       %u\n",        finfo.total_clu_cnt);
	exfat_debug("Number of used clusters:      %u\n",        finfo.used_clu_cnt);
	exfat_debug("FAT byte offset:              %llu\n",      finfo.fat_byte_off);
	exfat_debug("FAT byte length:              %llu\n",      finfo.fat_byte_len);
	exfat_debug("Cluster Heap byte offset:     %llu\n",      finfo.clu_byte_off);
	exfat_debug("Bitmap byte offset:           %llu\n",      finfo.bitmap_byte_off);
	exfat_debug("Bitmap byte length:           %u\n",        finfo.bitmap_byte_len);
	exfat_debug("Upcase table byte offset:     %llu\n",      finfo.ut_byte_off);
	exfat_debug("Upcase table byte length:     %u\n",        finfo.ut_byte_len);
	exfat_debug("Upcase table start cluster:   %u\n",        finfo.ut_start_clu);
	exfat_debug("Upcase table checksum:        0x%08X\n",    finfo.ut_chksum);
	exfat_debug("Root directory byte offset:   %llu\n",      finfo.root_byte_off);
	exfat_debug("Root directory byte length:   %u\n",        finfo.root_byte_len);
	exfat_debug("Root directory start cluster: %u\n",        finfo.root_start_clu);
	exfat_debug("Volume serial:                %04X-%04X\n",
			(finfo.volume_serial & 0xFFFF0000) >> 16,
			(finfo.volume_serial & 0x0000FFFF));

	return 0;
}

static long long parse_size(const char *size)
{
	int saved_errno;
	char *data_unit;
	unsigned long long byte_size;

	saved_errno = errno;
	errno = 0;
	byte_size = strtoull(size, &data_unit, 0);
	if (errno != 0)
		goto err;
	errno = saved_errno;

	switch (*data_unit) {
	case 'M':
	case 'm':
		if (byte_size >> 20) {
			/* overflow */
			errno = EOVERFLOW;
			goto err;
		}
		byte_size <<= 20;
		break;
	case 'K':
	case 'k':
		if (byte_size >> 10) {
			/* overflow */
			errno = EOVERFLOW;
			goto err;
		}
		byte_size <<= 10;
		break;
	case '\0':
		break;
	default:
		exfat_err("Wrong unit input('%c') for size\n",
				*data_unit);
		return -EINVAL;
	}

	return byte_size;
err:
	saved_errno = errno;
	exfat_err("Invalid size: %s(%s)\n", size, strerror(saved_errno));
	return -saved_errno;
}

int main(int argc, char *argv[])
{
	int c;
	int ret = EXIT_FAILURE;
	struct exfat_blk_dev bd = { 0, };
	struct exfat_user_input ui;
	bool version_only = false;
	bool quiet = false;
	bool gptwo_tried = false; /* Set if GPT structure write out has been tried */

	init_user_input(&ui);

	if (!setlocale(LC_CTYPE, ""))
		exfat_err("failed to init locale/codeset\n");

	opterr = 0;
	while ((c = getopt_long(argc, argv, "n:L:U:s:c:b:P:fCKVqvh", opts, NULL)) != EOF)
		switch (c) {
		/*
		 * Make 'n' option fallthrough to 'L' option for for backward
		 * compatibility with old utils.
		 */
		case 'n':
		case 'L':
		{
			ret = exfat_utf16_enc(optarg,
				ui.volume_label, sizeof(ui.volume_label));
			if (ret < 0)
				goto out;

			ui.volume_label_len = ret;
			break;
		}
		case 'U':
			if (*optarg != '\0' && *optarg != '\r')
				ui.guid = optarg;
			break;
		case 's':
			ret = parse_size(optarg);
			if (ret < 0)
				goto out;
			else if (ret & (ret - 1)) {
				exfat_err("sector size(%d) is not a power of 2\n",
					ret);
				goto out;
			} else if ((ret & 0x1e00) == 0) {
				exfat_err("sector size(%d) must be 512, 1024, "
					"2048 or 4096 bytes\n",
					ret);
				goto out;
			}
			ui.sector_size = ret;
			break;
		case 'c':
			ret = parse_size(optarg);
			if (ret < 0)
				goto out;
			else if (ret == 0 || (ret & (ret - 1))) {
				exfat_err("cluster size(%d) is not a power of 2\n", ret);
				goto out;
			} else if (ret > EXFAT_MAX_CLUSTER_SIZE) {
				exfat_err("cluster size(%d) exceeds max cluster size(%d)\n",
					ui.cluster_size, EXFAT_MAX_CLUSTER_SIZE);
				goto out;
			}
			ui.cluster_size = ret;
			break;
		case 'b':
			ret = parse_size(optarg);
			if (ret < 0)
				goto out;
			else if (ret == 0 || ret & (ret - 1)) {
				exfat_err("boundary align(%d) is not a power of 2\n",
					ret);
				goto out;
			}
			ui.boundary_align = ret;
			break;
		case UPCASE_FILE:
			ui.upcase.file = optarg;
			break;
		case PACK_BITMAP:
			ui.pack_bitmap = true;
			break;
		case BOOTCODE_MSG:
			ui.bootcode_msg = optarg;
			/* A long message will just get truncated silently. */
			break;
		case 'f':
			ui.quick = false;
			break;
		case 'C':
			ui.verify = true;
			break;
		case 'K':
			ui.discard = false;
			break;
		case 'P':
			if (strcmp(optarg, "auto") == 0)
				ui.part_table = PART_TABLE_AUTO;
			else if (strcmp(optarg, "none") == 0)
				ui.part_table = PART_TABLE_NONE;
			else if (strcmp(optarg, "mbr") == 0)
				ui.part_table = PART_TABLE_MBR;
			else if (strcmp(optarg, "gpt") == 0)
				ui.part_table = PART_TABLE_GPT;
			else {
				exfat_err("not a valid partition table type: %s\n", optarg);
				ret = -EINVAL;
				goto out;
			}
			break;
		case 'V':
			version_only = true;
			break;
		case 'q':
			print_level = EXFAT_ERROR;
			quiet = true;
			break;
		case 'v':
			print_level = EXFAT_DEBUG;
			break;
		case '?':
		case 'h':
		default:
			usage();
	}

	if (version_only) {
		show_version();
		exit(EXIT_FAILURE);
	} else if (!quiet) {
		show_version();
	}

	if (argc - optind != 1) {
		usage();
	}

	if (ui.sector_size && ui.cluster_size && ui.sector_size > ui.cluster_size) {
		exfat_err("cluster size (%u bytes) is smaller than sector size (%u bytes)\n",
				  ui.cluster_size, ui.sector_size);
		ret = -1;
		goto out;
	}

	ui.dev_name = argv[optind];

	exfat_open_fd_devzero();

	ret = exfat_load_upcase(&ui);
	if (ret < 0)
		goto out;

	ret = exfat_get_blk_dev_info(&ui, &bd);
	if (ret < 0)
		goto out;
	if (!quiet && ui.sector_size) {
		exfat_info(
			"!!! Use -s option only if you know what you're doing !!!\n"
			"There are legitimate use cases for this option, but you really should be fixing\n"
			"the underlying hardware issue rather than overriding the sector size to force\n"
			"things to work!\n"
		);

		if (bd.dev_sector_size && bd.dev_sector_size != ui.sector_size) {
			/*
			 * The user supplied the sector size(-b option) and it doesn't match up with
			 * that of the device the kernel reported.
			 *
			 * Linux kernel exFAT is able to handle it by increasing the logical sector
			 * size of the blockdev, but other implementations are not as sophisticated.
			 */
			exfat_info("!!! Sector size mismatch: requested=%u, dev=%u !!!\n",
				   ui.sector_size, bd.dev_sector_size);
			exfat_info("!!! The volume will be unusable in many operating systems !!!\n");
		}
	}

	ret = exfat_select_part_type(&bd, &ui.part_table, quiet);
	if (ret)
		goto close;
	/* Whether we should create a partition table or not MUST be decided after this point. */
	assert(ui.part_table == PART_TABLE_NONE ||
		ui.part_table == PART_TABLE_MBR ||
		ui.part_table == PART_TABLE_GPT);
	exfat_info("Partition table: %s\n", exfat_part_table_type_str[ui.part_table]);

	if (ui.part_table == PART_TABLE_GPT) {
		ret = build_gpt(&bd, &ui);
		if (ret)
			goto close;

		/* build the info based on the new GPT partition */
		ret = exfat_build_mkfs_info(&bd, &ui,
				finfo.gpt.arr[0].starting_lba,
				finfo.gpt.arr[0].ending_lba - finfo.gpt.arr[0].starting_lba + 1,
				finfo.gpt.arr[0].starting_lba * bd.sector_size);
	} else
		/* business as usual: use the entire target */
		ret = exfat_build_mkfs_info(&bd, &ui, 0, bd.num_sectors, bd.offset);

	if (ret)
		goto close;

	exfat_discard_dev(&bd, &ui);
	/*
	 * Zeroing out still needs to be conducted as per JESD84-B51 6.6.9:
	 * "content of an explicitly erased memory range shall be ‘0’ or ‘1’
	 * depending on different memory technology,"
	 */
	ret = exfat_zero_out_disk(&bd, &ui);
	if (ret)
		goto close;

	if (ui.part_table == PART_TABLE_GPT) {
		gptwo_tried = true;

		for (size_t i = 0; i < EXFAT_GPT_ENTRY_CNT; i++)
			gpt_entry_arr_to_le(&finfo.gpt.arr[i]);
		gpt_header_to_le(finfo.gpt.head_main);
		gpt_header_to_le(finfo.gpt.head_backup);

		if (!exfat_write_mkfs_data_regions(&bd, finfo.gpt.regions) || fsync(bd.dev_fd)) {
			exfat_err("Failed to write GPT regions: %s\n", strerror(errno));
			ret = -1;
			goto out;
		}
		if (ui.verify && !exfat_verify_mkfs_data_regions(&bd, finfo.gpt.regions)) {
			exfat_err("Failed to verify GPT regions\n");
			ret = -1;
			goto out;
		}
	}

	ret = make_exfat(&bd, &ui);
	if (ret)
		goto close;

	if (!quiet) {
		exfat_info("Filesystem UUID: %04X-%04X\n",
			   (finfo.volume_serial & 0xFFFF0000) >> 16,
			   (finfo.volume_serial & 0x0000FFFF));
		if (gptwo_tried) {
			exfat_print_guid(exfat_info, "GPT disk GUID",
					 finfo.gpt.head_main->disk_guid.b);
			exfat_print_guid(exfat_info, "GPT exFAT partition GUID",
					 finfo.gpt.arr[0].unique_partition_guid.b);
		}
	}

	exfat_info("Synchronizing...\n");
	ret = fsync(bd.dev_fd);
close:
	close(bd.dev_fd);
out:
	if (ret && gptwo_tried) {
		exfat_info("Wiping out GPT structures...\n");

		exfat_zero_mkfs_data_regions(finfo.gpt.regions);

		if (!exfat_write_mkfs_data_regions(&bd, finfo.gpt.regions) || fsync(bd.dev_fd))
			exfat_err("Failed to wipe out GPT structures: %s\n", strerror(errno));
	}

	free(finfo.gpt.mbr);
	free(finfo.gpt.head_main);
	free(finfo.gpt.head_backup);
	free(finfo.gpt.arr);

	exfat_free_upcase(&ui);
	exfat_close_fd_devzero();
	exfat_close_rnddev();

	if (!ret)
		exfat_info("\nexFAT format complete!\n");
	else
		exfat_err("\nexFAT format fail!\n");
	return ret ? EXIT_FAILURE : EXIT_SUCCESS;
}

void exfat_zero_mkfs_data_regions(struct exfat_mkfs_data_region *r)
{
	for (size_t i = 0; i < EXFAT_GPT_DATA_REGION_CNT; i++)
		memset(r[i].data, 0, r[i].len);
}

bool exfat_write_mkfs_data_regions(struct exfat_blk_dev *bd, const struct exfat_mkfs_data_region *r)
{
	for (size_t i = 0; i < EXFAT_GPT_DATA_REGION_CNT; i++)
		if (!exfat_write_full(bd->dev_fd, r[i].data, r[i].len, r[i].ofs))
			return false;

	return true;
}

bool exfat_verify_mkfs_data_regions(struct exfat_blk_dev *bd,
		const struct exfat_mkfs_data_region *r)
{
	for (size_t i = 0; i < EXFAT_GPT_DATA_REGION_CNT; i++)
		if (exfat_check_written_data(bd, r[i].data, r[i].len, r[i].ofs, "GPT region"))
			return false;

	return true;
}

bool exfat_open_rnddev(void)
{
	if (fd_rnddev >= 0)
		return 0;

	fd_rnddev = open("/dev/urandom", O_RDONLY);
	if (fd_rnddev < 0)
		return -1;

	return 0;
}

void exfat_close_rnddev(void)
{
	if (fd_rnddev < 0)
		return;

	close(fd_rnddev);
	fd_rnddev = -1;
}

/*
 * In the absence of /dev/urandom, try our best to get entropy "good enough" by
 * falling back to LCG with seeds from basic syscalls available to any UNIX
 * systems.
 *
 * As GUID/UUID is quite large(16 bytes), generating UUID off PRNG can lead to
 * possibility of fingerprinting if we're not careful. Simple timestamp based
 * generation alone cannot be relied upon.
 *
 * https://en.wikipedia.org/wiki/Linear_congruential_generator#Parameters_in_common_use
 */
void exfat_gen_guid(void *out)
{
	static const uint64_t A = 6364136223846793005, C = 1;
	union {
		struct {
			uint64_t a, b;
		} n;
		struct {
			uint32_t a;
			uint16_t b;
			uint8_t ver[2];
			uint16_t c;
			uint32_t d;
		} parts;
	} __attribute__((__packed__)) rnd = { 0, };
	struct timespec ts[2] = { 0, };
	uint8_t zm[16] = { 0, };

	/* static_assert(sizeof(rnd) == 16); */

	for (int i = 0; i < 10; i++) {
		if (fd_rnddev >= 0) {
			volatile ssize_t rsize = read(fd_rnddev, &rnd, 16);
			/* I know what I'm doing, compiler. */
			(void)rsize;
		}

		clock_gettime(CLOCK_REALTIME, ts + 0);
		clock_gettime(CLOCK_MONOTONIC, ts + 1);

		rnd.n.a ^= (uint64_t)getpid() * A + C;
		rnd.n.b ^= (uint64_t)getpgrp() * A + C;
		rnd.n.a ^= (uint64_t)ts[0].tv_sec * A + C;
		rnd.n.a ^= (uint64_t)ts[0].tv_nsec * A + C;
		rnd.n.b ^= (uint64_t)ts[1].tv_sec * A + C;
		rnd.n.b ^= (uint64_t)ts[1].tv_nsec * A + C;

		/* There's no way in hell that all the entropy source above can fail */

		if (memcmp(&rnd, zm, 16) == 0)
			continue;

		rnd.parts.ver[0] &= 0xF0;
		rnd.parts.ver[0] |= 0x04;
		memcpy(out, &rnd, 16);
		return;
	}

	abort();
}
