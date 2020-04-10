AM_CFLAGS = -include $(top_srcdir)/config.h -I$(top_srcdir)/include -fno-common
LIBS = -lm
mkfs_exfat_LDADD = $(top_builddir)/lib/libexfat.la

sbin_PROGRAMS = mkfs.exfat

mkfs_exfat_SOURCES = mkfs.c upcase.c
