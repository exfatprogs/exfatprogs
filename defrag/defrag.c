// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * exFAT Defragmentation Tool - defrag.exfat
 * Usage: defrag.exfat [-a] /dev/sdX
 * Effect: defragment or assess the entire exFAT-formated device
 * Feature: support safe interruption via Ctrl+C
 * Copyright (C) 2025 Haodong Xia <3394797836@qq.com>
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <getopt.h>
#include <locale.h>
#include <sys/wait.h>

#include "defrag.h"
#include "exfat_ondisk.h"
#include "libexfat.h"
#include "exfat_dir.h"
#include "exfat_fs.h"

/*----------------------- Part 0: Signal Handling ---------------------*/

/* Global interrupt flag */
static volatile sig_atomic_t interrupt_received;

/* Index at which interruption occurred */
static uint32_t interrupt_point;

/* Signal handler for SIGINT/SIGTERM */
static void sigint_handler(int sig)
{
	(void)sig;			/* Suppress unused parameter warning */
	interrupt_received = 1;		/* Mark that an interrupt has occurred */
}

/*------------ Part 1: FAT, Bitmap, and Boot Checksum I/O -------------*/

/*
 * Read or write the FAT table.
 * Returns 0 on success.
 */
static int exfat_rw_FAT(struct exfat *exfat, uint32_t *list, uint32_t list_len, bool read)
{
	off_t offset;
	size_t len_1, len_2;
	uint32_t i;

	offset = (off_t)le32_to_cpu(exfat->bs->bsx.fat_offset) << exfat->bs->bsx.sect_size_bits;
	len_1 = (size_t)(list_len * sizeof(uint32_t));

	/* Read or write with endianness conversion */
	if (read) {
		len_2 = exfat_read(exfat->blk_dev->dev_fd, list, len_1, offset);
		if (len_2 != len_1)
			return -EIO;
		for (i = 0; i < list_len; i++)
			list[i] = le32_to_cpu(list[i]);
	} else {
		for (i = 0; i < list_len; i++)
			list[i] = cpu_to_le32(list[i]);
		len_2 = exfat_write(exfat->blk_dev->dev_fd, list, len_1, offset);
		if (len_2 != len_1)
			return -EIO;
	}

	return 0;
}

/*
 * Read or write the allocation bitmap.
 * Returns 0 on success.
 */
static int exfat_rw_bitmap(struct exfat *exfat, bool read)
{
	ssize_t rw_len = 0;

	struct exfat_dentry *dentry;
	struct exfat_lookup_filter filter = {
		.in.type		= EXFAT_BITMAP,
		.in.dentry_count	= 0,
		.in.filter		= NULL,
		.in.param		= NULL,
	};

	/* Locate the bitmap dentry in the root directory */
	int retval = exfat_lookup_dentry_set(exfat, exfat->root, &filter);

	if (retval != 0)
		return retval;

	/* Validate bitmap size and starting cluster */
	dentry = filter.out.dentry_set;
	if (le64_to_cpu(dentry->bitmap_size) < DIV_ROUND_UP(exfat->clus_count, 8)) {
		exfat_err("invalid size of alloc_bitmap. 0x%" PRIx64 "\n",
			  le64_to_cpu(dentry->bitmap_size));
		return -EINVAL;
	}
	if (!exfat_heap_clus(exfat, le32_to_cpu(dentry->bitmap_start_clu))) {
		exfat_err("invalid start cluster of alloc_bitmap. 0x%x\n",
			  le32_to_cpu(dentry->bitmap_start_clu));
		return -EINVAL;
	}

	/* Record bitmap cluster and size */
	exfat->disk_bitmap_clus = le32_to_cpu(dentry->bitmap_start_clu);
	exfat->disk_bitmap_size = DIV_ROUND_UP(exfat->clus_count, 8);

	free(filter.out.dentry_set);

	/* Perform read or write operation */
	if (read) {
		rw_len = exfat_read(exfat->blk_dev->dev_fd, exfat->alloc_bitmap,
				    exfat->disk_bitmap_size,
				    exfat_c2o(exfat, exfat->disk_bitmap_clus));
		if (rw_len != exfat->disk_bitmap_size)
			return -EIO;
	} else {
		rw_len = exfat_write(exfat->blk_dev->dev_fd, exfat->alloc_bitmap,
				     exfat->disk_bitmap_size,
				     exfat_c2o(exfat, exfat->disk_bitmap_clus));
		if (rw_len != exfat->disk_bitmap_size)
			return -EIO;
	}

	return 0;
}

/*
 * From libexfat.c
 * Updates the boot sector checksum.
 */
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

		boot_calc_checksum(buf, bd->sector_size, is_boot_sec, &checksum);
	}

	ret = exfat_write_checksum_sector(bd, checksum, is_backup);

free_buf:
	free(buf);

	return ret;
}

/*------------------- Part 2: Managing Node Queue --------------------*/

/*
 * Adds a new virtual node to set->next_clus and sets its value to 'data'.
 * Returns 0 on success.
 */
static int add_virtual_node(struct cluster_info_set *set, uint32_t data)
{
	uint32_t offset = set->num_phys_clus;
	uint32_t index = set->num_clus_chain;
	size_t new_size;
	uint32_t *new_next_clus;

	/* Allocate memory in chunks due to unknown total file count */
	if (index % VIRT_SPACE_ALIGN == 0) {
		new_size = (offset + index + VIRT_SPACE_ALIGN) * sizeof(uint32_t);
		new_next_clus = realloc(set->next_clus, new_size);
		if (new_next_clus == NULL) {
			exfat_err("memory run out in %s", __func__);
			return -ENOMEM;
		}
		set->next_clus = new_next_clus;
	}

	/* Set next_clus value and increment virtual cluster count */
	set->next_clus[offset + index] = data;
	set->num_clus_chain++;

	return 0;
}

/*
 * Sets FAT entries for a contiguous cluster chain to ensure correctness.
 */
static int set_physical_nodes(struct cluster_info_set *set,
				      uint32_t begin, uint32_t count, uint32_t data)
{
	uint32_t i;

	/* avoid the risk of out of memory */
	if (begin < EXFAT_FIRST_CLUSTER || begin + count >= set->num_phys_clus) {
		exfat_err("%s: invalid parameter \"begin = %u, count = %u\"\n", __func__,
				begin, count);
		return -1;
	}

	for (i = begin; i < begin + count; i++)
		set->next_clus[i] = data++;
	set->next_clus[begin + count] = EXFAT_EOF_CLUSTER;
	return 0;
}

/*
 * Builds the 'prev_clus' array from the 'next_clus' links.
 */
static void next_to_prev(struct cluster_info_set *set)
{
	uint32_t cur_clu, last_clu;
	uint32_t n_clus = set->num_phys_clus;
	uint32_t n_file = set->num_clus_chain;
	uint32_t i;

	for (i = n_clus; i < n_clus + n_file; i++) {
		cur_clu = i;
		last_clu = EXFAT_EOF_CLUSTER;
		while (cur_clu != EXFAT_EOF_CLUSTER) {
			set->prev_clus[cur_clu] = last_clu;
			last_clu = cur_clu;
			cur_clu = set->next_clus[cur_clu];
		}
	}
}

/*--------- Part 3: File Tree Traversal (Directory Entry I/O) ---------*/

/*
 * Implementation of BFS_read_file_tree.
 * Returns 0 on success.
 */
static int BFS_read_children(struct exfat_defrag *defrag, struct exfat_inode *inode)
{
	int ret;
	uint16_t attr, checksum;
	uint32_t first_clu, dentry_count, i, FAT_entry_count;
	uint64_t size;
	bool is_contiguous;
	struct exfat_de_iter de_iter;
	struct exfat_dentry *first_dentry = NULL, *dentry = NULL;
	struct exfat_inode *child_inode = NULL;
	struct cluster_info_set *set = &(defrag->clu_info_set);

	/* Initialize directory entry iterator */
	ret = exfat_de_iter_init(&de_iter, defrag->exfat, inode, defrag->dentry_buffer);
	if (ret == EOF)
		return 0;
	else if (ret != 0) {
		exfat_err("fail in exfat_de_iter_init\n");
		return -1;
	}

	while (1) {
		checksum = 0;
		dentry_count = 1;

		/* Get first dentry of the set */
		ret = exfat_de_iter_get(&de_iter, 0, &first_dentry);
		if (ret == EOF)
			goto success;
		else if (ret != 0) {
			exfat_err("fail in exfat_de_iter_get first\n");
			goto fail;
		}

		switch (first_dentry->type) {
		case EXFAT_FILE:
			/* File dentry */
			dentry_count += first_dentry->file_num_ext;
			attr = le16_to_cpu(first_dentry->file_attr);

			/* Stream dentry */
			ret = exfat_de_iter_get(&de_iter, 1, &dentry);
			if (ret != 0 || dentry->type != EXFAT_STREAM) {
				exfat_err("fail in exfat_de_iter_get second\n");
				goto fail;
			}
			first_clu = le32_to_cpu(dentry->stream_start_clu);
			is_contiguous = dentry->stream_flags & EXFAT_SF_CONTIGUOUS;
			size = le64_to_cpu(dentry->stream_size);

			/* Do not process empty files/directories */
			if (size != 0) {

				if (!exfat_heap_clus(defrag->exfat, first_clu)) {
					exfat_err("%s: invalid first_clu = %u\n", __func__,
						  first_clu);
					goto fail;
				}

				/* Record first cluster in virtual node list */
				ret = add_virtual_node(set, first_clu);
				if (ret != 0)
					goto fail;

				/* Fix FAT entries: if contiguous, assign correct chain */
				if (is_contiguous) {
					FAT_entry_count =
						(uint32_t)((size - 1) / defrag->exfat->clus_size);
					if (set_physical_nodes(set, first_clu,
							       FAT_entry_count, first_clu + 1) < 0)
						goto fail;
				}

				/* Allocate and update inode if it's a directory */
				if (attr & ATTR_SUBDIR) {
					child_inode = exfat_alloc_inode(attr);
					if (child_inode == NULL)
						goto fail;
					child_inode->parent = inode;
					child_inode->dentry_count = dentry_count;
					child_inode->first_clus = first_clu;
					child_inode->is_contiguous = is_contiguous;
					child_inode->size = size;
					list_add_tail(&child_inode->list, &defrag->exfat->dir_list);
				}
			}

			exfat_calc_dentry_checksum(first_dentry, &checksum, true);
			exfat_calc_dentry_checksum(dentry, &checksum, false);

			/* Handle potential vendor_alloc dentry in the remaining dentries */
			for (i = 2; i < dentry_count; i++) {
				ret = exfat_de_iter_get(&de_iter, i, &dentry);
				if (ret != 0) {
					exfat_err("%s: fail in exfat_de_iter_get extended\n",
						  __func__);
					goto fail;
				}
				exfat_calc_dentry_checksum(dentry, &checksum, false);

				if (dentry->type != EXFAT_VENDOR_ALLOC)
					continue;

				/* handle vendor_alloc dentry */
				first_clu = le32_to_cpu(dentry->vendor_alloc_start_clu);
				is_contiguous = dentry->vendor_alloc_flags & EXFAT_SF_CONTIGUOUS;
				size = le64_to_cpu(dentry->vendor_alloc_size);

				if (size == 0)
					continue;

				if (!exfat_heap_clus(defrag->exfat, first_clu)) {
					exfat_err("%s: invalid first_clu = %u\n", __func__,
						  first_clu);
					goto fail;
				}

				ret = add_virtual_node(set, first_clu);
				if (ret != 0)
					goto fail;

				if (is_contiguous) {
					FAT_entry_count =
						(uint32_t)((size - 1) / defrag->exfat->clus_size);
					if (set_physical_nodes(set, first_clu,
							       FAT_entry_count, first_clu + 1) < 0)
						goto fail;
				}
			}

			if (first_dentry->file_checksum != checksum) {
				exfat_err("%s: invalid checksum, please run fsck.exfat to check and repair\n",
					  __func__);
				goto fail;
			}
			break;

		case EXFAT_BITMAP:
			first_clu = le32_to_cpu(first_dentry->bitmap_start_clu);
			ret = add_virtual_node(set, first_clu);
			if (ret != 0)
				goto fail;

			size = le32_to_cpu(first_dentry->bitmap_size);
			FAT_entry_count = (uint32_t)((size - 1) / defrag->exfat->clus_size);
			if (set_physical_nodes(set, first_clu, FAT_entry_count, first_clu + 1) < 0)
				goto fail;
			break;

		case EXFAT_UPCASE:
			first_clu = le32_to_cpu(first_dentry->upcase_start_clu);
			ret = add_virtual_node(set, first_clu);
			if (ret != 0)
				goto fail;

			size = le32_to_cpu(first_dentry->upcase_size);
			FAT_entry_count = (uint32_t)((size - 1) / defrag->exfat->clus_size);
			if (set_physical_nodes(set, first_clu, FAT_entry_count, first_clu + 1) < 0)
				goto fail;
			break;

		case EXFAT_LAST:	/* End of valid entries */
			goto success;

		default:
			break;
		}

		exfat_de_iter_advance(&de_iter, dentry_count);
	}

success:
	exfat_de_iter_flush(&de_iter);
	return 0;
fail:
	exfat_de_iter_flush(&de_iter);
	return -1;
}

/*
 * BFS traversal to read directory entries and populate virtual head nodes in next_clus.
 */
static int BFS_read_file_tree(struct exfat_defrag *defrag)
{
	struct exfat *exfat = defrag->exfat;
	struct exfat_inode *parent;
	struct cluster_info_set *set = &(defrag->clu_info_set);
	uint32_t i, n_clus, n_chain, tmp_clu;

	list_add(&exfat->root->list, &exfat->dir_list);
	if (add_virtual_node(set, exfat->root->first_clus) < 0)
		goto err;

	while (!list_empty(&exfat->dir_list)) {
		parent = list_entry(exfat->dir_list.next, struct exfat_inode, list);

		/* All inodes in the queue should be directories */
		if (!(parent->attr & ATTR_SUBDIR)) {
			exfat_err("%s: parent_inode is not dir\n", __func__);
			goto err;
		}

		/* Process children */
		if (BFS_read_children(defrag, parent) < 0)
			goto err;

		/* Remove parent after processing */
		list_del(&parent->list);
		if (parent != exfat->root)
			exfat_free_inode(parent);
	}

	/* Check the validation of every cluster chains */
	n_clus = set->num_phys_clus;
	n_chain = set->num_clus_chain;
	for (i = n_clus; i < n_clus + n_chain; i++) {
		tmp_clu = set->next_clus[i];
		while (1) {
			if (!exfat_heap_clus(exfat, tmp_clu)) {
				exfat_err("%s: invalid fat entry = %u\n", __func__, tmp_clu);
				goto err;
			}
			if (!exfat_bitmap_get(exfat->alloc_bitmap, tmp_clu)) {
				exfat_err("%s: cluster %u should be alloced\n", __func__, tmp_clu);
				goto err;
			}
			tmp_clu = set->next_clus[tmp_clu];
			if (tmp_clu == EXFAT_EOF_CLUSTER)
				break;
		}
	}

	return 0;

err:
	exfat_free_dir_list(exfat);
	return -1;
}

/*
 * Implementation of BFS_write_file_tree.
 * Returns 0 on success.
 */
static int BFS_write_children(struct exfat_defrag *defrag,
		struct exfat_inode *inode, uint32_t *index)
{
	int ret = 0;
	uint16_t attr, checksum;
	uint32_t first_clu, dentry_count, i;
	struct exfat_de_iter de_iter;
	struct exfat_dentry *first_dentry = NULL, *dentry = NULL;
	struct exfat_inode *child_inode = NULL;
	struct cluster_info_set *set = &(defrag->clu_info_set);

	/* Initialize directory entry iterator */
	ret = exfat_de_iter_init(&de_iter, defrag->exfat, inode, defrag->dentry_buffer);
	if (ret == EOF)
		return 0;
	else if (ret != 0) {
		exfat_err("fail in exfat_de_iter_init\n");
		return -1;
	}

	while (1) {
		first_clu = set->next_clus[(*index)];
		checksum = 0;
		dentry_count = 1;

		/* Get first dentry of the set */
		ret = exfat_de_iter_get_dirty(&de_iter, 0, &first_dentry);
		if (ret == EOF)
			goto success;
		else if (ret != 0) {
			exfat_err("fail in exfat_de_iter_get first\n");
			goto fail;
		}

		switch (first_dentry->type) {
		case EXFAT_FILE:
			/* File dentry */
			dentry_count += first_dentry->file_num_ext;
			attr = le16_to_cpu(first_dentry->file_attr);

			/* Stream dentry */
			ret = exfat_de_iter_get_dirty(&de_iter, 1, &dentry);
			if (ret != 0 || dentry->type != EXFAT_STREAM) {
				exfat_err("fail in exfat_de_iter_get second\n");
				goto fail;
			}

			/* Do not process empty files/directories */
			if (le64_to_cpu(dentry->stream_size) != 0) {

				/* Set start_clu + contiguous_flag */
				dentry->stream_start_clu = cpu_to_le32(first_clu);
				if ((*index) <= interrupt_point)
					dentry->stream_flags |= EXFAT_SF_CONTIGUOUS;
				else
					dentry->stream_flags &= ~EXFAT_SF_CONTIGUOUS;
				(*index)++;

				/* Allocate and update inode if it's a directory */
				if (attr & ATTR_SUBDIR) {
					child_inode = exfat_alloc_inode(attr);
					if (child_inode == NULL)
						goto fail;
					child_inode->parent = inode;
					child_inode->dentry_count = dentry_count;
					child_inode->first_clus = first_clu;
					child_inode->is_contiguous = true;
					child_inode->size = le64_to_cpu(dentry->stream_size);
					list_add_tail(&child_inode->list, &defrag->exfat->dir_list);
				}
			}

			exfat_calc_dentry_checksum(first_dentry, &checksum, true);
			exfat_calc_dentry_checksum(dentry, &checksum, false);

			for (i = 2; i < dentry_count; i++) {
				ret = exfat_de_iter_get_dirty(&de_iter, i, &dentry);
				if (ret != 0) {
					exfat_err("%s: fail in exfat_de_iter_get extended\n",
						  __func__);
					goto fail;
				}
				exfat_calc_dentry_checksum(dentry, &checksum, false);

				if (dentry->type == EXFAT_VENDOR_ALLOC) {
					/* Set start_clu + contiguous_flag */
					dentry->vendor_alloc_start_clu =
						cpu_to_le32(set->next_clus[(*index)]);
					if ((*index) <= interrupt_point)
						dentry->vendor_alloc_flags |= EXFAT_SF_CONTIGUOUS;
					else
						dentry->vendor_alloc_flags &= ~EXFAT_SF_CONTIGUOUS;
					(*index)++;
				}

			}

			first_dentry->file_checksum = cpu_to_le16(checksum);
			break;

		case EXFAT_BITMAP:
			first_dentry->bitmap_start_clu = cpu_to_le32(first_clu);
			(*index)++;
			break;

		case EXFAT_UPCASE:
			first_dentry->upcase_start_clu = cpu_to_le32(first_clu);
			(*index)++;
			break;

		case EXFAT_LAST:
			goto success;

		default:
			break;
		}

		exfat_de_iter_advance(&de_iter, dentry_count);
	}

success:
	exfat_de_iter_flush(&de_iter);
	return 0;
fail:
	exfat_de_iter_flush(&de_iter);
	return -1;
}

/*
 * BFS traversal to write directory entries using virtual head node info in next_clus.
 */
static int BFS_write_file_tree(struct exfat_defrag *defrag)
{
	struct exfat *exfat = defrag->exfat;
	struct exfat_inode *parent;
	struct cluster_info_set *set = &(defrag->clu_info_set);
	uint32_t index = set->num_phys_clus;

	/* Update root cluster in memory and on disk */
	exfat->root->first_clus = set->next_clus[index++];
	exfat->root->is_contiguous = true;
	list_add(&exfat->root->list, &exfat->dir_list);

	exfat->bs->bsx.root_cluster = exfat->root->first_clus;
	if (exfat_write_sector(exfat->blk_dev, exfat->bs, BOOT_SEC_IDX) < 0) {
		exfat_err("%s: fail in update boot sector\n", __func__);
		goto err;
	}
	if (exfat_write_sector(exfat->blk_dev, exfat->bs, BACKUP_BOOT_SEC_IDX) < 0) {
		exfat_err("%s: fail in update backup boot sector\n", __func__);
		goto err;
	}
	if (exfat_update_boot_checksum(exfat->blk_dev, false) < 0) {
		exfat_err("%s: fail in update boot sector checksum\n", __func__);
		goto err;
	}
	if (exfat_update_boot_checksum(exfat->blk_dev, true) < 0) {
		exfat_err("%s: fail in update backup boot sector checksum\n", __func__);
		goto err;
	}

	while (!list_empty(&exfat->dir_list)) {
		parent = list_entry(exfat->dir_list.next, struct exfat_inode, list);

		if (!(parent->attr & ATTR_SUBDIR)) {
			exfat_err("%s: parent_inode is not dir\n", __func__);
			goto err;
		}

		if (BFS_write_children(defrag, parent, &index) < 0)
			goto err;

		list_del(&parent->list);
		if (parent != exfat->root)
			exfat_free_inode(parent);
	}
	return 0;

err:
	exfat_free_dir_list(exfat);
	return -1;
}

/*---------------------- Part 4: Cluster Swapping ---------------------*/

/*
 * Read or write a single cluster.
 * Returns 0 on success.
 */
static int exfat_rw_clu(struct exfat *exfat, void *tmp, uint32_t cluster, bool read)
{
	off_t offset;
	size_t rw_len;

	offset = (off_t)exfat_c2o(exfat, cluster);

	if (read)
		rw_len = exfat_read(exfat->blk_dev->dev_fd, tmp, (size_t)exfat->clus_size, offset);
	else
		rw_len = exfat_write(exfat->blk_dev->dev_fd, tmp, (size_t)exfat->clus_size, offset);

	if (rw_len != (size_t)exfat->clus_size)
		return -EIO;

	return 0;
}

/*
 * Swap two clusters at the data level.
 * Returns 0 on success.
 */
static int data_swap(struct exfat_defrag *defrag, uint32_t clu_1,
		uint32_t clu_2, bool clu_1_not_free)
{
	struct exfat *exfat = defrag->exfat;
	void *tmp1 = defrag->tmp_clus;
	void *tmp2 = defrag->tmp_clus + exfat->clus_size;
	int ret = 0;

	if (clu_1_not_free) {
		ret = exfat_rw_clu(exfat, tmp1, clu_1, true);
		if (ret < 0)
			goto out;
		ret = exfat_rw_clu(exfat, tmp2, clu_2, true);
		if (ret < 0)
			goto out;
		ret = exfat_rw_clu(exfat, tmp1, clu_2, false);
		if (ret < 0)
			goto out;
		ret = exfat_rw_clu(exfat, tmp2, clu_1, false);
		if (ret < 0)
			goto out;
	} else {
		ret = exfat_rw_clu(exfat, tmp2, clu_2, true);
		if (ret < 0)
			goto out;
		ret = exfat_rw_clu(exfat, tmp2, clu_1, false);
		if (ret < 0)
			goto out;
	}
out:
	return ret;
}

/* Metadata level: Connect two clusters in the chain */
static void clus_connect(struct cluster_info_set *set, uint32_t clu_1, uint32_t clu_2)
{
	set->next_clus[clu_1] = clu_2;
	if (clu_2 != EXFAT_EOF_CLUSTER)
		set->prev_clus[clu_2] = clu_1;
}

/* Metadata level: Insert clu_2 between clu_1_prev and clu_1_next */
static void clus_insert(struct cluster_info_set *set, uint32_t clu_1_prev,
		uint32_t clu_1_next, uint32_t clu_2)
{
	clus_connect(set, clu_1_prev, clu_2);
	clus_connect(set, clu_2, clu_1_next);
}

/*
 * Special case for adjacent clusters during swap:
 * Before: clu_0 -> clu_1 -> clu_2 -> clu_3
 * After:  clu_0 -> clu_2 -> clu_1 -> clu_3
 */
static void clus_exchange(struct cluster_info_set *set, uint32_t clu_0,
		uint32_t clu_1, uint32_t clu_2, uint32_t clu_3)
{
	clus_connect(set, clu_1, clu_3);
	clus_connect(set, clu_0, clu_2);
	clus_connect(set, clu_2, clu_1);
}

/*
 * Swap two clusters at both data and metadata levels.
 * Returns 0 on success.
 */
static int exfat_swap_clus(struct exfat_defrag *defrag, uint32_t clu_1, uint32_t clu_2)
{
	char *bitmap = defrag->exfat->alloc_bitmap;
	bool clu_1_not_free = exfat_bitmap_get(bitmap, clu_1);

	struct cluster_info_set *set = &(defrag->clu_info_set);
	uint32_t clu_1_next = set->next_clus[clu_1];
	uint32_t clu_1_prev = set->prev_clus[clu_1];
	uint32_t clu_2_next = set->next_clus[clu_2];
	uint32_t clu_2_prev = set->prev_clus[clu_2];

	/* Data-level swap */
	if (data_swap(defrag, clu_1, clu_2, clu_1_not_free) < 0)
		return -EIO;

	/* Metadata-level update */
	if (clu_1_not_free) {
		if (clu_1_next == clu_2) {
			clus_exchange(set, clu_1_prev, clu_1, clu_2, clu_2_next);
		} else if (clu_2_next == clu_1) {
			clus_exchange(set, clu_2_prev, clu_2, clu_1, clu_1_next);
		} else {
			clus_insert(set, clu_1_prev, clu_1_next, clu_2);
			clus_insert(set, clu_2_prev, clu_2_next, clu_1);
		}
	} else {
		clus_connect(set, clu_2_prev, clu_1);
		clus_connect(set, clu_1, clu_2_next);
		set->next_clus[clu_2] = EXFAT_FREE_CLUSTER;
		set->prev_clus[clu_2] = EXFAT_FREE_CLUSTER;
		exfat_bitmap_set(bitmap, clu_1);
		exfat_bitmap_clear(bitmap, clu_2);
	}

	return 0;
}

/*-------------- Part 5: Defragmentation and Assessment ---------------*/

/*
 * Core defragmentation logic.
 * 1. exfat_defrag uses exfat_swap_clus for cluster swapping.
 * 2. exfat_swap_clus uses data_swap for data exchange.
 * 3. exfat_swap_clus uses clus_connect, clus_exchange,
 *    clus_insert for metadata update.
 * Returns 0 on success.
 */
static int exfat_defrag(struct exfat_defrag *defrag)
{
	int ret = 0;
	struct cluster_info_set *set = &(defrag->clu_info_set);
	uint32_t n_clus = set->num_phys_clus;
	uint32_t n_file = set->num_clus_chain;
	/* scan_ptr moves forward linearly; exchanged_ptr follows cluster chains */
	uint32_t scan_ptr = EXFAT_RESERVED_CLUSTERS, exchanged_ptr = 0;
	uint32_t i;

	exfat_info("Defragmentation is in progress — please keep the device connected.\n");
	exfat_info("If you do not want to wait, just press Ctrl+C to terminate it safely.\n");

	for (i = n_clus; i < n_clus + n_file; i++) {
		exchanged_ptr = set->next_clus[i];
		while (1) {
			if (exchanged_ptr == EXFAT_EOF_CLUSTER)
				break;

			if (scan_ptr != exchanged_ptr) {
				ret = exfat_swap_clus(defrag, scan_ptr, exchanged_ptr);
				if (ret < 0)
					goto out;
			}
			exchanged_ptr = set->next_clus[scan_ptr];
			scan_ptr++;
		}
		exfat_info("\r\033[Kdefrag.exfat has processed %u / %u tasks",
			   i - n_clus + 1, n_file);
		fflush(stdout);

		if (interrupt_received) {
			interrupt_point = i;
			exfat_info("\nReceived termination signal! Writing back metadata...\n");
			goto out;
		}
	}
	exfat_info("\nWriting back metadata...\n");

out:
	return ret;
}

/*
 * Evaluate fragmentation level from two perspectives:
 * cluster chain contiguity and free cluster contiguity
 */
static void exfat_defrag_assess(struct exfat_defrag *defrag)
{
	uint32_t i, n_chain, n_clus, cur_clu, last_clu;
	uint32_t num_alloc_breakpoint = 0, num_free_breakpoint = 0;
	uint32_t num_free_clus = 0, num_alloc_clus = 0;
	double alloc_space_rate, free_space_rate;
	double alloc_space_frag_rate = 0.0, free_space_frag_rate = 0.0, overall_frag_rate = 0.0;
	struct exfat *exfat = defrag->exfat;
	struct cluster_info_set *set = &(defrag->clu_info_set);

	n_chain = set->num_clus_chain;
	n_clus = set->num_phys_clus;

	/* 1. Assess cluster chain contiguity */
	for (i = n_clus; i < n_clus + n_chain; i++) {
		cur_clu = set->next_clus[i];
		last_clu = EXFAT_EOF_CLUSTER;
		while (cur_clu != EXFAT_EOF_CLUSTER) {
			if (last_clu != EXFAT_EOF_CLUSTER && last_clu + 1 != cur_clu)
				num_alloc_breakpoint++;
			last_clu = cur_clu;
			cur_clu = set->next_clus[cur_clu];
		}
	}

	/* 2. Assess free cluster contiguity */
	last_clu = 0;
	for (cur_clu = EXFAT_FIRST_CLUSTER; cur_clu < exfat->clus_count + EXFAT_FIRST_CLUSTER;
	     cur_clu++) {
		if (!exfat_bitmap_get(exfat->alloc_bitmap, cur_clu)) {
			num_free_clus++;
			if (last_clu != 0 && cur_clu != last_clu + 1)
				num_free_breakpoint++;
			last_clu = cur_clu;
		}
	}
	num_alloc_clus = exfat->clus_count - num_free_clus;

	/* 3. Calculate and output */
	if (exfat->clus_count == 0) {
		exfat_err("%s: invalid cluster count == 0\n", __func__);
		return;
	}
	alloc_space_rate = (double)(num_alloc_clus * 100) / exfat->clus_count;
	free_space_rate = (double)(num_free_clus * 100) / exfat->clus_count;
	if (num_alloc_clus > 0) {
		alloc_space_frag_rate = (double)(num_alloc_breakpoint * 100) / num_alloc_clus;
		overall_frag_rate += (alloc_space_frag_rate * alloc_space_rate) / 100;
	}
	if (num_free_clus > 0) {
		free_space_frag_rate = (double)(num_free_breakpoint * 100) / num_free_clus;
		overall_frag_rate += (free_space_frag_rate * free_space_rate) / 100;
	}

	exfat_info("Fragmentation Assessment Report:\n");
	exfat_info("1. Allocated space: cluster count = %u (%.2lf%%), breakpoint count = %u, fragmentation rate = %.2lf%%\n",
			num_alloc_clus, alloc_space_rate, num_alloc_breakpoint,
			alloc_space_frag_rate);
	exfat_info("2. Free space: cluster count = %u (%.2lf%%), breakpoint count = %u, fragmentation rate = %.2lf%%\n",
			num_free_clus, free_space_rate, num_free_breakpoint, free_space_frag_rate);
	exfat_info("3. Overall weighted fragmentation rate = %.2lf%% (Reference: 0%% ~ Light ~ %u%% ~ Moderate ~ %u%% ~ Severe ~ 100%%)\n",
			overall_frag_rate, WATERMARK_1, WATERMARK_2);
}

/*--------------------------- Part 6: Usage information----------------------------- */

static struct option opts[] = {
	{"force",	no_argument,	NULL,	'f' },
	{"assess",	no_argument,	NULL,	'a' },
	{"version",	no_argument,	NULL,	'v' },
	{"help",	no_argument,	NULL,	'h' },
	{NULL,		0,		NULL,	 0  }
};

static void usage(char *name, int exit_code)
{
	fprintf(stderr, "Usage: %s\n", name);
	fprintf(stderr, "\tno option           Perform defragmentation with \"fsck warning\"\n");
	fprintf(stderr, "\t-f | --force        Perform defragmentation\n");
	fprintf(stderr, "\t-a | --assess       Assess fragmentation status\n");
	fprintf(stderr, "\t-v | --version      Show version\n");
	fprintf(stderr, "\t-h | --help         Show help\n");

	exit(exit_code);
}

int main(int argc, char *argv[])
{
	struct exfat_defrag defrag;
	struct exfat_user_input ui;
	struct exfat_blk_dev bd;
	struct cluster_info_set *set = &(defrag.clu_info_set);
	int ret = 0, c = 0;
	/* if no option, initial state */
	bool only_assessment = false;
	bool only_show_version = false;
	bool show_fsck_warning = true;

	memset(&ui, 0, sizeof(ui));
	memset(&bd, 0, sizeof(bd));
	bd.dev_fd = -1;

	/* step-0: Parameter Processing and Mode Recognition */

	if (!setlocale(LC_CTYPE, ""))
		exfat_err("failed to init locale/codeset\n");

	/*
	 * Recognizable command format:
	 * 1. defrag.exfat /dev/sdX      Perform defragmentation with "fsck warning"
	 * 2. defrag.exfat -f /dev/sdX   Perform defragmentation
	 * 3. defrag.exfat -a /dev/sdX   Assess fragmentation status
	 * 4. defrag.exfat -h            Usage information
	 * 5. defrag.exfat -v            Show version
	 */
	while ((c = getopt_long(argc, argv, "afhv", opts, NULL)) != EOF) {
		switch (c) {
		case 'f':
			show_fsck_warning = false;
			break;
		case 'a':
			only_assessment = true;
			break;
		case 'v':
			only_show_version = true;
			break;
		case 'h':
			usage(argv[0], 0);
			break;
		default:
			exfat_err("Invalid command! Please refer to the help information below or consult the manual.\n");
			usage(argv[0], 2);
		}
	}

	show_version();
	if (only_show_version)
		return ret;

	if (optind != argc - 1) {
		exfat_err("Invalid command! Please refer to the help information below or consult the manual.\n");
		usage(argv[0], 2);
	}

	if ((!only_assessment) && show_fsck_warning) {
		exfat_info("\nWARNING: To ensure data safety and consistency, we strongly recommend\n");
		exfat_info("running \"fsck.exfat\" to check and repair the device before defragmentation.\n");
		exfat_info("Proceed without filesystem check? [y/N]: ");
		fflush(stdout);

		c = fgetc(stdin);
		if (!(c == 'Y' || c == 'y')) {
			exfat_info("Defrag aborted.");
			return 1;
		}
		exfat_info("\n");
	}

	/* Step 1: Get block device info */
	ui.dev_name = argv[optind];
	if (only_assessment)
		ui.writeable = false;
	else
		ui.writeable = true;
	if (exfat_get_blk_dev_info(&ui, &bd) < 0) {
		exfat_err("fail in exfat_get_blk_dev_info\n");
		ret = 1;
		goto out;
	}

	/* Step 2: Initialize exfat structure */
	memset(&defrag, 0, sizeof(defrag));
	defrag.exfat = exfat_alloc_exfat(&bd, NULL, NULL);
	if (defrag.exfat == NULL) {
		exfat_err("fail in exfat_alloc_exfat\n");
		ret = 1;
		goto out;
	}

	/* Step 3: Allocate memory: next_clus array, temp clusters, dentry buffer */
	set->num_phys_clus = defrag.exfat->clus_count + EXFAT_FIRST_CLUSTER;
	set->num_clus_chain = 0;
	set->next_clus = calloc(set->num_phys_clus, sizeof(uint32_t));
	if (set->next_clus == NULL) {
		exfat_err("memory run out in defrag.cluster_info_set->next_clus\n");
		ret = 1;
		goto fsycn_and_free;
	}
	defrag.tmp_clus = calloc(2, defrag.exfat->clus_size);
	if (defrag.tmp_clus == NULL) {
		exfat_err("memory run out in defrag.tmp_clus\n");
		ret = 1;
		goto fsycn_and_free;
	}
	defrag.dentry_buffer = exfat_alloc_buffer(defrag.exfat);
	if (defrag.dentry_buffer == NULL) {
		exfat_err("memory run out in defrag.dentry_buffer\n");
		ret = 1;
		goto fsycn_and_free;
	}

	/* Step 4: Read bitmap and FAT */
	if (exfat_rw_bitmap(defrag.exfat, true) < 0) {
		exfat_err("fail in exfat_read_bitmap\n");
		ret = 1;
		goto fsycn_and_free;
	}
	if (exfat_rw_FAT(defrag.exfat, set->next_clus, set->num_phys_clus, true) < 0) {
		exfat_err("fail in exfat_read_FAT\n");
		ret = 1;
		goto fsycn_and_free;
	}

	/* Step 5: Traverse file tree (BFS), collect first cluster info into virtual nodes */
	if (BFS_read_file_tree(&defrag) < 0) {
		exfat_err("fail in BFS_read_file_tree\n");
		ret = 1;
		goto fsycn_and_free;
	}

	/* For fragmentation assessment, reaching this step is sufficient */
	if (only_assessment) {
		exfat_defrag_assess(&defrag);
		goto fsycn_and_free;
	}

	/* Step 6: Allocate prev_clus array and build backward links */
	set->prev_clus = calloc(set->num_phys_clus + set->num_clus_chain, sizeof(uint32_t));
	if (set->prev_clus == NULL) {
		exfat_err("memory run out in defrag.cluster_info_set->prev_clus\n");
		ret = 1;
		goto fsycn_and_free;
	}
	next_to_prev(set);

	/* Step 7: Perform defragmentation with interrupt support */
	interrupt_point = 0;
	interrupt_received = 0;
	if (signal(SIGINT, sigint_handler) == SIG_ERR ||
	    signal(SIGTERM, sigint_handler) == SIG_ERR) {
		exfat_err("fail in signal register\n");
		ret = 1;
		goto fsycn_and_free;
	}

	if (exfat_defrag(&defrag) < 0) {
		exfat_err("fail in exfat_defrag\n");
		ret = 1;
		goto fsycn_and_free;
	}

	/* Step 8: Rewrite first cluster and contiguous flags in directory entries */
	if (BFS_write_file_tree(&defrag) < 0) {
		exfat_err("fail in BFS_write_file_tree\n");
		ret = 1;
		goto fsycn_and_free;
	}

	/* Step 9: Write back updated bitmap and FAT */
	if (exfat_rw_bitmap(defrag.exfat, false) < 0) {
		exfat_err("fail in exfat_write_bitmap\n");
		ret = 1;
		goto fsycn_and_free;
	}
	if (exfat_rw_FAT(defrag.exfat, set->next_clus, set->num_phys_clus, false) < 0) {
		exfat_err("fail in exfat_write_FAT\n");
		ret = 1;
		goto fsycn_and_free;
	}

fsycn_and_free:
	/* Step 10: fsync and free allocated resources */
	fsync(bd.dev_fd);

	if (set->next_clus)
		free(set->next_clus);
	if (set->prev_clus)
		free(set->prev_clus);
	if (defrag.tmp_clus)
		free(defrag.tmp_clus);
	if (defrag.dentry_buffer)
		exfat_free_buffer(defrag.exfat, defrag.dentry_buffer);
	if (defrag.exfat)
		exfat_free_exfat(defrag.exfat);

out:
	if (bd.dev_fd >= 0)
		close(bd.dev_fd);

	if (ret == 0) {
		if (only_assessment)
			exfat_info("Assessment has completed successfully!\n");
		else
			exfat_info("Defragmentation has completed successfully!\n");
	}

	return ret;
}
