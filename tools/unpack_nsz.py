#!/usr/bin/env python3
"""Turns compressed dumps (.nsz, .xcz) into plain NSPs the other tools read: unpack_nsz.py --out <dir> <file>...

The NCZ format is the one of nicoboss/nsz (nsz/IndependentNczDecompressorConcise.py, 5.0.0): the first 0x4000 bytes of
the NCA as they are, "NCZSECTN" and its section table, then the rest compressed with zstd, either as one stream or in
"NCZBLOCK" blocks, decrypted: each section is encrypted again here. An NCA is named after its SHA-256, so every one
rebuilt is checked against its name.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nx  # noqa: E402

NCA_HEADER = 0x4000
CHUNK = 1 << 22


def plain_path(out: Path, package: Path) -> Path:
    """The NSP made from package; its size in the name so another dump of the same name is not taken for it."""
    return out / f"{package.stem}-{package.stat().st_size}.nsp"


class Bounded:
    """The bytes of one container entry, for zstd's stream reader."""

    def __init__(self, f, end: int):
        self.f, self.end = f, end

    def read(self, n: int = -1) -> bytes:
        left = self.end - self.f.tell()
        return self.f.read(left if n < 0 else min(n, left)) if left > 0 else b""


def read_sections(f) -> list:
    nx.require(f.read(8) == b"NCZSECTN", "Not an NCZ (no section table)")
    count = struct.unpack("<Q", f.read(8))[0]
    nx.require(0 < count <= 64, "Invalid NCZ section table")
    out = []
    for _ in range(count):
        offset, size, crypto, _pad = struct.unpack("<QQQQ", f.read(32))
        key, counter = f.read(16), f.read(16)
        out.append((offset, size, crypto, key, counter))
    return sorted(out)


def ncz_size(f, offset: int) -> int:
    """The size of the NCA an NCZ rebuilds: where its last section ends."""
    f.seek(offset + NCA_HEADER)
    return max(s[0] + s[1] for s in read_sections(f))


def encrypt(key: bytes, nonce: bytes, pos: int, data: bytes) -> bytes:
    skip = pos & 15
    iv = nonce[:8] + (pos >> 4).to_bytes(8, "big")
    return nx.decrypt(key, nx.modes().CTR(iv), bytes(skip) + data)[skip:]   # CTR: the same both ways


def rebuild(f, offset: int, size: int, out) -> str:
    """Writes the NCA of the NCZ at offset, returns its SHA-256."""
    import zstandard
    f.seek(offset)
    header = f.read(NCA_HEADER)
    nx.require(len(header) == NCA_HEADER, "Truncated NCZ")
    sections = read_sections(f)
    digest = hashlib.sha256(header)
    out.write(header)
    pos, at = sections[0][0], 0
    nx.require(pos == NCA_HEADER, "Unexpected first NCZ section")

    def emit(data: bytes) -> None:
        nonlocal pos, at
        while data:
            while at < len(sections) and pos >= sections[at][0] + sections[at][1]:
                at += 1
            nx.require(at < len(sections) and pos >= sections[at][0], "NCZ data outside its sections")
            start, length, crypto, key, nonce = sections[at]
            part, data = data[:start + length - pos], data[start + length - pos:]
            if crypto in (3, 4):
                part = encrypt(key, nonce, pos, part)
            out.write(part)
            digest.update(part)
            pos += len(part)

    end = offset + size
    mark = f.tell()
    if f.read(8) == b"NCZBLOCK":
        _ver, _kind, _unused, exponent, count, total = struct.unpack("<BBBBIQ", f.read(16))
        nx.require(14 <= exponent <= 32 and count > 0, "Corrupted NCZBLOCK header")
        sizes = struct.unpack(f"<{count}I", f.read(4 * count))
        block = 1 << exponent
        for i, packed in enumerate(sizes):
            want = (total % block or block) if i == count - 1 else block
            data = f.read(packed)
            nx.require(len(data) == packed and f.tell() <= end, "Truncated NCZ block")
            emit(zstandard.ZstdDecompressor().decompress(data, max_output_size=want) if packed < want else data)
    else:
        f.seek(mark)
        reader = zstandard.ZstdDecompressor().stream_reader(Bounded(f, end))
        while True:
            data = reader.read(CHUNK)
            if not data:
                break
            emit(data)
    nx.require(pos == sections[-1][0] + sections[-1][1], "Truncated NCZ")
    return digest.hexdigest()


def unpack(package: Path, dst: Path) -> None:
    """package as a plain NSP at dst, written to .part first: a run cut short never leaves half an NSP."""
    part = dst.with_name(dst.name + ".part")
    with package.open("rb") as f:
        _, entries = nx.container_entries(f, package.stat().st_size)
        items = []
        for name, offset, size in entries:
            ncz = name.lower().endswith(".ncz")
            items.append((name[:-4] + ".nca" if ncz else name, name, offset, size, ncz_size(f, offset) if ncz else size, ncz))
        names = b"".join(i[0].encode("utf-8") + b"\0" for i in items)
        names += bytes(-(16 + 24 * len(items) + len(names)) % 16)
        table, data_at, name_at = b"", 0, 0
        for new, _name, _offset, _size, length, _ncz in items:
            table += struct.pack("<QQII", data_at, length, name_at, 0)
            data_at += length
            name_at += len(new.encode("utf-8")) + 1
        dst.parent.mkdir(parents=True, exist_ok=True)
        with part.open("wb") as out:
            out.write(b"PFS0" + struct.pack("<III", len(items), len(names), 0) + table + names)
            for new, name, offset, size, length, is_ncz in items:
                before = out.tell()
                if is_ncz:
                    sha = rebuild(f, offset, size, out)
                    nx.require(sha[:32] == Path(new).stem.split(".")[0].lower(), f"{name} is damaged (its hash does not match)")
                else:
                    f.seek(offset)
                    left = size
                    while left:
                        block = f.read(min(left, CHUNK))
                        nx.require(bool(block), f"{package.name} ends inside {name}")
                        out.write(block)
                        left -= len(block)
                nx.require(out.tell() - before == length, f"{name}: wrong size once unpacked")
    os.replace(part, dst)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("packages", type=Path, nargs="+")
    args = ap.parse_args()
    for package in args.packages:
        dst = plain_path(args.out, package)
        if dst.is_file():
            print(f"{package.name}: already unpacked", flush=True)
            continue
        print(f"Unpacking {package.name} ...", flush=True)
        try:
            unpack(package, dst)
        except (ValueError, OSError, ImportError) as e:
            dst.with_name(dst.name + ".part").unlink(missing_ok=True)
            print(f"ERROR: could not unpack {package.name}: {e}", flush=True)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
