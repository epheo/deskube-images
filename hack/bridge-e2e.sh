#!/usr/bin/env bash
# Streams a desktop through the web bridge beside it, in one pod: the
# bridge is paired through its HTTP API and Sunshine's, and the video
# frames are counted on its WebSocket, where a browser would be.
# ./bridge-e2e.sh [name] [port]   CODEC=h264|hevc|av1  SIZE=1920x1080  FPS=60  SECS=20
# IMAGE and BRIDGE are the two images. RENDER is as in run.sh.
# CUBE=1 spins a Vulkan cube on the desktop, a moving picture.
# MIN_FPS is the rate the stream must reach, MAX_KEY_FRAMES what it may not pass.
# KEEP=1 leaves the pod and its volumes, and says where its page is.
# Every name starts with dkb-. Only the bridge's port leaves the pod,
# on 127.0.0.1: Sunshine is reached from inside.
set -euo pipefail
cd "$(dirname "$0")"
POD=dkb-${1:-bridge}; PORT=${2:-18080}
IMAGE=${IMAGE:-ghcr.io/epheo/deskube-desktop:${TAG:-main}}
BRIDGE=${BRIDGE:-ghcr.io/epheo/deskube-bridge:${TAG:-main}}
WORK=${WORK:-$(mktemp -d)}; mkdir -p "$WORK"
# Sunshine keeps its default port: nothing of it is published.
SUNSHINE=47989; WEB=$((SUNSHINE + 1))
HEADER=X-Deskube-User; PREFIX=/play/desk
API=http://127.0.0.1:$PORT$PREFIX/api
# What OpenShift's restricted-v2 gives a container: a UID the image does
# not know, in group 0, no capability, no privilege escalation.
DESK_UID=12345; BRIDGE_UID=23456
RESTRICTED=(--cap-drop ALL --security-opt no-new-privileges)

GPU_ARGS=()
case ${RENDER:-} in
  none) ;;
  "") GPU_ARGS=(--device /dev/dri/renderD128) ;;
  *) GPU_ARGS=(--device "$RENDER" -e DESKUBE_RENDER_NODE="$RENDER") ;;
esac

remove() {
  podman pod rm -f "$POD" > /dev/null 2>&1 || true
  podman volume rm "$POD-home" "$POD-state" > /dev/null 2>&1 || true
}
finish() {
  status=$?
  if [ "$status" != 0 ]; then
    podman logs --tail 30 "$POD-web" || true
    podman logs --tail 30 "$POD-desk" || true
    podman exec "$POD-desk" tail -n 30 /home/desk/.config/sunshine/sunshine.log || true
    echo "RESULT: FAIL"
  fi
  if [ "${KEEP:-}" != 1 ]; then remove; rm -f "$WORK/admin-password"; fi
  exit "$status"
}
trap finish EXIT
remove

# Group 0 reads the password, as it reads a Secret's file under fsGroup.
(umask 027 && openssl rand -hex 12 > "$WORK/admin-password")
podman pod create --name "$POD" --infra-name "$POD-infra" -p "127.0.0.1:$PORT:8080" > /dev/null
podman run -d --pod "$POD" --name "$POD-desk" --user "$DESK_UID:0" "${RESTRICTED[@]}" \
  "${GPU_ARGS[@]}" --security-opt label=disable \
  -v "$POD-home:/home/desk" -e DESKUBE_NAME="$POD" \
  -v "$WORK/admin-password:/run/deskube/admin/password:ro" -e DESKUBE_ADMIN_PASSWORD_FILE=/run/deskube/admin/password \
  "$IMAGE" > /dev/null
# The bridge writes nowhere but its state. podman would add a writable
# /tmp and /run to a read-only root; a cluster adds none.
podman run -d --pod "$POD" --name "$POD-web" --user "$BRIDGE_UID:0" "${RESTRICTED[@]}" \
  --read-only --read-only-tmpfs=false \
  -v "$POD-state:/moonlight-web/server" \
  -e BIND_ADDRESS=0.0.0.0:8080 -e FORWARDED_HEADER="$HEADER" -e PATH_PREFIX="$PREFIX" -e LOG_LEVEL=INFO \
  "$BRIDGE" > /dev/null

desk() { podman exec "$POD-desk" "$@"; }
# The session wrote Sunshine's admin login where only its UID reads it.
sunshine() { desk curl -sk -K "/tmp/run-$DESK_UID/sunshine-admin.curl" -H 'Content-Type: application/json' "$@"; }
# The bridge takes the user from this header, as from its reverse proxy.
api() { curl -sS -f -m 30 -H "$HEADER: e2e" -H 'Content-Type: application/json' "$@"; }
# One value from the first line of an answer: the bridge streams some.
field() {
  python3 -c '
import json, sys
v = json.loads(sys.stdin.read().splitlines()[0])
for k in sys.argv[1:]:
    v = v[int(k) if k.isdigit() else k]
print(v)' "$@"
}
up() {
  for _ in $(seq 1 60); do "$@" > /dev/null 2>&1 && return 0; sleep 1; done
  echo "no answer after 60 s: $*"; return 1
}

up desk curl -sf -m 2 "http://127.0.0.1:$SUNSHINE/serverinfo"
up curl -sf -m 2 "http://127.0.0.1:$PORT$PREFIX/config.js"
[ "$(curl -s -m 5 -o /dev/null -w '%{http_code}' "$API/hosts")" = 401 ] \
  || { echo "the bridge answers a caller who sets no $HEADER"; exit 1; }
# Upstream makes the first caller of /api/login the admin of a new
# bridge, header or not. The image's config turns that off. The desktop
# asks too: it shares the pod's network, where no NetworkPolicy stands.
LOGIN=(curl -s -m 5 -o /dev/null -w '%{http_code}' -H 'Content-Type: application/json' -d '{"name":"first","password":"first"}')
for caller in host desktop; do
  case $caller in
    host) code=$("${LOGIN[@]}" "$API/login") ;;
    desktop) code=$(desk "${LOGIN[@]}" "http://127.0.0.1:8080$PREFIX/api/login") ;;
  esac
  case $code in
    4*) ;;
    *) echo "/api/login without $HEADER, from the $caller: HTTP $code, a first login may have made an admin"; exit 1 ;;
  esac
done
echo "no admin by first login, from the host or from the desktop"
# Upstream's default hands every WebRTC attempt to Google's STUN servers.
# The bridge lists its ICE servers in Link headers and uses the same list.
web() { podman exec "$POD-web" /moonlight-web/web-server --config-path /moonlight-web/config.json "$@"; }
[ "$(web config print | python3 -c 'import json, sys; c = json.load(sys.stdin); print(c["webrtc"]["ice_servers"], c["web_server"]["first_login_create_admin"])')" = "[] False" ] \
  || { echo "the bridge's config names an ICE server, or lets a first login be admin"; exit 1; }
if api -D - -o /dev/null -X OPTIONS "$API/host/stream/webrtc" | grep -i '^link:'; then
  echo "the bridge hands a browser the ICE servers above"; exit 1
fi
echo "no ICE server in the bridge's config or in what it tells a browser"

HOST_ID=$(api -X POST "$API/host" -d "{\"address\":\"127.0.0.1\",\"http_port\":$SUNSHINE}" | field host host_id)
echo "host $HOST_ID added"

# The answer streams: the PIN at once, the outcome when Sunshine has it.
api -N -X POST "$API/pair" -d "{\"host_id\":$HOST_ID}" > "$WORK/pair.ndjson" &
PAIR=$!
PIN=""; ID=""
for _ in $(seq 1 30); do
  PIN=$(sed -n 's/^{"Pin":"\([0-9]*\)"}$/\1/p' "$WORK/pair.ndjson")
  [ -n "$PIN" ] && break; sleep 0.5
done
[ -n "$PIN" ] || { cat "$WORK/pair.ndjson"; echo "no PIN from the bridge"; exit 1; }
# The bridge's request can reach Sunshine just after its PIN reaches us.
for _ in $(seq 1 30); do
  ID=$(sunshine "https://127.0.0.1:$WEB/api/pin" | python3 -c 'import sys, json; p = json.load(sys.stdin).get("pairings", []); print(p[0]["id"] if p else "")' || true)
  [ -n "$ID" ] && break; sleep 0.5
done
[ -n "$ID" ] || { echo "the bridge never asked Sunshine to pair"; exit 1; }
sunshine -X POST "https://127.0.0.1:$WEB/api/pin" -d "{\"pairing_id\":\"$ID\",\"pin\":\"$PIN\",\"name\":\"bridge\"}" > /dev/null
wait "$PAIR"
grep -q '^{"Paired":' "$WORK/pair.ndjson" || { cat "$WORK/pair.ndjson"; echo "not paired"; exit 1; }
echo "paired with PIN $PIN"

# The pairing is in the state volume, not in the container.
podman restart "$POD-web" > /dev/null
up curl -sf -m 2 "http://127.0.0.1:$PORT$PREFIX/config.js"
[ "$(api "$API/hosts" | field hosts 0 paired)" = Paired ] || { echo "the pairing did not survive a restart of the bridge"; exit 1; }
echo "still paired after a restart of the bridge"

APP=${APP:-Desktop}
APP_ID=$(api "$API/apps?host_id=$HOST_ID" | python3 -c 'import sys, json; print(next((a["app_id"] for a in json.load(sys.stdin)["apps"] if a["title"] == sys.argv[1]), ""))' "$APP")
[ -n "$APP_ID" ] || { echo "Sunshine lists no app named $APP"; exit 1; }
echo "app $APP is $APP_ID"

[ "${CUBE:-}" != 1 ] || podman exec -d -e XDG_RUNTIME_DIR="/tmp/run-$DESK_UID" -e WAYLAND_DISPLAY=wayland-1 "$POD-desk" \
  sh -c 'exec vkcube > /tmp/vkcube.log 2>&1' > /dev/null
# The bridge is one process: /proc has its CPU time and its peak memory.
BRIDGE_PID=$(podman inspect -f '{{.State.Pid}}' "$POD-web")
ticks() { awk '{ print $14 + $15 }' "/proc/$BRIDGE_PID/stat"; }
TICKS=$(ticks); SINCE=$(date +%s)
SECS=${SECS:-20} ./bridge-frames.py "$API" "$HOST_ID" "$APP_ID" "$HEADER: e2e" 2>&1 | tee "$WORK/stream.log"
awk -v ticks="$(($(ticks) - TICKS))" -v hz="$(getconf CLK_TCK)" -v secs="$(($(date +%s) - SINCE))" \
  '/VmHWM/ { printf "bridge: %.1f %% of a CPU over %d s, %d MiB resident at most\n", 100 * ticks / hz / secs, secs, $2 / 1024 }' \
  "/proc/$BRIDGE_PID/status"

# A cube that died leaves a still desktop, and its rate says nothing of
# a moving picture.
if [ "${CUBE:-}" != 1 ]; then
  echo "picture: a still desktop"
elif desk pgrep -x vkcube > /dev/null; then
  echo "picture: vkcube, still turning after the stream"
else
  echo "picture: vkcube is not running"; desk cat /tmp/vkcube.log || true; exit 1
fi

# Closing the stream leaves the app running, as closing Moonlight does.
[ "${QUIT:-1}" = 0 ] || echo "app closed: $(api -X POST "$API/host/cancel" -d "{\"host_id\":$HOST_ID}")"

# The first packet proves little: a capture that restarts on every frame
# still sends one. MIN_FPS is low because a still desktop sends few frames.
GOT=$(sed -n 's/^Video frames received: .* s, \([0-9]*\)\..* fps.*/\1/p' "$WORK/stream.log")
[ "${GOT:-0}" -ge "${MIN_FPS:-5}" ] || { echo "${GOT:-no} fps received, wanted ${MIN_FPS:-5}"; exit 1; }
[ "${KEEP:-}" != 1 ] || echo "kept: http://127.0.0.1:$PORT$PREFIX/stream.html?hostId=$HOST_ID&appId=$APP_ID&dataTransport=websocket for a browser that sends $HEADER: e2e"
echo "RESULT: PASS"
