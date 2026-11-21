# Changelog

## Unreleased

### Fixed
- **Render scale 125 % and 140 % are back** (supersampling, issue of 0.1.0): the game keeps its GPU textures in a pool
  of 1510 MiB, cut from a GPU heap of fixed size, and the larger scene filled it, so a story cutscene of the first
  battle loaded forever. Both now grow with the scene (patch set `texpool`, `ACR_PATCH_TEXPOOL=0` turns it off): 1786
  MiB at 140 %.

### Added
- **Render scale 150 %** (2880x1620 for a 1920x1080 picture): the larger scenes that stopped the game as the second
  intro movie started were the same full texture pool. 175 % and 200 % still stop on other limits of the game.
- **Game resolution 2560x1440 and 3840x2160** (Screen tab): the game draws its picture and its HUD at that size, so
  text, icons and the minimap stay sharp on a 1440p or 4K screen. The game made its window's images a fixed 1920x1080,
  so a larger picture showed only its top left corner; they now take the game resolution. The 3D scene keeps at most
  2880x1620 pixels (a larger one stops the game on loading a save): at 3840x2160, Game (75 %) gives exactly that and
  DLSS or the game's FSR brings it up to 4K. DLSS also runs at these sizes (the game's scaling draw there is in its
  1920x1080 layout units).

## 0.1.1

### Fixed
- **Endless loading at the first cutscene** with the 3D render scale at 125 % or 140 %: the game's GPU texture pool
  has a fixed size and the larger scene filled it. Render scale now goes up to 100 %; a `settings.ini` with 125 or
  140 is read as 100. Supersampling comes back once that pool can be grown.

## 0.1.0 (beta)

First beta of AgeOfCalamityRecomp: Hyrule Warriors: Age of Calamity (update 1.3.0) built for Windows and the Steam
Deck from your own copy of the game.

### Setup
- The Windows download is an installer: the zip holds only the setup, which asks where to put the game
  (`%LOCALAPPDATA%\AgeOfCalamityRecomp` by default) and builds it there. The extracted zip can be deleted afterwards;
  a newer zip installs over the same folder.
- A plain setup window, started by `AgeOfCalamityRecomp Setup.exe` on Windows and `setup.sh` on Linux: the folder with
  your NSPs and `prod.keys` with a check mark for each file found, the platforms, a progress bar in percent and the
  log behind **Show details**. Where Smart App Control refuses the setup exe, `python/Setup (if blocked).exe` opens the
  same window.
- It is an installer only: the game's settings are in the game (F1, or Minus + Plus on a controller). Run it again on
  a folder that already holds the game and **Update** installs the new version over it, redoing only what changed and
  keeping your saves and settings.
- The Windows build uses the whole PC: a first install takes about 20 minutes on an 8-core PC with an NVIDIA card,
  the shader preparation included, and the progress bar follows it to the end. A dump on another disk is read in
  place instead of being copied first.
- **Shaders prepared before play**: every one of the game's 40,727 shaders is translated by the setup (and, on AMD,
  built into the graphics driver's cache), so a fight does not stop to translate them. Without it, the first seconds
  of each fight skipped about 170 draws (objects missing, effects blinking) and stalled for up to 700 ms.
- With an NVIDIA RTX graphics card the setup downloads NVIDIA's DLSS library (`nvngx_dlss.dll` 310.9.1, from NVIDIA's
  DLSS SDK repository, checked against its pinned hash).
- The game goes in the Start Menu, and on the desktop unless the box is unticked, with its own icon.
- The Steam Deck part says how the folder reaches the Deck (copied by hand or sent over SSH) and when the 10.9 GB of
  game data are needed.
- A `recomplauncher.json` for RecompLauncher: the build, the game, its saves, logs and main settings.

### Graphics
- **60 fps**: Frame rate 60 in the quick menu or the full menu. The game runs at the right speed (the same distance
  run and the same dialogue timing as at 30), and a 1080p battle holds 59-60 fps on a Ryzen 7 5800X with an RTX 4070.
  Big fights hold it too: the frames are now prepared for the GPU on a thread of their own (Advanced tab, GPU frame
  prep on its own thread, on by default), where the game thread alone fell to about 45 fps.
- **NVIDIA DLAA** (DLSS at full resolution) works in Age of Calamity: it finds the game's own scene depth and camera,
  gets the camera's motion with a reprojection worked out in double precision, uses NVIDIA's transformer model
  (preset K), the real time between frames and a jitter sequence as long as NVIDIA asks for. Edges are smooth and
  details stay steady when the camera moves.
- **DLSS Quality, Balanced and Performance** now really run DLSS: the scene is drawn at a lower resolution and
  DLSS brings it up to the output, in place of the game's own scaling. The HUD stays at full resolution and is
  never passed through DLSS.
- **DLSS model** setting (Screen tab): Automatic (NVIDIA's advice per mode, K for DLAA), K, or J, which leaves a
  little less ghosting behind moving characters for a little more flicker. It applies at once.
- **Steadier 60 fps**: the shader cache is saved in calm moments (5 s without a new shader, at most every 2 minutes,
  and at exit) instead of every 20 s, which dropped 60 fps to 54 for a second every 24 s in long battles.
- **120, 144 and 240 fps (experimental, still in development)** in the Frame rate setting (full menu and quick menu), applied at once in the middle of a
  game: as in the TotK recomp, above 60 the game steps by the real time of each frame, so it runs at the right speed
  at any rate, cutscenes included. Smoother battles too: about half the time of turning the game's graphics commands
  into Vulkan went to a check made on every draw, and the game's main thread no longer waits as often for its
  processor core.
- **FXAA** for every graphics card: the picture goes through an FXAA pass just before the game draws the HUD, so the
  edges of the 3D scene are smoothed while the HUD stays sharp. It is on by default and
  turns itself off while DLAA or DLSS runs.
- **Shadow resolution** works in Age of Calamity: the sun's 1024x2048 shadow map is drawn up to 4 times larger on
  each side, so shadow edges on stairs and walls no longer show steps that crawl when the camera moves.
- **Render scale** up to 140 %: 100 % draws the 3D scene at full 1920x1080, and 125 % and 140 % draw it larger and
  scale it down (supersampling, 2688x1512 at 140 %). 150 % and more crash the game: one of its own fixed memory heaps
  runs out when a cutscene starts.
- With fullscreen on, the game opens fullscreen straight away, behind your other windows while it loads, instead of
  showing its picture in the top left corner and then growing to the whole screen.
- The options menu (F1) has its own Sheikah-blue look and shows only the settings that do something in this game.
- **Frame queue** (Game tab, off by default): the game starts its next frame while the previous one waits for its
  turn on screen, so a slow frame is less likely to be dropped, for up to one frame more input delay.

### Sound
- **Output device** (Sound tab): the system default, or a device chosen by name. The sound moves to it at once; if it
  is unplugged the system default plays until it is back.

### Controls
- **Xbox button prompts** (Controls tab, Button prompts: Xbox; applies at once, no restart): the game's prompts and menus show A, B,
  X and Y in the Xbox colours, LB, RB, LT, RT, Menu and View. With the Nintendo layout (A on the right button) the
  prompt for A shows B, as that button is labelled B on an Xbox pad. The setup draws them over the game's own button
  textures from your copy of the game.
- **PlayStation button prompts** (Button prompts: PlayStation): cross, circle, square and triangle in their colours,
  L1, R1, L2, R2, Options and Create.
- **One button layout**: every controller plays by position as on the Switch (A on the right button: circle on a
  PlayStation pad, B on an Xbox pad). The Button layout option is gone (an older settings.ini reads as Nintendo); the
  remapping rows of the Controls tab change any button instead.
- Keyboard and mouse with every key rebindable; Xbox, PlayStation and Switch controllers with rumble and motion.

### Cleanup
- Settings left over from the recompilations this port started from, which never did anything in Age of Calamity,
  are gone: shadow map size, shadow cascade range, field of view, stick camera speed, game speed and cloth on the
  clock, and high frame rate fixes. Lines left for them in `settings.ini` do nothing (the log names them unknown).
- Code made for those games' files and versions is gone too (their resource size table for mods, their zstd
  decoder, a wait made for one Astral Chain version's uniform buffers): Age of Calamity has none of them.
- Earlier logs are kept as `logs/game.1.log` to `game.4.log`. On Linux the window belongs to
  `AgeOfCalamityRecomp.desktop`, so the taskbar shows the game's icon.

### Known limits
- Above 60 fps a busy battle stays around 85-110 fps on a Ryzen 7 5800X: the game's main thread is the limit.
- With Xbox or PlayStation prompts, the L and R tabs at the top of the menus and the stick and d-pad icons keep the Switch's look.
- In DLSS Quality, Balanced and Performance, each step of the game's own dynamic resolution (it lowers the 3D
  resolution down to 70 % in big fights, then raises it again a step at a time) starts DLSS over, which costs
  15 to 35 ms (sometimes a dropped frame) and a frame of softer edges.
- The first seconds after a fight starts are a little soft: the game loads its sharp textures progressively, as on
  the Switch.
