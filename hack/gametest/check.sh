#!/usr/bin/env bash
# Proves the game path of a running gaming desktop without a Steam
# account: ./check.sh [name]
# - grabbed mouse, keys, wheel, and click through X11, Wayland, and gamescope
# - a Direct3D 11 program through Proton and gamescope, checked on screen
# The Proton part downloads about 1.2 GB into the desktop's home, once.
set -euo pipefail
cd "$(dirname "$0")"
NAME=${1:-deskgame}
BUILD=${BUILD:-$(mktemp -d)}
podman run --rm --security-opt label=disable -v "$PWD:/src:ro" -v "$BUILD:/out" \
  registry.fedoraproject.org/fedora:latest sh -ec '
    dnf -y -q install gcc sdl2-compat-devel mingw64-gcc >/dev/null 2>&1
    gcc -O2 -Wall -o /out/relmouse /src/relmouse.c $(pkg-config --cflags --libs sdl2)
    x86_64-w64-mingw32-gcc -O2 -Wall -mwindows -static -o /out/d3d11test.exe /src/d3d11test.c \
      -ld3d11 -ldxgi -ldxguid -luuid -lm'
podman cp "$BUILD/relmouse" "$NAME:/tmp/relmouse"
podman cp "$BUILD/d3d11test.exe" "$NAME:/tmp/d3d11test.exe"
podman cp guest.sh "$NAME:/tmp/gametest-guest.sh"
podman exec -e UMU_VERSION="${UMU_VERSION:-1.4.4}" -e SKIP_PROTON="${SKIP_PROTON:-}" "$NAME" sh /tmp/gametest-guest.sh
