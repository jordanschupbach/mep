// Standard Common Lisp indentation over plain lines: the entry points the
// editor and the language server use, over lisp_format.cpp's port of
// Emacs's rules (see lisp_indent.h).

#include "lisp_indent.h"

#include <cstddef>
#include <string>
#include <vector>

#include "lisp_format.h"

namespace {

/** @brief Counts a line's leading blanks. */
int LeadingBlanks(const std::string &s) {
    int n = 0;
    while (static_cast<size_t>(n) < s.size() && (s[static_cast<size_t>(n)] == ' ' || s[static_cast<size_t>(n)] == '\t')) {
        n++;
    }
    return n;
}

}  // namespace

int LispIndentForLine(const std::vector<std::string> &lines, int line) {
    if (line <= 0) return 0;
    const size_t stop = static_cast<size_t>(line) < lines.size() ? static_cast<size_t>(line) : lines.size();
    std::string text;
    for (size_t i = 0; i < stop; i++) text += lines[i] + "\n";
    if (stop < lines.size()) text += lines[stop];
    const int col = lispfmt::IndentLine(text, static_cast<int>(stop));
    // Typing on in a docstring or a block comment: keep the previous line's margin.
    return col >= 0 ? col : LeadingBlanks(lines[stop - 1]);
}

std::vector<std::string> LispReindent(const std::vector<std::string> &lines) {
    std::string text;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i > 0) text += '\n';
        text += lines[i];
    }
    const std::string fixed = lispfmt::Reindent(text);
    std::vector<std::string> out;
    out.reserve(lines.size());
    size_t p = 0;
    for (;;) {
        const size_t nl = fixed.find('\n', p);
        if (nl == std::string::npos) {
            out.push_back(fixed.substr(p));
            break;
        }
        out.push_back(fixed.substr(p, nl - p));
        p = nl + 1;
    }
    out.resize(lines.size());
    return out;
}
