#!/usr/bin/env bash

TESTCASE=$1
NEED_LOOPDEV=$2
IMAGE_FILE=exfat.img
FSCK_PROG=${FSCK1:-"fsck.exfat"}
FSCK_PROG_2=${FSCK2:-"fsck.exfat"}
FSCK_OPTS="-y -s"
PASS_COUNT=0

cleanup() {
	echo ""
	echo "Passed ${PASS_COUNT} of ${TEST_COUNT}"
	if [ ${PASS_COUNT} -ne ${TEST_COUNT} ]; then
		exit 1
	else
		exit 0
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
	$FSCK_PROG "$DEV_FILE" | grep -q "ERROR:\|corrupted"
	if [ $? -ne 0 ]; then
		echo ""
		echo "Failed to detect corruption for ${TESTCASE}"
		if [ $NEED_LOOPDEV ]; then
			losetup -d "${DEV_FILE}"
		fi
		cleanup
	fi

	# Run fsck for repair
	$FSCK_PROG $FSCK_OPTS "$DEV_FILE"
	if [ $? -ne 1 ] && [ $? -ne 0 ]; then
		echo ""
		echo "Failed to repair ${TESTCASE}"
		if [ $NEED_LOOPDEV ]; then
			losetup -d "${DEV_FILE}"
		fi
		cleanup
	fi

	echo ""
	# Run fsck again
	$FSCK_PROG_2 "$DEV_FILE"
	if [ $? -ne 0 ]; then
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
