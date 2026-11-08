// rust_format_lists.cpp -- rustfmt's lists.rs
// Ported from rustfmt (rust-lang/rust 1.97.1 src/tools/rustfmt, MIT OR Apache-2.0).
// See rust_format_internal.h for the conventions.
#include "rust_format_internal.h"

#include <algorithm>

namespace rustfmt {

namespace {

// rustc's Span::new swaps lo and hi when lo > hi (mk_sp in the header does not).
Span mk_sp_ordered(uint32_t lo, uint32_t hi) {
    if (lo > hi) std::swap(lo, hi);
    return Span(lo, hi);
}

bool opt_contains_nl(const Opt& s) { return s.has_value() && s->find('\n') != std::string::npos; }

// The length in bytes of the last UTF-8 character of `s` (non-empty).
size_t last_char_len_utf8(std::string_view s) {
    size_t i = s.size();
    while (i > 0) {
        --i;
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) != 0x80) break;
    }
    return s.size() - i;
}

// str::trim_matches(&[' ', '\t'])
std::string_view trim_space_tab(std::string_view s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) b++;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
    return s.substr(b, e - b);
}

// calculate_width: the count and total width of the list items.
std::pair<size_t, size_t> calculate_width(const ListItem* b, const ListItem* e) {
    size_t count = 0, total = 0;
    for (const ListItem* it = b; it != e; ++it) {
        count += 1;
        total += total_item_width(*it);
    }
    return {count, total};
}

size_t comment_len(const Opt& comment) {
    if (!comment) return 0;
    size_t text_len = trim(*comment).size();
    if (text_len > 0) {
        // We'll put " /*" before and " */" after inline comments.
        return text_len + 6;
    }
    return text_len;
}

size_t max_width_of_item_with_post_comment(const std::vector<ListItem>& items, size_t i, size_t overhead,
                                           size_t max_budget) {
    size_t max_width = 0;
    bool first = true;
    for (size_t k = i; k < items.size(); ++k) {
        const ListItem& item = items[k];
        size_t inner_item_width = unicode_str_width(item.inner_as_ref());
        if (!first && (item.is_different_group() || !item.post_comment.has_value() ||
                       inner_item_width + overhead > max_budget)) {
            return max_width;
        }
        if (max_width < inner_item_width) max_width = inner_item_width;
        if (item.new_lines) return max_width;
        first = false;
    }
    return max_width;
}

size_t post_comment_alignment(std::optional<size_t> item_max_width, size_t inner_item_width) {
    return sat_sub(item_max_width.value_or(0), inner_item_width);
}

}  // namespace

// definitive_tactic over a slice [b, e) (the vector form below wraps it).
DefinitiveListTactic definitive_tactic(const ListItem* b, const ListItem* e, ListTactic tactic, Separator sep,
                                             size_t width) {
    bool pre_line_comments = false;
    for (const ListItem* it = b; it != e; ++it) {
        if (it->has_single_line_comment()) {
            pre_line_comments = true;
            break;
        }
    }

    size_t limit = 0;
    if (pre_line_comments) return DefinitiveListTactic::Vertical();
    switch (tactic.kind) {
    case ListTacticKind::Horizontal: return DefinitiveListTactic::Horizontal();
    case ListTacticKind::Vertical: return DefinitiveListTactic::Vertical();
    case ListTacticKind::LimitedHorizontalVertical: limit = std::min(width, tactic.limit); break;
    case ListTacticKind::Mixed:
    case ListTacticKind::HorizontalVertical: limit = width; break;
    }

    auto [sep_count, total_width] = calculate_width(b, e);
    size_t total_sep_len = separator_len(sep) * sat_sub(sep_count, 1);
    size_t real_total = total_width + total_sep_len;

    bool any_multiline = false;
    for (const ListItem* it = b; it != e; ++it) {
        if (it->is_multiline()) {
            any_multiline = true;
            break;
        }
    }
    if (real_total <= limit && !any_multiline) return DefinitiveListTactic::Horizontal();
    if (tactic.kind == ListTacticKind::Mixed) return DefinitiveListTactic::Mixed();
    return DefinitiveListTactic::Vertical();
}

// ---------------------------------------------------------------------------
// config/lists.rs bits

// SeparatorPlace::from_tactic
SeparatorPlace separator_place_from_tactic(SeparatorPlace def, DefinitiveListTactic tactic, std::string_view sep) {
    if (tactic.kind == DefinitiveListTacticKind::Vertical) return def;
    if (sep == ",") return SeparatorPlace::Back;
    return def;
}

// ---------------------------------------------------------------------------
// ListFormatting / ListItem

bool ListFormatting::needs_trailing_separator() const {
    switch (trailing_separator_) {
    // We always put separator in front.
    case SeparatorTactic::Always: return true;
    case SeparatorTactic::Vertical: return tactic_ == DefinitiveListTactic::Vertical();
    case SeparatorTactic::Never:
        return tactic_ == DefinitiveListTactic::Vertical() && separator_place_ == SeparatorPlace::Front;
    }
    return false;
}

ListItem ListItem::from_item(RewriteResult item) {
    ListItem li;
    li.item = std::move(item);
    return li;
}

ListItem ListItem::from_str(std::string s) {
    ListItem li;
    li.item = RewriteResult(std::move(s));
    return li;
}

bool ListItem::is_different_group() const {
    return contains(inner_as_ref(), '\n') || pre_comment.has_value() || opt_contains_nl(post_comment);
}

bool ListItem::is_multiline() const {
    return contains(inner_as_ref(), '\n') || opt_contains_nl(pre_comment) || opt_contains_nl(post_comment);
}

bool ListItem::has_single_line_comment() const {
    return (pre_comment.has_value() && starts_with(trim_start(*pre_comment), "//")) ||
           (post_comment.has_value() && starts_with(trim_start(*post_comment), "//"));
}

bool ListItem::has_comment() const { return pre_comment.has_value() || post_comment.has_value(); }

// Returns `true` if the item causes something to be written.
bool ListItem::is_substantial() const {
    auto empty = [](const Opt& s) { return !(s.has_value() && !s->empty()); };
    auto empty_result = [](const RewriteResult& s) { return !(s.ok() && !s->empty()); };
    return !(empty(pre_comment) && empty_result(item) && empty(post_comment));
}

DefinitiveListTactic definitive_tactic(const std::vector<ListItem>& items, ListTactic tactic, Separator sep,
                                       size_t width) {
    return definitive_tactic(items.data(), items.data() + items.size(), tactic, sep, width);
}

// Format a list of commented items into a string.
RewriteResult write_list(const std::vector<ListItem>& items, const ListFormatting& formatting) {
    DefinitiveListTactic tactic = formatting.tactic_;
    size_t sep_len = formatting.separator_.size();

    // Now that we know how we will layout, we can decide for sure if there
    // will be a trailing separator.
    bool trailing_separator = formatting.needs_trailing_separator();
    std::string result;
    result.reserve(128);
    std::optional<size_t> item_max_width;
    SeparatorPlace sep_place =
        separator_place_from_tactic(formatting.separator_place_, tactic, formatting.separator_);
    bool prev_item_had_post_comment = false;
    bool prev_item_is_nested_import = false;

    size_t line_len = 0;
    const std::string indent_str = formatting.shape_.indent.to_string(*formatting.config_);
    for (size_t i = 0; i < items.size(); ++i) {
        const ListItem& item = items[i];
        if (!item.item.ok()) return Err(item.item.error());
        const std::string& inner_item = *item.item;
        bool first = i == 0;
        bool last = i + 1 == items.size();
        bool separate = false;
        switch (sep_place) {
        case SeparatorPlace::Front: separate = !first; break;
        case SeparatorPlace::Back: separate = !last || trailing_separator; break;
        }
        size_t item_sep_len = separate ? sep_len : 0;

        // Item string may be multi-line. Its length (used for block comment alignment)
        // should be only the length of the last line.
        std::string_view item_last_line;
        if (item.is_multiline()) {
            std::vector<std::string_view> ls = lines(inner_item);
            item_last_line = ls.empty() ? std::string_view() : ls.back();
        } else {
            item_last_line = inner_item;
        }
        size_t item_last_line_width = unicode_str_width(item_last_line) + item_sep_len;
        if (starts_with(item_last_line, indent_str)) item_last_line_width -= unicode_str_width(indent_str);

        if (!item.is_substantial()) continue;

        switch (tactic.kind) {
        case DefinitiveListTacticKind::Horizontal:
            if (!first) result.push_back(' ');
            break;
        case DefinitiveListTacticKind::SpecialMacro: {
            size_t num_args_before = tactic.special;
            if (i == 0) {
                // Nothing
            } else if (i < num_args_before) {
                result.push_back(' ');
            } else if (i <= num_args_before + 1) {
                result.push_back('\n');
                result += indent_str;
            } else {
                result.push_back(' ');
            }
            break;
        }
        case DefinitiveListTacticKind::Vertical:
            if (!first && !inner_item.empty() && !result.empty()) {
                result.push_back('\n');
                result += indent_str;
            }
            break;
        case DefinitiveListTacticKind::Mixed: {
            size_t total_width = total_item_width(item) + item_sep_len;

            // 1 is space between separator and item.
            if ((line_len > 0 && line_len + 1 + total_width > formatting.shape_.width) || prev_item_had_post_comment ||
                (formatting.nested_ && (prev_item_is_nested_import || (!first && contains(inner_item, "::"))))) {
                result.push_back('\n');
                result += indent_str;
                line_len = 0;
                if (formatting.ends_with_newline_) trailing_separator = true;
            } else if (line_len > 0) {
                result.push_back(' ');
                line_len += 1;
            }

            if (last && formatting.ends_with_newline_) {
                separate = formatting.trailing_separator_ != SeparatorTactic::Never;
            }

            line_len += total_width;
            break;
        }
        }

        // Pre-comments
        if (item.pre_comment.has_value()) {
            const std::string& comment_src = *item.pre_comment;
            // Block style in non-vertical mode.
            bool block_mode = tactic == DefinitiveListTactic::Horizontal();
            // Width restriction is only relevant in vertical mode.
            RewriteResult comment = rewrite_comment(comment_src, block_mode, formatting.shape_, *formatting.config_);
            if (!comment) return comment;
            result += *comment;

            if (!inner_item.empty()) {
                if (tactic.kind == DefinitiveListTacticKind::Vertical || tactic.kind == DefinitiveListTacticKind::Mixed ||
                    tactic.kind == DefinitiveListTacticKind::SpecialMacro) {
                    // We cannot keep pre-comments on the same line if the comment is normalized.
                    bool keep_comment = false;
                    if (formatting.config_->normalize_comments() ||
                        item.pre_comment_style == ListItemCommentStyle::DifferentLine) {
                        keep_comment = false;
                    } else {
                        // We will try to keep the comment on the same line with the item here.
                        // 1 = ` `
                        size_t total_width = total_item_width(item) + item_sep_len + 1;
                        keep_comment = total_width <= formatting.shape_.width;
                    }
                    if (keep_comment) {
                        result.push_back(' ');
                    } else {
                        result.push_back('\n');
                        result += indent_str;
                        // This is the width of the item (without comments).
                        line_len = item.item.ok() ? unicode_str_width(*item.item) : 0;
                    }
                } else {
                    result.push_back(' ');
                }
            }
            item_max_width = std::nullopt;
        }

        if (separate && sep_place == SeparatorPlace::Front && !first) {
            result += trim(formatting.separator_);
            result.push_back(' ');
        }
        result += inner_item;

        // Post-comments
        if (tactic == DefinitiveListTactic::Horizontal() && item.post_comment.has_value()) {
            const std::string& comment = *item.post_comment;
            RewriteResult formatted_comment = rewrite_comment(
                comment, true, Shape::legacy(formatting.shape_.width, Indent::empty()), *formatting.config_);
            if (!formatted_comment) return formatted_comment;

            result.push_back(' ');
            result += *formatted_comment;
        }

        if (separate && sep_place == SeparatorPlace::Back) result += formatting.separator_;

        if (tactic != DefinitiveListTactic::Horizontal() && item.post_comment.has_value()) {
            const std::string& comment = *item.post_comment;
            size_t overhead_outer = last_line_width(result) + first_line_width(trim(comment));

            auto rewrite_post_comment = [&](std::optional<size_t>& imw) -> RewriteResult {
                if (!imw.has_value() && !last && !contains(inner_item, '\n')) {
                    imw = max_width_of_item_with_post_comment(items, i, overhead_outer,
                                                              formatting.config_->max_width());
                }
                size_t overhead = 0;
                if (starts_with_newline(comment)) {
                    overhead = 0;
                } else if (imw.has_value()) {
                    overhead = *imw + 2;
                } else {
                    // 1 = space between item and comment.
                    overhead = item_last_line_width + 1;
                }
                size_t width = checked_sub(formatting.shape_.width, overhead).value_or(1);
                Indent offset = formatting.shape_.indent + overhead;
                Shape comment_shape = Shape::legacy(width, offset);

                bool block_style = false;
                if (!formatting.ends_with_newline_ && last) {
                    block_style = true;
                } else if (starts_with_newline(comment)) {
                    block_style = false;
                } else {
                    block_style = contains(trim(comment), '\n') || unicode_str_width(trim(comment)) > width;
                }

                return rewrite_comment(trim_start(comment), block_style, comment_shape, *formatting.config_);
            };

            RewriteResult formatted_comment = rewrite_post_comment(item_max_width);
            if (!formatted_comment) return formatted_comment;

            if (!starts_with_newline(comment)) {
                if (formatting.align_comments_) {
                    size_t comment_alignment = post_comment_alignment(item_max_width, unicode_str_width(inner_item));
                    if (first_line_width(*formatted_comment) + last_line_width(result) + comment_alignment + 1 >
                        formatting.config_->max_width()) {
                        item_max_width = std::nullopt;
                        formatted_comment = rewrite_post_comment(item_max_width);
                        if (!formatted_comment) return formatted_comment;
                        comment_alignment = post_comment_alignment(item_max_width, unicode_str_width(inner_item));
                    }
                    for (size_t k = 0; k <= comment_alignment; ++k) result.push_back(' ');
                }
                // An additional space for the missing trailing separator (or
                // if we skipped alignment above).
                if (!formatting.align_comments_ ||
                    (last && item_max_width.has_value() && !separate && !formatting.separator_.empty())) {
                    result.push_back(' ');
                }
            } else {
                result.push_back('\n');
                result += indent_str;
            }
            if (contains(*formatted_comment, '\n')) item_max_width = std::nullopt;
            result += *formatted_comment;
        } else {
            item_max_width = std::nullopt;
        }

        if (formatting.preserve_newline_ && !last && tactic == DefinitiveListTactic::Vertical() && item.new_lines) {
            item_max_width = std::nullopt;
            result.push_back('\n');
        }

        prev_item_had_post_comment = item.post_comment.has_value();
        prev_item_is_nested_import = contains(inner_item, "::");
    }

    return RewriteResult(std::move(result));
}

std::pair<Opt, ListItemCommentStyle> extract_pre_comment(std::string_view pre_snippet) {
    std::string_view trimmed_pre_snippet = trim(pre_snippet);
    // Both start and end are checked to support keeping a block comment inline with
    // the item, even if there are preceding line comments, while still supporting
    // a snippet that starts with a block comment but also contains one or more
    // trailing single line comments.
    // https://github.com/rust-lang/rustfmt/issues/3025
    // https://github.com/rust-lang/rustfmt/pull/3048
    // https://github.com/rust-lang/rustfmt/issues/3839
    bool starts_with_block_comment = starts_with(trimmed_pre_snippet, "/*");
    bool ends_with_block_comment = ends_with(trimmed_pre_snippet, "*/");
    bool starts_with_single_line_comment = starts_with(trimmed_pre_snippet, "//");
    if (ends_with_block_comment) {
        size_t comment_end = pre_snippet.rfind('/');
        if (comment_end == std::string_view::npos) panic("extract_pre_comment: no '/'");
        if (contains(pre_snippet.substr(comment_end), '\n')) {
            return {std::string(trimmed_pre_snippet), ListItemCommentStyle::DifferentLine};
        }
        return {std::string(trimmed_pre_snippet), ListItemCommentStyle::SameLine};
    }
    if (starts_with_single_line_comment || starts_with_block_comment) {
        return {std::string(trimmed_pre_snippet), ListItemCommentStyle::DifferentLine};
    }
    return {std::nullopt, ListItemCommentStyle::None};
}

Opt extract_post_comment(std::string_view post_snippet_in, size_t comment_end, std::string_view separator,
                         bool is_last) {
    // Cleanup post-comment: strip separators and whitespace.
    if (comment_end > post_snippet_in.size()) panic("extract_post_comment: comment_end out of range");
    std::string_view post_snippet = trim(post_snippet_in.substr(0, comment_end));

    bool last_inline_comment_ends_with_separator = false;
    if (is_last) {
        std::vector<std::string_view> ls = lines(post_snippet);
        if (!ls.empty()) {
            std::string_view line = ls.back();
            last_inline_comment_ends_with_separator = ends_with(line, separator) && starts_with(trim(line), "//");
        }
    }

    std::string_view post_snippet_trimmed;
    if (!post_snippet.empty() && (post_snippet[0] == ',' || post_snippet[0] == ':')) {
        post_snippet_trimmed = trim_space_tab(post_snippet.substr(1));
    } else if (starts_with(post_snippet, separator)) {
        post_snippet_trimmed = trim_space_tab(post_snippet.substr(separator.size()));
    } else if (last_inline_comment_ends_with_separator) {
        // since we're on the last item it's fine to keep any trailing separators in comments
        post_snippet_trimmed = trim_space_tab(post_snippet);
    }
    // not comment or over two lines
    else if (ends_with(post_snippet, separator) &&
             (!starts_with(trim(post_snippet), "//") || contains(trim(post_snippet), '\n'))) {
        post_snippet_trimmed = trim_space_tab(post_snippet.substr(0, post_snippet.size() - 1));
    } else {
        post_snippet_trimmed = post_snippet;
    }
    // FIXME(#3441): post_snippet includes 'const' now
    // it should not include here
    std::string_view removed_newline_snippet = trim(post_snippet_trimmed);
    if (!post_snippet_trimmed.empty() &&
        (starts_with(removed_newline_snippet, "//") || starts_with(removed_newline_snippet, "/*"))) {
        return std::string(post_snippet_trimmed);
    }
    return std::nullopt;
}

size_t get_comment_end(std::string_view post_snippet, std::string_view separator, std::string_view terminator,
                       bool is_last) {
    if (is_last) return find_uncommented(post_snippet, terminator).value_or(post_snippet.size());

    std::optional<size_t> block_open_index;
    if (size_t p = post_snippet.find("/*"); p != std::string_view::npos) block_open_index = p;
    // check if it really is a block comment (and not `//*` or a nested comment)
    if (block_open_index) {
        size_t i = *block_open_index;
        size_t j = post_snippet.find('/');
        if (j != std::string_view::npos && j < i) {
            block_open_index = std::nullopt;
        } else if (ends_with(post_snippet.substr(0, i), "/")) {
            block_open_index = std::nullopt;
        }
    }
    std::optional<size_t> newline_index;
    if (size_t p = post_snippet.find('\n'); p != std::string_view::npos) newline_index = p;
    if (std::optional<size_t> sep = find_uncommented(post_snippet, separator)) {
        size_t separator_index = *sep;
        auto comment_end_from = [&](size_t i) {
            std::optional<size_t> ce = find_comment_end(post_snippet.substr(i));
            if (!ce) panic("get_comment_end: unterminated block comment");
            return std::max(*ce + i, separator_index + 1);
        };
        if (block_open_index && !newline_index) {
            size_t i = *block_open_index;
            // Separator before comment, with the next item on same line.
            // Comment belongs to next item.
            if (i > separator_index) return separator_index + 1;
            // Block-style post-comment before the separator.
            return comment_end_from(i);
        }
        if (block_open_index && newline_index && *block_open_index < *newline_index) {
            // Block-style post-comment. Either before or after the separator.
            return comment_end_from(*block_open_index);
        }
        // Potential *single* line comment.
        if (newline_index && *newline_index > separator_index) return *newline_index + 1;
        return post_snippet.size();
    }
    if (newline_index) {
        // Match arms may not have trailing comma. In any case, for match arms,
        // we will assume that the post comment belongs to the next arm if they
        // do not end with trailing comma.
        return *newline_index + 1;
    }
    return 0;
}

// Account for extra whitespace between items. This is fiddly
// because of the way we divide pre- and post- comments.
bool has_extra_newline(std::string_view post_snippet, size_t comment_end) {
    if (post_snippet.empty() || comment_end == 0) return false;

    size_t len_last = last_char_len_utf8(post_snippet.substr(0, comment_end));
    // Everything from the separator to the next item.
    std::string_view test_snippet = post_snippet.substr(comment_end - len_last);
    size_t first_newline = test_snippet.find('\n');
    if (first_newline == std::string_view::npos) first_newline = test_snippet.size();
    // From the end of the first line of comments.
    test_snippet = test_snippet.substr(first_newline);
    size_t first = test_snippet.size() - trim_start(test_snippet).size();
    // From the end of the first line of comments to the next non-whitespace char.
    test_snippet = test_snippet.substr(0, first);

    // There were multiple line breaks which got trimmed to nothing.
    return count_newlines(test_snippet) > 1;
}

// itemize_list + ListItems::next, eagerly.
std::vector<ListItem> itemize_list(const SnippetProvider& sp, size_t n, std::string_view terminator,
                                   std::string_view separator, const std::function<uint32_t(size_t)>& get_lo,
                                   const std::function<uint32_t(size_t)>& get_hi,
                                   const std::function<RewriteResult(size_t)>& get_item_string,
                                   uint32_t prev_span_end, uint32_t next_span_start, bool leave_last) {
    std::vector<ListItem> out;
    out.reserve(n);
    for (size_t idx = 0; idx < n; ++idx) {
        // Pre-comment
        std::string_view pre_snippet = sp.span_to_snippet(mk_sp_ordered(prev_span_end, get_lo(idx))).value_or("");
        auto [pre_comment, pre_comment_style] = extract_pre_comment(pre_snippet);

        // Post-comment
        bool is_last = idx + 1 == n;
        uint32_t next_start = is_last ? next_span_start : get_lo(idx + 1);
        uint32_t hi = get_hi(idx);
        std::string_view post_snippet = sp.span_to_snippet(mk_sp_ordered(hi, next_start)).value_or("");
        size_t comment_end = get_comment_end(post_snippet, separator, terminator, is_last);
        bool new_lines = has_extra_newline(post_snippet, comment_end);
        Opt post_comment = extract_post_comment(post_snippet, comment_end, separator, is_last);

        prev_span_end = get_hi(idx) + static_cast<uint32_t>(comment_end);

        ListItem li;
        li.pre_comment = std::move(pre_comment);
        li.pre_comment_style = pre_comment_style;
        // leave_last is set to true only for rewrite_items
        if (is_last && leave_last) {
            li.item = RewriteResult(RewriteError::skip());
        } else {
            li.item = get_item_string(idx);
        }
        li.post_comment = std::move(post_comment);
        li.new_lines = new_lines;
        out.push_back(std::move(li));
    }
    return out;
}

size_t total_item_width(const ListItem& item) {
    return comment_len(item.pre_comment) + comment_len(item.post_comment) +
           (item.item.ok() ? unicode_str_width(*item.item) : 0);
}

// Compute horizontal and vertical shapes for a struct-lit-like thing.
std::optional<std::pair<std::optional<Shape>, Shape>> struct_lit_shape(Shape shape, const RewriteContext& ctx,
                                                                       size_t prefix_width, size_t suffix_width) {
    // non-default config: indent_style = Visual
    Shape v_shape = shape.block_indent(ctx.config->tab_spaces());
    v_shape.width = ctx.budget(v_shape.indent.width());
    std::optional<Shape> h_shape;
    if (std::optional<size_t> w = checked_sub(shape.width, prefix_width + suffix_width)) {
        size_t shape_width = std::min(*w, ctx.config->struct_lit_width());
        h_shape = Shape::legacy(shape_width, shape.indent);
    }
    return std::make_pair(h_shape, v_shape);
}

// Compute the tactic for the internals of a struct-lit-like thing.
DefinitiveListTactic struct_lit_tactic(std::optional<Shape> h_shape, const RewriteContext& ctx,
                                       const std::vector<ListItem>& items) {
    if (h_shape) {
        // non-default config: indent_style = Visual, struct_lit_single_line = false
        (void)ctx;
        ListTactic prelim_tactic = ListTactic::HorizontalVertical();
        return definitive_tactic(items, prelim_tactic, Separator::Comma, h_shape->width);
    }
    return DefinitiveListTactic::Vertical();
}

// Given a tactic and possible shapes for horizontal and vertical layout,
// come up with the actual shape to use.
Shape shape_for_tactic(DefinitiveListTactic tactic, std::optional<Shape> h_shape, Shape v_shape) {
    if (tactic.kind == DefinitiveListTacticKind::Horizontal) {
        if (!h_shape) panic("shape_for_tactic: no horizontal shape");
        return *h_shape;
    }
    return v_shape;
}

// Create a ListFormatting object for formatting the internals of a
// struct-lit-like thing, that is a series of fields.
ListFormatting struct_lit_formatting(Shape shape, DefinitiveListTactic tactic, const RewriteContext& ctx,
                                     bool force_no_trailing_comma) {
    bool ends_with_newline = tactic == DefinitiveListTactic::Vertical();
    ListFormatting f(shape, *ctx.config);
    f.tactic_ = tactic;
    f.separator_ = ",";
    // trailing_comma = Vertical (default)
    f.trailing_separator_ = force_no_trailing_comma ? SeparatorTactic::Never : SeparatorTactic::Vertical;
    f.separator_place_ = SeparatorPlace::Back;
    f.ends_with_newline_ = ends_with_newline;
    f.preserve_newline_ = true;
    f.nested_ = false;
    f.align_comments_ = true;
    return f;
}

}  // namespace rustfmt
