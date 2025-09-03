AC_PREREQ([2.70])

m4_define([exfat_progs_version], m4_esyscmd_s(
	 grep "define EXFAT_PROGS_VERSION " include/version.h | \
		 awk '{print $3}' | sed 's/\"//g'))

AC_INIT([exfatprogs],
	exfat_progs_version,
	[linkinjeon@kernel.org],
	[exfatprogs],
	[https://github.com/exfatprogs/exfatprogs])

AC_CONFIG_SRCDIR([config.h.in])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([foreign tar-pax dist-xz subdir-objects])

AC_LANG([C])
AC_PROG_CC
AM_SILENT_RULES([yes])
LT_INIT
AC_SYS_LARGEFILE
AC_C_BIGENDIAN

AC_CONFIG_FILES([
	Makefile
	lib/Makefile
	mkfs/Makefile
	fsck/Makefile
	tune/Makefile
	label/Makefile
	dump/Makefile
	exfat2img/Makefile
	defrag/Makefile
])

AC_OUTPUT
