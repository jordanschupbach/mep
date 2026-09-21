#pragma once

// Facade over IInputBackend (see gfx/backend.h). Same call shapes as the
// raylib input-polling calls used throughout editor.cpp today.

#include "gfx/backend.h"
#include "gfx/types.h"

namespace gfx {

inline bool IsKeyPressed(Key key) { return GetBackends().input->IsKeyPressed(key); }
inline bool IsKeyPressedRepeat(Key key) { return GetBackends().input->IsKeyPressedRepeat(key); }
inline bool IsKeyDown(Key key) { return GetBackends().input->IsKeyDown(key); }
inline bool IsKeyReleased(Key key) { return GetBackends().input->IsKeyReleased(key); }
inline Key GetKeyPressed() { return GetBackends().input->GetKeyPressed(); }
inline int GetCharPressed() { return GetBackends().input->GetCharPressed(); }
inline bool WindowFocusLostThisFrame() { return GetBackends().input->WindowFocusLostThisFrame(); }
inline bool IsMouseButtonPressed(MouseButton button) {
    return GetBackends().input->IsMouseButtonPressed(button);
}
inline bool IsMouseButtonDown(MouseButton button) { return GetBackends().input->IsMouseButtonDown(button); }
inline bool IsMouseButtonReleased(MouseButton button) {
    return GetBackends().input->IsMouseButtonReleased(button);
}
inline Vector2 GetMousePosition() { return GetBackends().input->GetMousePosition(); }
inline Vector2 GetMouseWheelMoveV() { return GetBackends().input->GetMouseWheelMoveV(); }

}  // namespace gfx
