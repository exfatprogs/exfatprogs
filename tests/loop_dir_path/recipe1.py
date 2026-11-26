#!/usr/bin/env python3
import io
import sys
import os
import subprocess
import pathlib
import struct

TARGET = sys.argv[1]
DRYRUN = False
DEBUG = 1
root_ofs: int = None
path_map = dict[str, list[tuple[str, dict[str, str]]]]()

cur_path = ""

class EndOfEntries(Exception): ...

inbuf = list[str]()

bps = 0
spc = 0
heap_ofs = 0

with subprocess.Popen([os.environ.get("DUMP_EXFAT", "dump.exfat"), TARGET],
		stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL) as proc:
	while True:
		l = proc.stdout.readline()
		if not l:
			break
		l = l.decode().lower()

		if l.startswith('cluster heap offset (sector offset):'):
			heap_ofs = int(l[36:].strip(), 0)
		elif l.startswith('bytes per sector:'):
			bps = int(l[17:].strip(), 0)
		elif l.startswith('sectors per cluster:'):
			spc = int(l[20:].strip(), 0)

if bps <= 0 or spc <= 0 or heap_ofs <= 0:
	raise ValueError('Failed to get volume meta')
cluster_size = bps * spc
cluster_ofs = heap_ofs * bps

# Run dump.exfat and load the stdout
with subprocess.Popen([os.environ.get("DUMP_EXFAT", "dump.exfat"), "-rcs", "/", TARGET],
		stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL) as proc:
	while True:
		l = proc.stdout.readline()
		if not l:
			break
		l = l.decode()
		l = l.rstrip('\r\n')
		inbuf.append(l)

def parse_entry() -> dict[str, str]:
	global inbuf
	ret = dict[str, str]()

	while inbuf and inbuf[0][0].isspace():
		l = inbuf[0]

		delim = l.find(':')
		if delim < 0:
			raise ValueError('Expected ":"')
		k = l[:delim].strip().lower()
		v = l[delim + 1:].strip()
		if not k or not v:
			raise ValueError('Key or value empty')

		ret[k] = v
		del inbuf[0]

	return ret

def parse_path() -> list[tuple[str, dict[str, str]]]:
	global inbuf
	ret = list[tuple[str, dict[str, str]]]()

	while inbuf and not inbuf[0].startswith('-'):
		type_line = inbuf[0]
		delim = type_line.find('.')
		if delim < 0:
			break
		# Validation only
		int(type_line[:delim], 0)
		if not type_line[delim + 1:]:
			raise ValueError('Empty value')

		del inbuf[0]
		attrs = parse_entry()
		ret.append((type_line, attrs,))

	return ret

def get_stream_ext(entries: list[tuple[str, dict[str, str]]]) -> dict[str, str] | None:
	for entry, a in entries:
		if entry.lower().endswith('stream extension directory entry'):
			return a

def get_dir_clus(entries: list[tuple[str, dict[str, str]]]) -> int | None:
	stream_ext = get_stream_ext(entries)
	for k, v in stream_ext.items():
		if k.lower() == 'firstcluster':
			return int(v, 0)

def get_pos(attrs: dict[str, str]) -> int | None:
	return int(attrs.get('position'), 0)

def set_dir_clus(img: io.FileIO, entries: list[tuple[str, dict[str, str]]], clus: int):
	stream_ext = get_stream_ext(entries)
	pos = get_pos(stream_ext)

	img.seek(pos, io.SEEK_SET)
	dentry = bytearray(img.read(32))

	if DEBUG:
		sys.stderr.write(('Update 0x%X ...') % (pos,) + os.linesep)
		sys.stderr.write('from:	' + dentry.hex() + os.linesep)

	clus += 2
	data = struct.pack('<I', clus)
	dentry[20] = data[0]
	dentry[21] = data[1]
	dentry[22] = data[2]
	dentry[23] = data[3]

	if DEBUG:
		sys.stderr.write('to:	' + dentry.hex() + os.linesep)

	if not DRYRUN:
		img.seek(pos, io.SEEK_SET)
		img.write(dentry)

def calc_chksum(chksum: int, data: bytes, primary: bool) -> int:
	chksum = ((chksum << 15) | (chksum >> 1)) & 0xFFFF
	chksum = (chksum + data[0]) & 0xFFFF
	chksum = ((chksum << 15) | (chksum >> 1)) & 0xFFFF
	chksum = (chksum + data[1]) & 0xFFFF

	for i in range(4 if primary else 2, 32):
		chksum = (chksum << 15) | (chksum >> 1)
		chksum += data[i]
		chksum &= 0xFFFF

	return chksum

def update_chksum(img: io.FileIO, entries: list[tuple[str, dict[str, str]]]):
	dentry_set = set[int]()
	chksum = 0
	primary = True
	p_dentry = None
	p_dentry_pos = None

	for entry, attrs in entries:
		pos = get_pos(attrs)
		if pos in dentry_set:
			raise ValueError()
		dentry_set.add(pos)

	for pos in dentry_set:
		img.seek(pos, io.SEEK_SET)
		data = img.read(32)

		chksum = calc_chksum(chksum, data, primary)

		if primary:
			p_dentry_pos = pos
			p_dentry = bytearray(data)
			primary = False

	data = struct.pack('<H', chksum)
	p_dentry[2] = data[0]
	p_dentry[3] = data[1]

	img.seek(p_dentry_pos, io.SEEK_SET)
	img.write(p_dentry)

# Parse the output

# Assume the first position: k-v pair line is the position of the root directory
while inbuf:
	l = inbuf[0].lstrip()
	del inbuf[0]
	if l.lower().startswith('position:'):
		root_ofs = int(l[9:].strip(), 0)
		break

root_dir_clus = (root_ofs - cluster_ofs) // cluster_size

# The reset is in the form of '''(Path|Directory): .*'''
while inbuf:
	l = inbuf[0]
	del inbuf[0]

	delim = l.find(':')
	if delim < 0:
		continue
	head = l[:delim]
	path = l[delim + 2:]

	if not (head.lower() == 'directory' or head.lower() == 'path'):
		continue

	entries = parse_path()
	path_map[path] = entries

# Debug print
if DEBUG > 1:
	sys.stderr.write('Cluster size: %d' % (cluster_size,) + os.linesep)
	sys.stderr.write('Cluster offset: %d' % (cluster_ofs,) + os.linesep)
	sys.stderr.write(str(root_ofs) + os.linesep)

	for path, entries in path_map.items():
		sys.stderr.write(path + os.linesep)
		for entry, attrs in entries:
			sys.stderr.write(entry + os.linesep)
			for k, v in attrs.items():
				sys.stderr.write(k + ': ' + v + os.linesep)

# Traverse and edit
with open(TARGET, 'rb' if DRYRUN else 'r+b') as img:
	for path, entries in path_map.items():
		p = pathlib.PurePosixPath(path)

		match p.name:
			case 'loops2root':
				if DEBUG:
					sys.stderr.write(('Looping %s to ROOT DIR') % (path,) + os.linesep)
				set_dir_clus(img, entries, root_dir_clus)
				update_chksum(img, entries)
			case 'loops2parent':
				if DEBUG:
					sys.stderr.write(('Looping %s to PARENT DIR') % (path,) + os.linesep)
				parent_entries = path_map[str(p.parent)]
				parent_clus = get_dir_clus(parent_entries)
				set_dir_clus(img, entries, parent_clus)
				update_chksum(img, entries)
