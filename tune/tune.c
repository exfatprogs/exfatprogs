// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2019 Namjae Jeon <linkinjeon@kernel.org>
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
	fprintf(stderr, "Usage: tune.exfat\n");
	fprintf(stderr, "\t-l | --print-label                    Print volume label\n");
	fprintf(stderr, "\t-L | --set-label=label                Set volume label\n");
	fprintf(stderr, "\t-i | --print-serial                   Print volume serial\n");
	fprintf(stderr, "\t-L | --set-serial=value               Set volume serial\n");
	fprintf(stderr, "\t-V | --version                        Show version\n");
	fprintf(stderr, "\t-v | --verbose                        Print debug\n");
	fprintf(stderr, "\t-h | --help                           Show help\n");

	exit(EXIT_FAILURE);
}

static struct option opts[] = {
	{"print-label",		no_argument,		NULL,	'l' },
	{"set-label",		required_argument,	NULL,	'L' },
	{"print-serial",	no_argument,		NULL,	'i' },
	{"set-serial",		required_argument,	NULL,	'I' },
	{"version",		no_argument,		NULL,	'V' },
	{"verbose",		no_argument,		NULL,	'v' },
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
	int flags = 0;
	char label_input[VOLUME_LABEL_BUFFER_SIZE];
	off_t root_clu_off;

	init_user_input(&ui);

	if (!setlocale(LC_CTYPE, ""))
		exfat_err("failed to init locale/codeset\n");

	opterr = 0;
	while ((c = getopt_long(argc, argv, "I:iL:lVvh", opts, NULL)) != EOF)
		switch (c) {
		case 'l':
			flags = EXFAT_GET_VOLUME_LABEL;
			break;
		case 'L':
			snprintf(label_input, sizeof(label_input), "%s",
					optarg);
			flags = EXFAT_SET_VOLUME_LABEL;
			break;
		case 'i':
			flags = EXFAT_GET_VOLUME_SERIAL;
			break;
		case 'I':
			ui.volume_serial = strtoul(optarg, NULL, 0);
			flags = EXFAT_SET_VOLUME_SERIAL;
			break;
		case 'V':
			version_only = true;
			break;
		case 'v':
			print_level = EXFAT_DEBUG;
			break;
		case '?':
		case 'h':
		default:
			usage();
	}

	show_version();
	if (version_only)
		exit(EXIT_FAILURE);

	if (argc < 3)
		usage();

	memset(ui.dev_name, 0, sizeof(ui.dev_name));
	snprintf(ui.dev_name, sizeof(ui.dev_name), "%s", argv[argc - 1]);

	ret = exfat_get_blk_dev_info(&ui, &bd);
	if (ret < 0)
		goto out;

	/* Mode to change or display volume serial */
	if (flags == EXFAT_GET_VOLUME_SERIAL) {
		ret = exfat_show_volume_serial(&bd, &ui);
		goto out;
	} else if (flags == EXFAT_SET_VOLUME_SERIAL) {
		ret = exfat_change_volume_serial(&bd, &ui);
		goto out;
	}

	root_clu_off = exfat_get_root_entry_offset(&bd);
	if (root_clu_off < 0)
		goto out;

	if (flags == EXFAT_GET_VOLUME_LABEL)
		ret = exfat_get_volume_label(&bd, root_clu_off);
	else if (flags == EXFAT_SET_VOLUME_LABEL)
		ret = exfat_set_volume_label(&bd, label_input, root_clu_off);

out:
	return ret;
}
