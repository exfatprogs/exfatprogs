#! /bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

img=${1:-"exfat.img"}

truncate -s 32M $img
mkfs.exfat $img >> /dev/null
dump_info=$(dump.exfat $img)
clu_heap_off=$(echo "${dump_info}" | grep "Cluster Heap Offset (sector offset):" | cut -d ':' -f 2)
upcase_clu=$(echo "${dump_info}" | grep "Upcase table start cluster:" | cut -d ':' -f 2)
clu_size=$(echo "${dump_info}" | grep "Cluster size:" | cut -d ':' -f 2)
sector_size=$(echo "${dump_info}" | grep "Bytes per Sector:" | cut -d ':' -f 2)
upcase_entry_off=$(echo "${dump_info}" | grep "Upcase table entry position:" | cut -d ':' -f 2)

# Make TableChecksum field corrupted
checksum_off=$((${upcase_entry_off}+4))
echo "$(printf "%x" $checksum_off):0xff" | xxd -r - $img

# Make upcase table corrupted
upcase_off=$(((${upcase_clu} - 2) * ${clu_size} + ${clu_heap_off} * ${sector_size}))
echo "$(printf "%x" $upcase_off):0x7F" | xxd -r - $img
