#!/usr/bin/env python3
"""Identifies and extracts the Age of Calamity dump into build/ (ignored by git).

Subcommands:
    inventory  lists the packages (base, update, DLCs) found in --game-dir, by the contents of their NCAs
    romfs-list lists the RomFS files of the base and the DLCs (metadata only) and writes build/v130/romfs-*.tsv
    romfs-index lists build/v130/romfs.bin (merged RomFS 1.3.0) into romfs-v130.tsv and compares it with the base
    hactool-home  prepares build/v130/tmp/hactool-home/.switch/{prod,title}.keys for hactool to merge the
               update (BKTR): title keys taken from the NSPs' tickets; prod.keys without comments and with
               Lockpick's extra NUL removed. Used by tools/merge_romfs.py.
    exefs      extracts the ExeFS of update 1.3.0 (or of the base, with --base-only) into build/v130/exefs/,
               checking each block against the hash table, and writes a manifest with SHA-256

Example (from the repository root):
    python3 tools/extract.py inventory --game-dir "Game Files" --keys "Game Files/prod.keys"
    python3 tools/extract.py exefs     --game-dir "Game Files" --keys "Game Files/prod.keys"

Reads the files in place (does not copy the 10 GB NCA). Keys stay in memory only and are never printed.
Merging the base RomFS with the update (BKTR) is tools/merge_romfs.py.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nx  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
BASE_TID = "01002B00111A2000"
UPDATE_TID = "01002B00111A2800"
DLC_PREFIX = "01002B00111A3"
UPDATE_VERSION = 327680   # the title number of update 1.3.0, the only one the port accepts (tools/data/known-builds.json)


def add_source_args(ap: argparse.ArgumentParser) -> None:
    """--game-dir and --nsp, the same in every tool that reads the dump."""
    ap.add_argument("--game-dir", type=Path, default=None,
                    help='folder with the NSPs, subfolders included (default "Game Files" when no --nsp is given)')
    ap.add_argument("--nsp", type=Path, action="append", default=[],
                    help="an NSP or XCI chosen on its own, anywhere (repeatable; with --game-dir or instead of it)")


def game_dir_of(args) -> Path | None:
    """The folder to search: --game-dir, else "Game Files" when no file was chosen on its own, else none."""
    return args.game_dir if args.game_dir else None if args.nsp else ROOT / "Game Files"


def package_paths(game_dir: Path | None, nsps=()) -> list[Path]:
    """The NSPs and XCIs of the folder (subfolders included) and the files chosen on their own, each once."""
    paths = {}
    if game_dir:
        for p in game_dir.rglob("*"):
            if p.suffix.lower() in (".nsp", ".xci") and p.is_file():
                paths.setdefault(p.resolve(), p)
    for p in nsps:
        paths.setdefault(Path(p).resolve(), Path(p))
    return sorted(paths.values(), key=lambda p: (p.name.lower(), str(p)))


def sources_text(game_dir: Path | None, nsps=()) -> str:
    """Where the packages were looked for, for messages."""
    return " and ".join(([str(game_dir)] if game_dir else []) + (["the chosen files"] if nsps else [])) or "nowhere"


def scan(paths: list[Path], keys: dict) -> list[dict]:
    """Each package of paths (package_paths) identified by its contents."""
    found = []
    for path in paths:
        with path.open("rb") as f:
            kind, entries = nx.container_entries(f, path.stat().st_size)
            tickets = nx.common_tickets(f, entries)
            ncas = nx.nca_inventory(f, entries, keys)
            try:
                cnmt = nx.package_version(f, ncas, keys, tickets)
            except ValueError:
                cnmt = None
        meta = [n for n in ncas if n.content_type == 1]
        package_tid = cnmt[0] if cnmt else meta[0].title_id if meta else (ncas[0].title_id if ncas else "?")
        version = cnmt[1] if cnmt else None
        if package_tid == BASE_TID:
            role = "base"
        elif package_tid == UPDATE_TID:
            # an update of another version (1.4.1 exists, v458752) does not fit: the generated code and the patches are 1.3.0's
            role = "update" if version in (None, UPDATE_VERSION) else "update-x"
        elif package_tid.startswith(DLC_PREFIX):
            role = "dlc"
        else:
            role = "outro"
        found.append({"path": path, "container": kind, "role": role, "title_id": package_tid, "version": version,
                      "ncas": ncas, "tickets": len(tickets)})
    return found


def describe(pkgs: list[dict]) -> None:
    for p in pkgs:
        version = "" if p["version"] is None else f"v{p['version']}  "
        print(f"[{p['role']:8s}] {p['title_id']}  {version}{p['path'].name}")
        for n in p["ncas"]:
            secs = ", ".join(f"{i}:{n.section_kind(i)}" for i in n.sections())
            print(f"         {n.content_name:10s} {n.size / 2**20:10.1f} MiB  keygen 0x{n.key_generation:02x}"
                  f"  sdk {n.sdk_version:8s} sections [{secs}]")


def cmd_inventory(args, keys) -> int:
    pkgs = scan(package_paths(game_dir_of(args), args.nsp), keys)
    describe(pkgs)
    roles = [p["role"] for p in pkgs]
    # Each problem also goes out as a "@@PROBLEM <code> <detail>" line, which tools/make_build.py words for the player.
    problems = []
    if roles.count("base") != 1:
        problems.append(("base", roles.count("base"), f"expected 1 base game, found {roles.count('base')}"))
    if roles.count("update") > 1:
        problems.append(("updates", roles.count("update"), f"expected 1 update 1.3.0, found {roles.count('update')}"))
    elif not roles.count("update"):
        other = [p for p in pkgs if p["role"] == "update-x"]
        if other:
            problems.append(("wrong_update", other[0]["version"],
                             f"the folder's update is version v{other[0]['version']} ({other[0]['path'].name}); the port "
                             f"only accepts update 1.3.0 (v{UPDATE_VERSION})"))
        else:
            problems.append(("update", 0, "update 1.3.0 is missing"))
    for code, detail, msg in problems:
        print("PROBLEM:", msg)
        print(f"@@PROBLEM {code} {detail}")
    return 1 if problems else 0


def cmd_exefs(args, keys) -> int:
    pkgs = scan(package_paths(game_dir_of(args), args.nsp), keys)
    role = "base" if args.base_only else "update"
    chosen = [p for p in pkgs if p["role"] == role]
    nx.require(len(chosen) == 1, f"expected exactly 1 '{role}' package in {sources_text(game_dir_of(args), args.nsp)}")
    pkg = chosen[0]
    out = args.work / "exefs"
    nx.require(not out.exists() or args.force, f"{out} already exists (use --force to redo it)")
    with pkg["path"].open("rb") as f:
        _, entries = nx.container_entries(f, pkg["path"].stat().st_size)
        tickets = nx.common_tickets(f, entries)
        programs = [n for n in nx.nca_inventory(f, entries, keys) if n.content_type == 0]
        nx.require(len(programs) == 1, "expected 1 program NCA")
        nca = programs[0]
        nx.require(nca.title_id == BASE_TID, f"program NCA of another game ({nca.title_id})")
        files = nx.read_exefs(f, nca, nx.nca_ctr_key(nca, keys, tickets))
    out.mkdir(parents=True, exist_ok=True)
    listing = []
    for name, data in files:
        (out / name).write_bytes(data)
        listing.append({"name": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()})
        print(f"  {name:10s} {len(data):>10d} B")
    manifest = {"source": pkg["path"].name, "role": role, "program_nca": nca.name, "sdk": nca.sdk_version,
                "key_generation": nca.key_generation,
                "integrity": "FS header, hash table and every ExeFS block verified; RSA signatures not",
                "files": listing}
    (args.work / "exefs-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"ExeFS in {out}")
    return 0


def cmd_romfs_list(args, keys) -> int:
    from collections import Counter
    pkgs = [p for p in scan(package_paths(game_dir_of(args), args.nsp), keys) if p["role"] in ("base", "dlc")]
    args.work.mkdir(parents=True, exist_ok=True)
    for pkg in pkgs:
        with pkg["path"].open("rb") as f:
            _, entries = nx.container_entries(f, pkg["path"].stat().st_size)
            tickets = nx.common_tickets(f, entries)
            for nca in nx.nca_inventory(f, entries, keys):
                if nca.content_type not in (0, 5):
                    continue
                key = nx.nca_ctr_key(nca, keys, tickets)
                for i in nca.sections():
                    if nca.section_kind(i) != "romfs":
                        continue
                    files = nx.romfs_files(nx.SectionReader(f, nca, i, key))
                    tsv = args.work / f"romfs-{nca.title_id}.tsv"
                    tsv.write_text("".join(f"{size}\t{path}\n" for path, _o, size in files), encoding="utf-8")
                    total = sum(s for _p, _o, s in files)
                    kinds = Counter()
                    for path, _o, size in files:
                        base = path.rsplit("/", 1)[-1]
                        k = ("rdb-dlc" if ".rdb.bin3_" in base else "rdb-index" if base.endswith(".rdb")
                             else "rdb-data" if ".rdb.bin" in base else "rdb-file" if base.endswith(".file")
                             else base.rsplit(".", 1)[-1] if "." in base else "(sem ext)")
                        kinds[k] += size
                    print(f"[{pkg['role']}] {nca.title_id}: {len(files)} files, {total / 2**30:.2f} GiB  → {tsv.name}")
                    for k, v in kinds.most_common(8):
                        print(f"      {k:12s} {v / 2**20:10.1f} MiB")
    return 0


def cmd_romfs_index(args, keys) -> int:
    image = args.work / "romfs.bin"
    with image.open("rb") as f:
        files = nx.romfs_image_files(f)
    (args.work / "romfs-v130.tsv").write_text("".join(f"{s}\t{p}\n" for p, _o, s in files), encoding="utf-8")
    base_tsv = args.work / f"romfs-{BASE_TID}.tsv"
    base = {}
    if base_tsv.exists():
        for line in base_tsv.read_text(encoding="utf-8").splitlines():
            size, path = line.split("\t", 1)
            base[path] = int(size)
    new = {p: s for p, _o, s in files}
    added = sorted(set(new) - set(base))
    removed = sorted(set(base) - set(new))
    changed = sorted(p for p in set(new) & set(base) if new[p] != base[p])
    total = sum(new.values())
    print(f"RomFS 1.3.0: {len(files)} files, {total / 2**30:.2f} GiB (image {image.stat().st_size / 2**30:.2f} GiB)")
    if base:
        print(f"compared with base 1.0.0: {len(added)} new, {len(removed)} removed, {len(changed)} with another size")
        for label, items in (("novo", added), ("removido", removed), ("mudou", changed)):
            for path in items[:15]:
                print(f"   {label:8s} {path}  {base.get(path, 0) / 2**20:.1f} → {new.get(path, 0) / 2**20:.1f} MiB")
    return 0


def cmd_hactool_home(args, keys) -> int:
    import os
    import re
    tickets: dict[bytes, bytes] = {}
    for pkg in scan(package_paths(game_dir_of(args), args.nsp), keys):
        if pkg["role"] not in ("base", "update"):
            continue
        with pkg["path"].open("rb") as f:
            _, entries = nx.container_entries(f, pkg["path"].stat().st_size)
            tickets.update(nx.common_tickets(f, entries))
    home = args.work / "tmp" / "hactool-home" / ".switch"
    home.mkdir(parents=True, exist_ok=True)
    lines = []
    for line in args.keys.read_text(encoding="utf-8", errors="replace").splitlines():
        body = line.split("#", 1)[0]
        if "=" not in body:
            continue
        name, value = (part.strip() for part in body.split("=", 1))
        if re.fullmatch(r"(?:mariko_)?master_kek_source_[0-9a-f]{2}", name.lower()) and len(value) == 34 \
                and value[-2:] == "00":
            value = value[:32]
        lines.append(f"{name} = {value}")
    for name, text in (("prod.keys", "\n".join(lines) + "\n"),
                       ("title.keys", "".join(f"{r.hex()} = {k.hex()}\n" for r, k in tickets.items()))):
        target = home / name
        fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as out:
            out.write(text)
    print(f"hactool-home ready in {home.parent} ({len(lines)} keys, {len(tickets)} title keys; values not shown)")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("command", choices=("inventory", "exefs", "romfs-list", "romfs-index", "hactool-home"))
    add_source_args(ap)
    ap.add_argument("--keys", type=Path, default=ROOT / "Game Files" / "prod.keys")
    ap.add_argument("--work", type=Path, default=ROOT / "build" / "v130")
    ap.add_argument("--base-only", action="store_true", help="ExeFS of base 1.0.0 instead of the update")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args(argv)
    keys = nx.load_keys(args.keys)
    if args.command == "inventory":
        return cmd_inventory(args, keys)
    if args.command == "romfs-list":
        return cmd_romfs_list(args, keys)
    if args.command == "romfs-index":
        return cmd_romfs_index(args, keys)
    if args.command == "hactool-home":
        return cmd_hactool_home(args, keys)
    return cmd_exefs(args, keys)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ValueError as e:
        print("ERROR:", e)
        sys.exit(1)
