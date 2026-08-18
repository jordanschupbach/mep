#include "raylib.h"

#include <cmath>
#include <string>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#endif

namespace {

constexpr int kScreenWidth = 900;
constexpr int kScreenHeight = 540;
constexpr int kFontSize = 20;
constexpr int kLineHeight = kFontSize + 6;
constexpr int kMarginX = 12;
constexpr int kMarginY = 12;
constexpr int kStatusBarHeight = kLineHeight;

struct EditorState {
    std::vector<std::string> lines{""};
    int cursorRow = 0;
    int cursorCol = 0;
    int scrollRow = 0;
};

EditorState g_editor;

int ClampInt(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

void MoveCursorTo(int row, int col) {
    g_editor.cursorRow = ClampInt(row, 0, static_cast<int>(g_editor.lines.size()) - 1);
    g_editor.cursorCol = ClampInt(col, 0, static_cast<int>(g_editor.lines[g_editor.cursorRow].size()));
}

void InsertChar(int codepoint) {
    std::string &line = g_editor.lines[g_editor.cursorRow];
    // Only handle single-byte (ASCII) input for this minimal editor.
    if (codepoint < 32 || codepoint > 126) return;
    line.insert(line.begin() + g_editor.cursorCol, static_cast<char>(codepoint));
    g_editor.cursorCol++;
}

void InsertNewline() {
    std::string &line = g_editor.lines[g_editor.cursorRow];
    std::string remainder = line.substr(g_editor.cursorCol);
    line.erase(g_editor.cursorCol);
    g_editor.lines.insert(g_editor.lines.begin() + g_editor.cursorRow + 1, remainder);
    MoveCursorTo(g_editor.cursorRow + 1, 0);
}

void Backspace() {
    if (g_editor.cursorCol > 0) {
        std::string &line = g_editor.lines[g_editor.cursorRow];
        line.erase(g_editor.cursorCol - 1, 1);
        g_editor.cursorCol--;
    } else if (g_editor.cursorRow > 0) {
        std::string current = g_editor.lines[g_editor.cursorRow];
        g_editor.lines.erase(g_editor.lines.begin() + g_editor.cursorRow);
        int prevRow = g_editor.cursorRow - 1;
        int joinCol = static_cast<int>(g_editor.lines[prevRow].size());
        g_editor.lines[prevRow] += current;
        MoveCursorTo(prevRow, joinCol);
    }
}

void DeleteForward() {
    std::string &line = g_editor.lines[g_editor.cursorRow];
    if (g_editor.cursorCol < static_cast<int>(line.size())) {
        line.erase(g_editor.cursorCol, 1);
    } else if (g_editor.cursorRow + 1 < static_cast<int>(g_editor.lines.size())) {
        std::string next = g_editor.lines[g_editor.cursorRow + 1];
        g_editor.lines.erase(g_editor.lines.begin() + g_editor.cursorRow + 1);
        line += next;
    }
}

void HandleTextInput() {
    int codepoint = GetCharPressed();
    while (codepoint > 0) {
        InsertChar(codepoint);
        codepoint = GetCharPressed();
    }

    if (IsKeyPressed(KEY_ENTER) || IsKeyPressedRepeat(KEY_ENTER)) {
        InsertNewline();
    }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        Backspace();
    }
    if (IsKeyPressed(KEY_DELETE) || IsKeyPressedRepeat(KEY_DELETE)) {
        DeleteForward();
    }
    if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) {
        if (g_editor.cursorCol > 0) {
            MoveCursorTo(g_editor.cursorRow, g_editor.cursorCol - 1);
        } else if (g_editor.cursorRow > 0) {
            MoveCursorTo(g_editor.cursorRow - 1, static_cast<int>(g_editor.lines[g_editor.cursorRow - 1].size()));
        }
    }
    if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) {
        if (g_editor.cursorCol < static_cast<int>(g_editor.lines[g_editor.cursorRow].size())) {
            MoveCursorTo(g_editor.cursorRow, g_editor.cursorCol + 1);
        } else if (g_editor.cursorRow + 1 < static_cast<int>(g_editor.lines.size())) {
            MoveCursorTo(g_editor.cursorRow + 1, 0);
        }
    }
    if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) {
        MoveCursorTo(g_editor.cursorRow - 1, g_editor.cursorCol);
    }
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) {
        MoveCursorTo(g_editor.cursorRow + 1, g_editor.cursorCol);
    }
    if (IsKeyPressed(KEY_HOME)) {
        MoveCursorTo(g_editor.cursorRow, 0);
    }
    if (IsKeyPressed(KEY_END)) {
        MoveCursorTo(g_editor.cursorRow, static_cast<int>(g_editor.lines[g_editor.cursorRow].size()));
    }
}

void UpdateScroll() {
    int visibleLines = (kScreenHeight - kMarginY * 2 - kStatusBarHeight) / kLineHeight;
    if (g_editor.cursorRow < g_editor.scrollRow) {
        g_editor.scrollRow = g_editor.cursorRow;
    } else if (g_editor.cursorRow >= g_editor.scrollRow + visibleLines) {
        g_editor.scrollRow = g_editor.cursorRow - visibleLines + 1;
    }
}

void DrawEditor() {
    BeginDrawing();
    ClearBackground(Color{30, 30, 30, 255});

    int visibleLines = (kScreenHeight - kMarginY * 2 - kStatusBarHeight) / kLineHeight;
    int lastLine = ClampInt(g_editor.scrollRow + visibleLines, 0, static_cast<int>(g_editor.lines.size()));

    for (int row = g_editor.scrollRow; row < lastLine; ++row) {
        int y = kMarginY + (row - g_editor.scrollRow) * kLineHeight;
        DrawText(g_editor.lines[row].c_str(), kMarginX, y, kFontSize, RAYWHITE);
    }

    // Blinking cursor.
    if (fmodf(static_cast<float>(GetTime()), 1.0f) < 0.5f) {
        const std::string &line = g_editor.lines[g_editor.cursorRow];
        std::string prefix = line.substr(0, g_editor.cursorCol);
        int cursorX = kMarginX + MeasureText(prefix.c_str(), kFontSize);
        int cursorY = kMarginY + (g_editor.cursorRow - g_editor.scrollRow) * kLineHeight;
        DrawRectangle(cursorX, cursorY, 2, kFontSize, RAYWHITE);
    }

    // Status bar.
    int statusY = kScreenHeight - kStatusBarHeight;
    DrawRectangle(0, statusY, kScreenWidth, kStatusBarHeight, Color{45, 45, 48, 255});
    std::string status = "mep  |  Ln " + std::to_string(g_editor.cursorRow + 1) +
                          ", Col " + std::to_string(g_editor.cursorCol + 1);
    DrawText(status.c_str(), kMarginX, statusY + 3, kFontSize - 4, LIGHTGRAY);

    EndDrawing();
}

void UpdateDrawFrame() {
    HandleTextInput();
    UpdateScroll();
    DrawEditor();
}

}  // namespace

int main() {
    InitWindow(kScreenWidth, kScreenHeight, "mep");

#if defined(__EMSCRIPTEN__)
    emscripten_set_main_loop(UpdateDrawFrame, 0, 1);
#else
    SetTargetFPS(60);
    while (!WindowShouldClose()) {
        UpdateDrawFrame();
    }
#endif

    CloseWindow();
    return 0;
}
