// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2020 Namjae Jeon <linkinjeon@kernel.org>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <locale.h>

#include "exfat_ondisk.h"
#include "libexfat.h"

static void usage(void)
{
	fprintf(stderr, "Usage: exfatlabel\n");
	fprintf(stderr, "\t-i | --volume-serial                  Switch to volume serial mode\n");
	fprintf(stderr, "\t-V | --version                        Show version\n");
	fprintf(stderr, "\t-h | --help                           Show help\n");

	exit(EXIT_FAILURE);
}

static struct option opts[] = {
	{"volume-serial",	no_argument,		NULL,	'i' },
	{"version",		no_argument,		NULL,	'V' },
	{"help",		no_argument,		NULL,	'h' },
	{"?",			no_argument,		NULL,	'?' },
	{NULL,			0,			NULL,	 0  }
};

int main(int argc, char *argv[])
{
	int c;
	int ret = EXIT_FAILURE;
	struct exfat_blk_dev bd;
	struct exfat_user_input ui;
	bool version_only = false;
	off_t root_clu_off;
	int serial_mode = 0;
	int flags = 0;

	init_user_input(&ui);

	if (!setlocale(LC_CTYPE, ""))
		exfat_err("failed to init locale/codeset\n");

	if (argc == 2)
		flags = EXFAT_GET_VOLUME_LABEL;
	else if (argc == 3)
		flags = EXFAT_SET_VOLUME_LABEL;

	opterr = 0;
	while ((c = getopt_long(argc, argv, "iVh", opts, NULL)) != EOF)
		switch (c) {
		case 'i':
			serial_mode = true;
			if (argc == 3)
				flags = EXFAT_GET_VOLUME_SERIAL;
			else if (argc == 4)
				flags = EXFAT_SET_VOLUME_SERIAL;

			break;
		case 'V':
			version_only = true;
			break;
		case '?':
		case 'h':
		default:
			usage();
	}

	show_version();
	if (version_only)
		exit(EXIT_FAILURE);

	if (argc < 2)
		usage();

	memset(ui.dev_name, 0, sizeof(ui.dev_name));
	snprintf(ui.dev_name, sizeof(ui.dev_name), "%s", argv[serial_mode + 1]);

	ret = exfat_get_blk_dev_info(&ui, &bd);
	if (ret < 0)
		goto out;

	if (serial_mode) {
		/* Mode to change or display volume serial */
		if (flags == EXFAT_GET_VOLUME_SERIAL) {
			ret = exfat_show_volume_serial(&bd, &ui);
		} else if (flags == EXFAT_SET_VOLUME_SERIAL) {
			ui.volume_serial = strtoul(argv[3], NULL, 0);
			ret = exfat_change_volume_serial(&bd, &ui);
		}
	} else {
		/* Mode to change or display volume label */
		root_clu_off = exfat_get_root_entry_offset(&bd);
		if (root_clu_off < 0)
			goto out;

		if (flags == EXFAT_GET_VOLUME_LABEL)
			ret = exfat_get_volume_label(&bd, root_clu_off);
		else if (flags == EXFAT_SET_VOLUME_LABEL)
			ret = exfat_set_volume_label(&bd, argv[2], root_clu_off);
	}

out:
	return ret;
}
