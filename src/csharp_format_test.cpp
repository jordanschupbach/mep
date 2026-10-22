// Tests for mep's C# formatter (csharp_format.h).
//
//   mep-csharp-format-test                 run the cases (csharp_format_test_cases.inc:
//                                          input / expected pairs from Roslyn's own
//                                          formatting tests, each checked against
//                                          `csharp-oracle format`)
//   mep-csharp-format-test --format [--indent N] [--tabs] FILE
//                                          print FILE formatted (for corpus comparison
//                                          with `csharp-oracle format`)
//   mep-csharp-format-test --format --out DIR FILE... | --format --out DIR --stdin-list
//                                          write each result to DIR/<absolute path>; prints
//                                          "ok FILE" or "error FILE LINE: message" per file
//   mep-csharp-format-test --scramble SEED --out DIR FILE... | --stdin-list
//                                          write a copy of each file with its whitespace
//                                          scrambled (indentation, spaces between tokens,
//                                          some lines joined) to DIR/<absolute path>, kept
//                                          only when it parses to the same tokens and errors;
//                                          prints "ok FILE" or "skip FILE"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "csharp_format.h"
#include "csharp_parser.h"

namespace {

std::string ReadFile(const std::string &path, bool *ok) {
    std::ifstream in(path, std::ios::binary);
    *ok = static_cast<bool>(in);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void MakeDirs(const std::string &path) {
    for (size_t i = 1; i < path.size(); i++) {
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
    }
}

std::string Absolute(const std::string &p) {
    if (!p.empty() && p[0] == '/') return p;
    char buf[4096];
    if (!getcwd(buf, sizeof buf)) return p;
    return std::string(buf) + "/" + p;
}

int FormatFiles(int argc, char **argv) {
    std::string out_dir;
    std::vector<std::string> files;
    bool stdin_list = false;
    csfmt::Options opt;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (a == "--indent" && i + 1 < argc) {
            opt.indent_size = opt.tab_size = std::atoi(argv[++i]);
        } else if (a == "--tabs") {
            opt.use_tabs = true;
        } else if (a == "--stdin-list") {
            stdin_list = true;
        } else {
            files.push_back(a);
        }
    }
    if (stdin_list) {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty()) files.push_back(line);
        }
    }
    int status = 0;
    for (const std::string &f : files) {
        bool ok = false;
        std::string src = ReadFile(f, &ok);
        if (!ok) {
            std::printf("error %s 0: cannot read\n", f.c_str());
            status = 1;
            continue;
        }
        csfmt::Result r = csfmt::Format(src, opt);
        if (out_dir.empty()) {
            if (!r.ok) {
                std::fprintf(stderr, "%s:%d: %s\n", f.c_str(), r.error_line, r.error.c_str());
                status = 1;
            }
            std::fwrite(r.text.data(), 1, r.text.size(), stdout);
            continue;
        }
        std::string dest = out_dir + Absolute(f);
        MakeDirs(dest);
        std::ofstream o(dest, std::ios::binary);
        o << r.text;
        if (r.ok) {
            std::printf("ok %s\n", f.c_str());
        } else {
            std::printf("error %s %d: %s\n", f.c_str(), r.error_line, r.error.c_str());
        }
    }
    return status;
}

// --- Scrambling -------------------------------------------------------------------------------------

struct Rng {
    unsigned long long s;
    unsigned Next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<unsigned>(s >> 33);
    }
    int Below(int n) { return static_cast<int>(Next() % static_cast<unsigned>(n)); }
};

/** @brief Token kinds and texts plus syntax error codes: what a scramble must keep. */
std::vector<std::string> Signature(const csharp::SyntaxTree &tree) {
    std::vector<std::string> sig;
    std::vector<const csharp::SyntaxNode *> stack{tree.Root()};
    while (!stack.empty()) {
        const csharp::SyntaxNode *n = stack.back();
        stack.pop_back();
        if (n->IsToken()) {
            sig.push_back(std::to_string(static_cast<int>(n->Kind())) + ":" + csharp::ToUtf8(n->green->text));
            for (const auto *list : {&n->leading, &n->trailing}) {
                for (const csharp::SyntaxTrivia &t : *list) {
                    if (t.Kind() == csharp::SyntaxKind::WhitespaceTrivia || t.Kind() == csharp::SyntaxKind::EndOfLineTrivia) continue;
                    sig.push_back("t" + std::to_string(static_cast<int>(t.Kind())));
                }
            }
            continue;
        }
        for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) stack.push_back(*it);
    }
    for (const csharp::Diagnostic &d : tree.Diagnostics()) sig.push_back("e" + d.Id());
    return sig;
}

/** @brief A copy of `src` with the whitespace between main-tree tokens perturbed (level 2: also joins lines). */
std::string Scramble(const std::string &src, Rng &rng, int level) {
    auto tree = csharp::SyntaxTree::Parse(src);
    const std::u16string &text = tree->Text16();
    std::u16string out;
    int pos = 0;
    std::vector<const csharp::SyntaxNode *> tokens;
    std::vector<const csharp::SyntaxNode *> stack{tree->Root()};
    while (!stack.empty()) {
        const csharp::SyntaxNode *n = stack.back();
        stack.pop_back();
        if (n->IsToken()) {
            tokens.push_back(n);
            continue;
        }
        for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) stack.push_back(*it);
    }
    for (size_t i = 0; i + 1 < tokens.size(); i++) {
        const csharp::SyntaxNode *a = tokens[i];
        const csharp::SyntaxNode *b = tokens[i + 1];
        int from = a->Span().end;
        int to = b->Span().start;
        if (to < from) continue;
        auto in_string = [](const csharp::SyntaxNode *t) {
            using K = csharp::SyntaxKind;
            K k = t->Kind();
            K pk = t->parent ? t->parent->Kind() : K::None;
            return k == K::InterpolatedStringStartToken || k == K::InterpolatedVerbatimStringStartToken ||
                   k == K::InterpolatedSingleLineRawStringStartToken || k == K::InterpolatedMultiLineRawStringStartToken ||
                   k == K::InterpolatedStringEndToken || k == K::InterpolatedRawStringEndToken ||
                   k == K::InterpolatedStringTextToken || pk == K::Interpolation || pk == K::InterpolationFormatClause ||
                   pk == K::InterpolatedStringText;
        };
        if (in_string(a) || in_string(b)) continue;
        if (to == from) {
            if (a->green->text.empty() || b->green->text.empty() || rng.Below(4) != 0) continue;
            out.append(text, static_cast<size_t>(pos), static_cast<size_t>(from - pos));
            out += u" ";
            pos = to;
            continue;
        }
        bool only_ws = true;
        int eols = 0;
        for (const auto *list : {&a->trailing, &b->leading}) {
            for (const csharp::SyntaxTrivia &t : *list) {
                if (t.Kind() == csharp::SyntaxKind::EndOfLineTrivia) {
                    eols++;
                } else if (t.Kind() != csharp::SyntaxKind::WhitespaceTrivia) {
                    only_ws = false;
                }
            }
        }
        // Interpolated and raw strings keep their insides.
        if (!only_ws || b->Kind() == csharp::SyntaxKind::EndOfFileToken) continue;
        std::u16string ws;
        auto indent = [&]() {
            int r = rng.Below(6);
            if (r == 0) return std::u16string();
            if (r == 1) return std::u16string(static_cast<size_t>(rng.Below(4)), u'\t');
            return std::u16string(static_cast<size_t>(rng.Below(17)), u' ');
        };
        if (eols == 0) {
            int r = rng.Below(5);
            if (r == 0) continue;  // keep
            ws = r == 1 ? u"" : r == 2 ? u" " : r == 3 ? u"   " : u"\t";
            if (ws.empty()) {
                // Only next to brackets and separators: anything else may merge into one token.
                auto safe = [](char16_t c) { return std::u16string_view(u"(){}[],;").find(c) != std::u16string_view::npos; };
                char16_t last = a->green->text.empty() ? u' ' : a->green->text.back();
                char16_t first = b->green->text.empty() ? u' ' : b->green->text.front();
                if (!safe(last) && !safe(first)) ws = u" ";
            }
        } else {
            std::u16string nl = text.find(u"\r\n", static_cast<size_t>(from)) == static_cast<size_t>(from) ? u"\r\n" : u"\n";
            if (level >= 2 && eols == 1 && rng.Below(8) == 0) {
                ws = u" ";
            } else {
                int lines = eols;
                if (rng.Below(6) == 0) lines = 1 + rng.Below(3);
                for (int k = 0; k < lines; k++) {
                    if (rng.Below(10) == 0) ws += u"  ";  // trailing blanks
                    ws += nl;
                }
                ws += indent();
            }
        }
        out.append(text, static_cast<size_t>(pos), static_cast<size_t>(from - pos));
        out += ws;
        pos = to;
    }
    out.append(text, static_cast<size_t>(pos), std::u16string::npos);
    bool bom = src.size() >= 3 && src.compare(0, 3, "\xEF\xBB\xBF") == 0;
    return (bom ? std::string("\xEF\xBB\xBF") : std::string()) + csharp::ToUtf8(out);
}

int ScrambleFiles(int argc, char **argv) {
    if (argc < 3) return 2;
    unsigned long long seed = std::strtoull(argv[2], nullptr, 10);
    std::string out_dir;
    std::vector<std::string> files;
    bool stdin_list = false;
    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (a == "--stdin-list") {
            stdin_list = true;
        } else {
            files.push_back(a);
        }
    }
    if (stdin_list) {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty()) files.push_back(line);
        }
    }
    for (const std::string &f : files) {
        bool ok = false;
        std::string src = ReadFile(f, &ok);
        if (!ok) continue;
        auto original = csharp::SyntaxTree::Parse(src);
        std::vector<std::string> sig = Signature(*original);
        Rng rng{seed ^ std::hash<std::string>()(f)};
        std::string kept;
        for (int level : {2, 1, 1}) {
            std::string s = Scramble(src, rng, level);
            auto again = csharp::SyntaxTree::Parse(s);
            if (Signature(*again) == sig) {
                kept = s;
                break;
            }
            if (getenv("SCRAMBLE_DEBUG")) {
                std::vector<std::string> sb = Signature(*again);
                for (size_t k = 0; k < sig.size() && k < sb.size(); k++) {
                    if (sig[k] != sb[k]) {
                        std::fprintf(stderr, "diff at %zu: %s vs %s\n", k, sig[k].c_str(), sb[k].c_str());
                        break;
                    }
                }
                std::fprintf(stderr, "sizes %zu %zu\n", sig.size(), sb.size());
            }
        }
        if (kept.empty()) {
            std::printf("skip %s\n", f.c_str());
            continue;
        }
        std::string dest = out_dir + Absolute(f);
        MakeDirs(dest);
        std::ofstream o(dest, std::ios::binary);
        o << kept;
        std::printf("ok %s\n", f.c_str());
    }
    return 0;
}

struct Case {
    const char *name;
    const char *input;
    const char *expected;
};

const Case kCases[] = {
#include "csharp_format_test_cases.inc"
};

int RunCases() {
    int failed = 0;
    int n = 0;
    for (const Case &c : kCases) {
        n++;
        csfmt::Result r = csfmt::Format(c.input);
        if (!r.ok || r.text != c.expected) {
            failed++;
            std::printf("FAIL %s%s%s\n--- input\n%s\n--- expected\n%s\n--- got\n%s\n", c.name, r.ok ? "" : ": ",
                        r.ok ? "" : r.error.c_str(), c.input, c.expected, r.text.c_str());
            continue;
        }
        // Idempotence.
        csfmt::Result again = csfmt::Format(r.text);
        if (!again.ok || again.text != r.text) {
            failed++;
            std::printf("FAIL %s: not idempotent\n%s\n", c.name, again.text.c_str());
        }
    }
    // A tree as deep as the code is long (the engine recurses on it): a 50,000-term concatenation, already
    // formatted, comes back as it is.
    {
        n++;
        std::string deep = "class C { string M() { return \"a\"";
        for (int i = 1; i < 50000; i++) deep += " + \"a\"";
        deep += "; } }\n";
        csfmt::Result r = csfmt::Format(deep);
        if (!r.ok || r.text != deep) {
            failed++;
            std::printf("FAIL deep concatenation%s%s\n", r.ok ? "" : ": ", r.error.c_str());
        }
    }
    std::printf("%d/%d cases passed\n", n - failed, n);
    return failed == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--format") == 0) return FormatFiles(argc, argv);
    if (argc >= 2 && std::strcmp(argv[1], "--scramble") == 0) return ScrambleFiles(argc, argv);
    return RunCases();
}
