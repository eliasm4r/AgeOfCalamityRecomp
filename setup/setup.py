"""The setup and launcher of AgeOfCalamityRecomp: one plain tkinter window, for Windows and Linux.

Build view: the player picks the folder with their own dump and the platforms, and the setup runs tools/make_build.py,
reading its protocol ("@@PLAN <id> <title>", "@@STEP <id> <run|ok|fail|skip> <text>", "@@NEED wsl", "@@DONE ok|fail";
every other line goes to the details log). It shows while the game for this PC is not built, after an update (VERSION
differs from build/forge-state.json) or on "Rebuild...".

Game view: once dist/windows (Windows) or dist/steamdeck (Linux) holds the game, the main settings of its settings.ini
(the keys runtime/cfg.c reads, edited in place the way cfg_save does), Play, Report a problem, the game folder and
Rebuild.

Started by "AgeOfCalamityRecomp Setup.exe" in the Windows package (python/pythonw.exe renamed, see sitecustomize.py), by
setup.sh on Linux, or with "python setup/setup.py". Its own settings live in setup.ini at the top of the folder.
"""
import ctypes
import os
import queue
import signal
import subprocess
import sys
import threading
import time
import tkinter
import webbrowser
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

sys.path.insert(0, str(Path(__file__).resolve().parent))
from texts import TEXTS as T  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
WIN = os.name == "nt"
BASE_ID, UPDATE_ID, DLC_PREFIX = "01002B00111A2000", "01002B00111A2800", "01002B00111A3"
ISSUES_URL = "https://github.com/eliasm4r/AgeOfCalamityRecomp/issues/new?template=problem.yml"
WIDTH = 560


def use_tk():
    tcl = ROOT / "python" / "tcl"
    if WIN and tcl.is_dir():
        os.environ["TCL_LIBRARY"] = str(tcl / "tcl8.6")
        os.environ["TK_LIBRARY"] = str(tcl / "tk8.6")


def read_text(path):
    try:
        return path.read_text(encoding="utf-8-sig")
    except OSError:
        return ""


def package_version():
    return read_text(ROOT / "VERSION").strip()


def built_version():
    import json
    try:
        return json.loads(read_text(ROOT / "build" / "forge-state.json") or "{}").get("version", "")
    except ValueError:
        return ""


def game_dir():
    return ROOT / "dist" / ("windows" if WIN else "steamdeck")


def game_ready():
    return (game_dir() / ("AgeOfCalamityRecomp.exe" if WIN else "AgeOfCalamityRecomp")).is_file()


def python_exe():
    for p in (ROOT / "python" / "python.exe", Path("C:/msys64/ucrt64/bin/python3.exe")):
        if WIN and p.is_file():
            return str(p)
    exe = Path(sys.executable)
    if WIN and exe.name.lower() not in ("python.exe", "pythonw.exe"):
        return str(exe.with_name("python.exe"))
    if WIN and exe.name.lower() == "pythonw.exe":
        return str(exe.with_name("python.exe"))
    return sys.executable


def open_path(path):
    if WIN:
        os.startfile(str(path))
    else:
        subprocess.Popen(["xdg-open", str(path)], start_new_session=True)


# ---- Windows shortcuts (IShellLinkW through COM, as WondersBeyondRecomp's installer/steps.py makes them)
SHORTCUT_NAME = "AgeOfCalamityRecomp"


def create_shortcut(link, target, folder):
    class Guid(ctypes.Structure):
        _fields_ = [("a", ctypes.c_ulong), ("b", ctypes.c_ushort), ("c", ctypes.c_ushort), ("d", ctypes.c_ubyte * 8)]

    ole32 = ctypes.OleDLL("ole32")

    def guid(text):
        value = Guid()
        ole32.CLSIDFromString(ctypes.c_wchar_p(text), ctypes.byref(value))
        return value

    def method(pointer, index, *types):
        table = ctypes.cast(pointer, ctypes.POINTER(ctypes.c_void_p))[0]
        address = ctypes.cast(table, ctypes.POINTER(ctypes.c_void_p))[index]
        return ctypes.WINFUNCTYPE(ctypes.HRESULT, ctypes.c_void_p, *types)(address)

    ole32.CoInitialize(None)
    shell_link = ctypes.c_void_p()
    ole32.CoCreateInstance(ctypes.byref(guid("{00021401-0000-0000-C000-000000000046}")), None, 1,
                           ctypes.byref(guid("{000214F9-0000-0000-C000-000000000046}")), ctypes.byref(shell_link))
    try:
        method(shell_link, 20, ctypes.c_wchar_p)(shell_link, str(target))                       # SetPath
        method(shell_link, 9, ctypes.c_wchar_p)(shell_link, str(folder))                        # SetWorkingDirectory
        method(shell_link, 17, ctypes.c_wchar_p, ctypes.c_int)(shell_link, str(target), 0)      # SetIconLocation
        file = ctypes.c_void_p()
        method(shell_link, 0, ctypes.c_void_p, ctypes.c_void_p)(
            shell_link, ctypes.byref(guid("{0000010B-0000-0000-C000-000000000046}")), ctypes.byref(file))
        try:
            method(file, 6, ctypes.c_wchar_p, ctypes.c_int)(file, str(link), 1)                  # IPersistFile::Save
        finally:
            method(file, 2)(file)
    finally:
        method(shell_link, 2)(shell_link)


def shell_folder(identifier):
    buffer = ctypes.create_unicode_buffer(260)
    ctypes.windll.shell32.SHGetFolderPathW(None, identifier, None, 0, buffer)
    return Path(buffer.value)


# ---- the setup's own settings (setup.ini)
class Settings:
    KEYS = ("game_dir", "deck_host", "win_work", "wsl_distro", "deck", "windows", "send", "with_romfs", "shortcut")

    def __init__(self):
        self.path = ROOT / "setup.ini"
        self.values = {"game_dir": "", "deck_host": "deck@steamdeck", "win_work": "", "wsl_distro": "",
                       "deck": "0" if WIN else "1", "windows": "1" if WIN else "0", "send": "0", "with_romfs": "1",
                       "shortcut": "1"}
        for line in read_text(self.path).splitlines():
            key, sep, value = line.partition("=")
            if sep and key.strip() in self.KEYS:
                self.values[key.strip()] = value.strip()
        if not self.values["game_dir"] and (ROOT / "Game Files").is_dir():
            self.values["game_dir"] = str(ROOT / "Game Files")

    def save(self):
        try:
            self.path.write_text("".join(f"{k}={self.values[k]}\n" for k in self.KEYS), encoding="utf-8")
        except OSError:
            pass


# ---- the dump folder at a glance (file names only; the build's check step identifies the packages by content)
def scan_dump(folder):
    found = {"exists": False, "nsp": 0, "base": False, "update": False, "dlc": 0, "keys": False}
    path = Path(folder) if folder else None
    if not path or not path.is_dir():
        return found
    found["exists"] = True
    found["keys"] = (path / "prod.keys").is_file()
    seen = 0
    for top, dirs, files in os.walk(path):
        for name in files:
            seen += 1
            upper = name.upper()
            if not upper.endswith(".NSP"):
                continue
            found["nsp"] += 1
            if BASE_ID in upper:
                found["base"] = True
            elif UPDATE_ID in upper:
                found["update"] = True
            elif DLC_PREFIX in upper:
                found["dlc"] += 1
        if seen > 20000:
            break
    return found


# ---- settings.ini, edited in place (the rules of runtime/cfg.c cfg_load and cfg_save)
class GameIni:
    def __init__(self, path):
        self.path = path
        raw = path.read_bytes() if path.is_file() else b""
        self.bom = raw.startswith(b"\xef\xbb\xbf")
        text = raw[3 if self.bom else 0:].decode("utf-8", "replace")
        self.crlf = "\r\n" in text
        self.lines = text.replace("\r\n", "\n").split("\n")
        if self.lines and self.lines[-1] == "":
            self.lines.pop()

    @staticmethod
    def key_of(line):
        t = line.strip()
        if not t or t[0] in "#;[" or "=" not in t:
            return ""
        k = t.split("=", 1)[0].strip().upper()
        return k[4:] if k.startswith("ACR_") else k

    def find(self, key):
        at = -1
        for i, line in enumerate(self.lines):
            if self.key_of(line) == key:
                at = i
        return at

    def get(self, key):
        i = self.find(key)
        if i < 0:
            return ""
        v = self.lines[i].split("=", 1)[1].strip()
        return v[1:-1] if len(v) >= 2 and v[0] == v[-1] == '"' else v

    def set(self, key, value, section):
        i = self.find(key)
        if i >= 0:
            self.lines[i] = self.lines[i].split("=", 1)[0].rstrip() + " = " + value
            return
        inside, tail = False, -1
        for j, line in enumerate(self.lines):
            t = line.strip()
            if t.startswith("[") and "]" in t:
                inside = t[1:t.index("]")] == section
                if inside:
                    tail = j
                continue
            if inside and t:
                tail = j
        if tail >= 0:
            self.lines.insert(tail + 1, f"{key.lower()} = {value}")
        else:
            while self.lines and not self.lines[-1].strip():
                self.lines.pop()
            if self.lines:
                self.lines.append("")
            self.lines += [f"[{section}]", f"{key.lower()} = {value}"]

    def save(self):
        eol = "\r\n" if self.crlf else "\n"
        data = ("\ufeff" if self.bom else "") + "".join(line + eol for line in self.lines)
        tmp = self.path.with_name(self.path.name + ".tmp")
        tmp.write_text(data, encoding="utf-8", newline="")
        os.replace(tmp, self.path)


# The settings the game view shows: keys, values and defaults as runtime/cfg.c's metas has them; the texts follow
# runtime/ui_text.c. (key, section, default, kind, values, names, label, description, disabled while this key is 1)
CHOICE, NUMBER, TOGGLE = 0, 1, 2
GRAPHICS = [
    ("FULLSCREEN", "display", "0", CHOICE, ["0", "1"], ["Windowed", "Fullscreen"], "Window mode",
     "Fullscreen fills the screen; F11 toggles it while playing.", None),
    ("WINDOW_SIZE", "display", "1920x1080", CHOICE, ["1280x720", "1600x900", "1920x1080", "2560x1440"],
     ["1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440"], "Window size",
     "Size of the game window when not fullscreen.", "FULLSCREEN"),
    ("VK_PRESENT", "display", "mailbox", CHOICE, ["mailbox", "fifo", "immediate"],
     ["Mailbox (recommended)", "VSync", "Immediate"], "Presentation",
     "How finished frames reach the screen. Mailbox: no tearing and little waiting. VSync: waits for the screen. "
     "Immediate: the least waiting, may tear.", None),
    ("OUTPUT_RES", "display", "1920x1080", CHOICE, ["1920x1080", "1280x720"],
     ["1920 x 1080 (the game's)", "1280 x 720 (lighter)"], "Game resolution",
     "The size the picture and interface are drawn in. 1280 x 720 matches the Steam Deck's screen and spares the GPU.",
     None),
    ("OPERATION_MODE", "language", "docked", CHOICE, ["docked", "handheld"], ["TV (sharper)", "Handheld (lighter)"],
     "Internal resolution",
     "TV: the game draws at its docked resolution, sharper and heavier. Handheld: lighter on the GPU and the battery.",
     None),
    ("RENDER_SCALE", "display", "game", CHOICE, ["game", "50", "60", "67", "85", "100"],
     ["Game (75 %)", "50 %", "60 %", "67 %", "85 %", "100 %"], "3D render scale",
     "Size of the 3D scene relative to the game resolution; the game's FSR scales it up. Higher: sharper and heavier.",
     None),
    ("ASPECT", "display", "fit", CHOICE, ["fit", "stretch"], ["Black bars", "Stretch"], "Picture shape",
     "On a screen of another shape (a 16:9 picture on a 21:9 screen): keep the shape with black bars, or stretch it.",
     None),
    ("ANISO", "display", "game", CHOICE, ["game", "2", "4", "8", "16"], ["Game", "2x", "4x", "8x", "16x"],
     "Texture filtering",
     "Ground, roads and walls seen at a shallow angle stay sharp into the distance. Game: as on the Switch.", None),
    ("SHADOW_RES", "display", "game", CHOICE, ["game", "1024", "1536", "2048"], ["Game (changes)", "1024", "1536", "2048"],
     "Shadows",
     "Size of the sun's shadow map. A fixed size keeps shadows steadier and sharper; above 1024 it costs more GPU time.",
     None),
    ("PERF_OVERLAY", "display", "off", CHOICE, ["off", "fps", "full"], ["Off", "FPS", "Full"], "Performance overlay",
     "Frame rate in the corner of the screen. Full adds frame times, CPU, GPU and memory. F10 cycles it.", None),
]
CONTROLS = [
    ("PAD_LAYOUT", "input", "auto", CHOICE, ["auto", "nintendo", "xbox"],
     ["Automatic", "Nintendo (A right)", "Xbox (A bottom)"], "Button layout",
     "Which button is A. Nintendo: the right one, as on a Switch. Xbox: the bottom one. Automatic picks by controller.",
     None),
    ("RUMBLE_STRENGTH", "input", "130", NUMBER, (0, 300, 10), None, "Vibration",
     "How strong the controller vibrates. 0 turns it off.", None),
    ("STICK_DEADZONE", "input", "8", NUMBER, (0, 50, 1), None, "Stick dead zone",
     "How far a stick must move before the game notices. Raise it if the character or the camera drifts.", None),
    ("INVERT_RX", "input", "0", TOGGLE, None, None, "Invert camera (horizontal)",
     "Mirrors the right stick's horizontal axis.", None),
    ("INVERT_RY", "input", "0", TOGGLE, None, None, "Invert camera (vertical)",
     "Mirrors the right stick's vertical axis.", None),
    ("VOLUME", "audio", "100", NUMBER, (0, 100, 5), None, "Volume", "Master volume of the game.", None),
    ("MUTE_BACKGROUND", "audio", "0", TOGGLE, None, None, "Mute in the background",
     "Silences the game while another window is active.", None),
]


# ---- make_build.py, run in the background
class Build:
    def __init__(self, args, events):
        self.events = events
        self.process = None
        self.cancelled = False
        self.args = args
        self.log = ROOT / "build" / "logs" / "setup.log"

    def start(self):
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        cmd = [python_exe(), "-u", str(ROOT / "tools" / "make_build.py")] + self.args
        extra = {"creationflags": 0x08000000} if WIN else {"start_new_session": True}
        env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUTF8="1")
        try:
            self.log.parent.mkdir(parents=True, exist_ok=True)
            log = open(self.log, "w", encoding="utf-8")
        except OSError:
            log = None
        try:
            self.process = subprocess.Popen(cmd, cwd=str(ROOT), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                            stdin=subprocess.DEVNULL, env=env, **extra)
        except OSError as error:
            self.events.put(("log", T["no_python"].format(error=error)))
            self.events.put(("end", "fail"))
            return
        result = ""
        for raw in self.process.stdout:
            line = raw.decode("utf-8", "replace").rstrip("\r\n")
            if log:
                log.write(line + "\n")
                log.flush()
            if line.startswith("@@PLAN "):
                sid, _, title = line[7:].partition(" ")
                self.events.put(("plan", (sid, title or sid)))
            elif line.startswith("@@STEP "):
                parts = line[7:].split(" ", 2)
                self.events.put(("step", (parts[0], parts[1] if len(parts) > 1 else "", parts[2] if len(parts) > 2 else "")))
            elif line.startswith("@@PROGRESS "):
                try:
                    self.events.put(("progress", float(line[11:])))
                except ValueError:
                    pass
            elif line.startswith("@@NEED wsl"):
                self.events.put(("wsl", None))
            elif line.startswith("@@DONE "):
                result = result or line[7:].strip()
            else:
                self.events.put(("log", line))
        self.process.wait()
        if log:
            log.close()
        self.events.put(("end", "cancel" if self.cancelled else (result or "fail")))

    def cancel(self, distro):
        self.cancelled = True
        process = self.process
        if not process or process.poll() is not None:
            return
        if WIN:
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(process.pid)], capture_output=True, creationflags=0x08000000)
            subprocess.run(["wsl", "-d", distro or "AgeOfCalamityRecomp", "-e", "pkill", "-f", "tools/build.py"],
                           capture_output=True, creationflags=0x08000000)
        else:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except OSError:
                pass


class App:
    def __init__(self):
        use_tk()
        self.root = tkinter.Tk()
        self.root.title(T["window"])
        self.root.resizable(False, False)
        self.set_icon()
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.settings = Settings()
        self.events = queue.Queue()
        self.build = None
        self.frame = None
        self.game = None
        self.report = None
        if game_ready() and built_version() == package_version():
            self.show_game()
        else:
            self.show_build()
        self.root.after(100, self.poll)

    def set_icon(self):
        """The game icon on the window and the taskbar. On Windows the sizes the screen's scaling asks for are loaded
        from the .ico and given to the window (Tk would stretch its 16 or 32 pixel one, which looks blurred)."""
        ico, png = ROOT / "assets" / "icon.ico", ROOT / "assets" / "icon.png"
        try:
            if not WIN:
                self.icon = tkinter.PhotoImage(file=str(png))
                self.root.iconphoto(True, self.icon)
                return
            self.root.iconbitmap(str(ico))
            self.root.update_idletasks()
            user32 = ctypes.windll.user32
            user32.LoadImageW.restype = ctypes.c_void_p
            user32.LoadImageW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint, ctypes.c_int, ctypes.c_int,
                                          ctypes.c_uint]
            user32.SendMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
            hwnd = int(self.root.wm_frame(), 16)
            for which, metric in ((1, 11), (0, 49)):   # ICON_BIG: SM_CXICON, ICON_SMALL: SM_CXSMICON
                size = max(user32.GetSystemMetrics(metric), 16) * (2 if which else 1)
                icon = user32.LoadImageW(None, str(ico), 1, size, size, 0x10)   # IMAGE_ICON, LR_LOADFROMFILE
                if icon:
                    user32.SendMessageW(hwnd, 0x80, ctypes.c_void_p(which), ctypes.c_void_p(icon))   # WM_SETICON
        except (tkinter.TclError, AttributeError, OSError, ValueError):
            pass

    def new_frame(self):
        if self.frame:
            self.frame.destroy()
        self.frame = ttk.Frame(self.root, padding=16)
        self.frame.grid(sticky="nsew")
        ttk.Label(self.frame, text=T["title"], font=("Segoe UI", 13, "bold")).grid(row=0, column=0, columnspan=3, sticky="w")
        return self.frame

    # ---- build view
    def show_build(self):
        f = self.new_frame()
        s = self.settings.values
        intro = T["intro"]
        if game_ready() and built_version() != package_version():
            intro += "\n\n" + T["update_note"]
        ttk.Label(f, text=intro, wraplength=WIDTH, justify="left").grid(row=1, column=0, columnspan=3, sticky="w", pady=(6, 14))
        ttk.Label(f, text=T["folder"]).grid(row=2, column=0, columnspan=3, sticky="w")
        self.folder = tkinter.StringVar(value=s["game_dir"])
        self.folder_entry = ttk.Entry(f, textvariable=self.folder, width=70)
        self.folder_entry.grid(row=3, column=0, columnspan=2, sticky="we", pady=(2, 6))
        self.folder_button = ttk.Button(f, text=T["browse"], command=self.choose_folder)
        self.folder_button.grid(row=3, column=2, sticky="e", padx=(8, 0), pady=(2, 6))

        found = ttk.Frame(f)
        found.grid(row=4, column=0, columnspan=3, sticky="w", pady=(0, 12))
        self.found = []
        for i, name in enumerate(("base", "update", "dlc", "keys")):
            mark = ttk.Label(found, width=2)
            mark.grid(row=i // 2, column=(i % 2) * 2, sticky="w")
            text = ttk.Label(found, width=34)
            text.grid(row=i // 2, column=(i % 2) * 2 + 1, sticky="w")
            self.found.append((name, mark, text))

        box = ttk.LabelFrame(f, text=T["targets"], padding=(10, 6))
        box.grid(row=5, column=0, columnspan=3, sticky="we")
        box.columnconfigure(0, weight=1)
        self.windows = tkinter.BooleanVar(value=s["windows"] == "1")
        self.deck = tkinter.BooleanVar(value=s["deck"] == "1")
        self.with_romfs = tkinter.BooleanVar(value=s["with_romfs"] == "1")
        self.send = tkinter.StringVar(value="ssh" if s["send"] == "1" else "copy")
        self.host = tkinter.StringVar(value=s["deck_host"])
        self.inputs = [self.folder_entry, self.folder_button]
        note = {"foreground": "#666", "wraplength": WIDTH - 40, "justify": "left"}
        row = 0
        if WIN:
            w = ttk.Checkbutton(box, text=T["windows"], variable=self.windows, command=self.refresh)
            w.grid(row=row, column=0, sticky="w")
            ttk.Label(box, text=T["windows_note"], **note).grid(row=row + 1, column=0, sticky="w", padx=(22, 0))
            self.shortcut = tkinter.BooleanVar(value=s["shortcut"] == "1")
            self.shortcut_box = ttk.Checkbutton(box, text=T["shortcut"], variable=self.shortcut)
            self.shortcut_box.grid(row=row + 2, column=0, sticky="w", padx=(22, 0), pady=(2, 8))
            self.inputs += [w, self.shortcut_box]
            row += 3
        d = ttk.Checkbutton(box, text=T["deck"] if WIN else T["deck_linux"], variable=self.deck, command=self.refresh)
        d.grid(row=row, column=0, sticky="w")
        ttk.Label(box, text=T["deck_note"] if WIN else T["deck_note_linux"], **note).grid(
            row=row + 1, column=0, sticky="w", padx=(22, 0))
        self.inputs.append(d)
        row += 2

        # Only while the Deck is ticked: how the folder reaches it, and whether it is the first copy
        self.deck_panel = ttk.Frame(box)
        self.deck_panel.grid(row=row, column=0, sticky="we", padx=(22, 0), pady=(8, 0))
        p = self.deck_panel
        ttk.Label(p, text=T["deck_how"]).grid(row=0, column=0, columnspan=3, sticky="w")
        copy = ttk.Radiobutton(p, text=T["deck_copy"], value="copy", variable=self.send, command=self.refresh)
        copy.grid(row=1, column=0, columnspan=3, sticky="w", padx=(12, 0))
        ssh = ttk.Radiobutton(p, text=T["deck_ssh"], value="ssh", variable=self.send, command=self.refresh)
        ssh.grid(row=2, column=0, columnspan=3, sticky="w", padx=(12, 0))
        self.host_label = ttk.Label(p, text=T["host"])
        self.host_label.grid(row=3, column=0, sticky="w", padx=(34, 6))
        self.host_entry = ttk.Entry(p, textvariable=self.host, width=24)
        self.host_entry.grid(row=3, column=1, sticky="w")
        self.ssh_note = ttk.Label(p, text=T["ssh_note"], **note)
        self.ssh_note.grid(row=4, column=0, columnspan=3, sticky="w", padx=(34, 0))
        first = ttk.Checkbutton(p, text=T["with_romfs"], variable=self.with_romfs)
        first.grid(row=5, column=0, columnspan=3, sticky="w", pady=(8, 0))
        ttk.Label(p, text=T["with_romfs_note"], **note).grid(row=6, column=0, columnspan=3, sticky="w", padx=(22, 0))
        self.ssh_widgets = [self.host_label, self.host_entry, self.ssh_note]
        self.inputs += [copy, ssh, first, self.host_entry]

        self.bar = ttk.Progressbar(f, maximum=1000)
        self.bar.grid(row=6, column=0, columnspan=3, sticky="we", pady=(16, 4))
        self.status = tkinter.StringVar()
        self.status_label = ttk.Label(f, textvariable=self.status, wraplength=WIDTH, justify="left")
        self.status_label.grid(row=7, column=0, columnspan=3, sticky="w")

        self.log_frame = ttk.Frame(f)
        self.log = tkinter.Text(self.log_frame, height=14, width=80, wrap="none", font=("Consolas", 9), state="disabled",
                                relief="solid", borderwidth=1)
        scroll = ttk.Scrollbar(self.log_frame, command=self.log.yview)
        self.log.configure(yscrollcommand=scroll.set)
        self.log.grid(row=0, column=0, sticky="nsew")
        scroll.grid(row=0, column=1, sticky="ns")
        self.log_frame.columnconfigure(0, weight=1)

        buttons = ttk.Frame(f)
        buttons.grid(row=9, column=0, columnspan=3, sticky="we", pady=(14, 0))
        buttons.columnconfigure(2, weight=1)
        self.details_button = ttk.Button(buttons, text=T["details"], command=self.toggle_details)
        self.details_button.grid(row=0, column=0, padx=(0, 8))
        self.extra_button = ttk.Button(buttons, text=T["check"], command=lambda: self.start(True))
        self.extra_button.grid(row=0, column=1)
        self.main_button = ttk.Button(buttons, text=T["build"], command=lambda: self.start(False), default="active")
        self.main_button.grid(row=0, column=3, padx=(0, 8))
        self.close_button = ttk.Button(buttons, text=T["close"], command=self.close)
        self.close_button.grid(row=0, column=4)
        f.columnconfigure(0, weight=1)
        self.folder.trace_add("write", lambda *change: self.refresh())
        self.steps = []
        self.refresh()

    def refresh(self):
        found = scan_dump(self.folder.get().strip().strip('"'))
        unnamed = found["nsp"] >= 2 and not (found["base"] or found["update"])
        for name, mark, text in self.found:
            if not found["exists"]:
                mark.configure(text="", foreground="")
                text.configure(text=T["no_folder"] if name == "base" and self.folder.get().strip() else "")
                continue
            if name == "dlc":
                ok, detail = found["dlc"] > 0, T["dlc_found"].format(n=found["dlc"])
            elif name == "keys":
                ok, detail = found["keys"], T["found"] if found["keys"] else T["missing"]
            else:
                ok = found[name] or unnamed
                detail = T["found"] if found[name] else T["unnamed"] if unnamed else T["missing"]
            optional = name == "dlc"
            mark.configure(text="\u2713" if ok else ("\u2013" if optional else "\u2717"),
                           foreground="#1a7f37" if ok else ("#666" if optional else "#c62828"))
            text.configure(text=f"{T[name]}: {detail}")
        self.files_ok = ((found["base"] and found["update"]) or found["nsp"] >= 2) and found["keys"]
        if self.deck.get():
            self.deck_panel.grid()
        else:
            self.deck_panel.grid_remove()
        for w in self.ssh_widgets:
            if self.send.get() == "ssh":
                w.grid()
            else:
                w.grid_remove()

    def choose_folder(self):
        path = filedialog.askdirectory(parent=self.root, title=T["folder_title"], initialdir=self.folder.get() or None)
        if path:
            self.folder.set(str(Path(path)))

    def toggle_details(self):
        if self.log_frame.winfo_ismapped():
            self.log_frame.grid_remove()
            self.details_button.configure(text=T["details"])
        else:
            self.log_frame.grid(row=8, column=0, columnspan=3, sticky="we", pady=(8, 0))
            self.details_button.configure(text=T["hide_details"])
            self.log.see("end")

    def add_log(self, line):
        self.log.configure(state="normal")
        self.log.insert("end", line + "\n")
        if int(self.log.index("end-1c").split(".")[0]) > 4000:
            self.log.delete("1.0", "500.0")
        self.log.configure(state="disabled")
        self.log.see("end")

    def save_choices(self):
        s = self.settings.values
        s["game_dir"] = self.folder.get().strip().strip('"')
        s["windows"] = "1" if self.windows.get() else "0"
        s["deck"] = "1" if self.deck.get() else "0"
        s["with_romfs"] = "1" if self.with_romfs.get() else "0"
        s["send"] = "1" if self.send.get() == "ssh" else "0"
        s["deck_host"] = self.host.get().strip()
        if WIN:
            s["shortcut"] = "1" if self.shortcut.get() else "0"
        self.settings.save()

    def start(self, check_only):
        if not self.files_ok:
            return messagebox.showwarning(T["window"], T["need_files"], parent=self.root)
        windows = WIN and self.windows.get()
        deck = self.deck.get()
        if not (windows or deck):
            return messagebox.showwarning(T["window"], T["need_target"], parent=self.root)
        if deck and self.send.get() == "ssh" and not self.host.get().strip():
            return messagebox.showwarning(T["window"], T["need_host"], parent=self.root)
        self.save_choices()
        s = self.settings.values
        args = ["--game-dir", s["game_dir"]]
        if deck:
            args.append("--deck")
            if self.with_romfs.get():
                args.append("--with-romfs")
            if self.send.get() == "ssh":
                args += ["--send", "--deck-host", s["deck_host"]]
        if windows:
            args.append("--windows")
        if WIN and s["win_work"]:
            args += ["--win-work", s["win_work"]]
        if WIN and s["wsl_distro"]:
            args += ["--wsl-distro", s["wsl_distro"]]
        if check_only:
            args.append("--check-only")
        self.check_only = check_only
        self.targets = (windows, deck)
        self.steps = []
        self.need_wsl = False
        self.started = time.monotonic()
        self.fraction = None   # the build's own "@@PROGRESS" figure, once it prints one
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")
        self.bar.configure(value=0)
        self.status.set(T["starting"])
        self.build = Build(args, self.events)
        for w in self.inputs:
            w.configure(state="disabled")
        self.extra_button.grid_remove()
        self.main_button.configure(state="disabled")
        self.close_button.configure(text=T["cancel"])
        self.build.start()

    def on_build_event(self, kind, value):
        if kind == "plan":
            self.steps.append({"id": value[0], "title": value[1], "state": "pending", "note": ""})
        elif kind == "step":
            sid, state, note = value
            for step in self.steps:
                if step["id"] == sid:
                    step["state"] = state
                    if state == "fail":
                        step["note"] = note
            self.show_progress()
        elif kind == "progress":
            self.fraction = max(self.fraction or 0.0, min(1.0, value))
            self.show_progress()
        elif kind == "log":
            self.add_log(value)
        elif kind == "wsl":
            self.need_wsl = True
        elif kind == "end":
            self.build_ended(value)

    def show_progress(self):
        total = len(self.steps) or 1
        finished = sum(1 for s in self.steps if s["state"] in ("ok", "skip"))
        current = next((s for s in self.steps if s["state"] == "run"), None)
        # The build's own figure follows the files it translates and compiles; without it, half a step per running step.
        fraction = self.fraction if self.fraction is not None else (finished + (0.5 if current else 0)) / total
        self.bar.configure(value=int(1000 * fraction))
        if current:
            n = self.steps.index(current) + 1
            minutes = int(time.monotonic() - self.started) // 60
            self.status.set(f"{int(100 * fraction)} %  ·  " + T["step"].format(n=n, total=total, title=current["title"])
                            + (f"  ({minutes} min)" if minutes else ""))

    def build_ended(self, result):
        self.build = None
        for w in self.inputs:
            w.configure(state="normal")
        self.extra_button.grid()
        self.main_button.configure(state="normal")
        self.close_button.configure(text=T["close"])
        self.refresh()
        if result == "ok":
            self.bar.configure(value=1000)
            if self.check_only:
                return self.status.set(T["checked"])
            if game_ready():
                if WIN and self.targets[0]:
                    self.make_shortcuts()
                return self.show_game()
            self.status.set(T["done_deck"])
            self.main_button.configure(text=T["open_deck"], command=lambda: open_path(ROOT / "dist" / "steamdeck"))
            return
        if result == "cancel":
            self.bar.configure(value=0)
            return self.status.set(T["cancelled"])
        failed = next((s for s in self.steps if s["state"] in ("fail", "run")), None)
        title = failed["title"] if failed else T["build"]
        self.status.set(T["failed"].format(step=title))
        self.main_button.configure(text=T["rebuild"])
        if self.need_wsl:
            return self.offer_wsl()
        messagebox.showerror(T["window"], T["failed_box"].format(step=title, note=(failed or {}).get("note", ""),
                                                                 path=ROOT / "build" / "logs" / "setup.log"), parent=self.root)

    def make_shortcuts(self):
        """The game in the Start Menu, and on the desktop when the box is ticked: shortcuts to the game's own exe, which
        carries the icon (the setup exe is Python's signed pythonw.exe and keeps Python's icon)."""
        exe = game_dir() / "AgeOfCalamityRecomp.exe"
        folders = [shell_folder(0x02)] + ([shell_folder(0x10)] if self.shortcut.get() else [])   # Programs, Desktop
        for folder in folders:
            try:
                create_shortcut(folder / (SHORTCUT_NAME + ".lnk"), exe, game_dir())
            except OSError as error:
                self.add_log(f"shortcut in {folder}: {error}")

    def offer_wsl(self):
        if not messagebox.askyesno(T["window"], T["need_wsl"], parent=self.root):
            return
        done = ctypes.windll.shell32.ShellExecuteW(None, "runas", "wsl.exe", "--install --no-distribution", None, 1)
        self.status.set(T["wsl_started"] if done > 32 else T["wsl_failed"])

    # ---- game view
    def show_game(self):
        f = self.new_frame()
        ttk.Label(f, text=T["version"].format(v=package_version()), foreground="#666").grid(row=1, column=0, columnspan=3,
                                                                                             sticky="w", pady=(2, 12))
        self.ini = GameIni(game_dir() / "settings.ini")
        self.option_vars = []
        self.hint = tkinter.StringVar(value=T["hint"])
        panels = ttk.Frame(f)
        panels.grid(row=2, column=0, columnspan=3, sticky="nsew")
        for column, (title, options) in enumerate(((T["graphics"], GRAPHICS), (T["controls"], CONTROLS))):
            box = ttk.LabelFrame(panels, text=title, padding=(10, 6))
            box.grid(row=0, column=column, sticky="nsew", padx=(0, 10) if column == 0 else 0)
            for row, option in enumerate(options):
                self.option_row(box, row, option)
            if column == 0:
                self.option_label(box, len(options), T["framerate"], T["framerate_desc"])
                ttk.Label(box, text=T["framerate_value"]).grid(row=len(options), column=1, sticky="w", pady=2)
        ttk.Label(f, textvariable=self.hint, wraplength=WIDTH + 200, justify="left", foreground="#555").grid(
            row=3, column=0, columnspan=3, sticky="w", pady=(10, 0))
        self.status = tkinter.StringVar()
        ttk.Label(f, textvariable=self.status, wraplength=WIDTH + 200, justify="left").grid(row=4, column=0, columnspan=3,
                                                                                           sticky="w", pady=(4, 0))
        buttons = ttk.Frame(f)
        buttons.grid(row=5, column=0, columnspan=3, sticky="we", pady=(14, 0))
        buttons.columnconfigure(3, weight=1)
        self.report_button = ttk.Button(buttons, text=T["report"], command=self.make_report)
        self.report_button.grid(row=0, column=0, padx=(0, 8))
        ttk.Button(buttons, text=T["game_folder"], command=lambda: open_path(game_dir())).grid(row=0, column=1, padx=(0, 8))
        ttk.Button(buttons, text=T["build_again"], command=self.show_build).grid(row=0, column=2)
        self.save_button = ttk.Button(buttons, text=T["save"], command=self.save_ini)
        self.save_button.grid(row=0, column=4, padx=(0, 8))
        self.play_button = ttk.Button(buttons, text=T["play"], command=self.play, default="active")
        self.play_button.grid(row=0, column=5)
        self.root.bind("<Return>", lambda event: self.play())
        self.update_dirty()

    def option_label(self, box, row, label, description):
        widget = ttk.Label(box, text=label)
        widget.grid(row=row, column=0, sticky="w", padx=(0, 12), pady=2)
        self.describe(widget, description)
        return widget

    def describe(self, widget, description):
        widget.bind("<Enter>", lambda event: self.hint.set(description))
        widget.bind("<Leave>", lambda event: self.hint.set(T["hint"]))

    def option_row(self, box, row, option):
        key, section, default, kind, values, names, label, description, needs_off = option
        value = self.ini.get(key) or default
        self.option_label(box, row, label, description)
        if kind == CHOICE:
            var = tkinter.StringVar(value=names[values.index(value)] if value in values else value)
            widget = ttk.Combobox(box, textvariable=var, values=names, state="readonly", width=22)
            read = lambda var=var, values=values, names=names: values[names.index(var.get())] if var.get() in names else var.get()
            widget.bind("<<ComboboxSelected>>", lambda event: self.update_dirty())
        elif kind == NUMBER:
            low, high, step = values
            var = tkinter.StringVar(value=value)
            widget = ttk.Spinbox(box, textvariable=var, from_=low, to=high, increment=step, width=8,
                                 command=self.update_dirty)
            widget.bind("<KeyRelease>", lambda event: self.update_dirty())
            read = lambda var=var, low=low, high=high, default=default: (
                str(min(high, max(low, int(var.get())))) if var.get().strip().lstrip("-").isdigit() else default)
        else:
            var = tkinter.BooleanVar(value=value == "1")
            widget = ttk.Checkbutton(box, variable=var, command=self.update_dirty)
            read = lambda var=var: "1" if var.get() else "0"
        widget.grid(row=row, column=1, sticky="w", pady=2)
        self.describe(widget, description)
        self.option_vars.append({"key": key, "section": section, "read": read, "saved": read(), "widget": widget,
                                 "needs_off": needs_off})

    def update_dirty(self):
        values = {o["key"]: o["read"]() for o in self.option_vars}
        for o in self.option_vars:
            if o["needs_off"]:
                o["widget"].configure(state="disabled" if values.get(o["needs_off"]) == "1" else "readonly")
        dirty = any(o["read"]() != o["saved"] for o in self.option_vars)
        self.save_button.configure(state="normal" if dirty else "disabled")
        if dirty:
            self.status.set(T["unsaved"])
        elif self.status.get() == T["unsaved"]:
            self.status.set("")
        return dirty

    def save_ini(self):
        try:
            self.ini = GameIni(game_dir() / "settings.ini")
            for o in self.option_vars:
                value = o["read"]()
                if value != o["saved"]:
                    self.ini.set(o["key"], value, o["section"])
            self.ini.save()
        except OSError:
            self.status.set(T["save_failed"])
            return False
        for o in self.option_vars:
            o["saved"] = o["read"]()
        self.update_dirty()
        self.status.set(T["saved"])
        return True

    def play(self):
        if self.game and self.game.poll() is None:
            return
        if self.update_dirty() and not self.save_ini():
            return
        folder = game_dir()
        try:
            if WIN:
                self.game = subprocess.Popen([str(folder / "AgeOfCalamityRecomp.exe")], cwd=str(folder), creationflags=0x08000000)
            else:
                self.game = subprocess.Popen(["sh", str(folder / "AgeOfCalamityRecomp.sh")], cwd=str(folder), start_new_session=True)
        except OSError as error:
            self.status.set(T["play_failed"].format(error=error))
            return
        self.play_button.configure(text=T["playing"], state="disabled")
        self.status.set("")

    def make_report(self):
        self.report_button.configure(state="disabled")
        self.status.set(T["reporting"])

        def run():
            try:
                code = subprocess.call([python_exe(), str(ROOT / "tools" / "make_report.py")], cwd=str(ROOT),
                                       **({"creationflags": 0x08000000} if WIN else {}))
            except OSError:
                code = 1
            self.events.put(("report", code))

        threading.Thread(target=run, daemon=True).start()

    def on_report(self, code):
        self.report_button.configure(state="normal")
        if code != 0:
            return self.status.set(T["report_failed"])
        self.status.set(T["report_done"])
        open_path(ROOT)
        webbrowser.open(ISSUES_URL)

    # ---- both
    def poll(self):
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == "report":
                    self.on_report(value)
                else:
                    self.on_build_event(kind, value)
        except queue.Empty:
            pass
        if self.build and self.steps:
            self.show_progress()
        if self.game and self.game.poll() is not None and getattr(self, "play_button", None):
            self.game = None
            try:
                self.play_button.configure(text=T["play"], state="normal")
            except tkinter.TclError:
                pass
        self.root.after(250, self.poll)

    def close(self):
        if self.build:
            if messagebox.askyesno(T["window"], T["confirm_cancel"], parent=self.root):
                build = self.build
                threading.Thread(target=build.cancel, args=(self.settings.values["wsl_distro"],), daemon=True).start()
            return
        if getattr(self, "folder", None) is not None and self.frame and self.frame.winfo_exists():
            try:
                self.save_choices()
            except tkinter.TclError:
                pass
        self.root.destroy()


def main():
    if WIN:
        try:
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("AgeOfCalamityRecomp.Setup")
        except (AttributeError, OSError):
            pass
    try:
        App().root.mainloop()
    except Exception:
        import traceback
        report = traceback.format_exc()
        try:
            (ROOT / "setup-error.txt").write_text(report, encoding="utf-8")
        except OSError:
            pass
        if WIN:
            ctypes.windll.user32.MessageBoxW(None, report[-1500:], "Setup", 0x10)
        else:
            print(report, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
