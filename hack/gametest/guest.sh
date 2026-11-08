#!/bin/sh
# Runs inside the desktop, started by check.sh.
XDG_RUNTIME_DIR=/tmp/run-$(id -u)
# shellcheck disable=SC2010
WAYLAND_DISPLAY=$(ls "$XDG_RUNTIME_DIR" | grep -E '^wayland-[0-9]+$' | head -1)
# shellcheck disable=SC2012
SWAYSOCK=$(ls "$XDG_RUNTIME_DIR"/sway-ipc.*.sock | head -1)
export XDG_RUNTIME_DIR WAYLAND_DISPLAY SWAYSOCK
FAIL=0
if pgrep -x gamescope-wl >/dev/null; then
  echo "Steam's gamescope is running; the test's would land on the same workspace. Stop Steam first."
  exit 2
fi

# Everything is started through sway so it gets the session's DISPLAY.
# sway splits its commands on commas and semicolons: keep them out.
input_case() {
  rm -f /tmp/relmouse.out
  swaymsg workspace number "$2" >/dev/null
  swaymsg exec -- "$3 /tmp/relmouse 8 /tmp/relmouse.out" >/dev/null
  sleep 4
  LD_PRELOAD=/usr/lib64/libdeskube-uinput.so /usr/libexec/deskube-uinput-test abc >/dev/null 2>&1
  sleep 6
  R=$(cat /tmp/relmouse.out 2>/dev/null)
  # X11 clients spend the first of the three scroll steps learning the axis.
  if echo "$R" | grep -q "dx=200 dy=100 keys=4" && echo "$R" | grep -qE "wheel=-[23] buttons=1"; then
    echo "PASS input $1: $R"
  else
    echo "FAIL input $1: ${R:-no result}"; FAIL=1
  fi
}
input_case x11 1 "env SDL_VIDEODRIVER=x11"
input_case wayland 1 "env SDL_VIDEODRIVER=wayland"
# Same size inside and outside, as deskube-steam runs it. With a scaled
# inner size the first motion carried a large jump (seen: 626,340).
input_case gamescope 2 "gamescope -f -W 1920 -H 1080 -w 1920 -h 1080 --"

if [ -z "${SKIP_PROTON:-}" ]; then
  mkdir -p ~/gametest && cd ~/gametest || exit 1
  [ -x umu/umu-run ] || curl -sL "https://github.com/Open-Wine-Components/umu-launcher/releases/download/$UMU_VERSION/umu-launcher-$UMU_VERSION-zipapp.tar" | tar x
  cp /tmp/d3d11test.exe . && rm -f d3d11test.out
  cat > run.sh <<'RUN'
#!/bin/sh
cd ~/gametest
export GAMEID=umu-default WINEPREFIX=$HOME/gametest/prefix
exec gamescope -f -W 1920 -H 1080 -w 1920 -h 1080 -r 60 -- ./umu/umu-run d3d11test.exe 10 d3d11test.out > /tmp/gametest-proton.log 2>&1
RUN
  chmod +x run.sh
  swaymsg workspace number 2 >/dev/null
  swaymsg exec -- ~/gametest/run.sh >/dev/null
  SEEN=""
  for _ in $(seq 1 600); do
    # The program creates the file at start and fills it at exit.
    [ -s d3d11test.out ] && break
    PX=$(grim -t ppm -g "960,540 1x1" - 2>/dev/null | tail -c 3 | od -An -tu1 | tr -s ' ')
    G=$(echo "$PX" | awk '{print $2}')
    if [ -n "$G" ] && [ "$G" -ge 49 ] && [ "$G" -le 53 ]; then
      case "$SEEN" in *"|$PX|"*) ;; *) SEEN="$SEEN|$PX|";; esac
    fi
    sleep 1
  done
  sleep 1
  R=$(cat d3d11test.out 2>/dev/null)
  COLOURS=$(echo "$SEEN" | tr '|' '\n' | grep -c .)
  if echo "$R" | grep -q "frames=" && [ "$COLOURS" -ge 2 ]; then
    echo "PASS proton: $R, $COLOURS distinct colours seen on screen"
  else
    echo "FAIL proton: ${R:-no result}, $COLOURS colours seen on screen, log in /tmp/gametest-proton.log"; FAIL=1
  fi
fi
swaymsg workspace number 1 >/dev/null
exit $FAIL
