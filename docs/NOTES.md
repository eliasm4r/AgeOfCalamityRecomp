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
- Optional NVIDIA DLSS/DLAA through the driver's NGX core (`vk_dlaa.c`, Windows only, needs `nvngx_dlss.dll`), or
  AMD FSR 3.1 at the same point of the frame with the same inputs (`vk_fsr3.cpp`, the FidelityFX SDK 1.1.4 upscaler on
  its Vulkan backend, any GPU; `dlaa_engine` auto|dlss|fsr3). FSR 3.1 takes the depth as R32F: each frame copies the
  depth aspect of the game's D32F/S8 into it through a buffer. The game's depth is reversed and 0..1 (measured from the
  scene's view-projection in the Hyrule Field charge: 1 at 64 units, 0 at 210,000; the units look like centimetres),
  so `tools/ffx_shaders.py` builds the upscaler for inverted depth (TotK Recomp's header is for standard depth). The
  near and far planes and the vertical field of view (37 degrees there) are read from the view-projection each frame
  (`camera_planes`). One FSR 3.1 history serves every dynamic-resolution share of a target size, where NGX needs a
  feature per share. Same cost as DLSS on an RTX 4070 at 1080p and 60 fps; its pipelines take about half a second
  more at the first frame. Only the NGX loading is Windows code: elsewhere `auto` takes FSR 3.1 (compiled with Linux
  clang in WSL; not yet run on a Linux GPU or the Deck).

### 5. Frame pacing

- Default: the game as on the Switch, **30 fps** (present interval 2).
- **60 fps** (`frame_rate.c`): the game's frame-rate block at `main+0x269e150` is `{fps, vsyncs per update, fps, fps
  as a float}`, `{30, 2, 30, 30.0}` as shipped. The game's own setter writes `60/n, n, 60/n, 60/n` from a request
  `(n << 4) | 1`; the game turns frame counts into seconds by dividing by the float and steps by the vsyncs per update.
  The host writes `{60, 1, 60, 60.0}` at every present and presents every vsync instead of every second one (the
  window's interval 2 is replaced). Measured: a scripted 3 s run in the Hyrule Field battle covers the same distance,
  and the dialogue lands at the same second, at 30 and at 60 fps (shots each second of world time). Changing only the
  present interval doubles the game speed.
- **120, 144 and 240 fps**: the vsyncs per update are whole numbers, so above 60 the game gets the frame's real time
  instead (TotK's real-time step: mean of the last 5 display intervals, restarted after a 0.25 s gap). 159
  instructions read +0x154; the 104 that convert it to a float in a straight line (no branch or branch target between
  load and ucvtf/scvtf) read +0x158 as 16.16 fixed point (`recomp/patches-v130.txt`, set `vsync`; +0x158's one reader
  moved to +0x150, the setter's two writes dropped). The 55 integer readers get whole vsyncs that add up to the same
  time (1, 0, 1, 0... at 120). The rate fields must hold the setting's rate: with them at 60 and a 120 Hz present the
  game holds itself at 60. Before the patch the intro cutscene ran faster at 120; with it the cutscene's shots land
  at the same second at 60 and 120 (90 s, shots every 3 s of world time). A global speed factor at main+0x263a8e8
  (1.0) multiplies 68 of the readers; it is left to the game.
- Cost above 60 (5800X + RTX 4070, recorded Hyrule Field battle, 1080p, no DLAA): 120 in cutscenes, 85-110 in the
  battle. The main thread (pinned to virtual core 0) is the limit: it runs about 68 % of the frame and waited 20-24 %
  for its core. Job workers allowed on every core now take core 0 last and lose ties for it to a thread pinned there
  (`kernel.c` core_take, core_grant): +5 to +10 fps. The replay's DLAA hook scanned every scene draw's constants for a
  diagnostic count, half the replay's busy time (8.6 ms -> 5 ms a frame); it stops at the camera now.
- Cost at 60 fps (Ryzen 7 5800X + RTX 4070, 1080p, Hyrule Field battle with the player running and attacking): 59-60 fps,
  short dips to 46-56 fps while a cutscene or an area loads.

### 6. Toolchain

- Clang + LLD (the generated C uses `__int128`, `musttail` and vector types), Ninja, Python, .NET (translator), Vulkan
  headers, SDL3, libopus, libvpx, VMA.
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
| Xbox and PlayStation button prompts | `prompts.c` (swaps the button textures at upload), `tools/make_prompts.py` (draws them) |
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
  SteamOS. Opus and libvpx are built in.
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
| Frame rate | **30** (default), 60, 120, 144 or 240 | each step up needs more processor time; all run at the right speed |
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
- **Button icons**: every prompt icon comes from one atlas, G1T `3226a433` in `asset/ScreenLayout.rdb.bin` (1024x1024
  BC3, linear blocks); the game uploads it from guest memory (`vk_texture.c` upload). Text and icons are drawn into a
  2048x2048 RGBA text atlas each menu frame (font pages are CPU-rendered 1024x768 RGBA from the G1N fonts in
  `asset/RRPreview.rdb.bin*`). Menu textures arrive through `nvnTextureWriteTexels`. The trapezoid L/R tab art also
  sits in G1T `8d1f8cb8` (512x256), but no upload of it was seen on the Allies screen: the tabs there still come from
  an unknown texture.
- The KTGL main command buffer gets 3 MiB of control memory and a 16 MiB command ring once; the main loop waits for the
  previous frame on its own fence queue (`main+0x157a390`).
- **Flushes of the buffer pool** (215 MiB, NVN flags 0x22, VRAM mirror): about 13 `nvnMemoryPoolFlushMappedRange` a
  frame on the save route at 1080p, 8.4 MiB in all, spread from +36 to +150 MiB of the pool. Kept as one span, they
  made the mirror copy about 115 MiB to the GPU every frame (117 MiB of sync copies a frame, 3 ms of an RTX 4070;
  with a 3840x2160 scene 215 MiB and 11 ms, the frame at 47 fps with 10.4 ms of other GPU work). Kept as up to 16
  ranges merged within 64 KiB (`fl_add`), the sync copies are 20 MiB a frame (`ACR_TRACE=perf`, `mem: mirror` line).

### Resolution

- `nnMain` builds the KTGL device configuration `{1920, 1080, 3, 9, 5, interval 2, 9, 1920, 1080, ...}` at
  `main+0x8d9490` and calls `main+0x17c34f0`. It then stores **1440x810** at `[x20+0x368]` (`main+0x8d94d0`): the 3D
  scene, 3/4 of the output on each side. The operation mode (docked or handheld) changes neither.
- The `output` and `scene` patch sets (`recomp/patches-v130.txt`) make `nnMain` read both sizes from the patch page.
- `main+0x1583c00` builds nine 2D views with `{1920, 1080}`: the HUD layout space. The HUD only lays out correctly
  with them left at 1080p, so a smaller output keeps them there.
- The KTGL 2D projection reads the target size from `[target+0x78]` (`main+0x17def58`, `0x17df7c8`, `0x17e103c`,
  `0x17e1e58`).
- Scene larger than the output: 145 % (2784x1566) reaches the battle, 148 % and more crash as the second intro movie
  starts. A resource loader (`main+0x18d3040`) gets null from a fixed heap (a linear allocator, then a fallback through
  its vtable +0x48), and `main+0x191cd7c` reads `[null+0x104]`. A 4 GiB application pool does not help.
- 125 % and 140 % reach the battle but the first story cutscene never finishes loading (a job thread polls
  `main+0x15209c0` forever). The game's GPU texture pool (NVN `COMPRESSIBLE`) is a fixed 1510 MiB, set at
  `main+0x8aecf8` (`mov w11, #0x5e600000`, next to 215 MiB at `0xd700000` and the 101 MiB shader pool): the same
  size at 100 and 140 %, so the application pool's growth never reaches it. Growing it alone (even by 32 MiB) makes
  `nnMain` give up right after the first `nvnMemoryPoolInitialize` (`main+0x4ee0`): its storage comes from another
  fixed game heap.
- That heap: `nnMain` allocates one GPU heap of a fixed **0x72204008** bytes (`main+0x8ad378`, `mov w1, #0x4008` and
  `movk w1, #0x7220, lsl #16`; size kept at `[main+0x2b5f550]`, heap at `[main+0x2b5f4d8]`, its allocator object at
  `main+0x2b5f9e8`, the one `[main+0x2b5fd40]` points to). `main+0x8b00c0` takes the three pools from it in order
  (`main+0x8b1684`, `0x8b1774`, `0x8b17ec`, each a 4 KiB header then the pool) through that allocator's vtable +0x40:
  215 + 1510 + 101 MiB + 3 headers fill it exactly. The `texpool` patch set reads both sizes from the patch page and
  grows them by the same amount (the targets' growth, `res_heap_extra_bytes`), and the application pool grows by that
  much more: at 140 % the pool is 1786 MiB and the game starts.
- The 148 % stop was the same pool: the loader `main+0x18d3040` creates a texture for the second intro movie and gets
  null when the pool is full. With it grown, 150 % (pool up to 1568 MiB used in the first battle) plays the first
  battle and the second one's opening cutscene. Counting the growth once more in the application pool asked for more
  than the 4 GiB heap at 175 % (`SetHeapSize` failed, `0xd401`), so the pool now comes out of the application pool's
  growth. 175 % (3360x1890, pool 2004 MiB, up to 1792 used) plays a new game's first battle but stops on loading a save:
  `main+0x151daf4` passes a null object (`[x24+0xb0]`) to `main+0x17bf930`, which reads `[null+0x48]`. 200 %
  (3840x2160) stops 18 s into the first battle on a null write in lifted game code.
- Outputs above 1920x1080: the device setup `main+0x17c34f0` sizes the window's images from the configuration when the
  device's byte `[x20+0x2a32]` is clear and a fixed 1920x1080 (`main+0x17c3770` `mov w10, #0x780`, `main+0x17c3784`
  `mov w11, #0x438`) when it is set, as it is. The game drew a 2560x1440 picture into 1920x1080 window images, and the
  screen showed its top left corner (the window crop the game sets is the output's size, so it was not the crop). The
  `output` set reads both constants from patch page +120; `res_init` writes the output there when it is larger, else
  1920x1080, so smaller outputs are unchanged. Measured: 2560x1440 with the scene at Game (1920x1080) and 100 %, and
  3840x2160 at Game (2880x1620), play the second story battle; the HUD is drawn at the output size from its 1920x1080
  layout. 3840x2160 at 100 % (a 3840x2160 scene, 1012 MiB of growth) first asked for more than the 4 GiB heap, and
  with the heap raised stopped on loading a save like 175 % at 1080p: the scene size is the limit, so the scene keeps
  at most 2880x1620 pixels. On outputs other than 1920x1080 the game's scaling draw (scene to output) is a quad in the
  1920x1080 layout units, and DLSS (`vk_dlaa_scale_draw`) now accepts it. DLSS modes at 1280x720 still never reach the
  scaling draw (the DLAA point is not armed there; not looked into).
- A scene of exactly the output's size above 1920x1080 (100 % at 2560x1440 or 3840x2160): the `output` set's test is a
  per-axis size equality with the output, so the scene's own targets took the 1920x1080 layout too. Battles looked
  whole, but in-engine cutscenes showed the top left 1440x810 of a 2560x1440 picture stretched: 0.5625 of each side
  (two passes at 0.75), the same with DLAA off; Game (1920x1080 scene) and 99.8 % (2556x1438) were whole (shots once a
  second through the second story battle's opening, each matched against 1920x1080 at Game as a whole and as top left
  crops, 2026-11-21). `resolution.c` now draws such a scene 2 pixels smaller on each equal axis.
- The 175 % stop and the 3840x2160 one were the heap's first pool: 215 MiB with NVN flags 0x22 (CPU uncached, GPU
  cached: buffers), set at `main+0x8aece8` (`mov w11, #0xd700000`). The object `main+0x151daf4` passed on is the
  movie player's (`[x20+0x4a0]`), whose buffer was never made. The end of the furthest buffer or texture made in that
  pool, on the save route with the pool grown: 167 MiB at Game, 181 at 100 %, 223 at 150 %, 249 at 175 %, 278 at
  200 % and 278 with a 3840x2160 output at 100 %, so 15.6 MiB more per million scene pixels and nothing for the
  output's size. It is an upper bound (150 % played in 215 MiB). The `texpool` set reads that size from patch page
  +128 and grows it by 16 MiB per million scene pixels over 1440x810 (`res_buffer_pool_extra_bytes`, 116 MiB at
  3840x2160), which keeps the game's own 48 MiB margin; the GPU heap and the application pool grow by as much. With
  it, 175 % and 200 % load a save and play a new game's first battle, and so does a 3840x2160 output at 100 %; the
  texture pool at 200 % holds up to 2077 MiB of its 2190. A 3840x2160 output and scene make a 4.3 GiB application
  pool, and the heap the SDK asks for no longer fit the 4 GiB heap region (`SetHeapSize` failed with `0xd401`), so
  `mem_init` sizes the region from the pool (5 GiB there, 4 GiB at any smaller size). A larger scene was never measured and is brought back to 3840x2160 pixels.
- The held picture: loading after "New game" (among others), a capture node (`main+0x161df30`) copies the current
  colour target into a texture of the scene's size every frame, and the next pass draws that texture over the output.
  The copy is `main+0x17e1880(ctx, source, uv rect, target, position rect)`, called at `main+0x161e894` with one
  rectangle for both (`[sp+0x40]`), built from `[source+0x58]` (KTGL texture objects keep their size there), the
  output's 1920x1080, times a normalized rect (`[[x21+0x110]+0x151c..0x1528]`, 0, 0, 1, 1 here). The 2D projection is
  the target's (`[[stack entry+0x50]+0x78]`, 2/3840 at 200 %), so the quad covered the target's top left 1920x1080
  pixels. The texture is made at the address of the scene's G-buffer colour target, which nothing clears, so the
  rest showed the last scene unlit (the black loading picture in the top left corner) at 150 and 200 %. At the game's
  own scale the target (1440x810) is smaller and the copy crops the picture, as on the Switch. The `hold` set scales
  the position rect by the target's size over the source's, at least 1, into `[sp+0x50]`. Found by logging the draws
  whose 2D quad is smaller than a target of 2000 pixels or more, then a probe site writing the registers to the patch
  page.
- Scene at 1920x1080 (`render_scale` 100) with a 1920x1080 output: the application pool grows by 89,088 KiB; the
  process uses 3.4 GB of RAM and 3.7 GB of VRAM on the title screen.

### Ultrawide

- The 3D cameras take a fixed 16/9 that `nnMain` stores at `[x21+0x1dd8]` (`main+0x89cb88`), next to their near, far
  and 45 degree field of view. The `aspect` set (`recomp/patches-v130.txt`) reads the output's width over its height
  from patch page +136 instead, and two camera builders that compare against 16/9 (`main+0x189ea34`, `0x189eaa4`)
  take it too. Horizontal field of view grows, vertical stays the game's (Hor+).
- The UI stays a 1920x1080 layout, centred: the KTGL 2D projection sites take their x scale and offset from page +200
  on a target of the output's size. A layout of the output's shape at 1080 units high (2560x1080 for 21:9) put the
  HUD at the screen's far sides but broke the pause menu's markers, which the game places in 1920x1080 units.
- Full-width 2D pieces: quads drawn from x 0 to 1920 reach the edges (`main+0x17e16c0`); the UI ortho cameras
  (`main+0x18ac480`, inlined at `main+0x18fdde4` for the pause menu) take the output's width; UI panes 1920 wide and
  centred (`main+0x18f1fa8`) widen. A pane's u spreads with it (texels keep their size) only when it is the 1920/width
  crop of a picture as wide as the output (the pause background); every other pane stretches, as spreading read past
  atlas pieces (streaks in the mission select menu) and, with clamp-to-border samplers, left the loading screen's
  black box 16:9 wide.
- The pause background's blur clamps its samples to a centred 1920/width rectangle of its target (ints at
  `obj+0x1420`, `main+0x18fe60c`); that rectangle becomes the whole target. Its test allows 1/64, as the rectangle
  is in whole texels (357 of 480 at 3440x1440).
- Outputs narrower than 16:9 (16:10) draw 16:9 at their width with bars, as on the Steam Deck: the 1920-wide UI lost
  its sides at 1920x1200.
- Checked 2026-11-26 at 2560x1080, 3440x1440 and 3840x1080 (battle HUD, minimap, dialogue boxes, pause menu and its
  markers, mission select menu, cutscene subtitles, title screen, loading screens) and 1920x1200. Left: the title
  screen's white haze is drawn by a perspective camera and ends at the 16:9 area's right edge.

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
