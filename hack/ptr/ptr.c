/* Virtual pointer and keyboard for driving a container desktop from inside:
 *   ptr move DX DY [steps]   relative motion, spread over steps 8 ms apart
 *   ptr click [hold-ms]      left button, held; games poll, a tap is lost
 *   ptr key CODE[,CODE..] [hold-ms] [linger-ms]
 *                            evdev key codes on a us keymap, pressed in order and
 *                            released in reverse: 125,28 is Super+Enter. Linger
 *                            keeps the keyboard alive after: it is the seat's
 *                            active one, and a client that starts after it is
 *                            gone gets no keymap, which Chromium cannot take.
 * Speaks the same wlroots protocols as the input shim. Build: build.sh. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "vp.h"
#include "vk.h"
#include <sys/mman.h>
#include <xkbcommon/xkbcommon.h>

static struct zwlr_virtual_pointer_manager_v1 *mgr;
static struct wl_seat *seat;
static struct zwp_virtual_keyboard_manager_v1 *kmgr;

static void global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
	(void)d; (void)ver;
	if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name))
		mgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
	else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name))
		kmgr = wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
	else if (!strcmp(iface, wl_seat_interface.name) && !seat)
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener listener = { global, global_remove };

static uint32_t now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy || argc < 2) return 1;
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &listener, NULL);
	wl_display_roundtrip(dpy);
	if (!mgr) return 2;
	struct zwlr_virtual_pointer_v1 *p = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(mgr, seat);
	wl_display_roundtrip(dpy);
	if (!strcmp(argv[1], "move") && argc >= 4) {
		int dx = atoi(argv[2]), dy = atoi(argv[3]), steps = argc > 4 ? atoi(argv[4]) : 1;
		for (int i = 0; i < steps; i++) {
			zwlr_virtual_pointer_v1_motion(p, now(), wl_fixed_from_double((double)dx / steps), wl_fixed_from_double((double)dy / steps));
			zwlr_virtual_pointer_v1_frame(p);
			wl_display_roundtrip(dpy);
			if (steps > 1) usleep(8000);
		}
	} else if (!strcmp(argv[1], "click")) {
		int hold = argc > 2 ? atoi(argv[2]) : 150;
		zwlr_virtual_pointer_v1_button(p, now(), 0x110, 1);
		zwlr_virtual_pointer_v1_frame(p);
		wl_display_roundtrip(dpy);
		usleep(hold * 1000);
		zwlr_virtual_pointer_v1_button(p, now(), 0x110, 0);
		zwlr_virtual_pointer_v1_frame(p);
		wl_display_roundtrip(dpy);
	}
	else if (!strcmp(argv[1], "key") && argc >= 3 && kmgr) {
		/* a standard us keymap, so codes are real scancodes */
		int codes[8], n = 0, hold = argc > 3 ? atoi(argv[3]) : 150, linger = argc > 4 ? atoi(argv[4]) : 0;
		for (char *tok = strtok(argv[2], ","); tok && n < 8; tok = strtok(NULL, ","))
			codes[n++] = atoi(tok);
		struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		struct xkb_rule_names names = { .layout = "us" };
		struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
		char *str = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
		size_t len = strlen(str) + 1;
		int fd = memfd_create("keymap", 0);
		if (write(fd, str, len) != (ssize_t)len) return 3;
		struct zwp_virtual_keyboard_v1 *k = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(kmgr, seat);
		zwp_virtual_keyboard_v1_keymap(k, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, len);
		wl_display_roundtrip(dpy);
		/* The compositor takes the modifier state from the client, as the shim sends it. */
		struct xkb_state *st = xkb_state_new(km);
		for (int i = 0; i < 2 * n; i++) {
			int code = i < n ? codes[i] : codes[2 * n - 1 - i], down = i < n;
			zwp_virtual_keyboard_v1_key(k, now(), code, down);
			xkb_state_update_key(st, code + 8, down ? XKB_KEY_DOWN : XKB_KEY_UP);
			zwp_virtual_keyboard_v1_modifiers(k, xkb_state_serialize_mods(st, XKB_STATE_MODS_DEPRESSED),
				xkb_state_serialize_mods(st, XKB_STATE_MODS_LATCHED), xkb_state_serialize_mods(st, XKB_STATE_MODS_LOCKED),
				xkb_state_serialize_layout(st, XKB_STATE_LAYOUT_EFFECTIVE));
			wl_display_roundtrip(dpy);
			usleep(i == n - 1 ? hold * 1000 : 20000);
		}
		usleep((50 + linger) * 1000);
		zwp_virtual_keyboard_v1_destroy(k);
	}
	usleep(50000);
	zwlr_virtual_pointer_v1_destroy(p);
	wl_display_roundtrip(dpy);
	return 0;
}
