## Makefile.am

ACLOCAL_AMFLAGS = -I m4

SUBDIRS = lib mkfs fsck tune label dump exfat2img dosattr

# manpages
dist_man1_MANS =
dist_man8_MANS =		\
	manpages/fsck.exfat.8	\
	manpages/tune.exfat.8	\
	manpages/mkfs.exfat.8	\
	manpages/exfatlabel.8	\
	manpages/dump.exfat.8	\
	manpages/exfat2img.8

# other stuff
EXTRA_DIST =			\
	include			\
	tests			\
	Android.bp		\
	lib/Android.bp		\
	mkfs/Android.bp		\
	tune/Android.bp		\
	fsck/Android.bp		\
	label/Android.bp	\
	dump/Android.bp		\
	exfat2img/Android.bp	\
	README.md

if ENABLE_DOSATTR
dist_man1_MANS +=		\
	manpages/lsdosattr.1	\
	manpages/chdosattr.1

endif
