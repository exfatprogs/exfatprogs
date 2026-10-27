#!/bin/bash
# Test EIO bail out behaviour of fsck.exfat: fsck.exfat mustn't modify the
# volume with no issues just because the underlying hardware is faulty. The
# script requires root privileges for dmsetup. Please run it on a VM.
#
#   1. Set up a loop device
#   2. Format the loop device
#   3. Mount the loop device
#   4. Populate a deep directory structure on the device
#   5. Unmount the device
#   6. Control run: run fsck.exfat on the device(expect exit code 0)
#   7. Hash the loop device (to ensure no modification of the filesystem upon EIO)
#   8. Set up a dm-dust device
#   9. For each logical sector of the device
#      a. Set the sector "read" bad (write should succeed)
#      b. Run fsck.exfat ...
#      c. Hash the loop device and confirm no modification of the device contents
#
# This is brute force testing so the script is quite slow even on
# enterprise-grade machines. To run the test in parallel, use the 1st and the
# 2nd args, which are the badblock offset and the step, respectively. For
# example, to run two instances in parallel,
#
#   ./test-bads.sh 0 2 &
#   ./test-bads.sh 1 2 &
#
set -e
declare -r PRINTABLES=( {a..z} {A..Z} {0..9} )
declare -r PRINTABLES_CNT=${#PRINTABLES[@]}
# parallel run params
[ -z "$1" ] && declare -r RUN_OFS="0" || declare -r RUN_OFS="$1"
[ -z "$2" ] && declare -r RUN_STEP="1" || declare -r RUN_STEP="$2"

declare -r IMG_FILE="$(mktemp)"
declare -r IMG_BACKUP="$(mktemp)"
declare -r IMG_SIZE="67108864"
declare -r IMG_SS=$((IMG_SIZE / 512))
declare -r LOOP="/dev/disk/by-loop-inode/$(stat -c '%Hd:%Ld-%i' "$IMG_FILE")"
[ -z "$MKFS_EXFAT" ] && MKFS_EXFAT="mkfs.exfat"
[ -z "$FSCK_EXFAT" ] && FSCK_EXFAT="fsck.exfat"
declare -r DUST_NAME="dustexfatbads$RUN_OFS"

hash_img()
{
	# Use CRC32C for speed. Ignore LBA 0 which contains volume dirty flag
	dd status=none if="$1" skip=1 | rhash --one-hash -C -
}

get_rand_name()
{
	local cnt="$1"
	local ret=""

	for (( i = 0; i < cnt; i++ ))
	do
		ret="$ret${PRINTABLES[RANDOM % PRINTABLES_CNT]}"
	done

	echo "$ret"
}

bomb_dir()
{
	local -r TLDS=(
		"0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
		"天下皆知美之為美，斯惡已；皆知善之為善，斯不善已。故有無相生，難易相成，長短相形，高下相傾，音聲相和，前後相隨。"
		"吾輩は猫である。名前はまだ無い。どこで生れたかとんと見当がつかぬ。何でも薄暗いじめじめした所でニャーニャー泣いていた事だけは記憶している。"
		"모든인간은태어날때부터자유로우며그존엄과권리에있어동등하다.인간은천부적으로이성과양심을부여받았으며서로형제애의정신으로행동하여야한다."
	)

	for tld in "${TLDS[@]}"
	do
		mkdir -p "$tld"
		pushd "$tld" > /dev/null

		mkdir -p {a,b,c}/{1,2,3}/{X,Y,Z}
		set -o pipefail
		find -mindepth 1 -type d | while read dir
		do
			pushd "$dir" > /dev/null
			for (( i = 0; i < 64; i++ ))
			do
				truncate -s $((i + 1)) "f-$(get_rand_name $i)"
			done
			popd > /dev/null
		done
		set +o pipefail

		popd > /dev/null
	done
}

cleanup()
{
	dmsetup remove "$DUST_NAME"
	udisksctl loop-delete -b "$LOOP"
	rm -f "$IMG_FILE" "$IMG_BACKUP"
}

dmsetup remove "$DUST_NAME" > /dev/null 2> /dev/null || true

fallocate -l "$IMG_SIZE" "$IMG_FILE"
"$MKFS_EXFAT" "$IMG_FILE"

set +e
udisksctl loop-setup --file "$IMG_FILE"
ec=$?
if [ "$ec" -ne 0 ]; then
	exit "$ec"
fi
set -e

udisksctl mount -b "$LOOP"
declare -r MP="$(findmnt "$LOOP" -no TARGET)"

pushd "$MP"
bomb_dir
popd

udisksctl unmount -b "$LOOP"

cat "$IMG_FILE" > "$IMG_BACKUP"
declare -r HASH="$(hash_img "$LOOP")"
echo "image: $IMG_FILE"
echo "backup: $IMG_BACKUP"

"$FSCK_EXFAT" -y "$IMG_FILE"

if [ "$HASH" != "$(hash_img "$LOOP")" ]
then
	echo "FAIL: the filesystem implementation corrupted the filesystem or there's a problem in fsck.exfat" >&2
	exit 1
fi

dmsetup create "$DUST_NAME" --table "0 $IMG_SS dust $LOOP 0 512"
dmsetup message "$DUST_NAME" 0 enable

set +e

for (( i = RUN_OFS; i < IMG_SS; i += RUN_STEP ))
do
	dmsetup message "$DUST_NAME" 0 addbadblock "$i"
	valgrind -q --error-exitcode=100 "$FSCK_EXFAT" -y "/dev/mapper/$DUST_NAME" > /dev/null
	ec=$?
	dmsetup message "$DUST_NAME" 0 removebadblock "$i" > /dev/null 2> /dev/null
	sync "/dev/mapper/$DUST_NAME"

	if [ "$HASH" != "$(hash_img "$LOOP")" ]
	then
		echo "FAIL: fsck.exfat modified the filesystem (badblock: $i)" >&2
		exit 1
	elif [ "$ec" -eq 100 ]
	then
		# Didn't modify the volume but there's a memory bug??
		echo "FAIL: fsck.exfat experienced memory error (badblock: $i)" >&2
		exit 1
	fi
done

echo "OK: fsck.exfat does not modify the filesystem on EIO"
cleanup
