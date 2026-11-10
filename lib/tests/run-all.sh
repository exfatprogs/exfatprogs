#!/bin/sh
# Note: this is only a convenience script for running test suites with Valgrind.
# This is used in the CI pipelines(Github Actions). For normal cases, the tests
# may be run with `make check` as well.

main()
{
	local tot=0
	local ok=0
	local fail=0

	while read tc
	do
		tot=$(( tot + 1 ))

		echo -n "$tc: "
		eval $TEST_PREOP "$tc" $TEST_POSTOP
		if [ "$?" -eq 0 ]; then
			echo "PASS"
			ok=$(( ok + 1 ))
		else
			echo "FAIL"
			fail=$(( fail + 1 ))
		fi
	done

	echo "PASS: $ok, FAIL: $fail (TOTAL: $tot)"

	if [ "$fail" -gt 0 ]; then
		exit 1
	fi
	exit 0
}

find . -mindepth 1 -maxdepth 1 -executable -name 'suite[[:digit:]]*' | sort | main
