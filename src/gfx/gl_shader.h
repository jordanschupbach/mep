#pragma once

// Small GLSL compile/link helper shared by the 2D and (later) 3D pieces
// of the native backend. Not part of gl_loader.h since it's a thin
// convenience on top of the raw function pointers there, not itself part
// of the GL binding.

#include "gfx/gl_loader.h"

namespace gfx {
namespace gl {

// Compiles one shader stage. Returns 0 and prints the compile log to
// stderr on failure. Named CompileShaderStage, not CompileShader, to
// avoid colliding with gl_loader.h's raw `gl::CompileShader` function
// pointer (glCompileShader) in this same gfx::gl namespace.
GLuint CompileShaderStage(GLenum type, const char *source);

// Links a vertex+fragment shader pair into a program, deleting both
// shader objects afterward either way (a linked program keeps its own
// copy). Returns 0 and prints the link log to stderr on failure. Named
// LinkShaderProgram for the same reason as CompileShaderStage above.
GLuint LinkShaderProgram(GLuint vertex_shader, GLuint fragment_shader);

// Convenience: compiles and links a vertex+fragment source pair in one
// call. Returns 0 on any failure (already logged by the two calls above).
GLuint BuildProgram(const char *vertex_source, const char *fragment_source);

}  // namespace gl
}  // namespace gfx
