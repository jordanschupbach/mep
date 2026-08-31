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
    /**
     * @brief Compares two colors for exact equality of kind and all channel/index fields.
     * @param o The color to compare against.
     * @return True if every field (kind, index, r, g, b) matches exactly.
     */
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
    /**
     * @brief Constructs a terminal emulator with the given grid size, allocating blank primary and alternate screens.
     * @param rows Number of visible rows (clamped to at least 1).
     * @param cols Number of visible columns (clamped to at least 1).
     */
    VTerm(int rows, int cols);

    // Feeds a raw chunk of the child process's output. Chunks may split a
    // UTF-8 sequence or an escape sequence at an arbitrary byte boundary
    // (PTY reads aren't message-framed) -- partial parser state carries
    // over to the next call.
    /**
     * @brief Parses a chunk of raw child-process output byte by byte, updating parser and grid state.
     * @param data Raw bytes to feed; may contain a partial UTF-8 or escape sequence continued from a previous call.
     */
    void Feed(const std::string &data);

    // Resizes the *visible* grid in place. The alternate screen is
    // reallocated blank (a full-screen program redraws on resize
    // regardless -- it gets a SIGWINCH and repaints), while the primary
    // screen preserves as much of its existing content as fits, top-left
    // anchored, matching how real terminals behave.
    /**
     * @brief Resizes the visible grid, preserving as much of the primary screen's content as fits and reallocating the alt screen blank.
     * @param rows New number of visible rows (clamped to at least 1).
     * @param cols New number of visible columns (clamped to at least 1).
     */
    void Resize(int rows, int cols);

    /**
     * @brief Returns the number of visible rows in the grid.
     * @return The current row count.
     */
    int Rows() const { return rows_; }
    /**
     * @brief Returns the number of visible columns in the grid.
     * @return The current column count.
     */
    int Cols() const { return cols_; }
    // Row 0 is the top of the *visible* screen -- scrollback (primary
    // screen only) is addressed separately below.
    /**
     * @brief Looks up a cell on the currently active (primary or alt) visible screen.
     * @param row Row index, 0 at the top of the visible screen.
     * @param col Column index, 0 at the left of the visible screen.
     * @return Reference to the cell at (row, col), or a shared blank cell if out of bounds.
     */
    const VTermCell &At(int row, int col) const;
    /**
     * @brief Returns the cursor's current row.
     * @return Zero-based row index of the cursor.
     */
    int CursorRow() const { return cursor_row_; }
    /**
     * @brief Returns the cursor's current column.
     * @return Zero-based column index of the cursor.
     */
    int CursorCol() const { return cursor_col_; }
    /**
     * @brief Returns whether the cursor should currently be drawn (DECTCEM).
     * @return True if the cursor is visible.
     */
    bool CursorVisible() const { return cursor_visible_; }
    /**
     * @brief Returns whether the alternate screen buffer is currently active.
     * @return True if a full-screen program has switched in the alt screen.
     */
    bool AltScreenActive() const { return alt_active_; }
    // DECCKM (CSI ?1h/?1l) -- whether the app has asked for "application
    // cursor keys" mode. The embedding UI's key encoder needs this to know
    // whether an arrow keypress should send CSI (`ESC [ A`) or SS3
    // (`ESC O A`) -- vim, among others, switches this on.
    /**
     * @brief Returns whether the app has enabled application cursor keys mode (DECCKM).
     * @return True if arrow keys should be encoded as SS3 sequences instead of CSI.
     */
    bool ApplicationCursorKeys() const { return app_cursor_keys_; }

    /**
     * @brief Returns the number of lines currently held in scrollback.
     * @return Count of scrollback lines.
     */
    int ScrollbackLines() const { return static_cast<int>(scrollback_.size()); }
    /**
     * @brief Looks up a cell from the primary screen's scrollback history.
     * @param row Scrollback line index, 0 being the oldest retained line.
     * @param col Column index within that line.
     * @return Reference to the cell at (row, col), or a shared blank cell if out of bounds.
     */
    const VTermCell &ScrollbackAt(int row, int col) const;

    // Window title set via an OSC 0/1/2 sequence (`ESC ] 0 ; ... BEL`), if
    // any -- surfaced so a terminal pane's header can show it, the same
    // way a real terminal emulator's tab/window title works.
    /**
     * @brief Returns the window title most recently set by an OSC 0/1/2 sequence.
     * @return The current title string, empty if none has been set.
     */
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

    /**
     * @brief Returns the currently active screen buffer (alt screen if active, otherwise primary).
     * @return Reference to the active cell vector.
     */
    std::vector<VTermCell> &Grid() { return alt_active_ ? alt_ : primary_; }
    /**
     * @brief Returns the currently active screen buffer (alt screen if active, otherwise primary).
     * @return Const reference to the active cell vector.
     */
    const std::vector<VTermCell> &Grid() const { return alt_active_ ? alt_ : primary_; }
    /**
     * @brief Returns the cell at the given position in the currently active screen buffer.
     * @param row Row index into the active grid.
     * @param col Column index into the active grid.
     * @return Reference to the cell at (row, col).
     */
    VTermCell &CellAt(int row, int col) { return Grid()[static_cast<size_t>(row) * cols_ + col]; }

    /**
     * @brief Feeds one raw byte through the parser state machine, handling UTF-8 continuation, control bytes, and escape/CSI/OSC sequences.
     * @param c The raw byte to process.
     */
    void PutByte(unsigned char c);
    /**
     * @brief Writes one decoded character into the cell under the cursor with the current pen attributes, then advances or defers wrap.
     * @param utf8_char The UTF-8 encoding of the character to write.
     */
    void PutChar(const std::string &utf8_char);
    /**
     * @brief Handles a C0 control byte (BEL, BS, TAB, LF/VT/FF, CR, or others which are ignored).
     * @param c The control byte to handle.
     */
    void HandleControl(unsigned char c);
    /**
     * @brief Handles the byte following an ESC, routing to CSI/OSC/DCS/charset-designation entry or executing a two-byte escape sequence.
     * @param c The byte immediately following ESC.
     */
    void HandleEscapeByte(unsigned char c);
    /**
     * @brief Accumulates or dispatches one byte of a CSI sequence, executing it once a final byte (0x40-0x7E) is seen.
     * @param c The next byte of the CSI sequence.
     */
    void HandleCsiByte(unsigned char c);
    /**
     * @brief Accumulates one byte of an OSC/DCS/PM/APC string payload, terminating on BEL or handing off to ESC-lookahead on ESC.
     * @param c The next byte of the string payload.
     */
    void HandleOscByte(unsigned char c);
    /**
     * @brief Executes a fully parsed CSI sequence (cursor motion, erase, scrolling, margins, SGR, mode set/reset, etc.) based on its final byte.
     * @param final_byte The CSI sequence's terminating byte, selecting which operation to perform.
     */
    void ExecuteCsi(char final_byte);
    /**
     * @brief Applies a list of SGR (Select Graphic Rendition) parameters to the pending pen state used for subsequently written cells.
     * @param params Parsed SGR parameter codes, processed in order (an empty list resets all attributes).
     */
    void ExecuteSgr(const std::vector<int> &params);
    /**
     * @brief Executes a two-byte escape sequence's final byte (DECSC/DECRC, full reset, index, reverse index, next line).
     * @param c The escape sequence's final byte.
     */
    void ExecuteEscFinal(unsigned char c);
    /**
     * @brief Parses the accumulated OSC payload and updates the window title if it is an OSC 0/1/2 (title-setting) sequence.
     */
    void ParseOscTitle();

    /**
     * @brief Clamps the cursor position to stay within the current grid bounds.
     */
    void ClampCursor();
    /**
     * @brief Moves the cursor down one row, scrolling the region up when already at the bottom margin.
     */
    void NewlineAtCursor();  // LF semantics: down one row, scrolling at bottom margin
    // Moves lines toward the top margin, pushing top lines off (into
    // scrollback when push_scrollback and the region's top is row 0 of
    // the primary screen -- IL/DL reuse this with push_scrollback=false
    // since they shift within an arbitrary cursor-relative sub-region,
    // not a real top-of-screen scroll).
    /**
     * @brief Shifts the scroll region's lines up by n, pushing the top lines off (optionally into scrollback) and blanking the exposed bottom lines.
     * @param n Number of lines to scroll.
     * @param push_scrollback Whether top lines that scroll off the primary screen's row 0 are saved into scrollback.
     */
    void ScrollRegionUp(int n, bool push_scrollback = true);
    /**
     * @brief Shifts the scroll region's lines down by n, discarding the bottom lines and blanking the exposed top lines.
     * @param n Number of lines to scroll.
     */
    void ScrollRegionDown(int n);
    /**
     * @brief Erases part or all of the visible screen (ED), per the given mode.
     * @param mode 0 erases from the cursor to the end of screen, 1 from the start to the cursor, 2 (or other) erases the whole screen.
     */
    void EraseInDisplay(int mode);
    /**
     * @brief Erases part or all of the cursor's current row (EL), per the given mode.
     * @param mode 0 erases from the cursor to end of line, 1 from start of line to the cursor, other erases the whole line.
     */
    void EraseInLine(int mode);
    /**
     * @brief Inserts n blank characters at the cursor, shifting existing characters on the current row right (ICH).
     * @param n Number of characters to insert.
     */
    void InsertChars(int n);
    /**
     * @brief Deletes n characters at the cursor, shifting the remainder of the current row left and blanking the vacated end (DCH).
     * @param n Number of characters to delete.
     */
    void DeleteChars(int n);
    /**
     * @brief Blanks n characters starting at the cursor on the current row without shifting the rest of the row (ECH).
     * @param n Number of characters to erase.
     */
    void EraseChars(int n);
    /**
     * @brief Switches to the alternate screen buffer, clearing it and homing the cursor. No-op if already active.
     */
    void EnterAltScreen();
    /**
     * @brief Switches back to the primary screen buffer and clamps the cursor to its bounds. No-op if not active.
     */
    void LeaveAltScreen();
    /**
     * @brief Splits a CSI sequence's parameter body on ';' into integer parameter values, treating a missing/non-digit field as 0.
     * @param body The CSI parameter bytes between the introducer (and optional '?') and the final byte.
     * @return Parsed parameter values, in order.
     */
    std::vector<int> ParseCsiParams(const std::string &body) const;
};

#endif  // MEP_VTERM_H
