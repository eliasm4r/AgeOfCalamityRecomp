#!/usr/bin/env python3
"""Matches the imports of the Age of Calamity modules against the `nn::*` functions the reference runtimes already implement.

The Astral Chain and TotK Recomp runtimes replace SDK functions with host implementations through
`HOST_OVERRIDE("<mangled name>", ...)` (and observe others with `HOST_OBSERVER`). This census tells, for the imports
of `main` and `subsdk0` (build/v130/imports.json, from tools/census_imports.py):

- which already have an override in a reference runtime (reusable);
- which do not — grouped by namespace, this is the work list (many need no override: the `sdk` itself resolves them,
  and they only become work when they reach a system service).

Function names only; no game data.

Usage:
    python3 tools/census_overrides.py --runtime refs/astral-chain-pc-recomp/runtime --runtime refs/totk-pc-recomp/runtime
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from census_imports import namespace  # noqa: E402

PATTERN = re.compile(r'HOST_(OVERRIDE|OBSERVER)\(\s*"((?:[^"\\]|\\.)*)"')


def host_names(runtime: Path) -> dict[str, str]:
    names: dict[str, str] = {}
    for path in sorted(runtime.rglob("*.c")) + sorted(runtime.rglob("*.h")):
        for m in PATTERN.finditer(path.read_text(encoding="utf-8", errors="replace")):
            names.setdefault(m.group(2), m.group(1).lower())
    return names


def demangle(names: list[str]) -> dict[str, str]:
    try:
        out = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True, check=True).stdout
        return dict(zip(names, out.splitlines()))
    except (OSError, subprocess.CalledProcessError):
        return {n: n for n in names}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--imports", type=Path, default=ROOT / "build" / "v130" / "imports.json")
    ap.add_argument("--runtime", type=Path, action="append", required=True,
                    help="runtime/ folder of a reference project (can repeat)")
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "v130" / "overrides.json")
    args = ap.parse_args(argv)
    imports = json.loads(args.imports.read_text(encoding="utf-8"))
    refs = {rt.parent.name: host_names(rt) for rt in args.runtime}
    for name, names in refs.items():
        print(f"{name}: {sum(1 for v in names.values() if v == 'override')} overrides, "
              f"{sum(1 for v in names.values() if v == 'observer')} observers")
    report = {}
    for mod in ("main", "subsdk0"):
        imp = [n for n in imports[mod]["import_names"] if not n.startswith("__rel_")]
        covered = {n: [r for r, names in refs.items() if n in names] for n in imp}
        have = [n for n in imp if covered[n]]
        missing = [n for n in imp if not covered[n]]
        by_ns_have, by_ns_missing = Counter(namespace(n) for n in have), Counter(namespace(n) for n in missing)
        print(f"\n{mod}: {len(imp)} imports — {len(have)} with an override in some reference, {len(missing)} without")
        print(f"    {'namespace':22s} {'with':>5s} {'w/o':>5s}")
        for ns in sorted(set(by_ns_have) | set(by_ns_missing), key=lambda k: -(by_ns_have[k] + by_ns_missing[k]))[:20]:
            print(f"    {ns:22s} {by_ns_have[ns]:5d} {by_ns_missing[ns]:5d}")
        dm = demangle(missing)
        groups = defaultdict(list)
        for n in missing:
            groups[namespace(n)].append(dm[n])
        report[mod] = {"imports": len(imp), "with_override": {n: covered[n] for n in have},
                       "without_override": {ns: sorted(v) for ns, v in sorted(groups.items())}}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"\nreport: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
