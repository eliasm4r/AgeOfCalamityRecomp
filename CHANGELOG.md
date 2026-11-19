# Changelog

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
- **NVIDIA DLAA** (DLSS at full resolution) works in Age of Calamity: it finds the game's own scene depth and camera,
  gets the camera's motion with a reprojection worked out in double precision, uses NVIDIA's transformer model
  (preset K), the real time between frames and a jitter sequence as long as NVIDIA asks for. Edges are smooth and
  details stay steady when the camera moves.
- **Render scale** up to 100 % works (the 3D scene at full 1920x1080). Above 100 % is no longer offered: the game
  crashes when its scene is larger than its output.
- The options menu (F1) has its own Sheikah-blue look and shows only the settings that do something in this game.

### Controls
- PlayStation controllers play by position like a Switch controller under **Automatic**: circle A, cross B, square Y,
  triangle X.
- Keyboard and mouse with every key rebindable; Xbox, PlayStation and Switch controllers with rumble and motion.

### Known limits
- 30 fps only for now: the frame rate limit above 30 stays greyed out until the high frame rate patch exists.
- The button icons shown in the game are the Switch's.
- The first seconds after a fight starts are a little soft: the game loads its sharp textures progressively, as on
  the Switch.
