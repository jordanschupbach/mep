// mep-mepml: mepml's converter as a command-line tool.
//
//   mep-mepml convert IN OUT
//
// Formats come from the file extensions. From .mepml to anything mep can
// export (html md org rtf docx odt tex pdf txt); from anything it can
// import (html md org rtf docx odt txt) to .mepml; and between any two of
// those by way of mepml. PDF is the LaTeX export compiled by tectonic,
// which must be on PATH.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "mepml_convert.h"

namespace {

bool ReadLines(const std::string &path, std::vector<std::string> *lines) {
    std::ifstream f(path);
    if (!f) return false;
    std::string l;
    while (std::getline(f, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines->push_back(l);
    }
    return true;
}

std::vector<std::string> SplitText(const std::string &s) {
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string l;
    while (std::getline(ss, l)) out.push_back(l);
    return out;
}

std::string ShellQuote(const std::string &s) {
    std::string o = "'";
    for (char c : s) o += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return o + "'";
}

int Usage() {
    std::fprintf(stderr,
                 "usage: mep-mepml convert [--beamer] IN OUT\n"
                 "  export from .mepml to: html md org rtf docx odt tex pdf txt pptx odp\n"
                 "  (with //? Type: presentation, html/tex/pdf are a slideshow and a Beamer deck;\n"
                 "  --beamer makes tex/pdf the Beamer deck of the \\slide blocks whatever the Type)\n"
                 "  import to .mepml from: html md org rtf docx odt txt\n");
    return 2;
}

}  // namespace

int main(int argc, char **argv) {
    const bool beamer = argc == 5 && std::string(argv[2]) == "--beamer";
    if (argc != 4 + (beamer ? 1 : 0) || std::string(argv[1]) != "convert") return Usage();
    const std::string in = argv[argc - 2], out = argv[argc - 1];
    const mepml::Format fin = mepml::FormatFromPath(in), fout = mepml::FormatFromPath(out);
    std::error_code ec;
    const std::string in_abs = std::filesystem::absolute(in, ec).string();
    const std::string base_dir = std::filesystem::path(in_abs).parent_path().string();

    // Everything goes through a mepml document.
    std::vector<std::string> lines;
    std::string err;
    if (fin == mepml::Format::Mepml) {
        if (!ReadLines(in, &lines)) {
            std::fprintf(stderr, "mep-mepml: cannot read %s\n", in.c_str());
            return 1;
        }
    } else if (mepml::CanImport(fin)) {
        std::string text;
        const std::string target = fout == mepml::Format::Mepml ? out : (std::filesystem::path(out).replace_extension(".mepml").string());
        if (!mepml::ImportFile(in, target, &text, &err)) {
            std::fprintf(stderr, "mep-mepml: %s\n", err.c_str());
            return 1;
        }
        if (fout == mepml::Format::Mepml) {
            std::ofstream o(out);
            o << text;
            return o ? 0 : 1;
        }
        lines = SplitText(text);
    } else {
        std::fprintf(stderr, "mep-mepml: cannot read %s files\n", in.c_str());
        return 2;
    }
    // Commands, \when and \raw are expanded for this export: its tags say
    // what it is (html, pdf, beamer, docx ...).
    const std::vector<std::string> tags = mepml::ExportTags(fout, mepml::Parse(lines), beamer);
    const mepml::Document doc = mepml::ParseForExport(
        in_abs, lines, [](const std::string &p, std::vector<std::string> *l) { return ReadLines(p, l); }, tags);
    for (const mepml::Diagnostic &d : doc.diagnostics)
        if (d.message.rfind("expanding commands: ", 0) == 0) std::fprintf(stderr, "mep-mepml: %s\n", d.message.c_str());
    std::string latex;
    if (fout == mepml::Format::Pdf || fout == mepml::Format::Latex) {
        latex = beamer ? mepml::ToBeamer(doc, base_dir, &err) : mepml::ToLatex(doc, base_dir);
        if (latex.empty()) {
            std::fprintf(stderr, "mep-mepml: %s\n", err.c_str());
            return 1;
        }
    } else if (beamer) {
        std::fprintf(stderr, "mep-mepml: --beamer writes .tex or .pdf, not %s\n", out.c_str());
        return 2;
    }
    if (fout == mepml::Format::Latex) {
        std::ofstream o(out);
        o << latex;
        return o ? 0 : 1;
    }
    if (fout == mepml::Format::Pdf) {
        const std::filesystem::path pdf = std::filesystem::absolute(out, ec);
        const std::filesystem::path tex = std::filesystem::path(pdf).replace_extension(".tex");
        {
            std::ofstream o(tex);
            o << latex;
        }
        const std::string cmd = "tectonic -X compile " + ShellQuote(tex.string()) + " --outdir " +
                                ShellQuote(pdf.parent_path().string()) + " >/dev/null 2>&1";
        const int rc = std::system(cmd.c_str());
        std::filesystem::remove(tex, ec);
        if (rc != 0) {
            std::fprintf(stderr, "mep-mepml: tectonic failed (is it on PATH?)\n");
            return 1;
        }
        return 0;
    }
    if (!mepml::CanExport(fout)) return Usage();
    if (!mepml::ExportFile(doc, out, base_dir, &err)) {
        std::fprintf(stderr, "mep-mepml: %s\n", err.c_str());
        return 1;
    }
    return 0;
}
