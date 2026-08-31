#include "vterm.h"

#include <algorithm>

VTerm::VTerm(int rows, int cols) : rows_(std::max(1, rows)), cols_(std::max(1, cols)) {
    primary_.assign(static_cast<size_t>(rows_) * static_cast<size_t>(cols_), VTermCell{});
    alt_.assign(static_cast<size_t>(rows_) * static_cast<size_t>(cols_), VTermCell{});
    bottom_margin_ = rows_ - 1;
}

void VTerm::Feed(const std::string &data) {
    for (char raw : data) PutByte(static_cast<unsigned char>(raw));
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
    if (pending_wrap_) {
        cursor_col_ = 0;
        NewlineAtCursor();
        pending_wrap_ = false;
    }
    VTermCell &cell = CellAt(cursor_row_, cursor_col_);
    cell.ch = utf8_char;
    cell.fg = pen_fg_;
    cell.bg = pen_bg_;
    cell.bold = pen_bold_;
    cell.faint = pen_faint_;
    cell.italic = pen_italic_;
    cell.underline = pen_underline_;
    cell.reverse = pen_reverse_;
    if (cursor_col_ + 1 < cols_) {
        cursor_col_++;
    } else {
        pending_wrap_ = true;
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
    if (num == "0" || num == "1" || num == "2") title_ = osc_buf_.substr(semi + 1);
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
        case 'u':
            cursor_row_ = saved_cursor_row_;
            cursor_col_ = saved_cursor_col_;
            pending_wrap_ = false;
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
                    }
                    // Other private modes (mouse tracking, bracketed
                    // paste, ...) are structurally consumed and ignored.
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
    for (int c = c0; c <= c1; c++) CellAt(cursor_row_, c) = VTermCell{};
}

void VTerm::InsertChars(int n) {
    n = std::max(0, n);
    for (int c = cols_ - 1; c >= cursor_col_ + n; c--) CellAt(cursor_row_, c) = CellAt(cursor_row_, c - n);
    for (int c = cursor_col_; c < std::min(cols_, cursor_col_ + n); c++) CellAt(cursor_row_, c) = VTermCell{};
}

void VTerm::DeleteChars(int n) {
    n = std::max(0, n);
    for (int c = cursor_col_; c < cols_ - n; c++) CellAt(cursor_row_, c) = CellAt(cursor_row_, c + n);
    for (int c = std::max(cursor_col_, cols_ - n); c < cols_; c++) CellAt(cursor_row_, c) = VTermCell{};
}

void VTerm::EraseChars(int n) {
    for (int c = cursor_col_; c < std::min(cols_, cursor_col_ + n); c++) CellAt(cursor_row_, c) = VTermCell{};
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
