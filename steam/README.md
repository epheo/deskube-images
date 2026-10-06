# Steam

Steam, its 32 bit userspace, and gamescope on the base image.
Steam Big Picture in gamescope is the only thing the stream shows.

    make steam
    PROFILE=steam hack/run.sh deskgame 49089
    hack/gametest/check.sh deskgame        # no Steam account needed
    APP=Steam SIZE=2560x1440 FPS=120 hack/e2e.sh deskgame 49089

The Sunshine app "Steam" starts Big Picture inside gamescope, at the
size and refresh rate the client asked for. Steam keeps running between
sessions. A later session with another client size makes the launcher
ask the running gamescope for that size (`GAMESCOPE_XWAYLAND_MODE_CONTROL`
on its X root); without that gamescope keeps its first size and scales
it. Its refresh rate stays the one it started with.

Result 2026-10-05, Ryzen AI Max+ 395, Radeon 8060S, Fedora 44:

- Steam's runtime containers (bubblewrap, user namespaces) start inside
  the rootless container. Big Picture reaches its sign in screen.
- A Windows Direct3D 11 program runs through Proton, DXVK, and RADV on
  the Radeon, inside the Steam Linux Runtime, inside gamescope: 120 fps
  at 2560x1440 on a 120 Hz output. The picture was checked on screen at
  1920x1080.
- A program that grabs the mouse like a first person game receives the
  exact relative motion, keys, wheel, and clicks sent through the input
  shim, as an X11 client, as a Wayland client, and under gamescope.
- Sunshine captures audio from the session's sink while Steam runs.
- Not tested here: a real game, which needs an account.

## Gamepads

No Wayland protocol carries a gamepad; games read them from kernel
devices. `GAMEPADS=1 hack/run.sh` passes the real `/dev/uinput` and
`/dev/input` into the desktop, and the shim then creates gamepads, and
only gamepads, on the kernel. Without the flag they are refused.

    PROFILE=steam GAMEPADS=1 hack/run.sh deskgame 49089
    hack/gametest/pad.sh deskgame 49089

`pad.sh` needs no controller: a virtual pad plays the client's, the
headless Moonlight client sends it, Sunshine recreates it, and an SDL
listener in the desktop received its 44 button presses and 91 stick
moves, hot plugged, on 2026-10-05.

- Every pad is an Xbox One pad (`gamepad = xone`). The DualSense
  emulation needs /dev/uhid, which is root only.
- No udev runs in the container. SDL finds pads that arrive later only
  with `SDL_JOYSTICK_DISABLE_UDEV=1`, which the image sets.
- No sudo was needed on this workstation: its logged in user already
  has access to uinput and to joystick nodes. A headless node will
  need a udev rule for both.
- Not isolated. A desktop started with the flag sees every gamepad on
  the machine, other desktops' included, and can create any input
  device on it, a keyboard at its console included. Fine for one
  owner, not for tenants.
- Not tested: rumble, Steam Input's own virtual pad, a real controller.

## What it needs from the launcher, and why

- `/proc` unmasked (`--security-opt unmask=ALL`). Steam's runtime
  mounts a fresh /proc, and the kernel refuses that while parts of the
  outer /proc are hidden. Nested user namespaces work without it. On a
  cluster the operator provides it through `spec.nestedSandboxes`.
- The GPU's primary node next to the render node. gamescope's Wayland
  backend refuses to start when the Vulkan device reports no primary
  node. Nothing opens it; it only has to exist.

## What bit

- A minimal Fedora lacks getopt, lspci, the DejaVu and Liberation fonts,
  and a UTF-8 locale. Without getopt Steam's web helper restarts in a
  loop and no window ever appears.
- Under plain sway a fullscreen window takes the fullscreen state from
  Big Picture and does not give it back when it closes. Inside
  gamescope Steam and its games are one sway window, and it is the
  only window there is.
- gamescope `--backend sdl` starts without the primary node but hung
  here: its window manager thread sat in a Vulkan present while its
  own window was not mapped in sway, and a client waiting for its
  window to map never got it.
- `swaymsg exec` splits its command on commas and semicolons.
- A console program under Wine gets a console window, and gamescope may
  show that instead of the game. Not a rendering failure.
