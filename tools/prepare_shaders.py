#!/usr/bin/env python3
"""Prepare every game shader once, before play, so a fight does not translate and build them while it runs.

A shader met for the first time in play is translated to SPIR-V and built by the GPU driver while the game waits on it:
until both are done its draws are skipped (objects missing, effects blinking for a few frames) and the frame that
finally needs it stalls for hundreds of milliseconds. Here every shader of the game is found statically (the GLSLC
outputs in `main` and in the RDB containers of the RomFS and of each DLC, read as tools/census_shaders.py does) and its default variant
(every texture slot a 2D texture, float vertex inputs and colour outputs, as TotK's recomp prepares its shaders) is
written next to AgeOfCalamityRecomp.exe in the record layouts of runtime/gfx_shader.c:

    seed-programs.bin  "ACSPRG": stage, length, c1 length, code hash, control section, code, c1
    seed-answers.bin   "ACSANS": stage, code hash, input id (zero: the runtime finds it), answers, priority

Then `AgeOfCalamityRecomp.exe --prepare-shaders` translates every record into the shader cache, writes
shader-prepared.bin and builds each variant's stage library or compute pipeline once into the GPU driver's own disk
cache. The variants play meets with other answers are recorded by the game itself (shader-answers.bin) and warmed at
each start. The seed files are deleted at the end; game-derived output stays in the (ignored) install folder.

A driver whose shader compiler crashes on a variant crashes on it in every run: the journal the run leaves names the
variants it was building, so only the driver step runs again (--warm-shaders), up to RETRIES times, without them.

    python3 tools/prepare_shaders.py <folder holding AgeOfCalamityRecomp.exe> [--work build/v130] [--no-run]
"""
from __future__ import annotations

import argparse
import hashlib
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import nx  # noqa: E402
from census_shaders import IDRK, SHADER_TYPES, glslc_shaders, idrk_inflate  # noqa: E402

STAGES = {"vs": 0, "ps": 4, "cs": 5}   # runtime/gfx.h GFX_STAGE_*
CONTROL = 0x880                        # runtime/gfx_shader.c CONTROL_SIZE
CONTROL_HASHED = 0x850
ANS_PACKED = 147
ANS_REC = 4 + 32 + 32 + ANS_PACKED + 1
JOURNAL = "shader-prepare-journal.bin"  # vk_pipeline.c skip_init: a 64-byte slot per worker, input id first
RETRIES = 3
EXE = "AgeOfCalamityRecomp.exe" if os.name == "nt" else "AgeOfCalamityRecomp"


def rdb_shaders(f, base: int, size: int):
    """Every GLSLC output in the shader containers of one RDB file -> [(stage, control, code)]."""
    out, pos = [], 0
    while pos + 0x30 <= size:
        hdr = nx.read_at(f, base + pos, 0x30)
        if hdr[:8] != IDRK:
            window = nx.read_at(f, base + pos, min(0x10000, size - pos))
            nxt = window.find(IDRK, 1)
            pos = pos + nxt if nxt > 0 else (pos // 0x10000 + 1) * 0x10000
            continue
        esz, csz, _length = struct.unpack_from("<QQQ", hdr, 8)
        typ, flags = struct.unpack_from("<II", hdr, 0x28)
        if esz < 0x30 or csz > esz:
            pos += 8
            continue
        if typ in SHADER_TYPES:
            out += glslc_shaders(idrk_inflate(f, base + pos + esz - csz, base + pos + esz, flags))
        pos += esz
    return out


def inventory(work: Path):
    """[(stage, control, code)] of the game: `main`, every RDB of the merged RomFS and of each DLC's RomFS
    (dlc-<title id>.bin, made by the dlc step)."""
    from census_imports import load_nso
    found = list(glslc_shaders(load_nso(work / "exefs" / "main")))
    for image in [work / "romfs.bin"] + sorted(work.glob("dlc-*.bin")):
        with image.open("rb") as f:
            files = [(p, o, s) for p, o, s in nx.romfs_image_files(f)
                     if ".rdb.bin" in p.rsplit("/", 1)[-1] or p.endswith(".file")]
            t0, done, before = time.monotonic(), 0, len(found)
            for _path, off, size in files:
                found += rdb_shaders(f, off, size)
                done += 1
                if time.monotonic() - t0 > 5:
                    print(f"  {image.name}: {done}/{len(files)} files read, {len(found)} shaders", flush=True)
                    t0 = time.monotonic()
            print(f"  {image.name}: {len(found) - before} shaders", flush=True)
    return found


def default_answers(stage: int, control: bytes) -> bytes:
    """GfxShaderAnswers in gfx_shader.c pack_answers layout: 2D textures, float inputs and outputs."""
    tex = bytes([4]) * 32   # 4: 2D (gfx_shader.c); slots outside the shader's use mask are dropped by the runtime
    local = (0, 0, 0)       # zero for graphics stages (they are in the input id)
    if stage == 5:          # compute: the control section's local size, 0 read as 1
        local = tuple(v or 1 for v in struct.unpack_from("<III", control, 0x748))
    packed = tex + bytes(32 * 3 + 8) + struct.pack("<HHHIB", *local, 0, 0)
    assert len(packed) == ANS_PACKED
    return packed


def code_hash(stage: int, control: bytes, code: bytes, length: int, c1: bytes) -> bytes:
    """gfx_shader.c build_code_hash."""
    return hashlib.sha256(struct.pack("<I", stage) + control[:CONTROL_HASHED] + struct.pack("<Q", length) +
                          code[:length] + struct.pack("<I", len(c1)) + c1).digest()


def write_seed(shaders, out: Path):
    progs, recs, skipped = {}, [], 0
    for st, control, code in shaders:
        stage = STAGES.get(st)
        if stage is None:
            skipped += 1
            continue
        control = (control + bytes(CONTROL))[:CONTROL]
        c1_off, length = struct.unpack_from("<II", control, 0x700)
        if length > len(code) or c1_off > length:
            skipped += 1
            continue
        c1 = code[c1_off:length]
        h = code_hash(stage, control, code, length, c1)
        if h in progs:
            continue
        progs[h] = struct.pack("<III", stage, length, len(c1)) + h + control + code[:length] + c1
        # priority 2 (rest): play's own records come first at each start anyway
        recs.append(struct.pack("<I", stage) + h + bytes(32) + default_answers(stage, control) + bytes([2]))
    with (out / "seed-programs.bin").open("wb") as f:
        f.write(b"ACSPRG\0\0" + struct.pack("<II", 1, 0))
        for body in progs.values():
            f.write(body)
    with (out / "seed-answers.bin").open("wb") as f:
        f.write(b"ACSANS\0\0" + struct.pack("<II", 1, ANS_REC))
        for rec in recs:
            assert len(rec) == ANS_REC
            f.write(rec)
    return len(progs), skipped


def driver_crashed(out: Path) -> bool:
    p = out / JOURNAL
    data = p.read_bytes() if p.is_file() else b""
    return any(any(data[i:i + 32]) for i in range(0, len(data), 64))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("dest", type=Path, help="folder holding AgeOfCalamityRecomp.exe")
    ap.add_argument("--work", type=Path, default=ROOT / "build" / "v130", help="romfs.bin and exefs/ (build/v130)")
    ap.add_argument("--no-run", action="store_true", help="write the seed files only")
    a = ap.parse_args(argv)
    t0 = time.monotonic()
    out = a.dest.resolve()
    shaders = inventory(a.work)
    n, skipped = write_seed(shaders, out)
    print(f"{len(shaders)} shaders found, {n} unique variants to prepare ({skipped} of other stages left out); "
          f"{time.monotonic() - t0:.0f} s", flush=True)
    if a.no_run:
        return 0
    env = dict(os.environ, ACR_MUTE="1", ACR_BACKGROUND_WINDOW="1")
    r = subprocess.run([str(out / EXE), "--prepare-shaders"], cwd=str(out), env=env)
    for _ in range(RETRIES):
        if not r.returncode or not driver_crashed(out):
            break
        print(f"prepare_shaders: the graphics driver crashed while building shaders (rc={r.returncode}); building them "
              "again without the variants it was building (--warm-shaders)", flush=True)
        r = subprocess.run([str(out / EXE), "--warm-shaders"], cwd=str(out), env=env)
    if not r.returncode:
        for name in ("seed-programs.bin", "seed-answers.bin"):
            (out / name).unlink(missing_ok=True)
    print(f"{EXE} --prepare-shaders: rc={r.returncode}; total {time.monotonic() - t0:.0f} s", flush=True)
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
