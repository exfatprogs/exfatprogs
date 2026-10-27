AM_CFLAGS = -Wall -include $(top_builddir)/config.h -I$(top_srcdir)/include -fno-common
noinst_LIBRARIES = libexfat.a

libexfat_a_SOURCES = libexfat.c exfat_fs.c exfat_dir.c utils.c

check_PROGRAMS = tests/suite0000-bitmap tests/suite0000-dump-arch tests/suite0001-parse
TESTS = tests/suite0000-bitmap tests/suite0000-dump-arch tests/suite0001-parse

tests_suite0000_dump_arch_SOURCES = tests/suite0000-dump-arch.c
tests_suite0000_dump_arch_LDADD = libexfat.a

tests_suite0000_bitmap_SOURCES = tests/suite0000-bitmap.c
tests_suite0000_bitmap_LDADD = libexfat.a

tests_suite0001_parse_SOURCES = tests/suite0001-parse.c
tests_suite0001_parse_LDADD = libexfat.a

if ENABLE_TESTS
noinst_PROGRAMS = $(check_PROGRAMS)
endif
