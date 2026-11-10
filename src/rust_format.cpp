// rust_format.cpp -- mep's Rust formatter entry point: rustfmt's lib.rs /
// formatting.rs (format_project / format_file / format_lines /
// format_snippet / format_code_block) over the module ports declared in
// rust_format_internal.h, then the token self-check (rust_format_check.cpp).
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
#include "rust_format.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include "rust_format_internal.h"

namespace rustfmt {

// rust_format_check.cpp
std::optional<std::pair<std::string, uint32_t>> CheckTokens(const rust::ParseResult& in, std::string_view out_text,
                                                            rust::Edition edition);

namespace {

rust::Edition EditionOf(int year) {
    switch (year) {
    case 2015: return rust::Edition::E2015;
    case 2018: return rust::Edition::E2018;
    case 2024: return rust::Edition::E2024;
    default: return rust::Edition::E2021;
    }
}

Config MakeConfig(const Options& opts) {
    Config c;
    c.max_width_ = opts.width > 0 ? static_cast<size_t>(opts.width) : 100;
    c.tab_spaces_ = opts.indent > 0 ? static_cast<size_t>(opts.indent) : 4;
    c.edition_ = EditionOf(opts.edition);
    c.style_edition_ = c.edition_ == rust::Edition::E2024 ? StyleEdition::E2024 : StyleEdition::E2015;
    return c;
}

/// rustc's normalize_src: drop a BOM, turn "\r\n" into "\n".
std::string NormalizeSource(std::string_view src) {
    if (src.substr(0, 3) == "\xEF\xBB\xBF") src.remove_prefix(3);
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); i++) {
        if (src[i] == '\r' && i + 1 < src.size() && src[i + 1] == '\n') continue;
        out += src[i];
    }
    return out;
}

/// formatting.rs FormatLines: strips nothing, but reports trailing
/// whitespace and truncates the trailing newlines to one.
void FormatLines(std::string& text, const std::vector<std::pair<size_t, size_t>>& skipped_range, const Config& config,
                 FormatState& state) {
    bool last_was_space = false;
    bool current_line_contains_string_literal = false;
    size_t cur_line = 1;
    size_t newline_count = 0;
    auto is_skipped_line = [&]() {
        for (const auto& r : skipped_range)
            if (r.first <= cur_line && cur_line <= r.second) return true;
        return false;
    };
    for (const ClassifiedChar& cc : char_classes(text)) {
        if (cc.ch == '\r') continue;
        if (cc.ch == '\n') {
            if (last_was_space) {
                // should_report_error(kind, TrailingWhitespace)
                bool allow = true;
                if (fcck_is_comment(cc.kind) || current_line_contains_string_literal)
                    allow = config.error_on_unformatted();
                if (allow && !is_skipped_line()) state.has_unformatted_code = true;
            }
            cur_line++;
            newline_count++;
            last_was_space = false;
            current_line_contains_string_literal = false;
        } else {
            newline_count = 0;
            last_was_space = cc.ch == ' ' || cc.ch == '\t' || (cc.ch >= 0x9 && cc.ch <= 0xD) || cc.ch == 0x85 ||
                             cc.ch == 0xA0 || cc.ch == 0x1680 || (cc.ch >= 0x2000 && cc.ch <= 0x200A) ||
                             cc.ch == 0x2028 || cc.ch == 0x2029 || cc.ch == 0x202F || cc.ch == 0x205F ||
                             cc.ch == 0x3000;
            if (fcck_is_string(cc.kind)) current_line_contains_string_literal = true;
        }
    }
    if (newline_count > 1) text.resize(text.size() - newline_count + 1);
}

struct FileResult {
    bool ok = false;
    std::string text;
    std::string error;
    uint32_t error_pos = 0;
    bool parse_error = false;
    std::vector<size_t> non_formatted_ranges;
};

/// format_project + format_file for one in-memory file (Input::Text).
FileResult FormatFile(std::unique_ptr<rust::ParseResult>& pr_out, const std::string& normalized, const Config& config,
                      bool is_macro_def, FormatState& state) {
    FileResult res;
    rust::ParseOptions po;
    po.edition = config.edition();
    pr_out = rust::parse_file(normalized, po);
    rust::ParseResult* pr = pr_out.get();
    if (!pr) {
        res.error = "the Rust parser is not available";
        res.parse_error = true;
        return res;
    }
    if (!pr->ok()) {
        res.parse_error = true;
        res.error = pr->diagnostics.empty() ? "parse error" : pr->diagnostics[0].message;
        res.error_pos = pr->diagnostics.empty() ? 0 : pr->diagnostics[0].span.lo;
        return res;
    }
    const std::string& src = *pr->source;
    SnippetProvider sp(src);
    // Stdin input with `#![rustfmt::skip]`: echo it back.
    {
        RewriteContext probe;
        probe.pr = pr;
        probe.config = &config;
        probe.snippet_provider = &sp;
        probe.state = &state;
        if (contains_skip(probe, pr->crate->attrs)) {
            res.ok = true;
            res.text = src;
            return res;
        }
    }
    FmtVisitor visitor = FmtVisitor::from_parse(pr, config, sp, state);
    visitor.skip_context.update_with_attrs(*pr, pr->crate->attrs);
    visitor.is_macro_def = is_macro_def;
    visitor.last_pos = sp.start_pos();
    visitor.skip_empty_lines(sp.end_pos());
    visitor.format_separate_mod(pr->crate, sp.end_pos());
    visitor.buffer += '\n';  // source_file::append_newline
    FormatLines(visitor.buffer, *visitor.skipped_range, config, state);
    // apply_newline_style(Auto): the snippet provider's text is normalised, so "\n".
    if (visitor.macro_rewrite_failure) state.has_macro_format_failure = true;
    for (const auto& r : *visitor.skipped_range) {
        res.non_formatted_ranges.push_back(r.first);
        res.non_formatted_ranges.push_back(r.second);
    }
    res.ok = true;
    res.text = std::move(visitor.buffer);
    return res;
}

uint32_t LineOf(std::string_view text, uint32_t pos) {
    if (pos > text.size()) pos = static_cast<uint32_t>(text.size());
    return 1 + static_cast<uint32_t>(std::count(text.begin(), text.begin() + pos, '\n'));
}

}  // namespace

std::optional<FormattedSnippet> format_snippet(std::string_view snippet, const Config& config_in, bool is_macro_def,
                                               FormatState& outer) {
    Config config = config_in;
    if (is_macro_def) config.error_on_unformatted_ = true;
    FormatState state;
    state.work = outer.work;
    state.work_budget = outer.work_budget;
    state.depth = outer.depth;
    state.max_depth = outer.max_depth;
    std::unique_ptr<rust::ParseResult> pr;
    FileResult r;
    try {
        r = FormatFile(pr, std::string(snippet), config, is_macro_def, state);
    } catch (const FormatPanic& p) {
        // catch_unwind: a panic while formatting the snippet discards it -- but
        // the depth cap and the work budget must still stop the outer format.
        outer.work = state.work;
        if (std::string_view(p.what()) == "formatter work budget exceeded") throw;
        return std::nullopt;
    }
    outer.work = state.work;
    bool formatting_error = state.has_macro_format_failure || (r.ok && r.text.empty() && !snippet.empty()) || !r.ok ||
                            (is_macro_def && state.has_unformatted_code);
    if (formatting_error) return std::nullopt;
    FormattedSnippet fs;
    fs.snippet = std::move(r.text);
    fs.non_formatted_ranges = std::move(r.non_formatted_ranges);
    return fs;
}

std::optional<FormattedSnippet> format_code_block(std::string_view code_snippet, const Config& config,
                                                  bool is_macro_def, FormatState& state) {
    const std::string_view FN_MAIN_PREFIX = "fn main() {\n";
    const Indent indent = Indent::from_width(config, config.tab_spaces());
    std::string snippet(FN_MAIN_PREFIX);
    bool need_indent = true;
    for (const auto& [kind, line] : line_classes(code_snippet)) {
        if (need_indent) snippet += indent.to_string(config);
        snippet += line;
        snippet += '\n';
        need_indent = indent_next_line(static_cast<uint8_t>(kind), line, config);
    }
    snippet += '}';

    auto formatted = format_snippet(snippet, config, is_macro_def, state);
    if (!formatted) return std::nullopt;
    // unwrap_code_block
    for (size_t& n : formatted->non_formatted_ranges) n -= 1;

    size_t block_len = formatted->snippet.rfind('}');
    if (block_len == std::string::npos) block_len = formatted->snippet.size();
    size_t block_start = std::min(FN_MAIN_PREFIX.size(), block_len);

    std::string result;
    bool is_first = true;
    bool is_indented = true;
    const std::string indent_str = indent.to_string(config);
    for (const auto& [kind, line] :
         line_classes(std::string_view(formatted->snippet).substr(block_start, block_len - block_start))) {
        if (!is_first) {
            result += '\n';
        } else {
            is_first = false;
        }
        std::string_view trimmed_line = line;
        if (is_indented) {
            if (line.size() > config.max_width()) return std::nullopt;
            if (line.size() > indent_str.size() && starts_with(line, indent_str))
                trimmed_line = std::string_view(line).substr(config.tab_spaces());
        }
        result += trimmed_line;
        is_indented = indent_next_line(static_cast<uint8_t>(kind), line, config);
    }
    FormattedSnippet fs;
    fs.snippet = std::move(result);
    fs.non_formatted_ranges = std::move(formatted->non_formatted_ranges);
    return fs;
}

Result Format(std::string_view src, const Options& opts) {
    Result res;
    res.text = std::string(src);
    const Config config = MakeConfig(opts);
    const std::string normalized = NormalizeSource(src);
    FormatState state;
    state.work_budget = 400 * static_cast<uint64_t>(normalized.size()) + 2000000;
    std::unique_ptr<rust::ParseResult> pr;
    FileResult fr;
    try {
        fr = FormatFile(pr, normalized, config, false, state);
    } catch (const FormatPanic& p) {
        res.error = std::string("formatter: ") + p.what();
        res.error_line = p.pos ? static_cast<int>(LineOf(normalized, p.pos)) : 0;
        return res;
    } catch (const std::exception& e) {
        res.error = std::string("formatter: internal error: ") + e.what();
        return res;
    }
    if (!fr.ok) {
        res.error = fr.error;
        res.error_line = fr.error_pos ? static_cast<int>(LineOf(normalized, fr.error_pos)) : 0;
        return res;
    }
    if (!opts.no_check) {
        if (auto bad = CheckTokens(*pr, fr.text, config.edition())) {
            res.error = "self-check: " + bad->first;
            res.error_line = static_cast<int>(LineOf(normalized, bad->second));
            return res;
        }
    }
    res.ok = true;
    res.text = std::move(fr.text);
    return res;
}

// ---------------------------------------------------------------------------
// Edition detection (tools/ts_grammar_diff/rust_check.py's edition()).

namespace {
std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string Dirname(const std::string& p) {
    size_t s = p.rfind('/');
    if (s == std::string::npos) return ".";
    if (s == 0) return "/";
    return p.substr(0, s);
}

int CargoEdition(std::string path) {
    static const std::regex kEditionKey(R"re((^|\n)\s*edition\s*=\s*"(\d{4})")re");
    static const std::regex kWorkspaceEdition(
        R"re((^|\n)\s*edition(\s*\.\s*workspace\s*=\s*true|\s*=\s*\{\s*workspace\s*=\s*true))re");
    if (path.empty() || path[0] != '/') {
        std::error_code ec;
        const std::filesystem::path cwd = std::filesystem::current_path(ec);
        if (!ec) path = cwd.generic_string() + "/" + path;
    }
    bool want_workspace = false;
    std::string d = Dirname(path);
    for (;;) {
        std::string text = ReadFile(d + "/Cargo.toml");
        if (!text.empty()) {
            std::smatch m;
            if (want_workspace) {
                if (text.find("[workspace") != std::string::npos) {
                    size_t wp = text.find("[workspace.package]");
                    if (wp != std::string::npos) {
                        std::string rest = text.substr(wp + 19);
                        if (std::regex_search(rest, m, kEditionKey)) return std::stoi(m[2].str());
                    }
                }
            } else if (std::regex_search(text, m, kEditionKey)) {
                return std::stoi(m[2].str());
            } else if (std::regex_search(text, kWorkspaceEdition)) {
                want_workspace = true;
            } else if (text.find("[package]") != std::string::npos) {
                return 2015;
            }
        }
        if (d == "/" || d.empty() || d == ".") return 0;
        d = Dirname(d);
    }
}
}  // namespace

int DetectEdition(std::string_view path, std::string_view head) {
    static const std::regex kHeader(R"re((^|\n)\s*//@\s*(compile-flags:.*--edition[= ]|edition:\s*)(\d{4}))re");
    std::string h(head.substr(0, 4000));
    std::smatch m;
    if (std::regex_search(h, m, kHeader)) return std::stoi(m[3].str());
    int e = CargoEdition(std::string(path));
    return e ? e : 2021;
}

}  // namespace rustfmt
