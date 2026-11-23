#! /bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# exfat2img must terminate on directory loops in a corrupted image:
#  loop:   a subdirectory whose FAT chain points back to itself while its
#          stream_size claims an enormous length.
#  parent: a subdirectory whose first cluster is the root directory.

EXFAT2IMG=${EXFAT2IMG:-"exfat2img"}
img=${1:-"exfat.img"}
size=${SIZE:-"32M"}
out=${img}.dump
failed=0

le32() {
	printf "%02x %02x %02x %02x" $(($1 & 0xff)) $((($1 >> 8) & 0xff)) \
		$((($1 >> 16) & 0xff)) $((($1 >> 24) & 0xff))
}

le64() {
	printf "%s %s" "$(le32 $(($1 & 0xffffffff)))" "$(le32 $(($1 >> 32)))"
}

field() {
	echo "${dump_info}" | grep "$1" | cut -d ':' -f 2 | tr -d ' '
}

run_case() {
	rm -f $img $out
	truncate -s $size $img
	mkfs.exfat $img >> /dev/null
	dump_info=$(dump.exfat $img)

	sect_size=$(field "Bytes per Sector:")
	fat_off=$(($(field "FAT Offset") * sect_size))
	heap_off=$(($(field "Cluster Heap Offset") * sect_size))
	root_clu=$(field "Root Cluster")
	clus_size=$(field "Cluster size:")

	# first free slot of the root directory, after label, bitmap, upcase
	slot=0
	for pos in $(echo "${dump_info}" | grep "entry position:" |
		     cut -d ':' -f 2); do
		[ $((pos)) -gt $slot ] && slot=$((pos))
	done
	slot=$((slot + 32))

	if [ $1 = loop ]; then
		clu=$((root_clu + 20))
		dir_size=0x7fffffffffffffff
	else
		clu=$root_clu
		dir_size=$clus_size
	fi

	# file (0x85, SUBDIR), stream (0xc0, FAT chain), name (0xc1)
	printf "%x: 85 02 00 00 10 00\n" $slot | xxd -r - $img
	printf "%x: c0 01 00 02\n" $((slot + 32)) | xxd -r - $img
	printf "%x: %s %s\n" $((slot + 52)) "$(le32 $clu)" \
		"$(le64 $dir_size)" | xxd -r - $img
	printf "%x: c1 00 63 00 79 00\n" $((slot + 64)) | xxd -r - $img

	if [ $1 = loop ]; then
		# FAT[clu] = clu, and no end-of-directory marker in that cluster
		printf "%x: %s\n" $((fat_off + clu * 4)) "$(le32 $clu)" |
			xxd -r - $img
		head -c $clus_size /dev/zero | tr '\0' '\377' |
			dd of=$img bs=$clus_size \
			   seek=$((heap_off + (clu - 2) * clus_size)) \
			   oflag=seek_bytes conv=notrunc status=none
	fi

	timeout 10 $EXFAT2IMG $img $out > /dev/null 2>&1
	ret=$?
	rm -f $img $out

	if [ $ret -eq 124 ]; then
		echo "Failed, exfat2img hung on directory $1"
		failed=1
	else
		echo "Passed exfat2img directory $1"
	fi
}

run_case loop
run_case parent
exit $failed
