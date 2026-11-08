#ifndef MEP_OCTAVE_LSP_VOCAB_H
#define MEP_OCTAVE_LSP_VOCAB_H

// What mep's Octave language server knows about Octave's own functions:
// every builtin (`__builtins__`) and every function file on the default
// path (`__list_functions__`), each with its usage lines ("Y = abs (X)").
// Names and signatures only -- the @deftypefn headers of Octave 11's help
// texts with the markup removed (tools/gen_octave_vocab.py regenerates
// src/octave_lsp_vocab.cpp); no prose.

#include <cstddef>

struct OctaveVocabEntry {
    const char *name;
    const char *usage;  // one usage line per '\n' ("" when the help has none)
    bool builtin;       // compiled into Octave (else a function file it ships)
};

const OctaveVocabEntry *OctaveVocab(size_t *count);
const OctaveVocabEntry *OctaveVocabFind(const char *name, size_t len);

#endif  // MEP_OCTAVE_LSP_VOCAB_H
