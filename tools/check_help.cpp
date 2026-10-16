// Consistency checks for the built-in help workspace (help/).
//
// Run by `just help-check` (and by `just test`). Five checks:
//
//   1. Freshness -- every help/*.html is exactly what mep's own exporter
//      produces from its .org source right now. Done by re-exporting into a
//      temp directory and comparing bytes, NOT by comparing mtimes: git does
//      not preserve mtimes, so a fresh clone would fail an mtime check and a
//      rebased branch could pass one while shipping stale HTML.
//   2. Structure -- every page declares a <title> and a help-section, the two
//      things the Help sidebar reads to place it.
//   3. Bytes -- no exported page carries a control byte, which is what an
//      unsubstituted exporter placeholder reaches the reader as.
//   4. Links -- every internal href in a page resolves to a file that exists,
//      and no Org source has a link straddling a line break (which the exporter
//      silently renders as literal text).
//   5. Coverage -- how much of mep's command and <leader> surface the manual
//      actually mentions. Reported always; enforced only under --strict,
//      because the manual is being written incrementally and a hard gate here
//      would just be permanently red until the last page lands.
//
// Built as the `mep-check-help` host tool; replaces scripts/check_help.py
// (same checks, same messages, same exit codes).
//
// Usage: mep-check-help <path-to-mep-binary> [--strict] [--min-coverage N]
//                       [--repo DIR]

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "py_text.h"

#ifdef _WIN32
#include <process.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

#ifndef MEP_SOURCE_DIR
#define MEP_SOURCE_DIR "."
#endif

namespace {

namespace fs = std::filesystem;
using pytext::U;
using Str = std::u32string;

std::vector<std::string> g_problems;

void Fail(const std::string &message) { g_problems.push_back(message); }

// sorted(HELP.glob("*<ext>")), as file names.
std::vector<fs::path> Glob(const fs::path &dir, const std::string &ext) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ext) out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end(),
              [](const fs::path &a, const fs::path &b) { return a.filename().string() < b.filename().string(); });
    return out;
}

std::string ReadTextUtf8(const fs::path &p) {
    Str text;
    pytext::ReadText(p.string(), &text);
    return pytext::Encode(text);
}

struct RunResult {
    int returncode = 0;
    std::string err;
};

// subprocess.run([mep, "--export-org", src, out], capture_output=True)
RunResult RunExport(const std::string &mep, const fs::path &src, const fs::path &out, const fs::path &tmp) {
    RunResult r;
    const fs::path err_path = tmp / ".stderr";
    const fs::path out_path = tmp / ".stdout";
#ifdef _WIN32
    const std::string cmd = "\"\"" + mep + "\" --export-org \"" + src.string() + "\" \"" + out.string() + "\" >\"" +
                            out_path.string() + "\" 2>\"" + err_path.string() + "\"\"";
    r.returncode = std::system(cmd.c_str());
#else
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 1, out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_addopen(&actions, 2, err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    std::string a0 = mep, a1 = "--export-org", a2 = src.string(), a3 = out.string();
    char *argv[] = {a0.data(), a1.data(), a2.data(), a3.data(), nullptr};
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, mep.c_str(), &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        r.returncode = 127;
        r.err = "cannot run " + mep;
        return r;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
    }
    if (WIFEXITED(status)) {
        r.returncode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        r.returncode = -WTERMSIG(status);
    }
#endif
    std::string err;
    pytext::ReadBytes(err_path.string(), &err);
    r.err = pytext::Encode(pytext::Strip(pytext::UniversalNewlines(pytext::Decode(err))));
    return r;
}

// Re-export every source and compare against the committed HTML.
void CheckFreshness(const std::string &mep, const fs::path &help) {
    std::error_code ec;
    std::random_device rng;
    fs::path tmp;
    for (int i = 0; i < 100; ++i) {
        tmp = fs::temp_directory_path(ec) / ("mep-check-help-" + std::to_string(rng()));
        if (fs::create_directory(tmp, ec)) break;
    }
    for (const fs::path &src : Glob(help, ".org")) {
        const std::string name = src.filename().string();
        if (name.rfind("_", 0) == 0) continue;
        fs::path committed = src;
        committed.replace_extension(".html");
        const std::string committed_name = committed.filename().string();
        if (!fs::exists(committed)) {
            Fail(name + ": no exported " + committed_name + " (run `just help`)");
            continue;
        }
        const fs::path out = tmp / committed_name;
        const RunResult r = RunExport(mep, src, out, tmp);
        if (r.returncode != 0) {
            Fail(name + ": export failed: " + (r.err.empty() ? std::to_string(r.returncode) : r.err));
            continue;
        }
        std::string fresh, old;
        pytext::ReadBytes(out.string(), &fresh);
        pytext::ReadBytes(committed.string(), &old);
        if (fresh != old) {
            Fail(committed_name + " is stale -- it differs from what " + name + " exports now (run `just help`)");
        }
    }
    fs::remove_all(tmp, ec);
}

// No exported page may carry a control byte.
//
// The exporter hides each construct's generated markup behind a `\0M<n>\0`
// placeholder while it converts the rest of the line, and substitutes them
// all back at the end. A placeholder that survives that reaches the reader
// as a literal NUL -- and makes git classify the page as binary, so it stops
// merging it and every branch that touches the page conflicts on it
// instead. Cheap to assert, and the symptom is otherwise invisible in a diff.
void CheckExportedBytes(const fs::path &help) {
    for (const fs::path &page : Glob(help, ".html")) {
        std::string data;
        pytext::ReadBytes(page.string(), &data);
        std::set<unsigned> bad;
        for (char ch : data) {
            const auto b = static_cast<unsigned char>(ch);
            if (b < 9 || (b > 13 && b < 32)) bad.insert(b);
        }
        if (bad.empty()) continue;
        const auto first = data.find(static_cast<char>(*bad.begin()));
        std::vector<std::string> hexes;
        for (unsigned b : bad) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "0x%x", b);
            hexes.emplace_back(buf);
        }
        std::sort(hexes.begin(), hexes.end());
        std::string list = "[";
        for (size_t i = 0; i < hexes.size(); ++i) list += (i ? ", '" : "'") + hexes[i] + "'";
        list += "]";
        Fail(page.filename().string() + ": control byte(s) " + list + " in the exported HTML (first at offset " +
             std::to_string(first) +
             ") -- an unsubstituted exporter placeholder, not something the source can contain");
    }
}

// re.search(r"<title>\s*\S.*?</title>", text, re.I | re.S)
bool HasTitle(const Str &text) {
    size_t pos = 0;
    while ((pos = pytext::FindNoCase(text, U"<title>", pos)) != Str::npos) {
        size_t i = pos + 7;
        while (i < text.size() && pytext::IsSpace(text[i])) ++i;
        if (i < text.size() && pytext::FindNoCase(text, U"</title>", i + 1) != Str::npos) return true;
        pos += 1;
    }
    return false;
}

// re.search(r'<meta\s+name="help-section"\s+content="[^"]+"', text, re.I)
bool HasSection(const Str &text) {
    size_t pos = 0;
    while ((pos = pytext::FindNoCase(text, U"<meta", pos)) != Str::npos) {
        size_t i = pos + 5;
        auto ws = [&]() {
            const size_t start = i;
            while (i < text.size() && pytext::IsSpace(text[i])) ++i;
            return i > start;
        };
        if (ws() && pytext::AtNoCase(text, i, U"name=\"help-section\"")) {
            i += 19;
            if (ws() && pytext::AtNoCase(text, i, U"content=\"")) {
                i += 9;
                const size_t close = text.find(U'"', i);
                if (close != Str::npos && close > i) return true;
            }
        }
        pos += 1;
    }
    return false;
}

// Every page needs a title and a section for the sidebar to place it.
void CheckStructure(const fs::path &help) {
    for (const fs::path &page : Glob(help, ".html")) {
        Str text;
        pytext::ReadText(page.string(), &text);
        const std::string name = page.filename().string();
        if (!HasTitle(text)) Fail(name + ": no <title> (the sidebar label)");
        if (!HasSection(text)) {
            Fail(name +
                 ": no help-section meta -- add `#+HTML_HEAD: <meta name=\"help-section\" ...>` to " +
                 page.stem().string() + ".org (see help/_template.org)");
        }
    }
}

// re.sub(r"<tag.*?</tag>", "", text, flags=re.S)
Str RemoveBlocks(const Str &text, const Str &open, const Str &close) {
    Str out;
    size_t i = 0;
    while (i < text.size()) {
        if (pytext::At(text, i, open)) {
            const size_t end = text.find(close, i + open.size());
            if (end != Str::npos) {
                i = end + close.size();
                continue;
            }
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

// Internal hrefs must resolve. External and in-page ones are skipped.
//
// Code blocks are stripped first: a page documenting HTML legitimately
// contains href="..." inside a <pre>, and that is sample text, not a link.
void CheckLinks(const fs::path &help) {
    for (const fs::path &page : Glob(help, ".html")) {
        Str text;
        pytext::ReadText(page.string(), &text);
        text = RemoveBlocks(text, U"<pre", U"</pre>");
        text = RemoveBlocks(text, U"<code", U"</code>");
        size_t pos = 0;
        const Str kw = U"href=\"";
        while ((pos = text.find(kw, pos)) != Str::npos) {
            const size_t start = pos + kw.size();
            const size_t close = text.find(U'"', start);
            if (close == Str::npos || close == start) {
                pos += 1;
                continue;
            }
            const Str href = text.substr(start, close - start);
            pos = close + 1;
            if (pytext::At(href, 0, U"http://") || pytext::At(href, 0, U"https://") ||
                pytext::At(href, 0, U"mailto:") || pytext::At(href, 0, U"#")) {
                continue;
            }
            const Str file = href.substr(0, href.find(U'#'));
            std::error_code ec;
            if (!fs::exists(help / fs::path(pytext::Encode(file)), ec)) {
                Fail(page.filename().string() + ": broken link to " + pytext::Encode(href));
            }
        }
    }
}

// An Org link must not straddle a line break.
//
// mep's exporter converts inline markup one line at a time, so
// `[[file:x.html][some\ndescription]]` is never recognised -- it renders as
// the literal bracket soup instead of a link, on a page that otherwise looks
// fine. Nothing downstream can catch it (it never becomes an href), so it has
// to be caught in the source.
void CheckSourceLinks(const fs::path &help) {
    for (const fs::path &src : Glob(help, ".org")) {
        Str text;
        pytext::ReadText(src.string(), &text);
        // \[\[[^\]]*\]\[[^\]]*\n[^\]]*\]\]
        size_t pos = 0;
        while ((pos = text.find(U"[[", pos)) != Str::npos) {
            const size_t first_close = text.find(U']', pos + 2);
            bool ok = first_close != Str::npos && pytext::At(text, first_close, U"][");
            size_t second_close = 0;
            if (ok) {
                second_close = text.find(U']', first_close + 2);
                ok = second_close != Str::npos && pytext::At(text, second_close, U"]]");
            }
            if (ok) {
                const Str desc = text.substr(first_close + 2, second_close - first_close - 2);
                ok = desc.find(U'\n') != Str::npos;
            }
            if (!ok) {
                pos += 1;
                continue;
            }
            const size_t end = second_close + 2;
            Str snippet = pytext::CollapseWs(text.substr(pos, end - pos)).substr(0, 60);
            Fail(src.filename().string() + ": link split across lines -- " + pytext::Encode(snippet));
            pos = end;
        }
    }
}

// set(re.findall(r"<prefix>([...]+)'<suffix>")) for the two coverage patterns.
std::set<std::string> Commands(const std::string &main) {
    std::set<std::string> out;
    const std::string kw = "mep.command('";
    size_t pos = 0;
    while ((pos = main.find(kw, pos)) != std::string::npos) {
        size_t i = pos + kw.size();
        const size_t start = i;
        while (i < main.size() && (std::isalnum(static_cast<unsigned char>(main[i])) || main[i] == '_')) ++i;
        if (i > start && i < main.size() && main[i] == '\'') {
            out.insert(main.substr(start, i - start));
            pos = i + 1;
        } else {
            pos += 1;
        }
    }
    return out;
}

std::set<std::string> Leaders(const Str &main) {
    // mep\.leader_map\('([^']+)'\s*,
    std::set<std::string> out;
    const Str kw = U"mep.leader_map('";
    size_t pos = 0;
    while ((pos = main.find(kw, pos)) != Str::npos) {
        const size_t start = pos + kw.size();
        const size_t close = main.find(U'\'', start);
        bool ok = close != Str::npos && close > start;
        size_t j = 0;
        if (ok) {
            j = close + 1;
            while (j < main.size() && pytext::IsSpace(main[j])) ++j;
            ok = j < main.size() && main[j] == U',';
        }
        if (ok) {
            out.insert(pytext::Encode(main.substr(start, close - start)));
            pos = j + 1;
        } else {
            pos += 1;
        }
    }
    return out;
}

std::string JoinFirst12(const std::vector<std::string> &items) {
    std::string out;
    for (size_t i = 0; i < items.size() && i < 12; ++i) out += (i ? ", " : "") + items[i];
    if (items.size() > 12) out += " ...";
    return out;
}

// repr(float): the shortest round-tripping digits, in fixed notation for
// decimal exponents in [-4, 16) (always with a ".0" if integral) and
// scientific otherwise -- 95.0, 90.5, 1e+16.
std::string PyFloatRepr(double v) {
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
    char buf[64];
    int prec = 1;
    for (; prec < 17; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*e", prec - 1, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    std::snprintf(buf, sizeof buf, "%.*e", prec - 1, v);
    const int exp = std::atoi(std::strchr(buf, 'e') + 1);
    if (exp < -4 || exp >= 16) {
        std::string s = buf;  // mantissa already minimal; Python drops a bare "."
        return s;
    }
    std::snprintf(buf, sizeof buf, "%.*f", std::max(0, prec - 1 - exp), v);
    std::string s = buf;
    if (s.find('.') == std::string::npos) s += ".0";
    return s;
}

// How much of the command/binding surface the manual mentions.
void Coverage(const fs::path &repo, const fs::path &help, bool strict, double minimum) {
    Str main_u;
    pytext::ReadText((repo / "src" / "main.cpp").string(), &main_u);
    const std::string main = pytext::Encode(main_u);
    const std::set<std::string> commands = Commands(main);
    // Require a comma right after the closing quote: the learning games
    // register theirs as `leader_map('og' .. g.leader, ...)`, and a looser
    // pattern captures the bare prefix `og` as though it were a binding.
    const std::set<std::string> leaders = Leaders(main_u);
    std::string prose;
    bool first = true;
    for (const fs::path &p : Glob(help, ".org")) {
        if (!first) prose += "\n";
        first = false;
        prose += ReadTextUtf8(p);
    }

    std::vector<std::string> missing_cmds, missing_keys;
    for (const std::string &c : commands) {
        if (prose.find(c) == std::string::npos) missing_cmds.push_back(c);
    }
    for (const std::string &k : leaders) {
        if (prose.find("<leader>" + k) == std::string::npos) missing_keys.push_back(k);
    }
    const size_t documented = (commands.size() - missing_cmds.size()) + (leaders.size() - missing_keys.size());
    const size_t total = commands.size() + leaders.size();
    const double pct = total ? 100.0 * static_cast<double>(documented) / static_cast<double>(total) : 100.0;

    std::printf("coverage: %zu/%zu (%.1f%%) of %zu commands and %zu <leader> bindings are mentioned in help/\n",
                documented, total, pct, commands.size(), leaders.size());
    if (!missing_cmds.empty()) {
        std::printf("  undocumented commands (%zu): %s\n", missing_cmds.size(), JoinFirst12(missing_cmds).c_str());
    }
    if (!missing_keys.empty()) {
        std::printf("  undocumented <leader> keys (%zu): %s\n", missing_keys.size(), JoinFirst12(missing_keys).c_str());
    }
    if (strict && pct < minimum) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "coverage %.1f%% is below the required ", pct);
        Fail(std::string(buf) + PyFloatRepr(minimum) + "%");
    }
}

int Usage() {
    std::fprintf(stderr, "usage: mep-check-help [--strict] [--min-coverage MIN_COVERAGE] [--repo DIR] mep\n");
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    std::string mep;
    bool strict = false;
    double minimum = 95.0;
    fs::path repo = MEP_SOURCE_DIR;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&](const std::string &flag, std::string *out) {
            if (a == flag) {
                if (i + 1 >= argc) return false;
                *out = argv[++i];
                return true;
            }
            if (a.rfind(flag + "=", 0) == 0) {
                *out = a.substr(flag.size() + 1);
                return true;
            }
            return false;
        };
        std::string v;
        if (a == "--strict") {
            strict = true;
        } else if (value("--min-coverage", &v)) {
            char *end = nullptr;
            minimum = std::strtod(v.c_str(), &end);
            if (v.empty() || *end) return Usage();
        } else if (value("--repo", &v)) {
            repo = v;
        } else if (a == "-h" || a == "--help") {
            Usage();
            return 0;
        } else if (!a.empty() && a[0] == '-' && a != "-") {
            return Usage();
        } else if (mep.empty()) {
            mep = a;
        } else {
            return Usage();
        }
    }
    if (mep.empty()) return Usage();

    std::error_code ec;
    if (!fs::is_regular_file(mep, ec)) {
        std::fprintf(stderr, "check_help: no mep binary at %s\n", mep.c_str());
        return 2;
    }
    const fs::path help = repo / "help";

    CheckFreshness(mep, help);
    CheckExportedBytes(help);
    CheckStructure(help);
    CheckLinks(help);
    CheckSourceLinks(help);
    Coverage(repo, help, strict, minimum);
    std::fflush(stdout);

    if (!g_problems.empty()) {
        std::fprintf(stderr, "\ncheck_help: %zu problem(s):\n", g_problems.size());
        for (const std::string &p : g_problems) std::fprintf(stderr, "  - %s\n", p.c_str());
        return 1;
    }
    std::printf("check_help: help/ is consistent\n");
    return 0;
}
