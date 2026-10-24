#!/usr/bin/env bash
# Builds dist/p4d-x86_64.AppImage: the port's current build (run build.sh first),
# its scripts and the launcher, bundled with their Nix closure (nix-appimage: the AppImage mounts
# its /nix/store with user namespaces, available on SteamOS and most desktops).
# Running it opens the launcher; `--play` starts the game with the launcher's saved settings.
# Data (generated files, saves, p4d.ini): ~/.local/share/p4d (P4D_DATA_DIR).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
[[ -f out/p4d-probe && -f out/gpu/libp4dgpu.so ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
root=$PWD
# The libraries' store paths (RUNPATH entries and their closures come along).
{
    echo '['
    for elf in out/p4d-probe out/gpu/libp4dgpu.so; do
        readelf -d "$elf" | sed -n 's/.*\[\(.*\)\]/\1/p' | tr ':' '\n'
    done | grep -o '^/nix/store/[^/]*' | sort -u | grep -v -- '-nix-shell$' | sed 's/.*/  "&"/'
    echo ']'
} > packaging/runtime-paths.nix
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
(cd "$work" && nix bundle --impure --bundler github:ralismark/nix-appimage \
    --expr "import $root/packaging {}")
mkdir -p dist
install -m755 "$work/p4d.AppImage" dist/p4d-x86_64.AppImage
ls -lh dist/p4d-x86_64.AppImage
