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
	upcase/Makefile
	dosattr/Makefile
])

AC_CHECK_HEADERS([fts.h], [have_fts_h="yes"])
AM_CONDITIONAL([ENABLE_DOSATTR], [test "x$have_fts_h" == "xyes"])
AM_COND_IF(
	[ENABLE_DOSATTR],
	[AC_CHECK_LIB([fts], [fts_open])],
	[echo "<fts.h> not found. Not building dosattr programs"]
)

PKG_CHECK_MODULES([BLKID], [blkid >= 2.20],
	[
		AC_DEFINE([HAVE_LIBBLKID], [1], [Define to 1 if libblkid is available.])
	],
	[
		BLKID_CFLAGS=
		BLKID_LIBS=
		AC_MSG_WARN([libblkid not found or too old; foreign filesystem detection in mkfs.exfat will be disabled])
	]
)
AC_SUBST([BLKID_CFLAGS])
AC_SUBST([BLKID_LIBS])

AC_OUTPUT
