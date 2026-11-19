# Contributing

Thanks for helping. This page covers building from source and the rules every change follows. How the port works is
in [`docs/NOTES.md`](docs/NOTES.md).

## Priorities

- **Correct picture before speed.** Never hide missing work (skipped draws, zeroed textures, relaxed depth) and call
  it a fix.
- **Real implementations.** No stubs that only return success for a service the game uses.
- **Steam Deck performance.** The frame budget is a requirement. Measure with the overlay and `AOC_TRACE=perf` instead
  of guessing; no per-frame allocation on hot paths.

## Game content stays out

This repository is public-ready: game data lives outside it and everything derived from it stays in ignored folders.

- The dump and keys go in `Game Files/` (ignored; the tools use that path by default). Tools take the key file's path
  and read it themselves: **key values never appear in output, logs, commands or files**. Neither do tokens.
- Everything extracted or generated goes to `build/` or `generated/`; the finished folders go to `dist/`.
- Every file the generator writes carries `GENERATED_MARKER` (defined in `tools/check_content.py`).
- Run `python3 tools/check_content.py --staged` before committing. Enable the same check as a hook once per clone:
  `git config core.hooksPath .githooks`. CI runs it too.

## Reusing code

- Copy only MIT, BSD, ISC, zlib or Apache-2.0 code, keep its header and add a line to
  [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) with the project, commit and files.
- yuzu, suyu, Eden, Citron and any GPL code: read it to understand behaviour, then write our own.
- nxnative addresses (`main+0x...`) are valid for 1.3.0 with the build IDs in `tools/data/known-builds.json`; check the
  original bytes in the dump before using any of them.

## Code rules

- Generated C is never edited by hand: change the generator (`recomp/`) and generate again.
- Every lifter change passes the differential tests against Unicorn (Unicorn is used in tests only, never linked into
  the game).
- `recomp/layout.py` is the only place that decides module bases.
- Game patches are lifter patches in `recomp/patches-v130.txt` with their original bytes checked, never output edits.
- Comments say what the code does and why. Cite our code by file and function, external code by project, file and
  function.
- Everything is written in English: docs, comments, messages and commits. Commit messages are plain sentences without
  type prefixes. Only the game's own name-entry keyboard follows the game language.

## Building from source

Everything compiles on a PC: WSL2 or Linux builds the Steam Deck / Linux folder (`dist/aoc-deck/`), MSYS2 builds the
Windows one (`dist/aoc-windows/`). The Deck compiles nothing.

### The build tool (what the setup runs)

```sh
python tools/make_build.py --game-dir "D:/dump" --deck --windows [--with-romfs] [--send] [--check-only]
```

Finished steps are skipped, so building again is incremental:

| Step | What it does |
|---|---|
| `check` | `prod.keys` and the inventory (packages are identified by their contents; an update other than 1.3.0 is refused), disk space and RAM |
| `tools` | sets up the environment: the private **`AoC-Recomp`** WSL distro and `tools/setup_wsl.sh --minimal` for the Deck target, MSYS2 UCRT64 in `C:/msys64` for the Windows target, the system package manager on Linux |
| `extract` | `build/v130/exefs`, reused only when its build IDs are 1.3.0's |
| `romfs` | `build/v130/romfs.bin` with `tools/merge_romfs.py` |
| `dlc` | `build/v130/dlc-<title id>.bin`, the RomFS of each DLC (`tools/extract_dlc.py`) |
| `deck` | `build.py gen guest dist` in WSL (or natively on Linux), giving `dist/aoc-deck` |
| `wingen` | the generated C for Windows |
| `winxlate` | the shader translator `aoc_xlate.exe` (prebuilt in packages, published by `dotnet` from the repository) |
| `winbuild` | `build.py guest host` in MSYS2 |
| `windist` | `dist/aoc-windows` (exe, DLLs, translator, ExeFS, RomFS by hard link, index, `aoc.ini`) |
| `send` | `tools/deck/deploy.sh --no-build` |

The setup (`setup/setup.py`, tkinter) runs this script and reads its `@@PLAN`, `@@STEP`, `@@NEED` and `@@DONE`
lines. In a copy of the repository open it with `setup.bat` (any Python 3 with tkinter) or `sh setup.sh`. Release
packages come from `tools/make_release.py`: the repository at HEAD plus an embeddable Python with Tcl/Tk,
`AgeOfCalamityRecomp.exe` (its `pythonw.exe` renamed, which `setup/sitecustomize.py` turns into the setup), hactool and
the prebuilt translator, and nothing of the game.

The icon (`assets/icon.ico` for `aoc.exe` and the setup, `assets/icon.png` on Linux) is cut from
`assets/key-art.jpg` by `tools/make_icon.py` (Pillow); `--list` shows the other crops kept aside.

### By hand in WSL or Linux

```sh
bash tools/setup_wsl.sh                       # clang, lld, ninja, SDL3, Opus, libvpx, zstd, Vulkan, Python venv
source ~/.venvs/aoc/bin/activate
python3 tools/extract.py inventory            # checks base, update 1.3.0 and DLCs in "Game Files/"
python3 tools/extract.py exefs                # build/v130/exefs/
python3 tools/merge_romfs.py                  # build/v130/romfs.bin
python3 tools/extract_dlc.py                  # build/v130/dlc-<title id>.bin (optional)
python3 tools/build.py --work ~/aoc-build     # gen + guest + host
python3 tools/build.py --work ~/aoc-build dist --with-romfs   # dist/aoc-deck/
```

- `gen` writes about 600 MB of C (about 3 min) and runs again by itself when `recomp/` or the patch table changes.
- `guest` compiles 377 files into `libaoc_guest.a`; 40 giant functions of `main` (1-7 MB of C, up to 1.6 GB of RAM
  each) dominate it, about 48 min on a 6-core laptop. Rebuilds are incremental.
- Keep the work folder on the Linux disk (`~/aoc-build`), never under `/mnt/c` or `/mnt/d`.
- `dist` links against the Steam Runtime 3 sniper sysroot (downloaded once, 1.2 GB), so the result needs GLIBC 2.29 at
  most and runs on the Deck.
- The `host` executable uses the build machine's glibc and is for development: `aoc --selftest`,
  `AOC_GPU=null aoc --boot`. WSL only has Dozen for Vulkan, so graphics tests run on the Deck, native Linux or Windows.

### Native Windows (MSYS2 UCRT64)

```sh
pacman -S mingw-w64-ucrt-x86_64-{clang,lld,llvm,ninja,sdl3,opus,libvpx,zstd,vulkan-headers,vulkan-loader,vulkan-memory-allocator,python,python-cryptography,python-lz4}
python3 tools/build.py --work /c/aoc-win --big-jobs 2 guest host   # aoc.exe in C:\aoc-win\host
```

### Tests

```sh
python3 tests/flags_check.py
python3 tests/diff/lift.py --modules main,subsdk0,sdk --jobs 16
python3 tests/diff/func.py --modules main,subsdk0,sdk --functions 300 --jobs 16
python3 tests/jumptables_check.py --modules main,subsdk0,sdk
python3 tests/diff/simd.py && python3 tests/diff/atomic.py     # no game files needed
python3 tests/spin_waits_test.py && python3 tests/test_tools.py
```

Test dependencies: `pip install numpy lz4 capstone unicorn`.

## Reporting a problem

In the setup window, **Report a problem...** builds `aoc-report.zip` (logs and PC details, nothing of the game, the user
name replaced by `user`) and opens the [issue form](https://github.com/eliasm4r/AgeOfCalamityRecomp/issues). On a
Steam Deck without the setup, `./aoc-logs.sh bundle` makes the same archive.
