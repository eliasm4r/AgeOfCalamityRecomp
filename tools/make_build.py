"""Builds the game from the player's own dump, start to finish, for the setup (setup/setup.py) or by hand.

Runs on Windows (the release's embedded Python, or MSYS2's) and on Linux (the system python3: this script needs only
the standard library; the tools it calls run in ~/.venvs/AgeOfCalamityRecomp, which the tools step makes).

Steps (each prints "@@STEP <id> <run|ok|fail|skip> <text>"; "@@PROGRESS <0-1> <seconds left>" is the whole build's
progress; every other line is log; the last line is "@@DONE ok|fail"; "@@NEED wsl" asks the setup to offer installing
WSL):

  check     the dump folder (NSPs of the base, update 1.3.0 and DLCs, and prod.keys; each package is identified by
            its contents, not its file name, and an update of another version is refused), free disk and memory
  tools     the build environment, set up once and then only checked:
              Windows + Deck: WSL and a private Arch Linux distro "AgeOfCalamityRecomp" (wsl --install archlinux --name), with
                              tools/setup_wsl.sh --minimal inside (clang, lld, ninja, Python venv)
              Windows target: MSYS2 UCRT64 (C:/msys64; a pinned msys2-base is unpacked there if missing) with clang,
                              SDL3, Opus, libvpx, zstd and Vulkan
              Linux:          the distro's packages and the venv (tools/setup_wsl.sh --minimal; pkexec asks for the
                              password in a window)
  extract   build/v130/exefs from the update (tools/extract.py exefs), once
  romfs     build/v130/romfs.bin, base + update merged (tools/merge_romfs.py with hactool), once
  dlc       build/v130/dlc-<title id>.bin, the RomFS of each DLC in the dump (tools/extract_dlc.py), once; the game
            folders get them next to romfs.bin, and a dump without DLCs builds a game without them
  deck      dist/steamdeck: generate the C, compile and assemble (tools/build.py gen guest dist), in WSL or natively
  wingen    the generated C for Windows: copied from WSL when the Deck step made it, else generated here
  winxlate  xlate.exe, the shader translator for Windows (prebuilt/xlate/win-x64 from the release package)
  winbuild  AgeOfCalamityRecomp.exe in MSYS2 UCRT64 (tools/build.py guest host)
  windist   dist/windows: AgeOfCalamityRecomp.exe, its DLLs, the translator, exefs/, romfs/ (a hard link), romfs-index.bin
  prompts   the Xbox and PlayStation button prompts (tools/make_prompts.py): the game's button textures from romfs.bin
            with Xbox or PlayStation buttons drawn over the Switch ones, into each game folder this build made
  dlss      dist/windows/nvngx_dlss.dll, NVIDIA's DLSS library, downloaded from NVIDIA's DLSS SDK repository when the PC
            has an NVIDIA RTX GPU (skipped otherwise; a failed download warns and the game runs without DLAA)
  shaders   every game shader translated into dist/windows/shader-cache and built into the GPU driver's cache
            (tools/prepare_shaders.py, AgeOfCalamityRecomp.exe --prepare-shaders), so fights do not stutter; skipped
            while shader-prepared.bin is newer than the translator id and of the current version
  send      the Deck folder's changed files to the Steam Deck over SSH (tools/deck/deploy.sh --no-build)

Nothing here reads or prints key values: the key file's path goes to the tools that need it.

  python tools/make_build.py --game-dir "D:/dump" --deck --windows [--send] [--with-romfs]
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION = (ROOT / "VERSION").read_text().strip() if (ROOT / "VERSION").is_file() else "?"
BASE_ID, UPDATE_ID = "01002B00111A2000", "01002B00111A2800"
WIN = os.name == "nt"
DISTRO = "AgeOfCalamityRecomp"   # the private WSL distro the tools step makes
VENV_PY = Path.home() / ".venvs" / "AgeOfCalamityRecomp" / "bin" / "python"
# MSYS2 base, pinned (https://github.com/msys2/msys2-installer/releases/tag/2026-09-27); unpacked only when the PC has
# no MSYS2. The packages come from MSYS2's own mirrors.
MSYS2_SFX = ("https://github.com/msys2/msys2-installer/releases/download/2026-09-27/msys2-base-x86_64-20260927.sfx.exe",
             "ad336cccfda47758b5e15cda993fbba421115cb0b126697daef1ee4dfe37209f")
# python-cryptography and python-lz4: tools/build.py's host step indexes the RomFS (romfs_index.py imports nx.py).
MSYS2_PKGS = ["clang", "lld", "llvm", "ninja", "sdl3", "opus", "libvpx", "zstd", "vulkan-headers", "vulkan-loader",
              "vulkan-memory-allocator", "python", "python-cryptography", "python-lz4"]
GIB = 2**30

TEXT = {
    # step titles
    "t_check": "Check your game files",
    "t_tools": "Install the build tools",
    "t_extract": "Unpack the game code",
    "t_romfs": "Unpack the game data (RomFS 1.3.0)",
    "t_dlc": "Unpack the DLC",
    "t_deck": "Build for Steam Deck / Linux",
    "t_wingen": "Translate the game code for Windows",
    "t_winxlate": "Prepare the shader translator",
    "t_winbuild": "Build for Windows",
    "t_windist": "Put the Windows game folder together",
    "t_prompts": "Draw the Xbox and PlayStation button prompts",
    "t_dlss": "Download NVIDIA DLSS",
    "t_shaders": "Prepare the shaders (once, so fights do not stutter)",
    "t_send": "Send to the Steam Deck",
    # messages
    "no_dir": "the game folder does not exist: {0}",
    "missing": "missing from the chosen folder: {0}",
    "base": "the base game (01002B00111A2000)",
    "update": "update 1.3.0 (01002B00111A2800)",
    "disk": "not enough free space on {0}: {1:.0f} GB free, the build needs about {2:.0f} GB",
    "ram": "WARNING: {0:.0f} GB of RAM; with less than 8 GB the build may fail or take hours",
    "send_deck": "sending to the Deck needs the Deck folder",
    "need_wsl": "WSL is not installed: click \"Install WSL\" (asks for administrator) and restart the PC",
    "wsl_install": "creating the private Linux distro {0} (Arch Linux, ~1 GB, first time only)...",
    "wsl_old": "WSL could not create the distro: update it (in an administrator terminal: wsl --update) and build again",
    "no_distro": "the WSL distro \"{0}\" does not exist (wsl -l -v)",
    "msys_get": "downloading MSYS2 ({0:.0f} MB, first time only)...",
    "msys_hash": "the download of {0} does not match its pinned hash",
    "msys_pkgs": "installing the compiler and libraries into MSYS2 (~1.5 GB, first time only)...",
    "tools_ok": "tools ready",
    "exit": "the command ended with code {0}",
    "no_win_linux": "the Windows version can only be built on Windows",
    "missing_file": "missing {0}",
    "folder_ok": "folder ready: {0}",
    "left": "about {0} min left",
    "no_xlate": "the Windows shader translator is missing (prebuilt/xlate/win-x64) and there is no .NET to build it",
    "no_bash": "Git Bash is not in C:/Program Files/Git (sending uses its ssh and scp)",
    "unexpected": "Unexpected ERROR: {0}",
    "dlss_no_rtx": "no NVIDIA RTX graphics card found ({0}): DLSS is not downloaded",
    "dlss_get": "downloading nvngx_dlss.dll {0} from NVIDIA's DLSS SDK (license: {1})...",
    "dlss_kept": "a DLSS library of another version is already in the game folder: kept",
    "dlss_failed": "WARNING: NVIDIA DLSS could not be downloaded ({0}). The game runs without DLAA; the next build tries again",
    "shaders_kept": "the shaders are already prepared for this translator",
    "update_from": "update: from the game data the first build extracted ({0}), without the NSPs",
    "no_extracted": "the game data the first build extracted is not in {0}: choose the folder with your NSPs and "
                    "prod.keys and build again",
    "dlc_kept": "update: the DLC unpacked by the first build are kept (to add one, build from your NSP folder)",
    "prompts_failed": "WARNING: the Xbox and PlayStation button prompts could not be made ({0}); the game keeps the Switch ones",
    # what tools/extract.py inventory found wrong in the dump ("@@PROBLEM <code> <detail>")
    "p_base": "the base game (01002B00111A2000) is not among the NSPs in the folder (or there is more than one)",
    "p_update": "update 1.3.0 (01002B00111A2800) is not among the NSPs in the folder",
    "p_updates": "there is more than one update 1.3.0 in the folder; keep one",
    "p_wrong_update": "the update in your folder is another version (v{0}; 1.4.1 is v458752). AgeOfCalamityRecomp only works "
                       "with update 1.3.0 (v327680): replace the update NSP with the 1.3.0 one and build again",
    "no_nsp": "there are no .nsp files in the chosen folder",
    "wrong_build": "the extracted game code is not version 1.3.0. AgeOfCalamityRecomp only works with update 1.3.0 (v327680); "
                    "1.4.1 is not accepted yet. Replace the update NSP and build again",
    "stale_exefs": "the game code extracted by an earlier attempt is not version 1.3.0: extracting again",
    "stale_romfs": "the romfs.bin of an earlier attempt is not the 1.3.0 one ({0} bytes): building it again",
    "romfs_size": "the merged RomFS has {0} bytes, the 1.3.0 one has {1}: check that the base game and the update are "
                   "the right ones (base v0 and update 1.3.0)",
    "steamos": "SteamOS does not let the build tools be installed (the system is read-only). Build on a Windows or "
                "Linux PC and copy the dist/steamdeck folder here (README, Steam Deck section)",
}


def tr(key: str, *a) -> str:
    return TEXT[key].format(*a)


LOG = None   # build/logs/forge-<time>.log: everything printed, for the launcher's Report button


OUT_LOCK = threading.Lock()   # the progress ticker prints too


def out(line: str) -> None:
    with OUT_LOCK:
        print(line, flush=True)
        if LOG:
            LOG.write(line + "\n")
            LOG.flush()


def step(sid: str, state: str, text: str = "") -> None:
    out(f"@@STEP {sid} {state} {text}".rstrip())


class StepFailed(Exception):
    pass


# ---- overall progress: "@@PROGRESS <0-1> <seconds left>" (with --prepare: "progress <0-1> <step title> (about n min
# left)", RecompLauncher's format). Each step weighs about what it takes on a first build (seconds on an 8-core PC);
# inside a step the C generator's "emitted a/b chunks", ninja's "[n/m]" lines, the shader counts and the RomFS merge's
# byte counts move it forward. In a step that compiles, the first ninja run is the game code (most of the time) and any
# later one is the runtime. A step with no such lines moves with the time it is expected to take, up to 90 %, so the
# bar never sits still for minutes.
WEIGHTS = {"check": 5, "tools": 120, "extract": 30, "romfs": 300, "dlc": 60, "deck": 900, "wingen": 40,
           "winxlate": 5, "winbuild": 480, "windist": 15, "prompts": 3, "dlss": 10, "shaders": 400, "send": 60}
MEASURED = {"romfs", "deck", "wingen", "winbuild", "shaders"}
NINJA = re.compile(r"^\[(\d+)/(\d+)\] ")
EMITTED = re.compile(r"emitted (\d+)/(\d+) chunks")
# The shaders step (gfx_shader.c, vk_pipeline.c): first the translation ("prepare: n of m pairs", the whole step on
# NVIDIA, about 6 minutes on an RTX 4070), then, on other GPUs, the driver builds them ("preparation progress: n of m
# variants").
SHADERS_XLATE = re.compile(r"prepare: (\d+) of (\d+) pairs")
SHADERS_BUILD = re.compile(r"preparation progress: (\d+) of (\d+) variants")
ROMFS_BYTES = re.compile(r"romfs progress: (\d+) of (\d+) MiB")   # tools/merge_romfs.py
TICK = 2.0   # seconds between the ticker's looks
QUICK = 0.1  # a step done in under this share of its weight in seconds had nothing to do (Progress.finish)


class Progress:
    """The whole build's progress and the time it has left. The weights are a first guess of each step's seconds; the
    time left is the weight still to do times this PC's pace (seconds spent per weight done, measured on the steps that
    did work: a skipped step costs no time and teaches nothing)."""

    def __init__(self):
        self.plan, self.index, self.inside, self.prepare = [], 0, 0.0, False
        self.ninja_totals = []
        self.skipped = set()        # indexes of the steps that had nothing to do
        self.spent = 0.0            # seconds and weight of the finished steps that did work
        self.spent_weight = 0.0
        self.step_started = time.monotonic()
        self.shown = (-1.0, None)
        self.lock = threading.RLock()
        self.ticker = None

    def weight(self, i: int) -> float:
        return 0.0 if i in self.skipped else WEIGHTS.get(self.plan[i], 10)

    def start(self, plan, prepare):
        with self.lock:
            self.plan, self.prepare = [sid for sid, _ in plan], prepare
        if self.ticker is None:
            self.ticker = threading.Thread(target=self.tick_forever, daemon=True)
            self.ticker.start()

    def step(self, index):
        with self.lock:
            self.index, self.inside, self.ninja_totals = index, 0.0, []
            self.step_started = time.monotonic()
            self.report()

    def finish(self, result: str):
        """The current step ended with result (ok or skip). A step done in under QUICK of its weight had nothing to do
        (the tools already installed: 1 s for a weight of 120) and counts as skipped: as done work it made this PC look
        100 times faster than it is, and the first minutes showed seconds left."""
        with self.lock:
            took = time.monotonic() - self.step_started
            if result == "skip" or took < QUICK * WEIGHTS.get(self.plan[self.index], 10):
                self.skipped.add(self.index)
            else:
                self.spent += time.monotonic() - self.step_started
                self.spent_weight += self.weight(self.index)
            self.inside = 1.0

    def line(self, text):
        with self.lock:
            if not self.plan:
                return
            sid = self.plan[self.index]
            if sid == "shaders":
                for pattern, low, high in ((SHADERS_XLATE, 0.0, 0.9), (SHADERS_BUILD, 0.9, 1.0)):
                    m = pattern.search(text)
                    if m:
                        self.advance(low + (high - low) * int(m[1]) / max(1, int(m[2])))
                return
            if sid == "romfs":
                m = ROMFS_BYTES.search(text)
                if m:
                    self.advance(int(m[1]) / max(1, int(m[2])))
                return
            m = EMITTED.search(text)
            if m:   # the generator: all of wingen, the first tenth of the Deck step
                done = int(m[1]) / max(1, int(m[2]))
                self.advance(done * (0.1 if sid == "deck" else 1.0))
                return
            m = NINJA.match(text)
            if m:
                total = int(m[2])
                if total not in self.ninja_totals:
                    self.ninja_totals.append(total)
                first = self.ninja_totals.index(total) == 0
                low, high = ((0.1 if sid == "deck" else 0.0), 0.95) if first else (0.95, 1.0)
                self.advance(low + (high - low) * int(m[1]) / max(1, total))

    def advance(self, inside):
        if inside > self.inside:
            self.inside = min(1.0, inside)
            self.report()

    def tick(self):
        """Every TICK seconds: a step without progress lines moves with its expected time; the time left is shown
        again when its minute changes."""
        with self.lock:
            if not self.plan or self.inside >= 1.0:
                return
            if self.plan[self.index] not in MEASURED:
                expected = WEIGHTS.get(self.plan[self.index], 10) * self.pace()
                self.advance(min(0.9, (time.monotonic() - self.step_started) / max(1.0, expected)))
            self.report()

    def tick_forever(self):
        while True:
            time.sleep(TICK)
            self.tick()

    def pace(self) -> float:
        """Seconds per weight on this PC: finished steps that did work, plus the running one when its own lines measure
        it (a silent step only moves with this pace: counting it would feed its guess back); 1 (the weights' own guess)
        until a minute of weight is done."""
        seconds, weight = self.spent, self.spent_weight
        if self.plan and self.plan[self.index] in MEASURED and self.inside > 0:
            seconds += time.monotonic() - self.step_started
            weight += self.weight(self.index) * self.inside
        return seconds / weight if weight >= 60 and seconds > 0 else 1.0

    def figures(self) -> tuple:
        """(fraction done, seconds left) of the work this build does."""
        weights = [self.weight(i) for i in range(len(self.plan))]
        done = sum(weights[:self.index]) + weights[self.index] * self.inside
        total = max(1.0, sum(weights))
        return min(1.0, done / total), int((total - done) * self.pace())

    def report(self):
        if not self.plan:
            return
        value, left = self.figures()
        minute = (left + 59) // 60
        if value - self.shown[0] < 0.002 and minute == self.shown[1] and value < 1:
            return
        value = max(value, self.shown[0])   # a step found to have had nothing to do leaves the total: never go back
        self.shown = (value, minute)
        if self.prepare:
            out(f"progress {value:.3f} {tr('t_' + self.plan[self.index])} ({tr('left', minute)})")
        else:
            out(f"@@PROGRESS {value:.3f} {left}")


PROGRESS = Progress()


def run(cmd, cwd=None, env=None, problems=None) -> None:
    """Runs a command, its output going to the log line by line; a non-zero exit fails the step. problems: a list that
    receives the (code, detail) of each "@@PROBLEM <code> <detail>" line the command prints."""
    env = dict(env or os.environ, PYTHONUTF8="1", PYTHONIOENCODING="utf-8", WSL_UTF8="1")
    p = subprocess.Popen([str(c) for c in cmd], cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL)
    for raw in iter(p.stdout.readline, b""):
        line = raw.decode("utf-8", "replace").rstrip("\r\n").replace("\x00", "")
        if line.startswith("@@PROBLEM ") and problems is not None:
            problems.append(tuple((line.split(" ", 2) + [""])[1:3]))
        elif line.strip():
            out(line)
            PROGRESS.line(line)
    if p.wait():
        raise StepFailed(tr("exit", p.returncode))


def capture(cmd, env=None) -> subprocess.CompletedProcess:
    env = dict(env or os.environ, WSL_UTF8="1")
    return subprocess.run([str(c) for c in cmd], capture_output=True, env=env, stdin=subprocess.DEVNULL)


def wsl_path(p: Path) -> str:
    s = str(p.resolve()).replace("\\", "/")
    return f"/mnt/{s[0].lower()}{s[2:]}" if len(s) > 1 and s[1] == ":" else s


def msys_path(p: Path) -> str:
    s = str(p.resolve()).replace("\\", "/")
    return f"/{s[0].lower()}{s[2:]}" if len(s) > 1 and s[1] == ":" else s


def sh_quote(s: str) -> str:
    return "'" + s.replace("'", "'\\''") + "'"


def distro(args) -> str:
    return args.wsl_distro or DISTRO


def wsl(args, script: str, root: bool = False) -> None:
    run(["wsl", "-d", distro(args)] + (["-u", "root"] if root else []) + ["-e", "bash", "-lc", script])


def msys_root(args) -> Path:
    return Path(args.msys2 or os.environ.get("ACR_MSYS2", "") or "C:/msys64")


def msys(args, script: str) -> None:
    env = dict(os.environ, MSYSTEM="UCRT64", CHERE_INVOKING="1")
    run([msys_root(args) / "usr/bin/bash.exe", "-lc", script], cwd=str(ROOT), env=env)


def tool_python() -> str:
    """The Python with cryptography, lz4 and numpy: this one on Windows (the launcher's embedded Python or MSYS2's),
    the venv on Linux."""
    if not WIN and VENV_PY.is_file():
        return str(VENV_PY)
    return sys.executable


def mem_gib() -> float:
    try:
        if WIN:
            import ctypes

            class MS(ctypes.Structure):
                _fields_ = [("len", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                            ("avail", ctypes.c_ulonglong), ("a", ctypes.c_ulonglong), ("b", ctypes.c_ulonglong),
                            ("c", ctypes.c_ulonglong), ("d", ctypes.c_ulonglong), ("e", ctypes.c_ulonglong)]
            m = MS()
            m.len = ctypes.sizeof(MS)
            ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
            return m.total / GIB
        return os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES") / GIB
    except Exception:
        return 0.0


# ---- steps

def extracted() -> bool:
    """The game code and data a first build extracted from the dump, which an update builds from (--update)."""
    return (ROOT / "build/v130/exefs/main").is_file() and (ROOT / "build/v130/romfs.bin").is_file()


def s_check(args) -> None:
    if args.update:
        if not extracted():
            raise StepFailed(tr("no_extracted", ROOT / "build/v130"))
        out(tr("update_from", ROOT / "build/v130"))
    else:
        check_dump(args)
    if args.windows and not WIN:
        raise StepFailed(tr("no_win_linux"))
    if args.send and not args.deck:
        raise StepFailed(tr("send_deck"))
    # Space: the RomFS merge holds the NCAs (~11 GB) and the image (10.9 GB) at once; each build ~6-8 GB.
    need = (23 if not (ROOT / "build/v130/romfs.bin").is_file() else 0) + 7 * args.deck + 9 * args.windows
    free = shutil.disk_usage(ROOT).free / 1e9
    out(f"{ROOT}: {free:.0f} GB")
    if free < need:
        raise StepFailed(tr("disk", ROOT.anchor or ROOT, free, need))
    ram = mem_gib()
    if ram and ram < 7.5:
        out(tr("ram", ram))


def check_dump(args) -> None:
    g = args.game_dir
    if not g.is_dir():
        raise StepFailed(tr("no_dir", g))
    nsps = sorted(g.rglob("*.nsp"))
    out(f"{g}: {len(nsps)} NSP; prod.keys {'ok' if args.keys.is_file() else '--'}")
    if not args.keys.is_file():
        raise StepFailed(tr("missing", "prod.keys"))
    if not nsps:
        raise StepFailed(tr("no_nsp"))
    # The inventory identifies each package by what its NCAs say (title id and version), whatever the file is called;
    # it needs cryptography: on Linux only once the venv exists, and until then the names are the hint.
    if WIN or VENV_PY.is_file():
        found = []
        try:
            run([tool_python(), ROOT / "tools/extract.py", "inventory", "--game-dir", g, "--keys", args.keys],
                problems=found)
        except StepFailed:
            if found and "p_" + found[0][0] in TEXT:
                raise StepFailed(tr("p_" + found[0][0], found[0][1]))
            raise
    else:
        miss = [tr("base")] * (not any(BASE_ID in p.name.upper() for p in nsps)) + \
               [tr("update")] * (not any(UPDATE_ID in p.name.upper() for p in nsps))
        if miss:
            raise StepFailed(tr("missing", ", ".join(miss)))


def wsl_ready() -> bool:
    try:
        return capture(["wsl", "--status"]).returncode == 0
    except OSError:
        return False


def wsl_distros() -> list:
    p = capture(["wsl", "-l", "-q"])
    text = p.stdout.decode("utf-8", "replace").replace("\x00", "")
    return [l.strip() for l in text.splitlines() if l.strip()]


def tools_wsl(args) -> None:
    if not wsl_ready():
        out("@@NEED wsl")
        raise StepFailed(tr("need_wsl"))
    d = distro(args)
    if d not in wsl_distros():
        if args.wsl_distro and args.wsl_distro != DISTRO:
            raise StepFailed(tr("no_distro", d))
        out(tr("wsl_install", d))
        loc = Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "AgeOfCalamityRecomp" / "wsl"
        loc.mkdir(parents=True, exist_ok=True)
        try:
            run(["wsl", "--install", "archlinux", "--name", d, "--location", loc, "--no-launch"])
        except StepFailed:   # --name and --location need WSL 2.4 or newer (Windows 10 may ship an older one)
            raise StepFailed(tr("wsl_old"))
    # The managed distro runs as root (the Arch image's default user); a distro of the user's own runs as its user.
    wsl(args, f"cd {sh_quote(wsl_path(ROOT))} && bash tools/setup_wsl.sh --minimal", root=d == DISTRO)


def download(url: str, sha: str, dst: Path) -> None:
    if dst.is_file() and hashlib.sha256(dst.read_bytes()).hexdigest() == sha:
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    part = dst.with_suffix(".part")
    with urllib.request.urlopen(url, timeout=60) as r, open(part, "wb") as f:
        total, got, mark = int(r.headers.get("Content-Length") or 0), 0, 0
        h = hashlib.sha256()
        while chunk := r.read(1 << 20):
            f.write(chunk)
            h.update(chunk)
            got += len(chunk)
            if total and got * 10 // total > mark:
                mark = got * 10 // total
                out(f"  {got / 1e6:.0f} / {total / 1e6:.0f} MB")
    if h.hexdigest() != sha:
        part.unlink(missing_ok=True)
        raise StepFailed(tr("msys_hash", dst.name))
    part.replace(dst)


def tools_msys2(args) -> None:
    root = msys_root(args)
    bash = root / "usr/bin/bash.exe"
    if not bash.is_file():
        url, sha = MSYS2_SFX
        out(tr("msys_get", 43))
        sfx = ROOT / "build" / "dl" / url.rsplit("/", 1)[1]
        download(url, sha, sfx)
        # the self-extracting archive makes <parent>/msys64
        run([sfx, "-y", f"-o{root.parent}"])
        if root.name != "msys64":
            (root.parent / "msys64").rename(root)
        sfx.unlink(missing_ok=True)
        msys(args, "true")   # first start: makes the home folder and the pacman keyring
        # A fresh base updates its core first and MSYS2 closes every one of its processes, this shell included (exit
        # code 1): that first round may "fail"; the second updates the rest and must succeed.
        try:
            msys(args, "pacman -Syuu --noconfirm --disable-download-timeout")
        except StepFailed:
            out("MSYS2: core updated, second round")
        msys(args, "pacman -Syuu --noconfirm --disable-download-timeout")
    pkgs = [f"mingw-w64-ucrt-x86_64-{p}" for p in MSYS2_PKGS]
    q = capture([bash, "-lc", "pacman -Q " + " ".join(pkgs)], env=dict(os.environ, MSYSTEM="UCRT64"))
    if q.returncode != 0:
        out(tr("msys_pkgs"))
        msys(args, "pacman -S --needed --noconfirm --disable-download-timeout " + " ".join(pkgs))


def steamos_readonly() -> bool:
    """SteamOS (Steam Deck, Steam Machine) with its read-only system and no compiler: pacman cannot install there."""
    try:
        text = Path("/etc/os-release").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    ids = {l.split("=", 1)[1].strip().strip('"') for l in text.splitlines() if l.startswith("ID=")}
    return "steamos" in ids and not (shutil.which("clang") and shutil.which("ninja"))


def s_tools(args) -> str:
    if WIN:
        if args.deck:
            tools_wsl(args)
        if args.windows:
            tools_msys2(args)
    else:
        if steamos_readonly():
            raise StepFailed(tr("steamos"))
        run(["bash", ROOT / "tools/setup_wsl.sh", "--minimal"], cwd=str(ROOT))
    out(tr("tools_ok"))
    return "ok"


def s_extract(args) -> str:
    # An ExeFS on disk counts only when its build IDs are the 1.3.0 ones (tools/data/known-builds.json): an earlier
    # attempt with another update leaves one that would be skipped here and fail later, at the code generator.
    exefs = ROOT / "build/v130/exefs"
    inspect = [tool_python(), ROOT / "tools/inspect_dump.py", "--exefs", exefs]
    if (exefs / "main").is_file():
        if capture(inspect).returncode == 0:
            return "skip"
        out(tr("stale_exefs"))
    if args.update:
        raise StepFailed(tr("no_extracted", ROOT / "build/v130"))
    if exefs.is_dir():
        shutil.rmtree(exefs)
    run([tool_python(), ROOT / "tools/extract.py", "exefs", "--game-dir", args.game_dir, "--keys", args.keys])
    try:
        run(inspect)
    except StepFailed:
        shutil.rmtree(exefs, ignore_errors=True)
        raise StepFailed(tr("wrong_build"))
    return "ok"


def romfs_size() -> int:
    """Bytes of the merged 1.3.0 RomFS image (tools/data/known-builds.json); 0 when the table has none."""
    try:
        known = json.loads((ROOT / "tools/data/known-builds.json").read_text(encoding="utf-8"))
        return int(known["builds"]["1.3.0"].get("romfs_image_size", 0))
    except (OSError, ValueError, KeyError):
        return 0


def s_romfs(args) -> str:
    rom, want = ROOT / "build/v130/romfs.bin", romfs_size()
    if rom.is_file():
        if not want or rom.stat().st_size == want:
            return "skip"
        if args.update:
            raise StepFailed(tr("no_extracted", ROOT / "build/v130"))
        out(tr("stale_romfs", rom.stat().st_size))   # merged with another update by an earlier attempt
        rom.unlink()
        rom.with_name("romfs.bin.sha256").unlink(missing_ok=True)
    if args.update:
        raise StepFailed(tr("no_extracted", ROOT / "build/v130"))
    run([tool_python(), ROOT / "tools/merge_romfs.py", "--game-dir", args.game_dir, "--keys", args.keys])
    got = rom.stat().st_size if rom.is_file() else 0
    if want and got != want:
        rom.unlink(missing_ok=True)
        raise StepFailed(tr("romfs_size", got, want))
    return "ok"


def s_dlc(args) -> str:
    # finds the DLC NCAs by content, checks their hash trees and skips the images already extracted
    if args.update:
        out(tr("dlc_kept"))
        return "skip"
    run([tool_python(), ROOT / "tools/extract_dlc.py", "--game-dir", args.game_dir, "--keys", args.keys])
    return "ok"


def link_dlc(dist_romfs: Path) -> None:
    """The extracted DLC images into a game folder's romfs/ (hard links on the same disk)."""
    for src in sorted((ROOT / "build/v130").glob("dlc-*.bin")):
        dst = dist_romfs / src.name
        if dst.exists() and dst.stat().st_size == src.stat().st_size and dst.stat().st_mtime >= src.stat().st_mtime:
            continue
        dst.unlink(missing_ok=True)
        try:
            os.link(src, dst)
        except OSError:
            shutil.copy2(src, dst)
        out(f"  {src.name}")


def s_deck(args) -> str:
    extra = " --with-romfs" if args.with_romfs else ""
    if WIN:
        wsl(args, f"cd {sh_quote(wsl_path(ROOT))} && . ~/.venvs/AgeOfCalamityRecomp/bin/activate && "
                  f"python3 -u tools/build.py --work ~/AgeOfCalamityRecomp-build gen guest dist{extra}", root=distro(args) == DISTRO)
    else:
        run([tool_python(), "-u", ROOT / "tools/build.py", "--work", Path.home() / "AgeOfCalamityRecomp-build", "gen", "guest", "dist"]
            + (["--with-romfs"] if args.with_romfs else []), cwd=str(ROOT))
    return "ok"


def s_wingen(args) -> str:
    gen = args.win_work / "generated"
    if not args.deck:   # no WSL build: generate the C here (the generator runs on Windows too)
        run([tool_python(), "-u", ROOT / "tools/build.py", "--work", args.win_work, "gen"], cwd=str(ROOT))
        return "ok"
    # the Deck step generated it in WSL: copy only the files that changed
    w = wsl_path(gen)
    wsl(args, "cd ~/AgeOfCalamityRecomp-build/generated && n=0 && while IFS= read -r -d '' f; do d=" + sh_quote(w) + "\"/$f\"; "
              "if ! cmp -s \"$f\" \"$d\"; then mkdir -p \"$(dirname \"$d\")\"; cp \"$f\" \"$d\"; n=$((n+1)); fi; "
              "done < <(find . -type f -print0) && echo \"$n generated files copied\"", root=distro(args) == DISTRO)
    return "ok"


def s_winxlate(args) -> str:
    host = args.win_work / "host"
    host.mkdir(parents=True, exist_ok=True)
    pre = ROOT / "prebuilt/xlate/win-x64"
    if (pre / "xlate.exe").is_file():
        for f in ("xlate.exe", "xlate.id"):
            if not (host / f).is_file() or (host / f).read_bytes() != (pre / f).read_bytes():
                shutil.copy2(pre / f, host / f)
        return "ok"
    # developer tree: published from the WSL build of the translator (dotnet in WSL)
    if not args.deck and not wsl_ready():
        raise StepFailed(tr("no_xlate"))
    w = wsl_path(host)
    wsl(args, f"cd {sh_quote(wsl_path(ROOT))} && . ~/.venvs/AgeOfCalamityRecomp/bin/activate && "
              f"( [ -x ~/AgeOfCalamityRecomp-build/xlate/xlate ] || python3 tools/build_xlate.py --out ~/AgeOfCalamityRecomp-build/xlate "
              f"--sysroot ~/AgeOfCalamityRecomp-build/toolchain/sniper ) && "
              f"if [ -f '{w}/xlate.exe' ] && cmp -s ~/AgeOfCalamityRecomp-build/xlate/xlate.id '{w}/xlate.id'; then "
              f"echo 'Windows translator up to date'; else B=~/AgeOfCalamityRecomp-build/xlate && rm -rf /tmp/AgeOfCalamityRecomp-xlwin && "
              f"dotnet publish \"$B/xlate/xlate.csproj\" -c Release -r win-x64 --self-contained -p:PublishAot=false "
              f"-p:PublishSingleFile=true -p:InvariantGlobalization=true -p:RyuSrc=\"$B/ryujinx\" -o /tmp/AgeOfCalamityRecomp-xlwin "
              f"| tail -2 && cp /tmp/AgeOfCalamityRecomp-xlwin/xlate.exe '{w}/' && cp \"$B/xlate.id\" '{w}/'; fi")
    return "ok"


def s_winbuild(args) -> str:
    msys(args, f"python3 -u tools/build.py --work {sh_quote(msys_path(args.win_work))} guest host")
    return "ok"


def s_windist(args) -> str:
    src, dist = args.win_work / "host", ROOT / "dist/windows"
    (dist / "exefs").mkdir(parents=True, exist_ok=True)
    (dist / "romfs").mkdir(exist_ok=True)

    def copy(a: Path, b: Path) -> None:
        if not a.is_file():
            raise StepFailed(tr("missing_file", a))
        if b.exists() and b.stat().st_size == a.stat().st_size and b.stat().st_mtime >= a.stat().st_mtime:
            return
        shutil.copy2(a, b)

    for f in ("AgeOfCalamityRecomp.exe", "xlate.exe", "xlate.id", "romfs-index.bin"):
        copy(src / f, dist / f)
    # the DLLs AgeOfCalamityRecomp.exe imports from MSYS2 (SDL3, libopus and what they pull in)
    # from "/": ldd resolves like the loader, current folder included, and a shell that keeps the caller's folder
    # (CHERE_INVOKING) would find the launcher's own SDL3.dll there instead of MSYS2's and leave the game without one
    p = capture([msys_root(args) / "usr/bin/bash.exe", "-lc", f"cd / && ldd {sh_quote(msys_path(dist / 'AgeOfCalamityRecomp.exe'))}"],
                env=dict(os.environ, MSYSTEM="UCRT64"))
    lines = p.stdout.decode("utf-8", "replace").splitlines()
    dlls = [l.split("=>")[1].split("(")[0].strip() for l in lines if "/ucrt64/" in l]
    for d in dlls:
        win = msys_root(args) / d.lstrip("/")
        copy(win, dist / win.name)
        out(f"  {win.name}")
    for f in ("main", "subsdk0", "sdk", "main.npdm"):
        copy(ROOT / "build/v130/exefs" / f, dist / "exefs" / f)
    rom, dst = ROOT / "build/v130/romfs.bin", dist / "romfs/romfs.bin"
    if dst.exists() and dst.stat().st_size != rom.stat().st_size:   # the image of an earlier attempt with another update
        dst.unlink()
    if not dst.exists():
        try:
            os.link(rom, dst)   # same disk: no second 10.9 GiB
            out("romfs.bin: hard link")
        except OSError:
            out("romfs.bin: copy (10.9 GiB)")
            shutil.copy2(rom, dst)
    link_dlc(dist / "romfs")
    # monitor mode (captures of hits, F8 marks, frame dumps on a dark scene; runtime/diag.c), always the current one
    shutil.copy2(ROOT / "tools/windows/monitor.bat", dist / "monitor.bat")
    ini = dist / "settings.ini"
    if not ini.exists():
        ini.write_text("# Age of Calamity Recomp settings. The menu (F1) writes here.\n", encoding="utf-8")
    out(tr("folder_ok", dist))
    return "ok"


# ---- Xbox and PlayStation button prompts (tools/make_prompts.py, runtime/prompts.c): made from the player's own romfs.bin, next to
# romfs-index.bin of each game folder. A failure only warns: the game then shows the Switch prompts.
def s_prompts(args) -> str:
    folders = [ROOT / "dist" / d for d, on in (("windows", args.windows), ("steamdeck", args.deck)) if on]
    folders = [f for f in folders if f.is_dir()]
    if not folders:
        return "skip"
    for f in folders:
        p = capture([tool_python(), ROOT / "tools/make_prompts.py", "--image", ROOT / "build/v130/romfs.bin", "--out", f])
        text = (p.stdout + p.stderr).decode("utf-8", "replace").strip()
        if p.returncode:
            out(tr("prompts_failed", text.splitlines()[-1] if text else p.returncode))
            return "skip"
        out(text)
    return "ok"


# ---- NVIDIA DLSS (runtime/vk_dlaa.c): NGX loads nvngx_dlss.dll from the folder of AgeOfCalamityRecomp.exe. The package
# does not ship it: NVIDIA's own copy comes from the DLSS SDK repository (NVIDIA RTX SDK license) at a pinned tag and
# hash, as TotK's installer gets it.
DLSS_DLL = "nvngx_dlss.dll"
DLSS_VERSION = "310.9.1"
DLSS_URL = f"https://raw.githubusercontent.com/NVIDIA/DLSS/v{DLSS_VERSION}/lib/Windows_x86_64/rel/{DLSS_DLL}"
DLSS_SHA256 = "3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983"
DLSS_LICENSE = "https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt"


def gpu_names() -> list:
    """The display adapters Windows knows (Win32_VideoController), empty when the query fails."""
    p = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command",
                        "(Get-CimInstance Win32_VideoController).Name"],
                       capture_output=True, creationflags=0x08000000)
    return [l.strip() for l in p.stdout.decode("utf-8", "replace").splitlines() if l.strip()]


def s_dlss(args) -> str:
    gpus = gpu_names()
    if not any("NVIDIA" in g.upper() and "RTX" in g.upper() for g in gpus):
        out(tr("dlss_no_rtx", ", ".join(gpus) or "?"))
        return "skip"
    dst = ROOT / "dist/windows" / DLSS_DLL
    if dst.is_file() and hashlib.sha256(dst.read_bytes()).hexdigest() != DLSS_SHA256:
        out(tr("dlss_kept"))   # the player put another version there on purpose
        return "ok"
    out(tr("dlss_get", DLSS_VERSION, DLSS_LICENSE))
    try:
        download(DLSS_URL, DLSS_SHA256, dst)
    except Exception as e:   # urllib's errors, a cut connection (http.client, not OSError), a bad hash
        out(tr("dlss_failed", str(e)[:300]))
        return "skip"
    out(f"  {DLSS_DLL} {DLSS_VERSION}")
    return "ok"


# ---- shaders (tools/prepare_shaders.py): every game shader translated and built into the GPU driver's cache before
# the first fight, as TotK's setup prepares its shaders. Skipped when shader-prepared.bin is newer than the translator
# id (the translations are keyed by it, so a rebuild of the game code keeps them) and of PREPARED_VERSION, the
# runtime's PRP_VERSION (runtime/gfx_shader.c; 2: the refined variants too). Over an older one only what is missing is
# translated.
PREPARED_VERSION = 2


def prepared_version(path: Path) -> int:
    with open(path, "rb") as f:
        head = f.read(12)   # "ACSPRP", 2 bytes, u32 version
    return int.from_bytes(head[8:12], "little") if len(head) == 12 and head[:6] == b"ACSPRP" else 0


def s_shaders(args) -> str:
    dist = ROOT / "dist/windows"
    done, xid = dist / "shader-prepared.bin", dist / "xlate.id"
    if (done.is_file() and xid.is_file() and done.stat().st_mtime >= xid.stat().st_mtime
            and prepared_version(done) >= PREPARED_VERSION):
        out(tr("shaders_kept"))
        return "skip"
    msys(args, f"python3 -u tools/prepare_shaders.py {sh_quote(msys_path(dist))}")
    return "ok"


def s_send(args) -> str:
    bash = Path("C:/Program Files/Git/bin/bash.exe") if WIN else Path(shutil.which("bash") or "/bin/bash")
    if not bash.is_file():
        raise StepFailed(tr("no_bash"))
    env = dict(os.environ)
    if args.deck_host:
        env["ACR_DECK"] = args.deck_host
    run([bash, ROOT / "tools/deck/deploy.sh", "--no-build"], cwd=str(ROOT), env=env)
    return "ok"


def main() -> int:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")   # the launcher reads UTF-8
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game-dir", type=Path, default=ROOT / "Game Files", help="folder with the NSPs and prod.keys")
    ap.add_argument("--keys", type=Path, help="key file (default: prod.keys in the game folder)")
    ap.add_argument("--update", action="store_true",
                    help="update an install from the game data its first build extracted (build/v130): no NSPs read")
    ap.add_argument("--deck", action="store_true", help="Steam Deck / Linux folder (dist/steamdeck)")
    ap.add_argument("--windows", action="store_true", help="Windows version (dist/windows)")
    ap.add_argument("--with-romfs", action="store_true", help="Deck: also put romfs.bin in the folder (first install)")
    ap.add_argument("--send", action="store_true", help="send to the Deck over SSH at the end")
    ap.add_argument("--deck-host", default="", help="user@address of the Deck (required for send)")
    ap.add_argument("--win-work", type=Path, default=None, help="work folder of the Windows build")
    ap.add_argument("--wsl-distro", default=os.environ.get("ACR_WSL_DISTRO", ""),
                    help=f"WSL distro (default: {DISTRO}, made by the tools step)")
    ap.add_argument("--msys2", default="", help="MSYS2 folder (default: C:/msys64, installed there if missing)")
    ap.add_argument("--lang", default="en", help=argparse.SUPPRESS)   # kept so older launchers still work; English only
    ap.add_argument("--check-only", action="store_true", help="only the check step")
    ap.add_argument("--prepare", action="store_true",
                    help="setup for a launcher: build for this PC (Windows or Linux), print 'progress <0-1> <text>' lines")
    args = ap.parse_args()
    args.game_dir = args.game_dir.expanduser()
    args.keys = (args.keys or args.game_dir / "prod.keys").expanduser()
    if args.win_work is None:
        args.win_work = Path("C:/AgeOfCalamityRecomp-win") if Path("C:/AgeOfCalamityRecomp-win/host").is_dir() else ROOT / "build/win"
    if args.prepare:   # the build for this PC: Windows folder on Windows, Deck/Linux folder on Linux
        args.windows, args.deck = WIN, not WIN
    if not (args.deck or args.windows):
        ap.error("--deck, --windows or both")
    plan = [("check", s_check), ("tools", s_tools), ("extract", s_extract), ("romfs", s_romfs), ("dlc", s_dlc)]
    if args.deck:
        plan.append(("deck", s_deck))
    if args.windows:
        plan += [("wingen", s_wingen), ("winxlate", s_winxlate), ("winbuild", s_winbuild), ("windist", s_windist)]
    if args.deck or args.windows:
        plan.append(("prompts", s_prompts))
    if args.windows:
        if WIN:
            plan += [("dlss", s_dlss), ("shaders", s_shaders)]
    if args.send:
        plan.append(("send", s_send))
    if args.check_only:
        plan = plan[:1]
    global LOG
    logs = ROOT / "build" / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    for old in sorted(logs.glob("forge-*.log"))[:-9]:   # keep the last ten
        old.unlink(missing_ok=True)
    LOG = open(logs / time.strftime("forge-%Y%m%d-%H%M%S.log"), "w", encoding="utf-8")
    out(f"AgeOfCalamityRecomp {VERSION}, {' '.join(sys.argv[1:])}")
    for sid, _ in plan:
        out(f"@@PLAN {sid} {tr('t_' + sid)}")
    PROGRESS.start(plan, args.prepare)
    for n, (sid, fn) in enumerate(plan):
        title = tr("t_" + sid)
        step(sid, "run", title)
        PROGRESS.step(n)
        try:
            r = fn(args)
            PROGRESS.finish(r or "ok")
            step(sid, r or "ok", title)
        except StepFailed as e:
            out(f"ERROR: {e}")
            step(sid, "fail", str(e))
            out("@@DONE fail")
            if args.prepare:
                print(str(e).replace("\n", " "), file=sys.stderr, flush=True)
            return 1
        except Exception as e:   # anything unexpected still ends the protocol cleanly
            out(tr("unexpected", repr(e)))
            step(sid, "fail", repr(e))
            out("@@DONE fail")
            return 1
    if not args.check_only:
        # What was forged and from which version: the launcher shows the build screen again when VERSION changes
        # (an update unpacked over the folder) and the play screen once the folder for this PC is there.
        state = ROOT / "build" / "forge-state.json"
        old = json.loads(state.read_text(encoding="utf-8")) if state.is_file() else {}
        old.update({"version": VERSION, "time": time.strftime("%Y-%m-%d %H:%M:%S")})
        old["deck"] = old.get("deck", False) or args.deck
        old["windows"] = old.get("windows", False) or args.windows
        state.write_text(json.dumps(old, indent=1) + "\n", encoding="utf-8")
    if args.prepare:
        out("progress 1.00 " + tr("folder_ok", ROOT / "dist"))
    else:
        out("@@PROGRESS 1.000")
    out("@@DONE ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
