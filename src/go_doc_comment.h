#pragma once

// A port of the parts of Go's go/doc/comment (Go 1.26) that gofmt uses:
// Parser.Parse with the default Parser (no Words, LookupPackage or
// LookupSym) and Printer.Comment, which go/printer calls to reformat doc
// comments (go/printer/comment.go). The document model is collapsed to
// what Printer.Comment emits -- a link and a doc link print as their
// bracketed text -- but every parsing decision that changes the output
// (spans, lists, code blocks, headings, link definitions and which are
// used, `` and '' quotes, URLs and identifiers skipped by them) is Go's.
//
// Copyright 2022 The Go Authors. All rights reserved. BSD-style license;
// the full notice is in go_parse.cpp.

#include <string>
#include <string_view>

namespace godoc {

/** @brief comment.Printer{}.Comment((&comment.Parser{}).Parse(text)). */
std::string ReformatComment(std::string_view text);

/** @brief strings.TrimSpace (Unicode white space). */
std::string_view TrimSpace(std::string_view s);

}  // namespace godoc
