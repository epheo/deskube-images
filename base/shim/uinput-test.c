/*
 * Drives /dev/uinput the way Sunshine's input backend does: access(),
 * open(), libevdev setup on that descriptor, then one input_event per
 * write(). Run it under the shim to prove input reaches the compositor
 * without a Moonlight client:
 *
 *     LD_PRELOAD=/usr/lib64/libdeskube-uinput.so uinput-test 'touch /tmp/typed'
 *
 * "uinput-test --pad [rounds]" creates a gamepad instead, which the shim hands
 * to the kernel. It only clicks the right stick and nudges it: the
 * device is real and visible to the whole node while the test runs.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libevdev/libevdev-uinput.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>

static void emit(int fd, int type, int code, int value)
{
	struct input_event ev = {.type = type, .code = code, .value = value};
	if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
		perror("write");
}

static int key_for(char c)
{
	static const char *rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
	static const int first[] = {KEY_Q, KEY_A, KEY_Z};
	for (int r = 0; r < 3; r++) {
		const char *p = strchr(rows[r], c);
		if (p && c)
			return first[r] + (int)(p - rows[r]);
	}
	if (c >= '1' && c <= '9')
		return KEY_1 + (c - '1');
	switch (c) {
	case '0': return KEY_0;
	case ' ': return KEY_SPACE;
	case '/': return KEY_SLASH;
	case '-': return KEY_MINUS;
	case '.': return KEY_DOT;
	case '\n': return KEY_ENTER;
	}
	return -1;
}

static int make(const char *name, int kind, struct libevdev_uinput **out)
{
	if (access("/dev/uinput", R_OK | W_OK) != 0) {
		perror("access /dev/uinput");
		return -1;
	}
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		perror("open /dev/uinput");
		return -1;
	}
	struct libevdev *dev = libevdev_new();
	libevdev_set_name(dev, name);
	libevdev_set_id_bustype(dev, BUS_USB);
	libevdev_enable_event_type(dev, EV_KEY);
	if (kind == 0) {
		for (int k = KEY_ESC; k <= KEY_MICMUTE; k++)
			libevdev_enable_event_code(dev, EV_KEY, k, NULL);
	} else if (kind == 2) {
		static const int buttons[] = {BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR,
					      BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR};
		static const int sticks[] = {ABS_X, ABS_Y, ABS_RX, ABS_RY};
		struct input_absinfo stick = {.minimum = -32768, .maximum = 32767, .fuzz = 16, .flat = 128};
		struct input_absinfo trigger = {.minimum = 0, .maximum = 255};
		struct input_absinfo hat = {.minimum = -1, .maximum = 1};
		/* The ids of an Xbox One S pad, which every game library maps. */
		libevdev_set_id_vendor(dev, 0x045e);
		libevdev_set_id_product(dev, 0x02ea);
		libevdev_set_id_version(dev, 0x0408);
		for (size_t i = 0; i < sizeof(buttons) / sizeof(*buttons); i++)
			libevdev_enable_event_code(dev, EV_KEY, buttons[i], NULL);
		libevdev_enable_event_type(dev, EV_ABS);
		for (size_t i = 0; i < sizeof(sticks) / sizeof(*sticks); i++)
			libevdev_enable_event_code(dev, EV_ABS, sticks[i], &stick);
		libevdev_enable_event_code(dev, EV_ABS, ABS_Z, &trigger);
		libevdev_enable_event_code(dev, EV_ABS, ABS_RZ, &trigger);
		libevdev_enable_event_code(dev, EV_ABS, ABS_HAT0X, &hat);
		libevdev_enable_event_code(dev, EV_ABS, ABS_HAT0Y, &hat);
	} else {
		libevdev_enable_event_code(dev, EV_KEY, BTN_LEFT, NULL);
		libevdev_enable_event_code(dev, EV_KEY, BTN_RIGHT, NULL);
		libevdev_enable_event_type(dev, EV_REL);
		libevdev_enable_event_code(dev, EV_REL, REL_X, NULL);
		libevdev_enable_event_code(dev, EV_REL, REL_Y, NULL);
		libevdev_enable_event_code(dev, EV_REL, REL_WHEEL, NULL);
		libevdev_enable_event_code(dev, EV_REL, REL_WHEEL_HI_RES, NULL);
	}
	int rc = libevdev_uinput_create_from_device(dev, fd, out);
	libevdev_free(dev);
	if (rc < 0) {
		fprintf(stderr, "create %s: %s\n", name, strerror(-rc));
		close(fd);
		return -1;
	}
	return fd;
}

static int pad_main(int rounds)
{
	struct libevdev_uinput *pad_dev;
	int pad = make("Test gamepad", 2, &pad_dev);
	if (pad < 0)
		return 1;
	printf("node %s\n", libevdev_uinput_get_devnode(pad_dev) ?: "unknown");
	fflush(stdout);
	sleep(3);
	for (int round = 0; round < rounds; round++) {
		for (int i = 0; i < 3; i++) {
			emit(pad, EV_KEY, BTN_THUMBR, 1);
			emit(pad, EV_SYN, SYN_REPORT, 0);
			usleep(50 * 1000);
			emit(pad, EV_KEY, BTN_THUMBR, 0);
			emit(pad, EV_SYN, SYN_REPORT, 0);
			usleep(50 * 1000);
		}
		for (int i = 1; i <= 5; i++) {
			emit(pad, EV_ABS, ABS_RX, i * 2000);
			emit(pad, EV_SYN, SYN_REPORT, 0);
			usleep(20 * 1000);
		}
		emit(pad, EV_ABS, ABS_RX, 0);
		emit(pad, EV_SYN, SYN_REPORT, 0);
		if (round + 1 < rounds)
			sleep(1);
	}
	sleep(2);
	libevdev_uinput_destroy(pad_dev);
	close(pad);
	puts("pad events sent");
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--pad"))
		return pad_main(argc > 2 ? atoi(argv[2]) : 1);
	const char *text = argc > 1 ? argv[1] : "touch /tmp/deskube-typed";
	struct libevdev_uinput *kbd_dev, *mouse_dev;
	int kbd = make("Test keyboard", 0, &kbd_dev);
	int mouse = make("Test mouse", 1, &mouse_dev);
	if (kbd < 0 || mouse < 0)
		return 1;
	usleep(500 * 1000);

	for (int i = 0; i < 20; i++) {
		emit(mouse, EV_REL, REL_X, 10);
		emit(mouse, EV_REL, REL_Y, 5);
		emit(mouse, EV_SYN, SYN_REPORT, 0);
		usleep(5 * 1000);
	}
	/* X11 clients drop the first scroll event to learn the axis position. */
	for (int i = 0; i < 3; i++) {
		emit(mouse, EV_REL, REL_WHEEL, -1);
		emit(mouse, EV_REL, REL_WHEEL_HI_RES, -120);
		emit(mouse, EV_SYN, SYN_REPORT, 0);
		usleep(20 * 1000);
	}
	emit(mouse, EV_KEY, BTN_LEFT, 1);
	emit(mouse, EV_SYN, SYN_REPORT, 0);
	emit(mouse, EV_KEY, BTN_LEFT, 0);
	emit(mouse, EV_SYN, SYN_REPORT, 0);

	for (const char *p = text; *p; p++) {
		int key = key_for(*p);
		if (key < 0) {
			fprintf(stderr, "no key for '%c'\n", *p);
			continue;
		}
		emit(kbd, EV_KEY, key, 1);
		emit(kbd, EV_SYN, SYN_REPORT, 0);
		emit(kbd, EV_KEY, key, 0);
		emit(kbd, EV_SYN, SYN_REPORT, 0);
		usleep(10 * 1000);
	}
	emit(kbd, EV_KEY, KEY_ENTER, 1);
	emit(kbd, EV_SYN, SYN_REPORT, 0);
	emit(kbd, EV_KEY, KEY_ENTER, 0);
	emit(kbd, EV_SYN, SYN_REPORT, 0);
	usleep(300 * 1000);

	libevdev_uinput_destroy(kbd_dev);
	libevdev_uinput_destroy(mouse_dev);
	close(kbd);
	close(mouse);
	puts("events sent");
	return 0;
}
