#!/usr/bin/env bash

TESTCASE=$1
NEED_LOOPDEV=$2
IMAGE_FILE=exfat.img
FSCK_PROG=${FSCK1:-"fsck.exfat"}
FSCK_PROG_2=${FSCK2:-"fsck.exfat"}
FSCK_OPTS="-y -s"
DUMP_PROG=${DUMP1:-"dump.exfat"}
EXFAT2IMG_PROG=${EXFAT2IMG1:-"exfat2img"}
PASS_COUNT=0
RUN_TIMEOUT=60

cleanup() {
	echo ""
	echo "Passed ${PASS_COUNT} of ${TEST_COUNT}"
	if [ ${PASS_COUNT} -ne ${TEST_COUNT} ]; then
		exit 1
	else
		exit 0
	fi
}

raise_timeout() {
	if [ "$1" -eq 124 ]; then
		echo ""
		echo "Command timed out"
		exit 124
	fi
}

if [ $# -eq 0 ]; then
	TESTCASE_LIST=($(find . -name "${IMAGE_FILE}.tar.xz" -exec dirname {} \;))
	TESTCASE_LIST+=($(find . -name "[[:digit:]]*.sh" | sort))
else
	TESTCASE_LIST=($@)
fi

TEST_COUNT=${#TESTCASE_LIST[*]}

for TESTCASE in ${TESTCASE_LIST[*]}; do
	if [ ! -e "${TESTCASE}/${IMAGE_FILE}.tar.xz" -a ! -e ${TESTCASE} ]; then
		TEST_COUNT=$((TEST_COUNT - 1))
		continue
	fi

	echo "Running ${TESTCASE}"
	echo "-----------------------------------"

	# Create a corrupted image
	rm -f ${IMAGE_FILE}
	if [ -e "${TESTCASE}/${IMAGE_FILE}.tar.xz" ]; then
		tar -C . -xf "${TESTCASE}/${IMAGE_FILE}.tar.xz"
	else
		./${TESTCASE} ${IMAGE_FILE}
	fi

	if [ ! -e ${IMAGE_FILE} ]; then
		echo ""
		echo "Failed to create corrupted image"
		cleanup
	fi

	# Set up image file as loop device
	if [ $NEED_LOOPDEV ]; then
		DEV_FILE=$(losetup -f "${IMAGE_FILE}" --show)
	else
		DEV_FILE=$IMAGE_FILE
	fi

	# Run fsck to detect corruptions
	timeout $RUN_TIMEOUT $FSCK_PROG "$DEV_FILE" | grep -q "ERROR:\|corrupted"
	if [ $? -ne 0 ]; then
		echo ""
		echo "Failed to detect corruption for ${TESTCASE}"
		if [ $NEED_LOOPDEV ]; then
			losetup -d "${DEV_FILE}"
		fi
		cleanup
	fi

	# Run other utils to see if it copes well with the errors
	# Exit codes are not checked
	timeout $RUN_TIMEOUT $DUMP_PROG "$DEV_FILE" < /dev/null > /dev/null
	ec=$?
	raise_timeout $ec
	[ $ec -ne 0 ] && echo "dump.exfat returned $ec"
	timeout $RUN_TIMEOUT $EXFAT2IMG_PROG -o - "$DEV_FILE" < /dev/null > /dev/null
	ec=$?
	raise_timeout $ec
	[ $ec -ne 0 ] && echo "exfat2img returned $ec"

	# Run fsck for repair
	timeout $RUN_TIMEOUT $FSCK_PROG $FSCK_OPTS "$DEV_FILE"
	ec=$?
	raise_timeout $ec
	if [ $ec -ne 1 ] && [ $ec -ne 0 ]; then
		echo ""
		echo "Failed to repair ${TESTCASE}"
		if [ $NEED_LOOPDEV ]; then
			losetup -d "${DEV_FILE}"
		fi
		cleanup
	fi

	# Now the utils SHOULD be happy with the fixed volume.
	# If not, fsck failed to do its job.
	timeout $RUN_TIMEOUT $DUMP_PROG "$DEV_FILE" < /dev/null > /dev/null
	ec=$?
	raise_timeout $ec
	[ $ec -ne 0 ] && echo "FAIL: dump.exfat returned $ec !!!" && exit 1
	timeout $RUN_TIMEOUT $EXFAT2IMG_PROG -o - "$DEV_FILE" < /dev/null > /dev/null
	ec=$?
	raise_timeout $ec
	[ $ec -ne 0 ] && echo "FAIL: dump.exfat returned $ec !!!" && exit 1

	echo ""
	# Run fsck again
	timeout $RUN_TIMEOUT $FSCK_PROG_2 "$DEV_FILE"
	ec=$?
	raise_timeout $ec
	if [ $ec -ne 0 ]; then
		echo ""
		echo "Failed, corrupted ${TESTCASE}"
		if [ $NEED_LOOPDEV ]; then
			losetup -d "${DEV_FILE}"
		fi
		cleanup
	fi

	echo ""
	echo "Passed ${TESTCASE}"
	PASS_COUNT=$((PASS_COUNT + 1))

	if [ $NEED_LOOPDEV ]; then
		losetup -d "${DEV_FILE}"
	fi
done
cleanup
