"""Merges the base 1.0.0 RomFS with the BKTR patch of update 1.3.0 using hactool: build/v130/romfs.bin.

The launcher runs this on Windows and Linux:

  python tools/merge_romfs.py [--game-dir "Game Files"] [--nsp <file> ...] [--keys <prod.keys>]

1. prepares build/v130/tmp/hactool-home/.switch/{prod,title}.keys (tools/extract.py hactool-home; values never shown),
   the home of every hactool run, so key files of the player's own (~/.switch) are never read;
2. opens the base and update NSPs where they are, or, when a path is not plain ASCII (the Windows hactool does not
   open those), hard-links them under ASCII names (copied when the dump is on another disk);
3. extracts the NCAs of both NSPs (~11 GB temporary; checked against each NSP's file table) and merges section 1 of
   the update's program NCA (BKTR) over the base, checking the six IVFC levels (-y);
4. deletes the temporary files and the copied keys, only once the image exists.

hactool's output goes to build/v130/extract-{base,update}.log and merge.log, key values hidden.

Needs ~23 GB free during the merge (temporary NCAs + the 10.9 GiB image). hactool comes from prebuilt/hactool/ (the
launcher package) or toolchain/hactool-1.4.0-win/ (development on Windows).
"""
import argparse
import os
import re
import shutil
import json
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import extract  # noqa: E402
import nx  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "build" / "v130"
TMP = WORK / "tmp"
HAC_HOME = TMP / "hactool-home"
OUT = WORK / "romfs.bin"
LOG = WORK / "merge.log"
BASE_ID, UPDATE_ID = "01002B00111A2000", "01002B00111A2800"
WIN = os.name == "nt"


def fail(msg: str) -> None:
    print(f"ERROR: {msg}", flush=True)
    sys.exit(1)


def hactool() -> Path:
    cands = ([ROOT / "prebuilt/hactool/win-x64/hactool.exe", ROOT / "toolchain/hactool-1.4.0-win/hactool.exe"] if WIN
             else [ROOT / "prebuilt/hactool/linux-x64/hactool", ROOT / "toolchain/hactool-1.4.0-linux/hactool"])
    for c in cands:
        if c.is_file():
            if not WIN:
                c.chmod(0o755)
            return c
    found = shutil.which("hactool")
    if found:
        return Path(found)
    fail("hactool not found (" + ", ".join(str(c) for c in cands) + ")")


def find_nsps(game_dir: Path | None, nsps: list, keys: Path) -> tuple[Path, Path]:
    """The base and update 1.3.0 NSPs of the folder and the files chosen on their own, found by their contents
    (tools/extract.py scan: title id and version from the .cnmt), whatever the file name."""
    pkgs = [p for p in extract.scan(extract.package_paths(game_dir, nsps), nx.load_keys(keys)) if p["container"] == "nsp"]
    out = []
    for role, what in (("base", "of the base game (01002B00111A2000)"), ("update", "of update 1.3.0 (01002B00111A2800)")):
        found = [p["path"] for p in pkgs if p["role"] == role]
        if len(found) != 1:
            fail(f"expected 1 NSP {what} in {extract.sources_text(game_dir, nsps)}, found {len(found)}")
        out.append(found[0])
    return out[0], out[1]


def link_or_copy(src: Path, dst: Path) -> Path:
    """The path hactool opens for src: src itself when it is plain ASCII (the Windows hactool fails only on other
    characters), else dst, hard-linked or, on another disk, copied (11 GB for the base game)."""
    if str(src).isascii():
        return src
    if dst.exists() and dst.stat().st_size == src.stat().st_size:
        return dst   # linked, or copied whole, by an earlier run
    dst.unlink(missing_ok=True)   # a copy cut short by an earlier run
    try:
        os.link(src, dst)   # same disk: copies nothing
        return dst
    except OSError:
        pass
    if not WIN:   # another disk (e.g. the dump in /mnt/c under WSL): the Linux hactool opens any path
        os.symlink(src, dst)
        return dst
    print(f"hard link failed; copying {dst.name} (this can take a while)...", flush=True)
    shutil.copyfile(src, dst)
    return dst


def hactool_env() -> dict:
    """The environment of every hactool run: the home of step 1 in place of the player's. At each start, whatever the
    command, hactool reads $HOME/.switch/{prod,title}.keys (else %USERPROFILE%/.switch, then $XDG_CONFIG_HOME/switch)
    and ends with code 1 on a line it rejects: Lockpick's master_kek_source with an extra 00, a key of another length,
    a rights ID twice in title.keys. Key files of other Switch tools there stopped the extraction of base.nsp while the
    check, which reads the game folder's prod.keys, said all was good (reported 2026-11-22)."""
    env = {k: v for k, v in os.environ.items() if k.upper() not in ("HOME", "USERPROFILE", "XDG_CONFIG_HOME")}
    env.update(HOME=str(HAC_HOME), USERPROFILE=str(HAC_HOME))
    return env


def run_hactool(args: list, log: Path) -> int:
    """Runs hactool (hactool_env) and writes its output to log, the values of keys it quotes hidden."""
    p = subprocess.run([str(hactool())] + args, env=hactool_env(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = p.stdout.decode("utf-8", errors="replace")
    text = re.sub(r"Key \(([^)\n]*)\)", lambda m: f"Key (<{len(m.group(1))} characters, not shown>)", text)
    text = re.sub(r'\(value "[^"\n]*"\)', "(value not shown)", text)
    log.write_text(text, encoding="utf-8")
    return p.returncode


class Bytes:
    """Prints "romfs progress: <done> of <total> MiB" every few seconds while hactool writes (tools/make_build.py
    moves its bar with it): the NCAs extracted so far, then the image. hactool itself prints nothing until it ends."""

    def __init__(self, total: int):
        self.total, self.before, self.measure = max(1, total), 0, None
        self.stop = threading.Event()
        threading.Thread(target=self.loop, daemon=True).start()

    def watch(self, measure):
        """measure() gives the bytes written so far by the running hactool; None stops watching."""
        self.measure = measure

    def done(self, n: int):
        self.before += n
        self.measure = None
        self.show(0)

    def show(self, now: int):
        mib = min(self.before + now, self.total) >> 20
        sys.stdout.write(f"romfs progress: {mib} of {self.total >> 20} MiB\n")
        sys.stdout.flush()

    def loop(self):
        while not self.stop.wait(3.0):
            measure = self.measure
            if measure:
                try:
                    self.show(measure())
                except OSError:   # a file renamed or removed while it was measured
                    pass


def tree_bytes(d: Path) -> int:
    return sum(p.stat().st_size for p in d.rglob("*") if p.is_file()) if d.is_dir() else 0


def file_bytes(p: Path) -> int:
    return p.stat().st_size if p.is_file() else 0


def image_size() -> int:
    """Bytes of the merged 1.3.0 RomFS (tools/data/known-builds.json), 11 GiB when the table has none."""
    try:
        known = json.loads((ROOT / "tools/data/known-builds.json").read_text(encoding="utf-8"))
        return int(known["builds"]["1.3.0"]["romfs_image_size"]) or 11 << 30
    except (OSError, ValueError, KeyError):
        return 11 << 30


def last_line(log: Path) -> str:
    """hactool's last line in log: what stopped it."""
    lines = [l.strip() for l in log.read_text(encoding="utf-8", errors="replace").splitlines() if l.strip()]
    return lines[-1] if lines else "no output"


def extraction_gap(nsp: Path, d: Path) -> str:
    """"" when d holds every file of the NSP at its size, else the first one that is not: hactool goes on with code 0
    when it cannot create a file, and does not check its writes (a full disk)."""
    with nsp.open("rb") as f:
        _, entries = nx.container_entries(f, nsp.stat().st_size)
    for name, _, size in entries:
        p = d / name
        if not p.is_file():
            return f"{name} is missing"
        if p.stat().st_size != size:
            return f"{name} has {p.stat().st_size} of its {size} bytes"
    return ""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    extract.add_source_args(ap)
    ap.add_argument("--keys", type=Path, help="default: prod.keys in the game folder")
    args = ap.parse_args()
    game_dir = extract.game_dir_of(args)
    if not (args.keys or game_dir):
        fail("--keys is needed when no game folder is given")
    keys = args.keys or game_dir / "prod.keys"
    if OUT.exists():
        print(f"{OUT} already exists; delete it to redo it")
        return 0
    hac = hactool()
    print(f"hactool: {hac}", flush=True)

    # 1. keys for hactool (extract.py never prints values)
    subprocess.run([sys.executable, str(ROOT / "tools/extract.py"), "hactool-home", "--keys", str(keys)]
                   + (["--game-dir", str(game_dir)] if game_dir else []) + [a for n in args.nsp for a in ("--nsp", str(n))],
                   check=True)
    if not (HAC_HOME / ".switch" / "title.keys").is_file():
        fail("extract.py hactool-home did not create the keys")

    # 2. the NSPs under ASCII names
    inp = TMP / "in"
    inp.mkdir(parents=True, exist_ok=True)
    base_nsp, update_nsp = find_nsps(game_dir, args.nsp, keys)
    nsps = {"base": link_or_copy(base_nsp, inp / "base.nsp"), "update": link_or_copy(update_nsp, inp / "update.nsp")}

    # 3. NCAs of both NSPs (the base is ~11 GB) and the merge. Each NSP is extracted into <name>.part, renamed once
    # every file is there at its size: a run cut short extracts again instead of merging half-written NCAs.
    for name in ("base", "update"):
        d = TMP / name
        if d.is_dir() and extraction_gap(nsps[name], d):   # left incomplete by an earlier version of this script
            shutil.rmtree(d)
    todo = [n for n in ("base", "update") if not (TMP / n).is_dir()]
    bytes_ = Bytes(sum(nsps[n].stat().st_size for n in todo) + image_size())
    for name in ("base", "update"):
        d, part = TMP / name, TMP / f"{name}.part"
        if name in todo:
            print(f"Extracting {name}.nsp ...", flush=True)
            shutil.rmtree(part, ignore_errors=True)
            log = WORK / f"extract-{name}.log"
            bytes_.watch(lambda part=part: tree_bytes(part))
            code = run_hactool(["-t", "pfs0", "--disablekeywarns", f"--outdir={part}", str(nsps[name])], log)
            bytes_.done(nsps[name].stat().st_size)
            gap = "" if code else extraction_gap(nsps[name], part)
            if code or gap:
                shutil.rmtree(part, ignore_errors=True)
                fail(f"hactool could not extract {name}.nsp ({f'code {code}: {last_line(log)}' if code else gap}). "
                     f"See {log}")
            part.rename(d)
    base_nca = max((TMP / "base").glob("*.nca"), key=lambda p: p.stat().st_size)
    upd_nca = max((TMP / "update").glob("*.nca"), key=lambda p: p.stat().st_size)
    print(f"base program NCA:   {base_nca.name}")
    print(f"update program NCA: {upd_nca.name}", flush=True)

    # For a BKTR section hactool ignores --romfs; only --section1 writes the image (the final RomFS level, without the
    # hash levels). -y checks the six IVFC levels.
    print("Merging the RomFS (base + 1.3.0); this takes a few minutes ...", flush=True)
    bytes_.watch(lambda: file_bytes(OUT))
    code = run_hactool(["-x", "-y", "--disablekeywarns", "--suppresskeys", "-k", str(HAC_HOME / ".switch" / "prod.keys"),
                        f"--basenca={base_nca}", f"--section1={OUT}", str(upd_nca)], LOG)
    text = LOG.read_text(encoding="utf-8", errors="replace")
    # Nintendo's RSA signatures (fixed-key, NPDM, ACID) say nothing about the data: the release hactool.exe marks them
    # GOOD and the Linux build (tools/build_hactool_linux.sh) FAIL. Integrity comes from the six IVFC levels.
    checked = "\n".join(l for l in text.splitlines() if not re.search(r"Signature \(FAIL\)", l))
    levels = sum(1 for i in range(6) if f"Level {i} (GOOD)" in text)
    if code or re.search(r"\(FAIL\)|\(INVALID\)|Failed to|Error:", checked) or levels != 6:
        OUT.unlink(missing_ok=True)
        fail(f"the merge did not verify (code {code}, good IVFC levels: {levels}/6). See {LOG}")
    bytes_.done(image_size())
    bytes_.stop.set()
    if not OUT.is_file() or OUT.stat().st_size < 2**30:
        fail(f"hactool verified but did not write {OUT}. NCAs kept in {TMP} for another try. See {LOG}")

    # 4. cleanup: temporary NCAs, links and the copied keys
    for d in (TMP / "base", TMP / "update", inp, HAC_HOME):
        shutil.rmtree(d, ignore_errors=True)
    print(f"OK: {OUT} ({OUT.stat().st_size / 2**30:.2f} GiB), six IVFC levels verified.", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
