# Changelog

## 0.2.0

First version of AgeOfCalamityRecomp, based on AoC Recomp 0.1.5.

### Fixed
- **Render scale above 75 % was ignored.** The 3D scene stayed at the Switch's 1440x810 whatever the setting, because
  the runtime waited for a Tears of the Kingdom heap table that Age of Calamity does not have. 85 % and 100 % now
  render the scene at 1632x918 and 1920x1080.
- **No `logs/aoc.log` when started from RecompLauncher.** Every game run now writes its log next to the game.

### Changed
- The options menu greys out, with the reason, the settings that do nothing in this game: the frame rate limit above
  30 fps (until the 60 fps patch exists), DLSS / DLAA without `nvngx_dlss.dll`, and the stick camera speed.
- Everything is written in English: README, notes, comments, tool messages and file names. In the Deck folder:
  `install-shortcut.sh`, `README.txt`, `tools/`, `logs/sessions/`; the performance logs go to `logs/performance/`;
  `aoc-logs.sh` takes `list`, `summary`, `live`, `system`, `detailed`, `performance`, `diag` and `bundle`.
- English only: the launcher, the build tool and the in-game menu no longer have Portuguese, Russian or Chinese text
  (the game's own name-entry keyboard still follows the game's language).
- The launcher's setup is a three-step wizard (Game files, Platforms, Build) with the full log folded behind
  **Technical details**; the play screen shows a short explanation under every setting.
- The documentation is five files: this changelog, the README, `CONTRIBUTING.md`, `THIRD_PARTY_NOTICES.md` and
  `docs/NOTES.md`.
- `tools/deck/deploy.sh` takes the Deck's address and folder from `AOC_DECK` / `AOC_DECK_DIR` instead of fixed values.
- The old PowerShell RomFS merge (`tools/windows/merge_romfs.ps1` / `.bat`) is gone: `tools/merge_romfs.py` does the
  same on Windows and Linux.
- Links, reports and the issue form point to this repository.

## AoC Recomp 0.1.5 and earlier

The versions below were released by the original project.

- **0.1.5**: packages identified by their contents (any file name), an update other than 1.3.0 refused with a clear
  message, leftovers of earlier attempts redone by themselves, a **Report a problem** button in the launcher, and
  RecompLauncher support.
- **0.1.4**: black screen after the logo with a save fixed (the second logo video is the only one with sound), and the
  **four DLCs** extracted from the player's files and mounted by the game.
- **0.1.3**: an always-on performance log, one row a second, with a mark button (F8, or L3+R3 on the Deck).
- **0.1.2**: report zips with the right logs, automatic reports on long black or frozen screens, `monitor.bat`.
- **0.1.1**: black screen at a cutscene after about 12 minutes fixed (a virtual core deadlock).
- **0.1.0**: first package for any player: the launcher builds the game for Windows and the Steam Deck from the
  player's own files.
