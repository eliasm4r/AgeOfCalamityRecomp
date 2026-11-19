#!/usr/bin/env python3
"""Census of the Koei Tecmo RDB files: what is inside RomFS 1.3.0 and the DLCs, by type.

The game data sits in `asset/*.rdb.bin*` and `asset/data/*.file`: a sequence of IDRK entries
('IDRK0000', u64 entry size, u64 stored size, u64 real size, u32 0, u32 file KTID, u32 type KTID, u32 flags; the
stored data sits at the end of the entry; flag 0x100000 = zlib in blocks of up to 16 KB). Format described by nxnative
(`dm/gfx/tools/shader_split.py`, `idrk_entries`/`idrk_data`).

For each type it counts entries, stored bytes and real bytes, and notes the data "magic" of a few samples (e.g.
`_S2G` = G2S shaders, `GT1G` = G1T textures). For G1T textures it reads each one's header and counts the format
codes — that is what decides the ASTC cost on the Steam Deck.

Only reads headers and the first block of each entry; resumes where it stopped (--budget seconds per run).

Usage:
    python3 tools/census_rdb.py --romfs build/v130/romfs.bin [--budget 150]
    python3 tools/census_rdb.py --report      # gathers the results into build/v130/rdb-census.json
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import time
import zlib
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nx  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
IDRK = b"IDRK0000"
SAMPLES_PER_TYPE = 3
# G1T format codes. 0x59–0x5F and 0x06/0x08: Amicitia Wiki (G1T), checked in our dump by the size/pixels ratio
# (BC1 and BC4 give 4 bpp, BC5/BC6H/BC7 give 8 bpp, with mipmaps). 0x01–0x04 only show up in 1×1 textures
# (uncompressed placeholders).
G1T_FORMATS = {0x01: "BGRA8 (1×1)", 0x02: "32 bpp (1×1)", 0x03: "uncompressed (1×1?)", 0x04: "uncompressed (1×1)",
               0x06: "BC1 (old)", 0x08: "BC3 (old)", 0x59: "BC1", 0x5A: "BC2", 0x5B: "BC3", 0x5C: "BC4",
               0x5D: "BC5", 0x5E: "BC6H", 0x5F: "BC7"}


def first_bytes(f, start: int, end: int, flags: int, n: int) -> bytes:
    """First n real bytes of an entry (decompresses only the first block)."""
    if not flags & 0x100000:
        return nx.read_at(f, start, min(n, end - start))
    head = nx.read_at(f, start, min(end - start, 4 + 0x4400))
    cs = struct.unpack_from("<I", head, 0)[0]
    if not cs:
        return b""
    if cs > len(head) - 4:
        head = nx.read_at(f, start, 4 + cs)
    return zlib.decompressobj().decompress(head[4:4 + cs], n)


def g1t_formats(data: bytes) -> list[int]:
    """Format codes of the textures of a G1T ('GT1G'): u32 magic, u32 version, u32 size, u32 table offset, u32
    texture count; the table has one u32 per texture (relative to the table); in each texture header the 2nd byte
    is the format."""
    if data[:4] != b"GT1G" or len(data) < 0x14:
        return []
    table, count = struct.unpack_from("<II", data, 0x0C)
    out = []
    for i in range(min(count, 4096)):
        if table + 4 * i + 4 > len(data):
            break
        off = table + struct.unpack_from("<I", data, table + 4 * i)[0]
        if off + 2 <= len(data):
            out.append(data[off + 1])
    return out


def scan_file(f, path: str, base: int, size: int, deadline: float, state: dict) -> bool:
    """Walks the entries of an RDB file from state['pos']; returns True when done."""
    pos = state.get("pos", 0)
    types = state.setdefault("types", {})
    while pos + 0x30 <= size:
        if time.time() > deadline:
            state["pos"] = pos
            return False
        hdr = nx.read_at(f, base + pos, 0x30)
        if hdr[:8] != IDRK:
            # entries can follow padding up to 64 KB: look for the next one
            window = nx.read_at(f, base + pos, min(0x10000, size - pos))
            nxt = window.find(IDRK, 1)
            if nxt < 0:
                pos = (pos // 0x10000 + 1) * 0x10000
                continue
            pos += nxt
            continue
        esz, csz, length = struct.unpack_from("<QQQ", hdr, 8)
        type_ktid, flags = struct.unpack_from("<II", hdr, 0x28)
        if esz < 0x30 or csz > esz:
            state.setdefault("bad", 0)
            state["bad"] += 1
            pos += 8
            continue
        key = f"{type_ktid:08x}"
        t = types.setdefault(key, {"count": 0, "stored": 0, "length": 0, "magic": {}, "g1t_formats": {}})
        t["count"] += 1
        t["stored"] += csz
        t["length"] += length
        start, end = base + pos + esz - csz, base + pos + esz
        sample = sum(t["magic"].values()) < SAMPLES_PER_TYPE
        is_g1t = t["magic"].get("GT1G", 0) > 0
        if sample or is_g1t:
            data = first_bytes(f, start, end, flags, 0x4000 if (is_g1t or sample) else 8)
            magic = data[:4].decode("ascii", "replace") if data[:4].isascii() else data[:4].hex()
            if sample:
                t["magic"][magic] = t["magic"].get(magic, 0) + 1
            if magic == "GT1G":
                for code in g1t_formats(data):
                    k = f"0x{code:02x}"
                    t["g1t_formats"][k] = t["g1t_formats"].get(k, 0) + 1
        pos += esz
    state["pos"] = size
    return True


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--romfs", type=Path, default=ROOT / "build" / "v130" / "romfs.bin")
    ap.add_argument("--state", type=Path, default=ROOT / "build" / "v130" / "rdb-census-state.json")
    ap.add_argument("--budget", type=float, default=150.0)
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--dlc", action="store_true", help="also the DLC RDBs, read straight from the NSPs in --game-dir")
    ap.add_argument("--game-dir", type=Path, default=ROOT / "Game Files")
    ap.add_argument("--keys", type=Path, default=ROOT / "Game Files" / "prod.keys")
    args = ap.parse_args(argv)
    state = json.loads(args.state.read_text(encoding="utf-8")) if args.state.exists() else {"files": {}}

    if not args.report:
        deadline = time.time() + args.budget
        sources = [("", args.romfs.open("rb"))]
        if args.dlc:
            sources += [(f"dlc:{tid}:", sf) for tid, sf in nx.dlc_romfs(args.game_dir, nx.load_keys(args.keys))]
        total_files = done = 0
        for label, f in sources:
            files = [(label + p, o, s) for p, o, s in nx.romfs_image_files(f)
                     if ".rdb.bin" in p.rsplit("/", 1)[-1] or p.endswith(".file")]
            total_files += len(files)
            pending = [x for x in files if not state["files"].get(x[0], {}).get("done")]
            for path, off, size in sorted(pending, key=lambda x: x[2]):
                st = state["files"].setdefault(path, {})
                st["done"] = scan_file(f, path, off, size, deadline, st)
                if not st["done"]:
                    break
            done += sum(1 for p, _o, _s in files if state["files"].get(p, {}).get("done"))
        args.state.write_text(json.dumps(state) + "\n", encoding="utf-8")
        print(f"RDB files processed: {done}/{total_files}" + ("" if done == total_files else " (run again to continue)"))
        if done < total_files:
            return 2

    total: dict = defaultdict(lambda: {"count": 0, "stored": 0, "length": 0, "magic": Counter(), "g1t_formats": Counter()})
    for st in state["files"].values():
        for k, t in st.get("types", {}).items():
            a = total[k]
            a["count"] += t["count"]; a["stored"] += t["stored"]; a["length"] += t["length"]
            a["magic"].update(t["magic"]); a["g1t_formats"].update(t["g1t_formats"])
    rows = sorted(total.items(), key=lambda kv: kv[1]["length"], reverse=True)
    print(f"{'tipo KTID':10s} {'magic':8s} {'entradas':>9s} {'guardado':>10s} {'real':>10s}")
    for k, t in rows[:30]:
        magic = ",".join(m for m, _ in t["magic"].most_common(2))
        print(f"{k:10s} {magic:8s} {t['count']:9d} {t['stored'] / 2**20:8.0f}Mi {t['length'] / 2**20:8.0f}Mi")
    g1t = Counter()
    for _k, t in rows:
        g1t.update(t["g1t_formats"])
    if g1t:
        print("formatos de textura G1T:")
        for code, n in g1t.most_common():
            print(f"    {code}  {G1T_FORMATS.get(int(code, 16), '?'):24s} {n}")
    out = args.state.with_name("rdb-census.json")
    out.write_text(json.dumps({k: {**t, "magic": dict(t["magic"]), "g1t_formats": dict(t["g1t_formats"])}
                               for k, t in rows}, indent=1) + "\n", encoding="utf-8")
    print(f"report: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
