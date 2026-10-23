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
	"varying vec2 v_pos;\n" // where in the box, 0..1
	"void main() {\n"
	"	vec3 pos3 = vec3(pos, 1.0);\n"
	"	gl_Position = vec4(pos3 * proj, 1.0);\n"
	"	v_texcoord = (pos3 * tex_proj).xy;\n"
	"	v_pos = pos;\n"
	"}\n";

// a texture cut to the rounded window it's part of: how much of the pixel
// is inside, by the rounded box's distance field (smooth edged). dst: the
// box drawn into, shape: the window's, buffer pixels. a radius of 0 for
// all of it
#define SHAPE \
	"varying vec2 v_pos;\n" \
	"uniform vec4 dst;\n" \
	"uniform vec4 shape;\n" \
	"uniform float shape_radius;\n" \
	"float inside() {\n" \
	"	if (shape_radius <= 0.0) {\n" \
	"		return 1.0;\n" \
	"	}\n" \
	"	vec2 p = dst.xy + v_pos * dst.zw;\n" \
	"	vec2 half_size = shape.zw * 0.5;\n" \
	"	float r = min(shape_radius, min(half_size.x, half_size.y));\n" \
	"	vec2 q = abs(p - shape.xy - half_size) - half_size + r;\n" \
	"	float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n" \
	"	return clamp(0.5 - d, 0.0, 1.0);\n" \
	"}\n"

static const char quad_src[] =
	"uniform vec4 color;\n"
	"void main() {\n"
	"	gl_FragColor = color;\n"
	"}\n";

static const char tex_rgba_src[] =
	SHAPE
	"varying vec2 v_texcoord;\n"
	"uniform sampler2D tex;\n"
	"uniform float alpha;\n"
	"void main() {\n"
	"	gl_FragColor = texture2D(tex, v_texcoord) * alpha * inside();\n"
	"}\n";

static const char tex_rgbx_src[] =
	SHAPE
	"varying vec2 v_texcoord;\n"
	"uniform sampler2D tex;\n"
	"uniform float alpha;\n"
	"void main() {\n"
	"	gl_FragColor = vec4(texture2D(tex, v_texcoord).rgb, 1.0) * alpha * inside();\n"
	"}\n";

static const char tex_ext_src[] =
	"#extension GL_OES_EGL_image_external : require\n"
	SHAPE
	"varying vec2 v_texcoord;\n"
	"uniform samplerExternalOES tex;\n"
	"uniform float alpha;\n"
	"void main() {\n"
	"	gl_FragColor = texture2D(tex, v_texcoord) * alpha * inside();\n"
	"}\n";

// dual kawase: each level down is half the size, five taps; back up, eight.
// texel is the source's, bounds the part of it that's in use (uv, half a
// texel in), nothing past it gets read

static const char blur_down_src[] =
	"varying vec2 v_texcoord;\n"
	"uniform sampler2D tex;\n"
	"uniform vec2 texel;\n"
	"uniform float offset;\n"
	"uniform vec4 bounds;\n"
	"vec4 at(vec2 uv) {\n"
	"	return texture2D(tex, clamp(uv, bounds.xy, bounds.zw));\n"
	"}\n"
	"void main() {\n"
	"	vec2 uv = v_texcoord;\n"
	"	vec2 d = texel * offset;\n"
	"	vec4 sum = at(uv) * 4.0;\n"
	"	sum += at(uv - d);\n"
	"	sum += at(uv + d);\n"
	"	sum += at(uv + vec2(d.x, -d.y));\n"
	"	sum += at(uv - vec2(d.x, -d.y));\n"
	"	gl_FragColor = sum / 8.0;\n"
	"}\n";

static const char blur_up_src[] =
	"varying vec2 v_texcoord;\n"
	"uniform sampler2D tex;\n"
	"uniform vec2 texel;\n"
	"uniform float offset;\n"
	"uniform vec4 bounds;\n"
	"vec4 at(vec2 uv) {\n"
	"	return texture2D(tex, clamp(uv, bounds.xy, bounds.zw));\n"
	"}\n"
	"void main() {\n"
	"	vec2 uv = v_texcoord;\n"
	"	vec2 d = texel * offset;\n"
	"	vec4 sum = at(uv + vec2(-d.x * 2.0, 0.0));\n"
	"	sum += at(uv + vec2(-d.x, d.y)) * 2.0;\n"
	"	sum += at(uv + vec2(0.0, d.y * 2.0));\n"
	"	sum += at(uv + vec2(d.x, d.y)) * 2.0;\n"
	"	sum += at(uv + vec2(d.x * 2.0, 0.0));\n"
	"	sum += at(uv + vec2(d.x, -d.y)) * 2.0;\n"
	"	sum += at(uv + vec2(0.0, -d.y * 2.0));\n"
	"	sum += at(uv + vec2(-d.x, -d.y)) * 2.0;\n"
	"	gl_FragColor = sum / 12.0;\n"
	"}\n";

// the pane: v_texcoord is the buffer pixel. a rounded box by its distance
// field, its rim a lens: a lip refraction * 2 wide that pulls what's behind
// in from further inside, more the nearer the edge, so lines under it
// curve along the rim. red bends a little more than blue there. then made
// more vivid, tinted, and lit: a thin bright edge where it faces the light
// (top left) and where the light comes out again (bottom right), and the
// lip a little brighter all around
static const char glass_src[] =
	"varying vec2 v_texcoord;\n"
	"uniform sampler2D tex;\n"
	"uniform vec4 reach;\n" // where tex starts in the buffer, its allocated size
	"uniform vec4 bounds;\n"
	"uniform vec4 box;\n"
	"uniform float radius;\n"
	"uniform vec4 tint;\n"
	"uniform float saturation;\n"
	"uniform float edge;\n"
	"uniform float refraction;\n"
	"uniform float rim;\n"
	"uniform float alpha;\n"
	"vec4 behind(vec2 at) {\n"
	"	return texture2D(tex, clamp((at - reach.xy) / reach.zw, bounds.xy, bounds.zw));\n"
	"}\n"
	"void main() {\n"
	"	vec2 p = v_texcoord;\n"
	"	vec2 half_size = box.zw * 0.5;\n"
	"	vec2 c = box.xy + half_size;\n"
	"	float r = min(radius, min(half_size.x, half_size.y));\n"
	"	vec2 q = abs(p - c) - half_size + r;\n"
	"	float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
	"	vec2 n = q.x > 0.0 || q.y > 0.0 ? normalize(max(q, 0.0))\n"
	"		: (q.x > q.y ? vec2(1.0, 0.0) : vec2(0.0, 1.0));\n"
	"	n *= sign(p - c + 0.0001);\n"
	"	float depth = max(-d, 0.0);\n"
	// how far in from the edge, how much: 1 at the edge, 0 past the lip.
	// which way it bends: along the box rounded at least as much as the lip
	// is wide, its corners turn smoothly, no seam where two sides' lips meet
	"	float lip = max(refraction * 2.0, 1.0);\n"
	"	float t = 1.0 - clamp(depth / lip, 0.0, 1.0);\n"
	"	float lr = min(max(r, lip), min(half_size.x, half_size.y));\n"
	"	vec2 lq = max(abs(p - c) - half_size + lr, 0.0);\n"
	"	vec2 ln = length(lq) > 0.0 ? normalize(lq) * sign(p - c + 0.0001) : vec2(0.0);\n"
	"	vec2 pull = -ln * refraction * t * t;\n"
	"	vec4 col = behind(p + pull);\n"
	"	col.r = behind(p + pull * 1.08).r;\n"
	"	col.b = behind(p + pull * 0.92).b;\n"
	"	float luma = dot(col.rgb, vec3(0.2126, 0.7152, 0.0722));\n"
	"	col.rgb = clamp(mix(vec3(luma), col.rgb, saturation), 0.0, 1.0) * col.a;\n"
	"	col = tint + col * (1.0 - tint.a);\n"
	"	float line = 1.0 - smoothstep(0.0, rim * 2.0, depth);\n"
	"	float facing = dot(n, normalize(vec2(-0.6, -0.8)));\n"
	"	float lit = 0.12 + 0.88 * facing * facing;\n"
	"	float light = edge * (line * lit * 0.85 + t * t * t * lit * 0.2);\n"
	"	col.rgb = min(col.rgb + vec3(light) * col.a, vec3(col.a));\n"
	"	gl_FragColor = col * clamp(0.5 - d, 0.0, 1.0) * alpha;\n"
	"}\n";

static GLuint compile_as(GLenum type, const char *precision, const char *src, bool quiet) {
	GLuint shader = fglCreateShader(type);
	const char *srcs[] = {precision, src};
	fglShaderSource(shader, 2, srcs, NULL);
	fglCompileShader(shader);
	GLint ok = GL_FALSE;
	fglGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (ok == GL_FALSE) {
		char log[1024] = "";
		fglGetShaderInfoLog(shader, sizeof(log), NULL, log);
		if (!quiet) {
			wlr_log(WLR_ERROR, "gl: a shader doesn't compile: %s", log);
		}
		fglDeleteShader(shader);
		return 0;
	}
	return shader;
}

// a fragment shader in high precision, else medium (a gpu without high).
// GL_FRAGMENT_PRECISION_HIGH can't be asked (nvidia doesn't say it), and
// medium is whole pixels past 1024: edges there would come out in steps
static GLuint compile(GLenum type, const char *src) {
	if (type != GL_FRAGMENT_SHADER) {
		return compile_as(type, "", src, false);
	}
	GLuint shader = compile_as(type, "precision highp float;\n", src, true);
	return shader ? shader : compile_as(type, "precision mediump float;\n", src, false);
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
	program->dst = fglGetUniformLocation(id, "dst");
	program->shape = fglGetUniformLocation(id, "shape");
	program->shape_radius = fglGetUniformLocation(id, "shape_radius");
	program->color = fglGetUniformLocation(id, "color");
	program->pos = fglGetAttribLocation(id, "pos");
	program->texel = fglGetUniformLocation(id, "texel");
	program->offset = fglGetUniformLocation(id, "offset");
	program->bounds = fglGetUniformLocation(id, "bounds");
	program->box = fglGetUniformLocation(id, "box");
	program->reach = fglGetUniformLocation(id, "reach");
	program->radius = fglGetUniformLocation(id, "radius");
	program->tint = fglGetUniformLocation(id, "tint");
	program->saturation = fglGetUniformLocation(id, "saturation");
	program->edge = fglGetUniformLocation(id, "edge");
	program->refraction = fglGetUniformLocation(id, "refraction");
	program->rim = fglGetUniformLocation(id, "rim");
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
			|| (gl->external && !link_program(&gl->tex_ext, tex_ext_src))
			|| !link_program(&gl->blur_down, blur_down_src)
			|| !link_program(&gl->blur_up, blur_up_src)
			|| !link_program(&gl->glass, glass_src)) {
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
		drop_program(&gl->blur_down);
		drop_program(&gl->blur_up);
		drop_program(&gl->glass);
		gl_blur_finish(&gl->blur);
	}
	gl->usable = false;
}
