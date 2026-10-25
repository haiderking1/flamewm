// a game's mouse look the way most games ask for it: sdl3's relative mouse
// mode, on X (XI2 raw motion off the root) or wayland (the pointer locked,
// relative-pointer). the real library games ship, not a client of ours that
// only does what flame expects. prints what came: the deltas summed
// usage: sdlmouse x11|wayland SECONDS
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
	if (argc < 3) {
		return 2;
	}
	setvbuf(stdout, NULL, _IONBF, 0);
	SDL_SetHint(SDL_HINT_VIDEO_DRIVER, argv[1]);
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		printf("no %s: %s\n", argv[1], SDL_GetError());
		return 1;
	}
	SDL_Window *window = SDL_CreateWindow("sdlmouse", 640, 480, 0);
	SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, "software") : NULL;
	if (!renderer) {
		printf("no window: %s\n", SDL_GetError());
		return 1;
	}
	// drawing every frame like a game: a wayland window that never drew
	// isn't there (no focus, no pointer). mapped and focused before it asks
	Uint64 settle = SDL_GetTicks() + 800;
	while (SDL_GetTicks() < settle) {
		SDL_PumpEvents();
		SDL_RenderClear(renderer);
		SDL_RenderPresent(renderer);
		SDL_Delay(10);
	}
	bool on = SDL_SetWindowRelativeMouseMode(window, true);
	printf("relative mode: %s\n", on ? "on" : SDL_GetError());
	int count = 0;
	float xrel = 0, yrel = 0;
	Uint64 end = SDL_GetTicks() + (Uint64)atoi(argv[2]) * 1000;
	while (SDL_GetTicks() < end) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_EVENT_MOUSE_MOTION) {
				count++;
				xrel += event.motion.xrel;
				yrel += event.motion.yrel;
			}
		}
		SDL_RenderClear(renderer);
		SDL_RenderPresent(renderer);
		SDL_Delay(5);
	}
	printf("moved: %s, %.0f across %.0f down\n", count ? "yes" : "no", xrel, yrel);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return 0;
}
