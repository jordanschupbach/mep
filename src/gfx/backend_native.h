#pragma once

// mep's in-house gfx:: backend (formerly "Stage B" during the raylib
// removal -- see PLAN, and GLFW_REMOVAL_PLAN.md for the X11/GLX
// windowing swap): a hand-written X11/GLX windowing/input/context layer
// (gfx/backend_native.cpp), hand-rolled OpenGL 3.3 core / OpenGL ES 3.0
// (gfx/gl_loader.h) for rendering, an in-house ALSA backend for audio
// (see MINIAUDIO_REMOVAL_PLAN.md), and hand-written importers for OBJ/
// IQM/VOX/M3D/glTF model files. Implements all six gfx:: interfaces (see
// gfx/backend.h) and is the only backend main.cpp installs via
// gfx::SetBackends.

#include "gfx/backend.h"

namespace gfx {

struct NativeBackendSet;
// Creates the backend set. No window exists yet at this point --
// IPlatformBackend::InitWindow() does the X11 window/GLX context
// creation and gfx::gl::LoadGLFunctions itself, the "InitWindow does
// everything" contract every gfx:: caller relies on.
NativeBackendSet *CreateNativeBackendSet();
Backends ToBackends(NativeBackendSet *set);

}  // namespace gfx
