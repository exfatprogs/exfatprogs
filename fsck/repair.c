/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Copyright (C) 2020 Hyunchul Lee <hyc.lee@gmail.com>
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>

#include "exfat_ondisk.h"
#include "libexfat.h"
#include "repair.h"
#include "exfat_fs.h"
#include "fsck.h"

struct exfat_repair_problem {
	er_problem_code_t	prcode;
	unsigned int		flags;
	unsigned int		prompt_type;
	unsigned int		default_number;
	unsigned int		bypass_number;
	unsigned int		max_number;
};

/* Problem flags */
#define ERF_PREEN_YES		0x00000001
#define ERF_DEFAULT_YES		0x00000002
#define ERF_DEFAULT_NO		0x00000004

/* Prompt types */
#define ERP_FIX			0x00000001
#define ERP_TRUNCATE		0x00000002
#define ERP_DELETE		0x00000003
#define ERP_RENAME		0x00000004

static const char *prompts[] = {
	"Repair",
	"Fix",
	"Truncate",
	"Delete",
	"Select",
};

static struct exfat_repair_problem problems[] = {
	{ER_BS_CHECKSUM, ERF_PREEN_YES, ERP_FIX, 0, 0, 0},
	{ER_BS_BOOT_REGION, 0, ERP_FIX, 0, 0, 0},
	{ER_DE_CHECKSUM, ERF_PREEN_YES, ERP_DELETE, 0, 0, 0},
	{ER_DE_UNKNOWN, ERF_PREEN_YES, ERP_DELETE, 0, 0, 0},
	{ER_DE_UNUSED, ERF_PREEN_YES, ERP_FIX, 0, 0, 0},
	{ER_DE_FILE, ERF_PREEN_YES, ERP_DELETE, 0, 0, 0},
	{ER_DE_UPCASE, ERF_PREEN_YES, ERP_FIX, 0, 0, 0},
	{ER_DE_SECONDARY_COUNT, ERF_PREEN_YES, ERP_FIX, 0, 0, 0},
	{ER_DE_STREAM, ERF_PREEN_YES, ERP_DELETE, 0, 0, 0},
	{ER_DE_NAME, ERF_PREEN_YES, ERP_DELETE, 0, 0, 0},
	{ER_DE_NAME_HASH, ERF_PREEN_YES, ERP_FIX, 0, 0, 0},
	{ER_DE_NAME_LEN, ERF_PREEN_YES, ERP_FIX, 0, 0, 0},
	{ER_DE_DOT_NAME, ERF_PREEN_YES, ERP_RENAME, 2, 3, 4},
	{ER_DE_DUPLICATED_NAME, ERF_PREEN_YES, ERP_RENAME, 2, 3, 4},
	{ER_DE_INVALID_NAME, ERF_PREEN_YES, ERP_RENAME, 2, 3, 4},
	{ER_FILE_VALID_SIZE, ERF_PREEN_YES, ERP_FIX, 0, 0, 0},
	{ER_FILE_INVALID_CLUS, ERF_PREEN_YES, ERP_TRUNCATE, 0, 0, 0},
	{ER_FILE_FIRST_CLUS, ERF_PREEN_YES, ERP_TRUNCATE, 0, 0, 0},
	{ER_FILE_SMALLER_SIZE, ERF_PREEN_YES, ERP_TRUNCATE, 0, 0, 0},
	{ER_FILE_LARGER_SIZE, ERF_PREEN_YES, ERP_TRUNCATE, 0, 0, 0},
	{ER_FILE_DUPLICATED_CLUS, ERF_PREEN_YES, ERP_TRUNCATE, 0, 0, 0},
	{ER_FILE_ZERO_NOFAT, ERF_PREEN_YES, ERP_FIX, 0, 0, 0},
	{ER_VENDOR_GUID, ERF_DEFAULT_NO, ERP_FIX, 0, 0, 0},
};

static struct exfat_repair_problem *find_problem(er_problem_code_t prcode)
{
	unsigned int i;

	for (i = 0; i < sizeof(problems)/sizeof(problems[0]); i++) {
		if (problems[i].prcode == prcode) {
			return &problems[i];
		}
	}
	return NULL;
}

static int ask_repair(struct exfat_fsck *fsck, struct exfat_repair_problem *pr)
{
	int repair = 0;
	char answer[8];

	if (fsck->options & FSCK_OPTS_REPAIR_NO ||
	    pr->flags & ERF_DEFAULT_NO)
		repair = 0;
	else if (fsck->options & FSCK_OPTS_REPAIR_YES ||
		 pr->flags & ERF_DEFAULT_YES)
		repair = 1;
	else {
		if (fsck->options & FSCK_OPTS_REPAIR_ASK) {
			do {
				if (pr->prompt_type & ERP_RENAME) {
					printf("%s (Number: ?) ",
					       prompts[pr->prompt_type]);
				} else {
					printf(". %s (y/N)? ",
					       prompts[pr->prompt_type]);
				}
				fflush(stdout);

				if (!fgets(answer, sizeof(answer), stdin))
					continue;

				if (pr->prompt_type & ERP_RENAME) {
					unsigned int number = atoi(answer);

					if (number > 0 && number < pr->max_number)
						return number;
				} else {
					if (strcasecmp(answer, "Y\n") == 0)
						return 1;
					else if (strcasecmp(answer, "\n") == 0 ||
						 strcasecmp(answer, "N\n") == 0)
						return 0;
				}
			} while (1);
		} else if (fsck->options & FSCK_OPTS_REPAIR_AUTO &&
			   pr->flags & ERF_PREEN_YES)
			repair = 1;
	}

	if (pr->prompt_type & ERP_RENAME) {
		int print_num = repair ? pr->default_number : pr->bypass_number;

		printf("%s (Number : %d)\n", prompts[pr->prompt_type],
		       print_num);
		repair = print_num;
	} else {
		printf(". %s (y/N)? %c\n", prompts[pr->prompt_type],
		       repair ? 'y' : 'n');
	}
	return repair;
}

int exfat_repair_ask(struct exfat_fsck *fsck, er_problem_code_t prcode,
		     const char *desc, ...)
{
	struct exfat_repair_problem *pr = NULL;
	va_list ap;
	int repair;

	pr = find_problem(prcode);
	if (!pr) {
		exfat_err("unknown problem code. %#x\n", prcode);
		return 0;
	}

	va_start(ap, desc);
	vprintf(desc, ap);
	va_end(ap);

	repair = ask_repair(fsck, pr);
	if (repair) {
		if (pr->prompt_type & ERP_TRUNCATE)
			fsck->dirty_fat = true;
		fsck->dirty = true;
	}
	return repair;
}

static int get_rename_from_user(struct exfat_de_iter *iter,
		__le16 *utf16_name, int name_size)
{
	int len = 0;
	char *rename = malloc(ENTRY_NAME_MAX + 2);

	if (!rename)
		return -ENOMEM;

retry:
	/* +2 means LF(Line Feed) and NULL terminator */
	memset(rename, 0x1, ENTRY_NAME_MAX + 2);
	printf("New name: ");
	if (fgets(rename, ENTRY_NAME_MAX + 2, stdin)) {
		int err;
		struct exfat_lookup_filter filter;

		len = strlen(rename);
		/* Remove LF in filename */
		rename[len - 1] = '\0';

		memset(utf16_name, 0, name_size);
		len = exfat_utf16_enc(rename, utf16_name, name_size);
		if (len < 0)
			goto out;

		err = exfat_check_name(utf16_name, len >> 1);
		if (err != len >> 1) {
			printf("filename contain invalid character(%c)\n",
					le16_to_cpu(utf16_name[err]));
			goto retry;
		}

		exfat_de_iter_flush(iter);
		err = exfat_lookup_file(iter->exfat, iter->parent, rename, &filter);
		if (!err) {
			printf("file(%s) already exists, retry to insert name\n", rename);
			goto retry;
		}
	}

out:
	free(rename);

	return len;
}

static int generate_rename(struct exfat_de_iter *iter, __le16 *utf16_name,
		int name_size)
{
	int err;
	char *rename;

	if (iter->invalid_name_num > INVALID_NAME_NUM_MAX)
		return -ERANGE;

	rename = malloc(ENTRY_NAME_MAX + 1);
	if (!rename)
		return -ENOMEM;

	while (1) {
		struct exfat_lookup_filter filter;

		snprintf(rename, ENTRY_NAME_MAX + 1, "FILE%07d.CHK",
			 iter->invalid_name_num++);
		err = exfat_lookup_file(iter->exfat, iter->parent, rename,
					&filter);
		if (!err)
			continue;
		break;
	}

	memset(utf16_name, 0, name_size);
	err = exfat_utf16_enc(rename, utf16_name, name_size);
	free(rename);

	return err;
}

int exfat_repair_rename_ask(struct exfat_fsck *fsck, struct exfat_de_iter *iter,
		__le16 *uname, er_problem_code_t prcode, char *error_msg)
{
	int num;
	char old_name[PATH_MAX + 1] = {0};

	if (exfat_utf16_dec(uname, NAME_BUFFER_SIZE, old_name, PATH_MAX) <= 0) {
		exfat_err("failed to decode filename\n");
		return -EINVAL;
	}

ask_again:
	num = exfat_repair_ask(fsck, prcode, "ERROR: '%s' %s.\n%s",
			old_name, error_msg,
			" [1] Insert the name you want to rename.\n"
			" [2] Automatically renames filename.\n"
			" [3] Bypass this check(No repair)\n");
	if (num) {
		__le16 utf16_name[ENTRY_NAME_MAX];
		__u16 hash;
		struct exfat_dentry *dentry;
		int ret;

		switch (num) {
		case 1:
			ret = get_rename_from_user(iter, utf16_name,
					sizeof(utf16_name));
			break;
		case 2:
			ret = generate_rename(iter, utf16_name,
					sizeof(utf16_name));
			break;
		case 3:
			return -EINVAL;
		default:
			exfat_info("select 1 or 2 number instead of %d\n", num);
			goto ask_again;
		}

		if (ret < 0)
			return -EINVAL;

		exfat_de_iter_get_dirty(iter, 2, &dentry);

		ret >>= 1;
		memcpy(dentry->name_unicode, utf16_name, ENTRY_NAME_MAX * 2);
		hash = exfat_calc_name_hash(iter->exfat, utf16_name, ret);
		exfat_de_iter_get_dirty(iter, 1, &dentry);
		dentry->stream_name_len = (__u8)ret;
		dentry->stream_name_hash = cpu_to_le16(hash);
		return 1;

	}

	return 0;
}
