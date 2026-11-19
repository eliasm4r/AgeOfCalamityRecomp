@echo off
rem Age of Calamity Recomp monitor mode: play as usual and the game keeps by itself what is needed to investigate.
rem Copy it into the game folder (next to AgeOfCalamityRecomp.exe) and start the game from here instead of the launcher.
rem
rem   - each hit (start of a controller rumble): 8 full-size pictures in logs\diag\hits\ (up to 40 hits)
rem   - frame dump on a hit: add set ACR_HIT_DUMP=<n> or ACR_ATTACK_DUMP=<n> (counted from the menu)
rem   - each attack (Y button, one every 20 s): 12 full pictures in logs\diag\hits\a*.png
rem   - F8: marks the moment (full picture in logs\diag\marks\, report in the log, frame dump)
rem   - dark or black scene for 15 s: report + picture + dump, without pressing anything
rem   - the session's commands go to logs\sessions\ and the log to logs\game.log
rem
rem After playing: python tools\diag_view.py  (at the repository root), or send the logs folder.
cd /d "%~dp0"
set ACR_VK_RECORDER=0
set ACR_DUMP=arm
set ACR_DUMP_DIR=%~dp0logs\diag\dump
set ACR_DUMP_UBO=24
set ACR_DUMP_SKIP_RUNS=1
set ACR_DUMP_MAX=6
set ACR_HIT_SHOTS=40
set ACR_ATTACK_SHOTS=30
set ACR_MARK_DUMP=1
start "" "%~dp0AgeOfCalamityRecomp.exe"
