// mep's COBOL language server, analysis half: see cobol_lsp.h.
//
// One pass per file (the document, and each copybook it COPYs):
// cobol_syntax.h splits each line into its code area and comment, the code
// is tokenized (words, numbers, literals, separator periods), and the
// token stream is cut into sentences at separator periods. A sentence
// that starts with a level number outside the PROCEDURE DIVISION is a data
// description entry; one that is a lone word (or `word SECTION`) inside
// it is a paragraph (section) header. `*>>` doc lines are collected as
// they go by and attach to the entry or header that starts the next code
// line. Nothing here needs cobc.
#include "cobol_lsp.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>

#include "cobol_syntax.h"

namespace {

namespace fs = std::filesystem;

struct Token {
    enum Kind { Word, Number, String, Period, Other } kind = Other;
    std::string text;   // as written (a literal with its quotes)
    std::string value;  // a literal's content; a word upper-cased
    int line = 0, col = 0, end_col = 0;
    bool first_on_line = false;
};

struct Item {
    std::string name;  // as written
    std::string key;   // upper case
    int level = 0;     // 1-49, 66, 77, 78, 88; 0 paragraph, -1 section, -2 FD/SD
    int line = 0, col = 0, end_col = 0;
    int end_line = 0;  // the line of the entry's period (paragraphs: the line before the next header)
    std::string decl;     // the entry as written, one line, comments dropped
    std::string clauses;  // what follows the name, without the period
    std::vector<std::string> doc;
    int doc_line = -1;  // first line of the doc block
    std::string call_name;           // the first CALL line's program
    std::vector<std::string> calls;  // every CALL line's program
    bool doc_shared = false;         // an undocumented 78 showing the enum block above it
    int parent = -1;
    std::vector<int> children;
};

struct CopyRef {
    std::string name;  // as written, quotes stripped
    int line = 0, col = 0, end_col = 0;
    int kw_line = 0, kw_col = 0, kw_end_col = 0;  // the COPY keyword
    std::string resolved;  // filled when a document is analysed
};

struct Unit {
    std::string path;
    std::vector<std::string> lines;
    std::vector<Token> tokens;
    std::vector<Item> items;
    std::vector<CopyRef> copies;
    std::vector<int> divisions;                   // lines of DIVISION headers
    std::vector<std::pair<int, int>> doc_blocks;  // first, last line
    int last_code_line = 0;
    // Built once per parse (a cached copybook keeps its index too).
    std::unordered_map<std::string, std::vector<int>> key_index;  // upper-case name -> items
    std::unordered_map<std::string, int> call_index;              // lower-case CALL program -> its record
    std::vector<std::pair<std::string, int>> calls;               // every CALL program as written, in order
};

// --- small helpers ---------------------------------------------------------------------------------------------

std::string Lower(const std::string &s) {
    std::string out = s;
    for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string Trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

std::string Basename(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string DirOf(const std::string &path) {
    if (path.empty()) return std::string();
    std::error_code ec;
    fs::path p = fs::absolute(fs::path(path), ec);
    if (ec) p = fs::path(path);
    return p.parent_path().string();
}

// Runs of blanks outside literals become one space.
std::string Squeeze(const std::string &s) {
    std::string out;
    char quote = 0;
    for (char c : s) {
        if (quote) {
            out += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') quote = c;
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!out.empty() && out.back() != ' ') out += ' ';
        } else {
            out += c;
        }
    }
    return Trim(out);
}

bool ValidLevel(const std::string &digits) {
    if (digits.empty() || digits.size() > 2) return false;
    for (char c : digits) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    const int n = std::stoi(digits);
    return (n >= 1 && n <= 49) || n == 66 || n == 77 || n == 78 || n == 88;
}

std::string Join(const std::vector<std::string> &v, const std::string &sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); i++) out += (i ? sep : "") + v[i];
    return out;
}

// --- tokenizing ------------------------------------------------------------------------------------------------

void TokenizeLine(const std::string &line, int row, const CobolLineInfo &info, std::vector<Token> *out) {
    int i = info.code_start;
    const int end = info.code_end;
    bool first = true;
    while (i < end) {
        const char c = line[static_cast<size_t>(i)];
        if (std::isspace(static_cast<unsigned char>(c))) {
            i++;
            continue;
        }
        Token t;
        t.line = row;
        t.col = i;
        t.first_on_line = first;
        first = false;
        const bool prefixed = std::string("XxNnZzBbHhGg").find(c) != std::string::npos && i + 1 < end &&
                              (line[static_cast<size_t>(i) + 1] == '"' || line[static_cast<size_t>(i) + 1] == '\'');
        if (c == '"' || c == '\'' || prefixed) {
            int j = prefixed ? i + 1 : i;
            const char q = line[static_cast<size_t>(j)];
            j++;
            std::string value;
            while (j < end) {
                const char d = line[static_cast<size_t>(j)];
                if (d == q) {
                    if (j + 1 < end && line[static_cast<size_t>(j) + 1] == q) {
                        value += q;
                        j += 2;
                        continue;
                    }
                    j++;
                    break;
                }
                value += d;
                j++;
            }
            t.kind = Token::String;
            t.value = value;
            t.end_col = j;
        } else if (c == '.' && (i + 1 >= end || std::isspace(static_cast<unsigned char>(line[static_cast<size_t>(i) + 1])))) {
            t.kind = Token::Period;
            t.end_col = i + 1;
        } else if (CobolWordChar(c)) {
            int j = i + 1;
            while (j < end && CobolWordChar(line[static_cast<size_t>(j)])) j++;
            bool digits = true;
            for (int k = i; k < j; k++) digits = digits && std::isdigit(static_cast<unsigned char>(line[static_cast<size_t>(k)]));
            if (digits && j + 1 < end && (line[static_cast<size_t>(j)] == '.' || line[static_cast<size_t>(j)] == ',') &&
                std::isdigit(static_cast<unsigned char>(line[static_cast<size_t>(j) + 1]))) {
                j++;
                while (j < end && std::isdigit(static_cast<unsigned char>(line[static_cast<size_t>(j)]))) j++;
            }
            t.kind = digits ? Token::Number : Token::Word;
            t.end_col = j;
        } else {
            t.kind = Token::Other;
            t.end_col = i + 1;
        }
        t.text = line.substr(static_cast<size_t>(t.col), static_cast<size_t>(t.end_col - t.col));
        if (t.kind == Token::Word) t.value = CobolUpper(t.text);
        if (t.kind == Token::Number) t.value = t.text;
        out->push_back(std::move(t));
        i = out->back().end_col;
    }
}

// The source text of tokens [a, b], one line, comments dropped, spacing as written (squeezed).
std::string SourceText(const Unit &u, size_t a, size_t b) {
    std::string out;
    int prev_line = -1, prev_end = -1;
    for (size_t k = a; k <= b && k < u.tokens.size(); k++) {
        const Token &t = u.tokens[k];
        if (prev_line >= 0 && (t.line != prev_line || t.col > prev_end)) out += ' ';
        out += t.text;
        prev_line = t.line;
        prev_end = t.end_col;
    }
    return Squeeze(out);
}

}  // namespace

// --- doc blocks --------------------------------------------------------------------------------------------------

namespace {

bool StartsWithWordCI(const std::string &t, const char *word) {
    const size_t n = std::char_traits<char>::length(word);
    if (t.size() < n || CobolUpper(t.substr(0, n)) != word) return false;
    return t.size() == n || t[n] == ' ' || t[n] == '"' || t[n] == '\'';
}

bool IsListItem(const std::string &t) {
    if (t.size() >= 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') return true;
    size_t i = 0;
    while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) i++;
    return i > 0 && i + 1 < t.size() && (t[i] == '.' || t[i] == ')') && t[i + 1] == ' ';
}

}  // namespace

std::vector<std::string> CobolLspUnwrapDoc(const std::vector<std::string> &doc) {
    std::vector<std::string> out;
    bool fence = false;
    for (const std::string &raw : doc) {
        const std::string t = Trim(raw);
        const bool indented = raw.size() >= 2 && raw[0] == ' ' && raw[1] == ' ';
        if (!fence && indented && !t.empty() && !out.empty() && !Trim(out.back()).empty() && !IsListItem(t) &&
            Trim(out.back()).compare(0, 3, "```") != 0) {
            out.back() += " " + t;
            continue;
        }
        if (t.compare(0, 3, "```") == 0) fence = !fence;
        out.push_back(raw);
    }
    return out;
}

CobolLspDocBlock CobolLspSplitDoc(const std::vector<std::string> &doc) {
    CobolLspDocBlock out;
    const std::vector<std::string> lines = CobolLspUnwrapDoc(doc);
    size_t head_end = 0;  // the head: up to the first blank line
    while (head_end < lines.size() && !Trim(lines[head_end]).empty()) head_end++;
    bool has_head = false;
    for (size_t i = 0; i < head_end; i++) {
        const std::string t = Trim(lines[i]);
        if (StartsWithWordCI(t, "CALL") || CobolUpper(t).compare(0, 25, "PROCEDURE DIVISION USING ") == 0) has_head = true;
    }
    size_t body_from = 0;
    if (has_head) {
        std::vector<std::string> extra;  // head lines after the CALLs that are not CALLs: body
        for (size_t i = 0; i < head_end; i++) {
            const std::string t = Trim(lines[i]);
            if (StartsWithWordCI(t, "CALL")) {
                const size_t q1 = t.find_first_of("\"'");
                const size_t q2 = q1 == std::string::npos ? std::string::npos : t.find(t[q1], q1 + 1);
                if (q2 != std::string::npos) {
                    out.calls.push_back(CobolLspDocCall{t, t.substr(q1 + 1, q2 - q1 - 1)});
                    continue;
                }
            }
            if (CobolUpper(t).compare(0, 25, "PROCEDURE DIVISION USING ") == 0) {
                out.using_line = t;
            } else if (out.calls.empty() && out.using_line.empty()) {
                out.signature.push_back(t);
            } else {
                extra.push_back(t);
            }
        }
        out.body = extra;
        body_from = head_end;
    }
    for (size_t i = body_from; i < lines.size(); i++) {
        if (out.body.empty() && Trim(lines[i]).empty()) continue;
        out.body.push_back(lines[i]);
    }
    while (!out.body.empty() && Trim(out.body.back()).empty()) out.body.pop_back();
    if (!out.calls.empty()) {
        out.call_line = out.calls.front().line;
        out.call_name = out.calls.front().name;
    }
    return out;
}

namespace {

Unit ParseUnit(const std::vector<std::string> &lines, const std::string &path) {
    Unit u;
    u.path = path;
    u.lines = lines;
    const std::vector<CobolFormat> formats = CobolLineFormats(lines);
    std::map<int, std::pair<std::vector<std::string>, int>> doc_before;  // first code line -> doc block
    std::vector<std::string> pending;
    int pending_first = -1, pending_last = -1;
    for (size_t row = 0; row < lines.size(); row++) {
        const std::string &line = lines[row];
        std::string text;
        if (CobolDocCommentText(line, &text)) {
            if (pending.empty()) pending_first = static_cast<int>(row);
            pending.push_back(text);
            pending_last = static_cast<int>(row);
            continue;
        }
        const CobolLineInfo info = CobolClassifyLine(line, formats[row]);
        if (info.directive) continue;
        const size_t before = u.tokens.size();
        if (!info.comment_line) TokenizeLine(line, static_cast<int>(row), info, &u.tokens);
        if (u.tokens.size() > before) {
            u.last_code_line = static_cast<int>(row);
            if (!pending.empty()) {
                doc_before[static_cast<int>(row)] = {pending, pending_first};
                u.doc_blocks.push_back({pending_first, pending_last});
                pending.clear();
            }
        }
    }
    if (!pending.empty()) u.doc_blocks.push_back({pending_first, pending_last});

    auto doc_for = [&doc_before](const Token &t, Item *item) {
        if (!t.first_on_line) return;
        const auto it = doc_before.find(t.line);
        if (it == doc_before.end()) return;
        item->doc = it->second.first;
        item->doc_line = it->second.second;
        const CobolLspDocBlock d = CobolLspSplitDoc(item->doc);
        item->call_name = d.call_name;
        for (const CobolLspDocCall &c : d.calls) item->calls.push_back(c.name);
    };

    bool procedure = false;
    std::vector<int> stack;  // open group items (levels 1-49)
    int last_data = -1;      // the latest level 1-49 / 77 item (an 88's parent)
    int last_section = -1;
    std::vector<int> headers;  // paragraphs and sections, in order
    static const std::set<std::string> kNotLevelAfter = {"VALUE", "VALUES", "IS", "ARE", "TO", "THRU", "THROUGH",
                                                         "OCCURS", "TIMES", "ALL", "FROM", "BY"};
    const size_t n = u.tokens.size();
    size_t i = 0;
    while (i < n) {
        // One sentence: [i, j), then the period at j if there is one. A level number that starts a
        // line also opens a new sentence when the period before it was forgotten.
        size_t j = i;
        while (j < n && u.tokens[j].kind != Token::Period) {
            if (j > i && !procedure && u.tokens[j].first_on_line && u.tokens[j].kind == Token::Number &&
                ValidLevel(u.tokens[j].text) &&
                !(u.tokens[j - 1].kind == Token::Word && kNotLevelAfter.count(u.tokens[j - 1].value))) {
                break;
            }
            j++;
        }
        if (j == i) {  // a stray period
            i = j + 1;
            continue;
        }
        const size_t last = j < n ? j : j - 1;  // the period, or the sentence's last token
        for (size_t k = i; k + 1 < j; k++) {
            const Token &a = u.tokens[k];
            const Token &b = u.tokens[k + 1];
            if (a.kind == Token::Word && a.value == "COPY" && (b.kind == Token::Word || b.kind == Token::String)) {
                CopyRef c;
                c.name = b.kind == Token::String ? b.value : b.text;
                c.line = b.line;
                c.col = b.col;
                c.end_col = b.end_col;
                c.kw_line = a.line;
                c.kw_col = a.col;
                c.kw_end_col = a.end_col;
                u.copies.push_back(c);
            }
            if (a.kind == Token::Word && b.kind == Token::Word && b.value == "DIVISION") {
                procedure = a.value == "PROCEDURE";
                u.divisions.push_back(a.line);
                stack.clear();
            }
            if (a.kind == Token::Word && a.value == "END" && b.kind == Token::Word &&
                (b.value == "PROGRAM" || b.value == "FUNCTION")) {
                procedure = false;
            }
        }
        const Token &t0 = u.tokens[i];
        const bool division = j > i + 1 && u.tokens[i + 1].kind == Token::Word && u.tokens[i + 1].value == "DIVISION";
        if (!procedure && t0.kind == Token::Number && ValidLevel(t0.text)) {
            Item item;
            item.level = std::stoi(t0.text);
            item.line = t0.line;
            item.col = t0.col;
            item.end_col = t0.end_col;
            const size_t name_at = i + 1;
            bool named = false;
            if (name_at < j && u.tokens[name_at].kind == Token::Word) {
                const Token &nt = u.tokens[name_at];
                if (nt.value == "FILLER") {
                    item.name = "FILLER";
                    named = true;
                } else if (!CobolIsKeyword(nt.value)) {
                    item.name = nt.text;
                    item.key = nt.value;
                    item.line = nt.line;
                    item.col = nt.col;
                    item.end_col = nt.end_col;
                    named = true;
                }
            }
            item.end_line = u.tokens[last].line;
            item.decl = SourceText(u, i, last);
            const size_t clause_from = named ? name_at + 1 : i + 1;
            const size_t clause_to = j - 1;  // the token before the period
            if (clause_from <= clause_to && clause_to < n) item.clauses = SourceText(u, clause_from, clause_to);
            doc_for(t0, &item);
            const int idx = static_cast<int>(u.items.size());
            if (item.level >= 1 && item.level <= 49) {
                while (!stack.empty() && u.items[static_cast<size_t>(stack.back())].level >= item.level) stack.pop_back();
                item.parent = stack.empty() ? -1 : stack.back();
                stack.push_back(idx);
                last_data = idx;
            } else if (item.level == 88) {
                item.parent = last_data;
            } else if (item.level == 66) {
                item.parent = stack.empty() ? -1 : stack.front();
            } else {  // 77, 78
                stack.clear();
                last_data = item.level == 77 ? idx : -1;
            }
            if (item.parent >= 0) u.items[static_cast<size_t>(item.parent)].children.push_back(idx);
            u.items.push_back(std::move(item));
        } else if (!procedure && t0.kind == Token::Word && (t0.value == "FD" || t0.value == "SD") && i + 1 < j &&
                   u.tokens[i + 1].kind == Token::Word) {
            const Token &nt = u.tokens[i + 1];
            Item item;
            item.level = -2;
            item.name = nt.text;
            item.key = nt.value;
            item.line = nt.line;
            item.col = nt.col;
            item.end_col = nt.end_col;
            item.end_line = u.tokens[last].line;
            item.decl = SourceText(u, i, last);
            doc_for(t0, &item);
            stack.clear();
            u.items.push_back(std::move(item));
        } else if (procedure && !division && t0.kind == Token::Word && t0.first_on_line && !CobolIsKeyword(t0.value) &&
                   (j == i + 1 || (j == i + 2 && u.tokens[i + 1].kind == Token::Word && u.tokens[i + 1].value == "SECTION"))) {
            Item item;
            item.level = j == i + 1 ? 0 : -1;
            item.name = t0.text;
            item.key = t0.value;
            item.line = t0.line;
            item.col = t0.col;
            item.end_col = t0.end_col;
            item.decl = SourceText(u, i, last);
            doc_for(t0, &item);
            const int idx = static_cast<int>(u.items.size());
            if (item.level == -1) {
                last_section = idx;
            } else {
                item.parent = last_section;
                if (last_section >= 0) u.items[static_cast<size_t>(last_section)].children.push_back(idx);
            }
            headers.push_back(idx);
            u.items.push_back(std::move(item));
        }
        i = j < n ? j + 1 : j;
    }
    // Rosetta documents an enum once, above its first 78 constant: the run of undocumented 78s right
    // after it (no blank line, no other entry between) shares that block.
    for (size_t k = 1; k < u.items.size(); k++) {
        Item &it = u.items[k];
        const Item &prev = u.items[k - 1];
        if (it.level == 78 && prev.level == 78 && it.doc.empty() && !prev.doc.empty() && it.line == prev.end_line + 1) {
            it.doc = prev.doc;
            it.doc_shared = true;
        }
    }
    // A paragraph ends where the next header (or division) starts; a section where the next section does.
    for (size_t h = 0; h < headers.size(); h++) {
        Item &it = u.items[static_cast<size_t>(headers[h])];
        int end = u.last_code_line;
        for (size_t k = h + 1; k < headers.size(); k++) {
            const Item &next = u.items[static_cast<size_t>(headers[k])];
            if (it.level == 0 || next.level == -1) {
                end = (next.doc_line >= 0 ? next.doc_line : next.line) - 1;
                break;
            }
        }
        for (int d : u.divisions) {
            if (d > it.line && d - 1 < end) end = d - 1;
        }
        while (end > it.line && Trim(u.lines[static_cast<size_t>(end)]).empty()) end--;
        it.end_line = std::max(end, it.line);
    }
    for (size_t k = 0; k < u.items.size(); k++) {
        const Item &it = u.items[k];
        if (!it.key.empty()) u.key_index[it.key].push_back(static_cast<int>(k));
        for (const std::string &c : it.calls) {
            if (u.call_index.emplace(Lower(c), static_cast<int>(k)).second) u.calls.push_back({c, static_cast<int>(k)});
        }
    }
    return u;
}

// --- copybooks --------------------------------------------------------------------------------------------------

std::vector<std::string> ReadLines(const std::string &path, bool *ok) {
    std::ifstream in(path, std::ios::binary);
    *ok = static_cast<bool>(in);
    std::vector<std::string> lines;
    if (!in) return lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::shared_ptr<const Unit> LoadCopybook(const std::string &path) {
    struct Entry {
        std::uintmax_t size = 0;
        fs::file_time_type mtime;
        std::shared_ptr<const Unit> unit;
    };
    static std::mutex mu;
    static std::map<std::string, Entry> cache;
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec) return nullptr;
    const fs::file_time_type mtime = fs::last_write_time(path, ec);
    if (ec) return nullptr;
    std::lock_guard<std::mutex> lock(mu);
    const auto it = cache.find(path);
    if (it != cache.end() && it->second.size == size && it->second.mtime == mtime) return it->second.unit;
    bool ok = false;
    const std::vector<std::string> lines = ReadLines(path, &ok);
    if (!ok) return nullptr;
    auto unit = std::make_shared<Unit>(ParseUnit(lines, path));
    cache[path] = Entry{size, mtime, unit};
    return unit;
}

std::vector<std::string> SplitPathList(const char *value) {
    std::vector<std::string> out;
    if (!value) return out;
#if defined(_WIN32)
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::string cur;
    for (const char *p = value;; p++) {
        if (*p == sep || *p == '\0') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            if (*p == '\0') break;
        } else {
            cur += *p;
        }
    }
    return out;
}

struct Ref {
    const Unit *unit = nullptr;
    int index = -1;
    const Item &item() const { return unit->items[static_cast<size_t>(index)]; }
};

}  // namespace

class CobolLspDocument {
public:
    Unit own;
    std::vector<std::shared_ptr<const Unit>> books;  // every copybook reached, in COPY order
    std::vector<std::string> dirs;
    std::vector<const Unit *> units;                 // own first, then the books

    /** @brief Every definition of an upper-case name: the document's own first, then each copybook's. */
    std::vector<Ref> Lookup(const std::string &key) const {
        std::vector<Ref> out;
        for (const Unit *u : units) {
            const auto it = u->key_index.find(key);
            if (it == u->key_index.end()) continue;
            for (int i : it->second) out.push_back(Ref{u, i});
        }
        return out;
    }
};

namespace {

// `dm_kmeans_fit` -> `DM-KMEANS-FIT`, the record rosetta names after a CALL.
std::string RecordKeyOfCall(const std::string &call) {
    std::string out = CobolUpper(call);
    std::replace(out.begin(), out.end(), '_', '-');
    return out;
}

bool CallTarget(const CobolLspDocument &doc, const std::string &call, Ref *out) {
    const std::string key = Lower(call);
    for (const Unit *u : doc.units) {
        const auto it = u->call_index.find(key);
        if (it != u->call_index.end()) {
            *out = Ref{u, it->second};
            return true;
        }
    }
    for (const Ref &r : doc.Lookup(RecordKeyOfCall(call))) {
        if (r.item().level == 1) {
            *out = r;
            return true;
        }
    }
    return false;
}

// The token at (line, col): the one containing col, else one ending exactly at col.
int TokenAt(const Unit &u, int line, int col) {
    int touching = -1;
    const auto first = std::lower_bound(u.tokens.begin(), u.tokens.end(), line,
                                        [](const Token &t, int l) { return t.line < l; });
    for (size_t i = static_cast<size_t>(first - u.tokens.begin()); i < u.tokens.size(); i++) {
        const Token &t = u.tokens[i];
        if (t.line < line) continue;
        if (t.line > line) break;
        if (col >= t.col && col < t.end_col) return static_cast<int>(i);
        if (col == t.end_col) touching = static_cast<int>(i);
    }
    return touching;
}

bool PrevWordIs(const Unit &u, int i, const char *word) {
    return i > 0 && u.tokens[static_cast<size_t>(i) - 1].kind == Token::Word && u.tokens[static_cast<size_t>(i) - 1].value == word;
}

std::string Footer(const CobolLspDocument &doc, const Unit &u, const Item &it) {
    if (&u == &doc.own) return "\n\n*line " + std::to_string(it.line + 1) + "*";
    return "\n\n*" + Basename(u.path) + ":" + std::to_string(it.line + 1) + "*";
}

// The first paragraph of a doc block on one line, cut at ~120 characters.
std::string Compact(const std::vector<std::string> &doc) {
    std::string s;
    for (const std::string &l : doc) {
        const std::string t = Trim(l);
        if (t.empty()) {
            if (!s.empty()) break;
            continue;
        }
        s += (s.empty() ? "" : " ") + t;
    }
    if (s.size() > 120) s = s.substr(0, 117) + "...";
    return s;
}

const Item *TopOf(const Unit &u, const Item &it) {
    const Item *cur = &it;
    while (cur->parent >= 0) cur = &u.items[static_cast<size_t>(cur->parent)];
    return cur;
}

void ListFields(const Unit &u, const Item &it, int depth, std::string *out, int *count) {
    for (int c : it.children) {
        const Item &f = u.items[static_cast<size_t>(c)];
        if (f.level == 88) continue;
        if (++*count > 60) continue;
        *out += "\n" + std::string(static_cast<size_t>(depth) * 2, ' ') + "- `" + (f.name.empty() ? "FILLER" : f.name) + "`";
        if (!f.clauses.empty()) *out += " `" + f.clauses + "`";
        const std::string d = Compact(f.doc);
        if (!d.empty()) *out += ": " + d;
        ListFields(u, f, depth + 1, out, count);
    }
}

bool LooksCpp(const std::vector<std::string> &lines) {
    if (!lines.empty() && lines[0].compare(0, 4, "C++ ") == 0) return false;  // `C++ class x::Y.`: a summary
    for (const std::string &l : lines) {
        if (l.find("::") != std::string::npos || l.find('(') != std::string::npos || l.find("->") != std::string::npos) return true;
    }
    return false;
}

// The head of a hover: the C++ signature (a ```cpp block, or plain text when it is a summary), then a
// ```cobol block with the CALL lines (or the PROCEDURE DIVISION USING line) and the entry itself.
std::string DocHead(const CobolLspDocBlock &d, const std::string &decl) {
    std::string out;
    if (!d.signature.empty()) {
        out += LooksCpp(d.signature) ? "```cpp\n" + Join(d.signature, "\n") + "\n```\n" : Join(d.signature, "\n") + "\n\n";
    }
    out += "```cobol\n";
    for (const CobolLspDocCall &c : d.calls) out += c.line + "\n";
    if (!d.using_line.empty()) out += d.using_line + "\n";
    return out + decl + "\n```";
}

std::string RecordHover(const CobolLspDocument &doc, const Unit &u, const Item &it) {
    const CobolLspDocBlock d = CobolLspSplitDoc(it.doc);
    std::string out = DocHead(d, it.decl);
    if (!d.body.empty()) out += "\n\n" + Join(d.body, "\n");
    std::string fields;
    int count = 0;
    ListFields(u, it, 0, &fields, &count);
    if (!fields.empty()) {
        out += "\n\n**Fields**\n" + fields;
        if (count > 60) out += "\n- ... and " + std::to_string(count - 60) + " more";
    }
    return out + Footer(doc, u, it);
}

std::string ItemHover(const CobolLspDocument &doc, const Unit &u, const Item &it) {
    if (it.level == 1 && (!it.children.empty() || !it.calls.empty())) return RecordHover(doc, u, it);
    const CobolLspDocBlock d = CobolLspSplitDoc(it.doc);
    std::string out = DocHead(d, it.decl);
    if (it.level == 0) out += "\n\nParagraph";
    if (it.level == -1) out += "\n\nSection";
    if (it.level == 0 && it.parent >= 0) out += " in section `" + u.items[static_cast<size_t>(it.parent)].name + "`";
    if (it.level == 78 || it.level == 88) {
        const size_t v = CobolUpper(it.clauses).find("VALUE");
        if (v != std::string::npos) out += "\n\n**Value:** `" + Trim(it.clauses.substr(v + 5)) + "`";
    }
    if (!d.body.empty()) out += "\n\n" + Join(d.body, "\n");
    // An undocumented 88 (rosetta documents the field, not each value): the field's meaning.
    if (it.level == 88 && it.doc.empty() && it.parent >= 0) {
        const std::string pd = Compact(u.items[static_cast<size_t>(it.parent)].doc);
        if (!pd.empty()) out += "\n\n" + pd;
    }
    std::string values;
    for (int c : it.children) {
        const Item &v = u.items[static_cast<size_t>(c)];
        if (v.level != 88) continue;
        values += "\n- `" + v.name + "`";
        if (!v.clauses.empty()) values += " " + v.clauses;
        const std::string dd = Compact(v.doc);
        if (!dd.empty()) values += ": " + dd;
    }
    if (!values.empty()) out += "\n\n**Values**\n" + values;
    if (it.level >= 2 && it.level <= 88 && it.parent >= 0) {
        const Item &parent = u.items[static_cast<size_t>(it.parent)];
        const Item *top = TopOf(u, it);
        if (it.level == 88) {
            out += "\n\nCondition on `" + parent.name + "`";
            if (top != &parent) out += ", in `" + top->name + "`";
        } else {
            out += "\n\nIn `" + top->name + "`";
        }
        if (!top->call_name.empty()) out += " (`CALL \"" + top->call_name + "\"`)";
    }
    return out + Footer(doc, u, it);
}

CobolLspRange TokenRange(const Token &t) {
    CobolLspRange r;
    r.line = r.end_line = t.line;
    r.col = t.col;
    r.end_col = t.end_col;
    return r;
}

CobolLspRange ItemRange(const Item &it) {
    CobolLspRange r;
    r.line = r.end_line = it.line;
    r.col = it.col;
    r.end_col = it.end_col;
    return r;
}

// The definition a word token names, honoring `X OF Y` / `X IN Y` qualification.
bool Resolve(const CobolLspDocument &doc, const Unit &u, int ti, Ref *out) {
    const Token &t = u.tokens[static_cast<size_t>(ti)];
    const std::vector<Ref> refs = doc.Lookup(t.value);
    if (refs.empty()) return false;
    *out = refs.front();
    // A definition at this very spot wins (hovering the declaration itself).
    for (const Ref &r : refs) {
        if (r.unit == &u && r.item().line == t.line && r.item().col == t.col) {
            *out = r;
            return true;
        }
    }
    if (refs.size() > 1 && static_cast<size_t>(ti) + 2 < u.tokens.size()) {
        const Token &of = u.tokens[static_cast<size_t>(ti) + 1];
        const Token &q = u.tokens[static_cast<size_t>(ti) + 2];
        if (of.kind == Token::Word && (of.value == "OF" || of.value == "IN") && q.kind == Token::Word) {
            for (const Ref &r : refs) {
                for (int p = r.item().parent; p >= 0; p = r.unit->items[static_cast<size_t>(p)].parent) {
                    if (r.unit->items[static_cast<size_t>(p)].key == q.value) {
                        *out = r;
                        return true;
                    }
                }
            }
        }
    }
    return true;
}

std::string CopyHover(const CobolLspDocument &doc, const CopyRef &c) {
    if (c.resolved.empty()) {
        std::string out = "`COPY " + c.name + "`: copybook not found. Searched:\n";
        for (const std::string &d : doc.dirs) out += "\n- `" + d + "`";
        return out;
    }
    std::string out = "Copybook `" + c.resolved + "`";
    const Unit *book = nullptr;
    for (const auto &b : doc.books) {
        if (b->path == c.resolved) book = b.get();
    }
    if (!book) return out;
    std::string list;
    int count = 0;
    for (const Item &it : book->items) {
        if (it.key.empty() || !(it.level == 1 || it.level == 77 || it.level == 78)) continue;
        if (++count > 40) continue;
        list += "\n- `" + it.name + "`";
        if (!it.call_name.empty()) list += " `CALL \"" + it.call_name + "\"`";
        const std::string d = Compact(CobolLspSplitDoc(it.doc).body);
        if (!d.empty()) list += ": " + d;
    }
    if (!list.empty()) out += "\n\n**Records**\n" + list;
    if (count > 40) out += "\n- ... and " + std::to_string(count - 40) + " more";
    return out;
}

const CopyRef *CopyAt(const Unit &u, int line, int col) {
    for (const CopyRef &c : u.copies) {
        if (c.line == line && col >= c.col && col <= c.end_col) return &c;
        if (c.kw_line == line && col >= c.kw_col && col < c.kw_end_col) return &c;
    }
    return nullptr;
}

}  // namespace

// --- public: analysis ---------------------------------------------------------------------------------------------

std::vector<std::string> CobolLspCopybookDirs(const std::string &doc_path, const std::vector<std::string> &include_dirs) {
    std::vector<std::string> out;
    auto add = [&out](const std::string &d) {
        if (!d.empty() && std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
    };
    const std::string own = DirOf(doc_path);
    add(own);
    for (const std::string &d : SplitPathList(std::getenv("COBCPY"))) add(d);
    for (const std::string &d : include_dirs) {
        if (d.empty()) continue;
        if (fs::path(d).is_absolute() || own.empty()) {
            add(d);
        } else {
            add((fs::path(own) / d).lexically_normal().string());
        }
    }
    const char *copy_dir = std::getenv("COB_COPY_DIR");
    if (copy_dir && *copy_dir) add(copy_dir);
    return out;
}

std::string CobolLspResolveCopy(const std::string &name, const std::vector<std::string> &dirs) {
    if (name.empty()) return std::string();
    std::error_code ec;
    if (fs::path(name).is_absolute()) return fs::is_regular_file(name, ec) ? name : std::string();
    std::vector<std::string> names = {name};
    for (const std::string &v : {CobolUpper(name), Lower(name)}) {
        if (std::find(names.begin(), names.end(), v) == names.end()) names.push_back(v);
    }
    static const char *const kExts[] = {"", ".cpy", ".CPY", ".cbl", ".CBL", ".cob", ".COB", ".cobol"};
    for (const std::string &dir : dirs) {
        for (const std::string &nm : names) {
            for (const char *ext : kExts) {
                const fs::path p = fs::path(dir) / (nm + ext);
                if (fs::is_regular_file(p, ec)) {
                    const fs::path canon = fs::weakly_canonical(p, ec);
                    return ec ? p.string() : canon.string();
                }
            }
        }
    }
    return std::string();
}

std::shared_ptr<const CobolLspDocument> CobolLspAnalyze(const std::vector<std::string> &lines, const std::string &path,
                                                        const std::vector<std::string> &include_dirs) {
    auto doc = std::make_shared<CobolLspDocument>();
    doc->own = ParseUnit(lines, path);
    doc->dirs = CobolLspCopybookDirs(path, include_dirs);
    doc->units.push_back(&doc->own);
    std::set<std::string> seen;
    if (!path.empty()) {
        std::error_code ec;
        seen.insert(fs::weakly_canonical(fs::absolute(path, ec), ec).string());
    }
    // Breadth-first through nested COPYs; a copybook's own directory comes first for the ones it COPYs.
    struct Pending {
        std::string name;
        std::string from_dir;
        CopyRef *ref;
    };
    std::vector<Pending> queue;
    for (CopyRef &c : doc->own.copies) queue.push_back({c.name, std::string(), &c});
    for (size_t at = 0; at < queue.size() && doc->books.size() < 64; at++) {
        std::vector<std::string> dirs = doc->dirs;
        if (!queue[at].from_dir.empty()) dirs.insert(dirs.begin(), queue[at].from_dir);
        const std::string found = CobolLspResolveCopy(queue[at].name, dirs);
        if (queue[at].ref) queue[at].ref->resolved = found;
        if (found.empty() || !seen.insert(found).second) continue;
        std::shared_ptr<const Unit> book = LoadCopybook(found);
        if (!book) continue;
        doc->books.push_back(book);
        doc->units.push_back(book.get());
        for (const CopyRef &c : book->copies) queue.push_back({c.name, DirOf(found), nullptr});
    }
    return doc;
}

std::vector<CobolLspItemInfo> CobolLspItems(const CobolLspDocument &doc) {
    std::vector<CobolLspItemInfo> out;
    auto add_unit = [&out](const Unit &u, bool own) {
        for (const Item &it : u.items) {
            CobolLspItemInfo info;
            info.name = it.name;
            info.level = it.level;
            info.path = own ? std::string() : u.path;
            info.line = it.line;
            info.doc = it.doc;
            info.call_name = it.call_name;
            info.calls = it.calls;
            if (it.parent >= 0) info.parent = u.items[static_cast<size_t>(it.parent)].name;
            out.push_back(std::move(info));
        }
    };
    add_unit(doc.own, true);
    for (const auto &b : doc.books) add_unit(*b, false);
    return out;
}

// --- diagnostics ------------------------------------------------------------------------------------------------

std::vector<CobolLspDiagnostic> CobolLspDiagnostics(const CobolLspDocument &doc) {
    std::vector<CobolLspDiagnostic> out;
    for (const CopyRef &c : doc.own.copies) {
        if (!c.resolved.empty()) continue;
        CobolLspDiagnostic d;
        d.range.line = d.range.end_line = c.line;
        d.range.col = c.col;
        d.range.end_col = c.end_col;
        d.severity = CobolLspSeverity::Warning;
        d.code = "copy-not-found";
        d.message = "copybook '" + c.name + "' not found (searched the file's directory, $COBCPY and includeDirs: " +
                    Join(doc.dirs, ":") + ")";
        out.push_back(d);
    }
    return out;
}

std::string CobolLspFormatDiagnostic(const std::string &path, const CobolLspDiagnostic &d) {
    const char *sev = d.severity == CobolLspSeverity::Error         ? "error"
                      : d.severity == CobolLspSeverity::Warning     ? "warning"
                      : d.severity == CobolLspSeverity::Information ? "info"
                                                                    : "hint";
    return path + ":" + std::to_string(d.range.line + 1) + ":" + std::to_string(d.range.col + 1) + ": " + sev + ": " +
           d.message + " [" + d.code + "]";
}

bool CobolLspDiagnosticFails(const CobolLspDiagnostic &d) {
    return d.severity == CobolLspSeverity::Error || d.severity == CobolLspSeverity::Warning;
}

// --- hover ---------------------------------------------------------------------------------------------------------

CobolLspHoverInfo CobolLspHover(const CobolLspDocument &doc, int line, int col) {
    CobolLspHoverInfo out;
    const Unit &u = doc.own;
    if (const CopyRef *c = CopyAt(u, line, col)) {
        out.found = true;
        out.text = CopyHover(doc, *c);
        out.range.line = out.range.end_line = c->line;
        out.range.col = c->col;
        out.range.end_col = c->end_col;
        return out;
    }
    const int ti = TokenAt(u, line, col);
    if (ti < 0) return out;
    const Token &t = u.tokens[static_cast<size_t>(ti)];
    Ref r;
    if (t.kind == Token::String && PrevWordIs(u, ti, "CALL")) {
        if (!CallTarget(doc, t.value, &r)) return out;
        out.text = RecordHover(doc, *r.unit, r.item());
    } else if (t.kind == Token::Word && Resolve(doc, u, ti, &r)) {
        out.text = ItemHover(doc, *r.unit, r.item());
    } else {
        return out;
    }
    out.found = true;
    out.range = TokenRange(t);
    return out;
}

// --- completion ------------------------------------------------------------------------------------------------------

namespace {

CobolLspKind KindOf(const Item &it) {
    if (it.level == 1) return it.children.empty() ? CobolLspKind::Variable : CobolLspKind::Struct;
    if (it.level == 88 || it.level == 78) return CobolLspKind::Constant;
    if (it.level == 0 || it.level == -1) return CobolLspKind::Function;
    if (it.level == -2) return CobolLspKind::File;
    if (it.level == 77) return CobolLspKind::Variable;
    return CobolLspKind::Field;
}

std::string ShortDoc(const Item &it) {
    const CobolLspDocBlock d = CobolLspSplitDoc(it.doc);
    std::string out = DocHead(d, it.decl);
    if (!d.body.empty()) out += "\n\n" + Join(d.body, "\n");
    return out;
}

bool StartsWithCI(const std::string &s, const std::string &prefix) {
    if (prefix.size() > s.size()) return false;
    for (size_t i = 0; i < prefix.size(); i++) {
        if (std::toupper(static_cast<unsigned char>(s[i])) != std::toupper(static_cast<unsigned char>(prefix[i]))) return false;
    }
    return true;
}

}  // namespace

std::vector<CobolLspCompletionItem> CobolLspCompletions(const CobolLspDocument &doc, int line, int col) {
    std::vector<CobolLspCompletionItem> out;
    const Unit &u = doc.own;
    if (line < 0 || static_cast<size_t>(line) >= u.lines.size()) return out;
    const std::string &cur = u.lines[static_cast<size_t>(line)];
    col = std::max(0, std::min(col, static_cast<int>(cur.size())));

    // The token the cursor is in (or right after), and the last one wholly before it.
    int ti = -1, before = -1;
    for (size_t i = 0; i < u.tokens.size(); i++) {
        const Token &t = u.tokens[i];
        if (t.line > line || (t.line == line && t.col >= col)) break;
        if (t.line == line && col > t.col && col <= t.end_col) {
            ti = static_cast<int>(i);
        } else {
            before = static_cast<int>(i);
        }
    }
    const auto call_items = [&doc](int start, int end, bool quoted, const std::string &prefix) {
        // Every CALL line of every copybook (a record can have many: _get/_set, _base, a container's
        // _new/_size/...), first definition of a name wins.
        std::map<std::string, std::pair<std::string, Ref>> found;  // lower-case -> as written, record
        for (const Unit *unit : doc.units) {
            for (const auto &c : unit->calls) {
                if (StartsWithCI(c.first, prefix)) found.emplace(Lower(c.first), std::make_pair(c.first, Ref{unit, c.second}));
            }
        }
        std::vector<CobolLspCompletionItem> items;
        for (const auto &kv : found) {
            if (items.size() >= 2000) break;
            const std::string &name = kv.second.first;
            const Item &it = kv.second.second.item();
            CobolLspCompletionItem c;
            c.label = name;
            c.insert_text = quoted ? "\"" + name + "\"" : name;
            c.kind = CobolLspKind::Function;
            const CobolLspDocBlock d = CobolLspSplitDoc(it.doc);
            for (const CobolLspDocCall &call : d.calls) {
                if (call.name != name) continue;
                const size_t using_at = CobolUpper(call.line).find(" USING ");
                c.detail = using_at == std::string::npos ? call.line : call.line.substr(using_at + 1);
            }
            if (c.detail.empty()) c.detail = "USING " + it.name;
            if (found.size() <= 300) c.documentation = ShortDoc(it);  // a bare `CALL "` on datamunge: thousands
            c.replace_start = start;
            c.replace_end = end;
            items.push_back(std::move(c));
        }
        return items;
    };
    // Inside the literal after CALL: the programs the copybooks document.
    if (ti >= 0 && u.tokens[static_cast<size_t>(ti)].kind == Token::String && PrevWordIs(u, ti, "CALL")) {
        const Token &t = u.tokens[static_cast<size_t>(ti)];
        const int quote = t.text[0] == '"' || t.text[0] == '\'' ? t.col : t.col + 1;
        const int start = std::min(quote + 1, col);
        const std::string prefix = cur.substr(static_cast<size_t>(start), static_cast<size_t>(col - start));
        return call_items(start, col, false, prefix);
    }
    int start = col;
    while (start > 0 && CobolWordChar(cur[static_cast<size_t>(start - 1)])) start--;
    const std::string prefix = cur.substr(static_cast<size_t>(start), static_cast<size_t>(col - start));
    const int prev = ti >= 0 ? ti - 1 : before;
    const Token *prev_tok = prev >= 0 ? &u.tokens[static_cast<size_t>(prev)] : nullptr;
    if (prev_tok && prev_tok->kind == Token::Word && prev_tok->value == "CALL") return call_items(start, col, true, prefix);
    if (prev_tok && prev_tok->kind == Token::Word && prev_tok->value == "COPY") {
        std::set<std::string> names;
        for (const std::string &dir : doc.dirs) {
            std::error_code ec;
            for (fs::directory_iterator it(dir, ec), e; !ec && it != e; it.increment(ec)) {
                const fs::path p = it->path();
                if (Lower(p.extension().string()) == ".cpy" && StartsWithCI(p.stem().string(), prefix)) names.insert(p.stem().string());
            }
        }
        for (const std::string &nm : names) {
            CobolLspCompletionItem c;
            c.label = nm;
            c.insert_text = nm;
            c.kind = CobolLspKind::File;
            c.replace_start = start;
            c.replace_end = col;
            out.push_back(c);
        }
        return out;
    }
    // Names: the document's own, the copybooks' records, then (with a prefix) every field; keywords last.
    std::set<std::string> taken;
    auto add = [&](const Ref &r) {
        const Item &it = r.item();
        if (it.key.empty() || !StartsWithCI(it.name, prefix) || out.size() >= 2000 || !taken.insert(it.key).second) return;
        CobolLspCompletionItem c;
        c.label = it.name;
        c.insert_text = it.name;
        c.kind = KindOf(it);
        c.detail = it.clauses;
        if (r.unit != &doc.own) c.detail += (c.detail.empty() ? "" : "  ") + std::string("(") + Basename(r.unit->path) + ")";
        c.documentation = it.level == 1 ? ShortDoc(it) : ItemHover(doc, *r.unit, it);
        c.replace_start = start;
        c.replace_end = col;
        out.push_back(std::move(c));
    };
    for (size_t i = 0; i < u.items.size(); i++) {
        if (u.items[i].line == line && u.items[i].col == start) continue;  // the word being typed
        add(Ref{&u, static_cast<int>(i)});
    }
    for (const auto &b : doc.books) {  // the copybooks' records (with a prefix, every name below)
        for (size_t i = 0; i < b->items.size(); i++) {
            const int level = b->items[i].level;
            if (level == 1 || level == 77 || level == 78) add(Ref{b.get(), static_cast<int>(i)});
        }
    }
    if (!prefix.empty()) {
        for (const auto &b : doc.books) {
            for (size_t i = 0; i < b->items.size(); i++) add(Ref{b.get(), static_cast<int>(i)});
        }
        for (const std::string &k : CobolKeywords()) {
            if (!StartsWithCI(k, prefix) || taken.count(k)) continue;
            CobolLspCompletionItem c;
            c.label = k;
            c.insert_text = k;
            c.kind = CobolLspKind::Keyword;
            c.replace_start = start;
            c.replace_end = col;
            out.push_back(c);
        }
    }
    // A short prefix on a big copybook matches thousands: send the list light (the hover has the docs).
    if (out.size() > 300) {
        for (CobolLspCompletionItem &c : out) c.documentation.clear();
    }
    return out;
}

CobolLspSignatureHelp CobolLspSignatureAt(const CobolLspDocument &doc, int line, int col) {
    CobolLspSignatureHelp out;
    const Unit &u = doc.own;
    int last = -1;
    for (size_t i = 0; i < u.tokens.size(); i++) {
        const Token &t = u.tokens[i];
        if (t.line > line || (t.line == line && t.col >= col)) break;
        last = static_cast<int>(i);
    }
    if (last < 0) return out;
    // Back to the CALL "x" this statement started with, counting the operands after USING.
    static const std::set<std::string> kPassing = {"BY", "REFERENCE", "CONTENT", "VALUE"};
    int operands = 0;
    bool using_seen = false;
    for (int i = last, steps = 0; i >= 1 && steps < 200; i--, steps++) {
        const Token &t = u.tokens[static_cast<size_t>(i)];
        if (t.kind == Token::Period) return out;
        if (t.kind == Token::Word && t.value == "USING") using_seen = true;
        if (!using_seen && (t.kind == Token::Word || t.kind == Token::String) && !kPassing.count(t.value)) operands++;
        if (t.kind != Token::String || !PrevWordIs(u, i, "CALL")) continue;
        Ref r;
        if (!using_seen || !CallTarget(doc, t.value, &r)) return out;
        const Item &it = r.item();
        const CobolLspDocBlock d = CobolLspSplitDoc(it.doc);
        out.label = "CALL \"" + t.value + "\" USING " + it.name;
        for (const CobolLspDocCall &c : d.calls) {
            if (Lower(c.name) == Lower(t.value)) {
                out.label = c.line;
                const size_t paren = out.label.find(" (");  // drop the `(sets -SIZE)` note
                if (paren != std::string::npos) out.label.erase(paren);
                break;
            }
        }
        const size_t using_at = CobolUpper(out.label).find(" USING ");
        if (using_at != std::string::npos) {
            std::istringstream ss(out.label.substr(using_at + 7));
            std::string w;
            while (ss >> w) {
                if (!kPassing.count(CobolUpper(w))) out.params.push_back(w);
            }
        }
        out.documentation = Join(d.body, "\n");
        // Still typing the last operand: that one is active, not the next.
        const Token &lt = u.tokens[static_cast<size_t>(last)];
        const bool inside = lt.line == line && lt.end_col == col && lt.kind == Token::Word && lt.value != "USING";
        out.active_param = std::max(0, operands - (inside ? 1 : 0));
        if (!out.params.empty()) out.active_param = std::min(out.active_param, static_cast<int>(out.params.size()) - 1);
        out.found = true;
        return out;
    }
    return out;
}

// --- navigation ---------------------------------------------------------------------------------------------------

std::vector<CobolLspLocation> CobolLspDefinition(const CobolLspDocument &doc, int line, int col) {
    std::vector<CobolLspLocation> out;
    const Unit &u = doc.own;
    if (const CopyRef *c = CopyAt(u, line, col)) {
        if (!c->resolved.empty()) out.push_back(CobolLspLocation{c->resolved, CobolLspRange{}});
        return out;
    }
    const int ti = TokenAt(u, line, col);
    if (ti < 0) return out;
    const Token &t = u.tokens[static_cast<size_t>(ti)];
    Ref r;
    const bool found = (t.kind == Token::String && PrevWordIs(u, ti, "CALL")) ? CallTarget(doc, t.value, &r)
                       : t.kind == Token::Word                                ? Resolve(doc, u, ti, &r)
                                                                              : false;
    if (!found) return out;
    out.push_back(CobolLspLocation{r.unit == &u ? std::string() : r.unit->path, ItemRange(r.item())});
    return out;
}

std::vector<CobolLspReference> CobolLspReferences(const CobolLspDocument &doc, int line, int col) {
    std::vector<CobolLspReference> out;
    const Unit &u = doc.own;
    const int ti = TokenAt(u, line, col);
    if (ti < 0 || u.tokens[static_cast<size_t>(ti)].kind != Token::Word) return out;
    const std::string key = u.tokens[static_cast<size_t>(ti)].value;
    if (CobolIsKeyword(key)) return out;
    for (const Token &t : u.tokens) {
        if (t.kind != Token::Word || t.value != key) continue;
        CobolLspReference r;
        r.range = TokenRange(t);
        for (const Item &it : u.items) {
            if (it.key == key && it.line == t.line && it.col == t.col) r.is_definition = true;
        }
        out.push_back(r);
    }
    return out;
}

CobolLspRenameTarget CobolLspPrepareRename(const CobolLspDocument &doc, int line, int col) {
    CobolLspRenameTarget out;
    const Unit &u = doc.own;
    const int ti = TokenAt(u, line, col);
    if (ti < 0 || u.tokens[static_cast<size_t>(ti)].kind != Token::Word) return out;
    const Token &t = u.tokens[static_cast<size_t>(ti)];
    for (const Item &it : u.items) {
        if (it.key == t.value) {
            out.found = true;
            out.range = TokenRange(t);
            out.placeholder = t.text;
            return out;
        }
    }
    return out;
}

std::vector<CobolLspTextEdit> CobolLspRename(const CobolLspDocument &doc, int line, int col, const std::string &new_name,
                                             std::string *error) {
    if (!CobolLspPrepareRename(doc, line, col).found) {
        *error = "only names this file defines can be renamed (a copybook's are shared)";
        return {};
    }
    if (new_name.empty() || !std::all_of(new_name.begin(), new_name.end(), CobolWordChar) || new_name.front() == '-' ||
        new_name.back() == '-' || CobolIsKeyword(new_name)) {
        *error = "'" + new_name + "' is not a COBOL user-defined word";
        return {};
    }
    std::vector<CobolLspTextEdit> edits;
    for (const CobolLspReference &r : CobolLspReferences(doc, line, col)) edits.push_back(CobolLspTextEdit{r.range, new_name});
    return edits;
}

// --- outline / folding -----------------------------------------------------------------------------------------

namespace {

int LastLineOf(const Unit &u, const Item &it) {
    int end = it.end_line;
    for (int c : it.children) end = std::max(end, LastLineOf(u, u.items[static_cast<size_t>(c)]));
    return end;
}

int EndOf(const Unit &u, const Item &it) {
    return std::max(it.line, it.level == 0 || it.level == -1 ? it.end_line : LastLineOf(u, it));
}

}  // namespace

std::vector<CobolLspSymbol> CobolLspSymbols(const CobolLspDocument &doc) {
    std::vector<CobolLspSymbol> out;
    const Unit &u = doc.own;
    std::map<int, int> index_of;  // item -> symbol
    for (size_t i = 0; i < u.items.size(); i++) {
        const Item &it = u.items[i];
        if (it.key.empty()) continue;
        CobolLspSymbol s;
        s.name = it.name;
        s.detail = it.level > 0 ? std::to_string(it.level) + (it.clauses.empty() ? "" : " " + it.clauses) : "";
        if (it.level == 1) s.kind = it.children.empty() ? CobolLspSymbolKind::Variable : CobolLspSymbolKind::Struct;
        else if (it.level == 88 || it.level == 78) s.kind = CobolLspSymbolKind::Constant;
        else if (it.level == 0) s.kind = CobolLspSymbolKind::Function;
        else if (it.level == -1) s.kind = CobolLspSymbolKind::Module;
        else if (it.level == -2) s.kind = CobolLspSymbolKind::File;
        else s.kind = CobolLspSymbolKind::Field;
        s.sel_range = ItemRange(it);
        s.range = s.sel_range;
        s.range.col = 0;
        s.range.end_line = std::min(EndOf(u, it), static_cast<int>(u.lines.size()) - 1);
        s.range.end_col = static_cast<int>(u.lines[static_cast<size_t>(s.range.end_line)].size());
        if (it.parent >= 0) {
            const auto p = index_of.find(it.parent);
            if (p != index_of.end()) s.parent = p->second;
        }
        index_of[static_cast<int>(i)] = static_cast<int>(out.size());
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<CobolLspFold> CobolLspFolds(const CobolLspDocument &doc) {
    std::vector<CobolLspFold> out;
    const Unit &u = doc.own;
    for (size_t d = 0; d < u.divisions.size(); d++) {
        int end = d + 1 < u.divisions.size() ? u.divisions[d + 1] - 1 : u.last_code_line;
        while (end > u.divisions[d] && Trim(u.lines[static_cast<size_t>(end)]).empty()) end--;
        if (end > u.divisions[d]) out.push_back({u.divisions[d], end, ""});
    }
    for (const Item &it : u.items) {
        const int end = EndOf(u, it);
        if ((it.level == 1 || it.level <= 0) && end > it.line) out.push_back({it.line, end, ""});
    }
    for (const auto &b : u.doc_blocks) {
        if (b.second > b.first) out.push_back({b.first, b.second, "comment"});
    }
    std::sort(out.begin(), out.end(), [](const CobolLspFold &a, const CobolLspFold &b) {
        return a.start_line != b.start_line ? a.start_line < b.start_line : a.end_line > b.end_line;
    });
    return out;
}
