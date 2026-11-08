#pragma once

// mep's own Rust formatter: the builtin behind `gf` in a Rust buffer
// (mep.format_rust), and textDocument/formatting in mep-rust-lsp.
//
// A structure-for-structure port of rustfmt 1.97.1 (rust-lang/rust
// src/tools/rustfmt, MIT OR Apache-2.0) over mep's rustc_parse port
// (rust_parse.h), default configuration: max_width 100, 4-space indent,
// style_edition from the edition (2015/2018/2021 -> style 2015, 2024 -> 2024).
// The result is checked before it is returned: the output is re-lexed and
// its tokens compared with the input's (rust_format_check.cpp), allowing only
// the token changes rustfmt makes; anything else is refused with a reason.
// Checked byte for byte against rustfmt with `just format-compare rust`.

#include <string>
#include <string_view>

namespace rustfmt {

struct Options {
    int width = 100;
    int indent = 4;
    /// Rust edition: 2015, 2018, 2021 or 2024 (keywords, and rustfmt's style edition).
    int edition = 2021;
    /// Skip the token self-check (for measuring the formatter alone).
    bool no_check = false;
};

struct Result {
    bool ok = false;
    std::string text;
    std::string error;
    int error_line = 0;  // 1-based; 0 when not tied to a line
};

/** @brief Formats Rust source; on failure `ok` is false and `text` is the input unchanged. */
Result Format(std::string_view src, const Options &opts = {});

/** @brief The edition of the crate holding `path`: a compiletest `//@ edition:` header in
 *  `head` (the file's start), else the nearest Cargo.toml's `edition` (following
 *  `edition.workspace = true`), else 2015 for a package that names none, else 2021. */
int DetectEdition(std::string_view path, std::string_view head);

}  // namespace rustfmt
