#!/usr/bin/env bash
# Runs a container desktop on this machine's GPU: ./run.sh [name] [base-port]
# PROFILE=steam picks the gaming image and what Steam and gamescope need.
# PRIMARY_NODE=1 passes only the GPU's primary node, for another image
# whose gamescope needs it.
# GAMEPADS=1 lets Sunshine create real gamepads, see below.
# Host network keeps the Sunshine ports identical inside and outside,
# which RTSP requires. NETWORK=<podman network> gives the desktop an
# address of its own instead and publishes the same ports: desktops on
# one network then see each other as machines on a LAN do.
# Only the render node is passed in. The home is a named volume, so
# pairings and the server certificate survive restarts.
set -euo pipefail
cd "$(dirname "$0")"
NAME=${1:-deskpod}; PORT=${2:-48989}
RENDER=${RENDER:-/dev/dri/renderD128}
PROFILE_ARGS=()
# See steam/Containerfile for the why of both.
if [ "${PROFILE:-}" = steam ] || [ "${PRIMARY_NODE:-}" = 1 ]; then
  CARD=$(basename "$(ls -d "/sys/class/drm/$(basename "$RENDER")/device/drm/card"* | head -1)")
  PROFILE_ARGS=(--device "/dev/dri/$CARD")
fi
if [ "${PROFILE:-}" = steam ]; then
  IMAGE=${IMAGE:-ghcr.io/epheo/deskube-steam:${TAG:-main}}
  PROFILE_ARGS+=(--security-opt unmask=ALL)
fi
IMAGE=${IMAGE:-ghcr.io/epheo/deskube-desktop:${TAG:-main}}
NET_ARGS=(--network host)
if [ -n "${NETWORK:-}" ]; then
  podman network exists "$NETWORK" || podman network create "$NETWORK" > /dev/null
  NET_ARGS=(--network "$NETWORK")
  for p in $((PORT - 5)) "$PORT" $((PORT + 1)) $((PORT + 21)); do NET_ARGS+=(-p "$p:$p/tcp"); done
  for p in $((PORT + 9)) $((PORT + 10)) $((PORT + 11)); do NET_ARGS+=(-p "$p:$p/udp"); done
fi
# GAMEPADS=1 gives the desktop the kernel's uinput. That is a privilege:
# anything in the desktop can then create input devices on this machine,
# a keyboard at its console included. Gamepads exist no other way.
[ -z "${GAMEPADS:-}" ] || PROFILE_ARGS+=(--device /dev/uinput -v /dev/input:/dev/input)
mkdir -p .rendered
PASS_FILE=.rendered/$NAME-sunshine-admin-password
[ -f "$PASS_FILE" ] || openssl rand -hex 12 > "$PASS_FILE"
chmod 600 "$PASS_FILE"
podman rm -f "$NAME" >/dev/null 2>&1 || true
podman run -d --name "$NAME" --hostname "$NAME" "${NET_ARGS[@]}" \
  --userns=keep-id --user 1000:1000 \
  --device "$RENDER" --security-opt label=disable --shm-size 1g \
  -v "$NAME-home:/home/desk" \
  -e DESKUBE_PORT="$PORT" -e DESKUBE_NAME="$NAME" \
  -v "$PWD/$PASS_FILE:/run/deskube/admin/password:ro" -e DESKUBE_ADMIN_PASSWORD_FILE=/run/deskube/admin/password \
  -e DESKUBE_ENCODER="${ENCODER:-vaapi}" -e DESKUBE_CAPTURE="${CAPTURE:-wlr}" \
  -e DESKUBE_RENDER_NODE="$RENDER" -e DESKUBE_KEYBOARD_LAYOUT="${LAYOUT:-us}" "${PROFILE_ARGS[@]}" ${EXTRA_ARGS:-} \
  "$IMAGE"
