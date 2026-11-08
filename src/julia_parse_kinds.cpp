// Kinds, heads, tree helpers and source locations for julia_parse.h.
//
// Ported from JuliaSyntax.jl 0.4.10 src/kinds.jl, parse_stream.jl (untokenize
// of heads) and source_files.jl (MIT "Expat" licence, Copyright (c) 2021
// Julia Computing and contributors).

#include "julia_parse_internal.h"

#include <algorithm>
#include <string>
#include <unordered_map>

namespace jlparse {

Kind KindFromName(std::string_view name) {
    static const std::unordered_map<std::string_view, uint16_t> table = [] {
        std::unordered_map<std::string_view, uint16_t> t;
        for (size_t i = 0; i < kNumKinds; ++i) t.emplace(kKindNames[i], static_cast<uint16_t>(i));
        for (const KindMarker &m : kKindMarkers) t.emplace(m.name, m.value);
        return t;
    }();
    auto it = table.find(name);
    return it == table.end() ? K("None") : static_cast<Kind>(it->second);
}

std::string HeadString(SyntaxHead h, bool include_flag_suffixes) {
    // untokenize(head; unique=false, include_flag_suff)
    std::string str = KindName(h.kind);
    if (is_dotted(h.flags)) str = "." + str;
    if (include_flag_suffixes) {
        Kind k = h.kind;
        if (is_trivia(h.flags)) str += "-t";
        if (is_infix_op_call(h.flags)) str += "-i";
        if (is_prefix_op_call(h.flags)) str += "-pre";
        if (is_postfix_op_call(h.flags)) str += "-post";
        if (k == K("string") || k == K("cmdstring") || k == K("Identifier")) {
            if (has_flags(h.flags, TRIPLE_STRING_FLAG)) str += "-s";
            if (has_flags(h.flags, RAW_STRING_FLAG)) str += "-r";
        } else if (k == K("tuple") || k == K("block") || k == K("macrocall")) {
            if (has_flags(h.flags, PARENS_FLAG)) str += "-p";
        } else if (k == K("quote")) {
            if (has_flags(h.flags, COLON_QUOTE)) str += "-:";
        } else if (k == K("toplevel")) {
            if (has_flags(h.flags, TOPLEVEL_SEMICOLONS_FLAG)) str += "-;";
        } else if (k == K("struct")) {
            if (has_flags(h.flags, MUTABLE_FLAG)) str += "-mut";
        } else if (k == K("module")) {
            if (has_flags(h.flags, BARE_MODULE_FLAG)) str += "-bare";
        }
        if (is_suffixed(h.flags)) str += "-suf";
        int n = numeric_flags(h.flags);
        if (n != 0) str += "-" + std::to_string(n);
    }
    return str;
}

uint32_t Tree::NumChildren(uint32_t i) const {
    uint32_t n = 0;
    for (uint32_t c = i + 1; c < nodes[i].end; c = nodes[c].end) ++n;
    return n;
}

std::vector<uint32_t> Tree::Children(uint32_t i) const {
    std::vector<uint32_t> out;
    for (uint32_t c = i + 1; c < nodes[i].end; c = nodes[c].end) out.push_back(c);
    return out;
}

bool ParseResult::any_error() const {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic &d) { return d.level == Level::Error; });
}

SourceIndex::SourceIndex(std::string_view text) : text_(text) {
    // SourceFile(code): line starts after each '\n'
    line_starts_.push_back(1);
    size_t i = 0;
    while (i < text.size()) {
        size_t len = 0;
        JChar c = ReadCharStr(text, i, &len);
        if (c == C('\n')) line_starts_.push_back(static_cast<int64_t>(i) + 2);
        i += len;
    }
    if (text.empty() || text.back() != '\n') line_starts_.push_back(static_cast<int64_t>(text.size()) + 1);
}

LineCol SourceIndex::Location(int64_t byte) const {
    // _source_line_index: searchsortedlast(line_starts, byte)
    auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), byte);
    int64_t lineidx = static_cast<int64_t>(it - line_starts_.begin());  // 1-based index of last <= byte
    if (lineidx < 1) lineidx = 1;
    if (lineidx >= static_cast<int64_t>(line_starts_.size())) lineidx = static_cast<int64_t>(line_starts_.size()) - 1;
    if (lineidx < 1) lineidx = 1;
    int64_t i = line_starts_[static_cast<size_t>(lineidx - 1)];
    int column = 1;
    while (i < byte) {
        // nextind(code, i): skip to the next character start
        if (i - 1 >= static_cast<int64_t>(text_.size())) {
            i += 1;
        } else {
            size_t len = 0;
            ReadCharStr(text_, static_cast<size_t>(i - 1), &len);
            i += static_cast<int64_t>(len);
        }
        column += 1;
    }
    return {static_cast<int>(lineidx), column};
}

}  // namespace jlparse
