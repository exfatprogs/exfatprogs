if ENABLE_DOSATTR
AM_CFLAGS = -Wall -Wextra -include $(top_builddir)/config.h -I$(top_srcdir)/include -fno-common
lsdosattr_LDADD = $(top_builddir)/lib/libexfat.a
chdosattr_LDADD = $(top_builddir)/lib/libexfat.a

bin_PROGRAMS = lsdosattr chdosattr

noinst_HEADERS = dosattr.h

lsdosattr_SOURCES = lsdosattr.c
chdosattr_SOURCES = chdosattr.c
endif
