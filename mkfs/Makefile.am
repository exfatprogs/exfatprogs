AM_CFLAGS = -Wall -include $(top_builddir)/config.h -I$(top_srcdir)/include -fno-common $(BLKID_CFLAGS)
mkfs_exfat_LDADD = $(top_builddir)/lib/libexfat.a $(BLKID_LIBS)

sbin_PROGRAMS = mkfs.exfat

mkfs_exfat_SOURCES = mkfs.c upcase.c mkfs.h crc.c
