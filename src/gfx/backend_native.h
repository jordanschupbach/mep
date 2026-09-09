#pragma once

// mep's in-house gfx:: backend (formerly "Stage B" during the raylib
// removal -- see PLAN): GLFW for windowing/input/context management,
// hand-rolled OpenGL 3.3 core / OpenGL ES 3.0 (gfx/gl_loader.h) for
// rendering, an in-house ALSA backend for audio (see
// MINIAUDIO_REMOVAL_PLAN.md), and hand-written importers for OBJ/
// IQM/VOX/M3D/glTF model files. Implements all six gfx:: interfaces (see
// gfx/backend.h) and is the only backend main.cpp installs via
// gfx::SetBackends.

#include "gfx/backend.h"

struct GLFWwindow;

namespace gfx {

struct NativeBackendSet;
// Creates the backend set. No window exists yet at this point --
// IPlatformBackend::InitWindow() does glfwInit/glfwCreateWindow/
// glfwMakeContextCurrent/gfx::gl::LoadGLFunctions itself, the "InitWindow
// does everything" contract every gfx:: caller relies on.
NativeBackendSet *CreateNativeBackendSet();
Backends ToBackends(NativeBackendSet *set);

}  // namespace gfx
