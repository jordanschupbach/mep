#pragma once

// A port of Go's text/tabwriter (Go 1.26), the column aligner go/printer
// writes through. Output accumulates in a std::string instead of an
// io.Writer. Copyright 2009 The Go Authors. All rights reserved.
// BSD-style license; the full notice is in go_parse.cpp.

#include <string>
#include <string_view>
#include <vector>

namespace gotab {

enum : unsigned {
    FilterHTML = 1u << 0,
    StripEscape = 1u << 1,
    AlignRight = 1u << 2,
    DiscardEmptyColumns = 1u << 3,
    TabIndent = 1u << 4,
    Debug = 1u << 5,
};

constexpr char Escape = '\xff';

class Writer {
public:
    Writer(std::string *out, int minwidth, int tabwidth, int padding, char padchar, unsigned flags);
    void Write(std::string_view buf);
    void Flush();

private:
    struct Cell {
        int size = 0;
        int width = 0;
        bool htab = false;
    };
    void AddLine(bool flushed);
    void Reset();
    void WritePadding(int textw, int cellw, bool use_tabs);
    int WriteLines(int pos0, int line0, int line1);
    int Format(int pos0, int line0, int line1);
    void Append(std::string_view text);
    void UpdateWidth();
    void StartEscape(char ch);
    void EndEscape();
    int TerminateCell(bool htab);
    void FlushNoDefers();

    std::string *out_;
    int minwidth_, tabwidth_, padding_;
    char padchar_;
    unsigned flags_;
    std::string buf_;
    size_t pos_ = 0;
    Cell cell_;
    char end_char_ = 0;
    std::vector<std::vector<Cell>> lines_;
    std::vector<int> widths_;
};

}  // namespace gotab
