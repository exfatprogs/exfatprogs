/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *   Copyright (C) 2021 LG Electronics.
 *
 *   Author(s): Hyunchul Lee <hyc.lee@gmail.com>
 */
#ifndef _EXFAT_FS_H_
#define _EXFAT_FS_H_

#include "list.h"

struct exfat_dentry;

struct exfat_inode {
	struct exfat_inode	*parent;
	struct list_head	children;
	struct list_head	sibling;
	struct list_head	list;
	clus_t			first_clus;
	__u16			attr;
	uint64_t		size;
	bool			is_contiguous;
	struct exfat_dentry	*dentry_set;
	int			dentry_count;
	off_t			dev_offset;
	__le16			name[0];	/* only for directory */
};

#define EXFAT_NAME_MAX			255
#define NAME_BUFFER_SIZE		((EXFAT_NAME_MAX + 1) * 2)

struct exfat {
	struct exfat_blk_dev	*blk_dev;
	struct pbr		*bs;
	char			volume_label[VOLUME_LABEL_BUFFER_SIZE];
	struct exfat_inode	*root;
	struct list_head	dir_list;
	clus_t			clus_count;
	unsigned int		clus_size;
	unsigned int		sect_size;
	unsigned char		*disk_bitmap;
	unsigned char		*alloc_bitmap;
	unsigned char		*ohead_bitmap;
	clus_t			disk_bitmap_clus;
	unsigned int		disk_bitmap_size;
	__u16			*upcase_table;
	clus_t			start_clu;
	unsigned int		buffer_count;
	struct buffer_desc	*lookup_buffer; /* for dentry set lookup */
	/*
	 * btree of directory start clusters for recursive directory traversal
	 *
	 * Contains cluster numbers of parent directories.
	 *
	 * See also: exfat_fsck.name_hash_tree
	 */
	void			*dir_clus_tree;
	size_t			dir_clus_tree_size;
};

struct exfat_dentry_loc {
	struct exfat_inode	*parent;
	off_t			file_offset;
	off_t			dev_offset;
};

struct path_resolve_ctx {
	struct exfat_inode	*ancestors[255];
	__le16			utf16_path[PATH_MAX + 2];
	char			local_path[PATH_MAX * MB_LEN_MAX + 1];
};

struct buffer_desc {
	__u32		p_clus;
	unsigned int	offset;
	char		*buffer;
	char		dirty[EXFAT_BITMAP_SIZE(4 * KB / 512)];
};

struct exfat *exfat_alloc_exfat(struct exfat_blk_dev *blk_dev, struct pbr *bs,
				 struct exfat_inode *root);
void exfat_free_exfat(struct exfat *exfat);

struct exfat_inode *exfat_alloc_inode(__u16 attr);
void exfat_free_inode(struct exfat_inode *node);

void exfat_free_children(struct exfat_inode *dir, bool file_only);
void exfat_free_file_children(struct exfat_inode *dir);
/*
 * Delete @child and all ancestors with no children
 *
 * This is a non-callback version of exfat_free_ancestors2().
 */
void exfat_free_ancestors(struct exfat_inode *child);
/*
 * Delete all the ancestors with no children, calling the callback before
 * freeing each node
 *
 * To disable callbacks, pass cull_cb as NULL.
 */
void exfat_free_ancestors2(struct exfat_inode *child,
		void (*cull_cb)(struct exfat_inode *child, void *ctx),
		void *cb_ctx);
void exfat_free_dir_list(struct exfat *exfat);

int exfat_resolve_path(struct path_resolve_ctx *ctx, struct exfat_inode *child);
int exfat_resolve_path_parent(struct path_resolve_ctx *ctx,
			      struct exfat_inode *parent, struct exfat_inode *child);

struct buffer_desc *exfat_alloc_buffer(struct exfat *exfat);
void exfat_free_buffer(const struct exfat *exfat, struct buffer_desc *bd);

/*
 * Add a cluster number to the tree
 *
 * Returns true if the cluster number has been successfully added to the tree.
 * Returns false otherwise and errno would probably be set to ENOMEM.
 */
bool exfat_add_dir_clus_tree(struct exfat *exfat, clus_t dir_clus);
/*
 * Try finding the cluster number in the tree and return true if found. Returns
 * false otherwise.
 */
bool exfat_has_dir_clus_tree(struct exfat *exfat, clus_t dir_clus);
/*
 * Delete the cluster number from the tree
 *
 * The function always succeeds. Returns true if the cluster number was found in
 * the tree and has been deleted. Returns false otherwise.
 */
bool exfat_del_dir_clus_tree(struct exfat *exfat, clus_t dir_clus);

static inline unsigned int exfat_get_read_size(const struct exfat *exfat)
{
	return MIN(exfat->clus_size, 4 * KB);
}
#endif
