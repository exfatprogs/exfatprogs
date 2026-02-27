// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2026 David Timber <dxdt@dev.snart.me>
 */

#include <stdbool.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <locale.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

#include "dosattr.h"

const char *program_name = "lsdosattr";

struct attrm {
	const char *ro; /* ATTR_READONLY */
	const char *hi; /* ATTR_HIDDEN   */
	const char *sy; /* ATTR_SYSTEM   */
	const char *vo; /* ATTR_VOLUME   */
	const char *su; /* ATTR_SUBDIR   */
	const char *ar; /* ATTR_ARCHIVE  */
};

static struct {
	bool recursive;
	bool version_only;
	bool all;
	bool dirs;
	bool long_attrs;
	bool verbose;
	bool help;
} ui;

static const struct attrm attrm_short = {
	.ro = "r",
	.hi = "h",
	.sy = "s",
	.vo = "v",
	.su = "d",
	.ar = "a",
};
static const struct attrm attrm_long = {
	.ro = "READONLY ",
	.hi = "HIDDEN ",
	.sy = "SYSTEM ",
	.vo = "VOLUME ",
	.su = "DIRECTORY ",
	.ar = "ARCHIVED ",
};
static const struct attrm *attrm;

static void usage(void)
{
	fprintf(stderr, "Usage: %s [OPTION] FILE...\n", program_name);
	fprintf(stderr, "List the DOS attributes of each FILE\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\t-R  list subdirectories recursively\n");
	fprintf(stderr, "\t-a  do not ignore entries starting with a dot(.)\n");
	fprintf(stderr, "\t-d  list directories themselves, not their contents\n");
	fprintf(stderr,
		"\t-l  use long attribute names instead of single character abbreviations\n");
	fprintf(stderr, "\t-V  show version\n");
	fprintf(stderr, "\t-h  show help\n");

	exit(DOSATTR_EXIT_USAGE);
}

static void push_attrs(char *out, size_t *p, const char *a)
{
	const size_t al = strlen(a);

	memcpy(out + *p, a, al);
	*p += al;
}

static int list_attributes(FTSENT *ent, bool *skip)
{
	char attrs[sizeof("read-only hidden system volume directory archived ")];
	char vdls[21];
	size_t ap = 0;
	uint32_t dosattrs = UINT32_MAX;
	int fd;
	int err = 0;
#define PUSH_ATTRS(x) push_attrs(attrs, &ap, x)

	if (!ui.recursive) {
		if (ui.dirs || (S_ISDIR(ent->fts_statp->st_mode) && ent->fts_level > 0)) {
			/* Don't visit the children, but the arg itself shouldn't be skipped */
			*skip = true;
		}
	}
	if (!ui.all && ent->fts_level > 0 && ent->fts_name[0] == '.') {
		/* Skip the "hidden" node */
		if (strcmp(ent->fts_name, ".") != 0 && strcmp(ent->fts_name, "..") != 0)
			/* Don't touch SEEDOTS, though */
			*skip = true;
		return 0;
	}

	/*
	 * FIXME: should do fstatfs() to get fstype before doing ioctl()? Should
	 * I also work on NTFS in the future?
	 */

	fd = open(ent->fts_accpath, O_RDONLY);
	if (fd < 0) {
		exfat_err("%s: %s: %s\n", program_name, ent->fts_path, strerror(errno));
		return 1;
	}

	if (ui.long_attrs)
		attrs[0] = 0;
	else
		strncpy(attrs, "******", sizeof(attrs));

	if (ioctl(fd, FAT_IOCTL_GET_ATTRIBUTES, &dosattrs) == 0) {
		if (dosattrs & ATTR_READONLY)
			PUSH_ATTRS(attrm->ro);
		else if (!ui.long_attrs)
			PUSH_ATTRS("-");

		if (dosattrs & ATTR_HIDDEN)
			PUSH_ATTRS(attrm->hi);
		else if (!ui.long_attrs)
			PUSH_ATTRS("-");

		if (dosattrs & ATTR_SYSTEM)
			PUSH_ATTRS(attrm->sy);
		else if (!ui.long_attrs)
			PUSH_ATTRS("-");

		if (dosattrs & ATTR_VOLUME)
			PUSH_ATTRS(attrm->vo);
		else if (!ui.long_attrs)
			PUSH_ATTRS("-");

		if (dosattrs & ATTR_SUBDIR)
			PUSH_ATTRS(attrm->su);
		else if (!ui.long_attrs)
			PUSH_ATTRS("-");

		if (dosattrs & ATTR_ARCHIVE)
			PUSH_ATTRS(attrm->ar);
		else if (!ui.long_attrs)
			PUSH_ATTRS("-");

		attrs[ap] = 0;
	} else if (errno != ENOTTY) {
		err = 2;
		exfat_err("%s: FAT_IOCTL_GET_ATTRIBUTES, %s: %s\n",
			program_name, ent->fts_path, strerror(errno));
	}

	snprintf(vdls, sizeof(vdls), "%c", '*');
	/*
	 * The Linux community has decided that instead of introducing a new
	 * IOCTL, the kernel should provide the VDL(valid data length)
	 * information via SEEK_DATA/SEEK_HOLE interface. If the in-kernel exFAT
	 * lseek() interface is "VDL-aware"(upcoming iomap patches), when the
	 * VDL falls short of st->st_size, the VDL can be inferred from the fact
	 * that SEEK_HOLE offset is less than EOF.
	 *
	 * Link: https://lore.kernel.org/linux-fsdevel/20260311222613.2010177-1-dxdt@dev.snart.me/
	 *
	 * Note that the VDL is in bytes but the kernel could round it up to
	 * prevent inadvertent misaligned I/O.
	 */
	if (S_ISREG(ent->fts_statp->st_mode)) {
		off_t vdl = lseek(fd, 0, SEEK_HOLE);

		if (vdl < 0 && errno == ENXIO)
			vdl = 0;

		if (vdl >= 0) {
			if (!ui.verbose && vdl == ent->fts_statp->st_size)
				snprintf(vdls, sizeof(vdls), "%c", '=');
			else
				snprintf(vdls, sizeof(vdls), "%"PRIu64, (uint64_t)vdl);
		} else if (errno != ENXIO) {
			err = 2;
			exfat_err("%s: lseek(), %s: %s\n", program_name,
				  ent->fts_path, strerror(errno));
		}
	}

	if (ui.long_attrs)
		printf("%-28s %12"PRIu64" %12s %s\n",
			ent->fts_path, ent->fts_statp->st_size, vdls, attrs);
	else
		printf("%s %12"PRIu64" %12s %s\n",
			attrs, ent->fts_statp->st_size, vdls, ent->fts_path);

	close(fd);
	return err;
#undef PUSH_ATTR
}

int main(int argc, char *argv[])
{
#define MY_FTSOPTS (FTS_PHYSICAL | FTS_SEEDOT)
	static int c, retval;

	setlocale(LC_MESSAGES, "");
	setlocale(LC_CTYPE, "");

	if (!(argc && *argv))
		usage();

	while ((c = getopt(argc, argv, "RVvadlh")) != EOF) {
		switch (c) {
		case 'R':
			ui.recursive = true;
			break;
		case 'V':
			ui.version_only = true;
			break;
		case 'v':
			ui.verbose = true;
			break;
		case 'a':
			ui.all = true;
			break;
		case 'd':
			ui.dirs = true;
			break;
		case 'l':
			ui.long_attrs = true;
			break;
		case 'h':
			ui.help = true;
			break;
		default:
			usage();
		}
	}

	if (ui.version_only)
		show_version();
	if (ui.help)
		usage();
	if (ui.help || ui.version_only)
		exit(DOSATTR_EXIT_USAGE);

	if (ui.long_attrs)
		attrm = &attrm_long;
	else
		attrm = &attrm_short;

	if (optind > argc - 1) {
		static const char *const DEFAULT_ARGV[] = { ".", NULL };

		retval = dosattr_fts((char * const *)DEFAULT_ARGV,
				list_attributes, MY_FTSOPTS);
	} else
		retval = dosattr_fts((char * const *)(argv + optind),
				list_attributes, MY_FTSOPTS);

	return retval;
}
