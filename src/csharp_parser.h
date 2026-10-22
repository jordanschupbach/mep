#ifndef MEP_CSHARP_PARSER_H
#define MEP_CSHARP_PARSER_H

// Parsing C#: mep's translation of Roslyn's lexer and parser (see
// csharp_syntax.h) behind one call, and a red layer over the green tree --
// parents, positions, tokens and trivia -- for the language server and the
// formatter.
//
//   auto tree = csharp::SyntaxTree::Parse(utf8_text);
//   for (const csharp::Diagnostic &d : tree->Diagnostics()) ...
//   const csharp::SyntaxNode *root = tree->Root();
//
// The tree is exactly the one Roslyn's CSharpSyntaxTree.ParseText builds
// (C# preview, DocumentationMode.Parse, no preprocessor symbols): the same
// node kinds, spans, missing tokens, skipped-token trivia and syntax errors.
// Positions are UTF-16 offsets, as Roslyn's; Utf8Offset / Utf16Offset and
// LineCol translate for callers working in UTF-8.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "csharp_syntax.h"
#include "csharp_syntax_factory.h"

namespace csharp {

struct TextSpan {
    int start = 0;
    int end = 0;
    int Length() const { return end - start; }
    bool Contains(int pos) const { return pos >= start && pos < end; }
    bool IntersectsOrTouches(int pos) const { return pos >= start && pos <= end; }
};

/** @brief One syntax diagnostic, as Roslyn's SyntaxTree.GetDiagnostics reports it. */
struct Diagnostic {
    ErrorCode code = ErrorCode::Void;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    TextSpan span;        // UTF-16 offsets
    std::string message;  // UTF-8, arguments substituted
    std::string Id() const { return ErrorId(code); }
};

struct SyntaxNode;

/** @brief A trivia with its position; `structure` is set for structured trivia (directives, skipped tokens,
 *         doc comments), whose nodes are part of the red tree. */
struct SyntaxTrivia {
    const GreenNode *green = nullptr;
    int position = 0;
    const SyntaxNode *token = nullptr;  // the token it belongs to
    const SyntaxNode *structure = nullptr;
    SyntaxKind Kind() const { return green->kind; }
    TextSpan Span() const { return {position, position + green->full_width}; }
};

/**
 * @brief A node or a token of the red tree (Roslyn's SyntaxNode / SyntaxToken in one type). Children are
 *        ChildNodesAndTokens(): lists are flattened, absent fields skipped. A token has no children but trivia.
 */
struct SyntaxNode {
    const GreenNode *green = nullptr;
    const SyntaxNode *parent = nullptr;  // for a structured trivia's root: nullptr (see parent_trivia)
    const SyntaxTrivia *parent_trivia = nullptr;
    int position = 0;  // full start
    int index = 0;     // in parent's children
    std::vector<const SyntaxNode *> children;
    std::vector<SyntaxTrivia> leading;  // tokens only
    std::vector<SyntaxTrivia> trailing;

    SyntaxKind Kind() const { return green->kind; }
    bool IsToken() const { return green->IsToken(); }
    bool IsMissing() const { return green->IsMissing(); }
    TextSpan FullSpan() const { return {position, position + green->full_width}; }
    /** @brief The span without leading trivia of the first token and trailing trivia of the last. */
    TextSpan Span() const;
    /** @brief A token's text (empty for missing tokens and nodes). */
    Text TokenText() const { return green->text; }
    Text ValueText() const { return green->value_text; }
    SyntaxKind ContextualKind() const { return green->contextual_kind; }
    /** @brief The child holding field `f` of this node's kind (nullptr when absent); lists come back as the
     *         first child of the list -- use FieldItems for those. */
    const SyntaxNode *Field(csharp::Field f) const;
    /** @brief The children that make up list field `f` (separators included for separated lists). */
    std::vector<const SyntaxNode *> FieldItems(csharp::Field f) const;
    /** @brief The first / last token at or below this node (missing ones included). */
    const SyntaxNode *FirstToken() const;
    const SyntaxNode *LastToken() const;
    /** @brief The previous / next token in the tree (not into structured trivia), skipping missing ones. */
    const SyntaxNode *PreviousToken() const;
    const SyntaxNode *NextToken() const;
};

class SyntaxTree {
public:
    /** @brief Parses UTF-8 source (a leading BOM dropped, invalid bytes decoded as U+FFFD, as .NET's
     *         File.ReadAllText does). Never fails: errors are in Diagnostics(). */
    static std::unique_ptr<SyntaxTree> Parse(std::string_view utf8);
    static std::unique_ptr<SyntaxTree> ParseUtf16(std::u16string text);
    ~SyntaxTree();

    const std::u16string &Text16() const { return text_; }
    const GreenNode *GreenRoot() const { return green_root_; }
    const SyntaxNode *Root() const { return root_; }
    /** @brief Every diagnostic in the tree, in Roslyn's order (tree order). */
    const std::vector<Diagnostic> &Diagnostics() const { return diagnostics_; }

    /** @brief The token whose full span holds `pos` (the EOF token at the end); like Roslyn's FindToken. */
    const SyntaxNode *FindToken(int pos) const;
    /** @brief The token touching `pos` from the left when `pos` is just past it, else FindToken. */
    const SyntaxNode *FindTokenOnLeft(int pos) const;

    // Offsets: UTF-16 (Roslyn) <-> UTF-8 bytes of the text as given to Parse.
    int Utf8Offset(int utf16) const;
    int Utf16Offset(int utf8) const;
    /** @brief 0-based line and UTF-8 byte column of a UTF-16 offset (lines end at \n, \r\n or \r, as LSP counts). */
    void LineCol(int utf16, int *line, int *col) const;
    /** @brief The UTF-16 offset of a 0-based line and UTF-8 byte column. */
    int OffsetOf(int line, int col) const;
    std::string Utf8(TextSpan span) const;
    std::string Utf8(const SyntaxNode *node) const { return Utf8(node->Span()); }

    /** @brief The tree in tools/csharp_oracle/DUMP.md's format (what `csharp-oracle dump` prints). */
    std::string Dump(const std::string &path) const;

private:
    SyntaxTree() = default;
    void Build();
    const SyntaxNode *BuildNode(const GreenNode *g, const SyntaxNode *parent, const SyntaxTrivia *ptrivia, int pos,
                                int index);

    std::u16string text_;
    std::unique_ptr<Arena> arena_;
    const GreenNode *green_root_ = nullptr;
    const SyntaxNode *root_ = nullptr;
    std::vector<std::unique_ptr<SyntaxNode>> nodes_;
    std::vector<const SyntaxNode *> tokens_;  // every token of the main tree, in order
    std::vector<Diagnostic> diagnostics_;
    std::vector<int> utf8_of_utf16_;  // utf16 offset -> utf8 offset (size text_.size()+1)
    std::vector<int> line_starts_;    // utf16 offsets
};

}  // namespace csharp

#endif  // MEP_CSHARP_PARSER_H
