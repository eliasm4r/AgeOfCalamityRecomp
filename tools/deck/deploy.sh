#!/bin/bash
# Builds the Deck folder (dist) and sends the changed small files to the Steam Deck over SSH (never romfs.bin), and
# the DLC images (romfs/dlc-*.bin, 1.4 GiB in all) the Deck lacks or has with another size.
# Usage (Git Bash): AOC_DECK=deck@<address> AOC_DECK_DIR=<folder on the Deck> bash deploy.sh [--no-build]
# AOC_WSL_REPO is the repository path as WSL sees it (needed unless --no-build).
set -e
R="$(cd "$(dirname "$0")/../.." && pwd)"
D="$R/dist/aoc-deck"
DECK=${AOC_DECK:?set AOC_DECK=user@host}
G=${AOC_DECK_DIR:-/run/media/mmcblk0p1/aoc-deck}   # the folder on the Deck
SSH="ssh -i $HOME/.ssh/id_ed25519_deck -o BatchMode=yes"
if [ "$1" != "--no-build" ]; then
  : "${AOC_WSL_REPO:?set AOC_WSL_REPO to the repository path inside WSL}"
  wsl.exe -- bash -c "cd \"$AOC_WSL_REPO\" && . ~/.venvs/aoc/bin/activate && python3 -u tools/build.py --work ~/aoc-build host dist 2>&1 | grep -E 'dist:|host: done|ERROR|error|FAILED'"
fi
cd "$D"
# nothing is sent while the game is open on the Deck
busy=$($SSH -o ConnectTimeout=10 $DECK "pgrep -ax aoc" 2>&1) || true
if [ -n "$busy" ]; then echo "Deck busy or unreachable, nothing sent:"; echo "$busy"; exit 2; fi
files="aoc aoc_xlate aoc_xlate.id aoc.sh install-shortcut.sh README.txt romfs-index.bin lib/libSDL3.so.0 aoc-logs.sh tools/logsum.py tools/diag_view.py tools/perf_view.py"
$SSH $DECK "mkdir -p $G/tools"
# only what differs (size + sha256)
remote=$($SSH $DECK "cd $G && sha256sum $files 2>/dev/null" || true)
send=""
for f in $files; do
  h=$(sha256sum "$f" | cut -d' ' -f1)
  echo "$remote" | grep -q "$h  $f" || send="$send $f"
done
if [ -n "$send" ]; then
  echo "sending:$send"
  # to a temporary name, then renamed: a connection lost mid-copy (the Deck suspends) never leaves a truncated aoc
  for f in $send; do
    scp -q -i "$HOME/.ssh/id_ed25519_deck" -o BatchMode=yes "$f" "$DECK:$G/$f.part" && $SSH $DECK "mv -f $G/$f.part $G/$f" \
      || { echo "failed: $f (the Deck's old file is unchanged)"; exit 1; }
  done
  $SSH $DECK "cd $G && chmod +x aoc aoc_xlate aoc.sh install-shortcut.sh aoc-logs.sh"
else
  echo "nothing to send"
fi
# DLC images: sent once (compared by size; they only change if the dump does)
for f in romfs/dlc-*.bin; do
  [ -f "$f" ] || continue
  sz=$(stat -c %s "$f")
  rsz=$($SSH $DECK "stat -c %s $G/$f 2>/dev/null" || true)
  [ "$sz" = "$rsz" ] && continue
  echo "sending $f ($((sz / 1048576)) MiB)"
  scp -q -i "$HOME/.ssh/id_ed25519_deck" -o BatchMode=yes "$f" "$DECK:$G/$f.part" && $SSH $DECK "mv -f $G/$f.part $G/$f" || { echo "failed: $f"; exit 1; }
done
