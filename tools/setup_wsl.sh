#!/usr/bin/env bash
# Prepares Linux (WSL2 or a Linux PC: Arch, Ubuntu/Debian or Fedora) to generate and compile Age of Calamity Recomp:
#   bash "<repository>/tools/setup_wsl.sh"            everything, for development (runtime libraries, gdb, tests)
#   bash "<repository>/tools/setup_wsl.sh" --minimal  only what the Deck forge needs (the launcher uses this)
# Installs the packages through pacman, apt or dnf and creates a Python venv in ~/.venvs/AgeOfCalamityRecomp. As root (WSL's Arch) it
# asks no password; in a terminal it uses sudo; without a terminal (the launcher) it uses pkexec, which opens the
# system's password window.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MINIMAL=0
[ "${1:-}" = "--minimal" ] && MINIMAL=1

if [ "$(id -u)" = 0 ]; then
    SUDO=()
elif [ -f /run/.containerenv ] || [ -f /.dockerenv ]; then
    # a distrobox container (tools/make_build.py on SteamOS, Bazzite and other read-only systems): its sudo asks no
    # password, and there is no terminal or polkit agent inside
    SUDO=(sudo -n)
elif [ -t 0 ] && command -v sudo >/dev/null; then
    SUDO=(sudo)
elif command -v pkexec >/dev/null; then
    SUDO=(pkexec)
else
    SUDO=(sudo)
fi

# What the forge needs: clang, lld and llvm-ar (the game code and the Deck executable against the sniper sysroot),
# ninja, Python with venv, make and curl (dist step: static zstd and the sysroot) and the Vulkan headers (dist copies
# them to compile against the sniper). The full mode adds the runtime libraries (SDL3, Opus, libvpx, zstd, the Vulkan
# loader) for the development executable, pkg-config, git and gdb.
have_minimal() {
    command -v clang >/dev/null && command -v ld.lld >/dev/null && command -v llvm-ar >/dev/null \
        && command -v ninja >/dev/null && command -v make >/dev/null && command -v curl >/dev/null \
        && [ -f /usr/include/vulkan/vulkan.h ] && python3 -c 'import venv, ensurepip' >/dev/null 2>&1
}
have_full() {
    have_minimal && command -v pkg-config >/dev/null && pkg-config --exists sdl3 opus vpx libzstd vulkan
}

if { [ $MINIMAL = 1 ] && have_minimal; } || have_full; then
    echo "Tools already installed."
elif command -v pacman >/dev/null; then
    # WSL's Arch image only creates the pacman keyring on the first interactive start (/usr/lib/wsl/first-setup.sh);
    # opened by the launcher (wsl -e) that does not run, and pacman refuses every package ("required key missing").
    # The local master key ("pacman@localhost") only exists after --init; a pacman that failed before leaves an empty
    # pubring.kbx, which is no sign.
    if ! "${SUDO[@]}" pacman-key --list-secret-keys 2>/dev/null | grep -q 'pacman@localhost'; then
        echo "Creating the pacman keyring (first time)..."
        "${SUDO[@]}" sh -c 'pacman-key --init >/dev/null 2>&1 && pacman-key --populate archlinux >/dev/null 2>&1'
    fi
    pkgs=(clang lld llvm ninja python make curl tar vulkan-headers)
    [ $MINIMAL = 0 ] && pkgs+=(python-pip git pkgconf gdb time sdl3 opus libvpx zstd vulkan-icd-loader)
    "${SUDO[@]}" pacman -Syu --noconfirm --needed "${pkgs[@]}"
elif command -v apt-get >/dev/null; then
    pkgs=(clang lld llvm ninja-build python3-venv make curl ca-certificates libvulkan-dev)
    # SDL3 only exists from Ubuntu 25.04 on (libsdl3-dev); the Deck forge does not need it (it uses the sniper's).
    [ $MINIMAL = 0 ] && pkgs+=(python3-pip git pkg-config gdb libopus-dev libvpx-dev libzstd-dev libsdl3-dev)
    "${SUDO[@]}" sh -c "export DEBIAN_FRONTEND=noninteractive; apt-get update && apt-get install -y ${pkgs[*]}"
elif command -v dnf >/dev/null; then
    pkgs=(clang lld llvm ninja-build python3 make curl tar vulkan-headers)
    [ $MINIMAL = 0 ] && pkgs+=(python3-pip git pkgconf gdb opus-devel libvpx-devel libzstd-devel vulkan-loader-devel SDL3-devel)
    "${SUDO[@]}" dnf install -y "${pkgs[@]}"
else
    echo "ERROR: distro without pacman, apt or dnf; install clang, lld, llvm, ninja, make, curl and python3 (with venv) by hand." >&2
    exit 1
fi
# Some Ubuntu versions install lld as ld.lld-NN; the dist step calls ld.lld through clang (-fuse-ld=lld).
command -v ld.lld >/dev/null || { echo "ERROR: ld.lld not found after the install" >&2; exit 1; }

if [ ! -x "$HOME/.venvs/AgeOfCalamityRecomp/bin/python" ]; then
    python3 -m venv "$HOME/.venvs/AgeOfCalamityRecomp"
    "$HOME/.venvs/AgeOfCalamityRecomp/bin/python" -m pip install -q --upgrade pip
fi
if [ $MINIMAL = 1 ]; then
    "$HOME/.venvs/AgeOfCalamityRecomp/bin/python" -m pip install -q -r "$REPO/tools/requirements-forge.txt"
else
    "$HOME/.venvs/AgeOfCalamityRecomp/bin/python" -m pip install -q -r "$REPO/tools/requirements.txt"
fi

echo
echo "Done. To use it:"
echo "  source ~/.venvs/AgeOfCalamityRecomp/bin/activate"
echo "  cd \"$REPO\""
echo "  python3 tools/build.py --work ~/AgeOfCalamityRecomp-build"
clang --version | head -1
echo "ninja $(ninja --version)"
