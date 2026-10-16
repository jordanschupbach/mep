#include "tex/tex_backend.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace {

std::vector<std::string> SplitWords(const std::string &s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

std::string ReplaceAll(std::string s, const std::string &from, const std::string &to) {
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
    return s;
}

std::string ShellQuote(const std::string &s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

bool StartsWith(const std::string &s, const std::string &p) { return s.compare(0, p.size(), p) == 0; }

}  // namespace

const std::vector<std::string> &TexSystemCommandNames() {
    static const std::vector<std::string> kNames = {"tectonic", "pdflatex", "xelatex", "lualatex", "latexmk"};
    return kNames;
}

TexCommand TexCompileCommand(const std::string &engine, const std::string &system_command,
                             const std::string &builtin_tool, const std::string &tex, const std::string &outdir,
                             bool keep_logs) {
    TexCommand c;
    if (engine == "builtin") {
        c.argv = {builtin_tool, "compile", tex, "--outdir", outdir};
        c.exe = builtin_tool;
        c.reruns = true;
        c.leaves_files = true;
        return c;
    }
    const std::string cmd = system_command.empty() ? "tectonic" : system_command;
    if (cmd == "tectonic") {
        c.argv = {"tectonic", "-X", "compile", tex, "--outfmt", "pdf", "--outdir", outdir};
        if (keep_logs) c.argv.push_back("--keep-logs");
        c.reruns = true;
    } else if (cmd == "latexmk") {
        c.argv = {"latexmk", "-pdf", "-interaction=nonstopmode", "-halt-on-error", "-file-line-error",
                  "-outdir=" + outdir, tex};
        c.reruns = true;
        c.leaves_files = true;
    } else if (cmd == "pdflatex" || cmd == "xelatex" || cmd == "lualatex") {
        c.argv = {cmd, "-interaction=nonstopmode", "-halt-on-error", "-file-line-error",
                  "-output-directory=" + outdir, tex};
        c.leaves_files = true;
    } else {
        // An argv typed as one string. Unknown to the table, so assumed to
        // rerun itself (a wrapper like latexmk is the usual reason to type
        // one) and to leave its files.
        bool has_file = false;
        for (const std::string &w : SplitWords(cmd)) {
            if (w.find("%f") != std::string::npos) has_file = true;
            c.argv.push_back(ReplaceAll(ReplaceAll(w, "%f", tex), "%o", outdir));
        }
        if (c.argv.empty()) return TexCompileCommand(engine, "tectonic", builtin_tool, tex, outdir, keep_logs);
        if (!has_file) c.argv.push_back(tex);
        c.reruns = true;
        c.leaves_files = true;
    }
    c.exe = c.argv[0];
    return c;
}

bool TexLogWantsRerun(const std::string &log_text) {
    return log_text.find("Rerun to get") != std::string::npos ||
           log_text.find("Label(s) may have changed") != std::string::npos;
}

std::string TexErrorLine(const std::vector<std::string> &lines) {
    for (const std::string &l : lines) {
        if (!StartsWith(l, "error: ")) continue;
        std::string m = l.substr(7);
        if (StartsWith(m, "something bad happened") || StartsWith(m, "the XeTeX engine")) continue;
        while (!m.empty() && (m[0] == '!' || m[0] == ' ')) m.erase(0, 1);
        return m;
    }
    for (const std::string &l : lines)
        if (StartsWith(l, "! ")) return l.substr(2);
    for (const std::string &l : lines) {
        // `file:12: message`, as -file-line-error writes it.
        const size_t c1 = l.find(':');
        if (c1 == std::string::npos || c1 == 0) continue;
        size_t d = c1 + 1;
        while (d < l.size() && l[d] >= '0' && l[d] <= '9') d++;
        if (d > c1 + 1 && l.compare(d, 2, ": ") == 0) return l.substr(d + 2);
    }
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        if (it->find_first_not_of(" \t") == std::string::npos) continue;
        if (StartsWith(*it, "warning:") || StartsWith(*it, "note:")) continue;
        return *it;
    }
    return "";
}

bool TexCompile(const std::string &engine, const std::string &system_command, const std::string &builtin_tool,
                const std::string &tex, const std::string &outdir, const std::string &cwd, std::string *err) {
    namespace fs = std::filesystem;
    const TexCommand c = TexCompileCommand(engine, system_command, builtin_tool, tex, outdir, false);
    const std::string base = (fs::path(outdir) / fs::path(tex).stem()).string();
    const std::string pdf = base + ".pdf", log = base + ".log";
    std::error_code ec;
    fs::remove(pdf, ec);
    std::string line = "cd " + ShellQuote(cwd) + " &&";
    for (const std::string &a : c.argv) line += " " + ShellQuote(a);
    line += " 2>&1";
    const int passes = c.reruns ? 1 : 3;
    std::vector<std::string> out;
    int rc = -1;
    for (int pass = 1; pass <= passes; pass++) {
        out.clear();
        FILE *p = popen(line.c_str(), "r");
        if (!p) {
            if (err) *err = "could not start " + c.exe;
            return false;
        }
        char buf[4096];
        std::string cur;
        while (fgets(buf, sizeof buf, p)) {
            cur += buf;
            if (!cur.empty() && cur.back() == '\n') {
                cur.pop_back();
                out.push_back(cur);
                cur.clear();
            }
        }
        if (!cur.empty()) out.push_back(cur);
        rc = pclose(p);
        if (rc != 0 || pass == passes) break;
        std::ifstream lf(log);
        const std::string text((std::istreambuf_iterator<char>(lf)), std::istreambuf_iterator<char>());
        if (!TexLogWantsRerun(text)) break;
    }
    if (c.leaves_files) {
        for (const char *ext : {".aux", ".log", ".out", ".toc", ".nav", ".snm", ".fls", ".fdb_latexmk", ".xdv"})
            fs::remove(base + ext, ec);
    }
    if (rc == 0 && fs::exists(pdf, ec)) return true;
    if (err) {
        *err = TexErrorLine(out);
        if (err->empty()) *err = c.exe + " failed (is it on PATH?)";
    }
    return false;
}
