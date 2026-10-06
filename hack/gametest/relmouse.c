/* Grabs the mouse the way a first person game does and reports what arrived. */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 8;
	const char *out = argc > 2 ? argv[2] : "/tmp/relmouse.out";
	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Window *w = SDL_CreateWindow("relmouse", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
					 1280, 720, SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
	SDL_Renderer *r = SDL_CreateRenderer(w, -1, 0);
	int rel = SDL_SetRelativeMouseMode(SDL_TRUE);
	long dx = 0, dy = 0, n = 0, keys = 0, wheel = 0, buttons = 0;
	Uint32 end = SDL_GetTicks() + secs * 1000;
	while (SDL_GetTicks() < end) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_MOUSEMOTION) { dx += e.motion.xrel; dy += e.motion.yrel; n++; }
			if (e.type == SDL_KEYDOWN && !e.key.repeat) keys++;
			if (e.type == SDL_MOUSEWHEEL) wheel += e.wheel.y;
			if (e.type == SDL_MOUSEBUTTONDOWN) buttons++;
		}
		SDL_SetRenderDrawColor(r, 40, 30, 80, 255);
		SDL_RenderClear(r);
		SDL_RenderPresent(r);
		SDL_Delay(4);
	}
	FILE *f = fopen(out, "w");
	fprintf(f, "driver=%s relative_rc=%d motions=%ld dx=%ld dy=%ld keys=%ld wheel=%ld buttons=%ld\n",
		SDL_GetCurrentVideoDriver(), rel, n, dx, dy, keys, wheel, buttons);
	fclose(f);
	SDL_Quit();
	return 0;
}
