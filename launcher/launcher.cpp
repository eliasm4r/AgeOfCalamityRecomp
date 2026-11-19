// AgeOfCalamityRecomp launcher: one window with two screens, for Windows and Linux. Dear ImGui on SDL3's renderer
// (runtime/third_party/imgui, MIT). The look is drawn here (colours, geometric lines, rings): no game or Nintendo
// artwork is in the repository.
//
// Build screen: the player picks the folder with their own dump and the targets, and the launcher runs
// tools/make_build.py, reading its protocol ("@@PLAN <id> <title>", "@@STEP <id> <state> <text>", "@@NEED wsl",
// "@@DONE ok|fail"; every other line goes to the log). It shows while the game for this PC is not forged yet, or after
// an update (VERSION differs from build/forge-state.json), or on "Forge again".
//
// Play screen: once the game for this PC is there (dist/aoc-windows on Windows, dist/aoc-deck on Linux), the window
// turns into the game's launcher: the main settings of that folder's aoc.ini (the keys runtime/cfg.c reads, edited in
// place the way cfg_save does: the line of a key keeps its spelling, a missing key goes into its section, the rest of
// the file is untouched), Play, Report (tools/make_report.py), the game folder and Rebuild.
//
// The interface is in English. Launcher settings live in launcher.ini beside the executable.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

namespace fs = std::filesystem;

// ---- palette (Sheikah blue, Guardian orange, Calamity red on a night background)
static const ImVec4 C_BG0(0.020f, 0.039f, 0.070f, 1.0f);
static const ImVec4 C_BG1(0.043f, 0.090f, 0.150f, 1.0f);
static const ImVec4 C_PANEL(0.035f, 0.070f, 0.120f, 0.86f);
static const ImVec4 C_CYAN(0.282f, 0.820f, 1.000f, 1.0f);
static const ImVec4 C_CYAN_DIM(0.160f, 0.470f, 0.620f, 1.0f);
static const ImVec4 C_ORANGE(1.000f, 0.604f, 0.235f, 1.0f);
static const ImVec4 C_RED(0.831f, 0.227f, 0.353f, 1.0f);
static const ImVec4 C_TEXT(0.902f, 0.945f, 1.000f, 1.0f);
static const ImVec4 C_MUTED(0.560f, 0.660f, 0.760f, 1.0f);
static const ImVec4 C_OK(0.380f, 0.900f, 0.660f, 1.0f);

static ImU32 col(const ImVec4 &c, float a = -1) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, a < 0 ? c.w : a)); }

// ---- the interface's text, Portuguese and English
enum Str {
    T_KICKER, T_FILES, T_FILES_HINT, T_BROWSE, T_NO_FOLDER, T_BASE, T_UPDATE, T_DLC, T_DLC_N, T_KEYS_OK, T_MISSING,
    T_TARGET, T_DECK, T_DECK_LINUX, T_ROMFS, T_SEND, T_HOST, T_WINDOWS, T_ADVANCED, T_DISTRO, T_DISTRO_HINT, T_WORK,
    T_WORK_HINT, T_FORGE, T_FORGING, T_CHECK, T_CANCEL, T_PROGRESS, T_HOW1, T_HOW2, T_HOW2_LINUX, T_HOW3, T_HOW4,
    T_DONE_SKIP, T_CHECKED, T_FORGED, T_FORGED_DECK, T_FAILED, T_CANCELED, T_LOG, T_FOLLOW, T_OPEN_DECK, T_FOOTER,
    T_GUIDE, T_NEED_WSL, T_NEED_WSL2, T_INSTALL_WSL, T_WSL_RUNNING, T_WSL_DONE, T_WSL_FAIL, T_CANCELLED_LOG,
    T_NO_PYTHON, T_UPDATE_NOTE, T_GRAPHICS, T_CONTROLS, T_GFX_NOTE, T_CTL_NOTE, T_PLAY, T_PLAYING, T_REPORT,
    T_REPORTING, T_REPORT_DONE, T_REPORT_FAIL, T_GAME_FOLDER, T_LOGS_FOLDER, T_FORGE_AGAIN, T_BACK_TO_GAME, T_SAVE,
    T_SAVED, T_SAVE_FAIL, T_CLOSE, T_FRAMERATE, T_FRAMERATE_VAL, T_FRAMERATE_DESC, T_UNSAVED, T_PLAY_FAIL, T_UNNAMED,
    // the setup wizard and the play screen
    T_STEP1, T_STEP2, T_STEP3, T_S1_TITLE, T_S1_HINT, T_S1_NOTE, T_KEYS, T_OPTIONAL, T_S2_TITLE, T_S2_HINT, T_WIN_DESC,
    T_DECK_DESC, T_DECK_DESC_LINUX, T_S3_TITLE, T_S3_FOR, T_BACK, T_NEXT, T_DETAILS, T_NEED_FILES, T_NEED_TARGET,
    T_READY, T_READY_FOR, T_COUNT
};
static const char *STR[T_COUNT] = {
    "HYRULE WARRIORS  ·  RECOMPILED",
    "YOUR COPY",
    "Folder with the NSPs (base, update 1.3.0, DLC) and prod.keys",
    "Browse...",
    "folder not found",
    "Base game",
    "Update 1.3.0",
    "DLCs",
    "%d found",
    "found (contents never shown)",
    "(missing)",
    "WHERE YOU WILL PLAY",
    "Steam Deck / Linux",
    "Steam Deck / Linux (this PC)",
    "Include the game data (10.9 GB): needed the first time you copy the folder",
    "Send the folder to the Deck over the network (SSH) when done",
    "Deck address (user@ip)",
    "Windows (this PC)",
    "Advanced",
    "WSL distro",
    "AoC-Recomp (automatic)",
    "Windows build folder",
    "default",
    "BUILD",
    "BUILDING...",
    "Check files only",
    "Cancel",
    "PROGRESS",
    "1. Pick the folder with your copy of the game (NSPs and prod.keys).",
    "2. Tick where you will play: Steam Deck / Linux, Windows or both.",
    "2. Keep Steam Deck / Linux ticked (the folder works on this PC and on the Deck).",
    "3. Click BUILD and let the PC work.",
    "The first build takes 1 to 2 hours (it downloads ~2 GB of tools and compiles everything).\nLater builds only redo what changed: a few minutes.",
    "(already done)",
    "Files checked: all good.",
    "Done. The game is built and ready to play.",
    "Done. Copy the Deck folder to your Steam Deck (see the README).",
    "The build failed. Open Technical details below to see why.",
    "Cancelled.",
    "Log",
    "follow",
    "Deck folder",
    "Use only your own copy, dumped from your console. Nothing from the game is distributed: it is all made on this PC.",
    "README",
    "WSL (Linux inside Windows) is needed for the Steam Deck version.",
    "Install it with the button below (Windows asks for administrator permission), then restart the PC.",
    "Install WSL",
    "Installing WSL... follow the Windows window.",
    "WSL installed. Restart the PC and open the launcher again.",
    "The WSL install did not finish (code %d). Try again or see the guide.",
    "cancelled by the user",
    "could not start Python: ",
    "A new AgeOfCalamityRecomp version is here: build again to update the game (only what changed is redone).",
    "DISPLAY AND GRAPHICS",
    "CONTROLS AND SOUND",
    "Changes apply the next time the game starts. More options in the game: press F1.",
    "In the game, F1 (or Minus + Plus on a controller) opens the full menu: button remapping, colours, HDR.",
    "PLAY",
    "PLAYING",
    "Report a problem...",
    "Making the report...",
    "aoc-report.zip made (logs and PC details, nothing of the game). Drag it into the page that opened in your browser.",
    "The report could not be made.",
    "Game folder",
    "Logs",
    "Rebuild...",
    "Back to the game",
    "Save",
    "Settings saved.",
    "Could not write aoc.ini.",
    "Close",
    "Frame rate",
    "30 fps (the game's own)",
    "30 FPS is the game's own rate: it moves a fixed step per frame. 60 FPS needs a patch that is not ported yet.",
    "unsaved changes",
    "Could not start the game.",
    "(no ID in the name; checked when building)",
    "Game files",
    "Platforms",
    "Build",
    "STEP 1 · YOUR GAME FILES",
    "Pick the folder with the NSPs dumped from your console (base game, update 1.3.0, DLC) and your prod.keys.",
    "Nothing is uploaded anywhere: the game is built on this PC from your own files.",
    "prod.keys (your console's keys)",
    "optional",
    "STEP 2 · WHERE WILL YOU PLAY?",
    "Tick one or both.",
    "Builds the game for this Windows PC.",
    "Builds a folder to copy to a Steam Deck or a Linux PC (uses WSL, the Linux inside Windows).",
    "Builds the game for this PC; the same folder also runs on a Steam Deck.",
    "STEP 3 · BUILD THE GAME",
    "For: ",
    "Back",
    "Next",
    "Technical details",
    "Something is missing in the chosen folder.",
    "Tick at least one platform.",
    "READY TO PLAY",
    "Version %s · built for this PC",
};
static const char *tr(Str s) { return STR[s]; }
// A label with its own ImGui id.
static std::string lab(Str s, const char *id) { return std::string(tr(s)) + "###" + id; }

// ---- paths and settings
static fs::path g_exe_dir, g_root;
struct Settings {
    std::string game_dir, deck_host = "deck@steamdeck", win_work, wsl_distro;
    bool deck = true, windows = false, send = false, with_romfs = false;
};
static Settings S;

#ifdef _WIN32
static std::wstring widen(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
static std::string narrow(const std::wstring &w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::string u8(const fs::path &p) { return narrow(p.wstring()); }
static fs::path upath(const std::string &s) { return fs::path(widen(s)); }
static const bool IS_WINDOWS = true;
#else
static std::string u8(const fs::path &p) { return p.string(); }
static fs::path upath(const std::string &s) { return fs::path(s); }
static const bool IS_WINDOWS = false;
#endif

static void settings_load() {
    std::ifstream f(g_exe_dir / "launcher.ini");
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);   // a UTF-8 BOM (an editor saved the file)
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "game_dir") S.game_dir = v;
        else if (k == "deck_host") S.deck_host = v;
        else if (k == "win_work") S.win_work = v;
        else if (k == "wsl_distro") S.wsl_distro = v;
        else if (k == "deck") S.deck = v == "1";
        else if (k == "windows") S.windows = v == "1";
        else if (k == "send") S.send = v == "1";
        else if (k == "with_romfs") S.with_romfs = v == "1";
    }
}
static void settings_save() {
    std::ofstream f(g_exe_dir / "launcher.ini");
    f << "game_dir=" << S.game_dir << "\ndeck_host=" << S.deck_host << "\nwin_work=" << S.win_work
      << "\nwsl_distro=" << S.wsl_distro << "\ndeck=" << S.deck
      << "\nwindows=" << S.windows << "\nsend=" << S.send << "\nwith_romfs=" << S.with_romfs << "\n";
}

static std::string read_text(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// The game this PC plays: dist/aoc-windows on Windows, dist/aoc-deck on Linux.
static fs::path game_dir() { return g_root / "dist" / (IS_WINDOWS ? "aoc-windows" : "aoc-deck"); }
static bool game_ready() {
    std::error_code ec;
    return fs::is_regular_file(game_dir() / (IS_WINDOWS ? "aoc.exe" : "aoc"), ec);
}
// "version" of build/forge-state.json (tools/make_build.py writes it after a successful forge).
static std::string forged_version() {
    std::string t = read_text(g_root / "build" / "forge-state.json");
    size_t k = t.find("\"version\"");
    if (k == std::string::npos) return "";
    size_t a = t.find('"', t.find(':', k) + 1), b = a == std::string::npos ? a : t.find('"', a + 1);
    return b == std::string::npos ? "" : t.substr(a + 1, b - a - 1);
}
static std::string package_version() { return trim(read_text(g_root / "VERSION")); }

// ---- the dump folder at a glance (names only, a hint; the forge's check step identifies the packages by content)
struct DumpScan {
    int nsp = 0, dlc = 0;
    bool base = false, update = false, keys = false, exists = false;
};
static DumpScan scan_dump(const std::string &dir) {
    DumpScan d;
    std::error_code ec;
    fs::path p = upath(dir);
    if (dir.empty() || !fs::is_directory(p, ec)) return d;
    d.exists = true;
    d.keys = fs::is_regular_file(p / "prod.keys", ec);
    int seen = 0;
    for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator() && seen < 20000; it.increment(ec), seen++) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        std::string name = u8(it->path().filename());
        for (auto &ch : name) ch = (char)toupper((unsigned char)ch);
        if (name.size() < 4 || name.substr(name.size() - 4) != ".NSP") continue;
        d.nsp++;
        if (name.find("01002B00111A2000") != std::string::npos) d.base = true;
        else if (name.find("01002B00111A2800") != std::string::npos) d.update = true;
        else if (name.find("01002B00111A3") != std::string::npos) d.dlc++;
    }
    return d;
}

// ---- aoc.ini, edited in place (the rules of runtime/cfg.c cfg_load and cfg_save)
struct GameIni {
    fs::path path;
    std::vector<std::string> lines;   // without their line ends
    bool crlf = false, bom = false;

    // "KEY" of a "key = value" line (upper case, no AOC_ prefix); "" for anything else
    static std::string key_of(const std::string &l) {
        std::string t = trim(l);
        if (t.empty() || t[0] == '#' || t[0] == ';' || t[0] == '[') return "";
        size_t eq = t.find('=');
        if (eq == std::string::npos) return "";
        std::string k = trim(t.substr(0, eq));
        for (auto &c : k) c = (char)toupper((unsigned char)c);
        if (k.rfind("AOC_", 0) == 0) k = k.substr(4);
        return k;
    }
    static std::string section_of_line(const std::string &l) {
        std::string t = trim(l);
        if (t.size() < 2 || t[0] != '[') return "";
        size_t e = t.find(']');
        return e == std::string::npos ? "" : t.substr(1, e - 1);
    }
    void load(const fs::path &p) {
        path = p;
        lines.clear();
        std::string t = read_text(p);
        bom = t.rfind("\xEF\xBB\xBF", 0) == 0;
        if (bom) t.erase(0, 3);
        crlf = t.find("\r\n") != std::string::npos;
        size_t s = 0;
        while (s < t.size()) {
            size_t e = t.find('\n', s);
            std::string l = t.substr(s, e == std::string::npos ? std::string::npos : e - s);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
            if (e == std::string::npos) break;
            s = e + 1;
        }
    }
    int find(const std::string &key) const {   // the last line of the key (the one cfg_load keeps)
        int at = -1;
        for (int i = 0; i < (int)lines.size(); i++)
            if (key_of(lines[i]) == key) at = i;
        return at;
    }
    std::string get(const std::string &key) const {
        int i = find(key);
        if (i < 0) return "";
        std::string v = trim(lines[i].substr(lines[i].find('=') + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        return v;
    }
    void set(const std::string &key, const std::string &value, const char *section) {
        int i = find(key);
        if (i >= 0) {   // keep the line's indentation and key spelling
            size_t eq = lines[i].find('=');
            std::string k = lines[i].substr(0, eq);
            while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
            lines[i] = k + " = " + value;
            return;
        }
        std::string lk = key;
        for (auto &c : lk) c = (char)tolower((unsigned char)c);
        int in = 0, tail = -1;
        for (int j = 0; j < (int)lines.size(); j++) {
            std::string sec = section_of_line(lines[j]);
            if (!sec.empty()) {
                in = sec == section;
                if (in) tail = j;
                continue;
            }
            if (in && !trim(lines[j]).empty()) tail = j;
        }
        if (tail >= 0) {
            lines.insert(lines.begin() + tail + 1, lk + " = " + value);
        } else {
            while (!lines.empty() && trim(lines.back()).empty()) lines.pop_back();
            if (!lines.empty()) lines.push_back("");
            lines.push_back(std::string("[") + section + "]");
            lines.push_back(lk + " = " + value);
        }
    }
    bool save() const {
        std::string eol = crlf ? "\r\n" : "\n", out = bom ? "\xEF\xBB\xBF" : "";
        for (auto &l : lines) out += l + eol;
        fs::path tmp = path;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return false;
            f << out;
            if (!f) return false;
        }
        std::error_code ec;
        fs::rename(tmp, path, ec);
        return !ec;
    }
};

// The settings the play screen shows: keys, values and defaults as runtime/cfg.c's metas has them; the texts follow
// runtime/ui_text.c. Only settings measured on Age of Calamity are here; the in-game menu (F1) has the rest.
struct Opt {
    const char *key, *section, *def;
    int kind;   // 0 choice, 1 slider (integer), 2 toggle
    std::vector<const char *> vals;
    std::vector<const char *> names;
    int lo, hi, step;
    const char *label, *desc;
    const char *needs_off;   // shown disabled while this toggle key is "1"
};
static const std::vector<Opt> GFX = {
    {"FULLSCREEN", "display", "0", 0, {"0", "1"}, {"Windowed", "Fullscreen"}, 0, 0, 0,
     "Window mode",
     "Fullscreen fills the screen; F11 toggles it while playing.", nullptr},
    {"WINDOW_SIZE", "display", "1920x1080", 0, {"1280x720", "1600x900", "1920x1080", "2560x1440"},
     {"1280 × 720", "1600 × 900", "1920 × 1080", "2560 × 1440"},
     0, 0, 0, "Window size",
     "Size of the game window when not fullscreen.", "FULLSCREEN"},
    {"VK_PRESENT", "display", "mailbox", 0, {"mailbox", "fifo", "immediate"},
     {"Mailbox (recommended)", "VSync", "Immediate"}, 0, 0, 0,
     "Presentation",
     "How finished frames reach the screen. Mailbox: no tearing and little waiting. VSync: waits for the screen. Immediate: the least waiting, may tear.", nullptr},
    {"OUTPUT_RES", "display", "1920x1080", 0, {"1920x1080", "1280x720"},
     {"1920 × 1080 (the game's)", "1280 × 720 (lighter)"}, 0, 0, 0,
     "Game resolution",
     "The size the picture and interface are drawn in. 1920 × 1080 is the game's own; 1280 × 720 matches the Steam Deck's screen and spares the GPU.", nullptr},
    {"OPERATION_MODE", "language", "docked", 0, {"docked", "handheld"},
     {"TV (sharper)", "Handheld (lighter)"}, 0, 0, 0,
     "Internal resolution",
     "TV: the game draws at its docked resolution, sharper and heavier. Handheld: its handheld resolution, lighter on the GPU and the battery.", nullptr},
    {"RENDER_SCALE", "display", "game", 0, {"game", "50", "60", "67", "85", "100"},
     {"Game (75 %)", "50 %", "60 %", "67 %", "85 %", "100 %"},
     0, 0, 0, "3D render scale",
     "Size of the 3D scene relative to the game resolution; the game's FSR scales it up. Higher: sharper and heavier.", nullptr},
    {"ASPECT", "display", "fit", 0, {"fit", "stretch"}, {"Black bars", "Stretch"}, 0, 0, 0,
     "Picture shape",
     "On a screen of another shape (a 16:9 picture on a 21:9 screen): keep the shape with black bars, or stretch it.", nullptr},
    {"ANISO", "display", "game", 0, {"game", "2", "4", "8", "16"},
     {"Game", "2x", "4x", "8x", "16x"}, 0, 0, 0,
     "Texture filtering",
     "Ground, roads and walls seen at a shallow angle stay sharp into the distance. Game: the level the game asks for, as on the Switch.", nullptr},
    {"SHADOW_RES", "display", "game", 0, {"game", "1024", "1536", "2048"},
     {"Game (changes)", "1024", "1536", "2048"}, 0, 0, 0,
     "Shadows",
     "Size of the sun's shadow map. A fixed size keeps shadows steadier and sharper; above 1024 it costs more GPU time.", nullptr},
    {"PERF_OVERLAY", "display", "off", 0, {"off", "fps", "full"},
     {"Off", "FPS", "Full"}, 0, 0, 0,
     "Performance overlay",
     "Frame rate in the corner of the screen. Full adds frame times, CPU, GPU and memory. F10 cycles it.", nullptr},
};
static const std::vector<Opt> CTL = {
    {"PAD_LAYOUT", "input", "auto", 0, {"auto", "nintendo", "xbox"},
     {"Automatic", "Nintendo (A right)", "Xbox (A bottom)"},
     0, 0, 0, "Button layout",
     "Which button is A. Nintendo: the right one, as on a Switch. Xbox: the bottom one. Automatic picks by controller.", nullptr},
    {"RUMBLE_STRENGTH", "input", "130", 1, {}, {}, 0, 300, 10, "Vibration",
     "How strong the controller vibrates. 0 turns it off.", nullptr},
    {"STICK_DEADZONE", "input", "8", 1, {}, {}, 0, 50, 1, "Stick dead zone",
     "How far a stick must move before the game notices. Raise it if the character or the camera drifts.", nullptr},
    {"INVERT_RX", "input", "0", 2, {}, {}, 0, 0, 0, "Invert camera (horizontal)",
     "Mirrors the right stick's horizontal axis.", nullptr},
    {"INVERT_RY", "input", "0", 2, {}, {}, 0, 0, 0, "Invert camera (vertical)",
     "Mirrors the right stick's vertical axis.", nullptr},
    {"VOLUME", "audio", "100", 1, {}, {}, 0, 100, 5, "Volume",
     "Master volume of the game.", nullptr},
    {"MUTE_BACKGROUND", "audio", "0", 2, {}, {}, 0, 0, 0, "Mute in the background",
     "Silences the game while another window is active.", nullptr},
};

static GameIni g_ini;
static std::vector<std::string> g_vals_gfx, g_vals_ctl, g_saved_gfx, g_saved_ctl;
static void ini_load() {
    g_ini.load(game_dir() / "aoc.ini");
    auto fill = [](const std::vector<Opt> &opts, std::vector<std::string> &v) {
        v.clear();
        for (auto &o : opts) {
            std::string x = g_ini.get(o.key);
            v.push_back(x.empty() ? o.def : x);
        }
    };
    fill(GFX, g_vals_gfx), fill(CTL, g_vals_ctl);
    g_saved_gfx = g_vals_gfx, g_saved_ctl = g_vals_ctl;
}
static bool ini_dirty() { return g_vals_gfx != g_saved_gfx || g_vals_ctl != g_saved_ctl; }
static bool ini_save() {
    g_ini.load(game_dir() / "aoc.ini");   // what the game may have written meanwhile stays
    auto put = [](const std::vector<Opt> &opts, const std::vector<std::string> &v, const std::vector<std::string> &old) {
        for (size_t i = 0; i < opts.size(); i++)
            if (v[i] != old[i]) g_ini.set(opts[i].key, v[i], opts[i].section);
    };
    put(GFX, g_vals_gfx, g_saved_gfx), put(CTL, g_vals_ctl, g_saved_ctl);
    if (!g_ini.save()) return false;
    g_saved_gfx = g_vals_gfx, g_saved_ctl = g_vals_ctl;
    return true;
}

// ---- the build process
struct Step {
    std::string id, title, state = "pending", note;
    double t0 = 0, t1 = 0;
};
static std::mutex g_lock;
static std::vector<Step> g_steps;
static std::deque<std::string> g_log;
static std::atomic<int> g_running{0};   // 1 while make_build.py runs
static std::string g_done;              // "", "ok", "fail", "cancel"
static bool g_check_only;               // the running or last run was "only check the files"
static bool g_need_wsl;                 // make_build.py said "@@NEED wsl"
static double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void log_line(const std::string &l) {
    g_log.push_back(l);
    while (g_log.size() > 4000) g_log.pop_front();
}

static void handle_line(const std::string &l) {   // g_lock held
    if (l.rfind("@@PLAN ", 0) == 0) {
        size_t sp = l.find(' ', 7);
        Step s;
        s.id = l.substr(7, sp - 7);
        s.title = sp == std::string::npos ? s.id : l.substr(sp + 1);
        g_steps.push_back(s);
    } else if (l.rfind("@@STEP ", 0) == 0) {
        size_t a = l.find(' ', 7), b = a == std::string::npos ? a : l.find(' ', a + 1);
        std::string id = l.substr(7, a - 7), st = a == std::string::npos ? "" : l.substr(a + 1, b - a - 1);
        std::string note = b == std::string::npos ? "" : l.substr(b + 1);
        for (auto &s : g_steps)
            if (s.id == id) {
                if (st == "run") s.t0 = now_s();
                else s.t1 = now_s();
                s.state = st;
                if (st == "fail") s.note = note;
            }
    } else if (l.rfind("@@NEED wsl", 0) == 0) {
        g_need_wsl = true;
    } else if (l.rfind("@@DONE ", 0) == 0) {
        if (g_done.empty()) g_done = l.substr(7);
    } else {
        log_line(l);
    }
}

// Feeds the child's output, split into lines, to handle_line.
struct LineSplitter {
    std::string buf;
    void feed(const char *p, size_t n) {
        buf.append(p, n);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::lock_guard<std::mutex> g(g_lock);
            handle_line(line);
            buf.erase(0, pos + 1);
        }
    }
};

static void finish_run() {   // the child ended
    std::lock_guard<std::mutex> g(g_lock);
    if (g_done.empty()) g_done = "fail";
    for (auto &s : g_steps)
        if (s.state == "run") s.state = g_done == "cancel" ? "cancel" : "fail", s.t1 = now_s();
    g_running = 0;
}

// make_build.py's arguments (UTF-8).
static std::vector<std::string> build_args(bool check_only) {
    std::vector<std::string> a = {"-u", u8(g_root / "tools" / "make_build.py"), "--game-dir", S.game_dir,
                                  "--lang", "en"};
    if (S.deck) a.push_back("--deck");
    if (S.windows && IS_WINDOWS) a.push_back("--windows");
    if (S.deck && S.with_romfs) a.push_back("--with-romfs");
    if (S.deck && S.send) a.insert(a.end(), {"--send", "--deck-host", S.deck_host});
    if (IS_WINDOWS && !S.win_work.empty()) a.insert(a.end(), {"--win-work", S.win_work});
    if (IS_WINDOWS && !S.wsl_distro.empty()) a.insert(a.end(), {"--wsl-distro", S.wsl_distro});
    if (check_only) a.push_back("--check-only");
    return a;
}

static void reset_run(bool check_only) {
    g_check_only = check_only;
    std::lock_guard<std::mutex> g(g_lock);
    g_steps.clear();
    g_log.clear();
    g_done.clear();
    g_need_wsl = false;
}

// A small job in the background (the report): its exit code lands here; -1 while it runs.
static std::atomic<int> g_job_state{0};   // 0 idle, 1 running, 2 ok, 3 failed

#ifdef _WIN32
static HANDLE g_job, g_proc, g_game;

// The Python that runs the tools: the one the release package carries (python/, with cryptography, lz4 and numpy),
// else MSYS2's (a developer's tree), else the one on PATH.
static fs::path python_exe() {
    std::error_code ec;
    for (fs::path p : {g_root / "python" / "python.exe", fs::path("C:/msys64/ucrt64/bin/python3.exe")})
        if (fs::is_regular_file(p, ec)) return p;
    return fs::path(L"python");
}

static std::wstring quote(const std::wstring &s) { return L"\"" + s + L"\""; }

static void start_build(bool check_only) {
    reset_run(check_only);
    std::wstring cmd = quote(python_exe().wstring());
    for (auto &a : build_args(check_only)) cmd += L" " + quote(widen(a));

    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cl = cmd, cwd = g_root.wstring();
    if (!CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                        cwd.c_str(), &si, &pi)) {
        CloseHandle(rd), CloseHandle(wr);
        std::lock_guard<std::mutex> g(g_lock);
        log_line(tr(T_NO_PYTHON) + narrow(python_exe().wstring()));
        g_done = "fail";
        return;
    }
    // A job: Cancel (or closing the launcher) ends the whole tree
    g_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(wr);
    g_proc = pi.hProcess;
    g_running = 1;
    std::thread([rd]() {
        LineSplitter ls;
        char chunk[4096];
        DWORD n;
        while (ReadFile(rd, chunk, sizeof chunk, &n, nullptr) && n) ls.feed(chunk, n);
        CloseHandle(rd);
        WaitForSingleObject(g_proc, INFINITE);
        finish_run();
    }).detach();
}

static void run_hidden(std::wstring cl) {
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
}

static void cancel_build() {
    {
        std::lock_guard<std::mutex> g(g_lock);
        g_done = "cancel";
        log_line(tr(T_CANCELLED_LOG));
    }
    if (g_job) TerminateJobObject(g_job, 1);
    // the Linux side of a WSL step keeps running without its wsl.exe: stop the build there too
    std::string d = S.wsl_distro.empty() ? "AoC-Recomp" : S.wsl_distro;
    run_hidden(L"wsl -d " + widen(d) + L" -e pkill -f tools/build.py");
}

static void open_path(const fs::path &p) {
    if ((INT_PTR)ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOW) <= 32)
        ShellExecuteW(nullptr, L"open", L"notepad.exe", quote(p.wstring()).c_str(), nullptr, SW_SHOW);
}
static void open_url(const char *url) { ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOW); }
// The game without a console window: its log goes to logs/aoc.log anyway.
static bool play() {
    fs::path d = game_dir();
    std::wstring cl = quote((d / "aoc.exe").wstring()), cwd = d.wstring();
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    if (g_game) CloseHandle(g_game);
    g_game = pi.hProcess;
    return true;
}
static bool game_running() { return g_game && WaitForSingleObject(g_game, 0) == WAIT_TIMEOUT; }

static void start_report() {
    g_job_state = 1;
    std::thread([]() {
        std::wstring cl = quote(python_exe().wstring()) + L" " + quote((g_root / "tools" / "make_report.py").wstring());
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi{};
        std::wstring cwd = g_root.wstring();
        if (!CreateProcessW(nullptr, cl.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi)) {
            g_job_state = 3;
            return;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
        g_job_state = code == 0 ? 2 : 3;
    }).detach();
}

// WSL install, elevated: wsl.exe --install --no-distribution (the tools step makes the distro afterwards).
static std::atomic<int> g_wsl_inst{0};   // 0 idle, 1 running, 2 done, 3 failed
static std::atomic<int> g_wsl_code{0};
static void install_wsl() {
    g_wsl_inst = 1;
    std::thread([]() {
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof sei;
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = L"wsl.exe";
        sei.lpParameters = L"--install --no-distribution";
        sei.nShow = SW_SHOW;
        if (!ShellExecuteExW(&sei) || !sei.hProcess) {
            g_wsl_code = (int)GetLastError();
            g_wsl_inst = 3;
            return;
        }
        WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        g_wsl_code = (int)code;
        g_wsl_inst = code == 0 ? 2 : 3;
    }).detach();
}
#else
static pid_t g_pid, g_game_pid;

static void start_build(bool check_only) {
    reset_run(check_only);
    std::vector<std::string> a = build_args(check_only);
    int fds[2];
    if (pipe(fds)) return;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]), close(fds[1]);
        return;
    }
    if (pid == 0) {   // child: own process group (Cancel stops the whole tree), output into the pipe
        setpgid(0, 0);
        dup2(fds[1], 1), dup2(fds[1], 2);
        close(fds[0]), close(fds[1]);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, 0);
        if (chdir(g_root.c_str())) _exit(126);
        std::vector<char *> argv = {(char *)"python3"};
        for (auto &s : a) argv.push_back(s.data());
        argv.push_back(nullptr);
        execvp("python3", argv.data());
        fprintf(stderr, "%spython3\n", tr(T_NO_PYTHON));
        _exit(127);
    }
    setpgid(pid, pid);
    close(fds[1]);
    g_pid = pid;
    g_running = 1;
    int rd = fds[0];
    std::thread([rd, pid]() {
        LineSplitter ls;
        char chunk[4096];
        ssize_t n;
        while ((n = read(rd, chunk, sizeof chunk)) > 0) ls.feed(chunk, (size_t)n);
        close(rd);
        int st;
        waitpid(pid, &st, 0);
        finish_run();
    }).detach();
}

static void cancel_build() {
    {
        std::lock_guard<std::mutex> g(g_lock);
        g_done = "cancel";
        log_line(tr(T_CANCELLED_LOG));
    }
    if (g_pid > 0) kill(-g_pid, SIGTERM);
}

static pid_t spawn_detached(const std::vector<std::string> &cmd, const fs::path &cwd) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (!cwd.empty() && chdir(cwd.c_str())) _exit(126);
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) dup2(nul, 0), dup2(nul, 1), dup2(nul, 2);
        std::vector<char *> argv;
        for (auto &s : cmd) argv.push_back((char *)s.c_str());
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    return pid;
}
static void open_path(const fs::path &p) { spawn_detached({"xdg-open", p.string()}, {}); }
static void open_url(const char *url) { spawn_detached({"xdg-open", url}, {}); }
static bool play() {
    g_game_pid = spawn_detached({(game_dir() / "aoc.sh").string()}, game_dir());
    return g_game_pid > 0;
}
static bool game_running() {
    if (g_game_pid <= 0) return false;
    int st;
    if (waitpid(g_game_pid, &st, WNOHANG) == g_game_pid) g_game_pid = 0;
    return g_game_pid > 0;
}
static void start_report() {
    g_job_state = 1;
    std::thread([]() {
        pid_t pid = fork();
        if (pid == 0) {
            if (chdir(g_root.c_str())) _exit(126);
            int nul = open("/dev/null", O_RDWR);
            if (nul >= 0) dup2(nul, 0), dup2(nul, 1), dup2(nul, 2);
            std::string script = (g_root / "tools" / "make_report.py").string();
            execlp("python3", "python3", script.c_str(), (char *)nullptr);
            _exit(127);
        }
        int st = 1;
        if (pid > 0) waitpid(pid, &st, 0);
        g_job_state = pid > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 2 : 3;
    }).detach();
}
#endif

// ---- folder dialog (SDL3 calls back, possibly on another thread)
static std::mutex g_pick_lock;
static std::string g_picked;
static void SDLCALL on_folder(void *, const char *const *list, int) {
    if (!list || !list[0]) return;
    std::lock_guard<std::mutex> g(g_pick_lock);
    g_picked = list[0];
}

// ---- drawing helpers
static ImFont *F_BODY, *F_HEAD, *F_TITLE, *F_SMALL;
static float g_scale = 1;

static void draw_background(float t) {
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    ImVec2 sz = ImGui::GetIO().DisplaySize;
    dl->AddRectFilledMultiColor(ImVec2(0, 0), sz, col(C_BG1), col(C_BG1), col(C_BG0), col(C_BG0));
    // the Calamity's glow along the bottom
    dl->AddRectFilledMultiColor(ImVec2(0, sz.y * 0.70f), sz, col(C_RED, 0.0f), col(C_RED, 0.0f), col(C_RED, 0.14f),
                                col(C_RED, 0.14f));
    // a faint technical grid
    for (float x = 0; x < sz.x; x += 48) dl->AddLine(ImVec2(x, 0), ImVec2(x, sz.y), col(C_CYAN, 0.03f));
    for (float y = 0; y < sz.y; y += 48) dl->AddLine(ImVec2(0, y), ImVec2(sz.x, y), col(C_CYAN, 0.03f));
    // slow concentric rings, upper right
    ImVec2 c(sz.x * 0.84f, sz.y * 0.13f);
    for (int i = 0; i < 6; i++) {
        float r = 50.0f + i * 30.0f, a0 = t * (0.15f + 0.05f * i) * (i % 2 ? -1 : 1) + i;
        dl->PathArcTo(c, r, a0, a0 + 3.14159f * (0.6f + 0.15f * i), 48);
        dl->PathStroke(col(i % 3 == 2 ? C_ORANGE : C_CYAN, 0.10f + 0.02f * (5 - i)), 0, 2.0f);
        dl->AddCircle(c, r, col(C_CYAN, 0.035f), 96, 1.0f);
    }
    for (int k = 0; k < 12; k++) {   // tick marks on the outer ring
        float a = k * 3.14159f / 6 + t * 0.05f, r0 = 50 + 5 * 30 + 6, r1 = r0 + 10;
        dl->AddLine(ImVec2(c.x + cosf(a) * r0, c.y + sinf(a) * r0), ImVec2(c.x + cosf(a) * r1, c.y + sinf(a) * r1),
                    col(C_CYAN, 0.16f), 2.0f);
    }
}

// Panel with a Sheikah-style frame: thin border, bright corner brackets, a spaced-out title.
static void panel_begin(const char *id, const char *title, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, C_PANEL);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * g_scale, 14 * g_scale));
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetWindowPos(), p1(p0.x + ImGui::GetWindowWidth(), p0.y + ImGui::GetWindowHeight());
    float L = 16;
    ImU32 cc = col(C_CYAN, 0.9f);
    dl->AddLine(p0, ImVec2(p0.x + L, p0.y), cc, 2), dl->AddLine(p0, ImVec2(p0.x, p0.y + L), cc, 2);
    dl->AddLine(ImVec2(p1.x - L, p0.y), ImVec2(p1.x, p0.y), cc, 2), dl->AddLine(ImVec2(p1.x, p0.y), ImVec2(p1.x, p0.y + L), cc, 2);
    dl->AddLine(ImVec2(p0.x, p1.y), ImVec2(p0.x + L, p1.y), cc, 2), dl->AddLine(ImVec2(p0.x, p1.y - L), ImVec2(p0.x, p1.y), cc, 2);
    dl->AddLine(ImVec2(p1.x - L, p1.y), p1, cc, 2), dl->AddLine(ImVec2(p1.x, p1.y - L), p1, cc, 2);
    if (title) {
        ImGui::PushFont(F_HEAD);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float x = p.x;
        for (const char *c = title; *c;) {   // letter-spaced, like an engraved plate
            int n = 1;
            unsigned char u = (unsigned char)*c;
            if (u >= 0xF0) n = 4;
            else if (u >= 0xE0) n = 3;
            else if (u >= 0xC0) n = 2;
            std::string ch(c, (size_t)n);
            dl->AddText(ImVec2(x, p.y), col(C_ORANGE), ch.c_str());
            x += ImGui::CalcTextSize(ch.c_str()).x + 2.5f * g_scale;
            c += n;
        }
        ImGui::Dummy(ImVec2(x - p.x, ImGui::GetTextLineHeight()));
        ImGui::PopFont();
        ImVec2 q = ImGui::GetCursorScreenPos();
        dl->AddRectFilledMultiColor(ImVec2(q.x, q.y), ImVec2(q.x + ImGui::GetContentRegionAvail().x, q.y + 1),
                                    col(C_CYAN, 0.6f), col(C_CYAN, 0.0f), col(C_CYAN, 0.0f), col(C_CYAN, 0.6f));
        ImGui::Dummy(ImVec2(0, 6 * g_scale));
    }
}
static void panel_end() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

static void status_dot(bool ok, const char *label, const char *detail = nullptr) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    ImVec2 c(p.x + 7, p.y + h * 0.5f);
    dl->AddCircleFilled(c, 5.0f, col(ok ? C_OK : C_RED));
    dl->AddCircle(c, 8.0f, col(ok ? C_OK : C_RED, 0.35f), 0, 1.5f);
    ImGui::Dummy(ImVec2(20, h));
    ImGui::SameLine();
    ImGui::TextColored(ok ? C_TEXT : C_MUTED, "%s", label);
    if (detail) {
        ImGui::SameLine();
        ImGui::TextColored(C_MUTED, "%s", detail);
    }
}

// State marker of a build step: ring (pending), turning arc (running), check (ok), cross (fail), dash (skipped).
static void step_marker(const std::string &st, float t) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    ImVec2 c(p.x + 9, p.y + h * 0.5f);
    if (st == "run") {
        dl->PathArcTo(c, 7, t * 6, t * 6 + 4.2f, 24);
        dl->PathStroke(col(C_ORANGE), 0, 2.5f);
    } else if (st == "ok") {
        dl->AddCircleFilled(c, 8, col(C_CYAN, 0.18f));
        dl->AddLine(ImVec2(c.x - 4, c.y), ImVec2(c.x - 1, c.y + 3.5f), col(C_CYAN), 2.2f);
        dl->AddLine(ImVec2(c.x - 1, c.y + 3.5f), ImVec2(c.x + 4.5f, c.y - 3.5f), col(C_CYAN), 2.2f);
    } else if (st == "skip") {
        dl->AddCircle(c, 7, col(C_CYAN_DIM), 0, 1.5f);
        dl->AddLine(ImVec2(c.x - 3, c.y), ImVec2(c.x + 3, c.y), col(C_CYAN_DIM), 2.0f);
    } else if (st == "fail" || st == "cancel") {
        dl->AddCircleFilled(c, 8, col(C_RED, 0.25f));
        dl->AddLine(ImVec2(c.x - 3.5f, c.y - 3.5f), ImVec2(c.x + 3.5f, c.y + 3.5f), col(C_RED), 2.2f);
        dl->AddLine(ImVec2(c.x - 3.5f, c.y + 3.5f), ImVec2(c.x + 3.5f, c.y - 3.5f), col(C_RED), 2.2f);
    } else {
        dl->AddCircle(c, 7, col(C_MUTED, 0.6f), 0, 1.5f);
    }
    ImGui::Dummy(ImVec2(24, h));
}

// The big action button (FORGE / PLAY): Guardian-orange to Calamity-red body, spaced letters, glow when hovered.
static bool big_button(const char *id, const char *label, ImVec2 size, bool enabled, float t) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton("b", size) && enabled;
    bool hov = ImGui::IsItemHovered() && enabled;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 q(p.x + size.x, p.y + size.y);
    float glow = hov ? 0.55f + 0.25f * sinf(t * 4) : 0.25f;
    if (enabled) {
        for (int i = 4; i >= 1; i--)
            dl->AddRect(ImVec2(p.x - i * 2, p.y - i * 2), ImVec2(q.x + i * 2, q.y + i * 2), col(C_ORANGE, glow * 0.12f * i), 6, 0, 2);
        dl->AddRectFilledMultiColor(p, q, col(C_ORANGE, 0.95f), col(C_RED, 0.95f), col(C_RED, 0.85f), col(C_ORANGE, 0.85f));
    } else {
        dl->AddRectFilled(p, q, col(C_MUTED, 0.25f), 4);
    }
    dl->AddRect(p, q, col(enabled ? C_TEXT : C_MUTED, 0.6f), 4, 0, 1.5f);
    ImGui::PushFont(F_HEAD);
    float w = 0;
    for (const char *c = label; *c; c++) w += ImGui::CalcTextSize(c, c + 1).x + 4 * g_scale;
    float x = p.x + (size.x - w + 4 * g_scale) * 0.5f, y = p.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f;
    for (const char *c = label; *c; c++) {
        dl->AddText(ImVec2(x, y), col(enabled ? C_TEXT : C_MUTED), c, c + 1);
        x += ImGui::CalcTextSize(c, c + 1).x + 4 * g_scale;
    }
    ImGui::PopFont();
    ImGui::PopID();
    return pressed;
}

static void tooltip(const char *text) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(360 * g_scale);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// The first sentence of a description (up to ". " or the end), shown under an option's label.
static std::string first_sentence(const char *d) {
    const char *e = strstr(d, ". ");
    return e ? std::string(d, (size_t)(e - d + 1)) : std::string(d);
}

// One settings row: the label on the left with the first sentence of its description under it, the control on the
// right; the whole description as a tooltip on both.
static void opt_row(const Opt &o, std::string &v, const std::vector<std::string> &all, const std::vector<Opt> &opts) {
    bool disabled = false;
    if (o.needs_off)
        for (size_t i = 0; i < opts.size(); i++)
            if (!strcmp(opts[i].key, o.needs_off) && all[i] == "1") disabled = true;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(disabled ? C_MUTED : C_TEXT, "%s", o.label);
    tooltip(o.desc);
    ImGui::PushFont(F_SMALL);
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(C_MUTED, "%s", first_sentence(o.desc).c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    tooltip(o.desc);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-1);
    ImGui::BeginDisabled(disabled);
    ImGui::PushID(o.key);
    if (o.kind == 0) {
        int cur = -1;
        for (size_t i = 0; i < o.vals.size(); i++)
            if (v == o.vals[i]) cur = (int)i;
        std::string shown = cur >= 0 ? o.names[(size_t)cur] : v;   // a value set elsewhere stays visible
        if (ImGui::BeginCombo("##c", shown.c_str())) {
            for (size_t i = 0; i < o.vals.size(); i++)
                if (ImGui::Selectable(o.names[i], (int)i == cur)) v = o.vals[i];
            ImGui::EndCombo();
        }
    } else if (o.kind == 1) {
        int x = atoi(v.c_str());
        if (ImGui::SliderInt("##s", &x, o.lo, o.hi, "%d %%")) {
            x = std::clamp((x + o.step / 2) / o.step * o.step, o.lo, o.hi);
            v = std::to_string(x);
        }
    } else {
        bool on = v == "1";
        if (ImGui::Checkbox(on ? "On" : "Off", &on)) v = on ? "1" : "0";
    }
    ImGui::PopID();
    ImGui::EndDisabled();
    tooltip(o.desc);
}

static void opt_table(const char *id, const std::vector<Opt> &opts, std::vector<std::string> &vals) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6 * g_scale, 7 * g_scale));
    if (ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthStretch, 0.46f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch, 0.54f);
        for (size_t i = 0; i < opts.size(); i++) opt_row(opts[i], vals[i], vals, opts);
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

static void apply_theme() {
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 0, s.FrameRounding = 3, s.GrabRounding = 3, s.ChildRounding = 4, s.ScrollbarRounding = 3;
    s.PopupRounding = 3, s.FramePadding = ImVec2(10, 6), s.ItemSpacing = ImVec2(10, 8), s.WindowBorderSize = 0;
    s.ChildBorderSize = 1, s.GrabMinSize = 12;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_Text] = C_TEXT;
    c[ImGuiCol_TextDisabled] = C_MUTED;
    c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ChildBg] = C_PANEL;
    c[ImGuiCol_PopupBg] = ImVec4(0.04f, 0.08f, 0.13f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(C_CYAN.x, C_CYAN.y, C_CYAN.z, 0.22f);
    c[ImGuiCol_FrameBg] = ImVec4(0.06f, 0.12f, 0.20f, 1);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.09f, 0.18f, 0.28f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.11f, 0.22f, 0.34f, 1);
    c[ImGuiCol_CheckMark] = C_CYAN;
    c[ImGuiCol_SliderGrab] = C_CYAN;
    c[ImGuiCol_SliderGrabActive] = C_ORANGE;
    c[ImGuiCol_Button] = ImVec4(0.08f, 0.20f, 0.30f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.12f, 0.30f, 0.44f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.16f, 0.40f, 0.56f, 1);
    c[ImGuiCol_Header] = ImVec4(0.10f, 0.26f, 0.38f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.12f, 0.30f, 0.44f, 1);
    c[ImGuiCol_HeaderActive] = ImVec4(0.16f, 0.40f, 0.56f, 1);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.2f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(C_CYAN_DIM.x, C_CYAN_DIM.y, C_CYAN_DIM.z, 0.6f);
    c[ImGuiCol_Separator] = ImVec4(C_CYAN.x, C_CYAN.y, C_CYAN.z, 0.25f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(C_CYAN.x, C_CYAN.y, C_CYAN.z, 0.30f);
}

static fs::path find_root(fs::path from) {
    std::error_code ec;
    for (int i = 0; i < 6 && !from.empty(); i++, from = from.parent_path())
        if (fs::is_regular_file(from / "tools" / "make_build.py", ec)) return from;
    return {};
}

static fs::path exe_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::current_path() : p.parent_path();
#endif
}

// ---- the two screens
enum Screen { SCR_BUILD, SCR_PLAY };
static Screen g_screen;
static std::string g_status;     // the bottom bar's message
static ImVec4 g_status_col = C_MUTED;
static double g_status_t;

static void set_status(const char *msg, const ImVec4 &c) { g_status = msg, g_status_col = c, g_status_t = now_s(); }

// Header: a kicker line, the title with a soft glow, a line with diamonds; the README button at the top right.
static void draw_header(ImGuiIO &io, float pad) {
    float s = g_scale;
    ImGui::SetCursorPos(ImVec2(io.DisplaySize.x - pad - 100 * s, pad * 0.55f));
    if (ImGui::Button(lab(T_GUIDE, "guide").c_str(), ImVec2(100 * s, 0))) open_path(g_root / "README.md");

    ImGui::SetCursorPos(ImVec2(pad, pad * 0.7f));
    ImGui::PushFont(F_SMALL);
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float x = p.x;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        for (const char *c = tr(T_KICKER); *c;) {
            int n = (unsigned char)*c >= 0xC0 ? ((unsigned char)*c >= 0xE0 ? 3 : 2) : 1;
            dl->AddText(ImVec2(x, p.y), col(C_CYAN, 0.85f), c, c + n);
            x += ImGui::CalcTextSize(c, c + n).x + 3 * s;
            c += n;
        }
        ImGui::Dummy(ImVec2(x - p.x, ImGui::GetTextLineHeight()));
    }
    ImGui::PopFont();
    ImGui::SetCursorPosX(pad);
    ImGui::PushFont(F_TITLE);
    {
        ImVec2 tp = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        for (int i = 1; i <= 3; i++)   // a soft glow under the title
            for (int k = 0; k < 4; k++)
                dl->AddText(ImVec2(tp.x + (k & 1 ? i : -i) * 1.2f, tp.y + (k & 2 ? i : -i) * 1.2f), col(C_CYAN, 0.06f),
                            "AGE OF CALAMITY");
    }
    ImGui::TextColored(C_TEXT, "AGE OF CALAMITY");
    ImGui::PopFont();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 q = ImGui::GetCursorScreenPos();
    float w = io.DisplaySize.x - 2 * pad;
    q.x = pad, q.y += 4 * s;
    dl->AddRectFilledMultiColor(q, ImVec2(q.x + w, q.y + 2), col(C_CYAN, 0.9f), col(C_ORANGE, 0.0f), col(C_ORANGE, 0.0f),
                                col(C_CYAN, 0.9f));
    for (int i = 0; i < 3; i++) {   // small diamonds on the line
        ImVec2 d(q.x + 60 + i * 18, q.y + 1);
        dl->AddQuadFilled(ImVec2(d.x, d.y - 5), ImVec2(d.x + 5, d.y), ImVec2(d.x, d.y + 5), ImVec2(d.x - 5, d.y),
                          col(i == 1 ? C_ORANGE : C_CYAN));
    }
    ImGui::Dummy(ImVec2(0, 18 * s));
}

// Where problems are reported: the repository's issue form (.github/ISSUE_TEMPLATE/problem.yml). "Report a problem"
// makes aoc-report.zip (tools/make_report.py), opens its folder and this page, where the zip is attached.
static const char ISSUES_URL[] = "https://github.com/eliasm4r/AgeOfCalamityRecomp/issues/new?template=problem.yml";

// The bottom bar's small buttons, all of one height.
static bool bar_button(Str s, const char *id) {
    return ImGui::Button(lab(s, id).c_str(), ImVec2(0, 44 * g_scale));
}

static void report_status() {
    static int shown = 0;
    int st = g_job_state.load();
    if (st == shown) return;
    shown = st;
    if (st == 1) set_status(tr(T_REPORTING), C_MUTED);
    else if (st == 2) {   // the folder with the zip, and the form it is attached to
        set_status(tr(T_REPORT_DONE), C_OK);
        open_path(g_root);
        open_url(ISSUES_URL);
    }
    else if (st == 3) set_status(tr(T_REPORT_FAIL), C_RED);
}

static void screen_play(ImGuiIO &io, float pad, float t, bool &quit) {
    float s = g_scale;
    float total_w = io.DisplaySize.x - 2 * pad, gap = 16 * s;
    float bar_h = 44 * s + 26 * s;
    float half = (total_w - gap) * 0.5f;

    ImGui::SetCursorPosX(pad);
    ImGui::PushFont(F_HEAD);
    ImGui::TextColored(C_OK, "%s", tr(T_READY));
    ImGui::PopFont();
    ImGui::SameLine(0, 14 * s);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(C_MUTED, tr(T_READY_FOR), package_version().c_str());
    ImGui::Dummy(ImVec2(0, 4 * s));
    float body_h = io.DisplaySize.y - ImGui::GetCursorPosY() - bar_h - 12 * s;

    ImGui::SetCursorPosX(pad);
    panel_begin("gfx", tr(T_GRAPHICS), ImVec2(half, body_h));
    opt_table("##gfx", GFX, g_vals_gfx);
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6 * s, 4 * s));
    if (ImGui::BeginTable("##fps", 2, ImGuiTableFlags_SizingStretchProp)) {   // the frame rate, as information
        ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthStretch, 0.46f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch, 0.54f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(C_TEXT, "%s", tr(T_FRAMERATE));
        tooltip(tr(T_FRAMERATE_DESC));
        ImGui::PushFont(F_SMALL);
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(C_MUTED, "%s", first_sentence(tr(T_FRAMERATE_DESC)).c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::TableSetColumnIndex(1);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(C_ORANGE, "%s", tr(T_FRAMERATE_VAL));
        tooltip(tr(T_FRAMERATE_DESC));
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetTextLineHeightWithSpacing() * 2.2f);
    ImGui::PushFont(F_SMALL);
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(C_MUTED, "%s", tr(T_GFX_NOTE));
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    panel_end();

    ImGui::SameLine(0, gap);
    panel_begin("ctl", tr(T_CONTROLS), ImVec2(half, body_h));
    opt_table("##ctl", CTL, g_vals_ctl);
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetTextLineHeightWithSpacing() * 2.2f);
    ImGui::PushFont(F_SMALL);
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(C_MUTED, "%s", tr(T_CTL_NOTE));
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    panel_end();

    // ---- bottom bar: Report, game folder, rebuild, save, close ... PLAY
    ImGui::SetCursorPos(ImVec2(pad, io.DisplaySize.y - bar_h));
    ImGui::BeginDisabled(g_job_state == 1);
    if (bar_button(T_REPORT, "report")) start_report();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (bar_button(T_GAME_FOLDER, "gfolder")) open_path(game_dir());
    ImGui::SameLine();
    if (bar_button(T_FORGE_AGAIN, "again")) g_screen = SCR_BUILD;
    ImGui::SameLine();
    bool dirty = ini_dirty();
    ImGui::BeginDisabled(!dirty);
    if (bar_button(T_SAVE, "save")) set_status(tr(ini_save() ? T_SAVED : T_SAVE_FAIL), ini_dirty() ? C_RED : C_OK);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (bar_button(T_CLOSE, "close")) quit = true;
    float play_w = 230 * s;
    bool running = game_running();
    ImGui::SameLine(io.DisplaySize.x - pad - play_w);
    if (big_button("play", tr(running ? T_PLAYING : T_PLAY), ImVec2(play_w, 44 * s), !running, t)) {
        if (dirty && !ini_save()) set_status(tr(T_SAVE_FAIL), C_RED);
        else if (!play()) set_status(tr(T_PLAY_FAIL), C_RED);
    }
    // the message line under the bar
    ImGui::SetCursorPos(ImVec2(pad, io.DisplaySize.y - 22 * s));
    ImGui::PushFont(F_SMALL);
    if (!g_status.empty() && now_s() - g_status_t < 8) ImGui::TextColored(g_status_col, "%s", g_status.c_str());
    else if (dirty) ImGui::TextColored(C_ORANGE, "%s", tr(T_UNSAVED));
    else ImGui::TextColored(C_MUTED, "%s", tr(T_FOOTER));
    ImGui::PopFont();
}

// ---- the setup wizard: 1 game files, 2 platforms, 3 build. A run in progress (or finished) keeps it on step 3.
static int g_step;   // 0, 1, 2

// The step bar: three numbered circles joined by a line, the current one lit; a done step can be clicked to go back.
static void step_bar(float w, bool locked) {
    float s = g_scale;
    const Str names[3] = {T_STEP1, T_STEP2, T_STEP3};
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float seg = w / 3, r = 13 * s, cy = p.y + r + 2 * s;
    for (int i = 0; i < 3; i++) {
        float cx = p.x + seg * i + r + 2 * s;
        if (i < 2) dl->AddLine(ImVec2(cx + r + 6 * s, cy), ImVec2(p.x + seg * (i + 1) - 6 * s, cy),
                               col(i < g_step ? C_CYAN : C_MUTED, i < g_step ? 0.8f : 0.3f), 2);
        bool cur = i == g_step, done = i < g_step;
        if (cur) dl->AddCircleFilled(ImVec2(cx, cy), r, col(C_CYAN, 0.9f));
        else dl->AddCircle(ImVec2(cx, cy), r, col(done ? C_CYAN : C_MUTED, done ? 0.9f : 0.5f), 0, 2);
        char n[2] = {(char)('1' + i), 0};
        ImVec2 ts = ImGui::CalcTextSize(n);
        dl->AddText(ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f), col(cur ? C_BG0 : done ? C_CYAN : C_MUTED), n);
        dl->AddText(ImVec2(cx + r + 8 * s, cy - ts.y * 0.5f), col(cur ? C_TEXT : C_MUTED), tr(names[i]));
        ImGui::SetCursorScreenPos(ImVec2(cx - r, cy - r));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("st", ImVec2(r * 2 + 8 * s + ImGui::CalcTextSize(tr(names[i])).x, r * 2)) && done && !locked)
            g_step = i;
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 2 * r + 14 * s));
}

// A platform choice: a checkbox with a muted line of explanation under it.
static void platform_choice(Str label, const char *id, bool *v, Str desc) {
    ImGui::PushFont(F_HEAD);
    ImGui::Checkbox(lab(label, id).c_str(), v);
    ImGui::PopFont();
    ImGui::Indent(34 * g_scale);
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(C_MUTED, "%s", tr(desc));
    ImGui::PopTextWrapPos();
    ImGui::Unindent(34 * g_scale);
    ImGui::Dummy(ImVec2(0, 8 * g_scale));
}

static void screen_build(ImGuiIO &io, float pad, float t, DumpScan &scan, char *path_buf, size_t path_n, char *host_buf,
                         size_t host_n, char *work_buf, size_t work_n, char *distro_buf, size_t distro_n,
                         bool &autoscroll, bool &quit, SDL_Window *win) {
    float s = g_scale;
    bool running = g_running.load() != 0;
    float total_w = io.DisplaySize.x - 2 * pad;
    float bar_h = 44 * s + 26 * s;
    bool files_ok = ((scan.base && scan.update) || scan.nsp >= 2) && scan.keys;
    bool target_ok = S.deck || S.windows;
    bool started;
    {
        std::lock_guard<std::mutex> g(g_lock);
        started = !g_steps.empty();
    }
    if (running || started) g_step = 2;   // a run shows its progress until the next one

    bool update = game_ready() && forged_version() != package_version();
    ImGui::SetCursorPosX(pad);
    if (update) ImGui::TextColored(C_ORANGE, "%s", tr(T_UPDATE_NOTE));
    ImGui::SetCursorPosX(pad + 6 * s);
    step_bar(total_w - 12 * s, running);
    float body_h = io.DisplaySize.y - ImGui::GetCursorPosY() - bar_h - 12 * s;
    float nav_h = 44 * s;

    ImGui::SetCursorPosX(pad);
    if (g_step == 0) {
        // ---- step 1: the folder and what was found in it
        panel_begin("s1", tr(T_S1_TITLE), ImVec2(total_w, body_h));
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(C_TEXT, "%s", tr(T_S1_HINT));
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 4 * s));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120 * s);
        ImGui::InputText("##dir", path_buf, path_n);
        ImGui::SameLine();
        if (ImGui::Button(lab(T_BROWSE, "browse").c_str(), ImVec2(110 * s, 0)))
            SDL_ShowOpenFolderDialog(on_folder, nullptr, win, S.game_dir.empty() ? nullptr : S.game_dir.c_str(), false);
        ImGui::Dummy(ImVec2(0, 10 * s));
        if (!scan.exists) {
            status_dot(false, tr(T_NO_FOLDER));
        } else {
            // The names are only a hint: with two or more NSPs whose names carry no title id, the build's check step
            // identifies the base and the update by their contents (tools/extract.py inventory).
            bool unnamed = scan.nsp >= 2;
            status_dot(scan.base || unnamed, tr(T_BASE), scan.base ? "1.0.0" : tr(unnamed ? T_UNNAMED : T_MISSING));
            ImGui::Dummy(ImVec2(0, 2 * s));
            status_dot(scan.update || unnamed, tr(T_UPDATE), scan.update ? "" : tr(unnamed ? T_UNNAMED : T_MISSING));
            ImGui::Dummy(ImVec2(0, 2 * s));
            char d[64];
            snprintf(d, sizeof d, tr(T_DLC_N), scan.dlc);
            std::string dl = std::string(d) + " (" + tr(T_OPTIONAL) + ")";
            status_dot(scan.dlc > 0, tr(T_DLC), dl.c_str());
            ImGui::Dummy(ImVec2(0, 2 * s));
            status_dot(scan.keys, tr(T_KEYS), scan.keys ? tr(T_KEYS_OK) : tr(T_MISSING));
        }
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - nav_h - 30 * s);
        ImGui::PushFont(F_SMALL);
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - 150 * s);
        ImGui::TextColored(files_ok ? C_MUTED : C_ORANGE, "%s", tr(files_ok ? T_FOOTER : T_NEED_FILES));
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 18 * s - 140 * s, ImGui::GetWindowHeight() - nav_h - 14 * s));
        ImGui::BeginDisabled(!files_ok);
        if (ImGui::Button(lab(T_NEXT, "next1").c_str(), ImVec2(140 * s, nav_h))) g_step = 1;
        ImGui::EndDisabled();
        panel_end();
    } else if (g_step == 1) {
        // ---- step 2: where the game will run
        panel_begin("s2", tr(T_S2_TITLE), ImVec2(total_w, body_h));
        ImGui::TextColored(C_MUTED, "%s", tr(T_S2_HINT));
        ImGui::Dummy(ImVec2(0, 6 * s));
        if (IS_WINDOWS) platform_choice(T_WINDOWS, "windows", &S.windows, T_WIN_DESC);
        platform_choice(IS_WINDOWS ? T_DECK : T_DECK_LINUX, "deck", &S.deck, IS_WINDOWS ? T_DECK_DESC : T_DECK_DESC_LINUX);
        if (S.deck) {
            ImGui::Indent(34 * s);
            ImGui::Checkbox(lab(T_ROMFS, "romfs").c_str(), &S.with_romfs);
            ImGui::Checkbox(lab(T_SEND, "send").c_str(), &S.send);
            if (S.send) {
                ImGui::SetNextItemWidth(260 * s);
                ImGui::InputText(lab(T_HOST, "host").c_str(), host_buf, host_n);
            }
            ImGui::Unindent(34 * s);
            ImGui::Dummy(ImVec2(0, 6 * s));
        }
        if (IS_WINDOWS) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
            if (ImGui::CollapsingHeader(lab(T_ADVANCED, "adv").c_str())) {
                ImGui::SetNextItemWidth(260 * s);
                ImGui::InputTextWithHint(lab(T_DISTRO, "distro").c_str(), tr(T_DISTRO_HINT), distro_buf, distro_n);
                ImGui::SetNextItemWidth(260 * s);
                ImGui::InputTextWithHint(lab(T_WORK, "work").c_str(), tr(T_WORK_HINT), work_buf, work_n);
            }
            ImGui::PopStyleColor();
        }
        if (!target_ok) {
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - nav_h - 30 * s);
            ImGui::TextColored(C_ORANGE, "%s", tr(T_NEED_TARGET));
        }
        ImGui::SetCursorPos(ImVec2(18 * s, ImGui::GetWindowHeight() - nav_h - 14 * s));
        if (ImGui::Button(lab(T_BACK, "back2").c_str(), ImVec2(140 * s, nav_h))) g_step = 0;
        ImGui::SameLine(ImGui::GetWindowWidth() - 18 * s - 140 * s);
        ImGui::BeginDisabled(!target_ok);
        if (ImGui::Button(lab(T_NEXT, "next2").c_str(), ImVec2(140 * s, nav_h))) settings_save(), g_step = 2;
        ImGui::EndDisabled();
        panel_end();
    } else {
        // ---- step 3: what will be built, the button, progress, and the log behind "Technical details"
        panel_begin("s3", tr(T_S3_TITLE), ImVec2(total_w, body_h));
        std::string targets;
        if (IS_WINDOWS && S.windows) targets = tr(T_WINDOWS);
        if (S.deck) targets += (targets.empty() ? "" : "  +  ") + std::string(tr(IS_WINDOWS ? T_DECK : T_DECK_LINUX));
        ImGui::TextColored(C_MUTED, "%s", tr(T_S3_FOR));
        ImGui::SameLine(0, 0);
        ImGui::TextColored(C_TEXT, "%s", targets.c_str());
        ImGui::PushFont(F_SMALL);
        ImGui::TextColored(C_MUTED, "%s", tr(T_HOW4));
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 4 * s));
        {
            std::lock_guard<std::mutex> g(g_lock);
            int done = 0;
            for (auto &st : g_steps) done += st.state == "ok" || st.state == "skip";
            float frac = g_steps.empty() ? 0.0f : (float)done / g_steps.size();
            ImDrawList *dl = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x, h = 8 * s;
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col(C_CYAN, 0.10f), 3);
            if (frac > 0) dl->AddRectFilledMultiColor(p, ImVec2(p.x + w * frac, p.y + h), col(C_CYAN_DIM), col(C_CYAN),
                                                      col(C_CYAN), col(C_CYAN_DIM));
            if (g_running) {
                float x = p.x + fmodf(t * 160 * s, w);
                dl->AddRectFilled(ImVec2(x, p.y), ImVec2(fminf(x + 40 * s, p.x + w), p.y + h), col(C_ORANGE, 0.5f), 3);
            }
            ImGui::Dummy(ImVec2(w, h + 8 * s));
            for (auto &st : g_steps) {
                step_marker(st.state, t);
                ImGui::SameLine();
                ImGui::TextColored(st.state == "pending" ? C_MUTED : C_TEXT, "%s", st.title.c_str());
                if (st.t0 > 0) {
                    double el = (st.t1 > 0 ? st.t1 : now_s()) - st.t0;
                    ImGui::SameLine();
                    if (st.state == "skip") ImGui::TextColored(C_MUTED, "%s", tr(T_DONE_SKIP));
                    else ImGui::TextColored(C_MUTED, "%dm%02ds", (int)el / 60, (int)el % 60);
                }
                if (!st.note.empty()) {
                    ImGui::PushFont(F_SMALL);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 34 * s);
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextColored(C_RED, "%s", st.note.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::PopFont();
                }
            }
            if (g_done == "ok") {
                Str msg = g_check_only ? T_CHECKED : (S.deck && !(IS_WINDOWS && S.windows) && IS_WINDOWS) ? T_FORGED_DECK : T_FORGED;
                ImGui::TextColored(C_OK, "%s", tr(msg));
            } else if (g_done == "fail") {
                ImGui::TextColored(C_RED, "%s", tr(T_FAILED));
            } else if (g_done == "cancel") {
                ImGui::TextColored(C_ORANGE, "%s", tr(T_CANCELED));
            }
#ifdef _WIN32
            // WSL missing: offer the elevated install
            if (g_need_wsl || g_wsl_inst) {
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::PushTextWrapPos(0);
                if (g_wsl_inst == 2) {
                    ImGui::TextColored(C_OK, "%s", tr(T_WSL_DONE));
                } else {
                    ImGui::TextColored(C_ORANGE, "%s", tr(T_NEED_WSL));
                    ImGui::TextColored(C_TEXT, "%s", tr(T_NEED_WSL2));
                    if (g_wsl_inst == 1) {
                        ImGui::TextColored(C_MUTED, "%s", tr(T_WSL_RUNNING));
                    } else {
                        if (g_wsl_inst == 3) ImGui::TextColored(C_RED, tr(T_WSL_FAIL), g_wsl_code.load());
                        if (ImGui::Button(lab(T_INSTALL_WSL, "wslinst").c_str())) install_wsl();
                    }
                }
                ImGui::PopTextWrapPos();
            }
#endif
            // the raw log, folded away unless asked for (or opened by itself when a run fails)
            ImGui::Dummy(ImVec2(0, 4 * s));
            float log_h = ImGui::GetWindowHeight() - ImGui::GetCursorPosY() - nav_h - 34 * s;
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
            if (g_done == "fail") ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            bool open = ImGui::CollapsingHeader(lab(T_DETAILS, "details").c_str());
            ImGui::PopStyleColor();
            if (open && log_h > 60 * s) {
                ImGui::PushFont(F_SMALL);
                ImGui::Checkbox(lab(T_FOLLOW, "follow").c_str(), &autoscroll);
                ImGui::BeginChild("log", ImVec2(0, log_h - ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders,
                                  ImGuiWindowFlags_HorizontalScrollbar);
                ImGuiListClipper clip;
                clip.Begin((int)g_log.size());
                while (clip.Step())
                    for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                        const std::string &l = g_log[i];
                        bool err = l.rfind("ERROR", 0) == 0 || l.find("FAILED") != std::string::npos ||
                                   l.find("error:") != std::string::npos;
                        ImGui::TextColored(err ? C_RED : C_MUTED, "%s", l.c_str());
                    }
                if (autoscroll && g_running) ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();
                ImGui::PopFont();
            }
        }
        // navigation: Back, then Check files only + BUILD (or Cancel + BUILDING...)
        float check_w = 170 * s, forge_w = 220 * s;
        ImGui::SetCursorPos(ImVec2(18 * s, ImGui::GetWindowHeight() - nav_h - 14 * s));
        ImGui::BeginDisabled(running);
        if (ImGui::Button(lab(T_BACK, "back3").c_str(), ImVec2(140 * s, nav_h))) {
            std::lock_guard<std::mutex> g(g_lock);
            g_steps.clear(), g_done.clear();   // back to the choices: the last run's list goes away
            g_step = 1;
        }
        ImGui::EndDisabled();
        ImGui::SameLine(ImGui::GetWindowWidth() - 18 * s - forge_w - check_w - 10 * s);
        if (!running) {
            ImGui::BeginDisabled(!scan.exists || !target_ok);
            if (ImGui::Button(lab(T_CHECK, "check").c_str(), ImVec2(check_w, nav_h))) start_build(true);
            ImGui::EndDisabled();
            ImGui::SameLine(0, 10 * s);
            if (big_button("forge", tr(T_FORGE), ImVec2(forge_w, nav_h), files_ok && target_ok && !g_root.empty(), t)) {
                settings_save();
                start_build(false);
            }
        } else {
            if (ImGui::Button(lab(T_CANCEL, "cancel").c_str(), ImVec2(check_w, nav_h))) cancel_build();
            ImGui::SameLine(0, 10 * s);
            big_button("forge", tr(T_FORGING), ImVec2(forge_w, nav_h), false, t);
        }
        panel_end();
    }

    // ---- bottom bar: Report, logs, Deck folder, back to the game, close
    ImGui::SetCursorPos(ImVec2(pad, io.DisplaySize.y - bar_h));
    ImGui::BeginDisabled(g_job_state == 1);
    if (bar_button(T_REPORT, "report")) start_report();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (bar_button(T_LOGS_FOLDER, "logs")) {
        std::error_code ec;
        fs::create_directories(g_root / "build" / "logs", ec);
        open_path(g_root / "build" / "logs");
    }
    ImGui::SameLine();
    std::error_code ec;
    fs::path deck_dir = g_root / "dist" / "aoc-deck";
    if (IS_WINDOWS && fs::exists(deck_dir / "aoc", ec)) {
        if (bar_button(T_OPEN_DECK, "odeck")) open_path(deck_dir);
        ImGui::SameLine();
    }
    if (game_ready() && !running && !update) {
        if (bar_button(T_BACK_TO_GAME, "back")) g_screen = SCR_PLAY;
        ImGui::SameLine();
    }
    if (bar_button(T_CLOSE, "close")) quit = true;
    ImGui::SetCursorPos(ImVec2(pad, io.DisplaySize.y - 22 * s));
    ImGui::PushFont(F_SMALL);
    if (!g_status.empty() && now_s() - g_status_t < 8) ImGui::TextColored(g_status_col, "%s", g_status.c_str());
    else ImGui::TextColored(C_MUTED, "%s", tr(T_S1_NOTE));
    ImGui::PopFont();
}

int main(int argc, char **argv) {
    g_exe_dir = exe_dir();
    g_root = find_root(g_exe_dir);
    settings_load();
    if (S.game_dir.empty() && !g_root.empty()) S.game_dir = u8(g_root / "Game Files");
    if (!IS_WINDOWS) S.windows = false;

    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0) scale = 1;
    g_scale = scale;
    SDL_Window *win = SDL_CreateWindow("Age of Calamity Recomp", (int)(1100 * scale), (int)(800 * scale),
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_SetWindowMinimumSize(win, (int)(960 * scale), (int)(720 * scale));
    SDL_Renderer *ren = SDL_CreateRenderer(win, nullptr);
    SDL_SetRenderVSync(ren, 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    fs::path font = g_root / "runtime" / "ui" / "fonts" / "Roboto-Medium.ttf";
    std::string fp = u8(font);
    std::error_code ec;
    if (fs::is_regular_file(font, ec)) {
        F_BODY = io.Fonts->AddFontFromFileTTF(fp.c_str(), 17 * scale);
        F_SMALL = io.Fonts->AddFontFromFileTTF(fp.c_str(), 14 * scale);
        F_HEAD = io.Fonts->AddFontFromFileTTF(fp.c_str(), 19 * scale);
        F_TITLE = io.Fonts->AddFontFromFileTTF(fp.c_str(), 44 * scale);
    }
    if (!F_BODY) F_BODY = F_SMALL = F_HEAD = F_TITLE = io.Fonts->AddFontDefault();
    apply_theme();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);

    DumpScan scan = scan_dump(S.game_dir);
    std::string scanned_dir = S.game_dir;
    char path_buf[1024], host_buf[256], work_buf[512], distro_buf[64];
    snprintf(path_buf, sizeof path_buf, "%s", S.game_dir.c_str());
    snprintf(host_buf, sizeof host_buf, "%s", S.deck_host.c_str());
    snprintf(work_buf, sizeof work_buf, "%s", S.win_work.c_str());
    snprintf(distro_buf, sizeof distro_buf, "%s", S.wsl_distro.c_str());
    bool autoscroll = true, quit = false;
    // The play screen once the game for this PC is forged from this version; else the build screen.
    g_screen = game_ready() && forged_version() == package_version() ? SCR_PLAY : SCR_BUILD;
    if (game_ready()) ini_load();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--check-now")) g_screen = SCR_BUILD, start_build(true);
        if (!strcmp(argv[i], "--build")) g_screen = SCR_BUILD;
    }
    double t_start = now_s();
    std::string last_done;

    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) quit = true;
        }
        {
            std::lock_guard<std::mutex> g(g_pick_lock);
            if (!g_picked.empty()) {
                snprintf(path_buf, sizeof path_buf, "%s", g_picked.c_str());
                g_picked.clear();
            }
        }
        S.game_dir = path_buf, S.deck_host = host_buf, S.win_work = work_buf, S.wsl_distro = distro_buf;
        if (S.game_dir != scanned_dir) scan = scan_dump(S.game_dir), scanned_dir = S.game_dir;
        // a forge that ends well with the game for this PC turns the window into the launcher
        {
            std::string d;
            {
                std::lock_guard<std::mutex> g(g_lock);
                d = g_done;
            }
            if (d != last_done) {
                last_done = d;
                if (d == "ok" && !g_check_only && game_ready() && !g_running) {
                    ini_load();
                    g_screen = SCR_PLAY;
                    set_status(tr(T_FORGED), C_OK);
                }
            }
        }
        report_status();

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        float t = (float)(now_s() - t_start);
        draw_background(t);

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PushFont(F_BODY);
        float pad = 26 * scale;
        draw_header(io, pad);
        if (g_screen == SCR_PLAY && game_ready())
            screen_play(io, pad, t, quit);
        else
            screen_build(io, pad, t, scan, path_buf, sizeof path_buf, host_buf, sizeof host_buf, work_buf,
                         sizeof work_buf, distro_buf, sizeof distro_buf, autoscroll, quit, win);
        ImGui::PopFont();
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderScale(ren, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColorFloat(ren, C_BG0.x, C_BG0.y, C_BG0.z, 1);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        SDL_RenderPresent(ren);
    }
    settings_save();
    if (g_running) cancel_build();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
