// The grammar half of mep's own C++ front end: a recursive-descent
// parser over the token stream cpp_ast.cpp's preprocessor produces.
// cpp_ast.h documents the tree; this file documents how it is built.
//
// Three things a C++ parser has to decide that a Python one never does,
// and the rule this file uses for each:
//
//   1. **Is this statement a declaration or an expression?**
//      `Foo bar(x);` is a declaration, `foo(x);` a call, and telling them
//      apart needs to know whether `Foo` names a type. IsDeclarationAhead
//      answers it syntactically: a qualified-id followed by another
//      identifier (after any `*`, `&` and cv-qualifiers) is a
//      declaration, anything else is an expression. That is the
//      standard's own preference -- "if it can be a declaration, it is"
//      -- and it is wrong only where a human reader is also unsure.
//
//   2. **Is this `<` a template argument list or a comparison?**
//      TemplateArgumentsAhead scans for the matching `>` and refuses if a
//      `;` or an unbalanced bracket turns up first. `>>` is split into
//      two `>` in place when a template argument list needs to close,
//      which is why this parser owns a mutable copy of its tokens.
//
//   3. **What does an unknown identifier in declaration position mean?**
//      It is a macro from a header nobody read (`MEP_API void f();`,
//      `Q_OBJECT`). The declaration-specifier loop treats a leading
//      identifier as the type, and replaces it if a real type specifier
//      turns up afterwards -- so the macro is absorbed rather than
//      reported.
//
// Error recovery is per declaration and per statement: an unparsable one
// is reported once (at most one diagnostic per line, the way CPython
// reports one syntax error and stops -- see the same note in
// python_ast.cpp) and skipped to the next `;` or balanced `}`, so a file
// in the middle of being typed still yields symbols for the parts that
// are fine.

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "cpp_ast.h"
#include "cpp_ast_internal.h"

namespace {

constexpr int kMaxParseDepth = 150;
constexpr int kMaxErrors = 200;

CppNodePtr MakeNode(CppNodeKind kind) {
    CppNodePtr node = std::make_unique<CppNode>();
    node->kind = kind;
    return node;
}

// Keywords that can only begin a declaration. An identifier is never in
// here: that is what makes IsDeclarationAhead a scan rather than a lookup.
const std::unordered_set<std::string> &DeclStartKeywords() {
    static const std::unordered_set<std::string> kSet = {
        "class",    "struct",   "union",     "enum",      "typedef",   "static",   "extern",
        "static_assert",
        "inline",   "virtual",  "explicit",  "constexpr", "consteval", "constinit", "mutable",
        "register", "thread_local", "friend", "template", "typename",  "using",    "namespace",
        "const",    "volatile", "signed",    "unsigned",  "void",      "bool",     "char",
        "char8_t",  "char16_t", "char32_t",  "wchar_t",   "short",     "int",      "long",
        "float",    "double",   "auto",      "__extension__", "__attribute__", "__restrict",
        "__restrict__", "__inline", "__inline__", "__int64", "__declspec", "__signed__",
    };
    return kSet;
}

// --- Template names ---------------------------------------------------

// The template names this file itself makes visible, used to settle the
// `<` ambiguity where nothing local can (see TemplateArgumentsAhead).
// Three sources, cheapest first:
//   1. Every `template<...>` declaration in the file, whose name is the
//      last identifier before the declaration's `(`, `{`, `:`, `;` or `=`.
//   2. Every name used as `Name<...>` somewhere the strict rules already
//      accept -- `vector<int>` on one line makes `vector` a template on
//      every other line, including the one with a `&&` in its arguments.
//   3. A small list of standard templates, for the first file that uses
//      one before declaring anything.
// Being wrong here costs a misparse of one expression, never a report:
// an unknown name simply keeps the strict rules.
const std::unordered_set<std::string> &StandardTemplateNames() {
    static const std::unordered_set<std::string> kSet = {
        "vector", "map", "set", "unordered_map", "unordered_set", "multimap", "multiset", "deque",
        "list", "forward_list", "array", "pair", "tuple", "optional", "variant", "span", "queue",
        "stack", "priority_queue", "basic_string", "basic_string_view", "unique_ptr", "shared_ptr",
        "weak_ptr", "function", "reference_wrapper", "initializer_list", "allocator", "atomic",
        "integral_constant", "bool_constant", "__bool_constant", "enable_if", "enable_if_t",
        "conditional", "conditional_t", "is_same", "is_same_v", "is_base_of", "is_convertible",
        "remove_reference", "remove_reference_t", "remove_cv", "decay", "decay_t", "common_type",
        "numeric_limits", "char_traits", "iterator_traits", "allocator_traits", "hash", "less",
        "greater", "equal_to", "plus", "minus", "tuple_element", "tuple_size", "make_index_sequence",
        "index_sequence", "integer_sequence", "type_identity", "void_t", "invoke_result",
        "invoke_result_t", "is_invocable", "is_invocable_v", "result_of", "add_pointer",
        "underlying_type", "underlying_type_t", "make_signed", "make_unsigned", "aligned_storage",
        "complex", "valarray", "bitset", "chrono", "duration", "time_point", "ratio", "unique_lock",
        "lock_guard", "scoped_lock", "future", "promise", "packaged_task", "expected", "monostate",
    };
    return kSet;
}

/** @brief Scans a `<...>` under the strict rules, for the template-name prepass. */
bool StrictTemplateArgumentsAt(const std::vector<CppToken> &t, size_t start) {
    if (t[start].text != "<") return false;
    size_t k = start + 1;
    int angle = 1;
    int nest = 0;
    int budget = 2000;
    size_t close = 0;
    while (k < t.size() && t[k].kind != CppTokKind::End && budget-- > 0) {
        const CppToken &tok = t[k];
        if (tok.kind == CppTokKind::Op) {
            if (tok.text == "(" || tok.text == "[" || tok.text == "{") {
                nest++;
            } else if (tok.text == ")" || tok.text == "]" || tok.text == "}") {
                if (nest == 0) return false;
                nest--;
            } else if (nest == 0) {
                if (tok.text == "||" || tok.text == "&&" || tok.text == "=" || tok.text == "?" ||
                    tok.text == ":" || tok.text == "==" || tok.text == "!=" || tok.text == "<=") {
                    return false;
                }
                if (tok.text == "<") {
                    angle++;
                } else if (tok.text == ">" || tok.text == ">>" || tok.text == ">=" || tok.text == ">>=") {
                    angle -= tok.text == ">>" ? 2 : 1;
                    if (angle <= 0) {
                        close = k + 1;
                        break;
                    }
                } else if (tok.text == ";" || tok.text == "{") {
                    return false;
                }
            }
        }
        k++;
    }
    if (close == 0) return false;
    const CppToken &after = t[std::min(close, t.size() - 1)];
    if (after.kind == CppTokKind::Ident || after.kind == CppTokKind::Keyword) return true;
    if (after.kind != CppTokKind::Op) return after.kind == CppTokKind::End;
    static const char *const kAllowed[] = {"(", "{", "::", "*", "&", "&&", ",", ";", ")", "]", ">", ">>"};
    for (const char *allowed : kAllowed) {
        if (after.text == allowed) return true;
    }
    return false;
}

std::unordered_set<std::string> CollectTemplateNames(const std::vector<CppToken> &t) {
    std::unordered_set<std::string> names = StandardTemplateNames();
    for (size_t k = 0; k + 1 < t.size(); k++) {
        if (t[k].kind == CppTokKind::Keyword && t[k].text == "template" && t[k + 1].text == "<") {
            // Step over the parameter list, then take the last name
            // before whatever ends the declaration's introducer.
            size_t j = k + 2;
            int angle = 1;
            while (j < t.size() && t[j].kind != CppTokKind::End && angle > 0) {
                if (t[j].text == "<") angle++;
                if (t[j].text == ">") angle--;
                if (t[j].text == ">>") angle -= 2;
                j++;
            }
            std::string last;
            int budget = 80;
            while (j < t.size() && t[j].kind != CppTokKind::End && budget-- > 0) {
                const std::string &text = t[j].text;
                if (text == "(" || text == "{" || text == ";" || text == ":" || text == "=") break;
                if (t[j].kind == CppTokKind::Ident) last = text;
                j++;
            }
            if (!last.empty()) names.insert(last);
            continue;
        }
        if (t[k].kind == CppTokKind::Ident && t[k + 1].text == "<" && names.count(t[k].text) == 0) {
            if (StrictTemplateArgumentsAt(t, k + 1)) names.insert(t[k].text);
        }
    }
    return names;
}

// --- The parser -------------------------------------------------------

class Parser {
public:
    Parser(std::vector<CppToken> tokens, std::vector<CppSyntaxError> *errors)
        : t_(std::move(tokens)), errors_(errors), template_names_(CollectTemplateNames(t_)) {
        if (t_.empty() || t_.back().kind != CppTokKind::End) {
            CppToken end_tok;
            end_tok.kind = CppTokKind::End;
            t_.push_back(end_tok);
        }
    }

    /** @brief Parses the whole token stream into a TranslationUnit node. */
    CppNodePtr ParseUnit() {
        CppNodePtr unit = MakeNode(CppNodeKind::TranslationUnit);
        unit->start = t_.front().start;
        ParseDeclarationSeq(unit->body, false, "");
        if (!AtEnd()) {
            // A `}` with nothing open: report it once and keep going, so
            // the rest of the file still produces symbols.
            while (!AtEnd()) {
                ErrorAt(Cur(), "unbalanced-brace", "Unmatched '" + Cur().text + "'");
                Next();
                ParseDeclarationSeq(unit->body, false, "");
            }
        }
        unit->end = t_.back().end;
        return unit;
    }

    bool truncated() const { return truncated_; }

private:
    std::vector<CppToken> t_;
    size_t i_ = 0;
    std::vector<CppSyntaxError> *errors_;
    std::unordered_set<std::string> template_names_;
    int depth_ = 0;
    int last_error_line_ = -1;
    int error_count_ = 0;
    bool truncated_ = false;
    // Inside a template argument list `>` closes the list instead of
    // comparing, and `>>` closes two of them.
    int no_greater_ = 0;
    // The token an expression statement began at. A `{` right after a
    // name there opens a block (`__try {`), not a braced functional cast
    // -- and swallowing the block would cost the rest of the function.
    size_t statement_start_ = static_cast<size_t>(-1);

    // --- Token access -------------------------------------------------

    const CppToken &Cur() const { return t_[i_]; }
    const CppToken &Peek(size_t n = 1) const { return t_[std::min(i_ + n, t_.size() - 1)]; }
    const CppToken &At(size_t index) const { return t_[std::min(index, t_.size() - 1)]; }
    bool AtEnd() const { return t_[i_].kind == CppTokKind::End; }
    bool AtOp(const char *text) const { return Cur().kind == CppTokKind::Op && Cur().text == text; }
    bool AtKw(const char *text) const { return Cur().kind == CppTokKind::Keyword && Cur().text == text; }
    bool AtIdent(const char *text) const { return Cur().kind == CppTokKind::Ident && Cur().text == text; }
    bool AtName() const { return Cur().kind == CppTokKind::Ident; }
    void Next() {
        if (!AtEnd()) i_++;
    }
    bool EatOp(const char *text) {
        if (!AtOp(text)) return false;
        Next();
        return true;
    }
    bool EatKw(const char *text) {
        if (!AtKw(text)) return false;
        Next();
        return true;
    }
    bool EatIdent(const char *text) {
        if (!AtIdent(text)) return false;
        Next();
        return true;
    }

    /** @brief Renders the source spelling of a token range, for a type's or signature's detail text. */
    std::string Spell(size_t from, size_t to) const {
        std::string out;
        for (size_t k = from; k < to && k < t_.size(); k++) {
            if (t_[k].kind == CppTokKind::End) break;
            if (!out.empty() && t_[k].space_before) out += ' ';
            out += t_[k].text;
        }
        return out;
    }

    // --- Errors and recovery ------------------------------------------

    void ErrorAt(const CppToken &tok, const char *code, const std::string &message) {
        if (errors_ == nullptr || error_count_ >= kMaxErrors) return;
        // One syntax error per line: a single missing `;` otherwise
        // cascades into a small pile of them, none of which is the one
        // worth reading.
        if (tok.start.line == last_error_line_) return;
        last_error_line_ = tok.start.line;
        error_count_++;
        CppSyntaxError e;
        e.start = tok.start;
        e.end = tok.end;
        if (e.end.line != e.start.line) e.end = e.start;
        e.code = code;
        e.message = message;
        errors_->push_back(e);
    }

    void ErrorHere(const char *code, const std::string &message) { ErrorAt(Cur(), code, message); }

    // Everything a speculative parse has to put back: the cursor, and the
    // errors that parse reported on its way to failing.
    struct Snapshot {
        size_t index = 0;
        size_t error_size = 0;
        int last_error_line = -1;
        int error_count = 0;
    };

    Snapshot Save() const {
        Snapshot s;
        s.index = i_;
        s.error_size = errors_ == nullptr ? 0 : errors_->size();
        s.last_error_line = last_error_line_;
        s.error_count = error_count_;
        return s;
    }

    bool ErrorsSince(const Snapshot &s) const {
        return errors_ != nullptr && errors_->size() > s.error_size;
    }

    void Restore(const Snapshot &s) {
        i_ = s.index;
        if (errors_ != nullptr && errors_->size() > s.error_size) errors_->resize(s.error_size);
        last_error_line_ = s.last_error_line;
        error_count_ = s.error_count;
    }

    /** @brief At an opening bracket, skips past its matching closer. */
    void SkipBalanced() {
        const std::string opener = Cur().text;
        const char *closer = opener == "(" ? ")" : (opener == "[" ? "]" : "}");
        int depth = 0;
        while (!AtEnd()) {
            if (Cur().kind == CppTokKind::Op) {
                if (Cur().text == "(" || Cur().text == "[" || Cur().text == "{") depth++;
                if (Cur().text == ")" || Cur().text == "]" || Cur().text == "}") {
                    depth--;
                    if (depth == 0) {
                        const bool matched = Cur().text == closer;
                        Next();
                        if (!matched) return;
                        return;
                    }
                }
            }
            Next();
        }
    }

    /** @brief Skips to just past the next `;`, or to the `}` that closes the enclosing block. */
    void SkipToDeclarationEnd() {
        int depth = 0;
        while (!AtEnd()) {
            if (Cur().kind == CppTokKind::Op) {
                if (Cur().text == "{" || Cur().text == "(" || Cur().text == "[") {
                    depth++;
                } else if (Cur().text == ")" || Cur().text == "]") {
                    if (depth > 0) depth--;
                } else if (Cur().text == "}") {
                    if (depth == 0) return;  // left for the enclosing body's loop
                    depth--;
                    Next();
                    if (depth == 0) return;
                    continue;
                } else if (Cur().text == ";" && depth == 0) {
                    Next();
                    return;
                }
            }
            Next();
        }
    }

    // A recursion guard: C++ nests, `((((...` nests without bound, and an
    // editor's helper process may not answer a pathological file with a
    // stack overflow.
    struct DepthGuard {
        Parser *parser;
        bool overflow;
        explicit DepthGuard(Parser *p) : parser(p), overflow(false) {
            parser->depth_++;
            if (parser->depth_ > kMaxParseDepth) {
                overflow = true;
                parser->truncated_ = true;
            }
        }
        DepthGuard(const DepthGuard &) = delete;
        DepthGuard &operator=(const DepthGuard &) = delete;
        ~DepthGuard() { parser->depth_--; }
    };

    // --- Attributes ---------------------------------------------------

    /** @brief Consumes `[[...]]`, `__attribute__((...))`, `alignas(...)` and `__declspec(...)`. */
    bool SkipAttributes(std::vector<CppNodePtr> *out) {
        bool any = false;
        for (;;) {
            if (AtOp("[") && Peek().kind == CppTokKind::Op && Peek().text == "[") {
                const size_t start = i_;
                Next();
                SkipBalanced();
                if (AtOp("]")) Next();
                if (out != nullptr) {
                    CppNodePtr attr = MakeNode(CppNodeKind::Attribute);
                    attr->start = t_[start].start;
                    attr->end = t_[i_ > 0 ? i_ - 1 : 0].end;
                    attr->name = Spell(start, i_);
                    out->push_back(std::move(attr));
                }
                any = true;
                continue;
            }
            if (AtKw("__attribute__") || AtKw("__attribute") || AtKw("__declspec") || AtKw("alignas")) {
                Next();
                if (AtOp("(")) SkipBalanced();
                any = true;
                continue;
            }
            if (AtKw("__extension__")) {
                Next();
                any = true;
                continue;
            }
            return any;
        }
    }

    // --- Names --------------------------------------------------------

    /**
     * @brief Scans a `<...>` from `start` to decide whether it is a template argument list.
     * @return the index just past the matching `>`, or 0 when this `<` is a comparison
     */
    size_t TemplateArgumentsAhead(size_t start, bool *leftover = nullptr) const {
        if (leftover != nullptr) *leftover = false;
        if (At(start).text != "<") return 0;
        // A name this file uses as a template elsewhere is a template
        // here too, and its arguments may be any constant expression:
        // `__bool_constant<sizeof(T) == 1 && is_integral<T>::value>`.
        // For every other name the strict rules below apply, because
        // `a < b && c > d` has to stay a pair of comparisons.
        const bool known_template = start > 0 && At(start - 1).kind == CppTokKind::Ident &&
                                    template_names_.count(At(start - 1).text) != 0;
        size_t k = start + 1;
        int angle = 1;
        int nest = 0;
        int budget = 4000;
        size_t close = 0;
        while (k < t_.size() && t_[k].kind != CppTokKind::End && budget-- > 0) {
            const CppToken &tok = t_[k];
            if (tok.kind == CppTokKind::Op) {
                if (tok.text == "(" || tok.text == "[" || tok.text == "{") {
                    nest++;
                } else if (tok.text == ")" || tok.text == "]" || tok.text == "}") {
                    if (nest == 0) return 0;  // the bracket closed outside us: not a template
                    nest--;
                } else if (nest == 0) {
                    // Operators that a template argument list cannot
                    // contain at its top level, but a chain of
                    // comparisons can: `a < b || c > d` is two
                    // comparisons, and this is what says so.
                    if (!known_template &&
                        (tok.text == "||" || tok.text == "?" || tok.text == "++" || tok.text == "--" ||
                         tok.text == ":")) {
                        return 0;
                    }
                    if (tok.text == "=" || tok.text == "+=" || tok.text == "-=" || tok.text == "*=" ||
                        tok.text == "/=" || tok.text == "%=" || tok.text == "&=" || tok.text == "|=" ||
                        tok.text == "^=" || tok.text == "<<=") {
                        return 0;
                    }
                    // `&&` is the hard one: `a < b && c > d` is two
                    // comparisons, `tuple<_Args&&...>` is a template
                    // argument. What follows it decides -- an rvalue
                    // reference is always at the end of its argument.
                    if (tok.text == "&&" && !known_template) {
                        const std::string &next = At(k + 1).text;
                        if (next != ">" && next != ">>" && next != "," && next != "...") return 0;
                    }
                    if (tok.text == "<") {
                        angle++;
                    } else if (tok.text == ">") {
                        angle--;
                        if (angle == 0) {
                            close = k + 1;
                            break;
                        }
                    } else if (tok.text == ">>") {
                        // One `>>` can close two lists at once, or one
                        // and leave a `>` for the list outside this one
                        // (`template<class T, class = A<T>>`). Which it
                        // is, is what `leftover` tells the caller.
                        angle -= 2;
                        if (angle <= 0) {
                            if (leftover != nullptr) *leftover = angle < 0;
                            close = k + 1;
                            break;
                        }
                    } else if (tok.text == ">>=" || tok.text == ">=") {
                        angle--;
                        if (angle <= 0) {
                            if (leftover != nullptr) *leftover = true;
                            close = k + 1;
                            break;
                        }
                    } else if (tok.text == ";" || tok.text == "{") {
                        return 0;  // a statement ended inside: this was a comparison
                    }
                }
            }
            k++;
        }
        if (close == 0) return 0;
        // What follows a template argument list is narrowly constrained:
        // a name, a call, a qualification, a declarator, or the end of
        // whatever it sits in. `month > 12)` fails here, which is the
        // second half of telling a comparison chain from a template.
        const CppToken &after = At(close);
        if (after.kind == CppTokKind::Ident || after.kind == CppTokKind::Keyword ||
            after.kind == CppTokKind::End) {
            return close;
        }
        if (after.kind == CppTokKind::Op) {
            static const char *const kAllowed[] = {"(", "{", "::", "*", "&", "&&", ",", ";",
                                                   ")", "]", ">", ">>", "...", "->", "=", ":"};
            for (const char *allowed : kAllowed) {
                if (after.text == allowed) return close;
            }
        }
        return 0;
    }

    /** @brief Consumes one `>` where a template argument list must close, splitting `>>` in place. */
    bool EatTemplateClose() {
        if (AtOp(">")) {
            Next();
            return true;
        }
        if (Cur().kind == CppTokKind::Op && (Cur().text == ">>" || Cur().text == ">=" || Cur().text == ">>=")) {
            // Split the token: the first `>` closes this list, the rest
            // stays for the enclosing one. Positions shift by a byte, so
            // the remainder still points at real source.
            CppToken &tok = t_[i_];
            tok.text = tok.text.substr(1);
            tok.start.col += 1;
            tok.space_before = false;
            return true;
        }
        return false;
    }

    /**
     * @brief Scans a (possibly qualified, possibly templated) name from `start` without consuming it.
     * @return the index just past the name, or `start` when there is no name there
     */
    size_t ScanQualifiedName(size_t start) const {
        size_t k = start;
        if (At(k).text == "::") k++;
        if (At(k).kind == CppTokKind::Keyword && (At(k).text == "typename" || At(k).text == "template")) k++;
        if (At(k).kind != CppTokKind::Ident && At(k).text != "~") return start;
        if (At(k).text == "~") k++;
        if (At(k).kind != CppTokKind::Ident) return start;
        k++;
        for (;;) {
            if (At(k).text == "<") {
                const size_t after = TemplateArgumentsAhead(k);
                if (after == 0) break;
                k = after;
                continue;
            }
            if (At(k).text == "::" && (At(k + 1).kind == CppTokKind::Ident || At(k + 1).text == "~" ||
                                       At(k + 1).text == "template" || At(k + 1).text == "operator")) {
                k += 2;
                if (At(k - 1).text == "~" || At(k - 1).text == "template") {
                    if (At(k).kind != CppTokKind::Ident) return k;
                    k++;
                }
                continue;
            }
            break;
        }
        return k;
    }

    // A parsed name: what it is called, where its last component sits, and
    // the template arguments it was written with.
    struct ParsedName {
        std::string spelled;   // `std::vector<int>`
        std::string last;      // `vector`
        std::string qualifier; // `std::`
        std::vector<std::string> args;
        CppPos start;
        CppPos name_pos;
        CppPos name_end;
        bool valid = false;
    };

    /** @brief Consumes a qualified name, recording its spelling, its last component and its template arguments. */
    ParsedName ParseQualifiedName() {
        ParsedName name;
        const size_t start = i_;
        name.start = Cur().start;
        const size_t after = ScanQualifiedName(i_);
        if (after == i_) return name;
        // Walk the name rather than jumping to `after`, so the last
        // component's own span (what hover and rename need) is recorded.
        if (AtOp("::")) Next();
        for (;;) {
            if (AtKw("typename") || AtKw("template")) {
                Next();
                continue;
            }
            bool destructor = false;
            if (AtOp("~")) {
                destructor = true;
                Next();
                if (!AtName()) break;
            }
            if (!AtName()) break;
            // The `~` is part of the name: a destructor whose name is
            // spelled like its class is a constructor to everything
            // downstream, which is exactly the wrong answer.
            name.last = (destructor ? "~" : "") + Cur().text;
            name.name_pos = Cur().start;
            name.name_end = Cur().end;
            Next();
            if (AtOp("<")) {
                bool leftover = false;
                const size_t close = TemplateArgumentsAhead(i_, &leftover);
                if (close != 0) {
                    const size_t args_start = i_ + 1;
                    // Split the arguments on top-level commas for the
                    // detail text completion and hover show.
                    size_t arg_start = args_start;
                    int nest = 0;
                    int angle = 1;
                    size_t k = args_start;
                    while (k + 1 < close) {
                        const CppToken &tok = t_[k];
                        if (tok.kind == CppTokKind::Op) {
                            if (tok.text == "(" || tok.text == "[" || tok.text == "{") nest++;
                            if (tok.text == ")" || tok.text == "]" || tok.text == "}") nest--;
                            if (nest == 0 && tok.text == "<") angle++;
                            if (nest == 0 && (tok.text == ">" || tok.text == ">>")) angle--;
                            if (nest == 0 && angle == 1 && tok.text == ",") {
                                name.args.push_back(Spell(arg_start, k));
                                arg_start = k + 1;
                            }
                        }
                        k++;
                    }
                    if (arg_start < close - 1) name.args.push_back(Spell(arg_start, close - 1));
                    if (leftover) {
                        // The closing token still has a `>` in it for an
                        // enclosing list: land on it and split it there.
                        i_ = close - 1;
                        EatTemplateClose();
                    } else {
                        i_ = close;
                    }
                }
            }
            if (AtOp("::") && Peek().kind == CppTokKind::Keyword && Peek().text == "operator") {
                Next();
                name.last = ParseOperatorName();
                name.name_pos = t_[i_ > 0 ? i_ - 1 : 0].start;
                name.name_end = t_[i_ > 0 ? i_ - 1 : 0].end;
                break;
            }
            if (AtOp("::") && (Peek().kind == CppTokKind::Ident || Peek().text == "~" || Peek().text == "template")) {
                Next();
                continue;
            }
            break;
        }
        name.spelled = Spell(start, i_);
        const size_t sep = name.spelled.rfind("::");
        name.qualifier = sep == std::string::npos ? "" : name.spelled.substr(0, sep + 2);
        name.valid = !name.last.empty();
        return name;
    }

    /** @brief Consumes an `operator` name (`operator+`, `operator()`, `operator std::string`, `operator""_km`). */
    std::string ParseOperatorName() {
        std::string spelled = "operator";
        Next();  // `operator`
        if (Cur().kind == CppTokKind::String && Cur().text.size() >= 2 && Cur().text.rfind("\"\"", 0) == 0) {
            spelled += Cur().text;
            Next();
            if (AtName()) {
                spelled += Cur().text;
                Next();
            }
            return spelled;
        }
        if (AtKw("new") || AtKw("delete")) {
            spelled += " " + Cur().text;
            Next();
            if (AtOp("[")) {
                Next();
                if (AtOp("]")) Next();
                spelled += "[]";
            }
            return spelled;
        }
        if (Cur().kind == CppTokKind::Op) {
            if (AtOp("(") && Peek().text == ")") {
                Next();
                Next();
                return spelled + "()";
            }
            if (AtOp("[") && Peek().text == "]") {
                Next();
                Next();
                return spelled + "[]";
            }
            spelled += Cur().text;
            Next();
            // `operator->*`, `operator()` and the compound assignments are
            // one token each; nothing more to gather.
            return spelled;
        }
        // A conversion operator: `operator const char *()`. Only the
        // type is part of the name -- letting a full declarator parse
        // here would swallow the `()` and the function body with it.
        const size_t start = i_;
        Specifiers spec;
        ParseDeclSpecifiers(spec, true);
        while (AtOp("*") || AtOp("&") || AtOp("&&") || AtKw("const") || AtKw("volatile")) Next();
        spelled += " " + Spell(start, i_);
        return spelled;
    }

    // --- Declaration specifiers ---------------------------------------

    struct Specifiers {
        CppNodePtr type;      // the Type node, or null when there was none
        CppNodePtr embedded;  // a class/enum *definition* written in place
        std::vector<CppNodePtr> attributes;
        bool is_typedef = false;
        bool is_static = false;
        bool is_extern = false;
        bool is_inline = false;
        bool is_virtual = false;
        bool is_explicit = false;
        bool is_constexpr = false;
        bool is_friend = false;
        bool is_mutable = false;
        bool is_const = false;
        bool is_thread_local = false;
        bool have_type = false;
        bool is_template = false;
        CppPos start;
    };

    /**
     * @brief Reports whether a keyword can never follow a type in a declaration specifier sequence.
     *
     * `const` and `volatile` can (`size_t const x`), which is why they
     * are absent: their presence here would turn every east-const
     * declaration into a macro plus a type.
     */
    static bool IsPostTypeImpossibleKeyword(const std::string &text) {
        static const std::unordered_set<std::string> kSet = {
            "static",   "extern",   "inline",  "virtual", "explicit",     "constexpr", "consteval",
            "constinit", "mutable", "register", "typedef", "friend",      "thread_local",
            "class",    "struct",   "enum",    "union",   "__inline",     "__inline__",
        };
        return kSet.count(text) != 0 || IsCppFundamentalType(text);
    }

    /** @brief Reports whether a token can only be a declaration specifier, never a declarator. */
    static bool IsSpecifierKeyword(const std::string &text) {
        static const std::unordered_set<std::string> kSet = {
            "static",    "extern",   "inline",  "virtual",      "explicit", "constexpr", "consteval",
            "constinit", "mutable",  "register", "thread_local", "typedef",  "friend",    "const",
            "volatile",  "__inline", "__inline__", "__restrict", "__restrict__", "__signed__", "__const",
        };
        return kSet.count(text) != 0;
    }

    void ParseDeclSpecifiers(Specifiers &spec, bool type_only = false) {
        spec.start = Cur().start;
        const size_t type_start = i_;
        size_t type_end = i_;
        std::string base;
        std::vector<std::string> args;
        bool fundamental = false;
        bool macro_type = false;
        for (;;) {
            SkipAttributes(&spec.attributes);
            if (Cur().kind == CppTokKind::Keyword) {
                const std::string &kw = Cur().text;
                if (kw == "typedef") {
                    spec.is_typedef = true;
                    Next();
                    continue;
                }
                if (kw == "static") {
                    spec.is_static = true;
                    Next();
                    continue;
                }
                if (kw == "extern") {
                    spec.is_extern = true;
                    Next();
                    continue;
                }
                if (kw == "inline" || kw == "__inline" || kw == "__inline__") {
                    spec.is_inline = true;
                    Next();
                    continue;
                }
                if (kw == "virtual") {
                    spec.is_virtual = true;
                    Next();
                    continue;
                }
                if (kw == "explicit") {
                    spec.is_explicit = true;
                    Next();
                    if (AtOp("(")) SkipBalanced();  // `explicit(bool)`
                    continue;
                }
                if (kw == "constexpr" || kw == "consteval" || kw == "constinit") {
                    spec.is_constexpr = true;
                    Next();
                    continue;
                }
                if (kw == "friend") {
                    spec.is_friend = true;
                    Next();
                    continue;
                }
                if (kw == "mutable") {
                    spec.is_mutable = true;
                    Next();
                    continue;
                }
                if (kw == "thread_local") {
                    spec.is_thread_local = true;
                    Next();
                    continue;
                }
                if (kw == "const") {
                    spec.is_const = true;
                    Next();
                    continue;
                }
                if (kw == "volatile" || kw == "register" || kw == "__restrict" || kw == "__restrict__" ||
                    kw == "__const" || kw == "__signed__") {
                    Next();
                    continue;
                }
                if (kw == "class" || kw == "struct" || kw == "union") {
                    CppNodePtr cls = ParseClassSpecifier();
                    if (cls) {
                        base = cls->name;
                        type_end = i_;
                        spec.have_type = true;
                        if (cls->is_definition) spec.embedded = std::move(cls);
                    }
                    continue;
                }
                if (kw == "enum") {
                    CppNodePtr en = ParseEnumSpecifier();
                    if (en) {
                        base = en->name;
                        type_end = i_;
                        spec.have_type = true;
                        if (en->is_definition) spec.embedded = std::move(en);
                    }
                    continue;
                }
                if (kw == "decltype" || kw == "__typeof__" || kw == "__decltype" || kw == "__typeof") {
                    Next();
                    if (AtOp("(")) SkipBalanced();
                    base = "decltype";
                    spec.have_type = true;
                    type_end = i_;
                    continue;
                }
                if (kw == "typename") {
                    Next();
                    continue;
                }
                if (IsCppFundamentalType(kw)) {
                    // `unsigned long int`: keep absorbing, and let a real
                    // type specifier replace an identifier we had taken
                    // for the type (an unknown macro, in practice).
                    if (spec.have_type && !fundamental) {
                        base.clear();
                    }
                    fundamental = true;
                    base = base.empty() ? kw : base + " " + kw;
                    spec.have_type = true;
                    Next();
                    type_end = i_;
                    continue;
                }
                break;
            }
            if (Cur().kind == CppTokKind::Ident && IsMacroShapedName(Cur().text) && Peek().text == "(" &&
                !spec.have_type && MacroInvocationIsType(i_)) {
                // A SHOUTED macro with arguments where a type belongs:
                // `typedef typename BOOST_CONTAINER_IMPDEF(t) s;`. It
                // expands to a type, so that is what it is treated as.
                base = Cur().text;
                ConsumeMacroInvocation();
                spec.have_type = true;
                macro_type = true;
                type_end = i_;
                continue;
            }
            if (Cur().kind == CppTokKind::Ident || AtOp("::") || AtOp("~")) {
                if (spec.have_type && IsMacroShapedName(Cur().text) &&
                    (Peek().kind == CppTokKind::Ident || Peek().text == "*" || Peek().text == "&" ||
                     Peek().text == "&&" || Peek().text == "::" || Peek().text == "~" ||
                     (Peek().kind == CppTokKind::Keyword && Peek().text == "operator"))) {
                    // The type is read and a SHOUTED name follows it, with
                    // a declarator after that: `_Tp _GLIBCXX20_CONSTEXPR
                    // norm(...)`. The middle one is a macro.
                    Next();
                    continue;
                }
                if (spec.have_type) {
                    // Normally the type is behind us and this is the
                    // declarator. The exception is an attribute macro
                    // taken for the type (`_GLIBCXX_NODISCARD std::string
                    // f();`): if another name follows *and* that name is
                    // itself followed by more type, the first one was the
                    // macro. `DWORD x;` is the shape this must not eat.
                    if (!macro_type) break;
                    const size_t next_name = ScanQualifiedName(i_);
                    if (next_name == i_) break;
                    const CppToken &after_next = At(next_name);
                    const bool type_continues = after_next.kind == CppTokKind::Ident ||
                                                after_next.text == "*" || after_next.text == "&" ||
                                                after_next.text == "&&" || after_next.text == "::";
                    if (!type_continues) break;
                    base.clear();
                    args.clear();
                    spec.have_type = false;
                    macro_type = false;
                }
                const size_t after = ScanQualifiedName(i_);
                if (after == i_) break;
                // `xml_attribute::operator bool()` -- the name scan walks
                // straight through the `operator`, but what it read is
                // the declarator, not this declaration's type.
                bool has_operator = false;
                for (size_t k = i_; k < after; k++) {
                    if (At(k).kind == CppTokKind::Keyword && At(k).text == "operator") has_operator = true;
                }
                if (has_operator) break;
                // A name with a parameter list right after it and no type
                // in front of it is a constructor or a destructor:
                // `PdfDoc();`, `~PdfDoc();`, `Editor::Editor(int w)`. It
                // is the declarator, not the type -- except where only a
                // type can appear at all (`new Foo(x)`, `sizeof(Foo)`),
                // and there the same shape is a type with an initializer.
                if (!type_only && At(after).text == "(" && At(after + 1).text != "*" &&
                    At(after + 1).text != "&" && At(after + 1).text != "(") {
                    break;
                }
                // An identifier followed by another identifier, `*`, `&`
                // or `(` is the type; an identifier followed by a
                // specifier keyword or a real type is a macro to absorb.
                const ParsedName name = ParseQualifiedName();
                base = name.spelled;
                args = name.args;
                spec.have_type = true;
                macro_type = name.qualifier.empty() && IsMacroShapedName(name.last);
                type_end = i_;
                if (Cur().kind == CppTokKind::Keyword && IsPostTypeImpossibleKeyword(Cur().text)) {
                    // `MEP_API static void f();` -- what we just read was
                    // an attribute macro, not the type. Deliberately not
                    // triggered by `const` or `volatile`: `size_t const
                    // x = y;` is east const, and the type is the name we
                    // just read.
                    spec.have_type = false;
                    base.clear();
                    args.clear();
                    continue;
                }
                continue;
            }
            break;
        }
        if (type_end <= type_start && !spec.have_type) return;
        CppNodePtr type = MakeNode(CppNodeKind::Type);
        type->start = t_[type_start].start;
        type->end = t_[type_end > type_start ? type_end - 1 : type_start].end;
        type->type_text = Spell(type_start, type_end);
        type->type_base = base;
        type->type_args = args;
        type->is_const = spec.is_const;
        type->name = base;
        spec.type = std::move(type);
    }

    // --- Declarators --------------------------------------------------

    struct Declarator {
        ParsedName name;
        int ptr_depth = 0;
        bool is_ref = false;
        bool is_function = false;
        bool is_array = false;
        bool is_pack = false;
        bool is_pure = false;
        bool is_defaulted = false;
        bool is_deleted = false;
        bool is_override = false;
        bool is_const_method = false;
        bool is_noexcept = false;
        bool has_initializer = false;
        bool valid = false;
        std::string trailing;
        std::vector<CppNodePtr> params;
        std::vector<CppNodePtr> bindings;  // structured binding names
        CppNodePtr trailing_return;
        CppNodePtr initializer;
        CppPos start;
        CppPos end;
        size_t type_suffix_start = 0;  // for spelling `T *` / `T &`
        size_t type_suffix_end = 0;
    };

    /**
     * @brief Reports whether the parenthesised list at `start` reads as constructor arguments rather than parameters.
     *
     * This is the "most vexing parse" decision, and the only one this
     * parser deliberately resolves *against* the standard: `Timer t(0);`
     * really is a function declaration by the grammar, but nobody writing
     * it means that, and treating it as a variable is what makes hover
     * and go-to-definition right on the line the reader is looking at.
     * Only an argument that cannot be a parameter declaration -- a
     * literal, an operator, a call -- flips the decision.
     */
    bool LooksLikeConstructorArguments(size_t start) const {
        size_t k = start + 1;
        int nest = 1;
        bool any_argument = false;
        // Each top-level argument is judged on its own, up to any `=`
        // (which introduces a default argument, and whose literal must
        // not be mistaken for an expression argument -- `int f(int n = 0)`
        // is what made that worth saying out loud).
        size_t arg_start = k;
        bool expression_argument = false;
        auto judge = [&](size_t from, size_t to) {
            size_t limit = to;
            int inner = 0;
            for (size_t j = from; j < to; j++) {
                const std::string &text = At(j).text;
                if (At(j).kind == CppTokKind::Op) {
                    if (text == "(" || text == "[" || text == "{") inner++;
                    if (text == ")" || text == "]" || text == "}") inner--;
                    if (inner == 0 && text == "=") {
                        limit = j;
                        break;
                    }
                }
            }
            if (from >= limit) return;
            any_argument = true;
            const CppToken &first = At(from);
            if (first.kind == CppTokKind::Number || first.kind == CppTokKind::String ||
                first.kind == CppTokKind::Char) {
                expression_argument = true;
                return;
            }
            if (first.kind == CppTokKind::Keyword) {
                if (first.text == "true" || first.text == "false" || first.text == "nullptr" ||
                    first.text == "this" || first.text == "new" || first.text == "sizeof" ||
                    first.text == "static_cast" || first.text == "dynamic_cast" ||
                    first.text == "const_cast" || first.text == "reinterpret_cast" ||
                    first.text == "delete" || first.text == "throw" || first.text == "alignof") {
                    expression_argument = true;
                }
                return;  // a type keyword: this is a parameter
            }
            if (first.kind == CppTokKind::Op) {
                // `-1`, `*p`, `&x`, `(a + b)`: none of them start a
                // parameter declaration.
                if (first.text == "-" || first.text == "+" || first.text == "!" || first.text == "~" ||
                    first.text == "*" || first.text == "&") {
                    expression_argument = true;
                }
                return;
            }
            // An identifier: a parameter if a name follows it (`Foo bar`),
            // an expression if an operator does (`a + b`, `a.b`, `f(x)`).
            const size_t after = ScanQualifiedName(from);
            if (after >= limit) return;  // a bare type name: an unnamed parameter
            size_t j = after;
            while (j < limit) {
                const std::string &text = At(j).text;
                if (text == "*" || text == "&" || text == "&&" || text == "..." || text == "const" ||
                    text == "volatile" || text == "__restrict" || text == "__restrict__") {
                    j++;
                    continue;
                }
                // A pointer to member: `_M1 _S1::*__m1`.
                if (At(j).kind == CppTokKind::Ident && At(j + 1).text == "::" && At(j + 2).text == "*") {
                    j += 3;
                    continue;
                }
                break;
            }
            // What is left has to be a parameter name, an array bound,
            // or nothing at all (`S(S &&)` declares an unnamed
            // parameter). `bytes * 2` gets here as `bytes` + `*` + `2`,
            // and the `2` is what says this is an expression.
            // The parameter's own name, optionally with an array bound,
            // and optionally followed by more names -- which can only be
            // macros (`int_type __c _IsUnused = eof()`).
            if (At(j).text == "(" && (At(j + 1).text == "*" || At(j + 1).text == "&")) {
                // A function-pointer parameter, `void (*fn)(int)` -- but
                // only if a second parameter list follows the group.
                // `std::move(*this)` has the same first three tokens and
                // is an argument.
                size_t close = j;
                int nest_local = 0;
                while (close < limit) {
                    if (At(close).text == "(") nest_local++;
                    if (At(close).text == ")") {
                        nest_local--;
                        if (nest_local == 0) {
                            close++;
                            break;
                        }
                    }
                    close++;
                }
                if (close < limit && (At(close).text == "(" || At(close).text == "[")) return;
            }
            // A pointer-to-member parameter, `R (T::*)`.
            if (At(j).text == "(" && At(j + 1).kind == CppTokKind::Ident) {
                size_t probe = j + 1;
                while (probe < limit && (At(probe).kind == CppTokKind::Ident || At(probe).text == "::")) probe++;
                if (probe < limit && At(probe).text == "*" && At(probe - 1).text == "::") return;
            }
            size_t tail = j;
            while (tail < limit && At(tail).kind == CppTokKind::Ident) tail++;
            const bool parameter_name =
                j >= limit || (tail > j && (tail >= limit || At(tail).text == "[" ||
                                            At(tail).text == "(" ||  // `_Tp __func(_Tp)`: a function parameter
                                            At(tail).text == "__attribute__" ||
                                            At(tail).text == "__attribute"));
            if (!parameter_name) expression_argument = true;
        };
        while (k < t_.size() && At(k).kind != CppTokKind::End && nest > 0) {
            const CppToken &tok = At(k);
            if (tok.kind == CppTokKind::Op) {
                // A template argument list has commas of its own
                // (`std::map<K, V> &m`), which are not argument
                // separators; skipping the whole list is what keeps that
                // parameter from being read as two.
                if (tok.text == "<") {
                    const size_t close = TemplateArgumentsAhead(k);
                    if (close != 0) {
                        k = close;
                        continue;
                    }
                }
                if (tok.text == "(" || tok.text == "[" || tok.text == "{") nest++;
                if (tok.text == ")" || tok.text == "]" || tok.text == "}") {
                    nest--;
                    if (nest == 0) break;
                }
                if (nest == 1 && tok.text == ",") {
                    judge(arg_start, k);
                    arg_start = k + 1;
                }
            }
            k++;
        }
        judge(arg_start, k);
        return any_argument && expression_argument;
    }

    void ParseDeclarator(Declarator &d, bool abstract_ok, bool allow_function_suffix = true) {
        d.start = Cur().start;
        const size_t prefix_start = i_;
        for (;;) {
            SkipAttributes(nullptr);
            if (AtOp("*")) {
                d.ptr_depth++;
                Next();
                continue;
            }
            if (AtOp("&") || AtOp("&&")) {
                d.is_ref = true;
                Next();
                continue;
            }
            if (AtKw("const") || AtKw("volatile") || AtKw("__restrict") || AtKw("__restrict__")) {
                Next();
                continue;
            }
            // A pointer to member: `int Class::*p`, `bool S::*f`. The
            // name scan stops before a `::` that is not followed by a
            // name, so both spellings have to be checked here.
            const size_t after = ScanQualifiedName(i_);
            if (after > i_ && At(after).text == "*") {
                i_ = after + 1;
                d.ptr_depth++;
                continue;
            }
            if (after > i_ && At(after).text == "::" && At(after + 1).text == "*") {
                i_ = after + 2;
                d.ptr_depth++;
                continue;
            }
            break;
        }
        d.type_suffix_start = prefix_start;
        d.type_suffix_end = i_;
        if (AtOp("...")) {
            d.is_pack = true;
            Next();
        }

        // A parenthesised declarator: `void (*callback)(int)`. Never in
        // a new-type-id, where `new P((P const &)*a)` has the same first
        // three tokens and the parentheses are the constructor's.
        if (allow_function_suffix && AtOp("(") &&
            (Peek().text == "*" || Peek().text == "&" || Peek().text == "(" ||
                           (Peek().kind == CppTokKind::Ident && Peek(2).text == ")" && At(i_ + 3).text == "("))) {
            Next();
            ParseDeclarator(d, abstract_ok);
            if (!EatOp(")")) ErrorHere("expected-paren", "Expected ')' to close this declarator");
        } else if (AtKw("operator")) {
            d.name.spelled = ParseOperatorName();
            d.name.last = d.name.spelled;
            d.name.name_pos = d.start;
            d.name.name_end = t_[i_ > 0 ? i_ - 1 : 0].end;
            d.name.valid = true;
        } else if (AtOp("[")) {
            // A structured binding: `auto [a, b] = pair;`.
            Next();
            while (!AtEnd() && !AtOp("]")) {
                if (AtName()) {
                    CppNodePtr binding = MakeNode(CppNodeKind::Variable);
                    binding->name = Cur().text;
                    binding->name_pos = Cur().start;
                    binding->name_end = Cur().end;
                    binding->start = Cur().start;
                    binding->end = Cur().end;
                    d.bindings.push_back(std::move(binding));
                }
                Next();
            }
            EatOp("]");
            d.valid = true;
        } else {
            const size_t after = ScanQualifiedName(i_);
            if (after > i_) {
                d.name = ParseQualifiedName();
                // `Foo::operator+`: the qualified name scan stops at the
                // keyword, so pick the operator up here.
                if (AtKw("operator")) {
                    const std::string qualifier = d.name.spelled;
                    d.name.spelled = qualifier + ParseOperatorName();
                    d.name.last = d.name.spelled.substr(qualifier.size());
                }
            } else if (!abstract_ok) {
                return;
            }
        }
        d.valid = d.valid || d.name.valid || abstract_ok;

        // Suffixes: parameter lists and array bounds, in the order written.
        for (;;) {
            if (AtOp("(") && allow_function_suffix) {
                if (!d.is_function && d.name.valid && LooksLikeConstructorArguments(i_)) {
                    // A variable with constructor arguments, not a
                    // function declaration (see LooksLikeConstructorArguments).
                    const size_t start = i_;
                    Next();
                    CppNodePtr init = MakeNode(CppNodeKind::Call);
                    init->start = t_[start].start;
                    ParseCallArguments(init->kids);
                    if (!EatOp(")")) ErrorHere("expected-paren", "Expected ')' to close these arguments");
                    init->end = t_[i_ > 0 ? i_ - 1 : 0].end;
                    d.initializer = std::move(init);
                    d.has_initializer = true;
                    break;
                }
                d.is_function = true;
                Next();
                ParseParameterList(d.params);
                if (!EatOp(")")) ErrorHere("expected-paren", "Expected ')' to close this parameter list");
                const size_t trailing_start = i_;
                for (;;) {
                    if (AtKw("const")) {
                        d.is_const_method = true;
                        Next();
                        continue;
                    }
                    if (AtKw("volatile") || AtOp("&") || AtOp("&&")) {
                        Next();
                        continue;
                    }
                    if (AtKw("noexcept")) {
                        d.is_noexcept = true;
                        Next();
                        if (AtOp("(")) SkipBalanced();
                        continue;
                    }
                    if (AtKw("throw")) {
                        Next();
                        if (AtOp("(")) SkipBalanced();
                        continue;
                    }
                    if (AtIdent("override")) {
                        d.is_override = true;
                        Next();
                        continue;
                    }
                    if (AtIdent("final")) {
                        Next();
                        continue;
                    }
                    if (AtKw("requires")) {
                        ParseRequiresClause();
                        continue;
                    }
                    if (SkipAttributes(nullptr)) continue;
                    if (SkipMacroInvocation()) continue;
                    break;
                }
                if (EatOp("->")) {
                    d.trailing_return = ParseTypeId();
                }
                d.trailing = Spell(trailing_start, i_);
                continue;
            }
            if (AtOp("[")) {
                d.is_array = true;
                SkipBalanced();
                continue;
            }
            if (d.name.valid && SkipMacroInvocation()) continue;
            // `void f(const locale &__loc __attribute__((__unused__)))`:
            // an attribute after the declarator, which is where the
            // GNU spelling is allowed to sit.
            if (SkipAttributes(nullptr)) continue;
            break;
        }
        d.end = t_[i_ > 0 ? i_ - 1 : 0].end;
    }

    void ParseParameterList(std::vector<CppNodePtr> &out) {
        while (!AtEnd() && !AtOp(")")) {
            if (AtOp("...")) {
                CppNodePtr param = MakeNode(CppNodeKind::Param);
                param->start = Cur().start;
                param->end = Cur().end;
                param->is_variadic = true;
                out.push_back(std::move(param));
                Next();
                EatOp(",");
                continue;
            }
            const size_t start = i_;
            Specifiers spec;
            ParseDeclSpecifiers(spec, true);
            Declarator d;
            ParseDeclarator(d, true);
            // Two names in a row in a parameter list: the second is a
            // macro from a header nobody read (`const locale &__loc
            // _IsUnused`). Not necessarily SHOUTED, but in this one
            // position there is nothing else it can be.
            while (AtName()) {
                if (!d.name.valid) {
                    // The declarator did not find a name because a macro
                    // stood where the type goes (`BOOST_RV_REF(T) x`):
                    // this identifier is the parameter's name.
                    d.name.last = Cur().text;
                    d.name.name_pos = Cur().start;
                    d.name.name_end = Cur().end;
                    d.name.valid = true;
                }
                Next();
            }
            CppNodePtr param = MakeNode(CppNodeKind::Param);
            param->start = t_[start].start;
            param->name = d.name.last;
            param->name_pos = d.name.name_pos;
            param->name_end = d.name.name_end;
            param->is_variadic = d.is_pack;
            CppNodePtr type = std::move(spec.type);
            if (!type) type = MakeNode(CppNodeKind::Type);
            type->ptr_depth = d.ptr_depth;
            type->is_ref = d.is_ref;
            type->type_text = Spell(start, d.name.valid ? i_ - 0 : i_);
            if (d.name.valid && !d.name.last.empty() && type->type_text.size() > d.name.last.size()) {
                // Trim the parameter's own name off its type's spelling.
                const size_t at = type->type_text.rfind(d.name.last);
                if (at != std::string::npos && at + d.name.last.size() == type->type_text.size()) {
                    type->type_text = type->type_text.substr(0, at);
                    while (!type->type_text.empty() && type->type_text.back() == ' ') type->type_text.pop_back();
                }
            }
            param->kids.push_back(std::move(type));
            if (EatOp("=")) {
                param->kids.push_back(ParseAssignExpression());
            }
            param->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            out.push_back(std::move(param));
            if (!EatOp(",")) break;
        }
    }

    /** @brief Consumes a `requires` clause or requires-expression, which has its own miniature grammar. */
    void ParseRequiresClause(bool expression_context = false) {
        if (!EatKw("requires")) return;
        for (;;) {
            EatOp("!");
            if (AtOp("(")) {
                SkipBalanced();
                // `requires (T t) { t.begin(); }` is a requires-expression
                // and the braces are its body; `void f() requires (C<T>)
                // { ... }` is a requires-*clause* and the braces are the
                // function's. Only an expression context can be the
                // former, which is why the caller has to say which it is.
                if (expression_context && AtOp("{")) SkipBalanced();
            } else if (AtKw("requires")) {
                Next();
                if (AtOp("(")) SkipBalanced();
                if (AtOp("{")) SkipBalanced();
            } else if (AtOp("{")) {
                SkipBalanced();
            } else {
                const size_t after = ScanQualifiedName(i_);
                if (after == i_) return;
                i_ = after;
            }
            if (AtOp("&&") || AtOp("||")) {
                Next();
                continue;
            }
            return;
        }
    }

    // --- Declarations -------------------------------------------------

    void ParseDeclarationSeq(std::vector<CppNodePtr> &out, bool in_class, const std::string &default_access) {
        std::string access = default_access;
        while (!AtEnd() && !AtOp("}")) {
            const size_t before = i_;
            CppNodePtr decl = ParseDeclaration(in_class, access);
            if (decl) {
                if (decl->kind == CppNodeKind::Access) {
                    access = decl->name;
                }
                if (decl->kind == CppNodeKind::DeclStmt) {
                    for (CppNodePtr &kid : decl->kids) out.push_back(std::move(kid));
                } else {
                    out.push_back(std::move(decl));
                }
            }
            if (i_ == before) {
                ErrorHere("syntax-error", "Unexpected '" + Cur().text + "' here");
                Next();
                SkipToDeclarationEnd();
            }
        }
    }

    CppNodePtr ParseDeclaration(bool in_class, const std::string &access) {
        DepthGuard guard(this);
        if (guard.overflow) {
            SkipToDeclarationEnd();
            return nullptr;
        }
        std::vector<CppNodePtr> attributes;
        SkipAttributes(&attributes);
        if (AtOp(";")) {
            CppNodePtr empty = MakeNode(CppNodeKind::Empty);
            empty->start = Cur().start;
            empty->end = Cur().end;
            Next();
            return empty;
        }
        if ((AtKw("public") || AtKw("private") || AtKw("protected")) && Peek().text == ":") {
            CppNodePtr node = MakeNode(CppNodeKind::Access);
            node->name = Cur().text;
            node->start = Cur().start;
            node->end = Peek().end;
            Next();
            Next();
            return node;
        }
        if (AtKw("namespace")) return ParseNamespace();
        if (AtKw("inline") && Peek().kind == CppTokKind::Keyword && Peek().text == "namespace") {
            Next();
            CppNodePtr ns = ParseNamespace();
            if (ns) ns->str_value = "inline";
            return ns;
        }
        if (AtKw("using")) return ParseUsing();
        if (AtKw("static_assert")) return ParseStaticAssert();
        if (AtKw("template") || (AtKw("export") && Peek().text == "template")) return ParseTemplate(in_class, access);
        if (AtKw("extern") && Peek().kind == CppTokKind::String) return ParseLinkageSpec(in_class);
        if (AtKw("asm") || AtKw("__asm__") || AtKw("__asm")) {
            CppNodePtr node = MakeNode(CppNodeKind::AsmStmt);
            node->start = Cur().start;
            Next();
            if (AtKw("volatile") || AtKw("__volatile__")) Next();
            if (AtOp("(")) SkipBalanced();
            EatOp(";");
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (AtKw("export") && Peek().kind != CppTokKind::End) {
            // `export` before a declaration, or `export module m;`.
            const CppPos start = Cur().start;
            Next();
            if (AtIdent("module")) return ParseModuleDeclaration(start, "export module");
            CppNodePtr inner = ParseDeclaration(in_class, access);
            return inner;
        }
        if ((AtIdent("module") || AtIdent("import")) && Cur().bol) {
            // Only at the start of a line, and only when what follows
            // cannot be a declarator: `module` and `import` are ordinary
            // identifiers everywhere else.
            const bool module_decl = Cur().text == "module";
            const CppToken &next = Peek();
            const bool looks_like_module = next.text == ";" || next.kind == CppTokKind::Ident ||
                                           next.kind == CppTokKind::String || next.text == "<";
            if (looks_like_module && !(next.kind == CppTokKind::Ident && Peek(2).text == "(")) {
                return ParseModuleDeclaration(Cur().start, module_decl ? "module" : "import");
            }
        }
        if (IsStandaloneMacroInvocation()) {
            // `_GLIBCXX_BEGIN_NAMESPACE_VERSION` on a line of its own:
            // a macro with no semicolon, which only the header that
            // defines it can explain. Consumed silently -- see
            // SkipMacroInvocation.
            CppNodePtr node = MakeNode(CppNodeKind::Empty);
            node->start = Cur().start;
            ConsumeMacroInvocation();
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (AtKw("friend")) {
            const CppPos start = Cur().start;
            Next();
            CppNodePtr inner = ParseDeclaration(in_class, access);
            CppNodePtr node = MakeNode(CppNodeKind::Friend);
            node->start = start;
            node->end = inner ? inner->end : start;
            if (inner) node->kids.push_back(std::move(inner));
            return node;
        }
        // A SHOUTED macro invocation whose arguments are not C++ at all
        // (`EM_JS(void, f, (int a), { ...javascript... })`): if reading
        // it as a declaration does not work, it was never a declaration,
        // and the honest recovery is to skip the invocation whole.
        if (Cur().kind == CppTokKind::Ident && IsMacroShapedName(Cur().text) && Peek().text == "(") {
            const Snapshot save = Save();
            const size_t macro_start = i_;
            CppNodePtr decl = ParseSimpleDeclaration(in_class, access, std::move(attributes));
            if (decl && !ErrorsSince(save)) return decl;
            Restore(save);
            i_ = macro_start;
            CppNodePtr node = MakeNode(CppNodeKind::Empty);
            node->start = Cur().start;
            ConsumeMacroInvocation();
            EatOp(";");
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        return ParseSimpleDeclaration(in_class, access, std::move(attributes));
    }

    CppNodePtr ParseModuleDeclaration(const CppPos &start, const char *what) {
        CppNodePtr node = MakeNode(CppNodeKind::ModuleDecl);
        node->start = start;
        node->str_value = what;
        Next();
        std::string name;
        while (!AtEnd() && !AtOp(";")) {
            if (!name.empty() && Cur().space_before) name += ' ';
            name += Cur().text;
            Next();
        }
        EatOp(";");
        node->name = name;
        node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
        return node;
    }

    CppNodePtr ParseNamespace() {
        CppNodePtr node = MakeNode(CppNodeKind::Namespace);
        node->start = Cur().start;
        Next();
        SkipAttributes(nullptr);
        if (AtKw("inline")) {
            node->str_value = "inline";
            Next();
        }
        std::string name;
        while (AtName()) {
            if (!name.empty()) name += "::";
            name += Cur().text;
            node->name_pos = Cur().start;
            node->name_end = Cur().end;
            Next();
            if (!EatOp("::")) break;
        }
        node->name = name;
        // `namespace std _GLIBCXX_VISIBILITY(default) {` -- and, once
        // that macro has been expanded, `namespace std
        // __attribute__((__visibility__("default"))) {`.
        while (SkipMacroInvocation() || SkipAttributes(nullptr)) {
        }
        if (EatOp("=")) {
            node->kind = CppNodeKind::NamespaceAlias;
            const ParsedName target = ParseQualifiedName();
            node->str_value = target.spelled;
            EatOp(";");
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (EatOp("{")) {
            ParseDeclarationSeq(node->body, false, "");
            if (!EatOp("}")) ErrorHere("expected-brace", "Expected '}' to close this namespace");
        } else {
            ErrorHere("expected-brace", "Expected '{' after a namespace name");
        }
        node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
        return node;
    }

    CppNodePtr ParseUsing() {
        const CppPos start = Cur().start;
        Next();
        if (AtKw("namespace")) {
            Next();
            CppNodePtr node = MakeNode(CppNodeKind::UsingDirective);
            node->start = start;
            const ParsedName name = ParseQualifiedName();
            node->name = name.spelled;
            node->name_pos = name.name_pos;
            node->name_end = name.name_end;
            EatOp(";");
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        SkipAttributes(nullptr);
        const ParsedName name = ParseQualifiedName();
        if (AtOp("=")) {
            Next();
            CppNodePtr node = MakeNode(CppNodeKind::UsingAlias);
            node->start = start;
            node->name = name.last;
            node->name_pos = name.name_pos;
            node->name_end = name.name_end;
            node->kids.push_back(ParseTypeId());
            EatOp(";");
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        CppNodePtr node = MakeNode(CppNodeKind::UsingDecl);
        node->start = start;
        node->name = name.last;
        node->str_value = name.spelled;
        node->name_pos = name.name_pos;
        node->name_end = name.name_end;
        while (!AtEnd() && !AtOp(";")) Next();
        EatOp(";");
        node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
        return node;
    }

    CppNodePtr ParseStaticAssert() {
        CppNodePtr node = MakeNode(CppNodeKind::StaticAssert);
        node->start = Cur().start;
        Next();
        if (EatOp("(")) {
            node->kids.push_back(ParseAssignExpression());
            if (EatOp(",")) node->kids.push_back(ParseAssignExpression());
            if (!EatOp(")")) ErrorHere("expected-paren", "Expected ')' to close static_assert");
        }
        EatOp(";");
        node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
        return node;
    }

    CppNodePtr ParseLinkageSpec(bool in_class) {
        CppNodePtr node = MakeNode(CppNodeKind::LinkageSpec);
        node->start = Cur().start;
        Next();
        node->name = Cur().text;
        if (node->name.size() >= 2) node->name = node->name.substr(1, node->name.size() - 2);
        Next();
        if (EatOp("{")) {
            ParseDeclarationSeq(node->body, in_class, "");
            if (!EatOp("}")) ErrorHere("expected-brace", "Expected '}' to close this extern block");
        } else {
            CppNodePtr inner = ParseDeclaration(in_class, "");
            if (inner) node->body.push_back(std::move(inner));
        }
        node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
        return node;
    }

    CppNodePtr ParseTemplate(bool in_class, const std::string &access) {
        const CppPos start = Cur().start;
        EatKw("export");
        Next();  // `template`
        CppNodePtr node = MakeNode(CppNodeKind::TemplateDecl);
        node->start = start;
        if (EatOp("<")) {
            no_greater_++;
            while (!AtEnd() && !AtOp(">")) {
                if (Cur().kind == CppTokKind::Ident && IsMacroShapedName(Cur().text) && Peek().text == "(") {
                    // `template <BOOST_PP_ENUM_PARAMS(N, class T)>`: the
                    // parameters themselves come out of a macro.
                    ConsumeMacroInvocation();
                    if (!EatOp(",")) break;
                    continue;
                }
                CppNodePtr param = MakeNode(CppNodeKind::TemplateParam);
                param->start = Cur().start;
                if (AtKw("template")) {
                    // A template template parameter: skip its own list.
                    Next();
                    if (AtOp("<")) {
                        int angle = 0;
                        do {
                            if (AtOp("<")) angle++;
                            if (AtOp(">")) angle--;
                            if (Cur().text == ">>") angle -= 2;
                            Next();
                        } while (!AtEnd() && angle > 0);
                    }
                    param->str_value = "template";
                    if (AtKw("class") || AtKw("typename")) Next();
                } else if (AtKw("class") || AtKw("typename")) {
                    param->str_value = "type";
                    Next();
                } else {
                    param->str_value = "non-type";
                    const size_t type_start = i_;
                    Specifiers spec;
                    ParseDeclSpecifiers(spec);
                    if (spec.type) {
                        param->kids.push_back(std::move(spec.type));
                    } else {
                        // A constrained parameter: `Integral auto N` or
                        // just a concept name.
                        const size_t after = ScanQualifiedName(i_);
                        if (after > i_) i_ = after;
                    }
                    (void)type_start;
                }
                if (AtOp("...")) {
                    param->is_variadic = true;
                    Next();
                }
                if (AtName()) {
                    param->name = Cur().text;
                    param->name_pos = Cur().start;
                    param->name_end = Cur().end;
                    Next();
                }
                if (EatOp("=")) {
                    if (param->str_value == "type") {
                        param->kids.push_back(ParseTypeId());
                    } else {
                        param->kids.push_back(ParseAssignExpression());
                    }
                }
                param->end = t_[i_ > 0 ? i_ - 1 : 0].end;
                node->params.push_back(std::move(param));
                if (!EatOp(",")) break;
            }
            if (!EatTemplateClose()) ErrorHere("expected-angle", "Expected '>' to close this template parameter list");
            no_greater_--;
        }
        if (AtKw("requires")) ParseRequiresClause();
        if (AtKw("concept")) {
            Next();
            CppNodePtr concept_node = MakeNode(CppNodeKind::Concept);
            concept_node->start = start;
            if (AtName()) {
                concept_node->name = Cur().text;
                concept_node->name_pos = Cur().start;
                concept_node->name_end = Cur().end;
                Next();
            }
            if (EatOp("=")) concept_node->kids.push_back(ParseAssignExpression());
            EatOp(";");
            concept_node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            concept_node->is_template = true;
            concept_node->template_params = std::move(node->params);
            return concept_node;
        }
        CppNodePtr inner = ParseDeclaration(in_class, access);
        if (!inner) {
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        // The template parameters belong to the declaration itself: the
        // analysis half wants one node per class or function, not a
        // wrapper it has to unwrap everywhere.
        inner->is_template = true;
        inner->start = start;
        inner->template_params = std::move(node->params);
        return inner;
    }

    /**
     * @brief Consumes an unknown macro invocation sitting where the grammar expects nothing.
     *
     * Every real header is full of them -- `namespace std
     * _GLIBCXX_VISIBILITY(default)`, `void abort(void) _GLIBCXX_NOTHROW
     * _GLIBCXX_NORETURN;` -- and they are defined in a header this
     * server never read. Skipping a SHOUTED name (plus the balanced
     * parentheses that may follow it) is how a reader gets past them
     * too, and it costs nothing when the guess is wrong: the name was
     * not going to resolve either way.
     */
    bool SkipMacroInvocation() {
        if (Cur().kind != CppTokKind::Ident || !IsMacroShapedName(Cur().text)) return false;
        ConsumeMacroInvocation();
        return true;
    }

    /** @brief Consumes the identifier at the cursor and the balanced `(...)` after it, if any. */
    void ConsumeMacroInvocation() {
        Next();
        if (AtOp("(")) SkipBalanced();
    }

    /**
     * @brief Reports whether the current token is a SHOUTED macro that stands alone, with no `;` of its own.
     *
     * Decided by what comes after it: a keyword that can only start a
     * fresh declaration, a closing brace, or another such macro. `DWORD
     * handle;` -- the same shape with a name after it -- is a
     * declaration, and stays one.
     */
    bool IsStandaloneMacroInvocation() const {
        if (Cur().kind != CppTokKind::Ident) return false;
        // SHOUTED, or reserved to the implementation
        // (`__glibcxx_function_requires(...)`, which is a macro in every
        // build and a call in none).
        if (!IsMacroShapedName(Cur().text) && Cur().text.rfind("__", 0) != 0) return false;
        size_t k = i_ + 1;
        if (At(k).text == "(") {
            int nest = 0;
            while (k < t_.size() && At(k).kind != CppTokKind::End) {
                if (At(k).text == "(") nest++;
                if (At(k).text == ")") {
                    nest--;
                    if (nest == 0) {
                        k++;
                        break;
                    }
                }
                k++;
            }
        }
        const CppToken &after = At(k);
        const bool shouted = IsMacroShapedName(Cur().text);
        if (after.kind == CppTokKind::End) return shouted;
        if (after.kind == CppTokKind::Ident) {
            // Two names in a row, both SHOUTED: a run of attribute
            // macros. In libstdc++ `__node_base_ptr __n = ...` has the
            // same shape and is an ordinary declaration, which is why a
            // reserved-looking name is not enough on its own.
            if (shouted && IsMacroShapedName(after.text)) return true;
            // Two *invocations* in a row, though, settle it whatever
            // they are called: `__glibcxx_function_requires(A)
            // __glibcxx_requires_valid_range(b, e)`. A call cannot follow
            // a call without a `;` between them.
            const bool reserved = IsMacroShapedName(after.text) || after.text.rfind("__", 0) == 0;
            return reserved && At(k + 1).text == "(";
        }
        if (after.kind == CppTokKind::Keyword) {
            // A statement keyword: whatever precedes it was not a call,
            // because a call has to be followed by `;` or an operator.
            // This much holds for any macro name, SHOUTED or reserved.
            if (after.text == "if" || after.text == "for" || after.text == "while" ||
                after.text == "switch" || after.text == "do" || after.text == "return" ||
                after.text == "try" || after.text == "case" || after.text == "goto" ||
                after.text == "typedef" || after.text == "template" || after.text == "namespace" ||
                after.text == "using" || after.text == "static_assert") {
                return true;
            }
            // The rest only settle it for a SHOUTED name: they are the
            // shapes an attribute macro takes in front of a declaration
            // (`_GLIBCXX17_DEPRECATED_SUGGEST("...")` above `bool
            // uncaught_exception()`), and a reserved-looking call
            // followed by a type is more likely a missing semicolon.
            if (!shouted) return false;
            if (IsCppFundamentalType(after.text)) return true;
            return after.text == "class" || after.text == "struct" ||
                   after.text == "enum" || after.text == "union" || after.text == "public" ||
                   after.text == "private" || after.text == "protected" || after.text == "static" ||
                   after.text == "inline" || after.text == "virtual" || after.text == "explicit" ||
                   after.text == "constexpr" || after.text == "extern" || after.text == "friend" ||
                   after.text == "typename" || after.text == "const";
        }
        // A missing semicolon before `}` is a real mistake worth
        // reporting, so only a SHOUTED macro is forgiven there.
        return shouted && (after.text == "}" || after.text == "#");
    }

    /**
     * @brief Reports whether a SHOUTED macro invocation stands where a type does, rather than where a name does.
     *
     * `BOOST_CONTAINER_IMPDEF(x) name;` expands to a type;
     * `TEST_F(Fixture, Name) { ... }` expands to a function definition,
     * and its macro name is the declarator. What follows the closing
     * parenthesis is what tells them apart.
     */
    bool MacroInvocationIsType(size_t start) const {
        size_t k = start + 1;
        int nest = 0;
        while (k < t_.size() && At(k).kind != CppTokKind::End) {
            if (At(k).text == "(") nest++;
            if (At(k).text == ")") {
                nest--;
                if (nest == 0) {
                    k++;
                    break;
                }
            }
            k++;
        }
        const CppToken &after = At(k);
        if (after.kind == CppTokKind::Ident) return true;
        if (after.kind == CppTokKind::Keyword) return true;
        return after.text == "*" || after.text == "&" || after.text == "&&" || after.text == "::";
    }

    /** @brief Reports whether a name is shaped like an attribute macro (`MEP_API`, `__declspec`). */
    static bool IsMacroShapedName(const std::string &name) {
        // SHOUTED, and nothing else: `_GLIBCXX_NOTHROW` and `Q_OBJECT`
        // qualify, `__x` and `_M_impl` -- which a real header is full of
        // as ordinary names -- do not.
        bool has_upper = false;
        for (char c : name) {
            if (c >= 'a' && c <= 'z') return false;
            if (c >= 'A' && c <= 'Z') has_upper = true;
        }
        return has_upper && name.size() > 1;
    }

    CppNodePtr ParseClassSpecifier() {
        CppNodePtr node = MakeNode(CppNodeKind::Class);
        node->start = Cur().start;
        node->str_value = Cur().text;  // class / struct / union
        Next();
        SkipAttributes(nullptr);
        // `class MEP_API Foo {`: an export macro sits where the name
        // goes. `struct timeval tv {` has exactly the same shape and
        // means something else entirely, so the macro is recognized the
        // way a reader recognizes it -- by being SHOUTED, or reserved
        // (`__declspec`-style). Anything else is the class's own name.
        while (AtName() && IsMacroShapedName(Cur().text) &&
               (Peek().kind == CppTokKind::Ident || Peek().text == "(")) {
            SkipMacroInvocation();
        }
        if (AtName()) {
            const ParsedName name = ParseQualifiedName();
            node->name = name.last;
            node->name_pos = name.name_pos;
            node->name_end = name.name_end;
            node->qualified = name.spelled;
        }
        EatIdent("final");
        SkipAttributes(nullptr);
        if (EatOp(":")) {
            while (!AtEnd() && !AtOp("{")) {
                CppNodePtr base = MakeNode(CppNodeKind::BaseSpecifier);
                base->start = Cur().start;
                base->str_value = node->str_value == "class" ? "private" : "public";
                for (;;) {
                    if (AtKw("public") || AtKw("protected") || AtKw("private")) {
                        base->str_value = Cur().text;
                        Next();
                        continue;
                    }
                    if (AtKw("virtual")) {
                        base->is_virtual = true;
                        Next();
                        continue;
                    }
                    break;
                }
                const ParsedName name = ParseQualifiedName();
                base->name = name.spelled;
                base->name_pos = name.name_pos;
                base->name_end = name.name_end;
                if (!name.valid && (AtKw("decltype") || AtKw("typename") || AtKw("__typeof__"))) {
                    // `: decltype(__detail::__or_fn<_Bn...>(0))` -- a base
                    // class can be any type-id, not only a name.
                    const size_t type_start = i_;
                    Next();
                    if (AtOp("(")) SkipBalanced();
                    base->name = Spell(type_start, i_);
                }
                if (AtOp("...")) Next();
                base->end = t_[i_ > 0 ? i_ - 1 : 0].end;
                if (base->name.empty()) {
                    Next();
                } else {
                    node->bases.push_back(std::move(base));
                }
                if (!EatOp(",")) break;
            }
        }
        if (AtOp("{")) {
            Next();
            node->is_definition = true;
            ParseDeclarationSeq(node->body, true, node->str_value == "class" ? "private" : "public");
            if (!EatOp("}")) ErrorHere("expected-brace", "Expected '}' to close this " + node->str_value);
        }
        node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
        return node;
    }

    CppNodePtr ParseEnumSpecifier() {
        CppNodePtr node = MakeNode(CppNodeKind::Enum);
        node->start = Cur().start;
        node->str_value = "enum";
        Next();
        if (AtKw("class") || AtKw("struct")) {
            node->str_value = "enum class";
            Next();
        }
        SkipAttributes(nullptr);
        if (AtName()) {
            const ParsedName name = ParseQualifiedName();
            node->name = name.last;
            node->name_pos = name.name_pos;
            node->name_end = name.name_end;
            node->qualified = name.spelled;
        }
        if (EatOp(":")) {
            const size_t start = i_;
            Specifiers spec;
            ParseDeclSpecifiers(spec);
            node->type_text = Spell(start, i_);
        }
        if (AtOp("{")) {
            Next();
            node->is_definition = true;
            while (!AtEnd() && !AtOp("}")) {
                SkipAttributes(nullptr);
                if (!AtName()) {
                    Next();
                    continue;
                }
                CppNodePtr enumerator = MakeNode(CppNodeKind::Enumerator);
                enumerator->name = Cur().text;
                enumerator->name_pos = Cur().start;
                enumerator->name_end = Cur().end;
                enumerator->start = Cur().start;
                Next();
                SkipAttributes(nullptr);
                if (EatOp("=")) enumerator->kids.push_back(ParseAssignExpression());
                enumerator->end = t_[i_ > 0 ? i_ - 1 : 0].end;
                node->body.push_back(std::move(enumerator));
                if (!EatOp(",")) break;
            }
            if (!EatOp("}")) ErrorHere("expected-brace", "Expected '}' to close this enum");
        }
        node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
        return node;
    }

    /** @brief Parses a declaration-specifier sequence followed by its declarators: the general case. */
    CppNodePtr ParseSimpleDeclaration(bool in_class, const std::string &access,
                                      std::vector<CppNodePtr> attributes) {
        const size_t start_index = i_;
        Specifiers spec;
        ParseDeclSpecifiers(spec);
        for (CppNodePtr &attr : spec.attributes) attributes.push_back(std::move(attr));

        if (AtOp(";")) {
            Next();
            if (spec.embedded) {
                spec.embedded->str_value = access.empty() ? spec.embedded->str_value : access;
                return std::move(spec.embedded);
            }
            if (!spec.have_type) {
                CppNodePtr empty = MakeNode(CppNodeKind::Empty);
                empty->start = t_[start_index].start;
                return empty;
            }
            // An elaborated type specifier on its own: `class Foo;`.
            CppNodePtr node = MakeNode(CppNodeKind::Class);
            node->start = t_[start_index].start;
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            node->name = spec.type ? spec.type->type_base : "";
            node->str_value = "class";
            return node;
        }
        if (!spec.have_type && !spec.is_typedef && !AtName() && !AtOp("~") && !AtOp("*") && !AtOp("&") &&
            !AtKw("operator") && !AtOp("::")) {
            // Nothing here looks like a declaration at all.
            if (i_ == start_index) return nullptr;
        }

        std::vector<CppNodePtr> results;
        for (;;) {
            const size_t declarator_start = i_;
            Declarator d;
            ParseDeclarator(d, false);
            if (!d.valid && d.bindings.empty()) {
                if (i_ == declarator_start) {
                    ErrorHere("syntax-error", "Expected a declarator here");
                    SkipToDeclarationEnd();
                    return nullptr;
                }
            }
            CppNodePtr node;
            if (d.is_function) {
                node = MakeNode(CppNodeKind::Function);
                node->name = d.name.last;
                node->qualified = d.name.spelled;
                node->name_pos = d.name.name_pos;
                node->name_end = d.name.name_end;
                node->params = std::move(d.params);
                node->is_virtual = spec.is_virtual;
                node->is_explicit = spec.is_explicit;
                node->is_static = spec.is_static;
                node->is_inline = spec.is_inline;
                node->is_constexpr = spec.is_constexpr;
                node->is_const = d.is_const_method;
                node->is_override = d.is_override;
                node->is_noexcept = d.is_noexcept;
                node->is_extern = spec.is_extern;
                node->trailing_qualifiers = d.trailing;
                node->str_value = access;
                CppNodePtr type = d.trailing_return ? std::move(d.trailing_return) : std::move(spec.type);
                if (!type) type = MakeNode(CppNodeKind::Type);
                type->ptr_depth = d.ptr_depth;
                type->is_ref = d.is_ref;
                node->kids.push_back(std::move(type));
            } else {
                node = MakeNode(in_class && !spec.is_typedef ? CppNodeKind::Field : CppNodeKind::Variable);
                if (spec.is_typedef) node->kind = CppNodeKind::Typedef;
                node->name = d.name.last;
                node->qualified = d.name.spelled;
                node->name_pos = d.name.name_pos;
                node->name_end = d.name.name_end;
                node->is_static = spec.is_static;
                node->is_extern = spec.is_extern;
                node->is_constexpr = spec.is_constexpr;
                node->is_mutable = spec.is_mutable;
                node->is_inline = spec.is_inline;
                node->str_value = access;
                CppNodePtr type = spec.type ? CloneType(*spec.type) : MakeNode(CppNodeKind::Type);
                type->ptr_depth = d.ptr_depth;
                type->is_ref = d.is_ref;
                type->is_const = spec.is_const;
                if (d.is_array) type->type_text += "[]";
                node->kids.push_back(std::move(type));
            }
            node->start = t_[start_index].start;
            node->is_template = spec.is_template;
            for (CppNodePtr &attr : attributes) {
                if (attr) node->decorators.push_back(CloneNodeShallow(*attr));
            }

            // Initializers, and the four things that can follow a
            // function declarator.
            if (d.has_initializer && d.initializer) {
                node->kids.push_back(std::move(d.initializer));
            } else if (AtOp("=")) {
                Next();
                if (AtKw("delete")) {
                    node->is_deleted = true;
                    Next();
                    if (AtOp("(")) SkipBalanced();
                } else if (AtKw("default")) {
                    node->is_defaulted = true;
                    Next();
                } else if (d.is_function && Cur().kind == CppTokKind::Number && Cur().text == "0") {
                    node->is_pure = true;
                    Next();
                } else {
                    node->kids.push_back(ParseInitializerClause());
                }
            } else if (AtOp("{") && !d.is_function) {
                node->kids.push_back(ParseBracedInitList());
            } else if (AtOp(":") && !d.is_function && in_class) {
                // A bit-field width.
                Next();
                node->kids.push_back(ParseAssignExpression());
            }

            if (d.is_function && (AtOp("{") || AtOp(":") || AtKw("try"))) {
                if (AtOp(":")) ParseMemberInitializers(*node);
                if (AtKw("try")) {
                    Next();
                    if (AtOp(":")) ParseMemberInitializers(*node);
                }
                if (AtOp("{")) {
                    node->is_definition = true;
                    CppNodePtr body = ParseCompoundStatement();
                    if (body) {
                        for (CppNodePtr &stmt : body->body) node->body.push_back(std::move(stmt));
                    }
                    while (AtKw("catch")) {
                        node->handlers.push_back(ParseCatchClause());
                    }
                }
                node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
                results.push_back(std::move(node));
                if (results.size() == 1) return std::move(results.front());
                break;
            }
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            // A structured binding declares several names at once.
            if (!d.bindings.empty()) {
                // `auto [a, b] = f();` declares two names from one
                // initializer. The initializer is attached to the first
                // of them rather than copied: a shallow copy would drop
                // the call inside it, and then `f` would look unused.
                CppNodePtr init = node->kids.size() > 1 ? std::move(node->kids[1]) : nullptr;
                for (CppNodePtr &binding : d.bindings) {
                    binding->kids.push_back(MakeNode(CppNodeKind::Type));
                    if (init) binding->kids.push_back(std::move(init));
                    results.push_back(std::move(binding));
                }
            } else if (!node->name.empty() || node->kind == CppNodeKind::Function) {
                results.push_back(std::move(node));
            }
            if (EatOp(",")) continue;
            break;
        }
        if (!AtOp(";") && !AtEnd() && !AtOp("}") && !AtOp(")")) {
            ErrorHere("expected-semicolon", "Expected ';' after this declaration");
            SkipToDeclarationEnd();
        } else {
            EatOp(";");
        }
        if (spec.embedded && results.empty()) return std::move(spec.embedded);
        if (spec.embedded) {
            results.insert(results.begin(), std::move(spec.embedded));
        }
        if (results.empty()) return nullptr;
        if (results.size() == 1) return std::move(results.front());
        CppNodePtr group = MakeNode(CppNodeKind::DeclStmt);
        group->start = t_[start_index].start;
        group->end = t_[i_ > 0 ? i_ - 1 : 0].end;
        group->kids = std::move(results);
        return group;
    }

    void ParseMemberInitializers(CppNode &function) {
        if (!EatOp(":")) return;
        while (!AtEnd() && !AtOp("{")) {
            CppNodePtr init = MakeNode(CppNodeKind::MemberInit);
            init->start = Cur().start;
            const ParsedName name = ParseQualifiedName();
            init->name = name.last;
            init->name_pos = name.name_pos;
            init->name_end = name.name_end;
            if (AtOp("(")) {
                Next();
                ParseCallArguments(init->kids);
                EatOp(")");
            } else if (AtOp("{")) {
                init->kids.push_back(ParseBracedInitList());
            }
            if (AtOp("...")) Next();
            init->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            if (!name.valid) {
                Next();
            } else {
                function.kids.push_back(std::move(init));
            }
            if (!EatOp(",")) break;
        }
    }

    static CppNodePtr CloneType(const CppNode &type) {
        CppNodePtr copy = MakeNode(CppNodeKind::Type);
        copy->start = type.start;
        copy->end = type.end;
        copy->name = type.name;
        copy->type_text = type.type_text;
        copy->type_base = type.type_base;
        copy->type_args = type.type_args;
        copy->is_const = type.is_const;
        return copy;
    }

    static CppNodePtr CloneNodeShallow(const CppNode &node) {
        CppNodePtr copy = MakeNode(node.kind);
        copy->start = node.start;
        copy->end = node.end;
        copy->name = node.name;
        copy->str_value = node.str_value;
        copy->name_pos = node.name_pos;
        copy->name_end = node.name_end;
        copy->type_text = node.type_text;
        copy->type_base = node.type_base;
        return copy;
    }

    // --- Statements ---------------------------------------------------

    CppNodePtr ParseCompoundStatement() {
        CppNodePtr node = MakeNode(CppNodeKind::Compound);
        node->start = Cur().start;
        if (!EatOp("{")) return node;
        DepthGuard guard(this);
        if (guard.overflow) {
            SkipToDeclarationEnd();
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        while (!AtEnd() && !AtOp("}")) {
            const size_t before = i_;
            CppNodePtr stmt = ParseStatement();
            if (stmt) node->body.push_back(std::move(stmt));
            if (i_ == before) {
                ErrorHere("syntax-error", "Unexpected '" + Cur().text + "' in this block");
                Next();
                SkipToDeclarationEnd();
            }
        }
        if (!EatOp("}")) ErrorHere("expected-brace", "Expected '}' to close this block");
        node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
        return node;
    }

    CppNodePtr ParseStatement() {
        DepthGuard guard(this);
        if (guard.overflow) {
            SkipToDeclarationEnd();
            return nullptr;
        }
        // An attribute may belong to the statement (`[[likely]] if (...)`)
        // or to a declaration (`[[maybe_unused]] int x = 0;`). Only the
        // declaration path can record it, so the decision is made before
        // consuming anything.
        if (AtOp("[") && Peek().kind == CppTokKind::Op && Peek().text == "[") {
            const Snapshot save = Save();
            SkipAttributes(nullptr);
            const bool declaration = IsDeclarationAhead();
            Restore(save);
            if (declaration) return ParseDeclarationStatement();
            SkipAttributes(nullptr);
        }
        const CppPos start = Cur().start;
        if (AtOp("{")) return ParseCompoundStatement();
        if (AtOp(";")) {
            CppNodePtr node = MakeNode(CppNodeKind::Empty);
            node->start = start;
            node->end = Cur().end;
            Next();
            return node;
        }
        if (AtKw("static_assert")) return ParseStaticAssert();
        if (IsStandaloneMacroInvocation()) {
            // A SHOUTED macro with no `;`, inside a function body this
            // time: `_PSTL_PRAGMA_SIMD_REDUCTION(& : __flag)` before a
            // `for`. Same reasoning as the declaration-level case.
            CppNodePtr node = MakeNode(CppNodeKind::Empty);
            node->start = start;
            ConsumeMacroInvocation();
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (AtKw("if")) return ParseIfStatement();
        if (AtKw("while")) {
            CppNodePtr node = MakeNode(CppNodeKind::While);
            node->start = start;
            Next();
            if (EatOp("(")) {
                node->kids.push_back(ParseCondition());
                if (!EatOp(")")) ErrorHere("expected-paren", "Expected ')' to close this condition");
            }
            CppNodePtr body = ParseStatement();
            if (body) node->body.push_back(std::move(body));
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (AtKw("do")) {
            CppNodePtr node = MakeNode(CppNodeKind::DoWhile);
            node->start = start;
            Next();
            CppNodePtr body = ParseStatement();
            if (body) node->body.push_back(std::move(body));
            if (EatKw("while")) {
                if (EatOp("(")) {
                    node->kids.push_back(ParseExpression());
                    if (!EatOp(")")) ErrorHere("expected-paren", "Expected ')' to close this condition");
                }
            }
            EatOp(";");
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (AtKw("for")) return ParseForStatement();
        if (AtKw("switch")) {
            CppNodePtr node = MakeNode(CppNodeKind::Switch);
            node->start = start;
            Next();
            if (EatOp("(")) {
                if (HasTopLevelSemicolon()) {
                    CppNodePtr init = ParseStatement();
                    if (init) node->kids.push_back(std::move(init));
                }
                node->kids.push_back(ParseCondition());
                if (!EatOp(")")) ErrorHere("expected-paren", "Expected ')' to close this condition");
            }
            CppNodePtr body = ParseStatement();
            if (body) node->body.push_back(std::move(body));
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (AtKw("case")) {
            CppNodePtr node = MakeNode(CppNodeKind::Case);
            node->start = start;
            Next();
            node->kids.push_back(ParseAssignExpression());
            if (EatOp("...")) node->kids.push_back(ParseAssignExpression());  // a GNU case range
            EatOp(":");
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (AtKw("default") && Peek().text == ":") {
            CppNodePtr node = MakeNode(CppNodeKind::Default);
            node->start = start;
            Next();
            Next();
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (AtKw("return") || AtKw("co_return")) {
            CppNodePtr node = MakeNode(AtKw("co_return") ? CppNodeKind::CoReturn : CppNodeKind::Return);
            node->start = start;
            Next();
            if (!AtOp(";")) {
                node->kids.push_back(AtOp("{") ? ParseBracedInitList() : ParseExpression());
            }
            if (!EatOp(";")) ErrorHere("expected-semicolon", "Expected ';' after this return");
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (AtKw("break") || AtKw("continue")) {
            CppNodePtr node = MakeNode(AtKw("break") ? CppNodeKind::Break : CppNodeKind::Continue);
            node->start = start;
            const std::string what = Cur().text;
            Next();
            if (!EatOp(";")) ErrorHere("expected-semicolon", "Expected ';' after this " + what);
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (AtKw("goto")) {
            CppNodePtr node = MakeNode(CppNodeKind::Goto);
            node->start = start;
            Next();
            if (AtName()) {
                node->name = Cur().text;
                node->name_pos = Cur().start;
                node->name_end = Cur().end;
                Next();
            }
            EatOp(";");
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (AtKw("try")) {
            CppNodePtr node = MakeNode(CppNodeKind::Try);
            node->start = start;
            Next();
            CppNodePtr body = ParseCompoundStatement();
            if (body) node->body.push_back(std::move(body));
            while (AtKw("catch")) node->handlers.push_back(ParseCatchClause());
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (AtKw("asm") || AtKw("__asm__") || AtKw("__asm")) {
            CppNodePtr node = MakeNode(CppNodeKind::AsmStmt);
            node->start = start;
            Next();
            if (AtKw("volatile") || AtKw("__volatile__")) Next();
            if (AtOp("(")) SkipBalanced();
            EatOp(";");
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        // A label: `name:` but not `name::`.
        if (AtName() && Peek().kind == CppTokKind::Op && Peek().text == ":") {
            CppNodePtr node = MakeNode(CppNodeKind::Label);
            node->start = start;
            node->name = Cur().text;
            node->name_pos = Cur().start;
            node->name_end = Cur().end;
            Next();
            Next();
            CppNodePtr inner = AtOp("}") ? nullptr : ParseStatement();
            if (inner) node->body.push_back(std::move(inner));
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (IsDeclarationAhead()) {
            CppNodePtr declaration = ParseDeclarationStatement();
            if (declaration) return declaration;
        }
        CppNodePtr node = MakeNode(CppNodeKind::ExprStmt);
        node->start = start;
        const size_t saved_statement_start = statement_start_;
        statement_start_ = i_;
        node->kids.push_back(ParseExpression());
        statement_start_ = saved_statement_start;
        if (AtOp("{") && !node->kids.empty() &&
            (node->kids.front()->kind == CppNodeKind::Call || node->kids.front()->kind == CppNodeKind::Id)) {
            // A macro nobody defined, invoked with a block after it:
            // `__catch(...) { }`, `TEST_F(Fixture, name) { }`,
            // `foreach (x, xs) { }`. The block is a real block, and
            // reporting a missing `;` here would be reporting the macro.
            CppNodePtr body = ParseCompoundStatement();
            if (body) node->body.push_back(std::move(body));
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (!AtOp(";") && !AtOp("}") && !AtEnd()) {
            ErrorHere("expected-semicolon", "Expected ';' after this expression");
            SkipToDeclarationEnd();
        } else {
            EatOp(";");
        }
        node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
        return node;
    }

    /**
     * @brief Parses a statement that is a declaration, falling back to an expression when it does not parse.
     *
     * `a && b;` and `x * y;` are declarations by the grammar and
     * expressions by intent, and which one they are depends on a symbol
     * table this parser does not have. So: try the declaration, and if
     * it does not parse cleanly, rewind -- errors included -- and let
     * the caller read it as an expression instead.
     */
    CppNodePtr ParseDeclarationStatement() {
        const Snapshot save = Save();
        CppNodePtr decl = ParseDeclaration(false, "");
        if (decl && !ErrorsSince(save)) {
            if (decl->kind == CppNodeKind::DeclStmt) return decl;
            CppNodePtr node = MakeNode(CppNodeKind::DeclStmt);
            node->start = decl->start;
            node->end = decl->end;
            node->kids.push_back(std::move(decl));
            return node;
        }
        Restore(save);
        return nullptr;
    }

    CppNodePtr ParseCatchClause() {
        CppNodePtr node = MakeNode(CppNodeKind::Catch);
        node->start = Cur().start;
        Next();
        if (EatOp("(")) {
            if (AtOp("...")) {
                Next();
            } else {
                const size_t start = i_;
                Specifiers spec;
                ParseDeclSpecifiers(spec, true);
                Declarator d;
                ParseDeclarator(d, true);
                CppNodePtr param = MakeNode(CppNodeKind::Param);
                param->start = t_[start].start;
                param->name = d.name.last;
                param->name_pos = d.name.name_pos;
                param->name_end = d.name.name_end;
                CppNodePtr type = spec.type ? std::move(spec.type) : MakeNode(CppNodeKind::Type);
                type->ptr_depth = d.ptr_depth;
                type->is_ref = d.is_ref;
                param->kids.push_back(std::move(type));
                param->end = t_[i_ > 0 ? i_ - 1 : 0].end;
                node->kids.push_back(std::move(param));
            }
            if (!EatOp(")")) ErrorHere("expected-paren", "Expected ')' to close this catch clause");
        }
        CppNodePtr body = ParseCompoundStatement();
        if (body) node->body.push_back(std::move(body));
        node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
        return node;
    }

    CppNodePtr ParseIfStatement() {
        CppNodePtr node = MakeNode(CppNodeKind::If);
        node->start = Cur().start;
        Next();
        if (AtKw("constexpr") || AtIdent("consteval") || AtKw("consteval")) {
            node->is_constexpr = true;
            Next();
        }
        // `if _GLIBCXX17_CONSTEXPR (cond)`: a macro standing in for
        // `constexpr`. Only the name is consumed -- the parentheses after
        // it are the condition, not the macro's arguments.
        while (Cur().kind == CppTokKind::Ident && IsMacroShapedName(Cur().text) && Peek().text == "(") Next();
        if (EatOp("(")) {
            // An init-statement, then the condition: `if (auto it =
            // m.find(k); it != m.end())`. Decided by looking for the `;`
            // first -- speculatively parsing an init-statement that is
            // not there turns `if (a && b)` into a declaration of `b`.
            if (HasTopLevelSemicolon()) {
                CppNodePtr init = ParseStatement();
                node->kids.push_back(init ? std::move(init) : MakeNode(CppNodeKind::Placeholder));
            } else {
                node->kids.push_back(MakeNode(CppNodeKind::Placeholder));
            }
            node->kids.push_back(ParseCondition());
            if (!EatOp(")")) ErrorHere("expected-paren", "Expected ')' to close this condition");
        }
        CppNodePtr body = ParseStatement();
        if (body) node->body.push_back(std::move(body));
        if (EatKw("else")) {
            CppNodePtr orelse = ParseStatement();
            if (orelse) node->orelse.push_back(std::move(orelse));
        }
        node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
        return node;
    }

    /** @brief Parses a condition, which may declare a variable (`while (Node *n = next())`). */
    CppNodePtr ParseCondition() {
        // [stmt.select]: a condition that declares something always
        // initializes it (`if (Node *n = next())`). Requiring the `=`
        // before committing is what keeps `a && b` -- which reads as a
        // declaration of an rvalue reference -- an expression.
        if (IsDeclarationAhead() && ConditionHasInitializer()) {
            const Snapshot save = Save();
            CppNodePtr decl = ParseSimpleDeclaration(false, "", {});
            if (decl && !ErrorsSince(save)) return decl;
            Restore(save);
        }
        return ParseExpression();
    }

    /** @brief Reports whether a `;` appears before the `)` that closes the bracket we are inside. */
    bool HasTopLevelSemicolon() const {
        int nest = 0;
        for (size_t k = i_; k < t_.size() && At(k).kind != CppTokKind::End; k++) {
            const CppToken &tok = At(k);
            if (tok.kind != CppTokKind::Op) continue;
            if (tok.text == "(" || tok.text == "[" || tok.text == "{") nest++;
            if (tok.text == ")" || tok.text == "]" || tok.text == "}") {
                if (nest == 0) return false;
                nest--;
            }
            if (nest == 0 && tok.text == ";") return true;
        }
        return false;
    }

    /** @brief Reports whether the condition starting here contains a top-level `=` before its closing `)`. */
    bool ConditionHasInitializer() const {
        int nest = 0;
        for (size_t k = i_; k < t_.size() && At(k).kind != CppTokKind::End; k++) {
            const CppToken &tok = At(k);
            if (tok.kind != CppTokKind::Op) continue;
            // Checked before the nesting is updated: the `{` of
            // `if (unique_lock l{m, try_to_lock})` is itself the
            // initializer this is looking for.
            if (nest == 0 && (tok.text == "=" || tok.text == "{")) return true;
            if (tok.text == "(" || tok.text == "[" || tok.text == "{") nest++;
            if (tok.text == ")" || tok.text == "]" || tok.text == "}") {
                if (nest == 0) return false;
                nest--;
            }
            if (nest == 0 && tok.text == ";") return false;
        }
        return false;
    }

    CppNodePtr ParseForStatement() {
        const CppPos start = Cur().start;
        Next();
        if (AtKw("constexpr")) Next();
        CppNodePtr node = MakeNode(CppNodeKind::For);
        node->start = start;
        if (!EatOp("(")) {
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        // Range-for is told apart by a `:` at bracket depth 0 before the
        // first `;` -- which is also how a human reads it.
        bool range_for = false;
        {
            int nest = 0;
            int question = 0;
            for (size_t k = i_; k < t_.size() && t_[k].kind != CppTokKind::End; k++) {
                const CppToken &tok = t_[k];
                if (tok.kind != CppTokKind::Op) continue;
                if (tok.text == "(" || tok.text == "[" || tok.text == "{") nest++;
                if (tok.text == ")" || tok.text == "]" || tok.text == "}") {
                    if (nest == 0) break;
                    nest--;
                }
                if (nest == 0 && tok.text == ";") break;
                if (nest == 0 && tok.text == "?") {
                    question++;
                    continue;
                }
                if (nest == 0 && tok.text == ":") {
                    // The `:` of a conditional expression in the init
                    // statement (`for (size_t i = n ? 1 : 0; ...)`) is
                    // not the `:` of a range-based for.
                    if (question > 0) {
                        question--;
                        continue;
                    }
                    range_for = true;
                    break;
                }
            }
        }
        if (range_for) {
            node->kind = CppNodeKind::RangeFor;
            node->kids.push_back(MakeNode(CppNodeKind::Placeholder));  // no init-statement
            CppNodePtr decl = ParseSimpleDeclarationNoSemicolon();
            node->kids.push_back(decl ? std::move(decl) : MakeNode(CppNodeKind::Placeholder));
            if (!EatOp(":")) ErrorHere("expected-colon", "Expected ':' in this range-based for");
            node->kids.push_back(AtOp("{") ? ParseBracedInitList() : ParseExpression());
        } else {
            if (AtOp(";")) {
                node->kids.push_back(MakeNode(CppNodeKind::Placeholder));
                Next();
            } else if (IsDeclarationAhead()) {
                CppNodePtr decl = ParseDeclaration(false, "");
                node->kids.push_back(decl ? std::move(decl) : MakeNode(CppNodeKind::Placeholder));
            } else {
                CppNodePtr init = MakeNode(CppNodeKind::ExprStmt);
                init->start = Cur().start;
                init->kids.push_back(ParseExpression());
                init->end = t_[i_ > 0 ? i_ - 1 : 0].end;
                node->kids.push_back(std::move(init));
                EatOp(";");
            }
            node->kids.push_back(AtOp(";") ? MakeNode(CppNodeKind::Placeholder) : ParseExpression());
            EatOp(";");
            node->kids.push_back(AtOp(")") ? MakeNode(CppNodeKind::Placeholder) : ParseExpression());
        }
        if (!EatOp(")")) ErrorHere("expected-paren", "Expected ')' to close this for");
        CppNodePtr body = ParseStatement();
        if (body) node->body.push_back(std::move(body));
        node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
        return node;
    }

    /** @brief Parses one declaration without its terminator: the loop variable of a range-for. */
    CppNodePtr ParseSimpleDeclarationNoSemicolon() {
        const size_t start_index = i_;
        Specifiers spec;
        ParseDeclSpecifiers(spec);
        Declarator d;
        ParseDeclarator(d, true);
        CppNodePtr node = MakeNode(CppNodeKind::Variable);
        node->start = t_[start_index].start;
        node->name = d.name.last;
        node->name_pos = d.name.name_pos;
        node->name_end = d.name.name_end;
        CppNodePtr type = spec.type ? std::move(spec.type) : MakeNode(CppNodeKind::Type);
        type->ptr_depth = d.ptr_depth;
        type->is_ref = d.is_ref;
        node->kids.push_back(std::move(type));
        node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
        if (!d.bindings.empty()) {
            CppNodePtr group = MakeNode(CppNodeKind::DeclStmt);
            group->start = node->start;
            group->end = node->end;
            for (CppNodePtr &binding : d.bindings) {
                binding->kids.push_back(MakeNode(CppNodeKind::Type));
                group->kids.push_back(std::move(binding));
            }
            return group;
        }
        return node;
    }

    /**
     * @brief The declaration-or-expression decision, made syntactically.
     *
     * See this file's header comment. The rule: a leading declaration-only
     * keyword settles it; otherwise a qualified name followed (after any
     * `*`, `&` or cv-qualifier) by another name is a declaration, and
     * anything else is an expression.
     */
    bool IsDeclarationAhead() const {
        const CppToken &tok = Cur();
        if (tok.kind == CppTokKind::Keyword) {
            if (tok.text == "decltype" || tok.text == "__typeof__" || tok.text == "__decltype") {
                size_t k = i_ + 1;
                if (At(k).text == "(") {
                    int nest = 0;
                    while (k < t_.size() && At(k).kind != CppTokKind::End) {
                        if (At(k).text == "(") nest++;
                        if (At(k).text == ")") {
                            nest--;
                            if (nest == 0) {
                                k++;
                                break;
                            }
                        }
                        k++;
                    }
                }
                return At(k).kind == CppTokKind::Ident || At(k).text == "*" || At(k).text == "&";
            }
            if (tok.text == "operator") return false;
            return DeclStartKeywords().count(tok.text) != 0;
        }
        if (tok.kind != CppTokKind::Ident && tok.text != "::") return false;
        size_t k = ScanQualifiedName(i_);
        if (k == i_) return false;
        bool saw_pointer = false;
        while (At(k).kind == CppTokKind::Op || At(k).kind == CppTokKind::Keyword) {
            const std::string &text = At(k).text;
            if (text == "*" || text == "&" || text == "&&") {
                saw_pointer = true;
                k++;
                continue;
            }
            if (text == "const" || text == "volatile" || text == "__restrict" || text == "__restrict__") {
                k++;
                continue;
            }
            break;
        }
        if (At(k).kind == CppTokKind::Ident) {
            // `Foo bar`, `Foo *bar`, `ns::Foo<int> bar`: a declaration.
            // Except when what follows makes it a call through a
            // pointer-to-function stored in a variable, which needs an
            // operator between the two names -- and there is none here.
            return true;
        }
        if (saw_pointer && At(k).text == "(") {
            // `Foo *(bar)` -- a declaration with a parenthesised
            // declarator. `a * (b)` is a multiplication, but it is also
            // exactly the case the standard resolves as a declaration.
            return At(k + 1).kind == CppTokKind::Ident && At(k + 2).text == ")";
        }
        return false;
    }

    // --- Expressions --------------------------------------------------

    static int BinaryPrecedence(const std::string &op) {
        if (op == "||") return 4;
        if (op == "&&") return 5;
        if (op == "|") return 6;
        if (op == "^") return 7;
        if (op == "&") return 8;
        if (op == "==" || op == "!=") return 9;
        if (op == "<=>") return 10;
        if (op == "<" || op == ">" || op == "<=" || op == ">=") return 11;
        if (op == "<<" || op == ">>") return 12;
        if (op == "+" || op == "-") return 13;
        if (op == "*" || op == "/" || op == "%") return 14;
        if (op == ".*" || op == "->*") return 15;
        return 0;
    }

    static bool IsAssignmentOp(const std::string &op) {
        return op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=" || op == "%=" || op == "&=" ||
               op == "|=" || op == "^=" || op == "<<=" || op == ">>=";
    }

    CppNodePtr ParseExpression() {
        CppNodePtr left = ParseAssignExpression();
        while (AtOp(",")) {
            CppNodePtr node = MakeNode(CppNodeKind::Binary);
            node->name = ",";
            node->start = left->start;
            Next();
            CppNodePtr right = ParseAssignExpression();
            node->end = right->end;
            node->kids.push_back(std::move(left));
            node->kids.push_back(std::move(right));
            left = std::move(node);
        }
        return left;
    }

    CppNodePtr ParseAssignExpression() {
        DepthGuard guard(this);
        if (guard.overflow) return MakeNode(CppNodeKind::Placeholder);
        if (AtKw("throw")) {
            CppNodePtr node = MakeNode(CppNodeKind::Throw);
            node->start = Cur().start;
            Next();
            if (!AtOp(")") && !AtOp(";") && !AtOp(",") && !AtEnd()) node->kids.push_back(ParseAssignExpression());
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (AtKw("co_yield")) {
            CppNodePtr node = MakeNode(CppNodeKind::Unary);
            node->name = "co_yield";
            node->str_value = "prefix";
            node->start = Cur().start;
            Next();
            node->kids.push_back(ParseAssignExpression());
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        CppNodePtr left = ParseConditionalExpression();
        if (Cur().kind == CppTokKind::Op && IsAssignmentOp(Cur().text)) {
            CppNodePtr node = MakeNode(CppNodeKind::Binary);
            node->name = Cur().text;
            node->start = left->start;
            node->name_pos = Cur().start;
            node->name_end = Cur().end;
            Next();
            CppNodePtr right = AtOp("{") ? ParseBracedInitList() : ParseAssignExpression();
            node->end = right->end;
            node->kids.push_back(std::move(left));
            node->kids.push_back(std::move(right));
            return node;
        }
        return left;
    }

    CppNodePtr ParseConditionalExpression() {
        CppNodePtr cond = ParseBinaryExpression(4);
        if (!AtOp("?")) return cond;
        CppNodePtr node = MakeNode(CppNodeKind::Conditional);
        node->start = cond->start;
        Next();
        CppNodePtr then_expr = AtOp(":") ? MakeNode(CppNodeKind::Placeholder) : ParseExpression();
        if (!EatOp(":")) ErrorHere("expected-colon", "Expected ':' in this conditional expression");
        CppNodePtr else_expr = ParseAssignExpression();
        node->end = else_expr->end;
        node->kids.push_back(std::move(cond));
        node->kids.push_back(std::move(then_expr));
        node->kids.push_back(std::move(else_expr));
        return node;
    }

    CppNodePtr ParseBinaryExpression(int min_precedence) {
        DepthGuard guard(this);
        if (guard.overflow) return MakeNode(CppNodeKind::Placeholder);
        CppNodePtr left = ParseUnaryExpression();
        for (;;) {
            if (Cur().kind != CppTokKind::Op) break;
            const std::string op = Cur().text;
            if (no_greater_ > 0 && (op == ">" || op == ">>" || op == ">=" || op == ">>=")) break;
            const int precedence = BinaryPrecedence(op);
            if (precedence == 0 || precedence < min_precedence) break;
            const CppPos op_start = Cur().start;
            const CppPos op_end = Cur().end;
            Next();
            // A fold expression: `(args + ...)`.
            if (AtOp("...")) {
                Next();
                CppNodePtr fold = MakeNode(CppNodeKind::Fold);
                fold->name = op;
                fold->start = left->start;
                fold->end = t_[i_ > 0 ? i_ - 1 : 0].end;
                fold->kids.push_back(std::move(left));
                left = std::move(fold);
                continue;
            }
            CppNodePtr right = ParseBinaryExpression(precedence + 1);
            CppNodePtr node = MakeNode(CppNodeKind::Binary);
            node->name = op;
            node->name_pos = op_start;
            node->name_end = op_end;
            node->start = left->start;
            node->end = right->end;
            node->kids.push_back(std::move(left));
            node->kids.push_back(std::move(right));
            left = std::move(node);
        }
        return left;
    }

    /** @brief Reports whether the `(` at `i_` opens a C-style cast rather than a parenthesised expression. */
    bool LooksLikeCast() const {
        size_t k = i_ + 1;
        if (At(k).kind == CppTokKind::Keyword && IsCppFundamentalType(At(k).text)) {
            // `(int)x`, `(unsigned long)n`: settled by the first token.
        } else if (At(k).kind == CppTokKind::Keyword &&
                   (At(k).text == "const" || At(k).text == "struct" || At(k).text == "typename")) {
            // `(const char *)p`
        } else if (At(k).kind == CppTokKind::Ident || At(k).text == "::") {
            const size_t after = ScanQualifiedName(k);
            if (after == k) return false;
            // Only a `*`, `&` or `)` immediately after the name can make
            // this a cast; `(a) + b` and `(a)(b)` stay expressions.
            size_t j = after;
            bool saw_ptr = false;
            while (At(j).text == "*" || At(j).text == "&" || At(j).text == "const" || At(j).text == "volatile") {
                saw_ptr = saw_ptr || At(j).text != "const";
                j++;
            }
            if (At(j).text != ")") return false;
            if (!saw_ptr) {
                // `(_Tp) 1.5` and `(size_t) 'a'`: a literal cannot follow
                // a parenthesised expression, so this is a cast. Anything
                // less certain -- `(a) (b)`, `(a) - b` -- is left as an
                // expression, where being wrong costs nothing.
                const CppToken &next = At(j + 1);
                const bool literal = next.kind == CppTokKind::Number || next.kind == CppTokKind::String ||
                                     next.kind == CppTokKind::Char ||
                                     (next.kind == CppTokKind::Keyword &&
                                      (next.text == "true" || next.text == "false" ||
                                       next.text == "nullptr" || next.text == "this"));
                // `~` and `!` cannot follow a parenthesised expression
                // either, so `(_Tp)~__x` is as clear as `(_Tp)1`.
                const bool only_unary = next.kind == CppTokKind::Op && (next.text == "~" || next.text == "!");
                // `(uintmax_t)__den`: an identifier cannot follow a
                // parenthesised expression either, so this is a cast too.
                const bool named = next.kind == CppTokKind::Ident;
                if (!literal && !only_unary && !named) return false;
            }
            k = j;
        } else {
            return false;
        }
        // Walk to the closing paren, then look at what follows: a cast is
        // followed by something an expression can start with.
        int nest = 0;
        while (k < t_.size() && At(k).kind != CppTokKind::End) {
            if (At(k).text == "(") nest++;
            if (At(k).text == ")") {
                if (nest == 0) break;
                nest--;
            }
            k++;
        }
        const CppToken &after = At(k + 1);
        if (after.kind == CppTokKind::Ident || after.kind == CppTokKind::Number ||
            after.kind == CppTokKind::String || after.kind == CppTokKind::Char) {
            return true;
        }
        if (after.kind == CppTokKind::Keyword) {
            return after.text == "this" || after.text == "true" || after.text == "false" ||
                   after.text == "nullptr" || after.text == "sizeof" || after.text == "new" ||
                   after.text == "static_cast" || after.text == "reinterpret_cast" || after.text == "const_cast";
        }
        if (after.kind == CppTokKind::Op) {
            return after.text == "(" || after.text == "-" || after.text == "*" || after.text == "&" ||
                   after.text == "!" || after.text == "~" || after.text == "+" || after.text == "{" ||
                   after.text == "++" || after.text == "--";
        }
        return false;
    }

    CppNodePtr ParseUnaryExpression() {
        DepthGuard guard(this);
        if (guard.overflow) return MakeNode(CppNodeKind::Placeholder);
        const CppPos start = Cur().start;
        if (Cur().kind == CppTokKind::Op &&
            (AtOp("++") || AtOp("--") || AtOp("+") || AtOp("-") || AtOp("!") || AtOp("~") || AtOp("*") ||
             AtOp("&") || AtOp("..."))) {
            CppNodePtr node = MakeNode(CppNodeKind::Unary);
            node->name = Cur().text;
            node->str_value = "prefix";
            node->start = start;
            Next();
            if (node->name == "..." && (AtOp(")") || AtEnd())) {
                node->kind = CppNodeKind::Placeholder;
                node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
                return node;
            }
            node->kids.push_back(ParseUnaryExpression());
            node->end = node->kids.back()->end;
            return node;
        }
        if (AtKw("sizeof") || AtKw("alignof") || AtKw("__alignof__")) {
            CppNodePtr node = MakeNode(CppNodeKind::SizeOf);
            node->name = Cur().text;
            node->start = start;
            Next();
            if (AtOp("...")) Next();  // `sizeof...(pack)`
            if (AtOp("(")) {
                const Snapshot save = Save();
                Next();
                CppNodePtr type = ParseTypeId();
                if (type && AtOp(")") && !ErrorsSince(save)) {
                    Next();
                    node->kids.push_back(std::move(type));
                    node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
                    return node;
                }
                Restore(save);
                node->kids.push_back(ParseUnaryExpression());
            } else {
                node->kids.push_back(ParseUnaryExpression());
            }
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (AtKw("__real__") || AtKw("__imag__") || AtKw("__extension__")) {
            // GNU's complex-part operators, which libstdc++'s <complex>
            // is written in terms of.
            CppNodePtr node = MakeNode(CppNodeKind::Unary);
            node->name = Cur().text;
            node->str_value = "prefix";
            node->start = start;
            Next();
            node->kids.push_back(ParseUnaryExpression());
            node->end = node->kids.back()->end;
            return node;
        }
        if (AtKw("co_await")) {
            CppNodePtr node = MakeNode(CppNodeKind::Unary);
            node->name = "co_await";
            node->str_value = "prefix";
            node->start = start;
            Next();
            node->kids.push_back(ParseUnaryExpression());
            node->end = node->kids.back()->end;
            return node;
        }
        if (AtKw("new") || (AtOp("::") && Peek().kind == CppTokKind::Keyword && Peek().text == "new")) {
            if (AtOp("::")) Next();  // `::new T(...)`: the global allocation function
            return ParseNewExpression();
        }
        if (AtKw("delete") || (AtOp("::") && Peek().kind == CppTokKind::Keyword && Peek().text == "delete")) {
            CppNodePtr node = MakeNode(CppNodeKind::Delete);
            node->start = start;
            if (AtOp("::")) Next();
            Next();
            if (AtOp("[")) {
                Next();
                EatOp("]");
                node->str_value = "[]";
            }
            node->kids.push_back(ParseUnaryExpression());
            node->end = node->kids.back()->end;
            return node;
        }
        if (AtOp("(") && LooksLikeCast()) {
            const Snapshot save = Save();
            Next();
            CppNodePtr type = ParseTypeId();
            if (type && AtOp(")") && !ErrorsSince(save)) {
                Next();
                CppNodePtr node = MakeNode(CppNodeKind::Cast);
                node->name = "c-style";
                node->start = start;
                node->kids.push_back(std::move(type));
                node->kids.push_back(ParseUnaryExpression());
                node->end = node->kids.back()->end;
                return node;
            }
            Restore(save);
        }
        return ParsePostfixExpression();
    }

    CppNodePtr ParseNewExpression() {
        CppNodePtr node = MakeNode(CppNodeKind::New);
        node->start = Cur().start;
        Next();
        if (AtOp("(")) {
            // Placement arguments, or a parenthesised type.
            const Snapshot save = Save();
            Next();
            CppNodePtr maybe_type = ParseTypeId(true);
            // `new (buffer) T(args)`: what follows the parenthesised
            // group decides. A name or a type keyword after it means the
            // group was placement arguments and the type is still ahead.
            const bool type_follows = Peek().kind == CppTokKind::Ident || Peek().text == "::" ||
                                      (Peek().kind == CppTokKind::Keyword && IsCppFundamentalType(Peek().text));
            if (maybe_type && AtOp(")") && !ErrorsSince(save) && !type_follows &&
                !(Peek().text == "(" || Peek().text == "[")) {
                Next();
                node->kids.push_back(std::move(maybe_type));
                node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
                return node;
            }
            Restore(save);
            Next();
            ParseCallArguments(node->kids);
            EatOp(")");
        }
        CppNodePtr type = ParseTypeId(true);
        if (type) node->kids.insert(node->kids.begin(), std::move(type));
        while (AtOp("[")) SkipBalanced();
        if (AtOp("(")) {
            Next();
            ParseCallArguments(node->kids);
            EatOp(")");
        } else if (AtOp("{")) {
            node->kids.push_back(ParseBracedInitList());
        }
        node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
        return node;
    }

    void ParseCallArguments(std::vector<CppNodePtr> &out) {
        while (!AtEnd() && !AtOp(")")) {
            out.push_back(AtOp("{") ? ParseBracedInitList() : ParseAssignExpression());
            EatOp("...");  // a pack expansion: `f(std::forward<_Args>(args)...)`
            if (!EatOp(",")) break;
        }
    }

    CppNodePtr ParseBracedInitList() {
        CppNodePtr node = MakeNode(CppNodeKind::InitList);
        node->start = Cur().start;
        if (!EatOp("{")) return node;
        DepthGuard guard(this);
        if (guard.overflow) {
            node->end = Cur().end;
            return node;
        }
        while (!AtEnd() && !AtOp("}")) {
            if (AtOp(".") && Peek().kind == CppTokKind::Ident) {
                // A designated initializer: `.field = value`.
                CppNodePtr designator = MakeNode(CppNodeKind::Designator);
                designator->start = Cur().start;
                Next();
                designator->name = Cur().text;
                designator->name_pos = Cur().start;
                designator->name_end = Cur().end;
                Next();
                if (EatOp("=")) {
                    designator->kids.push_back(AtOp("{") ? ParseBracedInitList() : ParseAssignExpression());
                } else if (AtOp("{")) {
                    designator->kids.push_back(ParseBracedInitList());
                }
                designator->end = t_[i_ > 0 ? i_ - 1 : 0].end;
                node->kids.push_back(std::move(designator));
            } else {
                node->kids.push_back(AtOp("{") ? ParseBracedInitList() : ParseAssignExpression());
            }
            EatOp("...");  // a pack expansion inside the list
            if (!EatOp(",")) break;
        }
        if (!EatOp("}")) ErrorHere("expected-brace", "Expected '}' to close this initializer list");
        node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
        return node;
    }

    CppNodePtr ParseInitializerClause() {
        if (AtOp("{")) return ParseBracedInitList();
        return ParseAssignExpression();
    }

    CppNodePtr ParsePostfixExpression() {
        const size_t start_index = i_;
        CppNodePtr expr = ParsePrimaryExpression();
        for (;;) {
            if (AtOp("(")) {
                CppNodePtr node = MakeNode(CppNodeKind::Call);
                node->start = expr->start;
                node->name = expr->name;
                node->name_pos = expr->name_pos;
                node->name_end = expr->name_end;
                Next();
                node->kids.push_back(std::move(expr));
                ParseCallArguments(node->kids);
                if (!EatOp(")")) ErrorHere("expected-paren", "Expected ')' to close this call");
                node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
                expr = std::move(node);
                continue;
            }
            if (AtOp("[")) {
                CppNodePtr node = MakeNode(CppNodeKind::Subscript);
                node->start = expr->start;
                Next();
                node->kids.push_back(std::move(expr));
                while (!AtEnd() && !AtOp("]")) {
                    node->kids.push_back(ParseAssignExpression());
                    if (!EatOp(",")) break;
                }
                if (!EatOp("]")) ErrorHere("expected-bracket", "Expected ']' to close this subscript");
                node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
                expr = std::move(node);
                continue;
            }
            if (AtOp(".") || AtOp("->")) {
                CppNodePtr node = MakeNode(CppNodeKind::Member);
                node->str_value = Cur().text;
                node->start = expr->start;
                Next();
                EatKw("template");
                const bool destructor = AtOp("~");
                if (destructor) Next();
                if (AtName() || AtKw("operator")) {
                    if (AtKw("operator")) {
                        node->name = ParseOperatorName();
                        node->name_pos = t_[i_ > 0 ? i_ - 1 : 0].start;
                        node->name_end = t_[i_ > 0 ? i_ - 1 : 0].end;
                    } else {
                        node->name = (destructor ? "~" : "") + Cur().text;
                        node->name_pos = Cur().start;
                        node->name_end = Cur().end;
                        Next();
                        if (AtOp("<")) {
                            bool leftover = false;
                            const size_t close = TemplateArgumentsAhead(i_, &leftover);
                            if (close != 0) {
                                i_ = leftover ? close - 1 : close;
                                if (leftover) EatTemplateClose();
                            }
                        }
                    }
                }
                node->kids.push_back(std::move(expr));
                node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
                expr = std::move(node);
                continue;
            }
            if (AtOp("++") || AtOp("--")) {
                CppNodePtr node = MakeNode(CppNodeKind::Unary);
                node->name = Cur().text;
                node->str_value = "postfix";
                node->start = expr->start;
                node->end = Cur().end;
                Next();
                node->kids.push_back(std::move(expr));
                expr = std::move(node);
                continue;
            }
            // A braced functional cast: `size_t{8}`, `Point{1, 2}`,
            // `std::vector<int>{}`. Only after a name -- everywhere else
            // a `{` in expression position opens a block, and swallowing
            // that would eat the rest of the function.
            if (AtOp("{") && (expr->kind == CppNodeKind::Id || expr->kind == CppNodeKind::TypeExpr) &&
                start_index != statement_start_) {
                CppNodePtr node = MakeNode(CppNodeKind::Call);
                node->start = expr->start;
                node->name = expr->name;
                node->name_pos = expr->name_pos;
                node->name_end = expr->name_end;
                CppNodePtr args = ParseBracedInitList();
                node->kids.push_back(std::move(expr));
                for (CppNodePtr &arg : args->kids) node->kids.push_back(std::move(arg));
                node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
                expr = std::move(node);
                continue;
            }
            // A user-defined literal suffix, or `"a" "b"` concatenation,
            // both of which the tokenizer already kept as separate tokens.
            if (Cur().kind == CppTokKind::String && expr->kind == CppNodeKind::Literal) {
                expr->name += Cur().text;
                expr->end = Cur().end;
                Next();
                continue;
            }
            break;
        }
        return expr;
    }

    /** @brief Reports whether the `[` at `i_` opens a lambda introducer. */
    bool LooksLikeLambda() const {
        if (At(i_ + 1).text == "[") return false;  // an attribute
        int nest = 0;
        size_t k = i_;
        while (k < t_.size() && At(k).kind != CppTokKind::End) {
            if (At(k).text == "[") nest++;
            if (At(k).text == "]") {
                nest--;
                if (nest == 0) {
                    const std::string &after = At(k + 1).text;
                    return after == "(" || after == "{" || after == "<" || after == "->" ||
                           At(k + 1).kind == CppTokKind::Keyword;
                }
            }
            if (At(k).text == ";") return false;
            k++;
        }
        return false;
    }

    CppNodePtr ParseLambda() {
        CppNodePtr node = MakeNode(CppNodeKind::Lambda);
        node->start = Cur().start;
        const size_t capture_start = i_;
        ParseLambdaCaptures(*node);
        node->str_value = Spell(capture_start, i_);
        if (AtOp("<")) {
            no_greater_++;
            int angle = 0;
            do {
                if (AtOp("<")) angle++;
                if (AtOp(">")) angle--;
                Next();
            } while (!AtEnd() && angle > 0);
            no_greater_--;
        }
        if (AtKw("requires")) ParseRequiresClause();
        if (AtOp("(")) {
            Next();
            ParseParameterList(node->params);
            if (!EatOp(")")) ErrorHere("expected-paren", "Expected ')' to close this lambda's parameters");
        }
        for (;;) {
            if (AtKw("mutable") || AtKw("constexpr") || AtKw("consteval") || AtKw("static")) {
                Next();
                continue;
            }
            if (AtKw("noexcept")) {
                Next();
                if (AtOp("(")) SkipBalanced();
                continue;
            }
            if (AtKw("requires")) {
                ParseRequiresClause();
                continue;
            }
            if (SkipAttributes(nullptr)) continue;
            if (SkipMacroInvocation()) continue;
            break;
        }
        if (EatOp("->")) node->kids.push_back(ParseTypeId());
        if (AtOp("{")) {
            CppNodePtr body = ParseCompoundStatement();
            if (body) {
                for (CppNodePtr &stmt : body->body) node->body.push_back(std::move(stmt));
            }
        }
        node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
        return node;
    }

    /**
     * @brief Parses a lambda's capture list into expressions the analysis half can resolve.
     *
     * A capture is a *use* of the name it captures -- `[&entry]` and
     * `[id = entry.second]` both read `entry` in the enclosing scope --
     * and skipping the list wholesale is what made a captured variable
     * look unused. An init-capture also introduces a name of its own,
     * which is recorded as one of the lambda's parameters.
     */
    void ParseLambdaCaptures(CppNode &lambda) {
        if (!EatOp("[")) return;
        while (!AtEnd() && !AtOp("]")) {
            if (AtOp("=") || AtOp("&") || AtOp(",") || AtOp("...")) {
                Next();
                continue;
            }
            if (AtKw("this")) {
                Next();
                continue;
            }
            if (AtOp("*") && Peek().kind == CppTokKind::Keyword && Peek().text == "this") {
                Next();
                Next();
                continue;
            }
            if (!AtName()) {
                Next();
                continue;
            }
            const CppToken name_token = Cur();
            Next();
            if (AtOp("=") || AtOp("{")) {
                // An init-capture: the initializer is evaluated outside
                // the lambda, and the name it introduces lives inside it.
                CppNodePtr param = MakeNode(CppNodeKind::Param);
                param->name = name_token.text;
                param->name_pos = name_token.start;
                param->name_end = name_token.end;
                param->start = name_token.start;
                param->end = name_token.end;
                param->kids.push_back(MakeNode(CppNodeKind::Type));
                lambda.params.push_back(std::move(param));
                if (EatOp("=")) {
                    lambda.kids.push_back(ParseAssignExpression());
                } else {
                    lambda.kids.push_back(ParseBracedInitList());
                }
                continue;
            }
            CppNodePtr captured = MakeNode(CppNodeKind::Id);
            captured->name = name_token.text;
            captured->str_value = name_token.text;
            captured->name_pos = name_token.start;
            captured->name_end = name_token.end;
            captured->start = name_token.start;
            captured->end = name_token.end;
            lambda.kids.push_back(std::move(captured));
        }
        if (!EatOp("]")) ErrorHere("expected-bracket", "Expected ']' to close this lambda's capture list");
    }

    CppNodePtr ParsePrimaryExpression() {
        DepthGuard guard(this);
        if (guard.overflow) return MakeNode(CppNodeKind::Placeholder);
        const CppPos start = Cur().start;
        if (Cur().kind == CppTokKind::Number || Cur().kind == CppTokKind::String ||
            Cur().kind == CppTokKind::Char) {
            CppNodePtr node = MakeNode(CppNodeKind::Literal);
            node->name = Cur().text;
            node->str_value = Cur().kind == CppTokKind::Number
                                  ? "number"
                                  : (Cur().kind == CppTokKind::String ? "string" : "char");
            node->start = start;
            node->end = Cur().end;
            Next();
            return node;
        }
        if (AtKw("true") || AtKw("false") || AtKw("nullptr")) {
            CppNodePtr node = MakeNode(CppNodeKind::Literal);
            node->name = Cur().text;
            node->str_value = Cur().text == "nullptr" ? "nullptr" : "bool";
            node->start = start;
            node->end = Cur().end;
            Next();
            return node;
        }
        if (AtKw("this")) {
            CppNodePtr node = MakeNode(CppNodeKind::This);
            node->start = start;
            node->end = Cur().end;
            Next();
            return node;
        }
        if (AtKw("static_cast") || AtKw("dynamic_cast") || AtKw("const_cast") || AtKw("reinterpret_cast")) {
            CppNodePtr node = MakeNode(CppNodeKind::Cast);
            node->name = Cur().text;
            node->start = start;
            Next();
            if (EatOp("<")) {
                no_greater_++;
                node->kids.push_back(ParseTypeId());
                no_greater_--;
                if (!EatTemplateClose()) ErrorHere("expected-angle", "Expected '>' after this cast's type");
            }
            if (EatOp("(")) {
                node->kids.push_back(ParseExpression());
                if (!EatOp(")")) ErrorHere("expected-paren", "Expected ')' to close this cast");
            }
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (AtKw("typeid") || AtKw("noexcept")) {
            CppNodePtr node = MakeNode(CppNodeKind::SizeOf);
            node->name = Cur().text;
            node->start = start;
            Next();
            if (AtOp("(")) {
                const Snapshot save = Save();
                Next();
                CppNodePtr type = ParseTypeId();
                if (type && AtOp(")") && !ErrorsSince(save)) {
                    Next();
                    node->kids.push_back(std::move(type));
                } else {
                    Restore(save);
                    Next();
                    node->kids.push_back(ParseExpression());
                    EatOp(")");
                }
            }
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (AtKw("decltype") || AtKw("__typeof__") || AtKw("__decltype")) {
            CppNodePtr node = MakeNode(CppNodeKind::SizeOf);
            node->name = "decltype";
            node->start = start;
            Next();
            if (AtOp("(")) SkipBalanced();
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (AtKw("requires")) {
            CppNodePtr node = MakeNode(CppNodeKind::SizeOf);
            node->name = "requires";
            node->start = start;
            ParseRequiresClause(true);
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (AtOp("[") && LooksLikeLambda()) return ParseLambda();
        if (AtOp("{")) return ParseBracedInitList();
        if (AtOp("(")) {
            CppNodePtr node = MakeNode(CppNodeKind::Paren);
            node->start = start;
            Next();
            const int saved_no_greater = no_greater_;
            no_greater_ = 0;  // inside parentheses, `>` is a comparison again
            if (AtOp("...")) {
                // A unary left fold: `(... + args)`.
                Next();
                CppNodePtr fold = MakeNode(CppNodeKind::Fold);
                fold->start = start;
                if (Cur().kind == CppTokKind::Op) {
                    fold->name = Cur().text;
                    Next();
                }
                fold->kids.push_back(ParseExpression());
                no_greater_ = saved_no_greater;
                if (!EatOp(")")) ErrorHere("expected-paren", "Expected ')' to close this fold expression");
                fold->end = t_[i_ > 0 ? i_ - 1 : 0].end;
                return fold;
            }
            node->kids.push_back(AtOp(")") ? MakeNode(CppNodeKind::Placeholder) : ParseExpression());
            no_greater_ = saved_no_greater;
            if (!EatOp(")")) ErrorHere("expected-paren", "Expected ')' to close this expression");
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        if (AtKw("operator")) {
            CppNodePtr node = MakeNode(CppNodeKind::Id);
            node->start = start;
            node->name = ParseOperatorName();
            node->str_value = node->name;
            node->name_pos = start;
            node->name_end = t_[i_ > 0 ? i_ - 1 : 0].end;
            node->end = node->name_end;
            return node;
        }
        if (AtName() || AtOp("::") || AtOp("~") || AtKw("typename")) {
            const size_t after = ScanQualifiedName(i_);
            if (after > i_) {
                const ParsedName name = ParseQualifiedName();
                CppNodePtr node = MakeNode(CppNodeKind::Id);
                node->start = start;
                node->name = name.last;
                node->str_value = name.spelled;
                node->name_pos = name.name_pos;
                node->name_end = name.name_end;
                node->type_args = name.args;
                node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
                return node;
            }
        }
        if (Cur().kind == CppTokKind::Keyword && IsCppFundamentalType(Cur().text)) {
            // A functional cast: `int(x)`, `double{}`.
            CppNodePtr node = MakeNode(CppNodeKind::TypeExpr);
            node->start = start;
            node->name = Cur().text;
            Next();
            node->end = t_[i_ > 0 ? i_ - 1 : 0].end;
            return node;
        }
        // Nothing here starts an expression. The caller reports it: this
        // returns a placeholder so the tree stays well formed.
        CppNodePtr node = MakeNode(CppNodeKind::Placeholder);
        node->start = start;
        node->end = Cur().end;
        return node;
    }

    // --- Types in expression position ---------------------------------

    /** @brief Parses a type-id (a type with no declarator name): `const char *`, `std::vector<int>`. */
    CppNodePtr ParseTypeId(bool new_type_id = false) {
        const size_t start = i_;
        Specifiers spec;
        ParseDeclSpecifiers(spec, true);
        if (!spec.have_type && !spec.embedded) {
            i_ = start;
            return nullptr;
        }
        Declarator d;
        // A new-type-id's declarator cannot contain a parameter list
        // ([expr.new]): in `new _Tp(args)` the parentheses are the
        // constructor's, and letting the declarator take them would eat
        // the arguments.
        ParseDeclarator(d, true, !new_type_id);
        CppNodePtr type = spec.type ? std::move(spec.type) : MakeNode(CppNodeKind::Type);
        type->ptr_depth = d.ptr_depth;
        type->is_ref = d.is_ref;
        type->type_text = Spell(start, i_);
        type->start = t_[start].start;
        type->end = t_[i_ > 0 ? i_ - 1 : 0].end;
        return type;
    }
};

}  // namespace

// --- Public entry points ----------------------------------------------

std::string CppTypeText(const CppNode *type) {
    if (type == nullptr) return "";
    if (!type->type_text.empty()) return type->type_text;
    std::string out = type->type_base;
    for (int k = 0; k < type->ptr_depth; k++) out += " *";
    if (type->is_ref) out += " &";
    return out;
}

std::string CppSignatureText(const CppNode *fn) {
    if (fn == nullptr) return "";
    std::string out = fn->name + "(";
    for (size_t k = 0; k < fn->params.size(); k++) {
        if (k > 0) out += ", ";
        const CppNode *param = fn->params[k].get();
        if (param == nullptr) continue;
        if (param->is_variadic && param->name.empty() && param->kids.empty()) {
            out += "...";
            continue;
        }
        std::string type = param->kids.empty() ? "" : CppTypeText(param->kids.front().get());
        out += type;
        if (!param->name.empty()) {
            if (!type.empty() && type.back() != '*' && type.back() != '&') out += ' ';
            out += param->name;
        }
        if (param->kids.size() > 1) out += " = ...";
    }
    out += ")";
    if (!fn->trailing_qualifiers.empty()) out += " " + fn->trailing_qualifiers;
    return out;
}

CppParseResult ParseCpp(const std::vector<std::string> &lines, const CppParseOptions &opts) {
    CppParseResult result;
    result.raw_tokens = TokenizeCpp(lines, &result.errors, &result.comments);

    CppPreprocessOutput pp;
    CppPreprocess(result.raw_tokens, lines, opts, &result.errors, &pp);
    result.directives = std::move(pp.directives);
    result.includes = std::move(pp.includes);
    result.macros = std::move(pp.macros);
    result.line_active = std::move(pp.line_active);
    result.truncated = pp.truncated;
    result.tokens = std::move(pp.tokens);

    Parser parser(result.tokens, &result.errors);
    result.unit = parser.ParseUnit();
    if (parser.truncated()) result.truncated = true;

    // Brace depth at the start of each line, for folding and for the
    // "which block is this line in" questions the analysis half asks.
    result.depth_at_line.assign(lines.size(), 0);
    int depth = 0;
    size_t line_index = 0;
    for (const CppToken &tok : result.tokens) {
        if (tok.kind == CppTokKind::End) break;
        while (line_index < result.depth_at_line.size() &&
               static_cast<int>(line_index) <= tok.start.line) {
            result.depth_at_line[line_index] = depth;
            line_index++;
        }
        if (tok.kind == CppTokKind::Op && !tok.from_macro) {
            if (tok.text == "{") depth++;
            if (tok.text == "}" && depth > 0) depth--;
        }
    }
    while (line_index < result.depth_at_line.size()) {
        result.depth_at_line[line_index] = depth;
        line_index++;
    }

    std::stable_sort(result.errors.begin(), result.errors.end(),
                     [](const CppSyntaxError &a, const CppSyntaxError &b) { return CppPosLess(a.start, b.start); });
    return result;
}

CppParseResult ParseCpp(const std::vector<std::string> &lines) {
    CppParseOptions opts;
    return ParseCpp(lines, opts);
}
