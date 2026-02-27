/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *   Copyright (C) 2026 David Timber <dxdt@dev.snart.me>
 */
#ifndef _DOSATTR_H_
#define _DOSATTR_H_

/*
 * Unfortunately, Linux doesn't provide a scalable and viable fs walk API and
 * we're stuck with the interface with 16-bit signed integers. The state of FTS
 * and ntfw() in Linux is such a mess at the moment, but FTS is still the best
 * option that doesn't involve detrimental recursion.
 */
#include <fts.h>

#include <linux/msdos_fs.h>
#undef ATTR_HIDDEN
#undef ATTR_VOLUME

#include "exfat_ondisk.h"
#include "libexfat.h"
#include "version.h"

#define DOSATTR_EXIT_USAGE	(2)
#define DOSATTR_EXIT_PARTIAL	(3)

typedef int (*fts_cb_f)(FTSENT *ent, bool *skip);

extern const char *program_name;

static int dosattr_fts_walk(FTS *fts, FTSENT *ent, fts_cb_f callback)
{
	int ret;
	bool skip = false;

	switch (ent->fts_info) {
	case FTS_ERR:
	case FTS_DNR:
	case FTS_NS:
		exfat_err("%s: %s: %s\n", program_name, ent->fts_path,
			strerror(ent->fts_errno));
		return 0;
	case FTS_D:
	case FTS_DOT:
	case FTS_F:
		break;
	/* case FTS_DP: */
	default:
		return 0;
	}

	/* Reached if the entry being visited is a node */
	assert(ent->fts_statp != NULL);

	ret = callback(ent, &skip);
	if (skip)
		fts_set(fts, ent, FTS_SKIP);

	return ret;
}

static int dosattr_fts(char *const *argv, fts_cb_f callback, const int options)
{
	FTSENT *ent;
	FTS *fts;
	int err = EXIT_SUCCESS, saved_errno;

	fts = fts_open(argv, options, NULL);
	if (fts == NULL)
		return 1;

	while ((ent = fts_read(fts)) != NULL) {
		if (dosattr_fts_walk(fts, ent, callback) != 0) {
			if (err == EXIT_SUCCESS)
				err = EXIT_FAILURE;
		} else if (err == EXIT_FAILURE)
			err = DOSATTR_EXIT_PARTIAL;
	}
	saved_errno = errno;
	fts_close(fts);
	if (saved_errno != 0)
		return EXIT_FAILURE;

	return err;
}

#endif
