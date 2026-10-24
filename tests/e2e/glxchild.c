// GL in child windows of a cpu drawn top-level, the way browsers (a web view
// each) and wine (its client window) do it: every child shows where X has
// it, stacked like X stacks them, cut to what it's in.
//   red: the top-level, XFillRectangle
//   green: a GL child at 40,30 120x90
//   blue: a GL child at 100,80 120x90, above green
//   yellow: a GL child at 50,50 200x200 inside a 100x100 window at 300,30,
//     only its corner inside that window shows
// glxchild remap: green is unmapped and mapped again in one go without
// drawing, like an app reusing a popup: its old picture is gone with it
// glxchild swap: green is unmapped and a new child in its place draws
// magenta a moment later, the way CEF swaps the window a page is drawn in
// (steam): screenshots in the gap (gap.ppm, green should still show) and
// after (after.ppm)
#include <GL/gl.h>
#include <GL/glx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Display *d;
static XVisualInfo *vi;
static Colormap colormap;

static Window child(Window parent, int x, int y, int w, int h) {
	XSetWindowAttributes swa = {.colormap = colormap};
	Window win = XCreateWindow(d, parent, x, y, w, h, 0, vi->depth, InputOutput, vi->visual,
		CWColormap, &swa);
	XMapWindow(d, win);
	return win;
}

static void draw(GLXContext ctx, Window win, float r, float g, float b) {
	glXMakeCurrent(d, win, ctx);
	for (int i = 0; i < 3; i++) {
		glClearColor(r, g, b, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glXSwapBuffers(d, win);
	}
	glFinish();
}

int main(int argc, char **argv) {
	d = XOpenDisplay(NULL);
	if (!d) {
		return 2;
	}
	int attrs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8,
		GLX_BLUE_SIZE, 8, None};
	vi = glXChooseVisual(d, DefaultScreen(d), attrs);
	if (!vi) {
		puts("no visual");
		return 1;
	}
	Window root = RootWindow(d, vi->screen);
	colormap = XCreateColormap(d, root, vi->visual, AllocNone);
	XSetWindowAttributes swa = {.colormap = colormap};
	Window top = XCreateWindow(d, root, 0, 0, 640, 400, 0, vi->depth, InputOutput, vi->visual,
		CWColormap, &swa);
	XStoreName(d, top, "glxchild");
	XMapWindow(d, top);
	Window green = child(top, 40, 30, 120, 90);
	Window blue = child(top, 100, 80, 120, 90);
	Window box = child(top, 300, 30, 100, 100);
	Window yellow = child(box, 50, 50, 200, 200);
	XSync(d, False);
	usleep(300000);
	XWindowAttributes wa;
	XGetWindowAttributes(d, top, &wa);
	GC gc = XCreateGC(d, top, 0, NULL);
	XSetForeground(d, gc, 0xff0000);
	XFillRectangle(d, top, gc, 0, 0, (unsigned)wa.width, (unsigned)wa.height);
	GLXContext ctx = glXCreateContext(d, vi, NULL, True);
	if (!ctx) {
		puts("no context");
		return 1;
	}
	draw(ctx, green, 0, 1, 0);
	draw(ctx, blue, 0, 0, 1);
	draw(ctx, yellow, 1, 1, 0);
	XSync(d, False);
	if (argc > 1 && !strcmp(argv[1], "swap")) {
		usleep(300000);
		XUnmapWindow(d, green);
		XSync(d, False);
		usleep(30000);
		if (system("grim -t ppm gap.ppm") != 0) {
			puts("no screenshot");
		}
		Window fresh = child(top, 40, 30, 120, 90);
		XSync(d, False);
		usleep(30000);
		draw(ctx, fresh, 1, 0, 1);
		XSync(d, False);
		usleep(300000);
		if (system("grim -t ppm after.ppm") != 0) {
			puts("no screenshot");
		}
	}
	if (argc > 1 && !strcmp(argv[1], "remap")) {
		usleep(300000);
		XUnmapWindow(d, green);
		XMapWindow(d, green);
		XSync(d, False);
	}
	puts("drawn");
	fflush(stdout);
	sleep(2);
	glXMakeCurrent(d, None, NULL);
	glXDestroyContext(d, ctx);
	XCloseDisplay(d);
	return 0;
}
