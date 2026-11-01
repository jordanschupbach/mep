#include "editor.h"
#include "gfx/backend.h"
#include "gfx/backend_native.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

float GetFontSizePx();
float GetFontSizePx() { return 16.0f; }
float GetCharWidthPx();
float GetCharWidthPx() { return 8.0f; }
void InvalidateOrgInlineImageTexture(const std::string &);
void InvalidateOrgInlineImageTexture(const std::string &) {}
void SetFontSizePx(float);
void SetFontSizePx(float) {}
std::string MathRenderFastPng(const std::string &, float, const std::string &);
std::string MathRenderFastPng(const std::string &, float, const std::string &) { return {}; }

namespace {
int failures = 0;
void Check(bool ok, const char *label) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", label);
    if (!ok) failures++;
}
class Keyboard final : public gfx::IInputBackend {
public:
    int character = 0;
    gfx::Key queued = gfx::Key::None, pressed = gfx::Key::None;
    bool IsKeyPressed(gfx::Key key) override { return key == pressed; }
    bool IsKeyPressedRepeat(gfx::Key) override { return false; }
    bool IsKeyDown(gfx::Key) override { return false; }
    bool IsKeyReleased(gfx::Key) override { return false; }
    gfx::Key GetKeyPressed() override { const auto key = queued; queued = gfx::Key::None; return key; }
    int GetCharPressed() override { const int c = character; character = 0; return c; }
    bool WindowFocusLostThisFrame() override { return false; }
    bool IsMouseButtonPressed(gfx::MouseButton) override { return false; }
    bool IsMouseButtonDown(gfx::MouseButton) override { return false; }
    bool IsMouseButtonReleased(gfx::MouseButton) override { return false; }
    gfx::Vector2 GetMousePosition() override { return {}; }
    gfx::Vector2 GetMouseWheelMoveV() override { return {}; }
    void Press(Editor &ed, gfx::Key key) { queued = pressed = key; ed.HandleInput(); queued = pressed = gfx::Key::None; }
    void Type(Editor &ed, const std::string &text) {
        for (char c : text) { character = static_cast<unsigned char>(c); ed.HandleInput(); }
    }
};
int Find(const Editor &ed, const std::string &key) {
    const auto &rows = ed.OrgSettingsRows();
    for (size_t i = 0; i < rows.size(); i++) if (rows[i].spec.key == key) return static_cast<int>(i);
    return -1;
}
void Set(Editor &ed, Keyboard &keyboard, const std::string &key, const std::string &value) {
    const int index = Find(ed, key);
    Check(index >= 0, ("menu has " + (key.empty() ? "Add option" : key)).c_str());
    if (index < 0) return;
    const OrgHeaderArgSpec spec = ed.OrgSettingsRows()[static_cast<size_t>(index)].spec;
    ed.OrgSettingsSelect(index, true);
    if (spec.list_popup) {
        const auto choice = std::find(spec.choices.begin(), spec.choices.end(), value);
        if (choice != spec.choices.end()) ed.OrgSettingsListChoose(static_cast<int>(choice - spec.choices.begin()));
        else { keyboard.Type(ed, value); keyboard.Press(ed, gfx::Key::Enter); }
    } else {
        const size_t n = ed.OrgSettingsPending().size();
        for (size_t i = 0; i < n; i++) keyboard.Press(ed, gfx::Key::Backspace);
        keyboard.Type(ed, value);
        keyboard.Press(ed, gfx::Key::Enter);
    }
}
std::string Option(const Editor &ed, const std::string &key) {
    const int index = Find(ed, key);
    return index < 0 ? "" : ed.OrgSettingsRows()[static_cast<size_t>(index)].value;
}
}  // namespace

int main() {
    auto backends = gfx::ToBackends(gfx::CreateNativeBackendSet());
    Keyboard keyboard;
    backends.input = &keyboard;
    gfx::SetBackends(backends);
    const std::string scratch = (std::filesystem::temp_directory_path() / "mep-block-settings-test").string();
    std::filesystem::create_directories(scratch);
#if defined(_WIN32)
    _putenv_s("XDG_DATA_HOME", scratch.c_str());
#else
    setenv("XDG_DATA_HOME", scratch.c_str(), 1);
#endif
    Editor ed;
    const int buf = ed.CurrentBufferId();
    ed.SetBufferFilenameForLua(buf, scratch + "/settings.mepml");
    ed.SetBufferLinesForLua(buf, {"//? Option: file=\"old plot.png\"", "//? Option: eval=false",
        "```{cpp, eval=true, flags=\"-O2\", custom=\"a,b\"}", "int main() { return 0; }", "```", "", "```python", "print(1)", "```"});
    ed.SetCursorForLua(3, 0);
    Check(ed.OpenOrgBlockSettingsAtCursor(), "a MEPML code block opens the shared settings menu");
    Check(ed.OrgSettingsTitle() == "cpp block settings", "menu identifies the block language");
    for (const char *key : {"eval", "results", "file", "exports", "echo", "include", "lines", "region", "dedent", "rows", "cols", "flags", "libs", "custom"})
        Check(Find(ed, key) >= 0, key);
    Check(Option(ed, "eval") == "true" && Option(ed, "file") == "old plot.png", "values combine affiliated options with fence overrides");
    Set(ed, keyboard, "results", "exec-gui");
    Check(Option(ed, "results") == "exec-gui", "MEPML results dropdown writes its own execution modes");
    Set(ed, keyboard, "file", "new plot, v2.png");
    Check(ed.GetBuffer(buf).lines[0] == "//? Option: file=\"new plot, v2.png\"", "editing an affiliated option writes it back to its own line");
    Set(ed, keyboard, "eval", "");
    Check(Option(ed, "eval").empty() && ed.GetBuffer(buf).lines[1].find("eval=") == std::string::npos,
          "clearing an overridden option removes all copies rather than revealing an older value");
    Check(ed.Cursor().row == 2 && ed.GetBuffer(buf).lines[2] == "int main() { return 0; }", "removing an option-only line preserves the code and cursor location");
    Set(ed, keyboard, "flags", "-DNAME=\"a,b\" -O3");
    Check(Option(ed, "flags") == "-DNAME=\"a,b\" -O3", "compiler flags survive quoting and reparsing");
    Set(ed, keyboard, "custom", "");
    Check(Find(ed, "custom") < 0, "unknown options can be removed");
    Set(ed, keyboard, "", "timeout=10");
    Check(Option(ed, "timeout") == "10", "new custom options become editable rows");
    Set(ed, keyboard, "rows", "24");
    Check(Option(ed, "rows") == "24", "numeric fields write MEPML numeric values");
    ed.CloseOrgBlockSettings();
    Check(ed.CurrentMode() == Mode::Normal && ed.GetBuffer(buf).modified, "closing the popup leaves an ordinary unsaved source edit");
    keyboard.Type(ed, "u");
    Check(ed.GetBuffer(buf).lines[1].find("rows=") == std::string::npos, "one undo reverses one committed option");
    ed.SetCursorForLua(6, 0);
    Check(ed.OpenOrgBlockSettingsAtCursor(), "a second block gets its own settings");
    Check(Find(ed, "python") >= 0 && Find(ed, "flags") < 0, "language-specific settings follow the selected block");
    Set(ed, keyboard, "include", "src/a file.py");
    Check(ed.GetBuffer(buf).lines[5].find("include=\"src/a file.py\"") != std::string::npos, "new options attach to the selected block's fence");
    ed.CloseOrgBlockSettings();
    ed.SetBufferFilenameForLua(buf, scratch + "/settings.org");
    ed.SetBufferLinesForLua(buf, {"#+begin_src python :results output :file old.png", "print(1)", "#+end_src"});
    ed.SetCursorForLua(1, 0);
    Check(ed.OpenOrgBlockSettingsAtCursor(), "Org settings still open");
    Set(ed, keyboard, "file", "updated.png");
    Check(ed.GetBuffer(buf).lines[0] == "#+begin_src python :results output :file updated.png", "Org settings retain Org syntax and result facets");
    ed.CloseOrgBlockSettings();
    std::printf("%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
