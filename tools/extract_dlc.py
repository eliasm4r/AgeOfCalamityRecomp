#!/usr/bin/env python3
"""Extracts the RomFS of each Age of Calamity DLC from the NSPs into build/v130/ (ignored by git).

Each DLC (title id 01002B00111A3001 to ...3004) has a PublicData NCA with a RomFS section. The output is the plain
RomFS image of that section (the IVFC data level, the same format as build/v130/romfs.bin), one per DLC:

    build/v130/dlc-<title id>.bin
    build/v130/dlc.json                title, index (title id - 01002B00111A3000), size, SHA-256 and files

The runtime (runtime/dlc.c) looks for these files in the romfs/ folder next to the executable; the ones present are
the DLCs the game sees (nn::aoc::ListAddOnContent) and mounts (nn::fs::MountAddOnContent).

The whole IVFC hash tree is checked during the copy: each data level block against the level above, and so on up to
the master hash of the section header (which the NCA header covers; nx.read_nca_header checks it). An image already on
disk with the size and date of dlc.json is not extracted again (--force redoes it).

    python3 tools/extract_dlc.py --game-dir "Game Files" --keys "Game Files/prod.keys"

Reads the NSPs in place. Keys stay in memory only and are never printed.
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
import extract  # noqa: E402
import nx  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
ACR_BASE = 0x01002B00111A3000   # application id + 0x1000: the DLC of index n has the title id ACR_BASE + n
CHUNK = 8 << 20


def ivfc_levels(fs: bytes) -> tuple[list[tuple[int, int, int]], bytes]:
    """The six levels (offset in the section, size, block size) and the master hash of an IVFC section header."""
    nx.require(fs[8:12] == b"IVFC", "Section without IVFC")
    master_size, count = struct.unpack_from("<II", fs, 0x10)
    nx.require(count == 7 and master_size == 0x20, "Unsupported IVFC layout")
    levels = []
    for k in range(6):
        off, size, log2, _ = struct.unpack_from("<QQII", fs, 0x18 + 0x18 * k)
        nx.require(12 <= log2 <= 24, "Unexpected IVFC block size")
        levels.append((off, size, 1 << log2))
    return levels, fs[0xC8:0xE8]


def block_hashes(data: bytes, block: int, out: bytearray) -> None:
    """SHA-256 of each data block (the last one padded with zeros, as IVFC does), appended to out."""
    for pos in range(0, len(data), block):
        part = data[pos:pos + block]
        if len(part) < block:
            part += bytes(block - len(part))
        out += hashlib.sha256(part).digest()


def copy_verified(sec: nx.SectionReader, dst: Path) -> tuple[int, str]:
    """Writes the data level to dst checking the whole IVFC tree. Returns (size, SHA-256 of the image)."""
    levels, master = ivfc_levels(sec.fs)
    # levels 0 to 4 are small hash tables: each is checked against the one above, the first against the master hash
    tables = []
    expected = master
    for off, size, block in levels[:5]:
        nx.require(size <= nx.MAX_READ, "Tabela de hash do IVFC grande demais")
        data = sec.read(off, size)
        got = bytearray()
        block_hashes(data, block, got)
        nx.require(bytes(got) == expected[:len(got)], "IVFC hash table mismatch (corrupt NSP?)")
        tables.append(data)
        expected = data
    off, size, block = levels[5]
    nx.require(CHUNK % block == 0, "IVFC block larger than expected")
    whole = hashlib.sha256()
    part = dst.with_name(dst.name + ".part")
    done = 0
    try:
        with part.open("wb") as out:
            while done < size:
                n = min(CHUNK, size - done)
                data = sec.read(off + done, n)
                got = bytearray()
                block_hashes(data, block, got)
                at = done // block * 32
                nx.require(bytes(got) == expected[at:at + len(got)], f"Data block at {done:#x} does not match the IVFC")
                out.write(data)
                whole.update(data)
                done += n
    except BaseException:
        part.unlink(missing_ok=True)   # a half-written image never looks finished
        raise
    part.replace(dst)
    return size, whole.hexdigest()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    extract.add_source_args(ap)
    ap.add_argument("--keys", type=Path, default=None, help="default: prod.keys in the game folder")
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "v130")
    ap.add_argument("--force", action="store_true", help="extracts again even with the image on disk")
    args = ap.parse_args(argv)
    game_dir = extract.game_dir_of(args)
    nx.require(bool(args.keys or game_dir), "--keys is needed when no game folder is given")
    keys = nx.load_keys(args.keys or game_dir / "prod.keys")
    args.out.mkdir(parents=True, exist_ok=True)
    manifest_path = args.out / "dlc.json"
    try:
        old = {e["title_id"]: e for e in json.loads(manifest_path.read_text(encoding="utf-8"))["dlc"]}
    except (OSError, ValueError, KeyError):
        old = {}
    found = []
    for path in (p for p in extract.package_paths(game_dir, args.nsp) if p.suffix.lower() in (".nsp", ".xci")):
        with path.open("rb") as f:
            _, entries = nx.container_entries(f, path.stat().st_size)
            tickets = nx.common_tickets(f, entries)
            for nca in nx.nca_inventory(f, entries, keys):
                tid = int(nca.title_id, 16)
                if nca.content_type != 5 or not 0 < tid - ACR_BASE < 0x1000:
                    continue
                key = nx.nca_ctr_key(nca, keys, tickets)
                for i in nca.sections():
                    if nca.section_kind(i) != "romfs":
                        continue
                    sec = nx.SectionReader(f, nca, i, key)
                    dst = args.out / f"dlc-{nca.title_id}.bin"
                    prev = old.get(nca.title_id)
                    if (not args.force and prev and dst.is_file() and dst.stat().st_size == prev["size"]
                            and dst.stat().st_mtime_ns == prev["mtime_ns"]):
                        print(f"DLC {tid - ACR_BASE} ({nca.title_id}): already extracted, {prev['size'] / 2**20:.1f} MiB")
                        found.append(prev)
                        continue
                    t0 = time.monotonic()
                    size, sha = copy_verified(sec, dst)
                    with dst.open("rb") as image:
                        files = nx.romfs_image_files(image)
                    nx.require(all(o + s <= size for _p, o, s in files), "RomFS file outside the image")
                    found.append({"title_id": nca.title_id, "index": tid - ACR_BASE, "file": dst.name, "size": size,
                                  "mtime_ns": dst.stat().st_mtime_ns, "sha256": sha, "files": len(files),
                                  "source": path.name})
                    print(f"DLC {tid - ACR_BASE} ({nca.title_id}): {len(files)} files, {size / 2**20:.1f} MiB, "
                          f"IVFC checked, {time.monotonic() - t0:.1f} s  → {dst.name}")
    found.sort(key=lambda e: e["index"])
    manifest_path.write_text(json.dumps({"dlc": found}, indent=1) + "\n", encoding="utf-8")
    if not found:
        print(f"No DLC in {extract.sources_text(game_dir, args.nsp)} (the game runs without them)")
    else:
        print(f"{len(found)} DLCs in {args.out} ({sum(e['size'] for e in found) / 2**30:.2f} GiB)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ValueError as e:
        print("ERROR:", e)
        sys.exit(1)
