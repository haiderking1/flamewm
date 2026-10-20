#include "render/gpu_time.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <string.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/util/log.h>

// loaded through EGL, so flame doesn't link GLES itself
static struct {
	PFNGLGENQUERIESEXTPROC gen_queries;
	PFNGLDELETEQUERIESEXTPROC delete_queries;
	PFNGLBEGINQUERYEXTPROC begin_query;
	PFNGLENDQUERYEXTPROC end_query;
	PFNGLGETQUERYIVEXTPROC get_query;
	// availability. nvidia's glGetQueryObjectivEXT is a stub that never
	// writes anything, the core gles 3 one works
	void (*get_query_object)(GLuint id, GLenum name, GLuint *value);
	PFNGLGETQUERYOBJECTUI64VEXTPROC get_query_object_u64;
	void (*get_integer)(GLenum name, GLint *data);
	void (*flush)(void);
} gl;

static bool load(void) {
	gl.gen_queries = (void *)eglGetProcAddress("glGenQueriesEXT");
	gl.delete_queries = (void *)eglGetProcAddress("glDeleteQueriesEXT");
	gl.begin_query = (void *)eglGetProcAddress("glBeginQueryEXT");
	gl.end_query = (void *)eglGetProcAddress("glEndQueryEXT");
	gl.get_query = (void *)eglGetProcAddress("glGetQueryivEXT");
	gl.get_query_object = (void *)eglGetProcAddress("glGetQueryObjectuiv");
	if (!gl.get_query_object) {
		gl.get_query_object = (void *)eglGetProcAddress("glGetQueryObjectuivEXT");
	}
	gl.get_query_object_u64 = (void *)eglGetProcAddress("glGetQueryObjectui64vEXT");
	gl.get_integer = (void *)eglGetProcAddress("glGetIntegerv");
	gl.flush = (void *)eglGetProcAddress("glFlush");
	return gl.gen_queries && gl.delete_queries && gl.begin_query && gl.end_query
		&& gl.get_query && gl.get_query_object && gl.get_query_object_u64
		&& gl.get_integer && gl.flush;
}

// GL calls need the renderer's context current. wlroots makes it current
// itself while drawing and puts back whatever was before, so we do the same
struct saved {
	EGLDisplay display;
	EGLContext context;
	EGLSurface draw, read;
};

static struct saved make_current(struct flame_gpu_time *gpu) {
	struct saved saved = {
		.display = eglGetCurrentDisplay(),
		.context = eglGetCurrentContext(),
		.draw = eglGetCurrentSurface(EGL_DRAW),
		.read = eglGetCurrentSurface(EGL_READ),
	};
	eglMakeCurrent(gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, gpu->context);
	return saved;
}

static void restore(struct flame_gpu_time *gpu, struct saved saved) {
	if (saved.context == EGL_NO_CONTEXT) {
		eglMakeCurrent(gpu->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	} else {
		eglMakeCurrent(saved.display, saved.draw, saved.read, saved.context);
	}
}

bool gpu_time_init(struct flame_gpu_time *gpu, struct wlr_renderer *renderer) {
	memset(gpu, 0, sizeof(*gpu));
	gpu->active = -1;
	if (!wlr_renderer_is_gles2(renderer)
			|| !wlr_gles2_renderer_check_ext(renderer, "GL_EXT_disjoint_timer_query")
			|| !load()) {
		wlr_log(WLR_INFO, "gpu timing unavailable, no GL_EXT_disjoint_timer_query");
		return false;
	}
	struct wlr_egl *egl = wlr_gles2_renderer_get_egl(renderer);
	gpu->display = wlr_egl_get_display(egl);
	gpu->context = wlr_egl_get_context(egl);

	struct saved saved = make_current(gpu);
	// zero bits means the driver has no usable elapsed counter
	GLint bits = 0;
	gl.get_query(GL_TIME_ELAPSED_EXT, GL_QUERY_COUNTER_BITS_EXT, &bits);
	if (bits > 0) {
		gl.gen_queries(GPU_TIME_SLOTS, gpu->queries);
		// a disjoint flag from before we started isn't about our queries
		GLint disjoint;
		gl.get_integer(GL_GPU_DISJOINT_EXT, &disjoint);
	}
	restore(gpu, saved);

	if (bits <= 0) {
		wlr_log(WLR_INFO, "gpu timing unavailable, elapsed counter has no bits");
		return false;
	}
	gpu->usable = true;
	return true;
}

void gpu_time_finish(struct flame_gpu_time *gpu) {
	if (!gpu->usable) {
		return;
	}
	struct saved saved = make_current(gpu);
	if (gpu->active >= 0) {
		gl.end_query(GL_TIME_ELAPSED_EXT);
	}
	gl.delete_queries(GPU_TIME_SLOTS, gpu->queries);
	restore(gpu, saved);
	gpu->usable = false;
}

void gpu_time_begin(struct flame_gpu_time *gpu) {
	if (!gpu->usable || gpu->active >= 0) {
		return;
	}
	for (int i = 0; i < GPU_TIME_SLOTS; i++) {
		if (!gpu->busy[i]) {
			struct saved saved = make_current(gpu);
			gl.begin_query(GL_TIME_ELAPSED_EXT, gpu->queries[i]);
			restore(gpu, saved);
			gpu->busy[i] = true;
			gpu->active = i;
			return;
		}
	}
}

void gpu_time_end(struct flame_gpu_time *gpu) {
	if (!gpu->usable || gpu->active < 0) {
		return;
	}
	struct saved saved = make_current(gpu);
	gl.end_query(GL_TIME_ELAPSED_EXT);
	// get the end marker to the gpu, without waiting on anything
	gl.flush();
	restore(gpu, saved);
	gpu->active = -1;
}

int gpu_time_collect(struct flame_gpu_time *gpu, double *max_ms) {
	*max_ms = 0;
	if (!gpu->usable) {
		return 0;
	}
	struct saved saved = make_current(gpu);
	double ms[GPU_TIME_SLOTS];
	int done = 0;
	for (int i = 0; i < GPU_TIME_SLOTS; i++) {
		if (!gpu->busy[i] || i == gpu->active) {
			continue;
		}
		// stays "not yet" if the driver doesn't answer
		GLuint available = 0;
		gl.get_query_object(gpu->queries[i], GL_QUERY_RESULT_AVAILABLE_EXT, &available);
		if (!available) {
			continue; // still running, next time
		}
		GLuint64 ns = 0;
		gl.get_query_object_u64(gpu->queries[i], GL_QUERY_RESULT_EXT, &ns);
		gpu->busy[i] = false;
		ms[done++] = (double)ns / 1e6;
	}
	// clocks jumped while these ran (power states, resets): can't trust them
	GLint disjoint = 0;
	gl.get_integer(GL_GPU_DISJOINT_EXT, &disjoint);
	restore(gpu, saved);

	if (disjoint) {
		return 0;
	}
	for (int i = 0; i < done; i++) {
		if (ms[i] > *max_ms) {
			*max_ms = ms[i];
		}
	}
	return done;
}
