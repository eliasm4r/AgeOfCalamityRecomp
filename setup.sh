#!/bin/sh
# Opens the setup on Linux (needs python3 with tkinter: python3-tk on Debian and Ubuntu, tk on Arch and SteamOS).
cd "$(dirname "$0")" || exit 1
if ! python3 -c "import tkinter" 2>/dev/null; then
    echo "The setup needs tkinter: install python3-tk (Debian, Ubuntu) or tk (Arch, SteamOS, Fedora), then run it again."
    exit 1
fi
exec python3 setup/setup.py "$@"
