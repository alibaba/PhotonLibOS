#!/usr/bin/env python3
import sys, os

usage="""
Usage:
	./export-header.py <path/to/header/file>
"""

if len(sys.argv) != 2:
	print(usage)
	sys.exit(0)

tools_dir = os.path.dirname(os.path.realpath(__file__))
root_dir = os.path.dirname(tools_dir)
os.chdir(root_dir)

fn = sys.argv[1].strip()

if not fn.endswith('.h'):
	print('must be a header file (*.h)')
	sys.exit(-1)

if not os.path.isfile(fn):
	print('must exist as a regular file')
	sys.exit(-1)

fn = os.path.abspath(fn)
if not '/photon/' in fn:
	print('must be a header file of photon')
	sys.exit(-1)

parts = fn.split('/')
i = parts.index('photon') + 1
photon = '/'.join(parts[:i])
file = parts[i:]
include = photon + '/include/photon/'
if not os.path.isdir(include):
	print('it must exist as a directory:', include)

target = '../../'
path = os.path.relpath(include) + '/'
for dir in file[:-1]:
	target += '../'
	path = path + dir + '/'
	if not os.path.exists(path):
		print('mkdir', path)
		os.makedirs(path)
	elif not os.path.isdir(path):
		print('path component conflicts:', path)
		os.exit(-1)

symlink = path + file[-1]
if os.path.exists(symlink):
	print('already exists:', symlink)
	sys.exit(-1)

target += '/'.join(file)
print('symlink:', symlink, '->', target)
os.symlink(target, symlink)
