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
 * /dev/uinput in, a gamepad is created there for real; otherwise it
 * fails like touch and pen.
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

	const char *layout = getenv("DESKUBE_KEYBOARD_LAYOUT");
	struct xkb_rule_names names = {.layout = layout && *layout ? layout : "us"};
	xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	xkb_map = xkb_ctx ? xkb_keymap_new_from_names(xkb_ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
	if (!xkb_map) {
		say("cannot build an xkb keymap for layout %s", names.layout);
		return false;
	}
	return true;
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
	char *text = xkb_keymap_get_as_string(xkb_map, XKB_KEYMAP_FORMAT_TEXT_V1);
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
	dev->xkb = xkb_state_new(xkb_map);
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

/*
 * Builds the gamepad on the kernel's uinput and puts it behind the
 * descriptor the host already holds, so rumble reads and everything
 * after this reach the kernel without us.
 */
static int create_gamepad(struct device *dev)
{
	int saved;
	int real = dev->calls_lost ? -1 : real_open(UINPUT_PATH, dev->open_flags);

	if (real < 0) {
		say("gamepad \"%s\" needs the real %s in this container, refusing it", dev->name, UINPUT_PATH);
		errno = ENODEV;
		return -1;
	}
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
		if (ev[i].type == EV_SYN) {
			if (ev[i].code == SYN_REPORT) {
				if (dev->pointer)
					pointer_frame(dev);
				pump();
			}
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
	if (flags & (O_CREAT | O_TMPFILE)) {
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
	if (flags & (O_CREAT | O_TMPFILE)) {
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
