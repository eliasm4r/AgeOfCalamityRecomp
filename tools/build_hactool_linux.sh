#!/bin/bash
# Builds hactool 1.4.0 (ISC) for Linux against the sniper sysroot (glibc 2.31), so it runs on any current distro.
# tools/make_release.py puts it in prebuilt/hactool/linux-x64 of the Linux package (tools/merge_romfs.py uses it).
#   bash tools/build_hactool_linux.sh <hactool 1.4.0 source> <output folder> [sysroot]
set -euo pipefail
SRC="$1"; OUT="$2"; SNIPER="${3:-$HOME/AgeOfCalamityRecomp-build/toolchain/sniper}"
[ -f "$SRC/main.c" ] || { echo "ERROR: $SRC is not the hactool source" >&2; exit 1; }
B="$(mktemp -d)"
cp -r "$SRC/." "$B/"
cd "$B"
cp config.mk.template config.mk
make -s CC="clang --target=x86_64-linux-gnu --sysroot=$SNIPER" AR=llvm-ar \
    LDFLAGS="-fuse-ld=lld -lmbedtls -lmbedx509 -lmbedcrypto" -j"$(nproc)" >/dev/null 2>&1 \
    || { echo "ERROR: hactool did not compile" >&2; exit 1; }
mkdir -p "$OUT"
cp hactool "$OUT/hactool"
llvm-strip "$OUT/hactool"
cp LICENSE "$OUT/LICENSE"
rm -rf "$B"
echo "hactool: $OUT/hactool ($(stat -c %s "$OUT/hactool") bytes)"
