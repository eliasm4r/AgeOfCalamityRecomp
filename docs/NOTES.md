# Technical notes

How the port is built, where each part of the code lives, and the numbers measured on the game. Addresses are
`main+0x...` offsets for update 1.3.0 with the build IDs listed in `tools/data/known-builds.json`; every one is
checked against the original bytes of the dump before it is used.

## Architecture

How the Switch build of *Age of Calamity* 1.3.0 becomes a native x86-64 program for Linux (Steam Deck) and Windows.

The CPU, host, Vulkan and Linux half comes from the Astral Chain Recomp and TotK Recomp pipeline; the game-specific
half comes from nxnative's and MacNativeGames' Age of Calamity profiles.

```
 player's dump (base + 1.3.0 + DLC, prod.keys)       never in git
        |  tools/extract.py (hactool + BKTR)
        v
 ExeFS (main, subsdk0, sdk, main.npdm)   RomFS (RDB/IDRK)
        |                                   |
        | recomp/ (ARM64 to C lifter)       | shader extraction (G2S/_DOK/main)
        v                                   v
 generated/*.c  --clang/lld-->  AgeOfCalamityRecomp (executable)     SPIR-V (Ryujinx translator, separate process)
                                   |
             runtime/: kernel, IPC, nn::*, audio, RomFS, NVN front end, Vulkan back end, SDL3
```

### 1. Input

- Base 1.0.0, update 1.3.0 (v327680) and DLCs as NSP/XCI, with the keys of the player's own console.
- Extraction and update merge (BKTR) with hactool.
- Verification: the build IDs of the three modules against `tools/data/known-builds.json`. Only **1.3.0** is accepted;
  any other build is refused before code is generated.

### 2. CPU: ARM64 to C

- One C function per guest function; `BL` becomes a direct call, indirect jumps go through `rt_dispatch` (a table by
  address, `musttail`).
- Modules: `main`, `subsdk0`, `sdk`. `rtld` is dropped; the host does the dynamic linking.
- Byte jump tables indexed by `x30` (pattern at `main+0x138216c`).
- Game patches are **lifter patches** in `recomp/patches-v130.txt`, with their original bytes checked, never edits of
  the generated C. `recomp/layout.py` is the only place that decides module bases.
- Tests: differential tests against Unicorn (in tests only, never linked) for each instruction class, whole functions
  and jump tables.
- Memory: guest address = host address, below 2^39; the application pool is about 3.2 GB.

### 3. Host (a stand-in Horizon OS)

- Each guest thread is a host thread, on **3 virtual cores** with affinity and priority preemption (the KTGL has a
  `KIDSTaskScheduler` with threads per core), a spin-wait gate and asynchronous preemption.
- The SDK boundary is the imported `nn::*` functions; real IPC services only where the SDK calls in (`audren:u`, `sm:`,
  `fsp-srv` as a CMIF domain, ...).
- Game specifics: the audio renderer work buffer has the service's size; `oe` copyright framebuffer calls do nothing;
  `HardwareOpusMultiStreamDecoder` becomes a libopus software decoder; videos are VP9, decoded by libvpx.
- Platform: `runtime/platform/` with `linux.c` and `win32.c`; window, input and audio through **SDL3**.

### 4. Graphics: NVN to Vulkan 1.3

- NVN front end: compute program binding, `nvnTexturePoolRegisterImage`, GPU-read indirect draws and dispatches
  (indirect quads become indexed draws through a compute pass), texture view cache by texture serial, uploads at
  `FlushMappedRange` for video textures.
- Vulkan back end: replay at `QueueSubmitCommands`, VMA, `VK_EXT_graphics_pipeline_library`, uniform snapshots, a
  recording thread, FSR 1. BC1-BC7 textures upload as they are (the game uses no ASTC).
- Shaders: the Ryujinx translator (MIT) runs as a separate process (`xlate`); translations and pipelines are cached
  on disk.
- Optional NVIDIA DLSS/DLAA through the driver's NGX core (`vk_dlaa.c`, Windows only, needs `nvngx_dlss.dll`).

### 5. Frame pacing

- Default and only mode today: the game as on the Switch, **30 fps** (present interval 2, clocks untouched).
- 60 fps needs the three frame-step clocks patched together: the KIDS task, the battle frame-rate block at
  `main+0x269e150` and the KTGL device configuration. Changing only the present interval doubles the game speed.
- 40 fps is not trivial: the game computes "vsyncs per update" as an integer of 60 / cap.

### 6. Toolchain

- Clang + LLD (the generated C uses `__int128`, `musttail` and vector types), Ninja, Python, .NET (translator), Vulkan
  headers, SDL3, libopus, libvpx, zstd, VMA.
- Build on the PC, play anywhere: WSL2 or Linux builds the Deck folder, MSYS2 builds the Windows one.

### 7. Content rules

- Nothing from the game goes into git: dump, keys, extracted ExeFS/RomFS, generated C, translated shaders, caches,
  screenshots, saves. All of it lives in `build/`, `generated/` and `dist/` (ignored).
- Every file the generator writes carries `GENERATED_MARKER` (`tools/check_content.py`).
- `tools/check_content.py --staged` runs before each commit (hook in `.githooks/`) and in CI.
- Copied code only from MIT, BSD, ISC, zlib or Apache-2.0 projects, with its header kept and a line in
  `THIRD_PARTY_NOTICES.md`.

## Source map

| Folder | What |
|---|---|
| `recomp/` | ARM64 to C lifter (from Astral Chain Recomp, itself from LAS Recomp): `gen.py` generates the C of the three modules, `layout.py` decides module bases, `patches-v130.txt` holds the game patches and spin waits |
| `runtime/` | the host "Horizon": loader, dispatch, memory, kernel, IPC, `nn::*` overrides, audio, RomFS, DLCs, saves, NVN front end, Vulkan back end, input, window, ImGui menu |
| `setup/` | the setup (tkinter): runs `tools/make_build.py` (install, or update over an installed game), then starts the game; settings are in the game's F1 menu |
| `assets/` | the key art and the icon cut from it (`tools/make_icon.py`) |
| `tools/` | extraction, RomFS merge, build, release, reports, censuses, Deck tools (`tools/deck/`) |
| `tests/` | differential lifter tests against Unicorn and tool tests |

| Runtime area | Files |
|---|---|
| Loader, dispatch, memory | `loader.c`, `dispatch.c`, `memory.c`, `hostfn.c`, `boot.c`, `main.c` |
| Kernel (3 virtual cores, SVCs) | `kernel.c`, `svc.c` |
| IPC and services | `ipc.c` (CMIF, TIPC, domains), `ov_*.c` (`nn::*` overrides and services: `fsp-srv`, `hid`, `hwopus`, `audren:u`...) |
| Audio | `audren.c`, `audio_*.c` (SDL3) |
| RomFS, DLCs, saves | `romfs.c`, `dlc.c`, `fs_host.c` |
| NVN front end | `nvn_api_*.c`, `nvn_obj.h`, `nvn_tex.c` |
| Vulkan back end | `vk_*.c`, `gfx_*.c` |
| Input, window, menu | `input*.c`, `gfx_window_*.c`, `ui/` (Dear ImGui) |
| Platform | `platform/linux.c`, `platform/win32.c`, `platform/platform.h` |
| CPU of the generated code | `cpu.h`, `simd.h`, `simd_fp.h`, `fma.c` |

Settings are `ACR_*` environment variables backed by `settings.ini` (`cfg.c` holds the table of keys, defaults and ranges).
The shader translator is a vendored, unmodified subset of Ryujinx 1.3.2 in `tools/vendor/ryujinx/` (MIT).

## Steam Deck

Target: Steam Deck (LCD or OLED), SteamOS 3.8 or newer. The Deck compiles nothing: it receives the ready folder
`dist/steamdeck` built on a PC ([CONTRIBUTING.md](../CONTRIBUTING.md)).

### Requirements

| Requirement | Why | Deck |
|---|---|---|
| x86-64-v3 (AVX2/FMA) | default build of the generated C | Zen 2: yes |
| Linux 6.7 or newer | write watching with `userfaultfd` WP + `PAGEMAP_SCAN` | SteamOS 3.8 ships kernel 6.16 |
| Vulkan 1.3 + `VK_EXT_external_memory_host` | importing the game's memory pools | RADV: yes |
| `VK_EXT_graphics_pipeline_library` | building pipelines without stutter | RADV: yes |
| glibc 2.29 or newer | built against the Steam Runtime 3 sniper sysroot | yes |

### The folder

```
steamdeck/
  AgeOfCalamityRecomp                    Linux x86-64 executable (recompiled game code + runtime)
  AgeOfCalamityRecomp.sh                 starter: Mesa shader cache next to the game, creates logs/, keeps the last 60 sessions
  logs.sh            logs: list, summary, live, system, detailed on|off, bundle
  lib/libSDL3.so.0       SDL 3.4 from the sniper sysroot (the executable looks in $ORIGIN/lib first)
  exefs/                 main, subsdk0, sdk, main.npdm
  romfs-index.bin        RomFS index (checked against romfs.bin)
  romfs/romfs.bin        merged RomFS 1.3.0 (10.9 GiB); `dist --with-romfs` adds it as a hard link
  romfs/dlc-<id>.bin     RomFS of each DLC (1.41 GiB for the four), optional
  README.txt             install, Steam shortcut and controls
  settings.ini                created the first time only; the player's afterwards
  profile/               saves and local account, created next to the executable
```

- The executable needs **GLIBC_2.29** at most; Vulkan, audio, X11/Wayland and the libraries SDL3 uses come from
  SteamOS. Opus, libvpx and zstd are built in.
- On the Deck: Desktop Mode, copy the folder, run `sh install-shortcut.sh` once, then add the game as a non-Steam game
  and play in Game Mode.
- Updating: run `dist` again and copy `AgeOfCalamityRecomp` (the rest only changes with the game); saves and `settings.ini` stay.
- The runtime has a "deck" profile (`cfg.c`, `ACR_DEVICE_PROFILE=auto|desktop|deck`, detected through DMI): full screen
  1280x800, vsync, **30 fps** and handheld mode, the game's own lighter render resolution.
- Copying over FTP from Windows loses the executable bit: `chmod +x AgeOfCalamityRecomp AgeOfCalamityRecomp.sh` once (`AgeOfCalamityRecomp.sh` also tries by itself).

### Quick menu (hold L3+R3)

Holding L3+R3 for about 1.5 s (`ACR_QUICK_HOLD_MS`) opens a small panel; a short L3+R3 press toggles the FPS overlay,
and Minus+Plus opens the full menu. B closes it.

| Row | Values | Note |
|---|---|---|
| Frame rate | **30** (default); 60 shows greyed out | the game moves a fixed step per frame, so it runs at 30 until the 60 fps patch exists |
| Console mode | **Handheld** (Deck default) / TV | the Switch operation mode; switched live, like docking a Switch |
| Performance overlay | off / FPS / full | same as a short L3+R3 press |

For less power use, Steam's own performance menu limits TDP and refresh rate.

The finished folder contains the game (RomFS and derived code). It is for your own use only: do not share it.

### Where the frame time goes

Measured in the first battle: 24.3-24.6 fps, CPU bound. The game thread takes about 27.6 ms per frame, and syncing the
shadow copies of the CPU-visible memory pools before each submit costs about 30 ms per frame
([below](#steam-deck-1)). That sync is the next target.

### Controls

- Steam Input exposes an Xbox-style controller through SDL3; a Nintendo button layout is available in the menu.
- Camera on the right stick.
- Keyboard and mouse in Desktop Mode and on PC.

## Measured facts

### Input

Six NSPs, about 12.5 GiB. All PFS0 containers are intact and every NCA header decrypts as `NCA3`.

| Package | Title ID | Version | Size | NCAs |
|---|---|---|---|---|
| Base | `01002B00111A2000` | v0 (1.0.0) | 10.65 GiB | Program 10,903 MiB, Control, Manual, Meta |
| Update | `01002B00111A2800` | v327680 (1.3.0) | 0.43 GiB | Program 434 MiB (BKTR patch over the base), Control, Manual, Meta |
| DLC: digital pre-order bonus | `01002B00111A3001` | v0 | 0.15 MiB | PublicData 0.1 MiB (licence only), Meta |
| DLC: Expansion Pass purchase bonus | `01002B00111A3002` | v0 | 88 MiB | PublicData 87.7 MiB, Meta |
| DLC: *Pulse of the Ancients* | `01002B00111A3003` | v0 | 218 MiB | PublicData 218.2 MiB, Meta |
| DLC: *Guardian of Remembrance* | `01002B00111A3004` | v0 | 1.11 GiB | PublicData 1,136.8 MiB, Meta |

- Content SDK **9.6.0** in every program and data NCA; key generation **0x0B** everywhere.
- Program and data NCAs use rights-ID crypto; each NSP carries its own ticket, so `title.keys` is not needed.

### ExeFS 1.3.0

| Module | Build ID (prefix) | File | In memory | text / ro / data / bss |
|---|---|---|---|---|
| `main` | `c3cf52bf2b05d7310884971b50bcccb8` | 17.1 MiB | 43.7 MiB | 27.3 / 10.9 / 1.2 / 4.2 MiB |
| `subsdk0` | `3ab92c3444d4ece2f52417b5d8ee86eb` | 3.4 MiB | 7.1 MiB | 3.4 / 2.3 / 0.19 / 1.1 MiB |
| `sdk` | `95832210b36121e88b4f9b7b66239dad` | 5.1 MiB | 12.6 MiB | 5.3 / 6.3 / 0.38 / 0.66 MiB |

- The three build IDs match nxnative's 1.3.0 profile, so its `main+0x...` addresses apply (original bytes are still
  checked before each one is used).
- NPDM: main thread priority 44, core 0, 1 MiB stack, **system resource 0** (the game uses `SetHeapSize`).

### Censuses

| Census | Result |
|---|---|
| RomFS 1.0.0 | 1,086 files, 10.60 GiB: RDB data 8,280 MiB, `.file` 1,290 MiB, `.webm` videos 1,246 MiB |
| Merged RomFS 1.3.0 (`build/v130/romfs.bin`) | **1,101 files, 10.91 GiB**, six IVFC levels verified; 18 new files, 3 removed, 120 resized |
| RomFS file types | RDB archives (`.rdb`, `.rdb.bin*`, `asset/data/*.file`) and `.webm`; **no `.zs` file at all** (no `HeapDef.byml.zs`, no `PlayerCamera.pack.zs`: those are Tears of the Kingdom files) |
| DLCs | 3001: 1 file (licence); 3002: 4 files, 87 MiB; 3003: 15 files, 218 MiB; 3004: 186 files, 1.11 GiB |
| `main` imports | 387: `nn::audio` 88, libc 67, `nn::os` 56, `nn::hid` 40, `nn::fs` 25, `movie` 20, `nn::settings` 16, `nn::oe` 14, `nn::nfp` 14 (amiibo), `nn::time` 8, `nn::vi` 7, ... plus `nvnBootstrapLoader` |
| `subsdk0` imports (video) | 650: libc 275, `android` 208, C++ 102, `nn::os` 25, `nn::audio` 19, `nn::sf` 11 |
| C++ exceptions / `longjmp` | none, so the lifter's callee-saved register optimisation stays on |
| Functions | **101,212**, 0 unsupported instructions |
| Shaders | **40,729 unique** (37,045 pixel, 3,441 vertex, 241 compute, 2 geometry) |
| Texture formats | **no ASTC**: BC7 11,819, BC1 6,110, BC5 1,941, BC3 594, BC4 521, BC6H 191, plus 1x1 uncompressed placeholders |

The game never uses ASTC: everything compressed is BC1-BC7, which the Steam Deck's RDNA2 (and any PC GPU) samples
natively.

#### RDB content (`tools/census_rdb.py`)

| Type (KTID) | Magic | What | Entries | Stored | Real |
|---|---|---|---|---|---|
| `afbec60c` | `GT1G` | G1T textures | 40,198 | 4.4 GiB | 10.3 GiB |
| `563bdef1` | `_M1G` | G1M models | 9,698 | 1.4 GiB | 2.5 GiB |
| `7bcd279f` | `_S2G` | G2S shaders | 23,108 | 0.8 GiB | 2.3 GiB |
| `0d34474d` | `TSRS` | sound (KTSR) | 969 | 1.3 GiB | 1.3 GiB |
| `20a6a0bb` | `_DOK` | engine system resources (151 shaders) | 5,062 | 75 MiB | 618 MiB |
| `bbd39f2d` | `ASRS` | sound | 1,270 | 478 MiB | 525 MiB |
| `6fa91671` | `_A1G`/`_A2G` | animations | 12,209 | 250 MiB | 453 MiB |

#### Shaders (`tools/census_shaders.py`)

| Stage | Unique | Uses | Maxwell code |
|---|---|---|---|
| pixel | 37,045 | 52,854 | 576 MiB |
| vertex | 3,441 | 50,378 | 23 MiB |
| compute | 241 | 262 | 2.8 MiB |
| geometry | 2 | 2 | - |

- The 40,729 keys match nxnative's `shader_map.tsv` one by one, stage included, so its shader tables apply unchanged.
- Control sections: 27,300 of 0x880 bytes, 13,426 of 0x850 and 3 of 0x7d1.
- The DLCs carry **no shader container**: the extra content uses the base game's shaders.

#### How the game mounts the DLCs (`ACR_TRACE=fs`)

- `main` imports four DLC functions: `nn::aoc::CountAddOnContent`, `nn::aoc::ListAddOnContent`,
  `nn::fs::QueryMountAddOnContentCacheSize`, `nn::fs::MountAddOnContent`.
- DLC index = title ID - `01002B00111A3000`. Each RomFS has a 4-byte `DLCDATA.dat` holding its own index.
- About 11 s after start the game mounts each index as `aoc`, reads `DLCDATA.dat` and unmounts it, then keeps the
  three with content mounted as `aoc001`, `aoc002`, `aoc003`.
- Each DLC's data is a suffix of the base RDBs: `asset/<Name>.rdb.bin1_0` in `aoc001`, `.bin2_0` in `aoc002`,
  `.bin3_0` in `aoc003`.

### Lifter (`recomp/`)

| Module | Functions | Jump tables | `br` via dispatch | Traps | Unsupported | Generated C |
|---|---|---|---|---|---|---|
| `main` | 52,412 | 1,706 | 5,140 | 87,701 | 0 | 440 MB |
| `subsdk0` | 13,618 | 335 | 3,970 | 16,189 | 0 | 62 MB |
| `sdk` | 35,182 | 534 | 8,907 | 38,967 | 0 | 97 MB |

Differential tests against Unicorn, on the game's own code:

| Test | Result |
|---|---|
| `tests/flags_check.py` | 2,000,484 operand pairs, 0 mismatches |
| `tests/diff/lift.py` (60,000 random encodings) | 179,958 cases, 0 failures |
| `tests/diff/func.py` (whole functions) | 627 functions, 4,246 cases, 0 failures |
| `tests/diff/simd.py` | 159,976 cases, 0 failures |
| `tests/diff/atomic.py` | 12,672 cases, 0 failures |
| `tests/jumptables_check.py` | `main`, `sdk`: 0 mismatches; `subsdk0`: one safe superset at `subsdk0+0x5da14` |

- **1,112 spin waits** (`main` 1,090, `subsdk0` 12, `sdk` 10), listed in `recomp/patches-v130.txt`.
- Full build on an i5-11400H (6c/12t, 11 GB for WSL): `gen` 2 min 46 s, `guest` about 48 min (40 giant functions of
  1-7 MB of C dominate it, up to 1.6 GB of RAM each), runtime about 1 min.

### Boot (`ACR_GPU=null`)

| Milestone | Result |
|---|---|
| `AgeOfCalamityRecomp --selftest` | 3 modules, 101,212 functions in the dispatch table, 19 ns per lookup |
| Loading | `main` 142,157 relocations, `subsdk0` 19,177, `sdk` 31,396; all imports resolved |
| Main loop | **30.0 fps**, present interval 2, game clock 0.999-1.000x real time |
| NVN | 8,756 `ProgramSetShaders` at start, about 490 textures, 9 pools |
| Audio | `audren` REV8, 48 kHz x 240, 132 mix buffers, 138 voices, 16 effects |
| Intro video | `movie/Movie001.webm`, VP9 1280x720, decoded by libvpx at 1.3-1.55 ms per frame |

Fixes the boot needed in the inherited runtime: `fsp-srv` as a CMIF domain (the domain header comes before `SFCI`),
`nn::hid::GetTouchScreenState<1>`, the `nn::oe` copyright framebuffer, `nn::aoc::CountAddOnContent`, and
`nvnCommandBufferSetCopyImageStride`.

### Graphics

- **NVN compute control section**: +0x748 group size (3 x u32), **+0x754 shared memory in bytes**, +0x758 local memory
  per thread, +0x760 = 0x600.
- **Battle frame** (1440x810 scene): 96 passes; G-buffer in pass 9 (945 draws), froxel light grid in pass 39, sun in
  40, local lights in 53, bloom 73-86, luminance histogram 87-89, tonemap 90, HUD 94-95.
- **Colour textures** are created UNORM and sampled through sRGB views (105 BC7, 58 BC3 and 23 BC1 bindings in one
  battle frame). Ignoring the view washes the picture out (mean luma 233 instead of 175).
- **GPU particles**: a chain of compute shaders builds the particle quads; `0a66de99` and `d6c20785` reserve space with
  a warp ballot taken before `if (lane == findMSB(ballot))` (translator patch 0013).
- **Translator patch 0014**: Ryujinx's BranchElimination dropped a predicated `SYNC` followed by a copy-only block
  (`if (c) x = y`); 11 shaders hit it, among them the heart-recovery effect that showed as a white rectangle.
- **Cost of a battle** (i5-11400H + RTX 3050): steady 30 fps; GPU 6.7 ms, replay 7.8 ms, game thread busy for the whole
  33 ms frame (CPU bound).
- The KTGL main command buffer gets 3 MiB of control memory and a 16 MiB command ring once; the main loop waits for the
  previous frame on its own fence queue (`main+0x157a390`).

### Resolution

- `nnMain` builds the KTGL device configuration `{1920, 1080, 3, 9, 5, interval 2, 9, 1920, 1080, ...}` at
  `main+0x8d9490` and calls `main+0x17c34f0`. It then stores **1440x810** at `[x20+0x368]` (`main+0x8d94d0`): the 3D
  scene, 3/4 of the output on each side. The operation mode (docked or handheld) changes neither.
- The `output` and `scene` patch sets (`recomp/patches-v130.txt`) make `nnMain` read both sizes from the patch page.
- `main+0x1583c00` builds nine 2D views with `{1920, 1080}`: the HUD layout space. The HUD only lays out correctly
  with them left at 1080p, so a smaller output keeps them there.
- The KTGL 2D projection reads the target size from `[target+0x78]` (`main+0x17def58`, `0x17df7c8`, `0x17e103c`,
  `0x17e1e58`).
- Scene at 1920x1080 (`render_scale` 100) with a 1920x1080 output: the application pool grows by 89,088 KiB; the
  process uses 3.4 GB of RAM and 3.7 GB of VRAM on the title screen.

### Audio

- Music and cutscenes: **PCM16 48 kHz** voices of **6 channels** (mixes 12 and 14) and **2 channels** (mix 10), fed by
  the KTSL stream (Opus decoded by the SDK's software decoder; the `hwopus` service is never opened). Sound effects:
  **ADPCM 32 kHz mono**, up to about 35 at once in a battle.
- Track mixes go out through splitters (mix 10 to splitter 3 with 4 destinations, mix 12 to splitter 5 with 12).
  Getting the mix order right took the silent 5 s windows of a 175 s route from 21 of 37 down to 1.

### Steam Deck

| Item | Measured |
|---|---|
| System | SteamOS 3.9.5, RADV VANGOGH, Vulkan 1.4, graphics pipeline library and fast link available |
| Profile | detected through DMI: 30 fps, handheld mode, full screen |
| Before the battle | locked 30 fps |
| First battle | **24.3-24.6 fps**: the game thread takes about 27.6 ms per frame (16 ms on a laptop), and syncing the shadow copies of the CPU-visible pools costs about 30 ms per frame |
| Power | APU 7.6-8.7 W in battle at 30 fps, 3.3-4.0 W in menus; CPU 68-72 C, GPU 62-65 C |
