// a GL app the X way: an fbconfig asking for 4x multisampling and srgb, a
// window, a context made current on it, frames swapped, a pixel read back.
// whatever GL glvnd loads for flame's gpu (nvidia's own, or mesa)
#include <GL/gl.h>
#include <GL/glx.h>
#include <stdio.h>
#include <unistd.h>

static int x_error(Display *d, XErrorEvent *e) {
	printf("X error %d on %d.%d\n", e->error_code, e->request_code, e->minor_code);
	return 0;
}

int main(void) {
	Display *d = XOpenDisplay(NULL);
	if (!d) {
		return 2;
	}
	XSetErrorHandler(x_error);
	int attrs[] = {GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
		GLX_DOUBLEBUFFER, True, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
		GLX_DEPTH_SIZE, 24, GLX_SAMPLE_BUFFERS, 1, GLX_SAMPLES, 4,
		GLX_FRAMEBUFFER_SRGB_CAPABLE_ARB, True, None};
	int n = 0;
	GLXFBConfig *configs = glXChooseFBConfig(d, DefaultScreen(d), attrs, &n);
	if (!n) {
		puts("no config");
		return 1;
	}
	int samples = 0;
	glXGetFBConfigAttrib(d, configs[0], GLX_SAMPLES, &samples);
	printf("samples %d\n", samples);
	XVisualInfo *vi = glXGetVisualFromFBConfig(d, configs[0]);
	XSetWindowAttributes swa = {
		.colormap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual, AllocNone),
	};
	Window w = XCreateWindow(d, RootWindow(d, vi->screen), 0, 0, 200, 150, 0, vi->depth,
		InputOutput, vi->visual, CWColormap, &swa);
	XMapWindow(d, w);
	XSync(d, False);
	GLXContext ctx = glXCreateNewContext(d, configs[0], GLX_RGBA_TYPE, NULL, True);
	if (!ctx || !glXMakeContextCurrent(d, w, w, ctx)) {
		puts("make current failed");
		return 1;
	}
	printf("renderer %s\n", glGetString(GL_RENDERER));
	for (int i = 0; i < 5; i++) {
		glClearColor(1, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glXSwapBuffers(d, w);
	}
	glClear(GL_COLOR_BUFFER_BIT);
	unsigned char px[4] = {0};
	glReadPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	printf("pixel %d %d %d\n", px[0], px[1], px[2]);
	glXSwapBuffers(d, w);
	glFinish();
	XSync(d, False);
	sleep(1);
	glXMakeContextCurrent(d, None, None, NULL);
	glXDestroyContext(d, ctx);
	XCloseDisplay(d);
	return 0;
}
