#include "gfx/gl_loader.h"

#include <cstdio>

namespace gfx {
namespace gl {

void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
void (*Clear)(GLbitfield) = nullptr;
void (*Viewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
void (*Enable)(GLenum) = nullptr;
void (*Disable)(GLenum) = nullptr;
void (*BlendFunc)(GLenum, GLenum) = nullptr;
void (*Scissor)(GLint, GLint, GLsizei, GLsizei) = nullptr;
void (*PolygonMode)(GLenum, GLenum) = nullptr;
void (*LineWidth)(GLfloat) = nullptr;
void (*PixelStorei)(GLenum, GLint) = nullptr;
GLenum (*GetError)() = nullptr;
const GLubyte *(*GetString)(GLenum) = nullptr;
void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) = nullptr;

void (*GenTextures)(GLsizei, GLuint *) = nullptr;
void (*DeleteTextures)(GLsizei, const GLuint *) = nullptr;
void (*BindTexture)(GLenum, GLuint) = nullptr;
void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *) = nullptr;
void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *) = nullptr;
void (*TexParameteri)(GLenum, GLenum, GLint) = nullptr;
void (*ActiveTexture)(GLenum) = nullptr;
void (*GenerateMipmap)(GLenum) = nullptr;

void (*GenBuffers)(GLsizei, GLuint *) = nullptr;
void (*DeleteBuffers)(GLsizei, const GLuint *) = nullptr;
void (*BindBuffer)(GLenum, GLuint) = nullptr;
void (*BufferData)(GLenum, GLsizeiptr, const void *, GLenum) = nullptr;
void (*BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *) = nullptr;

void (*GenVertexArrays)(GLsizei, GLuint *) = nullptr;
void (*DeleteVertexArrays)(GLsizei, const GLuint *) = nullptr;
void (*BindVertexArray)(GLuint) = nullptr;
void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *) = nullptr;
void (*EnableVertexAttribArray)(GLuint) = nullptr;
void (*DisableVertexAttribArray)(GLuint) = nullptr;

GLuint (*CreateShader)(GLenum) = nullptr;
void (*ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *) = nullptr;
void (*CompileShader)(GLuint) = nullptr;
void (*GetShaderiv)(GLuint, GLenum, GLint *) = nullptr;
void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *) = nullptr;
void (*DeleteShader)(GLuint) = nullptr;
GLuint (*CreateProgram)() = nullptr;
void (*AttachShader)(GLuint, GLuint) = nullptr;
void (*LinkProgram)(GLuint) = nullptr;
void (*GetProgramiv)(GLuint, GLenum, GLint *) = nullptr;
void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *) = nullptr;
void (*DeleteProgram)(GLuint) = nullptr;
void (*UseProgram)(GLuint) = nullptr;
GLint (*GetUniformLocation)(GLuint, const GLchar *) = nullptr;
void (*Uniform1i)(GLint, GLint) = nullptr;
void (*Uniform1f)(GLint, GLfloat) = nullptr;
void (*Uniform2f)(GLint, GLfloat, GLfloat) = nullptr;
void (*Uniform3f)(GLint, GLfloat, GLfloat, GLfloat) = nullptr;
void (*Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
void (*UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat *) = nullptr;
void (*UniformMatrix3fv)(GLint, GLsizei, GLboolean, const GLfloat *) = nullptr;
void (*Uniform1iv)(GLint, GLsizei, const GLint *) = nullptr;
void (*Uniform1fv)(GLint, GLsizei, const GLfloat *) = nullptr;
void (*Uniform3fv)(GLint, GLsizei, const GLfloat *) = nullptr;

void (*DrawArrays)(GLenum, GLint, GLsizei) = nullptr;
void (*DrawElements)(GLenum, GLsizei, GLenum, const void *) = nullptr;

void (*GenFramebuffers)(GLsizei, GLuint *) = nullptr;
void (*DeleteFramebuffers)(GLsizei, const GLuint *) = nullptr;
void (*BindFramebuffer)(GLenum, GLuint) = nullptr;
void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
GLenum (*CheckFramebufferStatus)(GLenum) = nullptr;
void (*GenRenderbuffers)(GLsizei, GLuint *) = nullptr;
void (*DeleteRenderbuffers)(GLsizei, const GLuint *) = nullptr;
void (*BindRenderbuffer)(GLenum, GLuint) = nullptr;
void (*RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei) = nullptr;
void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint) = nullptr;

namespace {

// The caller-supplied get_proc_address (glXGetProcAddressARB on the
// native backend) hands back a function pointer as void*; every GL entry
// point is resolved through this one helper so a missing symbol is
// reported with its name instead of silently leaving a null function
// pointer that crashes on first use.
template <typename Fn>
bool Load(GLProcAddressFn get_proc_address, Fn &out, const char *name) {
    void *p = get_proc_address(name);
    if (p == nullptr) {
        std::fprintf(stderr, "gfx::gl: failed to resolve GL function '%s'\n", name);
        return false;
    }
    out = reinterpret_cast<Fn>(p);
    return true;
}

}  // namespace

bool LoadGLFunctions(GLProcAddressFn get_proc_address) {
    bool ok = true;
#define GFX_GL_LOAD(name) ok = Load(get_proc_address, name, "gl" #name) && ok
    GFX_GL_LOAD(ClearColor);
    GFX_GL_LOAD(Clear);
    GFX_GL_LOAD(Viewport);
    GFX_GL_LOAD(Enable);
    GFX_GL_LOAD(Disable);
    GFX_GL_LOAD(BlendFunc);
    GFX_GL_LOAD(Scissor);
#ifndef __EMSCRIPTEN__
    // glPolygonMode doesn't exist on OpenGL ES/WebGL -- resolving it is
    // not required for startup to succeed there; the two call sites
    // (NativeRenderer3DBackend::Enable/DisableWireMode) are themselves
    // compiled out under Emscripten, so the pointer is simply never used.
    GFX_GL_LOAD(PolygonMode);
#endif
    GFX_GL_LOAD(LineWidth);
    GFX_GL_LOAD(PixelStorei);
    GFX_GL_LOAD(GetError);
    GFX_GL_LOAD(GetString);
    GFX_GL_LOAD(ReadPixels);

    GFX_GL_LOAD(GenTextures);
    GFX_GL_LOAD(DeleteTextures);
    GFX_GL_LOAD(BindTexture);
    GFX_GL_LOAD(TexImage2D);
    GFX_GL_LOAD(TexSubImage2D);
    GFX_GL_LOAD(TexParameteri);
    GFX_GL_LOAD(ActiveTexture);
    GFX_GL_LOAD(GenerateMipmap);

    GFX_GL_LOAD(GenBuffers);
    GFX_GL_LOAD(DeleteBuffers);
    GFX_GL_LOAD(BindBuffer);
    GFX_GL_LOAD(BufferData);
    GFX_GL_LOAD(BufferSubData);

    GFX_GL_LOAD(GenVertexArrays);
    GFX_GL_LOAD(DeleteVertexArrays);
    GFX_GL_LOAD(BindVertexArray);
    GFX_GL_LOAD(VertexAttribPointer);
    GFX_GL_LOAD(EnableVertexAttribArray);
    GFX_GL_LOAD(DisableVertexAttribArray);

    GFX_GL_LOAD(CreateShader);
    GFX_GL_LOAD(ShaderSource);
    GFX_GL_LOAD(CompileShader);
    GFX_GL_LOAD(GetShaderiv);
    GFX_GL_LOAD(GetShaderInfoLog);
    GFX_GL_LOAD(DeleteShader);
    GFX_GL_LOAD(CreateProgram);
    GFX_GL_LOAD(AttachShader);
    GFX_GL_LOAD(LinkProgram);
    GFX_GL_LOAD(GetProgramiv);
    GFX_GL_LOAD(GetProgramInfoLog);
    GFX_GL_LOAD(DeleteProgram);
    GFX_GL_LOAD(UseProgram);
    GFX_GL_LOAD(GetUniformLocation);
    GFX_GL_LOAD(Uniform1i);
    GFX_GL_LOAD(Uniform1f);
    GFX_GL_LOAD(Uniform2f);
    GFX_GL_LOAD(Uniform3f);
    GFX_GL_LOAD(Uniform4f);
    GFX_GL_LOAD(UniformMatrix4fv);
    GFX_GL_LOAD(UniformMatrix3fv);
    GFX_GL_LOAD(Uniform1iv);
    GFX_GL_LOAD(Uniform1fv);
    GFX_GL_LOAD(Uniform3fv);

    GFX_GL_LOAD(DrawArrays);
    GFX_GL_LOAD(DrawElements);

    GFX_GL_LOAD(GenFramebuffers);
    GFX_GL_LOAD(DeleteFramebuffers);
    GFX_GL_LOAD(BindFramebuffer);
    GFX_GL_LOAD(FramebufferTexture2D);
    GFX_GL_LOAD(CheckFramebufferStatus);
    GFX_GL_LOAD(GenRenderbuffers);
    GFX_GL_LOAD(DeleteRenderbuffers);
    GFX_GL_LOAD(BindRenderbuffer);
    GFX_GL_LOAD(RenderbufferStorage);
    GFX_GL_LOAD(FramebufferRenderbuffer);
#undef GFX_GL_LOAD
    return ok;
}

}  // namespace gl
}  // namespace gfx
