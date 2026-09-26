// Writes the generated halves of Part K's documentation.
//
// A TINY SEPARATE BINARY rather than a mode of `mep`, because `just
// cad-fem-docs` has to run in CI where there is no display and because
// this must not depend on anything that could fail for an unrelated
// reason. It links the method table and nothing else.

#include "cad_fem_methods.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace {

// Replaces the region between two markers, leaving everything else
// alone. The markers are comments in the target's own syntax, so the
// generated block is visible as generated to anyone reading the file and
// a hand edit inside it is obviously going to be lost.
bool ReplaceBetween(const std::string &path, const std::string &begin, const std::string &end,
                    const std::string &body) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const std::size_t from = text.find(begin);
    const std::size_t to = text.find(end);
    if (from == std::string::npos || to == std::string::npos || to < from) {
        std::fprintf(stderr, "%s has no %s ... %s block\n", path.c_str(), begin.c_str(),
                     end.c_str());
        return false;
    }
    text = text.substr(0, from + begin.size()) + "\n\n" + body + "\n" + text.substr(to);
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return false;
    }
    out << text;
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    bool ok = ReplaceBetween(root + "/MEP_AGENT_API.md", "<!-- BEGIN GENERATED cad-fem -->",
                             "<!-- END GENERATED cad-fem -->", cadfem::MarkdownReference());
    ok = ReplaceBetween(root + "/help/cad-fem.org", "# BEGIN GENERATED cad-fem",
                        "# END GENERATED cad-fem", cadfem::OrgReference()) &&
         ok;
    if (!ok) return 1;
    std::printf("wrote the reference for %d methods\n",
                static_cast<int>(cadfem::Methods().size()));
    return 0;
}
