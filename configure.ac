AC_PREREQ([2.68])

m4_define([exfat_tools_major_ver], [1])
m4_define([exfat_tools_minor_ver], [0])
m4_define([exfat_tools_micro_ver], [1])

m4_define([exfat_tools_version],
	[exfat_tools_major_ver.exfat_tools_minor_ver.exfat_tools_micro_ver])

AC_INIT([CIFSD tools], [exfat_tools_version],
	[linkinjeon@gmail.com])

AC_DEFINE([EXFAT_TOOLS_VERSION], "exfat_tools_version", [exfat-tools version])

AC_CONFIG_SRCDIR([config.h.in])
AC_CONFIG_HEADER([config.h])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR([build-aux])
AM_INIT_AUTOMAKE([foreign tar-pax dist-xz subdir-objects])

AC_LANG([C])
AC_PROG_CC
AC_PROG_CC_STDC
AM_SILENT_RULES([yes])
AC_PROG_LIBTOOL
AC_SYS_LARGEFILE

PKG_PROG_PKG_CONFIG([0.9])

AC_CONFIG_FILES([
	Makefile
	lib/Makefile
	mkfs/Makefile
	fsck/Makefile
])

AC_OUTPUT
