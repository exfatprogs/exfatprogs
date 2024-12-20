#! /bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

img=${1:-"exfat.img"}

truncate -s 32M $img
mkfs.exfat $img >> /dev/null

dump_info=$(dump.exfat $img)
upcase_entry_off=$(echo "${dump_info}" | grep "Upcase table entry position:" | cut -d ':' -f 2)
size_off=$((${upcase_entry_off}+24))

# Make DataLength field corrupted
echo "$(printf "%x" $size_off):0x00" | xxd -r - $img
echo "$(printf "%x" $(($size_off + 1))):0x00" | xxd -r - $img
