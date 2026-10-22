#include "gl/gl.h"

#include <string.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/util/log.h>

#include "gl/internal.h"

#define FLAME_GL_DEFINE(type, name) type fgl##name;
FLAME_GL_PROCS(FLAME_GL_DEFINE)
#undef FLAME_GL_DEFINE

#define FLAME_EGL_DEFINE(type, name) type f##name;
FLAME_EGL_PROCS(FLAME_EGL_DEFINE)
#undef FLAME_EGL_DEFINE

static bool load(void) {
	bool ok = true;
#define FLAME_GL_LOAD(type, name) \
	ok = (fgl##name = (type)eglGetProcAddress("gl" #name)) && ok;
	FLAME_GL_PROCS(FLAME_GL_LOAD)
#undef FLAME_GL_LOAD
	return ok;
}

// sync is optional: without it there are no timelines to wait on or signal
static bool load_fences(EGLDisplay display) {
	const char *exts = eglQueryString(display, EGL_EXTENSIONS);
	if (!exts || !strstr(exts, "EGL_ANDROID_native_fence_sync")
			|| !strstr(exts, "EGL_KHR_wait_sync")) {
		return false;
	}
	bool ok = true;
#define FLAME_EGL_LOAD(type, name) ok = (f##name = (type)eglGetProcAddress(#name)) && ok;
	FLAME_EGL_PROCS(FLAME_EGL_LOAD)
#undef FLAME_EGL_LOAD
	return ok;
}

// the vertex shader every program shares: pos is the unit square of the box
// being drawn, proj puts it in the buffer, tex_proj on the texture
static const char vertex_src[] =
	"uniform mat3 proj;\n"
	"uniform mat3 tex_proj;\n"
	"attribute vec2 pos;\n"
	"varying vec2 v_texcoord;\n"
	"void main() {\n"
	"	vec3 pos3 = vec3(pos, 1.0);\n"
	"	gl_Position = vec4(pos3 * proj, 1.0);\n"
	"	v_texcoord = (pos3 * tex_proj).xy;\n"
	"}\n";

#define PRECISION \
	"#ifdef GL_FRAGMENT_PRECISION_HIGH\n" \
	"precision highp float;\n" \
	"#else\n" \
	"precision mediump float;\n" \
	"#endif\n"

static const char quad_src[] =
	PRECISION
	"uniform vec4 color;\n"
	"void main() {\n"
	"	gl_FragColor = color;\n"
	"}\n";

static const char tex_rgba_src[] =
	PRECISION
	"varying vec2 v_texcoord;\n"
	"uniform sampler2D tex;\n"
	"uniform float alpha;\n"
	"void main() {\n"
	"	gl_FragColor = texture2D(tex, v_texcoord) * alpha;\n"
	"}\n";

static const char tex_rgbx_src[] =
	PRECISION
	"varying vec2 v_texcoord;\n"
	"uniform sampler2D tex;\n"
	"uniform float alpha;\n"
	"void main() {\n"
	"	gl_FragColor = vec4(texture2D(tex, v_texcoord).rgb, 1.0) * alpha;\n"
	"}\n";

static const char tex_ext_src[] =
	"#extension GL_OES_EGL_image_external : require\n"
	PRECISION
	"varying vec2 v_texcoord;\n"
	"uniform samplerExternalOES tex;\n"
	"uniform float alpha;\n"
	"void main() {\n"
	"	gl_FragColor = texture2D(tex, v_texcoord) * alpha;\n"
	"}\n";

static GLuint compile(GLenum type, const char *src) {
	GLuint shader = fglCreateShader(type);
	fglShaderSource(shader, 1, &src, NULL);
	fglCompileShader(shader);
	GLint ok = GL_FALSE;
	fglGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (ok == GL_FALSE) {
		char log[1024] = "";
		fglGetShaderInfoLog(shader, sizeof(log), NULL, log);
		wlr_log(WLR_ERROR, "gl: a shader doesn't compile: %s", log);
		fglDeleteShader(shader);
		return 0;
	}
	return shader;
}

static bool link_program(struct gl_program *program, const char *fragment_src) {
	memset(program, 0, sizeof(*program));
	GLuint vertex = compile(GL_VERTEX_SHADER, vertex_src);
	GLuint fragment = vertex ? compile(GL_FRAGMENT_SHADER, fragment_src) : 0;
	if (!fragment) {
		if (vertex) {
			fglDeleteShader(vertex);
		}
		return false;
	}
	GLuint id = fglCreateProgram();
	fglAttachShader(id, vertex);
	fglAttachShader(id, fragment);
	fglLinkProgram(id);
	fglDeleteShader(vertex);
	fglDeleteShader(fragment);
	GLint ok = GL_FALSE;
	fglGetProgramiv(id, GL_LINK_STATUS, &ok);
	if (ok == GL_FALSE) {
		char log[1024] = "";
		fglGetProgramInfoLog(id, sizeof(log), NULL, log);
		wlr_log(WLR_ERROR, "gl: a program doesn't link: %s", log);
		fglDeleteProgram(id);
		return false;
	}
	program->program = id;
	program->proj = fglGetUniformLocation(id, "proj");
	program->tex_proj = fglGetUniformLocation(id, "tex_proj");
	program->tex = fglGetUniformLocation(id, "tex");
	program->alpha = fglGetUniformLocation(id, "alpha");
	program->color = fglGetUniformLocation(id, "color");
	program->pos = fglGetAttribLocation(id, "pos");
	return true;
}

static void drop_program(struct gl_program *program) {
	if (program->program) {
		fglDeleteProgram(program->program);
	}
	program->program = 0;
}

void gl_make_current(struct flame_gl *gl) {
	// once it's ours it stays: letting go of a context on nvidia flushes and
	// can wait on the gpu. wlroots puts back whatever was current around its
	// own work, so with ours left current nobody toggles it
	if (eglGetCurrentContext() != gl->context) {
		eglMakeCurrent(gl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, gl->context);
	}
}

bool gl_init(struct flame_gl *gl, struct wlr_renderer *renderer) {
	memset(gl, 0, sizeof(*gl));
	gl->renderer = renderer;
	if (!wlr_renderer_is_gles2(renderer)) {
		wlr_log(WLR_INFO, "gl: the renderer isn't GLES2, wlroots draws");
		return false;
	}
	struct wlr_egl *egl = wlr_gles2_renderer_get_egl(renderer);
	gl->display = wlr_egl_get_display(egl);
	gl->context = wlr_egl_get_context(egl);
	if (!load()) {
		wlr_log(WLR_ERROR, "gl: GL functions missing, wlroots draws");
		return false;
	}
	gl_make_current(gl);
	gl->fences = load_fences(gl->display);
	const char *exts = (const char *)fglGetString(GL_EXTENSIONS);
	gl->external = exts && strstr(exts, "GL_OES_EGL_image_external");
	if (!link_program(&gl->quad, quad_src) || !link_program(&gl->tex_rgba, tex_rgba_src)
			|| !link_program(&gl->tex_rgbx, tex_rgbx_src)
			|| (gl->external && !link_program(&gl->tex_ext, tex_ext_src))) {
		gl_finish(gl);
		wlr_log(WLR_ERROR, "gl: shaders failed, wlroots draws");
		return false;
	}
	gl->usable = true;
	wlr_log(WLR_INFO, "gl: flame draws its frames itself (%s, %s)",
		(const char *)fglGetString(GL_RENDERER),
		gl->fences ? "explicit sync" : "implicit sync only");
	return true;
}

void gl_finish(struct flame_gl *gl) {
	if (!gl->display) {
		return;
	}
	if (fglDeleteProgram) {
		gl_make_current(gl);
		drop_program(&gl->quad);
		drop_program(&gl->tex_rgba);
		drop_program(&gl->tex_rgbx);
		drop_program(&gl->tex_ext);
	}
	gl->usable = false;
}
