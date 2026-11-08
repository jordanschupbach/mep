#pragma once

// Copyright 2016 Google Inc., Apache License 2.0, ported from google-java-format 1.36.1
// (core/src/main/java/com/google/googlejavaformat/java/javadoc/).
//
// GJF's javadoc formatter: re-wraps and re-tags a `/** ... */` comment the way
// JavadocFormatter.formatJavadoc does, byte for byte. Part of javafmt's GJF port.

#include <string>
#include <string_view>

namespace javafmt::gjf {

// JavadocFormatter.formatJavadoc(input, blockIndent).
//
// `input` is the whole `/** ... */` comment text (UTF-8); `block_indent` is the
// column the comment starts at. Line lengths are counted in UTF-16 code units,
// as Java's String.length() does. Where GJF gives up (unbalanced tags, which
// GJF signals with LexException) the input is returned unchanged; the same is
// done for the inputs on which GJF itself would throw (not starting with
// `/**`, too short), so this never throws.
std::string FormatJavadoc(std::string_view input, int block_indent);

}  // namespace javafmt::gjf
