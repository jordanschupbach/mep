// mep's OCaml language server: odoc doc comments to Markdown, hover text for
// items, and splitting a type at its arrows (signature help).
//
// odoc markup handled: `[code]`, `{[ code ]}` / `{@lang[ code ]}`, `{v verbatim v}`,
// `{b ..}`, `{i ..}`, `{e ..}`, `{^ ..}`, `{_ ..}`, `{!ref}`, `{{!ref} text}`,
// `{{:url} text}`, `{:url}`, `{ul {li ..}}`, `{ol {li ..}}`, `{- ..}`, `{1 ..}`..`{5 ..}`
// headings, `{%html: .. %}` (dropped), escapes, and the tags `@param`, `@return(s)`,
// `@raise(s)`, `@see`, `@since`, `@deprecated`, `@before`, `@author`, `@version`.

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

#include "ocaml_format.h"
#include "ocaml_lsp_internal.h"

namespace mllsp {

namespace {

std::string Trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return std::string(s.substr(b, e - b));
}

std::string CollapseWs(std::string_view s) {
    std::string out;
    bool sp = false;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            sp = true;
            continue;
        }
        if (sp && !out.empty()) out += ' ';
        sp = false;
        out += c;
    }
    return out;
}

std::string Dedent(const std::string &s) {
    std::vector<std::string> lines;
    std::istringstream in(s);
    std::string l;
    while (std::getline(in, l)) {
        while (!l.empty() && (l.back() == ' ' || l.back() == '\r')) l.pop_back();
        lines.push_back(l);
    }
    while (!lines.empty() && Trim(lines.front()).empty()) lines.erase(lines.begin());
    while (!lines.empty() && Trim(lines.back()).empty()) lines.pop_back();
    size_t common = SIZE_MAX;
    for (const std::string &x : lines) {
        if (Trim(x).empty()) continue;
        size_t n = 0;
        while (n < x.size() && x[n] == ' ') n++;
        common = std::min(common, n);
    }
    if (common == SIZE_MAX) common = 0;
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) out += '\n';
        out += lines[i].size() >= common ? lines[i].substr(common) : Trim(lines[i]);
    }
    return out;
}

/** `val-foo` / `module-Foo.type-t` -> `foo` / `Foo.t`. */
std::string CleanRef(std::string r) {
    std::string out;
    for (const std::string &part : SplitPath(r)) {
        std::string p = part;
        const size_t dash = p.find('-');
        if (dash != std::string::npos && dash > 0 && dash < 12 && std::islower(static_cast<unsigned char>(p[0])))
            p = p.substr(dash + 1);
        if (!out.empty()) out += '.';
        out += p;
    }
    return out.empty() ? r : out;
}

class Converter {
public:
    explicit Converter(std::string_view s) : s_(s) {}

    /** Converts until the end or an unmatched `}` (when @p in_brace). */
    std::string Inline(bool in_brace) {
        std::string out;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '\\' && i_ + 1 < s_.size() && std::strchr("{}[]@", s_[i_ + 1])) {
                out += s_[i_ + 1];
                i_ += 2;
            } else if (c == '}' && in_brace) {
                i_++;
                return out;
            } else if (c == '[') {
                if (!out.empty() && out.back() == '`') out += ' ';  // adjacent code spans
                out += InlineCode();
            } else if (c == '{') {
                out += Brace();
            } else {
                out += c;
                i_++;
            }
        }
        return out;
    }

private:
    std::string_view s_;
    size_t i_ = 0;

    std::string InlineCode() {
        // [code], brackets nest.
        int depth = 0;
        size_t j = i_;
        for (; j < s_.size(); j++) {
            if (s_[j] == '\\' && j + 1 < s_.size()) {
                j++;
                continue;
            }
            if (s_[j] == '[') depth++;
            if (s_[j] == ']' && --depth == 0) break;
        }
        std::string code = CollapseWs(s_.substr(i_ + 1, std::min(j, s_.size()) - i_ - 1));
        i_ = std::min(j + 1, s_.size());
        const std::string fence = code.find('`') != std::string::npos ? "``" : "`";
        return fence + code + fence;
    }

    std::string CodeBlock(const std::string &lang, const char *close) {
        const size_t end = s_.find(close, i_);
        std::string code(s_.substr(i_, end == std::string_view::npos ? std::string_view::npos : end - i_));
        i_ = end == std::string_view::npos ? s_.size() : end + std::strlen(close);
        return "\n\n```" + lang + "\n" + Dedent(code) + "\n```\n\n";
    }

    std::string Brace() {
        const std::string_view rest = s_.substr(i_);
        auto starts = [&](const char *p) { return rest.compare(0, std::strlen(p), p) == 0; };
        if (starts("{[")) {
            i_ += 2;
            return CodeBlock("ocaml", "]}");
        }
        if (starts("{@")) {
            const size_t br = rest.find('[');
            if (br != std::string_view::npos) {
                std::string lang(rest.substr(2, br - 2));
                const size_t colon = lang.find_first_of(" :");
                if (colon != std::string::npos) lang = lang.substr(0, colon);
                i_ += br + 1;
                return CodeBlock(lang, "]}");
            }
        }
        if (starts("{v")) {
            i_ += 2;
            return CodeBlock("", "v}");
        }
        if (starts("{%")) {
            const size_t end = rest.find("%}");
            i_ += end == std::string_view::npos ? rest.size() : end + 2;
            return std::string();
        }
        if (starts("{{!") || starts("{{:")) {
            // {{!ref} text} / {{:url} text}
            const size_t close = rest.find('}');
            if (close == std::string_view::npos) {
                i_++;
                return "{";
            }
            const std::string target(rest.substr(3, close - 3));
            const bool url = rest[2] == ':';
            i_ += close + 1;
            std::string text = Trim(Inline(true));
            if (url) return "[" + (text.empty() ? target : text) + "](" + target + ")";
            return text.empty() ? "`" + CleanRef(target) + "`" : text;
        }
        if (starts("{!")) {
            const size_t close = rest.find('}');
            if (close == std::string_view::npos) {
                i_++;
                return "{";
            }
            const std::string target(rest.substr(2, close - 2));
            i_ += close + 1;
            return "`" + CleanRef(target) + "`";
        }
        if (starts("{:")) {
            const size_t close = rest.find('}');
            if (close == std::string_view::npos) {
                i_++;
                return "{";
            }
            const std::string url(rest.substr(2, close - 2));
            i_ += close + 1;
            return "<" + url + ">";
        }
        // {tag content}
        size_t j = 1;
        while (j < rest.size() && !std::isspace(static_cast<unsigned char>(rest[j])) && rest[j] != '}' &&
               rest[j] != '{')
            j++;
        const std::string tag(rest.substr(1, j - 1));
        i_ += j;
        if (tag == "ul" || tag == "ol") return List(tag == "ol");
        if (tag == "-" || tag == "li") return "\n- " + CollapseWs(Inline(true)) + "\n";
        std::string inner = Trim(Inline(true));
        if (tag == "b") return "**" + inner + "**";
        if (tag == "i" || tag == "e") return "*" + inner + "*";
        if (tag == "^") return "^" + inner;
        if (tag == "_") return "_" + inner;
        if (!tag.empty() && std::isdigit(static_cast<unsigned char>(tag[0]))) {
            int level = tag[0] - '0';
            level = std::max(1, std::min(level + 1, 6));
            return "\n\n" + std::string(static_cast<size_t>(level), '#') + " " + CollapseWs(inner) + "\n\n";
        }
        return inner;  // unknown: keep the text
    }

    std::string List(bool ordered) {
        std::string out = "\n\n";
        int n = 1;
        while (i_ < s_.size()) {
            while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) i_++;
            if (i_ >= s_.size()) break;
            if (s_[i_] == '}') {
                i_++;
                break;
            }
            std::string item;
            if (s_.compare(i_, 3, "{li") == 0 && i_ + 3 < s_.size() &&
                std::isspace(static_cast<unsigned char>(s_[i_ + 3]))) {
                i_ += 3;
                item = Inline(true);
            } else if (s_.compare(i_, 2, "{-") == 0) {
                i_ += 2;
                item = Inline(true);
            } else {
                // Stray text inside the list: take it up to the next item.
                item = Inline(true);
                out += CollapseWs(item) + "\n";
                break;
            }
            out += (ordered ? std::to_string(n++) + ". " : std::string("- ")) + CollapseWs(item) + "\n";
        }
        return out + "\n";
    }
};

std::string Convert(std::string_view s) {
    Converter c(s);
    std::string md = c.Inline(false);
    // Normalise blank-line runs.
    std::string out;
    int newlines = 0;
    for (char ch : md) {
        if (ch == '\n') {
            if (++newlines <= 2) out += ch;
            continue;
        }
        newlines = 0;
        out += ch;
    }
    return Trim(out);
}

struct Tag {
    std::string name, arg, text;
};

}  // namespace

std::string OdocToMarkdown(const std::string &raw) {
    // Split off the tags: lines starting with `@word`.
    std::string body;
    std::vector<Tag> tags;
    std::istringstream in(raw);
    std::string line;
    bool in_code = false;
    while (std::getline(in, line)) {
        const std::string t = Trim(line);
        if (t.find("{[") != std::string::npos || t.find("{v") != std::string::npos) in_code = true;
        if (in_code && (t.find("]}") != std::string::npos || t.find("v}") != std::string::npos)) {
            in_code = false;
            (tags.empty() ? body : tags.back().text) += line + "\n";
            continue;
        }
        if (!in_code && t.size() > 1 && t[0] == '@' && std::isalpha(static_cast<unsigned char>(t[1]))) {
            Tag tag;
            size_t j = 1;
            while (j < t.size() && std::isalpha(static_cast<unsigned char>(t[j]))) j++;
            tag.name = t.substr(1, j - 1);
            std::string rest = Trim(t.substr(j));
            if (tag.name == "param" || tag.name == "raise" || tag.name == "raises" || tag.name == "before") {
                const size_t sp = rest.find_first_of(" \t");
                tag.arg = rest.substr(0, sp);
                rest = sp == std::string::npos ? std::string() : Trim(rest.substr(sp));
            } else if (tag.name == "see") {
                // @see <url> text / @see 'file' text / @see "doc" text
                if (!rest.empty() && (rest[0] == '<' || rest[0] == '\'' || rest[0] == '"')) {
                    const char close = rest[0] == '<' ? '>' : rest[0];
                    const size_t e = rest.find(close, 1);
                    if (e != std::string::npos) {
                        tag.arg = rest.substr(0, e + 1);
                        rest = Trim(rest.substr(e + 1));
                    }
                }
            }
            tag.text = rest + "\n";
            tags.push_back(tag);
            continue;
        }
        (tags.empty() ? body : tags.back().text) += line + "\n";
    }
    std::string out = Convert(body);
    auto section = [&](const std::string &s) {
        if (!out.empty()) out += "\n\n";
        out += s;
    };
    std::string params, raises, sees, other;
    std::string returns, deprecated, since;
    for (const Tag &t : tags) {
        const std::string text = Convert(t.text);
        if (t.name == "param") {
            params += "- `" + t.arg + "`" + (text.empty() ? "" : ": " + text) + "\n";
        } else if (t.name == "return" || t.name == "returns") {
            returns += (returns.empty() ? "" : " ") + text;
        } else if (t.name == "raise" || t.name == "raises") {
            raises += "- `" + t.arg + "`" + (text.empty() ? "" : ": " + text) + "\n";
        } else if (t.name == "see") {
            std::string target = t.arg;
            if (target.size() > 2 && target[0] == '<') {
                const std::string url = target.substr(1, target.size() - 2);
                target = "<" + url + ">";
            } else if (!target.empty()) {
                target = "`" + target.substr(1, target.size() - 2) + "`";
            }
            sees += "- " + target + (text.empty() ? "" : (target.empty() ? "" : " ") + text) + "\n";
        } else if (t.name == "since") {
            since = text;
        } else if (t.name == "deprecated") {
            deprecated = text;
        } else if (t.name == "before") {
            other += "**Before " + t.arg + ":** " + text + "\n\n";
        } else if (t.name == "author") {
            other += "**Author:** " + text + "\n\n";
        } else if (t.name == "version") {
            other += "**Version:** " + text + "\n\n";
        } else if (t.name == "canonical" || t.name == "inline" || t.name == "open" || t.name == "closed" ||
                   t.name == "hidden") {
            // odoc directives: nothing to show
        } else {
            other += "**@" + t.name + "** " + text + "\n\n";
        }
    }
    if (!deprecated.empty() || std::any_of(tags.begin(), tags.end(), [](const Tag &t) { return t.name == "deprecated"; }))
        section("**Deprecated.**" + (deprecated.empty() ? std::string() : " " + deprecated));
    if (!params.empty()) section("**Parameters**\n" + params.substr(0, params.size() - 1));
    if (!returns.empty()) section("**Returns:** " + returns);
    if (!raises.empty()) section("**Raises**\n" + raises.substr(0, raises.size() - 1));
    if (!sees.empty()) section("**See also:**\n" + sees.substr(0, sees.size() - 1));
    if (!since.empty()) section("**Since:** " + since);
    if (!other.empty()) section(Trim(other));
    return out;
}

std::string OdocSummary(const std::string &raw) {
    std::string md = OdocToMarkdown(raw);
    const size_t para = md.find("\n\n");
    if (para != std::string::npos) md = md.substr(0, para);
    md = CollapseWs(md);
    if (md.size() > 160) md = md.substr(0, 157) + "...";
    return md;
}

std::string ItemMarkdown(const Item &item, const std::string &qualifier) {
    std::string sig = item.signature.empty() ? item.name : item.signature;
    // Very long signatures (big variants) are cut.
    {
        size_t lines = 0, pos = 0;
        while ((pos = sig.find('\n', pos)) != std::string::npos) {
            if (++lines >= 30) {
                sig = sig.substr(0, pos) + "\n...";
                break;
            }
            pos++;
        }
    }
    std::string out = "```ocaml\n" + sig + "\n```";
    std::string where;
    if (item.kind == ItemKind::Constructor || item.kind == ItemKind::Field) {
        where = std::string(item.kind == ItemKind::Constructor ? "Constructor" : "Field") + " of type `" +
                (qualifier.empty() ? "" : qualifier + ".") + item.owner_type + "`";
    } else if (!qualifier.empty()) {
        where = "`" + qualifier + "`";
    }
    if (!where.empty()) out += "\n\n" + where;
    const std::string doc = OdocToMarkdown(item.doc);
    if (item.is_deprecated && doc.find("**Deprecated.**") == std::string::npos)
        out += "\n\n**Deprecated.**" + (item.deprecated.empty() ? std::string() : " " + item.deprecated);
    if (!doc.empty()) out += "\n\n---\n\n" + doc;
    return out;
}

std::vector<ArrowParam> SplitArrows(const std::string &type, std::string *result) {
    std::vector<ArrowParam> out;
    std::vector<ocamlfmt::Token> toks;
    std::string err;
    int err_line = 0;
    if (!ocamlfmt::Lex(type, &toks, &err, &err_line)) return out;
    toks.erase(std::remove_if(toks.begin(), toks.end(),
                              [](const ocamlfmt::Token &t) { return t.kind == ocamlfmt::TokKind::Comment; }),
               toks.end());
    size_t start = 0;
    // `'a 'b. ...` / `type a. ...`
    {
        size_t k = 0;
        if (k < toks.size() && toks[k].text == "type") k++;
        size_t vars = 0;
        while (k < toks.size() && (toks[k].kind == ocamlfmt::TokKind::TyVar || toks[k].kind == ocamlfmt::TokKind::Ident)) {
            k++;
            vars++;
        }
        if (vars > 0 && k < toks.size() && toks[k].text == ".") start = k + 1;
    }
    int depth = 0;
    std::vector<std::pair<size_t, size_t>> pieces;  // token index ranges
    size_t piece = start;
    for (size_t k = start; k < toks.size(); k++) {
        const std::string_view t = toks[k].text;
        if (t == "(" || t == "[" || t == "{" || t == "[|" || t == "[<" || t == "[>" || t == "{<" || t == "<") depth++;
        else if (t == ")" || t == "]" || t == "}" || t == "|]" || t == ">}" || t == ">") depth = std::max(0, depth - 1);
        else if (t == "->" && depth == 0) {
            pieces.emplace_back(piece, k);
            piece = k + 1;
        }
    }
    pieces.emplace_back(piece, toks.size());
    auto text_of = [&](size_t a, size_t b) {
        if (a >= b) return std::string();
        return type.substr(toks[a].begin, toks[b - 1].end - toks[a].begin);
    };
    for (size_t p = 0; p + 1 < pieces.size(); p++) {
        ArrowParam ap;
        const size_t a = pieces[p].first, b = pieces[p].second;
        ap.text = Collapse(text_of(a, b));
        if (a < b) {
            const ocamlfmt::Token &t0 = toks[a];
            if (t0.kind == ocamlfmt::TokKind::Label) {
                std::string l(t0.text);
                ap.optional = l[0] == '?';
                l.erase(0, 1);
                if (!l.empty() && l.back() == ':') l.pop_back();
                ap.label = l;
            } else if (t0.kind == ocamlfmt::TokKind::Ident && a + 1 < b && toks[a + 1].text == ":") {
                ap.label = std::string(t0.text);
            }
        }
        out.push_back(ap);
    }
    if (result) *result = Collapse(text_of(pieces.back().first, pieces.back().second));
    return out;
}

}  // namespace mllsp
