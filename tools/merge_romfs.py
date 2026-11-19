"""Merges the base 1.0.0 RomFS with the BKTR patch of update 1.3.0 using hactool: build/v130/romfs.bin.

The launcher runs this on Windows and Linux:

  python tools/merge_romfs.py [--game-dir "Game Files"] [--keys <prod.keys>]

1. prepares build/v130/tmp/hactool-home/.switch/{prod,title}.keys (tools/extract.py hactool-home; values never shown);
2. opens the base and update NSPs where they are, or, when a path is not plain ASCII (the Windows hactool does not
   open those), hard-links them under ASCII names (copied when the dump is on another disk);
3. extracts the NCAs of both NSPs (~11 GB temporary) and merges section 1 of the update's program NCA (BKTR) over the
   base, checking the six IVFC levels (-y);
4. deletes the temporary files and the copied keys, only once the image exists.

Needs ~23 GB free during the merge (temporary NCAs + the 10.9 GiB image). hactool comes from prebuilt/hactool/ (the
launcher package) or toolchain/hactool-1.4.0-win/ (development on Windows).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

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


def find_nsps(game_dir: Path, keys: Path) -> tuple[Path, Path]:
    """The base and update 1.3.0 NSPs, found by their contents (tools/extract.py scan: title id and version from the
    .cnmt), whatever the file name."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import extract
    import nx
    pkgs = [p for p in extract.scan(game_dir, nx.load_keys(keys)) if p["container"] == "nsp"]
    out = []
    for role, what in (("base", "of the base game (01002B00111A2000)"), ("update", "of update 1.3.0 (01002B00111A2800)")):
        found = [p["path"] for p in pkgs if p["role"] == role]
        if len(found) != 1:
            fail(f"expected 1 NSP {what} in {game_dir}, found {len(found)}")
        out.append(found[0])
    return out[0], out[1]


def link_or_copy(src: Path, dst: Path) -> Path:
    """The path hactool opens for src: src itself when it is plain ASCII (the Windows hactool fails only on other
    characters), else dst, hard-linked or, on another disk, copied (11 GB for the base game)."""
    if str(src).isascii():
        return src
    if dst.exists():
        return dst
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


def run_hactool(args: list, env=None, log=None) -> int:
    p = subprocess.run([str(hactool())] + args, env=env, stdout=log or subprocess.DEVNULL, stderr=subprocess.STDOUT)
    return p.returncode


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game-dir", type=Path, default=ROOT / "Game Files")
    ap.add_argument("--keys", type=Path, help="default: prod.keys in the game folder")
    args = ap.parse_args()
    keys = args.keys or args.game_dir / "prod.keys"
    if OUT.exists():
        print(f"{OUT} already exists; delete it to redo it")
        return 0
    hac = hactool()
    print(f"hactool: {hac}", flush=True)

    # 1. keys for hactool (extract.py never prints values)
    subprocess.run([sys.executable, str(ROOT / "tools/extract.py"), "hactool-home", "--game-dir", str(args.game_dir),
                    "--keys", str(keys)], check=True)
    if not (HAC_HOME / ".switch" / "title.keys").is_file():
        fail("extract.py hactool-home did not create the keys")

    # 2. the NSPs under ASCII names
    inp = TMP / "in"
    inp.mkdir(parents=True, exist_ok=True)
    base_nsp, update_nsp = find_nsps(args.game_dir, keys)
    nsps = {"base": link_or_copy(base_nsp, inp / "base.nsp"), "update": link_or_copy(update_nsp, inp / "update.nsp")}

    # 3. NCAs of both NSPs (the base is ~11 GB) and the merge
    for name in ("base", "update"):
        d = TMP / name
        if not d.is_dir():
            print(f"Extracting {name}.nsp ...", flush=True)
            if run_hactool(["-t", "pfs0", f"--outdir={d}", str(nsps[name])]):
                fail(f"hactool could not open {name}.nsp")
    base_nca = max((TMP / "base").glob("*.nca"), key=lambda p: p.stat().st_size)
    upd_nca = max((TMP / "update").glob("*.nca"), key=lambda p: p.stat().st_size)
    print(f"base program NCA:   {base_nca.name}")
    print(f"update program NCA: {upd_nca.name}", flush=True)

    # For a BKTR section hactool ignores --romfs; only --section1 writes the image (the final RomFS level, without the
    # hash levels). -y checks the six IVFC levels.
    print("Merging the RomFS (base + 1.3.0); this takes a few minutes ...", flush=True)
    env = dict(os.environ, HOME=str(HAC_HOME), USERPROFILE=str(HAC_HOME))
    with open(LOG, "wb") as log:
        code = run_hactool(["-x", "-y", "--disablekeywarns", "--suppresskeys", "-k",
                            str(HAC_HOME / ".switch" / "prod.keys"), f"--basenca={base_nca}", f"--section1={OUT}",
                            str(upd_nca)], env=env, log=log)
    text = LOG.read_text(encoding="utf-8", errors="replace")
    # Nintendo's RSA signatures (fixed-key, NPDM, ACID) say nothing about the data: the release hactool.exe marks them
    # GOOD and the Linux build (tools/build_hactool_linux.sh) FAIL. Integrity comes from the six IVFC levels.
    checked = "\n".join(l for l in text.splitlines() if not re.search(r"Signature \(FAIL\)", l))
    levels = sum(1 for i in range(6) if f"Level {i} (GOOD)" in text)
    if code or re.search(r"\(FAIL\)|\(INVALID\)|Failed to|Error:", checked) or levels != 6:
        OUT.unlink(missing_ok=True)
        fail(f"the merge did not verify (code {code}, good IVFC levels: {levels}/6). See {LOG}")
    if not OUT.is_file() or OUT.stat().st_size < 2**30:
        fail(f"hactool verified but did not write {OUT}. NCAs kept in {TMP} for another try. See {LOG}")

    # 4. cleanup: temporary NCAs, links and the copied keys
    for d in (TMP / "base", TMP / "update", inp, HAC_HOME):
        shutil.rmtree(d, ignore_errors=True)
    print(f"OK: {OUT} ({OUT.stat().st_size / 2**30:.2f} GiB), six IVFC levels verified.", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
