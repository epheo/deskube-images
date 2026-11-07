/*
 * deskube-uinput: gives a streaming host a /dev/uinput that never
 * touches the kernel.
 *
 * Sunshine injects input by creating uinput devices. Those are global
 * to the kernel: in a container they would show up on the node, be
 * consumed by whoever sits at its console, and stay invisible to the
 * headless compositor next to Sunshine. Preloaded into Sunshine, this
 * library answers the uinput calls itself and replays the events into
 * the compositor over the Wayland virtual pointer and virtual keyboard
 * protocols. Input then exists only inside the session it belongs to.
 *
 * Keyboard and mouse only. Touch and pen fail at creation, which the
 * host reports as "not supported".
 *
 * Gamepads are the exception: no Wayland protocol carries them, games
 * read them from kernel devices. When the launcher passed the real
 * /dev/uinput in, a gamepad is created there for real. Otherwise it
 * becomes a socket pad: its events go as datagrams to a socket a game
 * may bind, see send_pad(). A game that does not stays without a pad.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <linux/input.h>
#include <linux/uinput.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#define EXPORT __attribute__((visibility("default")))
#define UINPUT_PATH "/dev/uinput"
#define MAX_DEVICES 16
/* One wheel detent is 120 hi-res units and scrolls 15 px, the libinput convention. */
#define PIXELS_PER_DETENT 15.0
#define PAD_AXES 16
#define PAD_BATCH 64
#define PAD_MAGIC 0x44504b44 /* "DKPD" little endian */
#define PAD_EVENTS 1
#define PAD_GONE 2

enum kind { KIND_UNKNOWN, KIND_KEYBOARD, KIND_REL_POINTER, KIND_ABS_POINTER, KIND_GAMEPAD };

/* A setup call, kept so a gamepad can be replayed on the kernel's uinput. */
struct call {
	unsigned long request;
	long value;
	void *blob; /* copy of what a pointer argument pointed to */
};

struct device {
	int fd; /* 0 means the slot is free */
	pid_t owner;
	int open_flags;
	bool created;
	bool passthrough; /* fd is the kernel's uinput now, every call goes there */
	enum kind kind;
	char name[UINPUT_MAX_NAME_SIZE];
	bool has_key_a, has_btn_left, has_btn_south, has_rel_x, has_abs_x, has_multitouch, has_touch_or_pen;
	struct call *calls;
	int ncalls, calls_cap;
	bool calls_lost;
	int abs_min[2], abs_max[2];

	struct zwlr_virtual_pointer_v1 *pointer;
	struct zwp_virtual_keyboard_v1 *keyboard;
	struct xkb_state *xkb;
	uint8_t down[KEY_CNT / 8 + 1];

	/* Accumulated until SYN_REPORT, like the kernel does. */
	int dx, dy, abs_x, abs_y;
	bool moved_abs;
	int wheel[2], wheel_hires[2];
	bool has_hires[2];

	/* A socket pad: its number, axis ranges, and events since the last SYN_REPORT. */
	int pad_sock, pad_hold; /* 0 when unused: the slot is zeroed */
	uint16_t pad_id;
	int naxes;
	struct { uint16_t code; int32_t min, max; } axes[PAD_AXES];
	int nbatch;
	struct { uint16_t type, code; int32_t value; } batch[PAD_BATCH];
};

static struct device devices[MAX_DEVICES];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int next_sysname;

static struct wl_display *display;
static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *pointer_manager;
static uint32_t pointer_version;
static struct zwp_virtual_keyboard_manager_v1 *keyboard_manager;
static struct xkb_context *xkb_ctx;
static struct xkb_keymap *xkb_map;

static int (*real_open)(const char *, int, ...);
static int (*real_open64)(const char *, int, ...);
static int (*real_access)(const char *, int);
static int (*real_ioctl)(int, unsigned long, ...);
static ssize_t (*real_write)(int, const void *, size_t);
static int (*real_close)(int);

static void say(const char *fmt, ...)
{
	va_list ap;
	fputs("deskube-uinput: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static uint32_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/*
 * Lock free on purpose: write() and close() of the whole process pass here.
 *
 * A child forked to run a command inherits the descriptors and closes
 * them all before exec. It also shares our Wayland socket, so reacting
 * to that close would destroy the parent's devices in the compositor.
 * Only the process that opened a device may act on it. getpid() also
 * tells a vfork child apart, which shares this very memory.
 */
static struct device *find(int fd)
{
	if (fd <= 0)
		return NULL;
	for (int i = 0; i < MAX_DEVICES; i++)
		if (devices[i].fd == fd)
			return devices[i].owner == getpid() ? &devices[i] : NULL;
	return NULL;
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
			    const char *interface, uint32_t version)
{
	(void)data;
	if (!strcmp(interface, wl_seat_interface.name) && !seat) {
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
	} else if (!strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name)) {
		/* axis_discrete only exists from version 2; sending it to a v1 object is fatal. */
		pointer_version = version < 2 ? version : 2;
		pointer_manager = wl_registry_bind(registry, name,
						   &zwlr_virtual_pointer_manager_v1_interface, pointer_version);
	}
	else if (!strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name))
		keyboard_manager = wl_registry_bind(registry, name,
						    &zwp_virtual_keyboard_manager_v1_interface, 1);
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

/* A connection of our own; the host keeps its own for capture. */
static bool wayland_ready(void)
{
	if (display)
		return seat && pointer_manager && keyboard_manager;

	display = wl_display_connect(NULL);
	if (!display) {
		say("no Wayland display (WAYLAND_DISPLAY=%s)", getenv("WAYLAND_DISPLAY") ?: "unset");
		return false;
	}
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);
	if (!seat || !pointer_manager || !keyboard_manager) {
		say("compositor lacks %s%s%s",
		    seat ? "" : "wl_seat ",
		    pointer_manager ? "" : "wlr-virtual-pointer ",
		    keyboard_manager ? "" : "virtual-keyboard");
		return false;
	}
	return true;
}

/* A mistyped layout must not cost the session its keyboard. */
static struct xkb_keymap *keymap(void)
{
	const char *layout = getenv("DESKUBE_KEYBOARD_LAYOUT");
	struct xkb_rule_names names = {.layout = layout && *layout ? layout : "us"};

	if (xkb_map)
		return xkb_map;
	if (!xkb_ctx)
		xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!xkb_ctx)
		return NULL;
	xkb_map = xkb_keymap_new_from_names(xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!xkb_map && strcmp(names.layout, "us")) {
		say("no xkb layout %s, falling back to us", names.layout);
		names.layout = "us";
		xkb_map = xkb_keymap_new_from_names(xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
	if (!xkb_map)
		say("cannot build an xkb keymap for layout %s", names.layout);
	return xkb_map;
}

/* Requests go out now; replies are drained so the socket never backs up. */
static void pump(void)
{
	if (!display)
		return;
	wl_display_flush(display);
	if (wl_display_prepare_read(display) == 0) {
		struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLIN};
		if (poll(&pfd, 1, 0) > 0)
			wl_display_read_events(display);
		else
			wl_display_cancel_read(display);
	}
	wl_display_dispatch_pending(display);
}

static bool attach_keyboard(struct device *dev)
{
	struct xkb_keymap *map = keymap();
	char *text = map ? xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
	if (!text)
		return false;
	size_t size = strlen(text) + 1;
	int fd = memfd_create("deskube-keymap", MFD_CLOEXEC);
	bool ok = fd >= 0 && real_write(fd, text, size) == (ssize_t)size;
	free(text);
	if (!ok) {
		if (fd >= 0)
			real_close(fd);
		return false;
	}
	dev->keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(keyboard_manager, seat);
	zwp_virtual_keyboard_v1_keymap(dev->keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
	real_close(fd);
	dev->xkb = xkb_state_new(map);
	return dev->xkb != NULL;
}

static void record_call(struct device *dev, unsigned long request, void *arg)
{
	size_t size = 0;

	if (dev->ncalls == dev->calls_cap) {
		struct call *grown = realloc(dev->calls, (dev->calls_cap + 128) * sizeof(*grown));
		if (!grown) {
			dev->calls_lost = true;
			return;
		}
		dev->calls = grown;
		dev->calls_cap += 128;
	}
	/* The bit setters pass their argument by value, these three by pointer. */
	if (request == UI_DEV_SETUP || request == UI_ABS_SETUP)
		size = _IOC_SIZE(request);
	else if (request == UI_SET_PHYS)
		size = strlen(arg) + 1;

	struct call *call = &dev->calls[dev->ncalls];
	call->request = request;
	call->value = (long)(intptr_t)arg;
	call->blob = NULL;
	if (size) {
		call->blob = malloc(size);
		if (!call->blob) {
			dev->calls_lost = true;
			return;
		}
		memcpy(call->blob, arg, size);
	}
	dev->ncalls++;
}

static void forget_calls(struct device *dev)
{
	for (int i = 0; i < dev->ncalls; i++)
		free(dev->calls[i].blob);
	free(dev->calls);
	dev->calls = NULL;
	dev->ncalls = dev->calls_cap = 0;
}

static const char *pad_path(void)
{
	static char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	const char *env = getenv("DESKUBE_PAD_SOCKET");
	const char *run = getenv("XDG_RUNTIME_DIR");

	if (env && *env)
		snprintf(path, sizeof(path), "%s", env);
	else if (run && *run)
		snprintf(path, sizeof(path), "%s/deskube-pads", run);
	else
		return NULL;
	return path;
}

/*
 * One datagram per batch, to whoever bound the pad socket:
 *   u32 magic, u16 version 1, u16 pad, u8 kind, u8 naxes, u16 nevents,
 *   naxes x {u16 code, u16 0, i32 min, i32 max},
 *   nevents x {u16 type, u16 code, i32 value}, little endian.
 * Every batch carries the ranges, so a game that starts or restarts
 * mid stream reads the pad from its next move. Nobody listening, or a
 * full queue, drops the batch: a pad is state, the next one catches up.
 */
static void send_pad(struct device *dev, uint8_t kind)
{
	const char *path = pad_path();
	unsigned char buf[16 + PAD_AXES * 12 + PAD_BATCH * 8];
	size_t n = 0;
	struct sockaddr_un to = {.sun_family = AF_UNIX};

	if (!path || dev->pad_sock <= 0)
		return;
	snprintf(to.sun_path, sizeof(to.sun_path), "%s", path);
	uint32_t magic = PAD_MAGIC;
	uint16_t version = 1, nev = kind == PAD_EVENTS ? (uint16_t)dev->nbatch : 0;
	uint8_t naxes = (uint8_t)dev->naxes;
	memcpy(buf + n, &magic, 4), n += 4;
	memcpy(buf + n, &version, 2), n += 2;
	memcpy(buf + n, &dev->pad_id, 2), n += 2;
	buf[n++] = kind;
	buf[n++] = naxes;
	memcpy(buf + n, &nev, 2), n += 2;
	memset(buf + n, 0, 4), n += 4;
	for (int i = 0; i < dev->naxes; i++) {
		uint16_t zero = 0;
		memcpy(buf + n, &dev->axes[i].code, 2), n += 2;
		memcpy(buf + n, &zero, 2), n += 2;
		memcpy(buf + n, &dev->axes[i].min, 4), n += 4;
		memcpy(buf + n, &dev->axes[i].max, 4), n += 4;
	}
	for (int i = 0; i < nev; i++) {
		memcpy(buf + n, &dev->batch[i].type, 2), n += 2;
		memcpy(buf + n, &dev->batch[i].code, 2), n += 2;
		memcpy(buf + n, &dev->batch[i].value, 4), n += 4;
	}
	sendto(dev->pad_sock, buf, n, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)&to, sizeof(to));
	dev->nbatch = 0;
}

static void pad_event(struct device *dev, const struct input_event *ev)
{
	if (ev->type == EV_SYN) {
		if (ev->code == SYN_REPORT && dev->nbatch)
			send_pad(dev, PAD_EVENTS);
		return;
	}
	if (ev->type != EV_KEY && ev->type != EV_ABS)
		return;
	if (dev->nbatch == PAD_BATCH)
		send_pad(dev, PAD_EVENTS);
	dev->batch[dev->nbatch].type = ev->type;
	dev->batch[dev->nbatch].code = ev->code;
	dev->batch[dev->nbatch].value = ev->value;
	dev->nbatch++;
}

/* The ranges a game needs to read the sticks, from the recorded setup. */
static void keep_axes(struct device *dev)
{
	dev->naxes = 0;
	for (int i = 0; i < dev->ncalls && dev->naxes < PAD_AXES; i++) {
		const struct call *call = &dev->calls[i];
		if (call->request != UI_ABS_SETUP || !call->blob)
			continue;
		const struct uinput_abs_setup *abs = call->blob;
		dev->axes[dev->naxes].code = abs->code;
		dev->axes[dev->naxes].min = abs->absinfo.minimum;
		dev->axes[dev->naxes].max = abs->absinfo.maximum;
		dev->naxes++;
	}
}

static int create_socket_pad(struct device *dev)
{
	if (!pad_path()) {
		say("gamepad \"%s\": no real %s and no XDG_RUNTIME_DIR, refusing it", dev->name, UINPUT_PATH);
		errno = ENODEV;
		return -1;
	}
	/*
	 * The host polls a pad's descriptor for rumble. Ours is a memfd,
	 * always readable at its end: it would spin. The read end of a pipe
	 * nobody writes to never wakes it.
	 */
	int hold[2];
	if (pipe2(hold, O_CLOEXEC) < 0)
		return -1;
	if (dup3(hold[0], dev->fd, dev->open_flags & O_CLOEXEC) < 0) {
		real_close(hold[0]);
		real_close(hold[1]);
		return -1;
	}
	real_close(hold[0]);
	dev->pad_hold = hold[1];
	dev->pad_sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (dev->pad_sock < 0) {
		dev->pad_sock = 0;
		return -1;
	}
	dev->pad_id = (uint16_t)++next_sysname;
	keep_axes(dev);
	dev->created = true;
	say("\"%s\" is socket pad %d on %s", dev->name, dev->pad_id, pad_path());
	return 0;
}

/*
 * Builds the gamepad on the kernel's uinput and puts it behind the
 * descriptor the host already holds, so rumble reads and everything
 * after this reach the kernel without us. Without the real uinput it
 * becomes a socket pad.
 */
static int create_gamepad(struct device *dev)
{
	int saved;
	int real = dev->calls_lost ? -1 : real_open(UINPUT_PATH, dev->open_flags);

	if (real < 0)
		return create_socket_pad(dev);
	for (int i = 0; i < dev->ncalls; i++) {
		const struct call *call = &dev->calls[i];
		if (real_ioctl(real, call->request, call->blob ? call->blob : (void *)(intptr_t)call->value) < 0)
			goto fail;
	}
	if (real_ioctl(real, UI_DEV_CREATE, NULL) < 0)
		goto fail;
	if (dup3(real, dev->fd, dev->open_flags & O_CLOEXEC) < 0)
		goto fail;
	real_close(real);
	dev->passthrough = true;
	dev->created = true;
	say("\"%s\" created as a kernel gamepad", dev->name);
	return 0;
fail:
	saved = errno;
	say("cannot create gamepad \"%s\": %s", dev->name, strerror(saved));
	real_close(real);
	errno = saved;
	return -1;
}

static int create_device(struct device *dev)
{
	/* A keyboard may enable every key code, buttons included; axes tell it apart. */
	if (dev->has_key_a && !dev->has_rel_x && !dev->has_abs_x)
		dev->kind = KIND_KEYBOARD;
	else if (dev->has_multitouch || dev->has_touch_or_pen)
		dev->kind = KIND_UNKNOWN;
	else if (dev->has_btn_south && dev->has_abs_x && !dev->has_rel_x && !dev->has_btn_left)
		dev->kind = KIND_GAMEPAD;
	else if (dev->has_rel_x && dev->has_btn_left)
		dev->kind = KIND_REL_POINTER;
	else if (dev->has_abs_x && dev->has_btn_left)
		dev->kind = KIND_ABS_POINTER;

	if (dev->kind == KIND_GAMEPAD) {
		int ret = create_gamepad(dev);
		forget_calls(dev);
		return ret;
	}
	forget_calls(dev);
	if (dev->kind == KIND_UNKNOWN) {
		say("\"%s\" is not a keyboard or mouse, refusing it", dev->name);
		errno = ENODEV;
		return -1;
	}
	if (!wayland_ready()) {
		errno = ENODEV;
		return -1;
	}
	if (dev->kind == KIND_KEYBOARD) {
		if (!attach_keyboard(dev)) {
			errno = ENODEV;
			return -1;
		}
	} else {
		dev->pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(pointer_manager, seat);
	}
	dev->created = true;
	pump();
	say("\"%s\" attached to the compositor as a virtual %s", dev->name,
	    dev->kind == KIND_KEYBOARD ? "keyboard" : "pointer");
	return 0;
}

static void destroy_device(struct device *dev)
{
	if (dev->pad_sock > 0) {
		send_pad(dev, PAD_GONE);
		real_close(dev->pad_sock);
		dev->pad_sock = 0;
	}
	if (dev->pad_hold > 0) {
		real_close(dev->pad_hold);
		dev->pad_hold = 0;
	}
	if (dev->keyboard)
		zwp_virtual_keyboard_v1_destroy(dev->keyboard);
	if (dev->pointer)
		zwlr_virtual_pointer_v1_destroy(dev->pointer);
	if (dev->xkb)
		xkb_state_unref(dev->xkb);
	dev->keyboard = NULL;
	dev->pointer = NULL;
	dev->xkb = NULL;
	dev->created = false;
	pump();
}

static void note_capability(struct device *dev, unsigned int nr, int code)
{
	switch (nr) {
	case _IOC_NR(UI_SET_KEYBIT):
		if (code == KEY_A)
			dev->has_key_a = true;
		else if (code == BTN_LEFT)
			dev->has_btn_left = true;
		else if (code == BTN_SOUTH)
			dev->has_btn_south = true;
		else if (code == BTN_TOUCH || code == BTN_TOOL_PEN)
			dev->has_touch_or_pen = true;
		break;
	case _IOC_NR(UI_SET_RELBIT):
		if (code == REL_X)
			dev->has_rel_x = true;
		break;
	case _IOC_NR(UI_SET_ABSBIT):
		if (code == ABS_X)
			dev->has_abs_x = true;
		else if (code >= ABS_MT_SLOT)
			dev->has_multitouch = true;
		break;
	}
}

static int device_ioctl(struct device *dev, unsigned long request, void *arg)
{
	unsigned int nr = _IOC_NR(request);

	if (_IOC_TYPE(request) != UINPUT_IOCTL_BASE)
		return 0;
	if (request == UI_DEV_CREATE)
		return create_device(dev);
	if (!dev->created && request != UI_DEV_DESTROY && request != UI_GET_VERSION &&
	    nr != _IOC_NR(UI_GET_SYSNAME(0)))
		record_call(dev, request, arg);
	if (request == UI_DEV_DESTROY) {
		destroy_device(dev);
		return 0;
	}
	if (request == UI_DEV_SETUP) {
		const struct uinput_setup *setup = arg;
		snprintf(dev->name, sizeof(dev->name), "%s", setup->name);
		return 0;
	}
	if (request == UI_ABS_SETUP) {
		const struct uinput_abs_setup *abs = arg;
		if (abs->code == ABS_X || abs->code == ABS_Y) {
			dev->abs_min[abs->code] = abs->absinfo.minimum;
			dev->abs_max[abs->code] = abs->absinfo.maximum;
		}
		return 0;
	}
	if (request == UI_GET_VERSION) {
		/* 5 makes libevdev use UI_DEV_SETUP instead of the legacy write. */
		*(unsigned int *)arg = 5;
		return 0;
	}
	if (nr == _IOC_NR(UI_GET_SYSNAME(0))) {
		/* libevdev fails the whole creation without a sysname. */
		int n = snprintf(arg, _IOC_SIZE(request), "deskube%d", ++next_sysname);
		return n;
	}
	note_capability(dev, nr, (int)(intptr_t)arg);
	return 0;
}

static void key_event(struct device *dev, int code, int value)
{
	if (code < 0 || code >= KEY_CNT || value > 1)
		return;
	bool was_down = dev->down[code / 8] & (1 << (code % 8));
	/* The host re-sends presses of held keys; the compositor repeats on its own. */
	if ((value == 1) == was_down)
		return;
	if (value)
		dev->down[code / 8] |= 1 << (code % 8);
	else
		dev->down[code / 8] &= ~(1 << (code % 8));

	zwp_virtual_keyboard_v1_key(dev->keyboard, now_ms(), code,
				    value ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
	xkb_state_update_key(dev->xkb, code + 8, value ? XKB_KEY_DOWN : XKB_KEY_UP);
	zwp_virtual_keyboard_v1_modifiers(dev->keyboard,
		xkb_state_serialize_mods(dev->xkb, XKB_STATE_MODS_DEPRESSED),
		xkb_state_serialize_mods(dev->xkb, XKB_STATE_MODS_LATCHED),
		xkb_state_serialize_mods(dev->xkb, XKB_STATE_MODS_LOCKED),
		xkb_state_serialize_layout(dev->xkb, XKB_STATE_LAYOUT_EFFECTIVE));
}

static void scroll(struct device *dev, int axis, uint32_t wl_axis, double sign, uint32_t time)
{
	if (!dev->wheel[axis] && !dev->wheel_hires[axis])
		return;
	double detents = dev->has_hires[axis] ? dev->wheel_hires[axis] / 120.0 : dev->wheel[axis];
	double value = sign * detents * PIXELS_PER_DETENT;

	zwlr_virtual_pointer_v1_axis_source(dev->pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
	if (dev->wheel[axis] && pointer_version >= 2)
		zwlr_virtual_pointer_v1_axis_discrete(dev->pointer, time, wl_axis, wl_fixed_from_double(value),
						      (int32_t)(sign * dev->wheel[axis]));
	else
		zwlr_virtual_pointer_v1_axis(dev->pointer, time, wl_axis, wl_fixed_from_double(value));
	dev->wheel[axis] = dev->wheel_hires[axis] = 0;
	dev->has_hires[axis] = false;
}

static void pointer_frame(struct device *dev)
{
	uint32_t time = now_ms();

	if (dev->dx || dev->dy)
		zwlr_virtual_pointer_v1_motion(dev->pointer, time, wl_fixed_from_int(dev->dx),
					       wl_fixed_from_int(dev->dy));
	if (dev->moved_abs) {
		uint32_t ex = dev->abs_max[ABS_X] - dev->abs_min[ABS_X];
		uint32_t ey = dev->abs_max[ABS_Y] - dev->abs_min[ABS_Y];
		if (ex && ey)
			zwlr_virtual_pointer_v1_motion_absolute(dev->pointer, time,
				dev->abs_x - dev->abs_min[ABS_X], dev->abs_y - dev->abs_min[ABS_Y], ex, ey);
	}
	/* evdev wheels count up as positive, Wayland counts down as positive. */
	scroll(dev, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, -1.0, time);
	scroll(dev, 1, WL_POINTER_AXIS_HORIZONTAL_SCROLL, 1.0, time);
	zwlr_virtual_pointer_v1_frame(dev->pointer);
	dev->dx = dev->dy = 0;
	dev->moved_abs = false;
}

static void pointer_event(struct device *dev, const struct input_event *ev)
{
	if (ev->type == EV_KEY) {
		if (ev->value <= 1)
			zwlr_virtual_pointer_v1_button(dev->pointer, now_ms(), ev->code,
				ev->value ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
	} else if (ev->type == EV_REL) {
		switch (ev->code) {
		case REL_X: dev->dx += ev->value; break;
		case REL_Y: dev->dy += ev->value; break;
		case REL_WHEEL: dev->wheel[0] += ev->value; break;
		case REL_HWHEEL: dev->wheel[1] += ev->value; break;
		case REL_WHEEL_HI_RES: dev->wheel_hires[0] += ev->value; dev->has_hires[0] = true; break;
		case REL_HWHEEL_HI_RES: dev->wheel_hires[1] += ev->value; dev->has_hires[1] = true; break;
		}
	} else if (ev->type == EV_ABS) {
		if (ev->code == ABS_X)
			dev->abs_x = ev->value;
		else if (ev->code == ABS_Y)
			dev->abs_y = ev->value;
		dev->moved_abs = true;
	}
}

static void device_events(struct device *dev, const struct input_event *ev, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (ev[i].type == EV_SYN && dev->kind != KIND_GAMEPAD) {
			if (ev[i].code == SYN_REPORT) {
				if (dev->pointer)
					pointer_frame(dev);
				pump();
			}
		} else if (dev->kind == KIND_GAMEPAD) {
			pad_event(dev, &ev[i]);
		} else if (dev->keyboard) {
			if (ev[i].type == EV_KEY)
				key_event(dev, ev[i].code, ev[i].value);
		} else if (dev->pointer) {
			pointer_event(dev, &ev[i]);
		}
	}
}

/*
 * Commands and apps the host launches must not inherit the preload: a
 * game that opens /dev/uinput for its own virtual controller would get
 * this fake, and 32 bit programs cannot load the library at all.
 */
__attribute__((constructor)) static void drop_preload(void)
{
	const char *preload = getenv("LD_PRELOAD");
	if (preload && strstr(preload, "libdeskube-uinput") && !strchr(preload, ':') && !strchr(preload, ' '))
		unsetenv("LD_PRELOAD");
}

static void resolve(void)
{
	if (real_write)
		return;
	real_open = dlsym(RTLD_NEXT, "open");
	real_open64 = dlsym(RTLD_NEXT, "open64");
	real_access = dlsym(RTLD_NEXT, "access");
	real_ioctl = dlsym(RTLD_NEXT, "ioctl");
	real_close = dlsym(RTLD_NEXT, "close");
	real_write = dlsym(RTLD_NEXT, "write");
}

/* The descriptor only has to be a real, closable number of its own. */
static int open_uinput(int flags)
{
	int fd = memfd_create("deskube-uinput", (flags & O_CLOEXEC) ? MFD_CLOEXEC : 0);
	if (fd < 0)
		return -1;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < MAX_DEVICES; i++) {
		if (devices[i].fd == 0) {
			memset(&devices[i], 0, sizeof(devices[i]));
			devices[i].owner = getpid();
			devices[i].open_flags = flags;
			devices[i].fd = fd;
			pthread_mutex_unlock(&lock);
			return fd;
		}
	}
	pthread_mutex_unlock(&lock);
	real_close(fd);
	errno = EMFILE;
	return -1;
}

/* O_TMPFILE includes O_DIRECTORY: a plain & would read a mode nobody passed. */
static bool takes_mode(int flags)
{
	return (flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE;
}

EXPORT int access(const char *path, int mode)
{
	resolve();
	if (!strcmp(path, UINPUT_PATH))
		return 0;
	return real_access(path, mode);
}

EXPORT int open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	resolve();
	if (!strcmp(path, UINPUT_PATH))
		return open_uinput(flags);
	if (takes_mode(flags)) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	return real_open(path, flags, mode);
}

EXPORT int open64(const char *path, int flags, ...)
{
	mode_t mode = 0;
	resolve();
	if (!strcmp(path, UINPUT_PATH))
		return open_uinput(flags);
	if (takes_mode(flags)) {
		va_list ap;
		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}
	return real_open64(path, flags, mode);
}

EXPORT int ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);

	resolve();
	struct device *dev = find(fd);
	if (!dev || dev->passthrough)
		return real_ioctl(fd, request, arg);
	pthread_mutex_lock(&lock);
	int ret = device_ioctl(dev, request, arg);
	pthread_mutex_unlock(&lock);
	return ret;
}

EXPORT ssize_t write(int fd, const void *buf, size_t count)
{
	resolve();
	struct device *dev = find(fd);
	if (!dev || dev->passthrough)
		return real_write(fd, buf, count);
	pthread_mutex_lock(&lock);
	if (dev->created)
		device_events(dev, buf, count / sizeof(struct input_event));
	pthread_mutex_unlock(&lock);
	return (ssize_t)count;
}

EXPORT int close(int fd)
{
	resolve();
	struct device *dev = find(fd);
	if (dev) {
		pthread_mutex_lock(&lock);
		if (!dev->passthrough)
			destroy_device(dev);
		forget_calls(dev);
		dev->fd = 0;
		pthread_mutex_unlock(&lock);
	}
	return real_close(fd);
}
