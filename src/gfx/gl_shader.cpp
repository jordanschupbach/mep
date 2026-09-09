#include "gfx/gl_shader.h"

#include <cstdio>
#include <vector>

namespace gfx {
namespace gl {

namespace {
// Callers pass shader bodies with no #version line of their own (see
// each shader source's own comment) -- this is prepended instead, so the
// same GLSL body compiles on both desktop GL 3.3 core and, under
// Emscripten, WebGL2/GLES 3.0, which needs its own #version token and
// (unlike desktop, where it's implied) an explicit default float
// precision.
#ifdef __EMSCRIPTEN__
constexpr const char *kGlslPreamble = "#version 300 es\nprecision highp float;\n";
#else
constexpr const char *kGlslPreamble = "#version 330 core\n";
#endif
}  // namespace

GLuint CompileShaderStage(GLenum type, const char *source) {
    GLuint shader = CreateShader(type);
    const char *sources[2] = {kGlslPreamble, source};
    ShaderSource(shader, 2, sources, nullptr);
    CompileShader(shader);
    GLint compiled = 0;
    GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == 0) {
        GLint log_len = 0;
        GetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
        std::vector<char> log(static_cast<size_t>(log_len > 0 ? log_len : 1));
        GetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
        std::fprintf(stderr, "gfx::gl: shader compile failed: %s\n", log.data());
        DeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint LinkShaderProgram(GLuint vertex_shader, GLuint fragment_shader) {
    GLuint program = CreateProgram();
    AttachShader(program, vertex_shader);
    AttachShader(program, fragment_shader);
    LinkProgram(program);
    GLint linked = 0;
    GetProgramiv(program, GL_LINK_STATUS, &linked);
    DeleteShader(vertex_shader);
    DeleteShader(fragment_shader);
    if (linked == 0) {
        GLint log_len = 0;
        GetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
        std::vector<char> log(static_cast<size_t>(log_len > 0 ? log_len : 1));
        GetProgramInfoLog(program, static_cast<GLsizei>(log.size()), nullptr, log.data());
        std::fprintf(stderr, "gfx::gl: program link failed: %s\n", log.data());
        DeleteProgram(program);
        return 0;
    }
    return program;
}

GLuint BuildProgram(const char *vertex_source, const char *fragment_source) {
    GLuint vs = CompileShaderStage(GL_VERTEX_SHADER, vertex_source);
    if (vs == 0) return 0;
    GLuint fs = CompileShaderStage(GL_FRAGMENT_SHADER, fragment_source);
    if (fs == 0) {
        DeleteShader(vs);
        return 0;
    }
    return LinkShaderProgram(vs, fs);
}

}  // namespace gl
}  // namespace gfx
