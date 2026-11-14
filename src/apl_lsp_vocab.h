#ifndef MEP_APL_LSP_VOCAB_H
#define MEP_APL_LSP_VOCAB_H

// APL's built-in vocabulary for mep-apl-lsp: the primitive glyphs (with
// their monadic and dyadic meanings, the names they are completed from and
// the key that types them), the system names (⎕IO, ⎕NA, ...), the
// )commands and the Dyalog control words. Hand-written from the GNU APL
// 1.9 info manual and Dyalog APL 19/20's Language Reference.

#include <string>
#include <vector>

namespace apl {

/** @brief A primitive glyph: function, operator or syntax. */
struct Glyph {
    const char *glyph;      // UTF-8, one code point
    const char *kind;       // "function", "operator", "syntax", "constant"
    const char *monadic;    // name of the monadic form ("" when there is none)
    const char *dyadic;     // name of the dyadic form ("" when there is none)
    const char *names;      // space-separated completion names: rho reshape shape
    const char *key;        // the backtick key that types it (Dyalog US layout), "" if none
    const char *doc;        // one or two sentences
};

/** @brief A system name (⎕X): function or variable. */
struct SystemName {
    const char *name;     // with ⎕, canonical case: "⎕IO"
    const char *kind;     // "variable", "function", "constant", "namespace"
    const char *dialect;  // "both", "gnu", "dyalog"
    const char *syntax;   // e.g. "R←⎕NA 'decl'"
    const char *doc;
};

struct SystemCommand {
    const char *name;     // ")LOAD"
    const char *dialect;  // "both", "gnu", "dyalog"
    const char *doc;
};

struct ControlWord {
    const char *name;  // ":If"
    const char *doc;
};

const std::vector<Glyph> &Glyphs();
const std::vector<SystemName> &SystemNames();
const std::vector<SystemCommand> &SystemCommands();
const std::vector<ControlWord> &ControlWords();

/** @brief The glyph entry for a UTF-8 glyph, or nullptr. */
const Glyph *FindGlyph(const std::string &glyph);
/** @brief The system name entry (case-insensitive, with or without ⎕), or nullptr. */
const SystemName *FindSystemName(const std::string &name);
const ControlWord *FindControlWord(const std::string &name);  // case-insensitive, with the ':'
const SystemCommand *FindSystemCommand(const std::string &name);  // case-insensitive, with the ')'

/** @brief Markdown documentation of a glyph (hover, completion, --doc). */
std::string GlyphMarkdown(const Glyph &g);
std::string SystemNameMarkdown(const SystemName &s);

/** @brief The glyph a backtick key types (`r -> ⍴), or "" when none. */
std::string GlyphForKey(char key);

}  // namespace apl

#endif  // MEP_APL_LSP_VOCAB_H
