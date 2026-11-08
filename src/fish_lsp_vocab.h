#ifndef MEP_FISH_LSP_VOCAB_H
#define MEP_FISH_LSP_VOCAB_H

// What mep's Fish language server knows about fish's own commands: the
// builtins (`builtin -n`) and the functions fish ships (`functions -a`),
// each with its synopsis lines and its options. Names, synopses and option
// spellings only -- they come from fish 4.x's man pages and completions
// (tools/fish_vocab.py regenerates src/fish_lsp_vocab.cpp); no prose.

#include <cstddef>

struct FishVocabEntry {
    const char *name;
    const char *synopsis;  // one usage line per '\n'
    const char *options;   // space-separated: -s --long ...
    bool builtin;          // a builtin (else a function fish ships)
};

const FishVocabEntry *FishVocab(size_t *count);
const FishVocabEntry *FishVocabFind(const char *name, size_t len);

#endif  // MEP_FISH_LSP_VOCAB_H
