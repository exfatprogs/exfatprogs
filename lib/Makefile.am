AM_CFLAGS = -Wall -include $(top_builddir)/config.h -I$(top_srcdir)/include -fno-common
noinst_LIBRARIES = libexfat.a

libexfat_a_SOURCES = libexfat.c exfat_fs.c exfat_dir.c utils.c

if ENABLE_TESTS

noinst_PROGRAMS = tests/suite0000-bitmap

tests_suite0000_bitmap_SOURCES = tests/suite0000-bitmap.c
tests_suite0000_bitmap_LDADD = libexfat.a

endif
