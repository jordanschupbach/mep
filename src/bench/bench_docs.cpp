// mep-bench: document formats parsed on open (and, for Org, on many
// frames): the Org outline, the HTML engine (help pages, mail, the
// rendered-Markdown view), SVG rasterization (inline images).

#include "bench_util.h"
#include "bench/fixtures.h"

#include "html_doc.h"
#include "org_doc.h"
#include "svg_raster.h"

#include <string>
#include <vector>

using mep::bench::DoNotOptimize;
using mep::bench::Measure;

MEP_BENCH("docs/org") {
    const auto src = mep::bench::SplitLines(mep::bench::ReadFileOrEmpty(mep::bench::RepoPath("README.org")));
    for (size_t lines : {2000UL, 20000UL}) {
        const auto doc = mep::bench::GrowLines(src, lines);
        Measure("outline_" + std::to_string(lines), static_cast<long long>(lines),
                [&] { DoNotOptimize(ParseOrgOutline(doc).headlines.size()); });
    }
}

MEP_BENCH("docs/html") {
    const std::string html = mep::bench::ReadFileOrEmpty(mep::bench::RepoPath("README.html"));
    if (html.empty()) return;
    const long long kb = static_cast<long long>(html.size() / 1024);
    Measure("parse_and_style_readme", kb, [&] {
        HtmlDoc doc;
        ParseHtml(html, doc, true, true);
        DoNotOptimize(&doc);
    });
    Measure("parse_only_readme", kb, [&] {
        HtmlDoc doc;
        ParseHtml(html, doc, true, false);
        DoNotOptimize(&doc);
    });
}

MEP_BENCH("docs/svg") {
    // A dense generated drawing: 2000 paths and circles.
    std::string svg = "<svg xmlns='http://www.w3.org/2000/svg' width='1600' height='1000' viewBox='0 0 1600 1000'>";
    mep::bench::Rng rng;
    for (int i = 0; i < 2000; i++) {
        const size_t x = rng.Below(1600);
        const size_t y = rng.Below(1000);
        if (i % 2) {
            svg += "<circle cx='" + std::to_string(x) + "' cy='" + std::to_string(y) + "' r='" +
                   std::to_string(5 + rng.Below(40)) + "' fill='#3a7' fill-opacity='0.5' stroke='#123'/>";
        } else {
            svg += "<path d='M" + std::to_string(x) + " " + std::to_string(y) + " C " + std::to_string(rng.Below(1600)) +
                   " " + std::to_string(rng.Below(1000)) + " " + std::to_string(rng.Below(1600)) + " " +
                   std::to_string(rng.Below(1000)) + " " + std::to_string(rng.Below(1600)) + " " +
                   std::to_string(rng.Below(1000)) + "' stroke='#a33' stroke-width='3' fill='none'/>";
        }
    }
    svg += "</svg>";
    Measure("raster_2000_shapes", 2000, [&] {
        std::vector<unsigned char> rgba;
        int w = 0;
        int h = 0;
        std::string err;
        svg_raster::Rasterize(reinterpret_cast<const unsigned char *>(svg.data()), svg.size(), &rgba, &w, &h, &err);
        DoNotOptimize(rgba.size());
    });
}
