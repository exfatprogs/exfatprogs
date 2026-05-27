#!/bin/sh
sed -E '/^(\s+)?#/d' "$@" |	# every file in argv, lines starting with '#' removed
	grep -oiE '[0-9a-f]{4}' |	# hex characters only
	sed -E 's/(\w\w)(\w\w)/\2\1/' | # swap endian
	xxd -ps -r			# output hex as binary
