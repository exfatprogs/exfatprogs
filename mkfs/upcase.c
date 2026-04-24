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
	off_t zero_len;
	int ret;

	assert(finfo.root_byte_off >= finfo.ut_byte_off);

	if (!exfat_write_full(bd->dev_fd, ui->upcase.table, ui->upcase.len, finfo.ut_byte_off))
		return -1;
	if (ui->verify) {
		ret = exfat_check_written_data(bd,
					       ui->upcase.table,
					       ui->upcase.len,
					       finfo.ut_byte_off,
					       "upcase table");
		if (ret)
			goto verify_failed;
	}

	zero_ofs = finfo.ut_byte_off + ui->upcase.len;
	zero_len = finfo.root_byte_off - finfo.ut_byte_off - ui->upcase.len;
	ret = exfat_write_zero(bd->dev_fd, zero_len, zero_ofs);
	if (ret)
		return ret;
	if (ui->verify) {
		ret = exfat_check_written_data(bd, NULL, zero_len, zero_ofs, "upcase table");
		if (ret)
			goto verify_failed;
	}

	return 0;

verify_failed:
	exfat_err("upcase table verification failed (read-back mismatch)\n");
	return ret;
}
