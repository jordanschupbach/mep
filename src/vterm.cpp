#include "vterm.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <utility>

namespace {

// Zero-width codepoints: format controls, variation selectors, and the
// combining-mark ranges a terminal realistically sees. A pragmatic subset
// of Unicode's full Mn/Me/Cf categories (~350 ranges) -- the long tail is
// scripts whose base letters mep can't shape anyway; an unlisted combining
// mark degrades to a spurious 1-column cell, same as before this table
// existed. Sorted by first codepoint (binary-searched below).
constexpr std::pair<uint32_t, uint32_t> kZeroWidthRanges[] = {
    {0x0300, 0x036F},    // combining diacritical marks
    {0x0483, 0x0489},    // Cyrillic combining
    {0x0591, 0x05BD},    // Hebrew points
    {0x05BF, 0x05BF},
    {0x05C1, 0x05C2},
    {0x0610, 0x061A},    // Arabic marks
    {0x064B, 0x065F},
    {0x0670, 0x0670},
    {0x06D6, 0x06DC},
    {0x0E31, 0x0E31},    // Thai vowel/tone marks
    {0x0E34, 0x0E3A},
    {0x0E47, 0x0E4E},
    {0x1AB0, 0x1AFF},    // combining diacritical marks extended
    {0x1DC0, 0x1DFF},    // combining diacritical marks supplement
    {0x200B, 0x200F},    // ZWSP/ZWNJ/ZWJ/LRM/RLM
    {0x20D0, 0x20FF},    // combining marks for symbols
    {0xFE00, 0xFE0F},    // variation selectors (VS1-16 -- emoji/text presentation)
    {0xFE20, 0xFE2F},    // combining half marks
    {0xFEFF, 0xFEFF},    // zero-width no-break space / BOM
    {0xE0100, 0xE01EF},  // variation selectors supplement
};

// Wide (2-column) codepoints: the standard compact East Asian Wide/
// Fullwidth set plus the emoji-presentation pictograph blocks -- the same
// widths starship/TUI libraries (unicode-width et al.) assume when they
// compute prompt layout, which is the whole point: the grid's column
// arithmetic has to agree with the child process's. Sorted by first
// codepoint (binary-searched below).
constexpr std::pair<uint32_t, uint32_t> kWideRanges[] = {
    {0x1100, 0x115F},    // Hangul Jamo leading consonants
    {0x231A, 0x231B},    // watch/hourglass
    {0x2329, 0x232A},    // angle brackets
    {0x23E9, 0x23EC},    // black double arrows
    {0x23F0, 0x23F0},
    {0x23F3, 0x23F3},
    {0x25FD, 0x25FE},    // small squares
    {0x2614, 0x2615},    // umbrella/hot beverage
    {0x2648, 0x2653},    // zodiac
    {0x267F, 0x267F},
    {0x2693, 0x2693},
    {0x26A1, 0x26A1},
    {0x26AA, 0x26AB},
    {0x26BD, 0x26BE},
    {0x26C4, 0x26C5},
    {0x26CE, 0x26CE},
    {0x26D4, 0x26D4},
    {0x26EA, 0x26EA},
    {0x26F2, 0x26F3},
    {0x26F5, 0x26F5},
    {0x26FA, 0x26FA},
    {0x26FD, 0x26FD},
    {0x2705, 0x2705},
    {0x270A, 0x270B},
    {0x2728, 0x2728},
    {0x274C, 0x274C},
    {0x274E, 0x274E},
    {0x2753, 0x2755},
    {0x2757, 0x2757},
    {0x2795, 0x2797},
    {0x27B0, 0x27B0},
    {0x27BF, 0x27BF},
    {0x2B1B, 0x2B1C},
    {0x2B50, 0x2B50},
    {0x2B55, 0x2B55},
    {0x2E80, 0x303E},    // CJK radicals .. CJK symbols/punctuation
    {0x3041, 0x33FF},    // Hiragana .. CJK compatibility
    {0x3400, 0x4DBF},    // CJK ext A
    {0x4E00, 0x9FFF},    // CJK unified
    {0xA000, 0xA4CF},    // Yi
    {0xA960, 0xA97F},    // Hangul Jamo extended-A
    {0xAC00, 0xD7A3},    // Hangul syllables
    {0xF900, 0xFAFF},    // CJK compatibility ideographs
    {0xFE10, 0xFE19},    // vertical forms
    {0xFE30, 0xFE6B},    // CJK compatibility forms + small forms
    {0xFF00, 0xFF60},    // fullwidth forms
    {0xFFE0, 0xFFE6},    // fullwidth signs
    {0x1F004, 0x1F004},  // mahjong red dragon
    {0x1F0CF, 0x1F0CF},  // playing card joker
    {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A},
    {0x1F200, 0x1F2FF},  // enclosed ideographic supplement
    {0x1F300, 0x1F64F},  // pictographs + emoticons
    {0x1F680, 0x1F6FF},  // transport pictographs
    {0x1F900, 0x1F9FF},  // supplemental pictographs
    {0x1FA70, 0x1FAFF},  // symbols and pictographs extended-A
    {0x20000, 0x3FFFD},  // CJK ext B..H
};

/**
 * @brief Binary-searches a sorted, non-overlapping codepoint range table for membership.
 * @param ranges Table sorted ascending by each range's first codepoint.
 * @param count Number of ranges in the table.
 * @param cp Codepoint to look up.
 * @return True if cp falls inside any range.
 */
bool InRangeTable(const std::pair<uint32_t, uint32_t> *ranges, size_t count, uint32_t cp) {
    const std::pair<uint32_t, uint32_t> *end = ranges + count;
    // First range whose start is > cp; the candidate is the one before it.
    const std::pair<uint32_t, uint32_t> *it =
        std::upper_bound(ranges, end, cp, [](uint32_t v, const std::pair<uint32_t, uint32_t> &r) { return v < r.first; });
    return it != ranges && cp <= (it - 1)->second;
}

/**
 * @brief Decodes the first codepoint of a (well-formed, PutByte-assembled) UTF-8 string.
 * @param s UTF-8 bytes; empty or malformed input decodes as U+FFFD.
 * @return The first codepoint.
 */
uint32_t DecodeUtf8First(const std::string &s) {
    if (s.empty()) return 0xFFFD;
    unsigned char b0 = static_cast<unsigned char>(s[0]);
    if (b0 < 0x80) return b0;
    int len = (b0 & 0xE0) == 0xC0 ? 2 : (b0 & 0xF0) == 0xE0 ? 3 : (b0 & 0xF8) == 0xF0 ? 4 : 0;
    if (len == 0 || static_cast<int>(s.size()) < len) return 0xFFFD;
    uint32_t cp = b0 & (0xFF >> (len + 1));
    for (int i = 1; i < len; i++) cp = (cp << 6) | (static_cast<unsigned char>(s[static_cast<size_t>(i)]) & 0x3F);
    return cp;
}

// OSC 10/11 represents each 8-bit channel as a four-hex-digit value.  A
// byte replicated into both octets preserves its exact intensity.
std::string OscRgb16(const VTermColor &c) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "rgb:%02x%02x/%02x%02x/%02x%02x", c.r, c.r, c.g, c.g, c.b, c.b);
    return buf;
}

}  // namespace

int VTermCharWidth(uint32_t cp) {
    if (InRangeTable(kZeroWidthRanges, std::size(kZeroWidthRanges), cp)) return 0;
    if (InRangeTable(kWideRanges, std::size(kWideRanges), cp)) return 2;
    return 1;
}

VTerm::VTerm(int rows, int cols) : rows_(std::max(1, rows)), cols_(std::max(1, cols)) {
    primary_.assign(static_cast<size_t>(rows_) * static_cast<size_t>(cols_), VTermCell{});
    alt_.assign(static_cast<size_t>(rows_) * static_cast<size_t>(cols_), VTermCell{});
    bottom_margin_ = rows_ - 1;
}

std::string VTerm::Feed(const std::string &data) {
    replies_.clear();
    for (char raw : data) PutByte(static_cast<unsigned char>(raw));
    return std::move(replies_);
}

void VTerm::SetOscDefaultColors(VTermColor foreground, VTermColor background) {
    osc_default_fg_ = foreground;
    osc_default_bg_ = background;
}

void VTerm::Resize(int rows, int cols) {
    rows = std::max(1, rows);
    cols = std::max(1, cols);
    if (rows == rows_ && cols == cols_) return;
    // Primary screen preserves as much existing content as fits,
    // top-left anchored; the alt screen is just reallocated blank (see
    // header comment -- a full-screen program redraws on SIGWINCH anyway).
    std::vector<VTermCell> new_primary(static_cast<size_t>(rows) * static_cast<size_t>(cols), VTermCell{});
    for (int r = 0; r < std::min(rows, rows_); r++) {
        for (int c = 0; c < std::min(cols, cols_); c++) {
            new_primary[static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(c)] =
                primary_[static_cast<size_t>(r) * static_cast<size_t>(cols_) + static_cast<size_t>(c)];
        }
    }
    // A wide pair cut at the new right edge leaves a lead with no room
    // for its continuation -- blank it rather than let it draw into the
    // (now nonexistent) next column.
    for (int r = 0; r < rows; r++) {
        VTermCell &last = new_primary[static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(cols - 1)];
        if (last.width == 2) last = VTermCell{};
    }
    primary_ = std::move(new_primary);
    alt_.assign(static_cast<size_t>(rows) * static_cast<size_t>(cols), VTermCell{});
    rows_ = rows;
    cols_ = cols;
    top_margin_ = 0;
    bottom_margin_ = rows_ - 1;
    cursor_row_ = std::min(cursor_row_, rows_ - 1);
    cursor_col_ = std::min(cursor_col_, cols_ - 1);
    pending_wrap_ = false;
}

const VTermCell &VTerm::At(int row, int col) const {
    static const VTermCell kBlank{};
    if (row < 0 || row >= rows_ || col < 0 || col >= cols_) return kBlank;
    return Grid()[static_cast<size_t>(row) * static_cast<size_t>(cols_) + static_cast<size_t>(col)];
}

const VTermCell &VTerm::ScrollbackAt(int row, int col) const {
    static const VTermCell kBlank{};
    if (row < 0 || row >= static_cast<int>(scrollback_.size())) return kBlank;
    const std::vector<VTermCell> &line = scrollback_[static_cast<size_t>(row)];
    if (col < 0 || col >= static_cast<int>(line.size())) return kBlank;
    return line[static_cast<size_t>(col)];
}

// --- Byte-level parsing ------------------------------------------------

void VTerm::PutByte(unsigned char c) {
    if (state_ == ParseState::Utf8Cont) {
        if ((c & 0xC0) == 0x80) {
            utf8_pending_ += static_cast<char>(c);
            if (--utf8_remaining_ == 0) {
                PutChar(utf8_pending_);
                utf8_pending_.clear();
                state_ = ParseState::Ground;
            }
            return;
        }
        // Malformed continuation -- abandon the partial sequence and
        // reprocess this byte fresh rather than losing it.
        state_ = ParseState::Ground;
        utf8_pending_.clear();
    }

    switch (state_) {
        case ParseState::Ground:
            if (c == 0x1B) {
                state_ = ParseState::Escape;
                return;
            }
            if (c < 0x20 || c == 0x7F) {
                HandleControl(c);
                return;
            }
            if (c < 0x80) {
                PutChar(std::string(1, static_cast<char>(c)));
                return;
            }
            if ((c & 0xE0) == 0xC0) {
                utf8_pending_.assign(1, static_cast<char>(c));
                utf8_remaining_ = 1;
                state_ = ParseState::Utf8Cont;
                return;
            }
            if ((c & 0xF0) == 0xE0) {
                utf8_pending_.assign(1, static_cast<char>(c));
                utf8_remaining_ = 2;
                state_ = ParseState::Utf8Cont;
                return;
            }
            if ((c & 0xF8) == 0xF0) {
                utf8_pending_.assign(1, static_cast<char>(c));
                utf8_remaining_ = 3;
                state_ = ParseState::Utf8Cont;
                return;
            }
            // Invalid lead byte -- print it verbatim, best effort.
            PutChar(std::string(1, static_cast<char>(c)));
            return;
        case ParseState::Escape:
            HandleEscapeByte(c);
            return;
        case ParseState::CsiEntry:
            HandleCsiByte(c);
            return;
        case ParseState::StringTerm:
            HandleOscByte(c);
            return;
        case ParseState::StringTermEsc:
            if (c == '\\') {
                if (is_osc_) ParseOscTitle();
                state_ = ParseState::Ground;
            } else {
                // Not a real ST -- bail out defensively and reprocess this
                // byte fresh (single level of recursion: Ground can't
                // re-enter this branch from within itself).
                state_ = ParseState::Ground;
                PutByte(c);
            }
            return;
        case ParseState::CharsetDesignate:
            state_ = ParseState::Ground;  // designator byte consumed and ignored
            return;
        case ParseState::Utf8Cont:
            return;  // unreachable (handled above)
    }
}

void VTerm::PutChar(const std::string &utf8_char) {
    uint32_t cp = DecodeUtf8First(utf8_char);
    int width = VTermCharWidth(cp);
    // Zero-width characters (variation selectors, ZWJ, combining marks)
    // are DROPPED, not merged into the previous cell's string: the
    // renderer draws every codepoint in a cell's ch through the font's
    // cmap with no shaping, so a merged U+FE0F would draw as the
    // missing-glyph '?' *after* the base glyph -- strictly worse than
    // dropping. (If a color-emoji/shaping tier ever lands, the upgrade
    // path is merging here plus a renderer that consumes only the first
    // codepoint.) Cost of dropping: a decomposed accent (e + U+0301)
    // degrades to the bare base letter.
    if (width == 0) return;
    if (pending_wrap_) {
        cursor_col_ = 0;
        NewlineAtCursor();
        pending_wrap_ = false;
    }
    if (width == 2 && cols_ < 2) width = 1;  // degenerate 1-column grid: clamp rather than wrap forever
    if (width == 2 && cursor_col_ == cols_ - 1) {
        // A wide glyph doesn't fit in the last column: xterm blanks the
        // orphan column (with the pen's background) and writes the glyph
        // at the start of the next line.
        ClearWideOverwrite(cursor_row_, cursor_col_);
        VTermCell &orphan = CellAt(cursor_row_, cursor_col_);
        orphan = VTermCell{};
        orphan.bg = pen_bg_;
        cursor_col_ = 0;
        NewlineAtCursor();
    }
    ClearWideOverwrite(cursor_row_, cursor_col_);
    if (width == 2) ClearWideOverwrite(cursor_row_, cursor_col_ + 1);
    VTermCell &cell = CellAt(cursor_row_, cursor_col_);
    cell.ch = utf8_char;
    cell.fg = pen_fg_;
    cell.bg = pen_bg_;
    cell.bold = pen_bold_;
    cell.faint = pen_faint_;
    cell.italic = pen_italic_;
    cell.underline = pen_underline_;
    cell.reverse = pen_reverse_;
    cell.width = static_cast<uint8_t>(width);
    if (width == 2) {
        // Continuation cell: empty ch, width 0, same pen attrs as the
        // lead so background/underline stay consistent if drawn, and so
        // a plain-text snapshot (EnterTerminalNormalMode) degrades it to
        // a space that keeps columns aligned.
        VTermCell &cont = CellAt(cursor_row_, cursor_col_ + 1);
        cont = cell;
        cont.ch.clear();
        cont.width = 0;
    }
    if (cursor_col_ + width < cols_) {
        cursor_col_ += width;
    } else {
        cursor_col_ = cols_ - 1;
        pending_wrap_ = true;
    }
}

void VTerm::ClearWideOverwrite(int row, int col) {
    if (row < 0 || row >= rows_ || col < 0 || col >= cols_) return;
    VTermCell &cell = CellAt(row, col);
    if (cell.width == 0 && col > 0) {
        VTermCell &lead = CellAt(row, col - 1);
        if (lead.width == 2) lead = VTermCell{};
    } else if (cell.width == 2 && col + 1 < cols_) {
        VTermCell &cont = CellAt(row, col + 1);
        if (cont.width == 0) cont = VTermCell{};
    }
}

void VTerm::HandleControl(unsigned char c) {
    switch (c) {
        case 0x07:  // BEL
            break;
        case 0x08:  // BS
            cursor_col_ = std::max(0, cursor_col_ - 1);
            pending_wrap_ = false;
            break;
        case 0x09: {  // TAB
            int next = (cursor_col_ / 8 + 1) * 8;
            cursor_col_ = std::min(cols_ - 1, next);
            break;
        }
        case 0x0A:
        case 0x0B:
        case 0x0C:  // LF/VT/FF
            NewlineAtCursor();
            pending_wrap_ = false;
            break;
        case 0x0D:  // CR
            cursor_col_ = 0;
            pending_wrap_ = false;
            break;
        default:
            break;  // other C0 controls (SO/SI, ...) ignored
    }
}

void VTerm::HandleEscapeByte(unsigned char c) {
    if (c == '[') {
        state_ = ParseState::CsiEntry;
        csi_buf_.clear();
        return;
    }
    if (c == ']') {
        state_ = ParseState::StringTerm;
        is_osc_ = true;
        osc_buf_.clear();
        return;
    }
    if (c == 'P' || c == 'X' || c == '^' || c == '_') {  // DCS/SOS/PM/APC -- fully discarded
        state_ = ParseState::StringTerm;
        is_osc_ = false;
        return;
    }
    if (c == '(' || c == ')' || c == '*' || c == '+') {  // charset designation, next byte ignored
        state_ = ParseState::CharsetDesignate;
        return;
    }
    ExecuteEscFinal(c);
    state_ = ParseState::Ground;
}

void VTerm::HandleCsiByte(unsigned char c) {
    if (c >= 0x40 && c <= 0x7E) {
        ExecuteCsi(static_cast<char>(c));
        state_ = ParseState::Ground;
        return;
    }
    if (csi_buf_.size() < 256) csi_buf_ += static_cast<char>(c);  // safety cap against runaway input
}

void VTerm::HandleOscByte(unsigned char c) {
    if (c == 0x07) {
        if (is_osc_) ParseOscTitle();
        state_ = ParseState::Ground;
        return;
    }
    if (c == 0x1B) {
        state_ = ParseState::StringTermEsc;
        return;
    }
    if (is_osc_ && osc_buf_.size() < 1024) osc_buf_ += static_cast<char>(c);
}

void VTerm::ParseOscTitle() {
    size_t semi = osc_buf_.find(';');
    if (semi == std::string::npos) return;
    std::string num = osc_buf_.substr(0, semi);
    const std::string value = osc_buf_.substr(semi + 1);
    if (num == "0" || num == "1" || num == "2") {
        title_ = value;
    } else if (value == "?" && num == "10") {
        // OSC 10/11 color queries are used by modern TUIs (including
        // Codex) during startup to choose a legible prompt treatment.
        // Report the embedding terminal's configured defaults, not a
        // hard-coded dark-terminal pair.
        queried_osc_default_fg_ = true;
        Reply("\x1b]10;" + OscRgb16(osc_default_fg_) + "\x1b\\");
    } else if (value == "?" && num == "11") {
        queried_osc_default_bg_ = true;
        Reply("\x1b]11;" + OscRgb16(osc_default_bg_) + "\x1b\\");
    }
}

void VTerm::Reply(const std::string &bytes) {
    replies_ += bytes;
}

// --- CSI / SGR dispatch --------------------------------------------------

std::vector<int> VTerm::ParseCsiParams(const std::string &body) const {
    std::vector<int> out;
    size_t i = 0;
    while (i <= body.size()) {
        size_t semi = body.find(';', i);
        std::string field = body.substr(i, semi == std::string::npos ? std::string::npos : semi - i);
        int val = 0;
        bool any_digit = false;
        for (char ch : field) {
            if (ch >= '0' && ch <= '9') {
                val = val * 10 + (ch - '0');
                any_digit = true;
            } else {
                break;  // stray intermediate byte mixed into a param field -- stop reading digits
            }
        }
        out.push_back(any_digit ? val : 0);
        if (semi == std::string::npos) break;
        i = semi + 1;
    }
    return out;
}

void VTerm::ExecuteCsi(char final_byte) {
    bool priv = !csi_buf_.empty() && csi_buf_[0] == '?';
    std::string body = priv ? csi_buf_.substr(1) : csi_buf_;
    std::vector<int> p = ParseCsiParams(body);
    // 0/absent means "use default" for most ops (VT convention) -- ops
    // where 0 is itself meaningful (ED/EL mode, SGR codes) read p[i]
    // directly instead of through this.
    auto param = [&](size_t i, int def) { return (i < p.size() && p[i] != 0) ? p[i] : def; };

    switch (final_byte) {
        case 'n':
            // DSR 6 (cursor position report).  Codex probes this as part
            // of its terminal initialization and waits for the response
            // before drawing its fully styled input area.
            if (!priv && !p.empty() && p[0] == 6) {
                Reply("\x1b[" + std::to_string(cursor_row_ + 1) + ";" + std::to_string(cursor_col_ + 1) + "R");
            }
            break;
        case 'c':
            // A conservative xterm primary-device-attributes reply.  It
            // advertises only capabilities VTerm implements sufficiently
            // for normal TUI operation, rather than claiming a full xterm.
            if (!priv) Reply("\x1b[?62;4;6;22c");
            break;
        case 'u':
            // Kitty keyboard-protocol capability query (CSI ? u).  We do
            // not enable that input protocol, so report its baseline state
            // explicitly instead of leaving the querying TUI waiting.
            if (priv && p.size() == 1 && p[0] == 0) {
                Reply("\x1b[?0u");
            } else if (!priv) {
                cursor_row_ = saved_cursor_row_;
                cursor_col_ = saved_cursor_col_;
                pending_wrap_ = false;
            }
            break;
        case 'A':
            cursor_row_ = std::max(top_margin_, cursor_row_ - param(0, 1));
            break;
        case 'B':
            cursor_row_ = std::min(bottom_margin_, cursor_row_ + param(0, 1));
            break;
        case 'C':
            cursor_col_ = std::min(cols_ - 1, cursor_col_ + param(0, 1));
            break;
        case 'D':
            cursor_col_ = std::max(0, cursor_col_ - param(0, 1));
            break;
        case 'E':
            cursor_row_ = std::min(bottom_margin_, cursor_row_ + param(0, 1));
            cursor_col_ = 0;
            break;
        case 'F':
            cursor_row_ = std::max(top_margin_, cursor_row_ - param(0, 1));
            cursor_col_ = 0;
            break;
        case 'G':
            cursor_col_ = std::min(cols_ - 1, std::max(0, param(0, 1) - 1));
            break;
        case 'd':
            cursor_row_ = std::min(rows_ - 1, std::max(0, param(0, 1) - 1));
            break;
        case 'H':
        case 'f': {
            int row = param(0, 1);
            int col = p.size() > 1 ? param(1, 1) : 1;
            cursor_row_ = std::min(rows_ - 1, std::max(0, row - 1));
            cursor_col_ = std::min(cols_ - 1, std::max(0, col - 1));
            pending_wrap_ = false;
            break;
        }
        case 'J':
            EraseInDisplay(p.empty() ? 0 : p[0]);
            break;
        case 'K':
            EraseInLine(p.empty() ? 0 : p[0]);
            break;
        case 'L':
        case 'M': {
            // Insert/delete n lines at the cursor row, within the scroll
            // region -- reuse ScrollRegionDown/Up narrowed to
            // [cursor_row_, bottom_margin_] rather than duplicating the
            // shift logic. push_scrollback=false on the Up (delete) path:
            // this is a cursor-relative shift, not a real top-of-screen
            // scroll, even when cursor_row_ happens to be 0.
            int n = param(0, 1);
            int saved_top = top_margin_;
            top_margin_ = cursor_row_;
            if (final_byte == 'L') {
                ScrollRegionDown(n);
            } else {
                ScrollRegionUp(n, /*push_scrollback=*/false);
            }
            top_margin_ = saved_top;
            break;
        }
        case 'P':
            DeleteChars(param(0, 1));
            break;
        case '@':
            InsertChars(param(0, 1));
            break;
        case 'X':
            EraseChars(param(0, 1));
            break;
        case 'S':
            ScrollRegionUp(param(0, 1));
            break;
        case 'T':
            ScrollRegionDown(param(0, 1));
            break;
        case 'r': {
            int t = param(0, 1) - 1;
            int b = (p.size() > 1 ? param(1, rows_) : rows_) - 1;
            if (t < 0) t = 0;
            if (b >= rows_) b = rows_ - 1;
            if (t < b) {
                top_margin_ = t;
                bottom_margin_ = b;
            } else {
                top_margin_ = 0;
                bottom_margin_ = rows_ - 1;
            }
            cursor_row_ = top_margin_;
            cursor_col_ = 0;
            pending_wrap_ = false;
            break;
        }
        case 's':
            saved_cursor_row_ = cursor_row_;
            saved_cursor_col_ = cursor_col_;
            break;
        case 'm':
            ExecuteSgr(p);
            break;
        case 'h':
        case 'l': {
            bool set = (final_byte == 'h');
            if (priv) {
                for (int mode : p) {
                    if (mode == 25) {
                        cursor_visible_ = set;
                    } else if (mode == 1) {
                        app_cursor_keys_ = set;
                    } else if (mode == 1049 || mode == 47 || mode == 1047) {
                        if (set) EnterAltScreen();
                        else LeaveAltScreen();
                    } else if (mode == 1000) {
                        mouse_tracking_ = set ? VTermMouseTracking::Normal : VTermMouseTracking::Off;
                    } else if (mode == 1002) {
                        mouse_tracking_ = set ? VTermMouseTracking::ButtonEvent : VTermMouseTracking::Off;
                    } else if (mode == 1003) {
                        mouse_tracking_ = set ? VTermMouseTracking::AnyMotion : VTermMouseTracking::Off;
                    } else if (mode == 1006) {
                        mouse_sgr_ = set;
                    } else if (mode == 1015) {
                        mouse_urxvt_ = set;
                    } else if (mode == 2004) {
                        bracketed_paste_ = set;
                    }
                    // The tracking-level modes 1000/1002/1003 are mutually
                    // exclusive levels of one machine: resetting any of them
                    // turns tracking fully Off (an app disables the mouse by
                    // resetting whatever level it set). Other unrecognized
                    // private modes are structurally consumed and ignored.
                }
            }
            break;
        }
        default:
            break;  // unrecognized final byte -- structurally consumed, silently ignored
    }
    ClampCursor();
}

void VTerm::ExecuteSgr(const std::vector<int> &params) {
    if (params.empty()) {
        pen_fg_ = VTermColor{};
        pen_bg_ = VTermColor{};
        pen_bold_ = pen_faint_ = pen_italic_ = pen_underline_ = pen_reverse_ = false;
        return;
    }
    for (size_t i = 0; i < params.size(); i++) {
        int p = params[i];
        if (p == 0) {
            pen_fg_ = VTermColor{};
            pen_bg_ = VTermColor{};
            pen_bold_ = pen_faint_ = pen_italic_ = pen_underline_ = pen_reverse_ = false;
        } else if (p == 1) {
            pen_bold_ = true;
        } else if (p == 2) {
            pen_faint_ = true;
        } else if (p == 3) {
            pen_italic_ = true;
        } else if (p == 4) {
            pen_underline_ = true;
        } else if (p == 7) {
            pen_reverse_ = true;
        } else if (p == 22) {
            pen_bold_ = false;
            pen_faint_ = false;
        } else if (p == 23) {
            pen_italic_ = false;
        } else if (p == 24) {
            pen_underline_ = false;
        } else if (p == 27) {
            pen_reverse_ = false;
        } else if (p >= 30 && p <= 37) {
            pen_fg_ = VTermColor{VTermColorKind::Indexed, static_cast<uint8_t>(p - 30)};
        } else if (p == 38) {
            if (i + 2 < params.size() && params[i + 1] == 5) {
                pen_fg_ = VTermColor{VTermColorKind::Indexed, static_cast<uint8_t>(params[i + 2])};
                i += 2;
            } else if (i + 4 < params.size() && params[i + 1] == 2) {
                pen_fg_ = VTermColor{VTermColorKind::Rgb, 0, static_cast<uint8_t>(params[i + 2]),
                                      static_cast<uint8_t>(params[i + 3]), static_cast<uint8_t>(params[i + 4])};
                i += 4;
            }
        } else if (p == 39) {
            pen_fg_ = VTermColor{};
        } else if (p >= 40 && p <= 47) {
            pen_bg_ = VTermColor{VTermColorKind::Indexed, static_cast<uint8_t>(p - 40)};
        } else if (p == 48) {
            if (i + 2 < params.size() && params[i + 1] == 5) {
                pen_bg_ = VTermColor{VTermColorKind::Indexed, static_cast<uint8_t>(params[i + 2])};
                i += 2;
            } else if (i + 4 < params.size() && params[i + 1] == 2) {
                pen_bg_ = VTermColor{VTermColorKind::Rgb, 0, static_cast<uint8_t>(params[i + 2]),
                                      static_cast<uint8_t>(params[i + 3]), static_cast<uint8_t>(params[i + 4])};
                i += 4;
            }
        } else if (p == 49) {
            pen_bg_ = VTermColor{};
        } else if (p >= 90 && p <= 97) {
            pen_fg_ = VTermColor{VTermColorKind::Indexed, static_cast<uint8_t>(p - 90 + 8)};
        } else if (p >= 100 && p <= 107) {
            pen_bg_ = VTermColor{VTermColorKind::Indexed, static_cast<uint8_t>(p - 100 + 8)};
        }
        // Other SGR codes (5 blink, 9 strikethrough, ...) ignored.
    }
}

void VTerm::ExecuteEscFinal(unsigned char c) {
    switch (c) {
        case '7':  // DECSC
            saved_cursor_row_ = cursor_row_;
            saved_cursor_col_ = cursor_col_;
            break;
        case '8':  // DECRC
            cursor_row_ = saved_cursor_row_;
            cursor_col_ = saved_cursor_col_;
            pending_wrap_ = false;
            break;
        case 'c':  // RIS -- full reset
            for (VTermCell &cell : primary_) cell = VTermCell{};
            for (VTermCell &cell : alt_) cell = VTermCell{};
            alt_active_ = false;
            app_cursor_keys_ = false;
            mouse_tracking_ = VTermMouseTracking::Off;
            mouse_sgr_ = false;
            mouse_urxvt_ = false;
            bracketed_paste_ = false;
            cursor_row_ = 0;
            cursor_col_ = 0;
            pending_wrap_ = false;
            top_margin_ = 0;
            bottom_margin_ = rows_ - 1;
            pen_fg_ = VTermColor{};
            pen_bg_ = VTermColor{};
            pen_bold_ = pen_faint_ = pen_italic_ = pen_underline_ = pen_reverse_ = false;
            cursor_visible_ = true;
            break;
        case 'D':  // IND
            NewlineAtCursor();
            break;
        case 'M':  // RI (reverse index)
            if (cursor_row_ == top_margin_) ScrollRegionDown(1);
            else if (cursor_row_ > 0) cursor_row_--;
            break;
        case 'E':  // NEL
            cursor_col_ = 0;
            NewlineAtCursor();
            break;
        default:
            break;
    }
    ClampCursor();
}

// --- Grid manipulation ---------------------------------------------------

void VTerm::ClampCursor() {
    cursor_row_ = std::max(0, std::min(cursor_row_, rows_ - 1));
    cursor_col_ = std::max(0, std::min(cursor_col_, cols_ - 1));
}

void VTerm::NewlineAtCursor() {
    if (cursor_row_ == bottom_margin_) {
        ScrollRegionUp(1);
    } else if (cursor_row_ < rows_ - 1) {
        cursor_row_++;
    }
}

void VTerm::ScrollRegionUp(int n, bool push_scrollback) {
    if (n <= 0) return;
    int region_h = bottom_margin_ - top_margin_ + 1;
    n = std::min(n, region_h);
    if (push_scrollback && top_margin_ == 0 && !alt_active_) {
        for (int i = 0; i < n; i++) {
            std::vector<VTermCell> line(Grid().begin() + static_cast<ptrdiff_t>(i) * cols_,
                                         Grid().begin() + static_cast<ptrdiff_t>(i + 1) * cols_);
            scrollback_.push_back(std::move(line));
            if (scrollback_.size() > kMaxScrollback) scrollback_.pop_front();
        }
    }
    for (int r = top_margin_; r <= bottom_margin_ - n; r++) {
        for (int c = 0; c < cols_; c++) CellAt(r, c) = CellAt(r + n, c);
    }
    for (int r = bottom_margin_ - n + 1; r <= bottom_margin_; r++) {
        for (int c = 0; c < cols_; c++) CellAt(r, c) = VTermCell{};
    }
}

void VTerm::ScrollRegionDown(int n) {
    if (n <= 0) return;
    int region_h = bottom_margin_ - top_margin_ + 1;
    n = std::min(n, region_h);
    for (int r = bottom_margin_; r >= top_margin_ + n; r--) {
        for (int c = 0; c < cols_; c++) CellAt(r, c) = CellAt(r - n, c);
    }
    for (int r = top_margin_; r < top_margin_ + n; r++) {
        for (int c = 0; c < cols_; c++) CellAt(r, c) = VTermCell{};
    }
}

void VTerm::EraseInDisplay(int mode) {
    if (mode == 0) {
        EraseInLine(0);
        for (int r = cursor_row_ + 1; r < rows_; r++)
            for (int c = 0; c < cols_; c++) CellAt(r, c) = VTermCell{};
    } else if (mode == 1) {
        EraseInLine(1);
        for (int r = 0; r < cursor_row_; r++)
            for (int c = 0; c < cols_; c++) CellAt(r, c) = VTermCell{};
    } else {
        for (int r = 0; r < rows_; r++)
            for (int c = 0; c < cols_; c++) CellAt(r, c) = VTermCell{};
    }
}

void VTerm::EraseInLine(int mode) {
    int c0 = 0, c1 = cols_ - 1;
    if (mode == 0) c0 = cursor_col_;
    else if (mode == 1) c1 = cursor_col_;
    // Interior of the range resets widths wholesale; only the edges can
    // cut a wide pair in half and orphan the half outside the range.
    ClearWideOverwrite(cursor_row_, c0);
    ClearWideOverwrite(cursor_row_, c1);
    for (int c = c0; c <= c1; c++) CellAt(cursor_row_, c) = VTermCell{};
}

void VTerm::InsertChars(int n) {
    n = std::max(0, n);
    // A pair straddling the cursor loses its shifted half -- blank the
    // stranded lead before shifting. (A pair torn mid-row by the shift
    // itself can't happen: both halves move together.)
    ClearWideOverwrite(cursor_row_, cursor_col_);
    for (int c = cols_ - 1; c >= cursor_col_ + n; c--) CellAt(cursor_row_, c) = CellAt(cursor_row_, c - n);
    for (int c = cursor_col_; c < std::min(cols_, cursor_col_ + n); c++) CellAt(cursor_row_, c) = VTermCell{};
    // Post-shift edges: a continuation shifted to the seam whose lead was
    // torn off above, or a lead pushed into the last column whose
    // continuation fell off the row end.
    if (cursor_col_ + n < cols_ && CellAt(cursor_row_, cursor_col_ + n).width == 0)
        CellAt(cursor_row_, cursor_col_ + n) = VTermCell{};
    if (CellAt(cursor_row_, cols_ - 1).width == 2) CellAt(cursor_row_, cols_ - 1) = VTermCell{};
}

void VTerm::DeleteChars(int n) {
    n = std::max(0, n);
    // As in InsertChars: fix the pair straddling the cursor before the
    // shift, and afterwards a continuation pulled to the cursor position
    // whose lead was inside the deleted range.
    ClearWideOverwrite(cursor_row_, cursor_col_);
    for (int c = cursor_col_; c < cols_ - n; c++) CellAt(cursor_row_, c) = CellAt(cursor_row_, c + n);
    for (int c = std::max(cursor_col_, cols_ - n); c < cols_; c++) CellAt(cursor_row_, c) = VTermCell{};
    if (CellAt(cursor_row_, cursor_col_).width == 0) CellAt(cursor_row_, cursor_col_) = VTermCell{};
}

void VTerm::EraseChars(int n) {
    int c1 = std::min(cols_, cursor_col_ + n) - 1;
    if (c1 < cursor_col_) return;
    ClearWideOverwrite(cursor_row_, cursor_col_);
    ClearWideOverwrite(cursor_row_, c1);
    for (int c = cursor_col_; c <= c1; c++) CellAt(cursor_row_, c) = VTermCell{};
}

void VTerm::EnterAltScreen() {
    if (alt_active_) return;
    alt_active_ = true;
    for (VTermCell &cell : alt_) cell = VTermCell{};
    cursor_row_ = 0;
    cursor_col_ = 0;
    pending_wrap_ = false;
}

void VTerm::LeaveAltScreen() {
    if (!alt_active_) return;
    alt_active_ = false;
    ClampCursor();
    pending_wrap_ = false;
}
