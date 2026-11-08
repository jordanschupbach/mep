// Lookups over the generated Go standard-library table
// (go_lsp_vocab_data.cpp, sorted by package then name).

#include "go_lsp_vocab.h"

#include <algorithm>

extern const GoVocabEntry kGoVocab[];
extern const size_t kGoVocabCount;

namespace {

bool Less(const GoVocabEntry &e, std::string_view pkg, std::string_view name) {
    const int c = std::string_view(e.pkg).compare(pkg);
    return c != 0 ? c < 0 : std::string_view(e.name) < name;
}

const GoVocabEntry *LowerBound(std::string_view pkg, std::string_view name) {
    return std::lower_bound(kGoVocab, kGoVocab + kGoVocabCount, 0,
                            [&](const GoVocabEntry &e, int) { return Less(e, pkg, name); });
}

}  // namespace

const GoVocabEntry *GoVocabFind(std::string_view pkg, std::string_view name) {
    const GoVocabEntry *it = LowerBound(pkg, name);
    if (it != kGoVocab + kGoVocabCount && it->pkg == pkg && it->name == name) return it;
    return nullptr;
}

std::vector<const GoVocabEntry *> GoVocabMembers(std::string_view pkg) {
    std::vector<const GoVocabEntry *> out;
    for (const GoVocabEntry *it = LowerBound(pkg, ""); it != kGoVocab + kGoVocabCount && it->pkg == pkg; ++it) {
        const std::string_view name = it->name;
        if (!name.empty() && name.find('.') == std::string_view::npos) out.push_back(it);
    }
    return out;
}

std::vector<const GoVocabEntry *> GoVocabTypeMembers(std::string_view pkg, std::string_view type) {
    std::vector<const GoVocabEntry *> out;
    std::string prefix(type);
    prefix += '.';
    for (const GoVocabEntry *it = LowerBound(pkg, prefix); it != kGoVocab + kGoVocabCount && it->pkg == pkg; ++it) {
        const std::string_view name = it->name;
        if (name.substr(0, prefix.size()) != prefix) break;
        if (name.find('.', prefix.size()) == std::string_view::npos) out.push_back(it);
    }
    return out;
}

const std::vector<std::string_view> &GoVocabPackages() {
    static const std::vector<std::string_view> packages = [] {
        std::vector<std::string_view> v;
        for (size_t i = 0; i < kGoVocabCount; ++i)
            if (kGoVocab[i].kind == GoVocabKind::Package) v.emplace_back(kGoVocab[i].pkg);
        std::sort(v.begin(), v.end());
        return v;
    }();
    return packages;
}

size_t GoVocabSize() { return kGoVocabCount; }
