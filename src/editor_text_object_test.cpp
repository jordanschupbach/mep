// Drive the actual keyboard dispatch without a window. Selection endpoints,
// selected text and subsequent operators must agree, including in documents.
#include "editor.h"
#include "gfx/backend.h"
#include "gfx/backend_native.h"

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
class Keyboard final : public gfx::IInputBackend {
public:
    int character = 0;
    bool escape = false;
    bool IsKeyPressed(gfx::Key key) override { return escape && key == gfx::Key::Escape; }
    bool IsKeyPressedRepeat(gfx::Key) override { return false; }
    bool IsKeyDown(gfx::Key) override { return false; }
    bool IsKeyReleased(gfx::Key) override { return false; }
    gfx::Key GetKeyPressed() override { return gfx::Key::None; }
    int GetCharPressed() override { int c = character; character = 0; return c; }
    bool WindowFocusLostThisFrame() override { return false; }
    bool IsMouseButtonPressed(gfx::MouseButton) override { return false; }
    bool IsMouseButtonDown(gfx::MouseButton) override { return false; }
    bool IsMouseButtonReleased(gfx::MouseButton) override { return false; }
    gfx::Vector2 GetMousePosition() override { return {}; }
    gfx::Vector2 GetMouseWheelMoveV() override { return {}; }

    void Keys(Editor &ed, const std::string &keys) {
        for (char raw : keys) {
            const auto c = static_cast<unsigned char>(raw);
            character = c == 27 ? 0 : c;
            escape = c == 27;
            ed.HandleInput();
            escape = false;
        }
    }
};
int failures = 0;
void Expect(bool ok, const char *label) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", label);
    if (!ok) failures++;
}
void Selection(const Editor &ed, int first, int last, const std::string &text, const char *label) {
    CursorPos start{}, end{};
    ed.VisualRange(start, end);
    const std::string selected = ed.CurrentVisualSelectionText();
    const bool ok = ed.CurrentMode() == Mode::VisualLine && start.row == first && end.row == last && selected == text;
    Expect(ok, label);
    if (!ok) std::printf("  mode=%d rows=%d..%d text=[%s]\n", static_cast<int>(ed.CurrentMode()), start.row, end.row, selected.c_str());
}
}  // namespace

int main() {
    auto backends = gfx::ToBackends(gfx::CreateNativeBackendSet());
    Keyboard keyboard;
    backends.input = &keyboard;
    gfx::SetBackends(backends);
    const std::string scratch = (std::filesystem::temp_directory_path() / "mep-text-object-test").string();
    std::filesystem::create_directories(scratch);
#if defined(_WIN32)
    _putenv_s("XDG_DATA_HOME", scratch.c_str());
#else
    setenv("XDG_DATA_HOME", scratch.c_str(), 1);
#endif
    const std::vector<std::string> lines = {"First line.", "Second line.", "  \t", "", "Next paragraph.", "", "Last paragraph."};
    Editor ed;
    const int buf = ed.CurrentBufferId();
    auto reset = [&](int row) {
        keyboard.Keys(ed, "\x1b");
        ed.SetBufferLinesForLua(buf, lines);
        ed.SetCursorForLua(row, 4);
    };
    reset(1);
    keyboard.Keys(ed, "vip");
    Selection(ed, 0, 1, "First line.\nSecond line.\n", "vip stops at whitespace-only separators");
    keyboard.Keys(ed, "ip");
    Selection(ed, 0, 3, "First line.\nSecond line.\n  \t\n\n", "repeating ip extends through the separator run");
    keyboard.Keys(ed, "ip");
    Selection(ed, 0, 4, "First line.\nSecond line.\n  \t\n\nNext paragraph.\n", "repeating ip again includes the next paragraph");
    reset(1);
    keyboard.Keys(ed, "vap");
    Selection(ed, 0, 3, "First line.\nSecond line.\n  \t\n\n", "vap includes trailing blank lines");
    reset(1);
    keyboard.Keys(ed, "v2ip");
    Selection(ed, 0, 3, "First line.\nSecond line.\n  \t\n\n", "visual paragraph counts include successive runs");
    keyboard.Keys(ed, "j");
    CursorPos start{}, end{};
    ed.VisualRange(start, end);
    Expect(end.row == 4, "the paragraph count does not leak into the next motion");
    reset(1);
    keyboard.Keys(ed, "v2ap");
    Selection(ed, 0, 5, "First line.\nSecond line.\n  \t\n\nNext paragraph.\n\n", "counted ap includes two paragraphs and their separators");
    reset(2);
    keyboard.Keys(ed, "vip");
    Selection(ed, 2, 3, "  \t\n\n", "vip on whitespace selects only the blank run");
    reset(6);
    keyboard.Keys(ed, "vap");
    Selection(ed, 5, 6, "\nLast paragraph.\n", "vap at EOF includes the preceding separator");
    reset(4);
    keyboard.Keys(ed, "vipoip");
    Selection(ed, 2, 4, "  \t\n\nNext paragraph.\n", "repeated ip expands backwards after o");
    reset(1);
    keyboard.Keys(ed, "dip");
    Expect(ed.GetBuffer(buf).lines == std::vector<std::string>({"  \t", "", "Next paragraph.", "", "Last paragraph."}), "dip uses the same whitespace-aware paragraph range");
    reset(1);
    keyboard.Keys(ed, "d2ip");
    Expect(ed.GetBuffer(buf).lines == std::vector<std::string>({"Next paragraph.", "", "Last paragraph."}), "operator paragraph counts use the same resolver");
    reset(1);
    keyboard.Keys(ed, "vipiw");
    Expect(ed.CurrentMode() == Mode::Visual && ed.CurrentVisualSelectionText() == "Second", "a word object after vip returns to characterwise selection");
    reset(1);
    keyboard.Keys(ed, "v3iwj");
    ed.VisualRange(start, end);
    Expect(end.row == 2, "an unsupported word-object count is consumed rather than changing the next motion");
    reset(1);
    keyboard.Keys(ed, "viwip");
    Selection(ed, 0, 1, "First line.\nSecond line.\n", "ip expands a partial word selection to its paragraph");
    reset(1);
    keyboard.Keys(ed, "vipoy");
    Expect(!ed.HasVisualSelection() && ed.GetBuffer(buf).lines == lines, "yanking a reversed paragraph leaves the buffer unchanged");
    reset(1);
    keyboard.Keys(ed, "vipd");
    Expect(ed.GetBuffer(buf).lines == std::vector<std::string>({"  \t", "", "Next paragraph.", "", "Last paragraph."}), "visual delete agrees with the highlighted paragraph");
    reset(0);
    keyboard.Keys(ed, "v99ip");
    Expect(ed.CurrentMode() == Mode::Visual, "an out-of-range paragraph count leaves the previous selection intact");
    keyboard.Keys(ed, "\x1b");
    ed.SetBufferLinesForLua(buf, {"Only paragraph.", " \t", ""});
    ed.SetCursorForLua(1, 0);
    keyboard.Keys(ed, "vap");
    Selection(ed, 1, 2, " \t\n\n", "vap in trailing whitespace does not select the preceding paragraph");
    keyboard.Keys(ed, "\x1b");
    ed.SetBufferLinesForLua(buf, {""});
    ed.SetCursorForLua(0, 0);
    keyboard.Keys(ed, "vip");
    Selection(ed, 0, 0, "\n", "vip works in an empty buffer");
    keyboard.Keys(ed, "\x1b");
    ed.SetBufferLinesForLua(buf, {std::string(320, 'x'), "Last source line.", "", "Another paragraph."});
    ed.SetCursorForLua(0, 0);
    Pane &pane = ed.MutableActiveTabRoot()->pane;
    pane.scroll_row = pane.scroll_sub = pane.scroll_sub_row = 0;
    keyboard.Keys(ed, "vip");
    ed.UpdateScrollForPane(ed.ActivePaneId(), 10, 20);
    Expect(pane.scroll_row == 0 && pane.scroll_sub == 7, "vip keeps wrapped paragraph text visible instead of scrolling it off screen");
    ed.UpdateScrollForPane(ed.ActivePaneId(), 10, 20);
    Expect(pane.scroll_row == 0 && pane.scroll_sub == 7, "the wrapped paragraph highlight remains stable on subsequent frames");
    keyboard.Keys(ed, "\x1b");
    ed.SetBufferFilenameForLua(buf, scratch + "/example.mepml");
    ed.SetBufferLinesForLua(buf, {"# Heading", "", "A *styled* paragraph.", "Its second line.", "\t", "Another paragraph."});
    ed.MepmlScan(ed.CreateNamespace("mepml-test"));
    ed.SetCursorForLua(3, 4);
    keyboard.Keys(ed, "vip");
    Selection(ed, 2, 3, "A *styled* paragraph.\nIts second line.\n", "rendered MEPML preserves source paragraph endpoints");
    // r{char}: replaces the character under the cursor, including chars the
    // dispatcher would otherwise read as a count or a register prefix.
    keyboard.Keys(ed, "\x1b");
    ed.SetBufferFilenameForLua(buf, scratch + "/replace.txt");
    for (const std::string &ch : {std::string("x"), std::string("5"), std::string("0"), std::string("\""),
                                  std::string("j"), std::string("r")}) {
        ed.SetBufferLinesForLua(buf, {"abcdef"});
        ed.SetCursorForLua(0, 2);
        keyboard.Keys(ed, "r" + ch);
        const std::string want = "ab" + ch + "def";
        const bool ok = ed.GetBuffer(buf).lines == std::vector<std::string>({want}) && ed.CurrentMode() == Mode::Normal;
        Expect(ok, ("r" + ch + " replaces the character under the cursor").c_str());
        if (!ok) std::printf("  line=[%s]\n", ed.GetBuffer(buf).lines[0].c_str());
        keyboard.Keys(ed, "\x1b");
    }
    ed.SetBufferLinesForLua(buf, {"abcdef"});
    ed.SetCursorForLua(0, 1);
    keyboard.Keys(ed, "3rz");
    Expect(ed.GetBuffer(buf).lines == std::vector<std::string>({"azzzef"}), "3rz replaces three characters");
    std::printf("%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
