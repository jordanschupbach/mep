// rust_parse_attr.cpp -- outer / inner attributes and doc comments.
//
// Ported from rustc_parse/rustc_lexer (rust-lang/rust 1.97.1, MIT OR
// Apache-2.0): rustc_parse/src/parser/attr.rs. Only the parser path is
// ported (meta-item parsing is for later compiler passes); diagnostics keep
// their primary message and span.
#include "rust_parse_internal.h"

namespace rust {
namespace detail {

// attr::mk_doc_comment for the doc comment token just bumped.
Attribute Parser::mk_doc_comment_attr(const Token& doc_tok, AttrStyle style) {
    Attribute a;
    a.style = style;
    a.span = prev_token.span;
    a.is_doc_comment = true;
    a.comment_kind = doc_tok.doc_kind;
    a.doc = doc_tok.sym;
    return a;
}

/// Parses attributes that appear before an item.
AttrWrapper Parser::parse_outer_attributes() {
    AttrWrapper w;
    bool just_parsed_doc_comment = false;
    w.start_pos = st_.num_bump_calls;
    w.has_start_pos = true;
    for (;;) {
        bool have = false;
        Attribute attr;
        if (check(EXP(Pound))) {
            InnerAttrPolicy inner_parse_policy = InnerAttrPolicy::Forbidden();
            if (!w.attrs.empty()) {
                Span prev_outer_attr_sp = w.attrs.back().span;
                inner_parse_policy = just_parsed_doc_comment
                                         ? InnerAttrPolicy::Forbidden(InnerAttrForbiddenReasonKind::AfterOuterDocComment,
                                                                      prev_outer_attr_sp)
                                         : InnerAttrPolicy::Forbidden(InnerAttrForbiddenReasonKind::AfterOuterAttribute,
                                                                      prev_outer_attr_sp);
            }
            just_parsed_doc_comment = false;
            attr = parse_attribute(inner_parse_policy);
            have = true;
        } else if (token.kind == Tok::DocComment) {
            if (token.doc != DocStyle::Outer) {
                // annotate_following_item_if_applicable only adds notes / suggestions.
                emit("expected outer doc comment", token.span);
            }
            Token doc_tok = token;
            bump();
            just_parsed_doc_comment = true;
            attr = mk_doc_comment_attr(doc_tok, AttrStyle::Outer);
            have = true;
        }
        if (!have) break;
        if (attr.style == AttrStyle::Outer) w.attrs.push_back(attr);
    }
    return w;
}

/// Matches `attribute = # ! [ meta_item ]`.
/// `inner_parse_policy` prescribes how to handle inner attributes.
Attribute Parser::parse_attribute(InnerAttrPolicy inner_parse_policy) {
    Span lo = token.span;
    bool ate = eat(EXP(Pound));
    (void)ate;  // rustc asserts: only called at `#`
    AttrStyle style = eat(EXP(Bang)) ? AttrStyle::Inner : AttrStyle::Outer;
    expect(EXP(OpenBracket));
    Attribute attr;
    parse_attr_item(attr);
    expect(EXP(CloseBracket));
    Span attr_sp = lo.to(prev_token.span);
    if (style == AttrStyle::Inner) error_on_forbidden_inner_attr(attr_sp, inner_parse_policy, true);
    attr.style = style;
    attr.span = attr_sp;
    return attr;
}

void Parser::error_on_forbidden_inner_attr(Span attr_sp, InnerAttrPolicy policy, bool suggest_to_outer) {
    (void)suggest_to_outer;  // suggestion only
    if (policy.permitted) return;
    if (policy.has_reason && policy.reason == InnerAttrForbiddenReasonKind::AfterOuterDocComment) {
        emit("an inner attribute is not permitted following an outer doc comment", attr_sp);
    } else if (policy.has_reason && policy.reason == InnerAttrForbiddenReasonKind::AfterOuterAttribute) {
        emit("an inner attribute is not permitted following an outer attribute", attr_sp);
    } else {
        emit("an inner attribute is not permitted in this context", attr_sp);
    }
    // annotate_following_item_if_applicable: notes and suggestions only.
}

/// Parses an inner part of an attribute (the path and following tokens).
///     PATH `[` TOKEN_STREAM `]`
///     PATH `{` TOKEN_STREAM `}`
///     PATH
///     PATH `=` UNSUFFIXED_LIT
/// The delimiters or `=` are still put into the resulting token stream.
void Parser::parse_attr_item(Attribute& out) {
    bool is_unsafe = eat_keyword(KW(Unsafe));
    Safety unsafety;
    if (is_unsafe) {
        Span unsafe_span = prev_token.span;
        expect(EXP(OpenParen));
        unsafety.kind = Safety::Unsafe;
        unsafety.span = unsafe_span;
    }
    Path* path = parse_path(PathStyle::Mod);
    AttrArgs args = parse_attr_args();
    if (is_unsafe) expect(EXP(CloseParen));
    out.unsafety = unsafety;
    out.path = path;
    out.args = args;
}

/// Parses attributes that appear after the opening of an item. These should
/// be preceded by an exclamation mark, but we accept and warn about one
/// terminated by a semicolon.
///
/// Matches `inner_attrs*`.
AttrVec Parser::parse_inner_attributes() {
    AttrVec attrs;
    for (;;) {
        bool have = false;
        Attribute attr;
        if (check(EXP(Pound)) && look_ahead(1, [](const Token& t) { return t.kind == Tok::Bang; })) {
            attr = parse_attribute(InnerAttrPolicy::Permitted());
            have = true;
        } else if (token.kind == Tok::DocComment) {
            if (token.doc == DocStyle::Inner) {
                Token doc_tok = token;
                bump();
                attr = mk_doc_comment_attr(doc_tok, AttrStyle::Inner);
                have = true;
            }
        }
        if (!have) break;
        attrs.push_back(attr);
    }
    return attrs;
}

}  // namespace detail
}  // namespace rust
