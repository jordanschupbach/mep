#ifndef MEP_VTERM_H
#define MEP_VTERM_H

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// A cell's foreground/background color. Deliberately not raylib's Color --
// this header has no rendering dependency at all (usable/testable on its
// own) -- the embedding UI maps these to real colors at draw time. Default
// defers to whatever the UI treats as the terminal's own default fg/bg
// (mep's "Normal"/"NormalBg" highlight groups); Indexed is the classic
// 0-15 ANSI + 16-255 xterm 256-color palette; Rgb is 24-bit "true color"
// SGR (38/48;2;r;g;b).
enum class VTermColorKind : uint8_t { Default, Indexed, Rgb };
struct VTermColor {
    VTermColorKind kind = VTermColorKind::Default;
    uint8_t index = 0;
    uint8_t r = 0, g = 0, b = 0;
    bool operator==(const VTermColor &o) const {
        return kind == o.kind && index == o.index && r == o.r && g == o.g && b == o.b;
    }
};

struct VTermCell {
    std::string ch = " ";  // UTF-8 -- usually one byte, more for wide/multi-byte glyphs
    VTermColor fg;
    VTermColor bg;
    bool bold = false;
    bool faint = false;
    bool italic = false;
    bool underline = false;
    bool reverse = false;
};

// A minimal-but-broadly-compatible VT100/ANSI/xterm terminal emulator:
// parses a raw child-process output byte stream into a cursor-addressable
// grid of VTermCells, unlike kBuiltinRun's mep_ansi_render (main.cpp) --
// which only understands newlines and SGR colors, discarding cursor
// motion/erase entirely -- so this is what makes a real shell prompt or a
// full-screen program (vim, htop, less) render correctly instead of as
// garbled scrolling text.
//
// Covers: cursor motion (CUU/CUD/CUF/CUB/CNL/CPL/CHA/CUP/VPA), erase
// (ED/EL/ECH), insert/delete line/char (IL/DL/ICH/DCH), scroll regions
// (DECSTBM) and scrolling (SU/SD, and implicit scroll-at-margin on LF),
// SGR colors/attributes (16/256-indexed/24-bit, bold/faint/italic/
// underline/reverse), the alternate screen buffer (so full-screen programs
// swap in and out without trashing scrollback), cursor save/restore (both
// ESC 7/8 and CSI s/u), and UTF-8 text.
//
// Deliberately not attempted: mouse reporting, sixel/image protocols,
// bracketed-paste as anything other than a no-op, DEC special-graphics
// character sets (ESC ( / ESC ) -- the following byte is consumed and
// ignored rather than switching glyph sets), and device-status-report
// replies (CSI n / CSI c) -- these would need a way to write back to the
// PTY from inside the parser, which this class doesn't have; the couple
// of programs that block waiting on one (rare -- mostly legacy status-line
// tricks) will simply see no reply, same as a terminal that doesn't
// support the query. All unrecognized CSI/ESC/OSC/DCS sequences are
// parsed structurally (so their bytes never leak into the visible grid as
// stray text) and then silently discarded.
class VTerm {
public:
    VTerm(int rows, int cols);

    // Feeds a raw chunk of the child process's output. Chunks may split a
    // UTF-8 sequence or an escape sequence at an arbitrary byte boundary
    // (PTY reads aren't message-framed) -- partial parser state carries
    // over to the next call.
    void Feed(const std::string &data);

    // Resizes the *visible* grid in place. The alternate screen is
    // reallocated blank (a full-screen program redraws on resize
    // regardless -- it gets a SIGWINCH and repaints), while the primary
    // screen preserves as much of its existing content as fits, top-left
    // anchored, matching how real terminals behave.
    void Resize(int rows, int cols);

    int Rows() const { return rows_; }
    int Cols() const { return cols_; }
    // Row 0 is the top of the *visible* screen -- scrollback (primary
    // screen only) is addressed separately below.
    const VTermCell &At(int row, int col) const;
    int CursorRow() const { return cursor_row_; }
    int CursorCol() const { return cursor_col_; }
    bool CursorVisible() const { return cursor_visible_; }
    bool AltScreenActive() const { return alt_active_; }
    // DECCKM (CSI ?1h/?1l) -- whether the app has asked for "application
    // cursor keys" mode. The embedding UI's key encoder needs this to know
    // whether an arrow keypress should send CSI (`ESC [ A`) or SS3
    // (`ESC O A`) -- vim, among others, switches this on.
    bool ApplicationCursorKeys() const { return app_cursor_keys_; }

    int ScrollbackLines() const { return static_cast<int>(scrollback_.size()); }
    const VTermCell &ScrollbackAt(int row, int col) const;

    // Window title set via an OSC 0/1/2 sequence (`ESC ] 0 ; ... BEL`), if
    // any -- surfaced so a terminal pane's header can show it, the same
    // way a real terminal emulator's tab/window title works.
    const std::string &Title() const { return title_; }

private:
    // StringTerm collects an OSC/DCS/PM/APC payload until a terminator;
    // StringTermEsc is the one-byte lookahead after seeing ESC inside one,
    // to tell a real ST (ESC \) from anything else. CharsetDesignate is
    // the one-byte lookahead after ESC ( / ) / * / + (see class comment --
    // the designator itself is consumed and ignored, no charset switching
    // is modeled).
    enum class ParseState { Ground, Escape, CsiEntry, StringTerm, StringTermEsc, CharsetDesignate, Utf8Cont };

    int rows_, cols_;
    std::vector<VTermCell> primary_;  // rows_ * cols_, row-major
    std::vector<VTermCell> alt_;
    bool alt_active_ = false;
    bool app_cursor_keys_ = false;
    std::deque<std::vector<VTermCell>> scrollback_;
    static constexpr size_t kMaxScrollback = 5000;

    int cursor_row_ = 0, cursor_col_ = 0;
    bool cursor_visible_ = true;
    // Deferred autowrap: set once a character is written to the last
    // column rather than advancing past it immediately, so a line that
    // exactly fills the width doesn't scroll before the *next* character
    // proves there really is one -- matches real terminal behavior.
    bool pending_wrap_ = false;
    int saved_cursor_row_ = 0, saved_cursor_col_ = 0;
    int top_margin_ = 0, bottom_margin_ = 0;  // inclusive, 0-indexed scroll region

    // Pending SGR state applied to the next cell written.
    VTermColor pen_fg_, pen_bg_;
    bool pen_bold_ = false, pen_faint_ = false, pen_italic_ = false, pen_underline_ = false, pen_reverse_ = false;

    std::string title_;

    // Parser state.
    ParseState state_ = ParseState::Ground;
    std::string csi_buf_;       // accumulated bytes since CSI (ESC [) until a final byte
    std::string osc_buf_;       // accumulated bytes since OSC (ESC ]) until BEL/ST
    bool is_osc_ = false;       // StringTerm payload is OSC (title-relevant) vs. DCS/PM/APC (fully discarded)
    std::string utf8_pending_;  // partial multi-byte UTF-8 sequence
    int utf8_remaining_ = 0;

    std::vector<VTermCell> &Grid() { return alt_active_ ? alt_ : primary_; }
    const std::vector<VTermCell> &Grid() const { return alt_active_ ? alt_ : primary_; }
    VTermCell &CellAt(int row, int col) { return Grid()[static_cast<size_t>(row) * cols_ + col]; }

    void PutByte(unsigned char c);
    void PutChar(const std::string &utf8_char);
    void HandleControl(unsigned char c);
    void HandleEscapeByte(unsigned char c);
    void HandleCsiByte(unsigned char c);
    void HandleOscByte(unsigned char c);
    void ExecuteCsi(char final_byte);
    void ExecuteSgr(const std::vector<int> &params);
    void ExecuteEscFinal(unsigned char c);
    void ParseOscTitle();

    void ClampCursor();
    void NewlineAtCursor();  // LF semantics: down one row, scrolling at bottom margin
    // Moves lines toward the top margin, pushing top lines off (into
    // scrollback when push_scrollback and the region's top is row 0 of
    // the primary screen -- IL/DL reuse this with push_scrollback=false
    // since they shift within an arbitrary cursor-relative sub-region,
    // not a real top-of-screen scroll).
    void ScrollRegionUp(int n, bool push_scrollback = true);
    void ScrollRegionDown(int n);
    void EraseInDisplay(int mode);
    void EraseInLine(int mode);
    void InsertChars(int n);
    void DeleteChars(int n);
    void EraseChars(int n);
    void EnterAltScreen();
    void LeaveAltScreen();
    std::vector<int> ParseCsiParams(const std::string &body) const;
};

#endif  // MEP_VTERM_H
