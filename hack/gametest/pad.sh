#!/usr/bin/env bash
# Proves the gamepad path end to end without a controller:
# ./pad.sh [name] [base-port], on a desktop started with GAMEPADS=1.
# A virtual pad plays the client's controller, the headless Moonlight
# client sends it, Sunshine recreates it, and a listener inside the
# desktop must receive its buttons and stick.
set -euo pipefail
cd "$(dirname "$0")"
NAME=${1:-deskgame}; PORT=${2:-49089}
IMAGE=${IMAGE:-registry.desku.be/deskube/desktop:dev}
BUILD=${BUILD:-$(mktemp -d)}
podman run --rm --security-opt label=disable -v "$PWD:/src:ro" -v "$BUILD:/out" \
  registry.fedoraproject.org/fedora:latest sh -ec '
    dnf -y -q install gcc sdl2-compat-devel >/dev/null 2>&1
    gcc -O2 -Wall -o /out/padtest /src/padtest.c $(pkg-config --cflags --libs sdl2)'
podman cp "$BUILD/padtest" "$NAME:/tmp/padtest"

# The client's controller. It only clicks and nudges the right stick.
podman rm -f padsrc >/dev/null 2>&1 || true
podman run -d --name padsrc --userns=keep-id --user 1000:1000 --security-opt label=disable \
  --device /dev/uinput --entrypoint sh "$IMAGE" -c \
  'LD_PRELOAD=/usr/lib64/libdeskube-uinput.so exec /usr/libexec/deskube-uinput-test --pad 40' >/dev/null
trap 'podman rm -f padsrc >/dev/null 2>&1 || true' EXIT
PAD=""
for _ in $(seq 1 20); do
  PAD=$(podman logs padsrc 2>&1 | sed -n 's/^node //p' | head -1)
  [ -n "$PAD" ] && break; sleep 0.5
done
[ -n "$PAD" ] || { podman logs padsrc; echo "no client pad"; exit 1; }
echo "client pad: $PAD"

podman exec -d "$NAME" sh -c 'rm -f /tmp/padtest.out; /tmp/padtest 30 /tmp/padtest.out'
# Indexes follow the order of the button and axis codes uinput-test enables.
MAPPING="030000005e040000ea02000008040000,Test gamepad,a:b0,b:b1,y:b2,x:b3,leftshoulder:b4,rightshoulder:b5,back:b6,start:b7,guide:b8,leftstick:b9,rightstick:b10,leftx:a0,lefty:a1,lefttrigger:a2,rightx:a3,righty:a4,righttrigger:a5,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,platform:Linux,"
PAD="$PAD" PAD_MAPPING="$MAPPING" SECS=${SECS:-22} WORK=${WORK:-$(mktemp -d)} ../e2e.sh "$NAME" "$PORT" | tail -4
for _ in $(seq 1 40); do podman exec "$NAME" test -s /tmp/padtest.out && break; sleep 1; done
R=$(podman exec "$NAME" cat /tmp/padtest.out 2>/dev/null || true)
echo "listener: ${R:-no result}"
podman exec "$NAME" sh -c 'grep -iE "gamepad|controller" ~/.config/sunshine/sunshine.log | tail -4'
# The client's own pad is visible in the desktop too and delivers its
# events directly; only the other node proves the path through Sunshine.
if echo "$R" | tr '|' '\n' | grep -v " $PAD " | grep -qE "buttons=[1-9][0-9]* axes=[1-9]"; then
  echo "RESULT: PASS"
else
  echo "RESULT: FAIL"; exit 1
fi
