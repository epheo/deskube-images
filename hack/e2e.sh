#!/usr/bin/env bash
# Pairs the headless Moonlight client with a local container desktop
# through the Sunshine admin API and starts a stream.
# ./e2e.sh [name] [base-port]   CODEC=h264|hevc|av1  SIZE=1920x1080  FPS=60  APP=Desktop
# PAD=/dev/input/eventN makes the client send that gamepad instead of
# only watching; PAD_MAPPING is its SDL mapping line.
# MOONLIGHT=<image> streams with another build of moonlight-cli.
# QUIT=0 leaves the app running, as a user who closes Moonlight does.
set -euo pipefail
cd "$(dirname "$0")"
NAME=${1:-deskpod}; PORT=${2:-48989}
HOST=${HOST:-127.0.0.1}; WEB=$((PORT+1))
CODEC=${CODEC:-auto}; SIZE=${SIZE:-1920x1080}; FPS=${FPS:-60}; SECS=${SECS:-20}
PASS=$(cat ".rendered/$NAME-sunshine-admin-password")
WORK=${WORK:-$(mktemp -d)}
CACHE=$WORK/moonlight-cache; mkdir -p "$CACHE"
MOONLIGHT=${MOONLIGHT:-moonlight-cli}
ml() { podman run --rm --network host -v "$CACHE:/root/.cache/moonlight:Z" -i "$MOONLIGHT" "$@"; }
api() { curl -sk -u "admin:$PASS" -H 'Content-Type: application/json' "$@"; }

for _ in $(seq 1 30); do api "https://$HOST:$WEB/api/config" -o /dev/null && break; sleep 1; done
if [ ! -f "$CACHE/client.pem" ]; then
  ml pair -port "$PORT" "$HOST" > "$WORK/pair.log" 2>&1 &
  PAIR=$!
  PIN=""
  for _ in $(seq 1 30); do
    PIN=$(grep -o -E "PIN[^0-9]*[0-9]{4}" "$WORK/pair.log" | grep -o -E "[0-9]{4}$" | head -1 || true)
    [ -n "$PIN" ] && break; sleep 1
  done
  [ -n "$PIN" ] || { cat "$WORK/pair.log"; echo "no PIN from client"; exit 1; }
  ID=$(api "https://$HOST:$WEB/api/pin" | python3 -c 'import sys,json; p=json.load(sys.stdin).get("pairings",[]); print(p[0]["id"] if p else "")')
  api -X POST "https://$HOST:$WEB/api/pin" -d "{\"pairing_id\":\"$ID\",\"pin\":\"$PIN\",\"name\":\"e2e\"}" > /dev/null
  wait $PAIR || true
  tail -1 "$WORK/pair.log"
fi
INPUT_ARGS=(-viewonly); POD_ARGS=()
if [ -n "${PAD:-}" ]; then
  INPUT_ARGS=(-input "$PAD")
  POD_ARGS=(--security-opt label=disable -v /dev/input:/dev/input -e "SDL_GAMECONTROLLERCONFIG=$PAD_MAPPING")
fi
timeout "$SECS" podman run --rm --network host -v "$CACHE:/root/.cache/moonlight:Z" "${POD_ARGS[@]}" -i --entrypoint stdbuf "$MOONLIGHT" \
  -oL -eL moonlight stream -port "$PORT" -platform fake "${INPUT_ARGS[@]}" -app "${APP:-Desktop}" \
  -width "${SIZE%x*}" -height "${SIZE#*x}" -fps "$FPS" -bitrate "${BITRATE:-20000}" -codec "$CODEC" -verbose "$HOST" \
  > "$WORK/stream.log" 2>&1 || true
[ "${QUIT:-1}" = 0 ] || timeout 20 podman run --rm --network host -v "$CACHE:/root/.cache/moonlight:Z" -i "$MOONLIGHT" quit -port "$PORT" "$HOST" > /dev/null 2>&1 || true
# podman run can return before the client's last lines are written;
# the quit above used to hide that.
for _ in $(seq 1 20); do grep -q "Video frames received" "$WORK/stream.log" && break; sleep 0.5; done
grep -E "Stream |first audio packet|Video frames|rror" "$WORK/stream.log" || true
echo "logs in $WORK"
# The first packet proves little: a capture that restarts on every frame
# still sends one. MIN_FPS is low because a still desktop sends few frames.
GOT=$(sed -n 's/.*Video frames received: .* s, \([0-9]*\)\..* fps.*/\1/p' "$WORK/stream.log" | head -1)
[ "${GOT:-0}" -ge "${MIN_FPS:-5}" ] || { echo "RESULT: FAIL, ${GOT:-no} fps received, wanted ${MIN_FPS:-5}"; exit 1; }
# A session forks prep commands; the input devices must survive that.
if podman container exists "$NAME" 2>/dev/null; then
  INPUTS=$(podman exec "$NAME" sh -c 'SWAYSOCK=$(ls /tmp/run-*/sway-ipc.*.sock | head -1) swaymsg -t get_inputs | grep -c "\"identifier\""' || true)
  echo "virtual input devices after the session: $INPUTS"
  [ "${INPUTS:-0}" -ge 3 ] || { echo "RESULT: FAIL, input devices lost"; exit 1; }
fi
echo "RESULT: PASS"
