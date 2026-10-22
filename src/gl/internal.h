#ifndef FLAME_GL_INTERNAL_H
#define FLAME_GL_INTERNAL_H

// gl/ on the inside: the GL and EGL calls it makes, loaded through EGL so
// flame never links GLES itself (a gl* call straight to GLES doesn't link).
// fgl* names, not gl*: wlroots links the real glUseProgram, a symbol of ours
// by that name would take its place

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define FLAME_GL_PROCS(X) \
	X(PFNGLACTIVETEXTUREPROC, ActiveTexture) \
	X(PFNGLATTACHSHADERPROC, AttachShader) \
	X(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer) \
	X(PFNGLBINDTEXTUREPROC, BindTexture) \
	X(PFNGLBLENDFUNCPROC, BlendFunc) \
	X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, CheckFramebufferStatus) \
	X(PFNGLCLEARPROC, Clear) \
	X(PFNGLCLEARCOLORPROC, ClearColor) \
	X(PFNGLCOMPILESHADERPROC, CompileShader) \
	X(PFNGLCOPYTEXSUBIMAGE2DPROC, CopyTexSubImage2D) \
	X(PFNGLCREATEPROGRAMPROC, CreateProgram) \
	X(PFNGLCREATESHADERPROC, CreateShader) \
	X(PFNGLDELETEFRAMEBUFFERSPROC, DeleteFramebuffers) \
	X(PFNGLDELETEPROGRAMPROC, DeleteProgram) \
	X(PFNGLDELETESHADERPROC, DeleteShader) \
	X(PFNGLDELETETEXTURESPROC, DeleteTextures) \
	X(PFNGLDISABLEPROC, Disable) \
	X(PFNGLDISABLEVERTEXATTRIBARRAYPROC, DisableVertexAttribArray) \
	X(PFNGLDRAWARRAYSPROC, DrawArrays) \
	X(PFNGLENABLEPROC, Enable) \
	X(PFNGLENABLEVERTEXATTRIBARRAYPROC, EnableVertexAttribArray) \
	X(PFNGLFLUSHPROC, Flush) \
	X(PFNGLFRAMEBUFFERTEXTURE2DPROC, FramebufferTexture2D) \
	X(PFNGLGENFRAMEBUFFERSPROC, GenFramebuffers) \
	X(PFNGLGENTEXTURESPROC, GenTextures) \
	X(PFNGLGETATTRIBLOCATIONPROC, GetAttribLocation) \
	X(PFNGLGETERRORPROC, GetError) \
	X(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog) \
	X(PFNGLGETPROGRAMIVPROC, GetProgramiv) \
	X(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog) \
	X(PFNGLGETSHADERIVPROC, GetShaderiv) \
	X(PFNGLGETSTRINGPROC, GetString) \
	X(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation) \
	X(PFNGLLINKPROGRAMPROC, LinkProgram) \
	X(PFNGLSHADERSOURCEPROC, ShaderSource) \
	X(PFNGLTEXIMAGE2DPROC, TexImage2D) \
	X(PFNGLTEXPARAMETERIPROC, TexParameteri) \
	X(PFNGLUNIFORM1FPROC, Uniform1f) \
	X(PFNGLUNIFORM1IPROC, Uniform1i) \
	X(PFNGLUNIFORM2FPROC, Uniform2f) \
	X(PFNGLUNIFORM4FPROC, Uniform4f) \
	X(PFNGLUNIFORMMATRIX3FVPROC, UniformMatrix3fv) \
	X(PFNGLUSEPROGRAMPROC, UseProgram) \
	X(PFNGLVERTEXATTRIBPOINTERPROC, VertexAttribPointer) \
	X(PFNGLVIEWPORTPROC, Viewport)

#define FLAME_EGL_PROCS(X) \
	X(PFNEGLCREATESYNCKHRPROC, eglCreateSyncKHR) \
	X(PFNEGLDESTROYSYNCKHRPROC, eglDestroySyncKHR) \
	X(PFNEGLWAITSYNCKHRPROC, eglWaitSyncKHR) \
	X(PFNEGLDUPNATIVEFENCEFDANDROIDPROC, eglDupNativeFenceFDANDROID)

#define FLAME_GL_DECLARE(type, name) extern type fgl##name;
FLAME_GL_PROCS(FLAME_GL_DECLARE)
#undef FLAME_GL_DECLARE

#define FLAME_EGL_DECLARE(type, name) extern type f##name;
FLAME_EGL_PROCS(FLAME_EGL_DECLARE)
#undef FLAME_EGL_DECLARE

struct flame_gl;
// the renderer's context, current from here on
void gl_make_current(struct flame_gl *gl);

#endif
