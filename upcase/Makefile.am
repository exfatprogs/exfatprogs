upcase_bin_DATA = \
	upcase-mandatory.txt		\
	upcase-recommended.txt		\
	upcase-null.txt			\
	upcase-mandatory.bin		\
	upcase-recommended.bin		\
	upcase-null.bin

libexec_SCRIPTS = exfat-uctbl2bin.sh
upcase_bindir = $(docdir)

upcase-mandatory.bin: exfat-uctbl2bin.sh upcase-mandatory.txt
	./exfat-uctbl2bin.sh upcase-mandatory.txt > upcase-mandatory.tmp
	mv upcase-mandatory.tmp upcase-mandatory.bin
upcase-recommended.bin: exfat-uctbl2bin.sh upcase-recommended.txt
	./exfat-uctbl2bin.sh upcase-recommended.txt > upcase-recommended.tmp
	mv upcase-recommended.tmp upcase-recommended.bin
upcase-null.bin: exfat-uctbl2bin.sh upcase-null.txt
	./exfat-uctbl2bin.sh upcase-null.txt > upcase-null.tmp
	mv upcase-null.tmp upcase-null.bin
