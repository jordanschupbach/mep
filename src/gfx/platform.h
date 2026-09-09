#pragma once

// Facade over IPlatformBackend (see gfx/backend.h). Free functions with
// the same names/shapes as the raylib calls they replace, so retargeting
// main.cpp's window-lifecycle code is a mechanical `gfx::` prefix.

#include <string>

#include "gfx/backend.h"
#include "gfx/types.h"

namespace gfx {

inline void InitWindow(int width, int height, const char *title) {
    GetBackends().platform->InitWindow(width, height, title);
}
inline void CloseWindow() { GetBackends().platform->CloseWindow(); }
inline bool IsWindowReady() { return GetBackends().platform->IsWindowReady(); }
inline bool WindowShouldClose() { return GetBackends().platform->WindowShouldClose(); }
inline void SetWindowResizable() { GetBackends().platform->SetWindowResizable(); }
inline void MaximizeWindow() { GetBackends().platform->MaximizeWindow(); }
inline void SetTargetFPS(int fps) { GetBackends().platform->SetTargetFPS(fps); }
inline int GetScreenWidth() { return GetBackends().platform->GetScreenWidth(); }
inline int GetScreenHeight() { return GetBackends().platform->GetScreenHeight(); }
inline double GetTime() { return GetBackends().platform->GetTime(); }
inline float GetFrameTime() { return GetBackends().platform->GetFrameTime(); }
inline void SetExitKey(Key key) { GetBackends().platform->SetExitKey(key); }
inline std::string GetClipboardText() { return GetBackends().platform->GetClipboardText(); }
inline void SetClipboardText(const std::string &text) { GetBackends().platform->SetClipboardText(text); }
inline void SetMouseCursor(MouseCursor cursor) { GetBackends().platform->SetMouseCursor(cursor); }
inline void *GetNativeWindowHandle() { return GetBackends().platform->GetNativeWindowHandle(); }
inline void SetTraceLogCallback(IPlatformBackend::TraceLogCallback callback) {
    GetBackends().platform->SetTraceLogCallback(callback);
}

}  // namespace gfx
