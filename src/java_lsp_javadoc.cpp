// Javadoc declarations and rendering: see java_lsp_javadoc.h.
#include "java_lsp_javadoc.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

namespace javadoc {

namespace {

namespace fs = std::filesystem;

bool IdentStart(unsigned char c) { return std::isalpha(c) || c == '_' || c == '$' || c >= 0x80; }
bool IdentChar(unsigned char c) { return std::isalnum(c) || c == '_' || c == '$' || c >= 0x80; }

struct Tok {
    enum Kind { Ident, Punct, Literal, Doc } kind = Punct;
    std::string text;
    size_t start = 0, end = 0;
};

// The text of a /** */ comment without its markers: `/**`, `*/` and each line's leading `*`.
std::string StripDocMarkers(const std::string &body) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : body) {
        if (c == '\n') {
            lines.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    lines.push_back(cur);
    for (std::string &l : lines) {
        size_t k = 0;
        while (k < l.size() && (l[k] == ' ' || l[k] == '\t')) k++;
        if (k < l.size() && l[k] == '*') {
            k++;
            if (k < l.size() && l[k] == ' ') k++;
            l = l.substr(k);
        } else {
            // No star: keep the text, minus the comment's own indentation.
            l = l.substr(k);
        }
        while (!l.empty() && (l.back() == ' ' || l.back() == '\t')) l.pop_back();
    }
    while (!lines.empty() && lines.front().empty()) lines.erase(lines.begin());
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    std::string out;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) out += '\n';
        out += lines[i];
    }
    return out;
}

std::vector<Tok> Tokenize(const std::string &s) {
    std::vector<Tok> out;
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (std::isspace(c)) {
            i++;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            const size_t close = s.find("*/", i + 2);
            const size_t end = close == std::string::npos ? n : close + 2;
            const bool doc = i + 2 < n && s[i + 2] == '*' && !(i + 3 < n && s[i + 3] == '/');
            if (doc) {
                Tok t;
                t.kind = Tok::Doc;
                const size_t body_end = close == std::string::npos ? n : close;
                t.text = StripDocMarkers(s.substr(i + 3, body_end > i + 3 ? body_end - i - 3 : 0));
                t.start = i;
                t.end = end;
                out.push_back(t);
            }
            i = end;
            continue;
        }
        if (c == '"' && s.compare(i, 3, "\"\"\"") == 0) {
            size_t k = i + 3;
            while (k < n && s.compare(k, 3, "\"\"\"") != 0) k += s[k] == '\\' ? 2 : 1;
            const size_t end = std::min(n, k + 3);
            out.push_back({Tok::Literal, "\"\"\"", i, end});
            i = end;
            continue;
        }
        if (c == '"' || c == '\'') {
            size_t k = i + 1;
            while (k < n && s[k] != static_cast<char>(c) && s[k] != '\n') k += s[k] == '\\' ? 2 : 1;
            const size_t end = std::min(n, k + 1);
            out.push_back({Tok::Literal, s.substr(i, end - i), i, end});
            i = end;
            continue;
        }
        if (IdentStart(c)) {
            size_t k = i + 1;
            while (k < n && IdentChar(static_cast<unsigned char>(s[k]))) k++;
            out.push_back({Tok::Ident, s.substr(i, k - i), i, k});
            i = k;
            continue;
        }
        if (std::isdigit(c)) {
            size_t k = i + 1;
            while (k < n && (IdentChar(static_cast<unsigned char>(s[k])) || s[k] == '.')) k++;
            out.push_back({Tok::Literal, s.substr(i, k - i), i, k});
            i = k;
            continue;
        }
        out.push_back({Tok::Punct, std::string(1, static_cast<char>(c)), i, i + 1});
        i++;
    }
    return out;
}

bool Is(const Tok &t, const char *text) { return t.kind != Tok::Doc && t.kind != Tok::Literal && t.text == text; }

const std::set<std::string> &Modifiers() {
    static const std::set<std::string> m = {"public", "protected", "private", "static", "final", "abstract",
                                            "native", "synchronized", "transient", "volatile", "strictfp",
                                            "default", "sealed", "non-sealed"};
    return m;
}

class Indexer {
public:
    Indexer(const std::string &text, const std::string &file) : s_(text), file_(file), toks_(Tokenize(text)) {
        for (size_t i = 0; i < s_.size(); i++)
            if (s_[i] == '\n') line_starts_.push_back(i + 1);
    }

    std::vector<Decl> Run() {
        const size_t n = toks_.size();
        size_t i = 0;
        while (i < n) {
            const Tok &t = toks_[i];
            if (t.kind == Tok::Doc) {
                doc_ = t.text;
                i++;
                continue;
            }
            const bool in_type = stack_.empty() || stack_.back().type_body;
            if (!in_type) {
                if (Is(t, "{")) stack_.push_back({});
                if (Is(t, "}")) stack_.pop_back();
                doc_.clear();
                i++;
                continue;
            }
            if (!stack_.empty() && stack_.back().enum_constants) {
                i = EnumConstant(i);
                continue;
            }
            if (Is(t, "}")) {
                if (!stack_.empty()) stack_.pop_back();
                doc_.clear();
                i++;
                continue;
            }
            if (Is(t, ";")) {
                doc_.clear();
                i++;
                continue;
            }
            if (Is(t, "{")) {
                stack_.push_back({});
                doc_.clear();
                i++;
                continue;
            }
            if (stack_.empty() && (Is(t, "package") || Is(t, "import"))) {
                const bool pkg = Is(t, "package");
                std::string name;
                size_t k = i + 1;
                while (k < n && !Is(toks_[k], ";")) {
                    if (toks_[k].kind != Tok::Doc) name += toks_[k].text;
                    k++;
                }
                if (pkg) package_ = name;
                doc_.clear();
                i = k + 1;
                continue;
            }
            i = Member(i);
            doc_.clear();
        }
        return std::move(decls_);
    }

    std::string package() const { return package_; }

private:
    struct Ctx {
        bool type_body = false;
        bool enum_constants = false;
        std::string name;
    };

    const std::string &s_;
    std::string file_;
    std::vector<Tok> toks_;
    std::vector<size_t> line_starts_{0};
    std::vector<Ctx> stack_;
    std::vector<Decl> decls_;
    std::string doc_;
    std::string package_;

    int LineOf(size_t offset) const {
        return static_cast<int>(std::upper_bound(line_starts_.begin(), line_starts_.end(), offset) -
                                line_starts_.begin());
    }

    std::string Owner() const {
        std::string out;
        for (const Ctx &c : stack_) {
            if (!c.type_body) continue;
            if (!out.empty()) out += '.';
            out += c.name;
        }
        return out;
    }

    // Source text of tokens [a, b), comments dropped, white space collapsed to one space.
    std::string Text(size_t a, size_t b) const {
        std::string out;
        for (size_t k = a; k < b; k++) {
            if (toks_[k].kind == Tok::Doc) continue;
            if (!out.empty() && k > a && toks_[k].start > toks_[k - 1].end) out += ' ';
            std::string piece = s_.substr(toks_[k].start, toks_[k].end - toks_[k].start);
            for (char &c : piece)
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            out += piece;
        }
        return out;
    }

    void Add(const std::string &kind, const Tok &name, const std::string &signature) {
        Decl d;
        d.kind = kind;
        d.name = name.text;
        d.owner = Owner();
        d.package = package_;
        d.signature = signature;
        d.doc = doc_;
        d.file = file_;
        d.line = LineOf(name.start);
        decls_.push_back(d);
    }

    // Past an annotation starting at `k` (`@Name`, `@a.b.Name(...)`); `k` itself when there is none.
    size_t SkipAnnotation(size_t k, size_t end) const {
        if (k >= end || !Is(toks_[k], "@") || k + 1 >= end || Is(toks_[k + 1], "interface")) return k;
        k++;
        while (k < end && toks_[k].kind == Tok::Ident) {
            k++;
            if (k + 1 < end && Is(toks_[k], ".") && toks_[k + 1].kind == Tok::Ident) {
                k++;
                continue;
            }
            break;
        }
        if (k < end && Is(toks_[k], "(")) k = SkipBalanced(k, end);
        return k;
    }

    // Past the bracket group opening at `k` (`(`, `[` or `{`).
    size_t SkipBalanced(size_t k, size_t end) const {
        int depth = 0;
        for (; k < end; k++) {
            const Tok &t = toks_[k];
            if (Is(t, "(") || Is(t, "[") || Is(t, "{")) depth++;
            if (Is(t, ")") || Is(t, "]") || Is(t, "}")) {
                if (--depth == 0) return k + 1;
            }
        }
        return end;
    }

    size_t EnumConstant(size_t i) {
        const Tok &t = toks_[i];
        if (Is(t, ";")) {
            stack_.back().enum_constants = false;
            doc_.clear();
            return i + 1;
        }
        if (Is(t, "}")) {
            stack_.pop_back();
            doc_.clear();
            return i + 1;
        }
        if (Is(t, ",")) {
            doc_.clear();
            return i + 1;
        }
        size_t k = SkipAnnotation(i, toks_.size());
        while (k < toks_.size() && toks_[k].kind == Tok::Doc) k++;
        if (k < toks_.size() && toks_[k].kind == Tok::Ident) {
            const size_t name = k++;
            if (k < toks_.size() && Is(toks_[k], "(")) k = SkipBalanced(k, toks_.size());
            Add("constant", toks_[name], Text(name, k));
            if (k < toks_.size() && Is(toks_[k], "{")) k = SkipBalanced(k, toks_.size());
            doc_.clear();
            return k;
        }
        return k == i ? i + 1 : k;
    }

    // A member (or top-level type) whose header starts at token `i`; returns where scanning resumes.
    size_t Member(size_t i) {
        const size_t n = toks_.size();
        // The header: up to `{`, `;` or `=` outside brackets.
        size_t j = i;
        int depth = 0;
        for (; j < n; j++) {
            const Tok &t = toks_[j];
            if (t.kind == Tok::Doc) continue;
            if (Is(t, "(") || Is(t, "[")) depth++;
            else if (Is(t, ")") || Is(t, "]")) depth--;
            else if (depth <= 0 && (Is(t, "{") || Is(t, ";") || Is(t, "=") || Is(t, "}"))) break;
            else if (depth > 0 && Is(t, "{")) j = SkipBalanced(j, n) - 1;
        }
        if (j >= n) return n;
        const Tok &term = toks_[j];
        if (Is(term, "}")) return j;  // a stray header: let the loop close the block
        // Annotations and modifiers off the front.
        size_t h = i;
        for (;;) {
            while (h < j && toks_[h].kind == Tok::Doc) h++;
            const size_t a = SkipAnnotation(h, j);
            if (a != h) {
                h = a;
                continue;
            }
            break;
        }
        const size_t sig_start = h;
        while (h < j && toks_[h].kind == Tok::Ident && Modifiers().count(toks_[h].text)) h++;
        // A type declaration?
        for (size_t k = h; k < j; k++) {
            const Tok &t = toks_[k];
            std::string kind;
            if (Is(t, "class") || Is(t, "interface") || Is(t, "enum")) kind = t.text;
            if (Is(t, "@") && k + 1 < j && Is(toks_[k + 1], "interface")) {
                kind = "annotation";
                k++;
            }
            if (Is(t, "record") && k + 2 < j && toks_[k + 1].kind == Tok::Ident &&
                (Is(toks_[k + 2], "(") || Is(toks_[k + 2], "<")))
                kind = "record";
            if (kind.empty()) continue;
            if (k + 1 < j && toks_[k + 1].kind == Tok::Ident) {
                Add(kind, toks_[k + 1], Text(sig_start, j));
                if (Is(term, "{")) {
                    Ctx c;
                    c.type_body = true;
                    c.enum_constants = kind == "enum";
                    c.name = toks_[k + 1].text;
                    stack_.push_back(c);
                }
            } else if (Is(term, "{")) {
                stack_.push_back({});
            }
            return j + 1;
        }
        // A method or constructor: the identifier before the first `(` at depth 0.
        for (size_t k = h; k < j; k++) {
            if (!Is(toks_[k], "(")) continue;
            if (k > h && toks_[k - 1].kind == Tok::Ident) {
                const std::string owner = stack_.empty() ? "" : stack_.back().name;
                const bool ctor = toks_[k - 1].text == owner && k - 1 == h;
                Add(ctor ? "constructor" : "method", toks_[k - 1], Text(sig_start, j));
            }
            if (Is(term, "{")) stack_.push_back({});
            if (Is(term, "=")) return SkipInitializer(j + 1, 0, 0, "");
            return j + 1;
        }
        if (Is(term, "{")) {  // an initializer block, a compact record constructor
            stack_.push_back({});
            return j + 1;
        }
        // Fields: `Type a = 1, b[], c;` -- declarators at angle depth 0.
        int angle = 0;
        size_t type_end = j;
        std::vector<size_t> names;
        for (size_t k = h; k < j; k++) {
            const Tok &t = toks_[k];
            if (Is(t, "<")) angle++;
            if (Is(t, ">")) angle--;
            if (t.kind != Tok::Ident || angle != 0) continue;
            const bool last = k + 1 == j || Is(toks_[k + 1], ",") || Is(toks_[k + 1], "[");
            if (last && k > h) {
                if (names.empty()) type_end = k;
                names.push_back(k);
            }
        }
        if (names.empty()) return j + 1;
        const std::string type = Text(sig_start, type_end);
        for (size_t k : names) Add("field", toks_[k], type + " " + toks_[k].text);
        if (Is(term, "=")) return SkipInitializer(j + 1, sig_start, type_end, type);
        return j + 1;
    }

    // Past a field's initializer up to its `;`; a `,` at depth 0 starts another declarator.
    size_t SkipInitializer(size_t k, size_t, size_t, const std::string &type) {
        const size_t n = toks_.size();
        int depth = 0;
        for (; k < n; k++) {
            const Tok &t = toks_[k];
            if (Is(t, "(") || Is(t, "[") || Is(t, "{")) depth++;
            else if (Is(t, ")") || Is(t, "]") || Is(t, "}")) {
                if (--depth < 0) return k;  // malformed: let the caller close the block
            } else if (depth == 0 && Is(t, ";")) {
                return k + 1;
            } else if (depth == 0 && Is(t, ",") && !type.empty() && k + 1 < n && toks_[k + 1].kind == Tok::Ident) {
                Add("field", toks_[k + 1], type + " " + toks_[k + 1].text);
            }
        }
        return n;
    }
};

std::string ReadAll(const std::string &path, bool *ok) {
    std::ifstream in(path, std::ios::binary);
    *ok = static_cast<bool>(in);
    if (!in) return std::string();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> NameParts(std::string name) {
    const size_t paren = name.find('(');
    if (paren != std::string::npos) name = name.substr(0, paren);
    std::vector<std::string> parts;
    std::string cur;
    for (char c : name) {
        if (c == '.' || c == '#') {
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            cur += c;
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

std::string Trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

std::string DecodeEntities(const std::string &s) {
    static const std::pair<const char *, const char *> kEntities[] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"},
        {"&#64;", "@"}, {"&nbsp;", " "}, {"&#47;", "/"}, {"&#42;", "*"}, {"&#123;", "{"}, {"&#125;", "}"},
    };
    std::string out;
    for (size_t i = 0; i < s.size();) {
        bool hit = false;
        if (s[i] == '&') {
            for (const auto &e : kEntities) {
                const size_t len = std::char_traits<char>::length(e.first);
                if (s.compare(i, len, e.first) == 0) {
                    out += e.second;
                    i += len;
                    hit = true;
                    break;
                }
            }
        }
        if (!hit) out += s[i++];
    }
    return out;
}

// `{@link A#b(int) label}` -> label, else `A.b(int)`.
std::string LinkText(const std::string &arg) {
    std::string a = Trim(arg);
    size_t depth = 0, split = std::string::npos;
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i] == '(') depth++;
        if (a[i] == ')' && depth) depth--;
        if (depth == 0 && std::isspace(static_cast<unsigned char>(a[i]))) {
            split = i;
            break;
        }
    }
    if (split != std::string::npos) return Trim(a.substr(split + 1));
    if (!a.empty() && a[0] == '#') a = a.substr(1);
    for (char &c : a)
        if (c == '#') c = '.';
    return "`" + a + "`";
}

// Inline tags and HTML of a piece of Javadoc as Markdown.
std::string Inline(const std::string &s) {
    std::string out;
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        if (s[i] == '{' && i + 1 < n && s[i + 1] == '@') {
            // {@tag arg}, braces inside balanced.
            size_t k = i + 2, depth = 1;
            while (k < n && depth) {
                if (s[k] == '{') depth++;
                if (s[k] == '}') depth--;
                if (depth) k++;
            }
            const std::string inner = s.substr(i + 2, k - i - 2);
            size_t sp = 0;
            while (sp < inner.size() && !std::isspace(static_cast<unsigned char>(inner[sp]))) sp++;
            const std::string tag = inner.substr(0, sp);
            const std::string arg = sp < inner.size() ? inner.substr(sp + 1) : "";
            if (tag == "code") out += "`" + Trim(arg) + "`";
            else if (tag == "literal") out += Trim(arg);
            else if (tag == "link" || tag == "linkplain") out += LinkText(arg);
            else if (tag == "value") out += arg.empty() ? "(value)" : "`" + Trim(arg) + "`";
            else if (tag == "inheritDoc") out += "(inherited documentation)";
            else if (tag == "index") out += Trim(arg);
            else if (tag == "summary") out += Inline(arg);
            else out += Trim(arg);
            i = std::min(n, k + 1);
            continue;
        }
        if (s[i] == '<') {
            const size_t close = s.find('>', i);
            if (close != std::string::npos) {
                std::string tag = s.substr(i + 1, close - i - 1);
                bool end = !tag.empty() && tag[0] == '/';
                if (end) tag = tag.substr(1);
                size_t sp = 0;
                while (sp < tag.size() && std::isalnum(static_cast<unsigned char>(tag[sp]))) sp++;
                std::string name = tag.substr(0, sp);
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const bool known = !name.empty();
                if (known) {
                    if (name == "p") out += end ? "" : "\n\n";
                    else if (name == "br") out += "  \n";
                    else if (name == "code" || name == "tt") out += "`";
                    else if (name == "b" || name == "strong") out += "**";
                    else if (name == "i" || name == "em" || name == "var") out += "*";
                    else if (name == "li") out += end ? "" : "\n- ";
                    else if (name == "ul" || name == "ol" || name == "dl") out += "\n";
                    else if (name == "dt") out += end ? "" : "\n- ";
                    else if (name == "dd") out += end ? "" : ": ";
                    else if (name.size() == 2 && name[0] == 'h' && std::isdigit(static_cast<unsigned char>(name[1])))
                        out += end ? "\n\n" : "\n\n**";
                    // other tags (a, span, table...) drop out, their text stays
                    if (end && name.size() == 2 && name[0] == 'h' && std::isdigit(static_cast<unsigned char>(name[1])))
                        out.insert(out.size() - 2, "**");
                    i = close + 1;
                    continue;
                }
            }
        }
        out += s[i++];
    }
    return DecodeEntities(out);
}

// The description: <pre> blocks become fenced code, the rest goes through Inline.
std::string Description(const std::string &s) {
    std::string out;
    size_t i = 0;
    for (;;) {
        size_t pre = std::string::npos;
        for (size_t k = i; k + 4 <= s.size(); k++) {
            if (s[k] == '<' && (s.compare(k, 4, "<pre") == 0 || s.compare(k, 4, "<PRE") == 0) &&
                (k + 4 == s.size() || s[k + 4] == '>' || s[k + 4] == ' ')) {
                pre = k;
                break;
            }
        }
        if (pre == std::string::npos) {
            out += Inline(s.substr(i));
            break;
        }
        out += Inline(s.substr(i, pre - i));
        const size_t body = s.find('>', pre);
        size_t close = s.find("</pre>", body);
        if (close == std::string::npos) close = s.find("</PRE>", body);
        if (body == std::string::npos) break;
        std::string code = s.substr(body + 1, (close == std::string::npos ? s.size() : close) - body - 1);
        std::string t = Trim(code);
        if (t.compare(0, 6, "{@code") == 0 && !t.empty() && t.back() == '}') {
            code = t.substr(6, t.size() - 7);
        }
        while (!code.empty() && code[0] == '\n') code.erase(0, 1);
        while (!code.empty() && (code.back() == '\n' || code.back() == ' ')) code.pop_back();
        out += "\n\n```java\n" + DecodeEntities(code) + "\n```\n\n";
        if (close == std::string::npos) break;
        i = close + 6;
    }
    // Tidy: at most one blank line in a row, no trailing space at the ends.
    std::string tidy;
    int newlines = 0;
    for (char c : out) {
        if (c == '\n') {
            if (++newlines > 2) continue;
        } else {
            newlines = 0;
        }
        tidy += c;
    }
    return Trim(tidy);
}

bool PathExists(const std::string &p, bool want_dir) {
    std::error_code ec;
    return want_dir ? fs::is_directory(p, ec) : fs::is_regular_file(p, ec);
}

void JavaFilesUnder(const std::string &root, std::vector<std::string> *out, size_t limit) {
    std::error_code ec;
    if (fs::is_regular_file(root, ec)) {
        out->push_back(root);
        return;
    }
    if (!fs::is_directory(root, ec)) return;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end && !ec; it.increment(ec)) {
        const fs::path &p = it->path();
        const std::string base = p.filename().string();
        if (it->is_directory(ec)) {
            if (!base.empty() && (base[0] == '.' || base == "node_modules")) it.disable_recursion_pending();
            continue;
        }
        if (p.extension() == ".java") {
            out->push_back(p.string());
            if (out->size() >= limit) return;
        }
    }
}

bool ContainsWord(const std::string &text, const std::string &word) {
    for (size_t pos = text.find(word); pos != std::string::npos; pos = text.find(word, pos + 1)) {
        const bool left = pos == 0 || !IdentChar(static_cast<unsigned char>(text[pos - 1]));
        const size_t e = pos + word.size();
        const bool right = e >= text.size() || !IdentChar(static_cast<unsigned char>(text[e]));
        if (left && right) return true;
    }
    return false;
}

}  // namespace

std::string Decl::Qualified() const {
    std::string out = package;
    for (const std::string *p : {&owner, &name}) {
        if (p->empty()) continue;
        if (!out.empty()) out += '.';
        out += *p;
    }
    return out;
}

std::vector<Decl> IndexSource(const std::string &text, const std::string &file) { return Indexer(text, file).Run(); }

std::string PackageOf(const std::string &text) {
    const std::vector<Tok> toks = Tokenize(text);
    for (size_t i = 0; i < toks.size(); i++) {
        if (toks[i].kind == Tok::Doc) continue;
        if (toks[i].text == "@") {  // package annotations (package-info.java)
            continue;
        }
        if (Is(toks[i], "package")) {
            std::string name;
            for (size_t k = i + 1; k < toks.size() && !Is(toks[k], ";"); k++) name += toks[k].text;
            return name;
        }
        if (Is(toks[i], "import") || Is(toks[i], "class") || Is(toks[i], "interface") || Is(toks[i], "public"))
            return "";
    }
    return "";
}

std::string RenderDoc(const std::string &doc) {
    // Split the description from the block tags (a line starting with @word).
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : doc) {
            if (c == '\n') {
                lines.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        lines.push_back(cur);
    }
    std::string description;
    std::vector<std::pair<std::string, std::string>> tags;  // name, text
    bool in_pre = false;
    for (const std::string &l : lines) {
        const std::string t = Trim(l);
        if (l.find("<pre") != std::string::npos) in_pre = true;
        if (!in_pre && t.size() > 1 && t[0] == '@' && std::isalpha(static_cast<unsigned char>(t[1]))) {
            size_t sp = 1;
            while (sp < t.size() && !std::isspace(static_cast<unsigned char>(t[sp]))) sp++;
            tags.push_back({t.substr(1, sp - 1), sp < t.size() ? Trim(t.substr(sp)) : ""});
        } else if (!tags.empty()) {
            tags.back().second += (tags.back().second.empty() ? "" : "\n") + l;
        } else {
            description += l + "\n";
        }
        if (l.find("</pre>") != std::string::npos) in_pre = false;
    }
    std::string out;
    auto section = [&out](const std::string &text) {
        if (text.empty()) return;
        if (!out.empty()) out += "\n\n";
        out += text;
    };
    auto one_line = [](const std::string &s) {
        std::string r = Inline(Trim(s));
        for (char &c : r)
            if (c == '\n') c = ' ';
        return r;
    };
    auto first_word = [](const std::string &s, std::string *rest) {
        const std::string t = Trim(s);
        size_t sp = 0;
        int angle = 0;
        while (sp < t.size() && (angle > 0 || !std::isspace(static_cast<unsigned char>(t[sp])))) {
            if (t[sp] == '<') angle++;
            if (t[sp] == '>') angle--;
            sp++;
        }
        *rest = sp < t.size() ? Trim(t.substr(sp)) : "";
        return t.substr(0, sp);
    };
    for (const auto &tg : tags)
        if (tg.first == "deprecated") section("**Deprecated.** " + one_line(tg.second));
    section(Description(description));
    std::string params, throws, see, extra;
    std::string returns, since;
    for (const auto &tg : tags) {
        std::string rest;
        if (tg.first == "param") {
            std::string name = first_word(tg.second, &rest);
            params += "\n- `" + name + "`" + (rest.empty() ? "" : " -- " + one_line(rest));
        } else if (tg.first == "return") {
            returns = one_line(tg.second);
        } else if (tg.first == "throws" || tg.first == "exception") {
            std::string name = first_word(tg.second, &rest);
            throws += "\n- `" + name + "`" + (rest.empty() ? "" : " -- " + one_line(rest));
        } else if (tg.first == "see") {
            std::string ref = Trim(tg.second);
            if (!ref.empty() && ref[0] != '"' && ref[0] != '<') ref = LinkText(ref);
            else ref = one_line(ref);
            see += (see.empty() ? "" : ", ") + ref;
        } else if (tg.first == "since") {
            since = one_line(tg.second);
        } else if (tg.first == "apiNote" || tg.first == "implNote" || tg.first == "implSpec") {
            const char *title = tg.first == "apiNote" ? "API Note" : tg.first == "implNote" ? "Implementation Note"
                                                                                           : "Implementation Requirements";
            extra += std::string(extra.empty() ? "" : "\n\n") + "**" + title + ":** " + Description(tg.second);
        } else if (tg.first != "deprecated" && tg.first != "author" && tg.first != "version" &&
                   tg.first != "serial" && tg.first != "serialField" && tg.first != "serialData" &&
                   tg.first != "hidden") {
            extra += std::string(extra.empty() ? "" : "\n\n") + "**@" + tg.first + "** " + one_line(tg.second);
        }
    }
    if (!params.empty()) section("**Parameters:**" + params);
    if (!returns.empty()) section("**Returns:** " + returns);
    if (!throws.empty()) section("**Throws:**" + throws);
    section(extra);
    if (!see.empty()) section("**See also:** " + see);
    if (!since.empty()) section("**Since:** " + since);
    return out;
}

std::string RenderDecl(const Decl &d, bool with_location) {
    std::string out = "```java\n" + d.signature + "\n```";
    std::string where;
    const std::string owner = d.owner.empty() ? d.package : (d.package.empty() ? d.owner : d.package + "." + d.owner);
    if (!owner.empty()) where = d.kind + " in `" + owner + "`";
    else where = d.kind;
    out += "\n\n" + where;
    const std::string body = RenderDoc(d.doc);
    if (!body.empty()) out += "\n\n" + body;
    if (with_location && !d.file.empty()) out += "\n\n" + d.file + ":" + std::to_string(d.line);
    return out;
}

bool Matches(const Decl &d, const std::string &name) {
    const std::vector<std::string> q = NameParts(name);
    if (q.empty()) return false;
    std::vector<std::string> comps = NameParts(d.package);
    for (const std::string &p : NameParts(d.owner)) comps.push_back(p);
    comps.push_back(d.name);
    if (q.size() > comps.size()) return false;
    return std::equal(q.rbegin(), q.rend(), comps.rbegin());
}

std::string PackageRoot(const std::string &file, const std::string &text) {
    const std::string pkg = PackageOf(text);
    fs::path dir = fs::path(file).parent_path();
    if (dir.empty()) dir = ".";
    if (pkg.empty()) return dir.string();
    std::vector<std::string> parts = NameParts(pkg);
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (dir.filename().string() != *it) return "";
        dir = dir.parent_path();
    }
    return dir.empty() ? "." : dir.string();
}

std::vector<std::string> SplitPathList(const std::string &list) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : list) {
#if defined(_WIN32)
        const bool sep = c == ';';
#else
        const bool sep = c == ':';
#endif
        if (sep) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::vector<Decl> Find(const std::string &name, const SearchPath &where, size_t max_results) {
    std::vector<Decl> found;
    const std::vector<std::string> q = NameParts(name);
    if (q.empty()) return found;
    std::set<std::string> seen_files, seen_decls;
    auto take_file = [&](const std::string &path, bool must_contain) {
        std::error_code ec;
        std::string key = fs::weakly_canonical(path, ec).string();
        if (ec || key.empty()) key = path;
        if (!seen_files.insert(key).second) return;
        bool ok = false;
        const std::string text = ReadAll(path, &ok);
        if (!ok) return;
        if (must_contain && !ContainsWord(text, q.back())) return;
        for (Decl &d : IndexSource(text, path)) {
            if (!Matches(d, name)) continue;
            if (!seen_decls.insert(d.Qualified() + "\n" + d.signature).second) continue;
            found.push_back(std::move(d));
        }
    };
    for (const std::string &f : where.files) take_file(f, false);
    // A qualified name: its package's path under each root first.
    for (const std::string &root : where.sourcepath) {
        if (!PathExists(root, true)) continue;
        std::string rel;
        for (size_t k = 0; k < q.size(); k++) {
            rel += (k ? "/" : "") + q[k];
            const std::string candidate = root + "/" + rel + ".java";
            if (PathExists(candidate, false)) take_file(candidate, false);
        }
    }
    if (found.empty()) {
        for (const std::string &root : where.sourcepath) {
            std::vector<std::string> files;
            JavaFilesUnder(root, &files, 50000);
            // Files named after a part of the query first: DataFrame.java for DataFrame.filter.
            std::stable_partition(files.begin(), files.end(), [&q](const std::string &f) {
                const std::string stem = fs::path(f).stem().string();
                return std::find(q.begin(), q.end(), stem) != q.end();
            });
            for (const std::string &f : files) take_file(f, true);
        }
    }
    auto rank = [](const Decl &d) {
        if (d.kind == "class" || d.kind == "interface" || d.kind == "enum" || d.kind == "record" ||
            d.kind == "annotation")
            return 0;
        return d.doc.empty() ? 2 : 1;
    };
    std::stable_sort(found.begin(), found.end(), [&rank](const Decl &a, const Decl &b) { return rank(a) < rank(b); });
    if (found.size() > max_results) found.resize(max_results);
    return found;
}

}  // namespace javadoc
