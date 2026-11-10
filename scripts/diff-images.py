#!/usr/bin/env python3
'''
Usage: ./diff-images <DIR_A> <DIR_B>

Compare all contents of files in two directories. This script works much like
`diff -r`, except that it's more or less sparse-aware. It skips holes in files
to compare blocks containing actual data in them. This is to avoid meaningless
comparison of holes of the large sparse exfat volume images in the CI pipelines.
'''
from pathlib import Path
import sys
import os
import errno
import hashlib
import typing

DEBUG = os.environ.get('DEBUG')
try: DEBUG = int(DEBUG)
except: DEBUG = 0

class FilesDifferException(Exception): ...

def debugp(level: int, l: typing.Iterable):
	if DEBUG < level:
		return
	conv = [ str(s) for s in l ]
	conv.append('\n')
	sys.stderr.write(''.join(conv))

def die(ec: int, msg: str):
	sys.stderr.write(msg + '\n')
	sys.exit(ec)

def list_files(path: Path, cnt: int = 1000) -> list[str]:
	if not path.is_dir():
		raise NotADirectoryError(path)

	ret = list[Path]()
	for p in path.rglob('*'):
		if not p.is_file(): continue
		if len(ret) > cnt: raise RecursionError()
		ret.append(p.relative_to(path).as_posix())

	return ret

def get_filesize(fd: int) -> int:
	return os.lseek(fd, 0, os.SEEK_END)

def scan_data_ranges(fd: int) -> list[tuple[int, int]]:
	ret = list[tuple[int, int]]()
	end = 0
	try:
		while True:
			start = os.lseek(fd, end, os.SEEK_DATA)
			end = os.lseek(fd, start, os.SEEK_HOLE)
			ret.append((start, end,))
			debugp(2, [ 'data range [', start, ', ', end, ')' ])
	except OSError as e:
		if e.errno != errno.ENXIO:
			raise e
	return ret

def comp_files_range(a: typing.BinaryIO, b: typing.BinaryIO, rl: list[tuple[int, int]]) -> list[tuple[int, int]]:
	ret = list[tuple[int, int]]()

	for r in rl:
		a.seek(r[0])
		b.seek(r[0])
		l = r[1] - r[0]
		assert l > 0

		while l > 0:
			to_read = min(l, 65536)
			buf_a = a.read(to_read)
			buf_b = b.read(to_read)

			if buf_a != buf_b:
				ret.append(r)
			l -= to_read

	return ret

la = list_files(Path(sys.argv[1]))
la.sort()
lb = list_files(Path(sys.argv[2]))
lb.sort()
diff_cnt = 0
ok_cnt = 0

if len(la) != len(lb):
	die(1, '''Directory structures differ''')

for i in range(len(la)):
	path_a = sys.argv[1] + os.path.sep + la[i]
	path_b = sys.argv[2] + os.path.sep + lb[i]

	if la[i] != lb[i]:
		print('''Directory structures differ:\n%s\n%s''' % (path_a, path_b,))
		diff_cnt += 1
		break

	with (open(path_a, 'rb') as fa, open(path_b, 'rb') as fb):
		drange = None
		sa = get_filesize(fa.fileno())
		sb = get_filesize(fb.fileno())
		range_a = scan_data_ranges(fa.fileno())
		range_b = scan_data_ranges(fb.fileno())

		debugp(1, [ path_a, ': ', sa ])
		debugp(1, [ path_b, ': ', sb ])

		try:
			if sa != sb or set(range_a) != set(range_b):
				raise FilesDifferException()
			drange = comp_files_range(fa, fb, range_a)
			if drange:
				raise FilesDifferException()

			ok_cnt += 1
		except FilesDifferException:
			diff_cnt += 1
			print('''Files differ:\n%s\n%s''' % (path_a, path_b,))
			if drange is not None:
				for r in drange:
					print('''at [%d, %d)''' % (r[0], r[1],))

ec = 0
if diff_cnt:
	if ok_cnt:
		ec = 3
	else:
		ec = 1

sys.exit(ec)
