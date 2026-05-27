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

const char *program_name = "chdosattr";

enum Operator {
	CHDOSATTR_OP_NONE,
	CHDOSATTR_OP_ADD, /* add */
	CHDOSATTR_OP_SUB, /* subtract */
	CHDOSATTR_OP_ASS, /* assign */
	NB_CHDOSATTR, /* assign */
};

static struct {
	struct {
		enum Operator op;
		uint32_t operand;
	} attr;
	struct {
		bool recursive;
		bool version_only;
		bool dirs;
		bool help;
	} f;
} ui;

static const struct {
	char c;
	uint32_t attr;
} attrm[] = {
	{ 'a', ATTR_ARCHIVE  },
	/* { 'd', ATTR_SUBDIR   }, */
	{ 'h', ATTR_HIDDEN   },
	{ 'r', ATTR_READONLY },
	{ 's', ATTR_SYSTEM   },
	/* { 'v', ATTR_VOLUME   }, */
};

static uint32_t get_attrbyc(const char c)
{
	for (size_t i = 0; i < sizeof(attrm) / sizeof(attrm[0]); i += 1) {
		if (attrm[i].c == c)
			return attrm[i].attr;
	}
	return 0;
}

static uint32_t get_attrfromstr(const char *s)
{
	uint32_t ret = 0, tmp;

	for (size_t i = 0; s[i] != 0; i += 1) {
		tmp = get_attrbyc(s[i]);
		if (tmp == 0)
			return 0;
		ret |= tmp;
	}

	return ret;
}

static void usage(void)
{
	fprintf(stderr, "Usage: %s [OPTION] ATTR FILE...\n", program_name);
	fprintf(stderr, "Change the DOS attributes of each FILE to ATTR\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\t-R  change files and directories recursively\n");
	fprintf(stderr, "\t-V  show version\n");
	fprintf(stderr, "\t-H  show help\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "ATTR is in the form of '[+-=]([ahrs]+)?'\n");

	exit(DOSATTR_EXIT_USAGE);
}

static char *do_extract_argv(int off, int *argc, char **argv)
{
	char *ret = argv[off];

	memmove(argv + off, argv + off + 1, sizeof(char *) * (*argc - off - 1));
	*argc -= 1;
	argv[*argc] = NULL;

	return ret;
}

/*
 * Extract the attribute operation spec from the argv so that the rest of args
 * can be processed with getopt().
 */
static char *extract_attr_arg(int *argc, char **argv)
{
	int i;
	enum Operator op;
	uint32_t attrs;

	for (i = 1; i < *argc; i += 1) {
		if (argv[i] == NULL)
			return NULL;

		switch (argv[i][0]) {
		case '+':
			op = CHDOSATTR_OP_ADD;
			break;
		case '-':
			op = CHDOSATTR_OP_SUB;
			break;
		case '=':
			op = CHDOSATTR_OP_ASS;
			break;
		default:
			continue;
		}

		switch (argv[i][0]) {
		case '-':
			/*
			 * Does this option string contain chars other than dos
			 * attr chars?
			 */
			attrs = get_attrfromstr(argv[i] + 1);
			if (attrs == 0)
				continue;

			goto found;
		/* args starting with '+' or '=' are definitely attr spec */
		case '+':
		case '=':
			attrs = get_attrfromstr(argv[i] + 1);
			/* '=' operator can used with no operand. i.e. clear everything */
			if (attrs == 0 && op != CHDOSATTR_OP_ASS) {
				fprintf(stderr,
					"%s: %s: invalid attribute spec\n",
					program_name, argv[i] + 1);
				usage();
			}

			goto found;
		}
	}

	return NULL;
found:
	ui.attr.op = op;
	ui.attr.operand = attrs;
	return do_extract_argv(i, argc, argv);
}

static bool do_set_attrs(const int fd, const char *path)
{
	uint32_t attrs;

	assert(ui.attr.op > CHDOSATTR_OP_NONE && ui.attr.op < NB_CHDOSATTR);

	switch (ui.attr.op) {
	case CHDOSATTR_OP_ADD:
	case CHDOSATTR_OP_SUB:
		attrs = 0;
		if (ioctl(fd, FAT_IOCTL_GET_ATTRIBUTES, &attrs) != 0) {
			exfat_err("%s: getattr %s: %s\n",
				program_name, path, strerror(errno));
			return false;
		}
		break;
	case CHDOSATTR_OP_ASS:
		attrs = ui.attr.operand;
		break;
	default:
		break;
	}

	switch (ui.attr.op) {
	case CHDOSATTR_OP_ADD:
		attrs |= ui.attr.operand;
		break;
	case CHDOSATTR_OP_SUB:
		attrs &= ~ui.attr.operand;
		break;
	default:
		break;
	}

	if (ioctl(fd, FAT_IOCTL_SET_ATTRIBUTES, &attrs) != 0) {
		exfat_err("%s: setattr %s: %s\n",
			program_name, path, strerror(errno));
		return false;
	}

	return true;
}

static int do_chdosattr(FTSENT *ent, bool *skip)
{
	int fd;
	bool ret;

	if (!ui.f.recursive && S_ISDIR(ent->fts_statp->st_mode))
		/* Don't visit the children */
		*skip = true;

	/*
	 * FIXME: should check if fstatfs() fstype is exFAT or vfat before doing
	 * ioctl()?
	 */

	fd = open(ent->fts_accpath, O_RDONLY);
	if (fd < 0) {
		exfat_err("%s: open %s: %s\n",
			program_name, ent->fts_path, strerror(errno));
		return 1;
	}

	ret = do_set_attrs(fd, ent->fts_path);

	close(fd);

	return ret ? 0 : -1;
}

int main(int argc, char *argv[])
{
	static int c;
	static int retval;

	setlocale(LC_MESSAGES, "");
	setlocale(LC_CTYPE, "");

	if (argc <= 1)
		usage();

	extract_attr_arg(&argc, argv);

	while ((c = getopt(argc, argv, "RVH")) != -1) {
		switch (c) {
		case 'R':
			ui.f.recursive = true;
			break;
		case 'V':
			ui.f.version_only = true;
			break;
		case 'H':
			ui.f.help = true;
			break;
		default:
			usage();
		}
	}

	if (ui.f.version_only)
		show_version();
	if (ui.f.help)
		usage();
	if (ui.f.help || ui.f.version_only)
		exit(DOSATTR_EXIT_USAGE);

	if (ui.attr.op == CHDOSATTR_OP_NONE) {
		fprintf(stderr, "%s: no operation specified. Use -H option for help\n",
			program_name);
		exit(DOSATTR_EXIT_USAGE);
	}

	if (optind > argc - 1) {
		fprintf(stderr, "%s: no file specified. Use -H option for help\n",
			program_name);
		exit(DOSATTR_EXIT_USAGE);
	} else
		retval = dosattr_fts((char * const *)(argv + optind),
				do_chdosattr, FTS_PHYSICAL);

	return retval;
}
