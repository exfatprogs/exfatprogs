// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Prints attributes like integer widths and endianness to record the arch being
 * tested. The actual CPU arch being tested is not identified and the code is
 * arch agnostic for future proofing.
 *
 * The program always returns zero.
 */
#include <stdio.h>
#include <inttypes.h>
#include <limits.h>
#include <config.h>

#ifdef WORDS_BIGENDIAN
#define IS_BIGENDIAN	(1)
#else
#define IS_BIGENDIAN	(0)
#endif

int main(void)
{
	printf(	"\n"
		"CHAR_BIT:  %d\n"
		"BIGENDIAN: %d\n"
		"uintptr_t: %zu\n"
		"size_t:    %zu\n"
		"short:     %zu\n"
		"int:       %zu\n"
		"long:      %zu\n" /* for detecting LLP64 model(MSVC) */
		"long long: %zu\n",
		CHAR_BIT,
		IS_BIGENDIAN,
		sizeof(uintptr_t),
		sizeof(size_t),
		sizeof(short),
		sizeof(int),
		sizeof(long),
		sizeof(long long));

	return 0;
}
