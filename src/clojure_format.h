#pragma once

// mep's own Clojure formatter: the builtin behind `gf` in a .clj/.cljs/
// .cljc/.edn/.bb buffer (mep.format_clojure) and behind mep-clojure-lsp's
// textDocument/formatting. No cljfmt, no JVM.
//
// The layout is cljfmt's, with its default options -- what a formatted
// Clojure file is expected to look like:
//
//   - indentation recomputed from the forms: a call's arguments aligned
//     under its first argument (or one column in when none is on the
//     head's line), a body two columns in for the forms cljfmt's table
//     names (`defn`, `let`, `when`, `fn`, `->`... as `:inner`/`:block`
//     rules, plus any `def...` or `with-...` head), collections' elements
//     under their first; top-level forms at column 0;
//   - no whitespace just inside brackets -- `( foo )` is `(foo)`, and a
//     closing bracket left alone on a line joins the line before it;
//   - a space between forms that touch (`(foo(bar))`);
//   - no trailing whitespace, at most one blank line in a row.
//
// Like cljfmt it never breaks or joins lines otherwise, never touches a
// string's or a comment's text, and leaves a line that starts with a
// comment where it is.
//
// It never returns text it has not checked: the result is read again and
// its tokens compared with the input's; a file that does not read, or a
// result that differs anywhere but in whitespace, is left alone.

#include <string>
#include <string_view>

namespace cljfmt {

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int error_line = 0;  // 1-based; 0 when not tied to a line
};

/** @brief Formats Clojure source; on failure `ok` is false and `text` is the input unchanged. */
Result Format(std::string_view src);

}  // namespace cljfmt
