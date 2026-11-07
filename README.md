# DeskuBe images

Container desktops for [DeskuBe](https://github.com/epheo/deskube): a headless Wayland
compositor on the host GPU's render node, streamed by Sunshine to a
Moonlight client. No kernel, no VM. The operator only knows these
images by name; everything about building and proving them is here.

| Directory            | Image                                    | What                                              |
| -------------------- | ---------------------------------------- | ------------------------------------------------- |
| `base/`              | `ghcr.io/epheo/deskube-base`             | sway, Sunshine, the input shim; nothing a user can reach |
| `desktop/`           | `ghcr.io/epheo/deskube-desktop`          | base plus sway's usual keys, a terminal, a launcher, Chromium |
| `steam/`             | `ghcr.io/epheo/deskube-steam`            | base plus Steam Big Picture in gamescope          |
| `moonlight-cli/`     | `moonlight-cli`                          | headless Moonlight that counts frames, for tests  |
| `hack/`              |                                          | run an image with podman, stream to it, drive it  |

Rules: the newest Fedora release, not a pinned one; Wayland only, no
Xorg (Xwayland only inside gamescope); no file capabilities, no
privilege the pod does not need. `make help` lists the targets.

    make desktop moonlight-cli       # desktop builds base first
    hack/run.sh deskpod 48989        # rootless, only /dev/dri/renderD128 passed in
    hack/e2e.sh deskpod 48989        # pair through the admin API, stream headless
    CODEC=av1 SIZE=2560x1440 hack/e2e.sh deskpod 48989

Sunshine web UI: https://<host>:48990, user admin, password in
`hack/.rendered/deskpod-sunshine-admin-password`. It answers private
addresses only; `DESKUBE_WEB_UI_ALLOWED` is `pc`, `lan` (the default)
or `wan`.

## The base image

With the host's render node it draws and encodes on the GPU (wlroots
capture, VAAPI). Without one it does both on the CPU (screen cast
portal, libx264). Its sway config puts nothing on the output: no bar,
no terminal, no bindings, a black background. An image that puts one
app on it leaves a user nothing to reach but that app, and the stream
is black until the app draws. The base is 1.36 GB: Mesa with LLVM
0.42 GB, Sunshine with Qt 0.1 GB; sway and swaybg are 4 MiB.

Result 2026-10-05, Ryzen AI Max+ 395, Radeon 8060S, Fedora 44:

- Sunshine finds h264_vaapi, hevc_vaapi, av1_vaapi from an unprivileged
  container; all three streamed 1080p60 to the headless client.
- Rendering is on the GPU: radeonsi for GL (glmark2 46499), RADV for Vulkan.
- The headless output takes any mode, so `deskube-resize`, run as a
  Sunshine prep command, sets exactly the size and refresh rate the
  client asked for and resets to 1920x1080 at 60 Hz afterwards. Tested
  at 1280x720@60, 2560x1440@120, and 3024x1964@60. No fixed list as on
  virtio-gpu. Like any prep command it runs when the app launches, not
  when a client resumes it.
- Six desktops at once, each with a Vulkan cube and a 1080p60 hardware
  encoded stream: GPU 16 %, load average 0.6.

Frames delivered, counted by the client (`hack/e2e.sh`; the first
packet alone proved nothing, as the CPU path showed), 2026-10-05 with a
moving picture:

    GPU      1920x1080  60 fps  H.264   60.0 fps
    GPU      2560x1440 120 fps  HEVC   120.1 fps
    GPU      1920x1080  60 fps  AV1     60.0 fps
    no GPU   1920x1080  60 fps  H.264   59.9 fps   as UID 12345, no capabilities

Sunshine's own frame processing time, capture to encoded packet, with a
Vulkan cube rendering in the desktop (`DESKUBE_LOG_LEVEL=debug`):

    1920x1080  60 fps  H.264   avg 2.2 ms   max 6.3 ms
    2560x1440 120 fps  HEVC    avg 3.4 ms   max 13.1 ms   GPU 11 %

The software encoder in a VM desktop took about 15 ms per frame.

What bit:

- Fedora's sway and Sunshine binaries carry file capabilities. An
  unprivileged container cannot hold them and exec fails with
  "Operation not permitted". The image strips them.
- Fedora's Mesa has no H.264 or HEVC in VAAPI. The image takes
  mesa-va-drivers-freeworld from RPM Fusion.
- Sunshine's web UI rejects writes, the PIN form included, from any
  origin but localhost. The session script lists the desktop's own
  addresses in `csrf_allowed_origins`; `DESKUBE_WEB_ORIGINS` adds more.
- Sunshine's wlr capture only takes GPU buffers. On a compositor that
  draws on the CPU it restarted the capture on every frame, delivered
  one frame in twenty seconds, and wrote a megabyte of log a minute.
  Without a render node the session uses `capture = portal` with
  xdg-desktop-portal-wlr, which hands over plain memory.
- The portal is started by the session bus, which came up before the
  compositor: `dbus-update-activation-environment` passes it the socket.
- Sunshine hung on the portal path twice: once at the end of a session
  (it then kills itself after ten seconds), once at start before it
  opened its port. Neither came back in a dozen later sessions. The
  session script therefore supervises Sunshine: the compositor is the
  session, Sunshine is restarted, and the desktop's programs stay.
- RPM Fusion's Mesa codecs must match Fedora's Mesa. On rawhide they
  often lag and the build fails, so the images follow the newest
  release instead.
- Without a reaping PID 1 the desktop collects zombies; the image runs
  tini.

## Input

Sunshine injects through uinput, and uinput devices are global to the
kernel: created from a container they land on the node, where the
console session consumes them and the headless compositor never sees
them. `base/shim` is a preload library that answers Sunshine's
uinput calls itself and replays the events into sway over the Wayland
virtual pointer and virtual keyboard protocols. No device, no
privilege, no rule on the node, and input cannot leave its session.

    podman exec -e XDG_RUNTIME_DIR=/tmp/run-1000 -e WAYLAND_DISPLAY=wayland-1 \
      -e LD_PRELOAD=/usr/lib64/libdeskube-uinput.so deskpod \
      /usr/libexec/deskube-uinput-test 'touch /tmp/deskube-typed'

drives it the way Sunshine's backend does. Keyboard, relative and
absolute mouse, wheel. `LAYOUT=fr hack/run.sh` picks the keymap; an
unknown one falls back to us. Touch and pen are refused, Sunshine
reports them as unsupported. Gamepads: see `steam/README.md`.

`hack/ptr` is the same idea as a command: a virtual pointer and
keyboard for driving a desktop from inside it during tests
(`hack/ptr/build.sh`, then `podman cp hack/ptr/ptr <name>:/tmp/ptr`).
Games poll their keys, so it holds them.

## The desktop image

sway with the keys most people know from it, foot as the terminal,
fuzzel as the launcher, Chromium. Mod4 is the Super key, which Moonlight
passes on in fullscreen ("capture system keyboard shortcuts").

    Super+Return        terminal           Super+d             launcher
    Super+Shift+Return  Chromium           Super+Shift+q       close window
    Super+1..5          workspace          Super+Shift+1..5    move window there
    Super+arrows, hjkl  focus              Super+Shift+same    move
    Super+f             fullscreen         Super+Shift+space   float
    Super+b, v          split              Super+s, w, e       stacking, tabbed, split
    Super+r             resize mode        Super+Tab           last workspace
    Super+minus         scratchpad         Super+Shift+c       reload the config

There is no exit binding: the compositor is the session, ending it ends
the pod. Chromium runs on Wayland, with no first run page and no
keyring; its sandbox needs user namespaces, which podman allows and
the operator's `nestedSandboxes` provides. Result 2026-10-06: 60.1 fps
at 1920x1080, Chromium and a terminal side by side.

Chromium crashes at start when the compositor's seat has no active
keyboard: it then gets `modifiers` without a `keymap` and dereferences
a null state. That happens when the keyboard it last saw was destroyed
while others remain; with the shim alone it cannot, since one keyboard
gone removes the capability. Test tools that bring their own keyboard
must keep it alive past the app's start (`hack/ptr`'s linger).

## The games

`steam/` sits on base, so there is no desktop
behind the game: the app list holds one entry, and the base's sway
config leaves the keyboard nothing to reach. The compositor stays sway
because it is 4 MiB of the image and the proven path for gamescope,
Sunshine's capture and the input shim. cage, the wlroots kiosk, would
give the same by construction (it has screencopy, virtual input and
output management) at the price of a second session path. gamescope
alone as the compositor is out: Sunshine cannot capture it.

## Open

- A real game from a Steam library, keyboard and mouse.
- Gamepads between tenants, set aside on 2026-10-05: a node agent that
  creates the devices and shows each desktop only its own, instead of
  the real uinput inside.
- A full desktop (Plasma with KWin capture) instead of sway. The shim
  speaks the wlroots protocols, KWin needs its own injection path.
