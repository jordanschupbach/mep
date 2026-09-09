#pragma once

// A minimal, self-contained OpenGL 3.3 core function-pointer loader for
// Stage B's native backend -- entry points beyond GL 1.1 aren't
// guaranteed to be exported directly by the platform's GL library, so
// they're resolved at runtime via glfwGetProcAddress, the same mechanism
// generated loaders like glad use. Deliberately hand-written rather than
// glad-generated (no code-gen step, no vendored file): the function set
// mep actually needs (2D batched quads/lines, basic 3D mesh draw,
// framebuffers, screen readback) is small and fixed, unlike a general-
// purpose game engine's.
//
// No dependency on any system GL header (no <GL/gl.h>/<GL/glcorearb.h>):
// every type/constant used here is defined locally below, exactly the
// way a generated loader's own glad.h would.

#include <cstddef>

namespace gfx {
namespace gl {

// -- Types (glcorearb.h-compatible) --------------------------------------

using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLbyte = signed char;
using GLshort = short;
using GLint = int;
using GLsizei = int;
using GLubyte = unsigned char;
using GLushort = unsigned short;
using GLuint = unsigned int;
using GLfloat = float;
using GLclampf = float;
using GLdouble = double;
using GLchar = char;
using GLsizeiptr = ptrdiff_t;
using GLintptr = ptrdiff_t;

// -- Constants (stable values, unchanged since GL's original ARB specs) --

inline constexpr GLenum GL_FALSE_ = 0;
inline constexpr GLenum GL_TRUE_ = 1;

inline constexpr GLenum GL_POINTS = 0x0000;
inline constexpr GLenum GL_LINES = 0x0001;
inline constexpr GLenum GL_LINE_LOOP = 0x0002;
inline constexpr GLenum GL_LINE_STRIP = 0x0003;
inline constexpr GLenum GL_TRIANGLES = 0x0004;
inline constexpr GLenum GL_TRIANGLE_STRIP = 0x0005;
inline constexpr GLenum GL_TRIANGLE_FAN = 0x0006;

inline constexpr GLenum GL_UNSIGNED_BYTE = 0x1401;
inline constexpr GLenum GL_SHORT = 0x1402;
inline constexpr GLenum GL_UNSIGNED_SHORT = 0x1403;
inline constexpr GLenum GL_INT = 0x1404;
inline constexpr GLenum GL_UNSIGNED_INT = 0x1405;
inline constexpr GLenum GL_FLOAT = 0x1406;

inline constexpr GLbitfield GL_DEPTH_BUFFER_BIT = 0x00000100;
inline constexpr GLbitfield GL_STENCIL_BUFFER_BIT = 0x00000400;
inline constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x00004000;

inline constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
inline constexpr GLenum GL_RED = 0x1903;
inline constexpr GLenum GL_ALPHA = 0x1906;
inline constexpr GLenum GL_RGB = 0x1907;
inline constexpr GLenum GL_RGBA = 0x1908;
inline constexpr GLenum GL_RGBA8 = 0x8058;
inline constexpr GLenum GL_R8 = 0x8229;

inline constexpr GLenum GL_NEAREST = 0x2600;
inline constexpr GLenum GL_LINEAR = 0x2601;
inline constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
inline constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
inline constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
inline constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
inline constexpr GLenum GL_REPEAT = 0x2901;
inline constexpr GLenum GL_CLAMP_TO_EDGE = 0x812F;
inline constexpr GLenum GL_TEXTURE0 = 0x84C0;
inline constexpr GLenum GL_UNPACK_ALIGNMENT = 0x0CF5;
inline constexpr GLenum GL_PACK_ALIGNMENT = 0x0D05;

inline constexpr GLenum GL_ARRAY_BUFFER = 0x8892;
inline constexpr GLenum GL_ELEMENT_ARRAY_BUFFER = 0x8893;
inline constexpr GLenum GL_STREAM_DRAW = 0x88E0;
inline constexpr GLenum GL_STATIC_DRAW = 0x88E4;
inline constexpr GLenum GL_DYNAMIC_DRAW = 0x88E8;

inline constexpr GLenum GL_FRAGMENT_SHADER = 0x8B30;
inline constexpr GLenum GL_VERTEX_SHADER = 0x8B31;
inline constexpr GLenum GL_COMPILE_STATUS = 0x8B81;
inline constexpr GLenum GL_LINK_STATUS = 0x8B82;
inline constexpr GLenum GL_INFO_LOG_LENGTH = 0x8B84;

inline constexpr GLenum GL_BLEND = 0x0BE2;
inline constexpr GLenum GL_SRC_ALPHA = 0x0302;
inline constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
inline constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
inline constexpr GLenum GL_DEPTH_TEST = 0x0B71;
inline constexpr GLenum GL_CULL_FACE = 0x0B44;

inline constexpr GLenum GL_FRAMEBUFFER = 0x8D40;
inline constexpr GLenum GL_RENDERBUFFER = 0x8D41;
inline constexpr GLenum GL_COLOR_ATTACHMENT0 = 0x8CE0;
inline constexpr GLenum GL_DEPTH_ATTACHMENT = 0x8D00;
inline constexpr GLenum GL_DEPTH_COMPONENT = 0x1902;
inline constexpr GLenum GL_DEPTH_COMPONENT24 = 0x81A6;
inline constexpr GLenum GL_FRAMEBUFFER_COMPLETE = 0x8CD5;

inline constexpr GLenum GL_FRONT_AND_BACK = 0x0408;
inline constexpr GLenum GL_LINE = 0x1B01;
inline constexpr GLenum GL_FILL = 0x1B02;

inline constexpr GLenum GL_NO_ERROR = 0;
inline constexpr GLenum GL_VENDOR = 0x1F00;
inline constexpr GLenum GL_RENDERER = 0x1F01;
inline constexpr GLenum GL_VERSION = 0x1F02;

// -- Function pointers ----------------------------------------------------
//
// Populated by LoadGLFunctions() (gl_loader.cpp), which must run once,
// after glfwMakeContextCurrent, before any of these are called.

extern void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
extern void (*Clear)(GLbitfield);
extern void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
extern void (*Enable)(GLenum);
extern void (*Disable)(GLenum);
extern void (*BlendFunc)(GLenum, GLenum);
extern void (*Scissor)(GLint, GLint, GLsizei, GLsizei);
extern void (*PolygonMode)(GLenum, GLenum);
extern void (*LineWidth)(GLfloat);
extern void (*PixelStorei)(GLenum, GLint);
extern GLenum (*GetError)();
extern const GLubyte *(*GetString)(GLenum);
extern void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);

extern void (*GenTextures)(GLsizei, GLuint *);
extern void (*DeleteTextures)(GLsizei, const GLuint *);
extern void (*BindTexture)(GLenum, GLuint);
extern void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
extern void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
extern void (*TexParameteri)(GLenum, GLenum, GLint);
extern void (*ActiveTexture)(GLenum);
extern void (*GenerateMipmap)(GLenum);

extern void (*GenBuffers)(GLsizei, GLuint *);
extern void (*DeleteBuffers)(GLsizei, const GLuint *);
extern void (*BindBuffer)(GLenum, GLuint);
extern void (*BufferData)(GLenum, GLsizeiptr, const void *, GLenum);
extern void (*BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);

extern void (*GenVertexArrays)(GLsizei, GLuint *);
extern void (*DeleteVertexArrays)(GLsizei, const GLuint *);
extern void (*BindVertexArray)(GLuint);
extern void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
extern void (*EnableVertexAttribArray)(GLuint);
extern void (*DisableVertexAttribArray)(GLuint);

extern GLuint (*CreateShader)(GLenum);
extern void (*ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
extern void (*CompileShader)(GLuint);
extern void (*GetShaderiv)(GLuint, GLenum, GLint *);
extern void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
extern void (*DeleteShader)(GLuint);
extern GLuint (*CreateProgram)();
extern void (*AttachShader)(GLuint, GLuint);
extern void (*LinkProgram)(GLuint);
extern void (*GetProgramiv)(GLuint, GLenum, GLint *);
extern void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
extern void (*DeleteProgram)(GLuint);
extern void (*UseProgram)(GLuint);
extern GLint (*GetUniformLocation)(GLuint, const GLchar *);
extern void (*Uniform1i)(GLint, GLint);
extern void (*Uniform1f)(GLint, GLfloat);
extern void (*Uniform2f)(GLint, GLfloat, GLfloat);
extern void (*Uniform3f)(GLint, GLfloat, GLfloat, GLfloat);
extern void (*Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
extern void (*UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat *);

extern void (*DrawArrays)(GLenum, GLint, GLsizei);
extern void (*DrawElements)(GLenum, GLsizei, GLenum, const void *);

extern void (*GenFramebuffers)(GLsizei, GLuint *);
extern void (*DeleteFramebuffers)(GLsizei, const GLuint *);
extern void (*BindFramebuffer)(GLenum, GLuint);
extern void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
extern GLenum (*CheckFramebufferStatus)(GLenum);
extern void (*GenRenderbuffers)(GLsizei, GLuint *);
extern void (*DeleteRenderbuffers)(GLsizei, const GLuint *);
extern void (*BindRenderbuffer)(GLenum, GLuint);
extern void (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
extern void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);

// Resolves every function pointer above via `get_proc_address` (pass
// glfwGetProcAddress). Returns false if any of them failed to resolve
// (logged to stderr with the missing symbol's name) -- callers should
// treat that as fatal, same as a failed InitWindow.
using GLProcAddressFn = void *(*)(const char *);
bool LoadGLFunctions(GLProcAddressFn get_proc_address);

}  // namespace gl
}  // namespace gfx
