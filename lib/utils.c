// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2026 Hyunchul Lee <hyc.lee@gmail.com>
 *
 *   Portions of the progress bar code derived from ntfsprogs-plus and modified for exfatprogs.
 */

#include <stdio.h>
#include <inttypes.h>

#include "utils.h"

void progress_init(struct progress_bar *p, uint32_t start, uint32_t stop, uint32_t res)
{
	uint64_t total;

	p->start = start;
	p->stop = stop;
	p->current = start;

	total = stop - start + 1;
	if (total <= 0)
		total = 1;

#ifdef PROG_CALC_FLOAT
	p->unit = 100.0 / total;

	if (((res * 100 * 100) / total) == 0)
		p->resolution = (int)(total / (100 * 100.0));	// 0.01 resolution
	else
		p->resolution = res;
#else
	p->total = total;

	if (((res * 100) / p->total) == 0) {
		p->resolution = p->total / 100;
		if (!p->resolution)
			p->resolution = 1;
	} else
		p->resolution = res;
#endif
}

void progress_update(struct progress_bar *p, uint32_t update)
{
#ifdef PROG_CALC_FLOAT
	float percent;
#else
	uint32_t percent;
#endif

	p->current += update;
	if (p->current != p->stop) {
		if ((p->current - p->start) % p->resolution)
			return;
#ifdef PROG_CALC_FLOAT
		percent = p->unit * p->current;
		printf("%6.2f percent completed\r", percent);
#else
		percent = (p->current * 100) / p->total;
		printf("%3u percent completed\r", percent);
#endif
	} else
		printf("100.00 percent completed\n");
	fflush(stdout);
}
