#pragma once

// mep's own APL formatter (GNU APL and Dyalog APL): the builtin behind `gf`
// in an APL buffer (mep.format_apl), and textDocument/formatting in
// mep-apl-lsp. Conservative -- it only changes whitespace:
//
//   * the bodies of Dyalog control structures (:If ... :EndIf, :For, :While,
//     :Repeat, :Select, :Trap, :With, :Hold, :Section, :Property) and of
//     :Namespace / :Class scripts are indented `indent` columns past their
//     opening line, and their :Else / :Case / :End lines line up with it;
//   * a dfn whose braces span lines has its body indented `indent` columns
//     past the line holding its `{`, and a line starting with its `}` lines
//     up with that line;
//   * a tradfn's body lines up two columns past its ∇ (the header's text,
//     `∇ R←F Y`), and the closing ∇ with the opening one;
//   * no spaces around ← (`x←1`, `x+←1`), one space on each side of ⋄
//     (`a ⋄ b`), one space between code and a trailing ⍝ comment, and one
//     space after ⍝ when a comment's text starts right after it (`⍝text` ->
//     `⍝ text`; extra spaces after ⍝ are kept: they may be indented code in
//     the documentation);
//   * trailing whitespace is removed.
//
// Lines outside any of these structures keep their indentation, as do lines
// continuing a multi-line ( ) or [ ] (Dyalog array notation), )commands and
// ]user commands. The result is lexed again and must have the same tokens
// as the input (comments compared without the spaces after ⍝); otherwise,
// or when a string is unterminated or a structure is unbalanced, nothing is
// changed and an error says why. Formatting formatted text changes nothing.

#include <string>
#include <string_view>

namespace aplfmt {

struct Options {
    int width = 100;  // unused: no line is ever broken or joined
    int indent = 4;   // columns per control-structure / dfn level
};

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int error_line = 0;  // 1-based; 0 when not tied to a line
};

/** @brief Formats APL source; on failure `ok` is false and `text` is the input unchanged. */
Result Format(std::string_view src, const Options &opts = {});

}  // namespace aplfmt
