// a fullscreen GL game the X way: fullscreen asked before mapping, frames
// swapped as fast as they're taken for a few seconds. flame should show its
// buffers themselves (direct scanout) and hand them back so it never stalls
// vsyncoff: swap interval 0, a game with vsync off. its presents ask to tear
// jitter: each frame takes 0 to 15 ms more, an uneven game. prints how even
// its frames came out (the spread of the gaps between swaps, after the first
// second) and how many a second
// usage: glxfull SECONDS [vsyncoff] [jitter]
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

static double now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
	double seconds = argc > 1 ? atof(argv[1]) : 3;
	Display *d = XOpenDisplay(NULL);
	if (!d) {
		return 2;
	}
	int attrs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8,
		GLX_BLUE_SIZE, 8, None};
	XVisualInfo *vi = glXChooseVisual(d, DefaultScreen(d), attrs);
	if (!vi) {
		puts("no visual");
		return 1;
	}
	Window root = RootWindow(d, vi->screen);
	XSetWindowAttributes swa = {.colormap = XCreateColormap(d, root, vi->visual, AllocNone)};
	Window w = XCreateWindow(d, root, 0, 0, 640, 480, 0, vi->depth, InputOutput, vi->visual,
		CWColormap, &swa);
	XStoreName(d, w, "glxfull");
	Atom state = XInternAtom(d, "_NET_WM_STATE", False);
	Atom full = XInternAtom(d, "_NET_WM_STATE_FULLSCREEN", False);
	XChangeProperty(d, w, state, XA_ATOM, 32, PropModeReplace, (unsigned char *)&full, 1);
	// no pointer over it, like a game: a blank cursor, what wine sets
	char empty = 0;
	Pixmap blank = XCreateBitmapFromData(d, w, &empty, 1, 1);
	XColor black = {0};
	Cursor none = XCreatePixmapCursor(d, blank, blank, &black, &black, 0, 0);
	XDefineCursor(d, w, none);
	XMapWindow(d, w);
	XSync(d, False);
	GLXContext ctx = glXCreateContext(d, vi, NULL, True);
	if (!ctx || !glXMakeCurrent(d, w, ctx)) {
		puts("no context");
		return 1;
	}
	bool jitter = false;
	for (int i = 2; i < argc; i++) {
		jitter |= !strcmp(argv[i], "jitter");
	}
	if (argc > 2 && !strcmp(argv[2], "vsyncoff")) {
		PFNGLXSWAPINTERVALEXTPROC interval = (PFNGLXSWAPINTERVALEXTPROC)
			glXGetProcAddress((const GLubyte *)"glXSwapIntervalEXT");
		if (!interval) {
			puts("no swap control");
			return 1;
		}
		interval(d, w, 0);
	}
	int swaps = 0, gaps = 0;
	double start = now(), last = 0, lo = 1e9, hi = 0;
	srand(7);
	while (now() - start < seconds) {
		if (jitter) {
			struct timespec work = {0, (rand() % 16) * 1000000L};
			nanosleep(&work, NULL);
		}
		glClearColor(0, 1, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glXSwapBuffers(d, w);
		swaps++;
		double t = now();
		if (t - start > 1 && last > 0) {
			double gap = (t - last) * 1000;
			lo = gap < lo ? gap : lo;
			hi = gap > hi ? gap : hi;
			gaps++;
		}
		last = t;
	}
	glFinish();
	printf("swaps %d in %.1fs\n", swaps, seconds);
	printf("kept going: %s\n", swaps > seconds * 20 ? "yes" : "no");
	if (gaps > 0) {
		printf("gaps %.1f to %.1f ms, %.0f a second\n", lo, hi, gaps / (seconds - 1));
		printf("even: %s\n", hi - lo < 4 ? "yes" : "no");
	}
	glXMakeCurrent(d, None, NULL);
	glXDestroyContext(d, ctx);
	XCloseDisplay(d);
	return 0;
}
