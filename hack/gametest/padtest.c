/* Waits for game controllers the way a game does and reports what each one sent. */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_PADS 8

struct pad {
	SDL_JoystickID id;
	char name[96], path[64];
	int buttons, axes;
};

static struct pad pads[MAX_PADS];
static int npads;

static struct pad *by_id(SDL_JoystickID id)
{
	for (int i = 0; i < npads; i++)
		if (pads[i].id == id)
			return &pads[i];
	return NULL;
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 10;
	const char *out = argc > 2 ? argv[2] : "/tmp/padtest.out";
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	int removed = 0, at_start = SDL_NumJoysticks();
	Uint32 end = SDL_GetTicks() + secs * 1000;
	while (SDL_GetTicks() < end) {
		SDL_Event e;
		struct pad *p;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_CONTROLLERDEVICEADDED && npads < MAX_PADS) {
				/* Two pads of one model share a name; the device node tells them apart. */
				const char *raw = SDL_JoystickNameForIndex(e.cdevice.which);
				const char *path = SDL_JoystickPathForIndex(e.cdevice.which);
				SDL_GameController *c = SDL_GameControllerOpen(e.cdevice.which);
				if (!c)
					continue;
				p = &pads[npads++];
				p->id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
				snprintf(p->name, sizeof(p->name), "%s", raw ?: "unnamed");
				snprintf(p->path, sizeof(p->path), "%s", path ?: "nopath");
			}
			if (e.type == SDL_CONTROLLERDEVICEREMOVED) removed++;
			if (e.type == SDL_CONTROLLERBUTTONDOWN && (p = by_id(e.cbutton.which))) p->buttons++;
			if (e.type == SDL_CONTROLLERAXISMOTION && (p = by_id(e.caxis.which))) p->axes++;
		}
		SDL_Delay(5);
	}
	FILE *f = fopen(out, "w");
	fprintf(f, "at_start=%d added=%d removed=%d", at_start, npads, removed);
	for (int i = 0; i < npads; i++)
		fprintf(f, " | %s \"%s\" buttons=%d axes=%d", pads[i].path, pads[i].name, pads[i].buttons, pads[i].axes);
	fputc('\n', f);
	fclose(f);
	SDL_Quit();
	return 0;
}
