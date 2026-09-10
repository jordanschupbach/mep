#pragma once

// The pointee of gfx::Platform::GetNativeWindowHandle() on the in-house
// X11 native backend (see GLFW_REMOVAL_PLAN.md) -- an X11 Display*/
// Window pair. Typed as void*/unsigned long here rather than Display*/
// Window so this header stays Xlib-free: Xlib.h's `Font` typedef
// collides with other code that must live in the same translation unit
// as this header (see agent_ui_input.cpp's own top comment on exactly
// this collision), so nothing that transitively pulls in Xlib.h may be
// named here.
//
// Owned by gfx::NativeContext (gfx/backend_native.cpp); the pointer
// GetNativeWindowHandle() returns stays valid for the process's
// lifetime once InitWindow() has run.

namespace gfx {

struct NativeWindowHandle {
    void *display;
    unsigned long window;
};

}  // namespace gfx
