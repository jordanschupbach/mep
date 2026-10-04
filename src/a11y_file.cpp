#include "a11y_file.h"

#include "a11y_html.h"
#include "a11y_pdf.h"
#include "mepml_a11y.h"
#include "mepml_convert.h"
#include "pdf_document.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

namespace a11y {

namespace {

bool ReadBytes(const std::string &path, std::string *out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool ReadLines(const std::string &path, std::vector<std::string> *lines) {
    std::ifstream in(path);
    if (!in) return false;
    lines->clear();
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines->push_back(l);
    }
    return true;
}

}  // namespace

bool FromPdfBytes(const unsigned char *data, size_t len, Document *out, std::string *error) {
    pdfdoc::PdfDocument document;
    document.Load(data, len);
    if (document.PageCount() == 0) {
        if (error) *error = "not a readable PDF";
        return false;
    }
    *out = FromPdf(data, len, document);
    return true;
}

bool FromFile(const std::string &path, Document *out, std::string *error) {
    const mepml::Format format = mepml::FormatFromPath(path);
    auto fail = [&](const std::string &why) {
        if (error) *error = why;
        return false;
    };
    if (format == mepml::Format::Pdf) {
        std::string bytes;
        if (!ReadBytes(path, &bytes)) return fail("cannot read " + path);
        return FromPdfBytes(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(), out, error);
    }
    if (format == mepml::Format::Html) {
        std::string html;
        if (!ReadBytes(path, &html)) return fail("cannot read " + path);
        *out = FromHtml(html);
        return true;
    }
    std::vector<std::string> lines;
    const mepml::ReadFileFn read = [](const std::string &p, std::vector<std::string> *l) { return ReadLines(p, l); };
    std::error_code ec;
    const std::string abs = std::filesystem::absolute(path, ec).string();
    if (format == mepml::Format::Mepml) {
        if (!ReadLines(path, &lines)) return fail("cannot read " + path);
        *out = mepml::Accessibility(mepml::ParseWithImports(abs, lines, read));
        return true;
    }
    if (!mepml::CanImport(format)) return fail("no accessibility reader for " + path);
    // (An import writes a document's pictures beside its result: a
    // directory of our own, removed again.)
    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path(ec) / ("mep-a11y-" + std::to_string(std::hash<std::string>{}(abs)));
    std::filesystem::create_directories(tmp, ec);
    std::string text, err;
    const bool ok = mepml::ImportFile(path, (tmp / "import.mepml").string(), &text, &err);
    std::filesystem::remove_all(tmp, ec);
    if (!ok) return fail(err.empty() ? "cannot read " + path : err);
    std::istringstream ss(text);
    std::string l;
    while (std::getline(ss, l)) lines.push_back(l);
    *out = mepml::Accessibility(mepml::Parse(lines));
    out->format = mepml::FormatExtension(format);
    // (Their pictures are gone with the directory: a node's line is the
    // imported text's, which nobody has.)
    return true;
}

}  // namespace a11y
