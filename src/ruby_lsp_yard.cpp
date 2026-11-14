// YARD doc comments for mep's Ruby language server (ruby_lsp_internal.h).
//
// A doc comment is the block of `#` lines right above a definition. Its free
// text is Markdown (YARD's default markup in practice); tags start a line with
// `@name` and continue on the lines indented under them:
//
//   # Filters rows.
//   #
//   # @param column [String] the column to test
//   # @param value [Float, nil] the value to keep
//   #   (nil keeps everything)
//   # @option opts [Boolean] :strict (false) raise on unknown columns
//   # @return [Frame] a new frame
//   # @raise [ArgumentError] when the column does not exist
//   # @example
//   #   frame.filter("x", 1.0)
//   # @overload filter(column)
//   #   @param column [String] ...
//
// Rosetta's Ruby backend writes exactly this into the generated stub file.
#include <algorithm>
#include <cctype>

#include "ruby_lsp_internal.h"

namespace rblsp {

namespace {

std::string Trim(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

int Indent(const std::string &s) {
    int n = 0;
    while (static_cast<size_t>(n) < s.size() && (s[static_cast<size_t>(n)] == ' ' || s[static_cast<size_t>(n)] == '\t')) n++;
    return n;
}

bool Blank(const std::string &s) { return Trim(s).empty(); }

// Removes the common leading indentation of the non-blank lines.
std::vector<std::string> Dedent(const std::vector<std::string> &lines) {
    int common = -1;
    for (const std::string &l : lines) {
        if (Blank(l)) continue;
        const int i = Indent(l);
        common = common < 0 ? i : std::min(common, i);
    }
    std::vector<std::string> out;
    for (const std::string &l : lines) {
        if (Blank(l)) {
            out.emplace_back();
        } else {
            out.push_back(l.substr(static_cast<size_t>(std::max(common, 0))));
        }
    }
    while (!out.empty() && out.back().empty()) out.pop_back();
    while (!out.empty() && out.front().empty()) out.erase(out.begin());
    return out;
}

std::string Join(const std::vector<std::string> &lines, const char *sep) {
    std::string s;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) s += sep;
        s += lines[i];
    }
    return s;
}

// Joins a tag's text lines into one paragraph (blank lines kept as paragraph breaks).
std::string Flow(const std::vector<std::string> &lines) {
    std::string s;
    for (const std::string &raw : lines) {
        const std::string l = Trim(raw);
        if (l.empty()) {
            if (!s.empty() && s.back() != '\n') s += "\n\n";
            continue;
        }
        if (!s.empty() && s.back() != '\n') s += ' ';
        s += l;
    }
    return Trim(s);
}

// `[Types] rest` -> types, rest (the brackets may nest: `[Hash{String => Array<Float>}]`).
bool TakeTypes(std::string &rest, std::string *types) {
    std::string s = Trim(rest);
    if (s.empty() || s[0] != '[') return false;
    int depth = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '[') depth++;
        else if (s[i] == ']' && --depth == 0) {
            *types = Trim(s.substr(1, i - 1));
            rest = Trim(s.substr(i + 1));
            return true;
        }
    }
    return false;
}

// The next whitespace-delimited word of @p rest.
std::string TakeWord(std::string &rest) {
    std::string s = Trim(rest);
    size_t i = 0;
    while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) i++;
    std::string w = s.substr(0, i);
    rest = Trim(s.substr(i));
    return w;
}

}  // namespace

bool YardDoc::Empty() const {
    return text.empty() && params.empty() && options.empty() && returns.empty() && raises.empty() &&
           examples.empty() && overloads.empty() && yield.empty() && yieldparams.empty() && yieldreturn.empty() &&
           notes.empty() && see.empty() && deprecated.empty() && !deprecated_tag;
}

std::string YardDoc::Summary() const {
    std::string first;
    for (char c : text) {
        if (c == '\n') {
            if (!first.empty() && first.back() == ' ') break;  // paragraph break
            first += ' ';
            continue;
        }
        first += c;
    }
    first = Trim(first);
    const size_t dot = first.find(". ");
    if (dot != std::string::npos) first = first.substr(0, dot + 1);
    return first;
}

std::shared_ptr<YardDoc> ParseYard(const std::vector<std::string> &input) {
    auto doc = std::make_shared<YardDoc>();
    const std::vector<std::string> lines = Dedent(input);
    std::vector<std::string> free;
    size_t i = 0;
    bool in_fence = false;
    while (i < lines.size()) {
        const std::string &line = lines[i];
        const std::string t = Trim(line);
        if (t.compare(0, 3, "```") == 0) in_fence = !in_fence;
        if (in_fence || Indent(line) > 0 || t.empty() || t[0] != '@' || t.size() < 2 ||
            !(std::isalpha(static_cast<unsigned char>(t[1])) || t[1] == '!')) {
            free.push_back(line);
            i++;
            continue;
        }
        // A tag: its first line and the lines indented under it (blank lines inside too,
        // when an indented line follows).
        std::string rest = t.substr(1);
        const std::string tag = TakeWord(rest);
        std::vector<std::string> body;  // continuation lines, raw
        size_t j = i + 1;
        while (j < lines.size()) {
            if (Blank(lines[j])) {
                size_t k = j;
                while (k < lines.size() && Blank(lines[k])) k++;
                if (k < lines.size() && Indent(lines[k]) > 0) {
                    for (; j < k; j++) body.emplace_back();
                    continue;
                }
                break;
            }
            if (Indent(lines[j]) == 0) break;
            body.push_back(lines[j]);
            j++;
        }
        i = j;
        std::vector<std::string> text_lines{rest};
        text_lines.insert(text_lines.end(), body.begin(), body.end());
        if (tag == "param") {
            YardParam p;
            if (!TakeTypes(rest, &p.types)) {
                p.name = TakeWord(rest);
                TakeTypes(rest, &p.types);
            } else {
                p.name = TakeWord(rest);
            }
            std::vector<std::string> tl{rest};
            tl.insert(tl.end(), body.begin(), body.end());
            p.text = Flow(tl);
            doc->params.push_back(p);
        } else if (tag == "option") {
            YardOption o;
            o.hash = TakeWord(rest);
            TakeTypes(rest, &o.types);
            o.key = TakeWord(rest);
            std::string r = Trim(rest);
            if (!r.empty() && r[0] == '(') {
                const size_t close = r.find(')');
                if (close != std::string::npos) {
                    o.def = Trim(r.substr(1, close - 1));
                    r = Trim(r.substr(close + 1));
                }
            }
            std::vector<std::string> tl{r};
            tl.insert(tl.end(), body.begin(), body.end());
            o.text = Flow(tl);
            doc->options.push_back(o);
        } else if (tag == "return" || tag == "raise" || tag == "yieldreturn") {
            YardTyped y;
            TakeTypes(rest, &y.types);
            std::vector<std::string> tl{rest};
            tl.insert(tl.end(), body.begin(), body.end());
            y.text = Flow(tl);
            (tag == "return" ? doc->returns : tag == "raise" ? doc->raises : doc->yieldreturn).push_back(y);
        } else if (tag == "yieldparam") {
            YardParam p;
            if (!TakeTypes(rest, &p.types)) {
                p.name = TakeWord(rest);
                TakeTypes(rest, &p.types);
            } else {
                p.name = TakeWord(rest);
            }
            std::vector<std::string> tl{rest};
            tl.insert(tl.end(), body.begin(), body.end());
            p.text = Flow(tl);
            doc->yieldparams.push_back(p);
        } else if (tag == "yield") {
            doc->yield = Flow(text_lines);
        } else if (tag == "example") {
            YardExample e;
            e.title = Trim(rest);
            e.code = Join(Dedent(body), "\n");
            doc->examples.push_back(e);
        } else if (tag == "overload") {
            YardOverload o;
            o.signature = Trim(rest);
            o.doc = ParseYard(body);
            doc->overloads.push_back(o);
        } else if (tag == "note") {
            doc->notes.push_back(Flow(text_lines));
        } else if (tag == "see") {
            doc->see.push_back(Flow(text_lines));
        } else if (tag == "todo") {
            doc->todo.push_back(Flow(text_lines));
        } else if (tag == "deprecated") {
            doc->deprecated_tag = true;
            doc->deprecated = Flow(text_lines);
        } else if (tag == "since") {
            doc->since = Flow(text_lines);
        } else if (tag == "api") {
            doc->api = Flow(text_lines);
        } else if (tag == "author") {
            doc->author = Flow(text_lines);
        } else if (tag == "version") {
            doc->version = Flow(text_lines);
        } else if (tag == "private") {
            doc->private_tag = true;
        } else if (tag == "abstract") {
            doc->abstract_tag = true;
            if (!Trim(rest).empty()) doc->notes.push_back("Abstract: " + Flow(text_lines));
        }
        // Unknown tags and `@!directives` are left out.
    }
    doc->text = Join(Dedent(free), "\n");
    return doc;
}

std::string FirstType(const std::string &types) {
    int depth = 0;
    std::string cur;
    std::vector<std::string> all;
    for (char c : types) {
        if (c == '<' || c == '{' || c == '(' || c == '[') depth++;
        if (c == '>' || c == '}' || c == ')' || c == ']') depth--;
        if (c == ',' && depth == 0) {
            all.push_back(Trim(cur));
            cur.clear();
            continue;
        }
        cur += c;
    }
    all.push_back(Trim(cur));
    for (const std::string &t : all) {
        if (t.empty() || t == "nil" || t == "void" || t == "NilClass" || t[0] == '#') continue;
        return t;
    }
    return std::string();
}

namespace {

std::string Code(const std::string &s) {
    if (s.find('`') != std::string::npos) return "``" + s + "``";
    return "`" + s + "`";
}

std::string TypesMd(const std::string &types) { return types.empty() ? std::string() : Code(types); }

}  // namespace

std::string YardMarkdown(const YardDoc &d, const std::string &highlight) {
    std::string s;
    auto para = [&s](const std::string &p) {
        if (p.empty()) return;
        if (!s.empty()) s += "\n\n";
        s += p;
    };
    if (d.deprecated_tag) para("**Deprecated.**" + (d.deprecated.empty() ? std::string() : " " + d.deprecated));
    if (d.abstract_tag) para("*Abstract.*");
    para(d.text);
    if (!d.params.empty()) {
        std::string p = "**Parameters**";
        for (const YardParam &x : d.params) {
            const bool hl = !highlight.empty() && x.name == highlight;
            p += "\n- " + std::string(hl ? "**" : "") + Code(x.name) + (hl ? "**" : "");
            if (!x.types.empty()) p += " (" + TypesMd(x.types) + ")";
            if (!x.text.empty()) p += " — " + x.text;
        }
        para(p);
    }
    if (!d.options.empty()) {
        std::string p = "**Options**";
        for (const YardOption &o : d.options) {
            p += "\n- " + Code(o.key);
            std::string meta;
            if (!o.types.empty()) meta = TypesMd(o.types);
            if (!o.def.empty()) meta += std::string(meta.empty() ? "" : ", ") + "default " + Code(o.def);
            if (!meta.empty()) p += " (" + meta + ")";
            if (!o.hash.empty()) p += " in " + Code(o.hash);
            if (!o.text.empty()) p += " — " + o.text;
        }
        para(p);
    }
    if (!d.yield.empty() || !d.yieldparams.empty() || !d.yieldreturn.empty()) {
        std::string p = "**Yields**";
        if (!d.yield.empty()) p += " " + d.yield;
        for (const YardParam &x : d.yieldparams) {
            p += "\n- " + Code(x.name);
            if (!x.types.empty()) p += " (" + TypesMd(x.types) + ")";
            if (!x.text.empty()) p += " — " + x.text;
        }
        for (const YardTyped &y : d.yieldreturn) {
            p += "\n- block returns " + TypesMd(y.types);
            if (!y.text.empty()) p += " — " + y.text;
        }
        para(p);
    }
    for (const YardTyped &r : d.returns) {
        std::string p = "**Returns**";
        if (!r.types.empty()) p += " " + TypesMd(r.types);
        if (!r.text.empty()) p += " — " + r.text;
        para(p);
    }
    for (const YardTyped &r : d.raises) {
        std::string p = "**Raises**";
        if (!r.types.empty()) p += " " + TypesMd(r.types);
        if (!r.text.empty()) p += " — " + r.text;
        para(p);
    }
    for (const YardExample &e : d.examples) {
        para("**Example**" + (e.title.empty() ? std::string() : " " + e.title) + "\n```ruby\n" + e.code + "\n```");
    }
    for (const std::string &n : d.notes) para("**Note:** " + n);
    for (const std::string &n : d.todo) para("**TODO:** " + n);
    if (!d.see.empty()) {
        std::string p = "**See also:**";
        for (size_t i = 0; i < d.see.size(); i++) p += std::string(i ? ", " : " ") + d.see[i];
        para(p);
    }
    if (!d.since.empty()) para("*Since " + d.since + "*");
    return s;
}

}  // namespace rblsp
