/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Copyright (C) 2026 Hyunchul Lee <hyc.lee@gmail.com>
 *
 *  Portions of the progress bar code derived from ntfsprogs-plus and modified for exfatprogs.
 */

#ifndef _EXFAT_UTILS_H
#define _EXFAT_UTILS_H

#include <stdint.h>

struct progress_bar {
	uint32_t start;
	uint32_t stop;
	uint32_t current;
	uint32_t resolution;
#ifdef PROG_CALC_FLOAT
	float unit;
#else
	uint64_t total;
#endif
};

void progress_init(struct progress_bar *p, uint32_t start, uint32_t stop, uint32_t res);
void progress_update(struct progress_bar *p, uint32_t current);
#endif
