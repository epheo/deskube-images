#!/bin/sh
# Builds ptr in a Fedora container, static enough to copy into a desktop
# container (podman cp ptr <name>:/tmp/ptr). The protocol files are the
# shim's.
set -eu
cd "$(dirname "$0")"
podman run --rm --security-opt label=disable -v "$PWD/../..:/src:ro" -v "$PWD:/out" \
  registry.fedoraproject.org/fedora:latest sh -ec '
    dnf -y -q install gcc wayland-devel libxkbcommon-devel > /dev/null 2>&1
    cd /tmp
    for p in virtual-keyboard-unstable-v1 wlr-virtual-pointer-unstable-v1; do
      wayland-scanner client-header /src/desktop/shim/protocol/$p.xml $p.h
      wayland-scanner private-code /src/desktop/shim/protocol/$p.xml $p.c
    done
    ln -s wlr-virtual-pointer-unstable-v1.h vp.h; ln -s virtual-keyboard-unstable-v1.h vk.h
    gcc -O2 -Wall -I/tmp -o /out/ptr /src/hack/ptr/ptr.c *.c -lwayland-client -lxkbcommon -lm'
echo built hack/ptr/ptr
