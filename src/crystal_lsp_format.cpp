// mep-crystal-lsp's textDocument/formatting (CrLspFormat in crystal_lsp.h):
// the same formatter `gf` runs (crystal_format.h), as one edit over the
// lines that changed.

#include "crystal_format.h"
#include "crystal_lsp.h"

std::vector<CrLspTextEdit> CrLspFormat(const std::vector<std::string> &lines) {
    std::vector<CrLspTextEdit> out;
    std::string text;
    for (size_t k = 0; k < lines.size(); k++) text += lines[k] + (k + 1 < lines.size() ? "\n" : "");
    const crfmt::Result r = crfmt::Format(text);
    if (!r.ok || r.text == text) return out;
    std::vector<std::string> got;
    std::string cur;
    for (char c : r.text) {
        if (c == '\n') {
            got.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    got.push_back(cur);
    size_t head = 0;
    while (head < lines.size() && head < got.size() && lines[head] == got[head]) head++;
    size_t tail = 0;
    while (tail < lines.size() - head && tail < got.size() - head &&
           lines[lines.size() - 1 - tail] == got[got.size() - 1 - tail]) {
        tail++;
    }
    std::string repl;
    for (size_t k = head; k < got.size() - tail; k++) repl += got[k] + "\n";
    CrLspTextEdit e;
    e.range.start_line = static_cast<int>(head);
    e.range.end_line = static_cast<int>(lines.size() - tail);
    e.new_text = repl;
    out.push_back(e);
    return out;
}
