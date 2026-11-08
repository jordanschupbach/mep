// text/tabwriter port: see go_tabwriter.h.
#include "go_tabwriter.h"

#include "go_ast.h"

namespace gotab {

Writer::Writer(std::string *out, int minwidth, int tabwidth, int padding, char padchar, unsigned flags)
    : out_(out), minwidth_(minwidth), tabwidth_(tabwidth), padding_(padding), padchar_(padchar) {
    if (padchar == '\t') flags &= ~AlignRight;
    flags_ = flags;
    Reset();
}

void Writer::AddLine(bool) { lines_.emplace_back(); }

void Writer::Reset() {
    buf_.clear();
    pos_ = 0;
    cell_ = Cell{};
    end_char_ = 0;
    lines_.clear();
    widths_.clear();
    AddLine(true);
}

void Writer::WritePadding(int textw, int cellw, bool use_tabs) {
    if (padchar_ == '\t' || use_tabs) {
        if (tabwidth_ == 0) return;
        cellw = (cellw + tabwidth_ - 1) / tabwidth_ * tabwidth_;
        int n = cellw - textw;
        if (n < 0) n = 0;  // go panics ("internal error")
        out_->append(static_cast<size_t>((n + tabwidth_ - 1) / tabwidth_), '\t');
        return;
    }
    if (cellw - textw > 0) out_->append(static_cast<size_t>(cellw - textw), padchar_);
}

int Writer::WriteLines(int pos0, int line0, int line1) {
    int pos = pos0;
    for (int i = line0; i < line1; i++) {
        const auto &line = lines_[static_cast<size_t>(i)];
        bool use_tabs = (flags_ & TabIndent) != 0;
        for (size_t j = 0; j < line.size(); j++) {
            const Cell &c = line[j];
            if (j > 0 && (flags_ & Debug)) *out_ += '|';
            if (c.size == 0) {
                if (j < widths_.size()) WritePadding(c.width, widths_[j], use_tabs);
            } else {
                use_tabs = false;
                if (!(flags_ & AlignRight)) {
                    out_->append(buf_, static_cast<size_t>(pos), static_cast<size_t>(c.size));
                    pos += c.size;
                    if (j < widths_.size()) WritePadding(c.width, widths_[j], false);
                } else {
                    if (j < widths_.size()) WritePadding(c.width, widths_[j], false);
                    out_->append(buf_, static_cast<size_t>(pos), static_cast<size_t>(c.size));
                    pos += c.size;
                }
            }
        }
        if (static_cast<size_t>(i) + 1 == lines_.size()) {
            out_->append(buf_, static_cast<size_t>(pos), static_cast<size_t>(cell_.size));
            pos += cell_.size;
        } else {
            *out_ += '\n';
        }
    }
    return pos;
}

int Writer::Format(int pos0, int line0, int line1) {
    int pos = pos0;
    const size_t column = widths_.size();
    for (int th = line0; th < line1; th++) {
        if (column + 1 >= lines_[static_cast<size_t>(th)].size()) continue;
        pos = WriteLines(pos, line0, th);
        line0 = th;
        int width = minwidth_;
        bool discardable = true;
        for (; th < line1; th++) {
            const auto &line = lines_[static_cast<size_t>(th)];
            if (column + 1 >= line.size()) break;
            const Cell &c = line[column];
            if (int w = c.width + padding_; w > width) width = w;
            if (c.width > 0 || c.htab) discardable = false;
        }
        if (discardable && (flags_ & DiscardEmptyColumns)) width = 0;
        widths_.push_back(width);
        pos = Format(pos, line0, th);
        widths_.pop_back();
        line0 = th;
    }
    return WriteLines(pos, line0, line1);
}

void Writer::Append(std::string_view text) {
    buf_.append(text);
    cell_.size += static_cast<int>(text.size());
}

void Writer::UpdateWidth() {
    cell_.width += goast::RuneCount(std::string_view(buf_).substr(pos_));
    pos_ = buf_.size();
}

void Writer::StartEscape(char ch) {
    switch (ch) {
    case Escape: end_char_ = Escape; break;
    case '<': end_char_ = '>'; break;
    case '&': end_char_ = ';'; break;
    default: break;
    }
}

void Writer::EndEscape() {
    switch (end_char_) {
    case Escape:
        UpdateWidth();
        if (!(flags_ & StripEscape)) cell_.width -= 2;
        break;
    case ';': cell_.width++; break;
    default: break;
    }
    pos_ = buf_.size();
    end_char_ = 0;
}

int Writer::TerminateCell(bool htab) {
    cell_.htab = htab;
    auto &line = lines_.back();
    line.push_back(cell_);
    cell_ = Cell{};
    return static_cast<int>(line.size());
}

void Writer::FlushNoDefers() {
    if (cell_.size > 0) {
        if (end_char_ != 0) EndEscape();
        TerminateCell(false);
    }
    Format(0, 0, static_cast<int>(lines_.size()));
    Reset();
}

void Writer::Flush() { FlushNoDefers(); }

void Writer::Write(std::string_view buf) {
    size_t n = 0;
    for (size_t i = 0; i < buf.size(); i++) {
        const char ch = buf[i];
        if (end_char_ == 0) {
            switch (ch) {
            case '\t': case '\v': case '\n': case '\f': {
                Append(buf.substr(n, i - n));
                UpdateWidth();
                n = i + 1;
                const int ncells = TerminateCell(ch == '\t');
                if (ch == '\n' || ch == '\f') {
                    AddLine(ch == '\f');
                    if (ch == '\f' || ncells == 1) {
                        FlushNoDefers();
                        if (ch == '\f' && (flags_ & Debug)) *out_ += "---\n";
                    }
                }
                break;
            }
            case Escape:
                Append(buf.substr(n, i - n));
                UpdateWidth();
                n = i;
                if (flags_ & StripEscape) n++;
                StartEscape(Escape);
                break;
            case '<': case '&':
                if (flags_ & FilterHTML) {
                    Append(buf.substr(n, i - n));
                    UpdateWidth();
                    n = i;
                    StartEscape(ch);
                }
                break;
            default: break;
            }
        } else if (ch == end_char_) {
            size_t j = i + 1;
            if (ch == Escape && (flags_ & StripEscape)) j = i;
            Append(buf.substr(n, j - n));
            n = i + 1;
            EndEscape();
        }
    }
    Append(buf.substr(n));
}

}  // namespace gotab
