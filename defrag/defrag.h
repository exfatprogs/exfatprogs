/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2025 Haodong Xia <3394797836@qq.com>
 */

#ifndef _DEFRAG_H
#define _DEFRAG_H

#include <stdint.h>

struct exfat;
struct buffer_desc;

/*
 * This structure tracks the linkage of all cluster chains on the device.
 *
 * In exFAT, a cluster chain is typically represented by:
 *   - The 'first cluster' field in directory entries (e.g., file/directory start)
 *   - The rest of the chain maintained by FAT entries
 *
 * We model this as a linked list with two types of nodes:
 *   - Virtual Node: Represents the 'first cluster' from directory entries
 *   - Physical Node: Represents clusters linked by FAT entries
 *
 * Each chain starts with a Virtual Node, followed by zero or more Physical Nodes.
 * The entire structure is maintained as a doubly-linked list using two arrays:
 *   - 'next_clus': stores forward pointers
 *   - 'prev_clus': stores backward pointers
 *
 * Virtual Nodes are placed after Physical Nodes in the arrays.
 * Since the number of Virtual Nodes (i.e., number of files/directories)
 * is unknown until the file tree is traversed, memory for them is allocated dynamically.
 */
struct cluster_info_set {
	uint32_t *next_clus;      /* Forward pointers for the doubly-linked list */
	uint32_t *prev_clus;      /* Backward pointers for the doubly-linked list */
	uint32_t num_phys_clus;   /* Number of physical clusters (Physical Nodes) */
	uint32_t num_clus_chain;   /* Number of cluster chains (Virtual Nodes), one per file/directory */
};

#define VIRT_SPACE_ALIGN 4096  /* Allocate virtual nodes in chunks of 4096 */

/* Core metadata for defragmentation */
struct exfat_defrag {
	struct exfat *exfat;                  /* File system metadata */
	struct cluster_info_set clu_info_set; /* Doubly-linked list representing cluster chain structure */
	struct buffer_desc *dentry_buffer;    /* Temporary storage for directory entries */
	uint8_t *tmp_clus;                    /* Temporary buffer for cluster swapping */
};

/* Reference for fragmentation assessment */
#define WATERMARK_1 30
#define WATERMARK_2 70

#endif
