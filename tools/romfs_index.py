#!/usr/bin/env python3
"""Index of the merged RomFS that the runtime searches by bisection (runtime/romfs.c), and its romfs_config.h.

Adapted from astral-chain-pc-recomp tools/romfs_index.py @ 3bbaf259ac099a321c78b81f5402f6070a77874a (MIT), itself
adapted from MacNativeGames games/totk/scripts/prepare_romfs_backend.py @ b75be8cf (MIT, Copyright (c) 2026
MacNativeGames contributors). Change for Age of Calamity: the file list comes straight from the tables of romfs.bin itself
(tools/nx.py romfs_image_files), without the TotK extractor's index.json.

Input: build/v130/romfs.bin (RomFS 1.3.0 merged by tools/merge_romfs.py). Output, in the build folder (never in the
repository):

    romfs-index.bin   ACRFS1 layout: header <8sQQ (magic, count, strings offset), 24-byte rows <QQII
                      (offset in romfs.bin, size, name offset, name size with the NUL), then the NUL-terminated
                      names; rows in the order of the path's UTF-8 bytes (strcmp order)
    romfs_config.h    ROMFS_IMAGE_SIZE, ROMFS_IMAGE_SHA256, ROMFS_INDEX_SHA256, ROMFS_FILE_COUNT

The SHA-256 of the 10.9 GiB image is computed once and kept in build/v130/romfs.bin.sha256 with the size and the
modification time; it is only redone when the image changes. The two output files are only rewritten when their
bytes change, so ninja does not recompile romfs.c for nothing.

    python3 tools/romfs_index.py --image build/v130/romfs.bin --out ~/aoc-build/host
"""
from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nx  # noqa: E402
from check_content import GENERATED_MARKER  # noqa: E402

MAGIC = b'ACRFS1'   # the format runtime/romfs.c checks
HEADER = struct.Struct('<8sQQ')
ROW = struct.Struct('<QQII')


def build_index(files, image_size):
    """Index bytes for [(path, offset, size)]; stops with an error if the list is malformed."""
    files = sorted(files, key=lambda e: e[0].encode())
    strings, rows, previous = bytearray(), bytearray(), None
    for path, offset, size in files:
        name = path.encode()
        if not name or name.startswith(b'/') or b'\\' in name or b'\0' in name:
            sys.exit(f'romfs_index: malformed path in the RomFS (entry {len(rows) // ROW.size})')
        if name == previous:
            sys.exit(f'romfs_index: repeated path in the RomFS: {path}')
        previous = name
        if offset < 0 or size < 0 or offset + size > image_size:
            sys.exit(f'romfs_index: {path} lies outside the image')
        rows += ROW.pack(offset, size, len(strings), len(name) + 1)
        strings += name + b'\0'
    return HEADER.pack(MAGIC, len(files), HEADER.size + len(rows)) + bytes(rows) + bytes(strings)


def image_sha256(image: Path) -> str:
    """SHA-256 of the image, from the cache next to it when size and date match."""
    st = image.stat()
    cache = image.with_name(image.name + '.sha256')
    try:
        c = json.loads(cache.read_text())
        if c['size'] == st.st_size and c['mtime_ns'] == st.st_mtime_ns:
            return c['sha256']
    except (OSError, ValueError, KeyError):
        pass
    print(f'romfs_index: computing the SHA-256 of {image} ({st.st_size / 2**30:.1f} GiB, once)', flush=True)
    h = hashlib.sha256()
    with image.open('rb') as f:
        while chunk := f.read(16 << 20):
            h.update(chunk)
    digest = h.hexdigest()
    cache.write_text(json.dumps({'size': st.st_size, 'mtime_ns': st.st_mtime_ns, 'sha256': digest}) + '\n')
    return digest


def write_if_changed(path: Path, data: bytes) -> None:
    if path.exists() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def write(image: Path, out: Path):
    """Writes romfs-index.bin and romfs_config.h to out. Returns (files, index bytes, seconds)."""
    t0 = time.monotonic()
    size = image.stat().st_size
    with image.open('rb') as f:
        files = nx.romfs_image_files(f)
    index = build_index(files, size)
    write_if_changed(out / 'romfs-index.bin', index)
    config = (f'// Identity of the RomFS image and of the index, written by tools/romfs_index.py; do not edit. {GENERATED_MARKER}\n'
              '#pragma once\n'
              f'#define ROMFS_IMAGE_SIZE {size}ull\n'
              f'#define ROMFS_IMAGE_SHA256 "{image_sha256(image)}"\n'
              f'#define ROMFS_INDEX_SHA256 "{hashlib.sha256(index).hexdigest()}"\n'
              f'#define ROMFS_FILE_COUNT {len(files)}ull\n')
    write_if_changed(out / 'romfs_config.h', config.encode())
    return len(files), len(index), time.monotonic() - t0


def write_placeholder(out: Path) -> None:
    """romfs_config.h without a RomFS (a machine without the game extracted): the executable compiles and fs_init stops
    with a clear error."""
    (out / 'romfs-index.bin').unlink(missing_ok=True)
    config = (f'// No RomFS found by tools/romfs_index.py; do not edit. {GENERATED_MARKER}\n'
              '#pragma once\n'
              '#define ROMFS_MISSING 1\n'
              '#define ROMFS_IMAGE_SIZE 0ull\n'
              '#define ROMFS_IMAGE_SHA256 ""\n'
              '#define ROMFS_INDEX_SHA256 ""\n'
              '#define ROMFS_FILE_COUNT 0ull\n')
    write_if_changed(out / 'romfs_config.h', config.encode())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--image', type=Path, required=True, help='merged romfs.bin')
    ap.add_argument('--out', type=Path, required=True, help='build folder')
    args = ap.parse_args()
    count, size, dt = write(args.image.resolve(), args.out.expanduser().resolve())
    print(f'romfs index: {count} files, {size:,} bytes, {dt:.1f} s')
    return 0


if __name__ == '__main__':
    sys.exit(main())
