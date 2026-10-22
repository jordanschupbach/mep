// mep's C# formatter: Format() -- parse, run Roslyn's engine (the port in
// csharp_format_engine.cpp and friends), proof-read. See csharp_format.h.

#include "csharp_format.h"

#include <cstdio>
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
#include <pthread.h>
#endif

#include "csharp_format_internal.h"

namespace csfmt {

namespace {

bool ValidUtf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int n;
        uint32_t cp;
        if (c < 0x80) {
            i++;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            n = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            n = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            n = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        for (int k = 1; k <= n; k++) {
            if (i + static_cast<size_t>(k) >= s.size()) return false;
            unsigned char cc = static_cast<unsigned char>(s[i + static_cast<size_t>(k)]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF))) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        i += static_cast<size_t>(n) + 1;
    }
    return true;
}

bool IsWs(char16_t c) { return IsDotNetWhiteSpace(c); }

U16 StripWhitespace(U16View s) {
    U16 out;
    for (char16_t c : s) {
        if (!IsWs(c)) out += c;
    }
    return out;
}

/** @brief The trivia that must survive formatting, whitespace inside them aside (comments, directives...). */
std::vector<std::pair<SyntaxKind, U16>> Kept(const csharp::SyntaxTree &tree, const std::vector<csharp::SyntaxTrivia> &list) {
    std::vector<std::pair<SyntaxKind, U16>> out;
    for (const csharp::SyntaxTrivia &t : list) {
        SyntaxKind k = t.Kind();
        if (k == SyntaxKind::WhitespaceTrivia || k == SyntaxKind::EndOfLineTrivia) continue;
        U16View text = U16View(tree.Text16()).substr(static_cast<size_t>(t.position), static_cast<size_t>(t.green->full_width));
        if (k == SyntaxKind::DisabledTextTrivia) {
            out.emplace_back(k, U16(text));  // excluded code is kept byte for byte
        } else {
            out.emplace_back(k, StripWhitespace(text));
        }
    }
    return out;
}

/** @brief Compares two trees modulo whitespace trivia; on a difference, sets *pos (UTF-16, in `a`). */
bool SameTree(const csharp::SyntaxTree &ta, const Node *a, const csharp::SyntaxTree &tb, const Node *b, int *pos) {
    if (a->Kind() != b->Kind() || a->IsToken() != b->IsToken() || a->children.size() != b->children.size()) {
        *pos = a->Span().start;
        return false;
    }
    if (a->IsToken()) {
        if (a->IsMissing() != b->IsMissing() || U16View(a->green->text) != U16View(b->green->text) ||
            Kept(ta, a->leading) != Kept(tb, b->leading) || Kept(ta, a->trailing) != Kept(tb, b->trailing)) {
            *pos = a->Span().start;
            return false;
        }
        return true;
    }
    for (size_t i = 0; i < a->children.size(); i++) {
        if (!SameTree(ta, a->children[i], tb, b->children[i], pos)) return false;
    }
    return true;
}

}  // namespace

namespace {

Result FormatOnThisStack(std::string_view src, const Options &opts);

}  // namespace

Result Format(std::string_view src, const Options &opts) {
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
    // The engine walks the tree recursively, as deep as the code nests (a long `a + b + ...` is a tree as
    // deep as it is long), so it runs on a thread with a deep stack, like the parser.
    struct Job {
        std::string_view src;
        const Options *opts;
        Result result;
    } job{src, &opts, Result()};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, static_cast<size_t>(512) * 1024 * 1024);
    pthread_t thread;
    auto entry = [](void *p) -> void * {
        Job *j = static_cast<Job *>(p);
        j->result = FormatOnThisStack(j->src, *j->opts);
        return nullptr;
    };
    if (pthread_create(&thread, &attr, entry, &job) == 0) {
        pthread_join(thread, nullptr);
        pthread_attr_destroy(&attr);
        return job.result;
    }
    pthread_attr_destroy(&attr);
#endif
    return FormatOnThisStack(src, opts);
}

namespace {

Result FormatOnThisStack(std::string_view src, const Options &opts) {
    Result r;
    r.text = std::string(src);
    if (!ValidUtf8(src)) {
        r.error = "the file is not valid UTF-8";
        return r;
    }
    bool bom = src.size() >= 3 && static_cast<unsigned char>(src[0]) == 0xEF &&
               static_cast<unsigned char>(src[1]) == 0xBB && static_cast<unsigned char>(src[2]) == 0xBF;
    std::string_view body = bom ? src.substr(3) : src;

    std::unique_ptr<csharp::SyntaxTree> tree = csharp::SyntaxTree::ParseUtf16(csharp::ToUtf16(body));
    LineOptions lo;
    lo.use_tabs = opts.use_tabs;
    lo.tab_size = opts.tab_size > 0 ? opts.tab_size : 4;
    lo.indentation_size = opts.indent_size > 0 ? opts.indent_size : 4;
    lo.new_line = csharp::ToUtf16(opts.new_line.empty() ? std::string("\n") : opts.new_line);

    Tree t(tree.get());
    Rules rules(t);
    U16 formatted = FormatTree(t, rules, lo);

    if (opts.verify) {
        std::unique_ptr<csharp::SyntaxTree> again = csharp::SyntaxTree::ParseUtf16(formatted);
        int pos = 0;
        bool same = SameTree(*tree, tree->Root(), *again, again->Root(), &pos);
        if (same) {
            const auto &da = tree->Diagnostics();
            const auto &db = again->Diagnostics();
            if (da.size() != db.size()) {
                same = false;
            } else {
                for (size_t i = 0; i < da.size(); i++) {
                    if (da[i].code != db[i].code) {
                        same = false;
                        pos = da[i].span.start;
                        break;
                    }
                }
            }
        }
        if (!same) {
            int line = 0, col = 0;
            tree->LineCol(pos, &line, &col);
            r.error = "formatting would change the code here (mep's C# formatter refused the file)";
            r.error_line = line + 1;
            return r;
        }
    }

    std::string out = bom ? std::string("\xEF\xBB\xBF") : std::string();
    out += csharp::ToUtf8(formatted);
    r.text = std::move(out);
    r.ok = true;
    return r;
}

}  // namespace

}  // namespace csfmt
