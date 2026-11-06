// mep's own Nix formatter (nix_format.h): read, print, lay out, then prove
// the result means what the input did before handing it back.

#include "nix_format.h"

#include "nix_format_internal.h"

#include <algorithm>

namespace nixfmt {

namespace {

std::string Quote(const std::string &t) {
    std::string o = "\"";
    for (char c : t) {
        if (c == '"' || c == '\\') o += '\\';
        if (c == '\n') {
            o += "\\n";
            continue;
        }
        o += c;
    }
    return o + "\"";
}

void Dump(const detail::Doc &d, int depth, std::string &out) {
    using namespace detail;
    for (const auto &e : d) {
        out.append(static_cast<size_t>(depth) * 2, ' ');
        switch (e.kind) {
        case DocE::Text: {
            static const char *const kAnn[] = {"RegularT", "Comment", "TrailingComment", "Trailing"};
            out += "Text " + std::to_string(e.nest) + " " + std::to_string(e.off) + " " +
                   kAnn[static_cast<int>(e.tann)] + " " + Quote(e.text) + "\n";
            break;
        }
        case DocE::Spacing: {
            static const char *const kSp[] = {"Softbreak", "Break", "Hardspace", "Softspace", "Space", "Hardline", "Emptyline", "Newlines"};
            out += std::string("Spacing ") + kSp[static_cast<int>(e.sp)];
            if (e.sp == Sp::Newlines) out += " " + std::to_string(e.n);
            out += "\n";
            break;
        }
        case DocE::Group: {
            static const char *const kG[] = {"RegularG", "Priority", "Transparent"};
            out += std::string("Group ") + kG[static_cast<int>(e.g)] + "\n";
            Dump(e.kids, depth + 1, out);
            break;
        }
        }
    }
}

std::string DumpDoc(const detail::Doc &d) {
    std::string out;
    Dump(d, 0, out);
    return out;
}

Result Refuse(std::string_view src, std::string why, int line) {
    Result r;
    r.text = std::string(src);
    r.error = std::move(why);
    r.error_line = line;
    return r;
}

}  // namespace

Result Format(std::string_view src, const Options &opts) {
    using namespace detail;
    const std::string input(src);
    ParseResult parsed = ParseFile(input, opts.strict);
    if (!parsed.ok) return Refuse(src, parsed.error, parsed.error_line);
    // gf runs inside the editor: cap what the recursive printer is given.
    constexpr int kMaxTreeDepth = 1000;
    if (MaxDepth(parsed.file) > kMaxTreeDepth) return Refuse(src, "nested too deeply to format", 0);

    // Leading blank lines would only be stripped from the output anyway, but
    // left in the document they can change layout decisions.
    Whole w = parsed.file;
    w.e = MapFirst(w.e, [](AnnHdr &h) {
        size_t k = 0;
        while (k < h.pre.size() && h.pre[k].kind == Trivium::EmptyLine) k++;
        h.pre.erase(h.pre.begin(), h.pre.begin() + static_cast<std::ptrdiff_t>(k));
    });
    const Doc doc = Fixup(PrettyFile(w));
    if (opts.dump_ir) {
        Result r;
        r.ok = true;
        r.text = DumpDoc(doc);
        return r;
    }
    const int width = std::max(1, opts.width);
    const int indent = std::max(0, opts.indent);
    std::string out = StripSpace(LayoutGreedy(width, indent, doc)) + "\n";
    if (!opts.check) {
        Result r;
        r.ok = true;
        r.text = std::move(out);
        return r;
    }

    ParseResult again = ParseFile(out, opts.strict);
    if (!again.ok) return Refuse(src, "formatter bug: the result does not parse (" + again.error + ")", 0);
    if (!Equal(parsed.file, again.file)) return Refuse(src, "formatter bug: the result parses differently", 0);
    std::vector<std::string> before, after;
    CommentWords(parsed.file, before);
    CommentWords(again.file, after);
    std::sort(before.begin(), before.end());
    std::sort(after.begin(), after.end());
    if (before != after) return Refuse(src, "formatter bug: a comment would change", 0);

    Result r;
    r.ok = true;
    r.text = std::move(out);
    return r;
}

}  // namespace nixfmt
