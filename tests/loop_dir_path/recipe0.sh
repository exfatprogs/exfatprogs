#!/bin/bash
#
# Creates directory loops:
#
#  - loops2root: a directory with the first cluster pointing to the root
#    dentries
#  - loops2parent: a directory with the first cluster pointing to the parent
#    dentries
#
# Also calculates and sets the dentry checksums correctly. This case almost
# never happens in normal implementations. It's only a possibility with
# malicious intents.
#
# DO NOT actually mount the image on your system. You're gonna have a bad time.
# You've been warned.
#
# This is not actually run in CI or in any script... Yet.

[ $UID -ne 0 ] && echo "Run as root" >& 2 && exit 2

truncate -s 0 exfat.img
truncate -s 128M exfat.img
losetup /dev/loop42 exfat.img

mkfs.exfat /dev/loop42

mkdir -p /mnt/exfat.img
mount /dev/loop42 /mnt/exfat.img

pushd /mnt/exfat.img

mkdir parent deep loops2root leavemealone-d
touch leavemealone-f
mkdir parent/loops2root parent/loops2parent
mkdir deep/leavemealone-d deep/deeper
touch deep/leavemealone-f
mkdir deep/deeper/loops2root deep/deeper/loops2parent

popd

losetup -d /dev/loop42

python3 ./recipe1.py exfat.img
tar -S -Jcf exfat.img.tar.xz exfat.img
