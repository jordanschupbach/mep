#include "tex/tex_compile.h"

#include "pdf_crypt.h"
#include "tex/gen/tex_gen.h"
#include "tex/tex_backend.h"
#include "tex/tex_bundle.h"
#include "tex/tex_resolver.h"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>

namespace meptex {

namespace fs = std::filesystem;

namespace {

// Bumped when what a format holds changes in a way the engine's own
// checks would not catch.
constexpr const char *kFormatLayout = "mep-tex-fmt-1";

std::string ReadFile(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool IsDir(const std::string &p) {
    std::error_code ec;
    return !p.empty() && fs::is_directory(p, ec);
}

bool IsFile(const std::string &p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
}

std::string Hex(const std::string &bytes) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    for (char ch : bytes) {
        const auto b = static_cast<unsigned char>(ch);
        out.push_back(kDigits[b >> 4]);
        out.push_back(kDigits[b & 15]);
    }
    return out;
}

// Something that changes when a file does: its size and modification time.
std::string Stamp(const std::string &path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    const auto time = fs::last_write_time(path, ec);
    if (ec) return path + ":?";
    return path + ":" + std::to_string(size) + ":" + std::to_string(static_cast<long long>(time.time_since_epoch().count()));
}

std::vector<std::string> SplitPath(const std::string &s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ':')) {
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

std::string RunCapture(const std::string &cmd) {
    std::string out;
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) return out;
    std::array<char, 512> buf{};
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0) out.append(buf.data(), n);
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

// The format's .ini file: what INITEX reads to make it.
std::string IniFor(const std::string &format) {
    if (format == "pdflatex") return "pdflatex.ini";
    if (format == "pdftex") return "pdfetex.ini";
    return "";
}

std::string CanonicalFormat(const std::string &name) {
    if (name == "pdflatex" || name == "latex") return "pdflatex";
    if (name == "pdftex" || name == "pdfetex" || name == "etex" || name == "tex" || name == "plain") return "pdftex";
    return "";
}

std::string FirstErrorLine(const std::string &log_path) {
    std::ifstream in(log_path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("! ", 0) == 0) return line.substr(2);
    }
    return "";
}

// The files whose change means another pass: LaTeX's auxiliary files.
// An .aux that only says how many pages there are does not count.
std::string AuxState(const std::string &base) {
    std::string state;
    for (const char *ext : {".aux", ".toc", ".lof", ".lot", ".out", ".nav", ".snm", ".bbl"}) {
        std::string text = ReadFile(base + ext);
        if (std::string(ext) == ".aux") {
            std::string kept;
            std::stringstream ss(text);
            std::string line;
            while (std::getline(ss, line)) {
                if (line == "\\relax " || line == "\\relax" || line.rfind("\\gdef \\@abspage@last", 0) == 0) continue;
                kept += line + "\n";
            }
            text = kept;
        }
        state += std::string(ext) + "\n" + text;
    }
    return pdfcrypt::Md5(state);
}

std::string ExeIdentity() {
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return Stamp(exe.string());
    return std::string(__DATE__) + " " + __TIME__;
}

}  // namespace

std::vector<std::string> DiscoverTexTrees() {
    std::vector<std::string> trees;
    auto add = [&](const std::string &t) {
        if (!IsDir(t)) return;
        for (const std::string &have : trees) {
            if (have == t) return;
        }
        trees.push_back(t);
    };
    if (const char *env = std::getenv("MEP_TEX_TREES"); env && *env) {
        for (const std::string &t : SplitPath(env)) add(t);
        return trees;
    }
    // An installed TeX Live says where its trees are. The generated files
    // (language.dat, pdftex.map) are in the VAR trees, so they come first.
    if (!RunCapture("command -v kpsewhich 2>/dev/null").empty()) {
        for (const char *var : {"TEXMFHOME", "TEXMFVAR", "TEXMFCONFIG", "TEXMFSYSVAR", "TEXMFSYSCONFIG", "TEXMFLOCAL",
                                "TEXMFDIST"}) {
            const std::string v = RunCapture(std::string("kpsewhich -var-value=") + var + " 2>/dev/null");
            for (const std::string &t : SplitPath(v)) add(t);
        }
        if (!trees.empty()) return trees;
    }
    // The usual places: Debian/Ubuntu, Fedora/Arch, a TeX Live install.
    add("/var/lib/texmf");
    add("/usr/share/texlive/texmf-dist");
    add("/usr/share/texmf-dist");
    add("/usr/share/texmf");
    for (const std::string root : {"/usr/local/texlive", "/opt/texlive"}) {
        std::error_code ec;
        std::vector<std::string> years;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (!name.empty() && name[0] >= '0' && name[0] <= '9') years.push_back(it->path().string());
        }
        std::sort(years.rbegin(), years.rend());
        if (!years.empty()) {
            add(years.front() + "/texmf-var");
            add(years.front() + "/texmf-dist");
        }
    }
    return trees;
}

std::string DefaultTexCacheDir() {
    if (const char *x = std::getenv("XDG_CACHE_HOME"); x && *x) return std::string(x) + "/mep/tex";
    if (const char *h = std::getenv("HOME"); h && *h) return std::string(h) + "/.cache/mep/tex";
    return (fs::temp_directory_path() / "mep-tex-cache").string();
}

std::string FormatForDocument(const std::string &text) {
    if (text.rfind("%&", 0) == 0) {
        size_t e = 2;
        while (e < text.size() && text[e] != ' ' && text[e] != '\n' && text[e] != '\r') e++;
        const std::string f = CanonicalFormat(text.substr(2, e - 2));
        if (!f.empty()) return f;
    }
    // \documentclass outside a comment, near the top
    std::stringstream ss(text.substr(0, 1 << 16));
    std::string line;
    while (std::getline(ss, line)) {
        const size_t pct = line.find('%');
        const std::string code = line.substr(0, pct);
        if (code.find("\\documentclass") != std::string::npos || code.find("\\documentstyle") != std::string::npos)
            return "pdflatex";
    }
    return "pdftex";
}

std::vector<std::string> TexTreesFor(const std::string &packages_in, const std::string &cache_dir, bool verbose,
                                     std::string *err) {
    std::string packages = packages_in;
    if (packages.empty()) {
        const char *e = std::getenv("MEP_TEX_PACKAGES");
        packages = e && *e ? e : "bundle";
    }
    if (packages != "bundle" && packages != "bundle+system" && packages != "system") {
        *err = "unknown package source `" + packages + "' (bundle, bundle+system or system)";
        return {};
    }
    std::string bundle_tree;
    if (packages != "system") {
        const std::string bundle = FindTexBundle();
        if (!bundle.empty()) {
            bundle_tree = UnpackTexBundle(bundle, cache_dir, verbose, err);
            if (bundle_tree.empty()) return {};
        }
    }
    // bundle+system: the system's files first -- what it has installed is
    // why it was asked for -- and the bundle for the rest.
    std::vector<std::string> trees;
    if (packages != "bundle" || bundle_tree.empty()) trees = DiscoverTexTrees();
    if (!bundle_tree.empty()) trees.push_back(bundle_tree);
    if (trees.empty()) {
        *err = packages == "system"
                   ? "no TeX installation found: install TeX Live, or name trees with --tree DIR or MEP_TEX_TREES "
                     "(or set tex_packages to bundle in :MepSettings)"
                   : "mep's TeX bundle (tex-bundle.mtb) is not installed and no TeX installation was found "
                     "(build it with `just tex-bundle`, or set tex_engine to system in :MepSettings)";
        return {};
    }
    if (verbose && packages != "system" && bundle_tree.empty())
        std::fprintf(stderr, "mep-tex: no TeX bundle installed; using the system's TeX trees\n");
    return trees;
}

std::string EnsureFormat(const std::string &format, const std::vector<std::string> &trees, const std::string &cache_dir,
                         bool rebuild, bool verbose, std::string *err) {
    const std::string ini = IniFor(format);
    if (ini.empty()) {
        *err = "unknown format `" + format + "' (mep-tex makes pdflatex and pdftex)";
        return "";
    }
    Resolver resolver;
    for (const std::string &t : trees) resolver.add_tree(t);
    const std::string ini_path = resolver.find(ini, kTexFile);
    if (ini_path.empty()) {
        *err = "cannot find " + ini + " in the TeX trees";
        return "";
    }
    // The cache key: the engine build, the format, and the trees as they are now.
    std::string key = std::string(kFormatLayout) + "\n" + ExeIdentity() + "\n" + format + "\n" + Stamp(ini_path) + "\n";
    for (const std::string &t : trees) key += IsFile(t + "/ls-R") ? Stamp(t + "/ls-R") + "\n" : Stamp(t) + "\n";
    for (const char *f : {"latex.ltx", "pdftexconfig.tex", "language.dat", "plain.tex"}) {
        const std::string p = resolver.find(f, kTexFile);
        if (!p.empty()) key += Stamp(p) + "\n";
    }
    const std::string dir = cache_dir + "/formats/" + format + "-" + Hex(pdfcrypt::Md5(key)).substr(0, 16);
    const std::string fmt = dir + "/" + format + ".fmt";
    std::error_code ec;
    if (!rebuild && IsFile(fmt)) return dir;
    if (rebuild) fs::remove_all(dir, ec);

    // Made in a directory of its own, then moved into place whole, so a
    // half-made format is never found and two builds do not collide.
    const std::string tmp = dir + ".tmp" + std::to_string(::getpid());
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    if (ec) {
        *err = "cannot make " + tmp + ": " + ec.message();
        return "";
    }
    if (verbose) std::fprintf(stderr, "mep-tex: making the %s format (once; it is cached in %s)\n", format.c_str(), dir.c_str());
    {
        auto engine = std::make_unique<Engine>();
        engine->opt.ini = true;
        engine->opt.first_line = ini;
        engine->opt.format_dirs = {tmp};
        engine->opt.output_dir = tmp;
        engine->opt.interaction = tc::batch_mode;
        engine->opt.quiet = true;
        engine->resolver = &resolver;
        engine->run();
    }
    const std::string made = tmp + "/" + fs::path(ini).stem().string() + ".fmt";
    if (!IsFile(made)) {
        const std::string log = tmp + "/" + fs::path(ini).stem().string() + ".log";
        const std::string why = FirstErrorLine(log);
        *err = "making the " + format + " format failed" + (why.empty() ? "" : ": " + why) + " (see " + log + ")";
        return "";
    }
    if (made != tmp + "/" + format + ".fmt") fs::rename(made, tmp + "/" + format + ".fmt", ec);
    fs::rename(tmp, dir, ec);
    if (!ec) {
        // Old formats of this name (an older mep-tex, a changed tree)
        // are dead weight: keep the newest few, for other tree sets.
        std::vector<std::pair<fs::file_time_type, fs::path>> old;
        std::error_code ec2;
        for (fs::directory_iterator it(cache_dir + "/formats", ec2), end; !ec2 && it != end; it.increment(ec2)) {
            const std::string name = it->path().filename().string();
            if (name.rfind(format + "-", 0) == 0 && it->path() != fs::path(dir) && name.find(".tmp") == std::string::npos)
                old.emplace_back(fs::last_write_time(it->path(), ec2), it->path());
        }
        std::sort(old.rbegin(), old.rend());
        for (size_t k = 2; k < old.size(); k++) fs::remove_all(old[k].second, ec2);
    }
    if (ec) {
        // another build got there first: use its format
        fs::remove_all(tmp, ec);
        if (!IsFile(fmt)) {
            *err = "cannot store the format in " + dir;
            return "";
        }
    }
    return dir;
}

int CompileDocument(const CompileOptions &opt, std::string *err) {
    if (!IsFile(opt.tex)) {
        *err = "no such file: " + opt.tex;
        return 1;
    }
    const fs::path tex = fs::absolute(opt.tex);
    const std::string texdir = tex.parent_path().string();
    const std::string outdir = opt.outdir.empty() ? texdir : fs::absolute(opt.outdir).string();
    std::error_code ec;
    fs::create_directories(outdir, ec);

    const std::string cache = opt.cache_dir.empty() ? DefaultTexCacheDir() : opt.cache_dir;
    std::vector<std::string> trees = opt.trees;
    if (trees.empty()) {
        trees = TexTreesFor(opt.packages, cache, opt.verbose, err);
        if (trees.empty()) return 1;
    }
    const std::string format = opt.format.empty() ? FormatForDocument(ReadFile(tex.string())) : CanonicalFormat(opt.format);
    if (format.empty()) {
        *err = "unknown format `" + opt.format + "' (mep-tex makes pdflatex and pdftex)";
        return 1;
    }
    const std::string fmt_dir = EnsureFormat(format, trees, cache, opt.rebuild_format, opt.verbose, err);
    if (fmt_dir.empty()) return 1;

    // Files the document reads: its aux files first (as pdfTeX looks in
    // its output directory), then its own directory, then the trees.
    Resolver resolver;
    resolver.add_dir(outdir);
    if (texdir != outdir) resolver.add_dir(texdir);
    for (const std::string &d : opt.dirs) resolver.add_dir(d);
    for (const std::string &t : trees) resolver.add_tree(t);

    const std::string path = tex.string();
    // TeX reads the first line's file name itself; quoted, it may hold spaces
    const std::string first_line = path.find(' ') == std::string::npos ? path : "\"" + path + "\"";
    const std::string base = outdir + "/" + tex.stem().string();
    const std::string pdf = base + ".pdf";
    fs::remove(pdf, ec);
    int status = 1;
    for (int pass = 1; pass <= std::max(opt.max_passes, 1); pass++) {
        const std::string before = AuxState(base);
        {
            auto engine = std::make_unique<Engine>();
            engine->opt.format = format;
            engine->opt.format_dirs = {fmt_dir};
            engine->opt.output_dir = outdir;
            engine->opt.first_line = first_line;
            engine->opt.interaction = tc::nonstop_mode;
            engine->resolver = &resolver;
            status = engine->run();
        }
        std::fflush(stdout);
        // an error that still left a PDF (a missing character, say) is
        // reported by the exit status, but the passes go on: otherwise the
        // PDF keeps what a rerun would remove (the extra page LaTeX adds
        // for its last-page hook, wrong references)
        if (status != 0 && !IsFile(pdf)) break;
        const bool changed = AuxState(base) != before;
        const bool asked = TexLogWantsRerun(ReadFile(base + ".log"));
        if (!changed && !asked) break;
        if (pass < opt.max_passes && opt.verbose)
            std::fprintf(stderr, "mep-tex: running again (%s)\n", asked ? "the log asks for it" : "auxiliary files changed");
    }
    if (status == 0 && !IsFile(pdf)) {
        *err = "no PDF was written (the document made no pages?)";
        return 1;
    }
    return status;
}

}  // namespace meptex
