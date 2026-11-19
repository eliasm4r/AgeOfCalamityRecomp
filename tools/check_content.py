#!/usr/bin/env python3
"""Content guard: keeps game data, keys and generated files out of git.

Modos:
    python3 tools/check_content.py --staged   # files in the index (pre-commit hook)
    python3 tools/check_content.py --all      # every tracked file (CI)
    python3 tools/check_content.py FILE...    # specific files

Silent with code 0 when all is well; otherwise lists the problems and exits with 1.
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Every file the code generator emits must carry this marker; such a file can never be committed.
GENERATED_MARKER = "AOCRECOMP-GENERATED-DO-NOT-COMMIT"

MAX_BYTES = 2 * 1024 * 1024  # above this, only with an entry in ALLOW_LARGE

ALLOW_LARGE: set[str] = set()

FORBIDDEN_SUFFIXES = (
    ".nsp", ".xci", ".nsz", ".xcz", ".nca", ".ncz", ".tik", ".cert", ".keys",
    ".nso", ".npdm", ".rdb", ".g1t", ".g1m", ".bk2", ".webm", ".spv", ".metallib",
    ".bin", ".dmp",
)
# Dados RDB: .rdb.bin, .rdb.bin1..3, .rdb.bin3_0 (DLC)
BIN_SUFFIX = re.compile(r"\.bin\d*(_\d+)?$")
FORBIDDEN_PARTS = ("build/", "generated/", "dist/", "refs/", "romfs/", "exefs/", "shader-cache/",
                   "pipeline-cache/", "mesa-cache/", "saves/")

# Binary signatures of Switch formats (start of the file)
MAGICS = {
    b"NSO0": "NSO module",
    b"META": "NPDM",
    b"PFS0": "container PFS0/NSP",
    b"HFS0": "container HFS0/XCI",
    b"_DRK": "Koei Tecmo RDB index",
    b"IDRK": "entrada RDB da Koei Tecmo",
    b"_S2G": "container de shaders G2S",
    b"BNSH": "shader binary",
}

# Nomes de chaves do Switch seguidos de valor hexadecimal (formato prod.keys)
KEY_LINE = re.compile(
    r"^\s*(header_key|key_area_key_\w+|titlekek_\w+|master_key_\w+|package2_key_\w+|"
    r"aes_kek_generation_source|aes_key_generation_source|sd_card_\w+_key\w*)\s*=\s*[0-9a-fA-F]{32,}",
    re.M,
)
TITLE_KEY_LINE = re.compile(r"^\s*0100[0-9a-fA-F]{12}[0-9a-fA-F]{16}\s*=\s*[0-9a-fA-F]{32}\s*$", re.M)


def git(*args: str) -> list[str]:
    out = subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout
    return [line for line in out.splitlines() if line]


def check_file(rel: str, root: Path = ROOT) -> list[str]:
    problems: list[str] = []
    low = rel.lower()
    if low.endswith(FORBIDDEN_SUFFIXES) or BIN_SUFFIX.search(low):
        problems.append("game data or key extension")
    if any(seg + "/" in FORBIDDEN_PARTS for seg in low.split("/")[:-1]):
        problems.append("output/extraction folder (must stay out of git)")
    if low.rsplit("/", 1)[-1] in ("prod.keys", "title.keys", "dev.keys", "config.local.toml"):
        problems.append("local keys/configuration file")

    path = root / rel
    if not path.is_file():
        return problems
    size = path.stat().st_size
    if size > MAX_BYTES and rel not in ALLOW_LARGE:
        problems.append(f"large file ({size} bytes); add it to ALLOW_LARGE only if it is third-party and licensed")
    with path.open("rb") as f:
        head = f.read(8)
        f.seek(0)
        data = f.read(min(size, 8 * 1024 * 1024))
    for magic, what in MAGICS.items():
        if head.startswith(magic):
            problems.append(f"Switch binary content ({what})")
    if rel != "tools/check_content.py" and GENERATED_MARKER.encode() in data:
        problems.append("generated file (contains the generator marker)")
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        text = ""
    if text and (KEY_LINE.search(text) or TITLE_KEY_LINE.search(text)):
        problems.append("parece conter chaves do console")
    return problems


def main(argv: list[str]) -> int:
    if not argv or argv[0] == "--staged":
        files = git("diff", "--cached", "--name-only", "--diff-filter=ACMR")
    elif argv[0] == "--all":
        files = git("ls-files")
    else:
        files = [Path(a).resolve().relative_to(ROOT).as_posix() for a in argv]
    bad = 0
    for rel in files:
        for p in check_file(rel):
            print(f"{rel}: {p}")
            bad += 1
    if bad:
        print(f"\n{bad} problem(s). Game data, keys and generated files belong in build/ or generated/.")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
