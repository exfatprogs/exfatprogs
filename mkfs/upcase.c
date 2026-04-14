// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Copyright (C) 2019 Namjae Jeon <linkinjeon@kernel.org>
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>

#include "exfat_ondisk.h"
#include "libexfat.h"
#include "mkfs.h"

int exfat_create_upcase_table(struct exfat_blk_dev *bd,
		struct exfat_user_input *ui)
{
	off_t zero_ofs;
	size_t zero_len;
	ssize_t nbytes;
	int ret;

	assert(finfo.root_byte_off >= finfo.ut_byte_off);

	/*
	 * FIXME bytes written in [p]write() may be less than requested len.
	 * This won't work if the table is large enough.
	 *
	 * TODO do this in a loop.
	 */
	nbytes = pwrite(bd->dev_fd, ui->upcase.table, ui->upcase.len, finfo.ut_byte_off);
	if (nbytes != ui->upcase.len)
		return -1;

	zero_ofs = finfo.ut_byte_off + ui->upcase.len;
	zero_len = finfo.root_byte_off - finfo.ut_byte_off - ui->upcase.len;
	exfat_write_zero(bd->dev_fd, zero_len, zero_ofs);

	if (ui->verify) {
		ret = exfat_check_written_data(bd,
					       ui->upcase.table,
					       ui->upcase.len,
					       finfo.ut_byte_off,
					       "upcase table");
		if (ret) {
			exfat_err("upcase table verification failed (read-back mismatch)\n");
			return ret;
		}
	}

	return 0;
}
