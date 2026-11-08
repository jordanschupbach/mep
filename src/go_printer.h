#pragma once

// mep's port of Go's go/printer (Go 1.26: printer.go, nodes.go,
// comment.go, gobuild.go), the half of gofmt that turns a go/ast tree
// back into text, plus go/ast's SortImports, which gofmt applies before
// printing, and the parts of go/build/constraint that the //go:build line
// fixer needs. The code follows the Go source function for function so
// that the output is gofmt's byte for byte; see go_format.cpp for the
// cmd/gofmt driver around it.
//
// Copyright 2009 The Go Authors. All rights reserved. BSD-style license;
// the full notice is in go_parse.cpp.

#include <string>
#include <unordered_map>
#include <vector>

#include "go_ast.h"

namespace goprint {

/** @brief go/token's Position for the single file: 1-based line and byte column; line 0 = invalid. */
struct Position {
    int offset = 0;
    int line = 0;
    int column = 0;
    bool IsValid() const { return line > 0; }
    bool operator==(const Position &o) const {
        return offset == o.offset && line == o.line && column == o.column;
    }
};

/** @brief go/token's File for one parsed file: the line table, which SortImports may edit (MergeLine). */
class LineTable {
public:
    LineTable(std::vector<int> line_starts, int size) : lines_(std::move(line_starts)), size_(size) {}
    /** @brief FileSet.PositionFor(p, false): invalid for NoPos or a Pos outside the file. */
    Position PositionFor(goast::Pos p) const;
    int Line(goast::Pos p) const { return PositionFor(p).line; }
    /** @brief token.File.LineStart. */
    goast::Pos LineStart(int line) const;
    int LineCount() const { return static_cast<int>(lines_.size()); }
    /** @brief token.File.MergeLine: joins line `line` with the next one. */
    void MergeLine(int line);

private:
    std::vector<int> lines_;
    int size_;
};

enum Mode : unsigned {
    RawFormat = 1u << 0,
    TabIndent = 1u << 1,
    UseSpaces = 1u << 2,
    SourcePos = 1u << 3,  // not supported (no //line directives are written)
    NormalizeNumbers = 1u << 30,
};

struct Config {
    unsigned mode = 0;
    int tabwidth = 8;
    int indent = 0;
};

/** @brief The Config cmd/gofmt prints with. */
inline Config GofmtConfig() { return Config{UseSpaces | TabIndent | NormalizeNumbers, 8, 0}; }

/**
 * @brief Config.Fprint of a whole file. Returns false (and sets *error) only
 * when the tree nests deeper than this printer is willing to recurse.
 */
bool Fprint(const Config &cfg, const LineTable &lines, goast::File *file, std::string *out, std::string *error);

/** @brief ast.SortImports(fset, f). */
void SortImports(LineTable &lines, goast::File *file);

}  // namespace goprint
