// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2019 Namjae Jeon <linkinjeon@kernel.org>
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include "exfat_ondisk.h"
#include "libexfat.h"
#include "mkfs.h"
#include "upcase_table.h"

int exfat_create_upcase_table(struct exfat_blk_dev *bd,
		struct exfat_user_input *ui)
{
	int nbytes;
	int ret;

	nbytes = pwrite(bd->dev_fd, default_upcase_table,
			EXFAT_UPCASE_TABLE_SIZE, finfo.ut_byte_off);
	if (nbytes != EXFAT_UPCASE_TABLE_SIZE)
		return -1;

	if (ui->verify) {
		ret = exfat_check_written_data(bd,
				default_upcase_table,
				EXFAT_UPCASE_TABLE_SIZE,
				finfo.ut_byte_off,
				"upcase table");
		if (ret) {
			exfat_err("upcase table verification failed (read-back mismatch)\n");
			return ret;
		}
	}

	return 0;
}
