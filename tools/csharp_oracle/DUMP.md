# csharp-oracle dump format

`csharp-oracle dump FILE...` (or `dump --stdin-list`, file names one per line
on stdin) prints the syntax tree and the diagnostics Roslyn -- the copy of
`Microsoft.CodeAnalysis.CSharp.dll` the .NET SDK ships -- produces for each
file. `mep-csharp-lsp-test --parse FILE...` prints the same text from mep's
C++ port (src/csharp_parser.cpp, `SyntaxTree::Dump`), so the two can be
diffed byte for byte. Build the oracle with `just csharp-oracle`.

The parse is `CSharpSyntaxTree.ParseText(File.ReadAllText(path), new
CSharpParseOptions(LanguageVersion.Preview, DocumentationMode.Parse), path)`:
C# preview, structured doc comments, no preprocessor symbols defined.
`File.ReadAllText` drops a byte-order mark and decodes invalid UTF-8 as
U+FFFD; mep does the same.

Everything is UTF-8, lines end in `\n`. Positions are 0-based offsets in
UTF-16 code units (Roslyn's positions), end-exclusive.

## Per file

```
=== <path as given>
<tree>
D <id> <severity> <start> <end> <message>      -- zero or more
```

`UNREADABLE <ExceptionType>` replaces the tree when the file cannot be read.

## Tree lines

Preorder, two spaces of indentation per depth. A node:

```
N <SyntaxKind> <span start> <span end>[ missing]
```

then its children, `ChildNodesAndTokens()` (lists flattened, absent fields
skipped), at depth + 1. A token:

```
L <TriviaKind> <start> <end>        -- each leading trivia
T <SyntaxKind> <span start> <span end>[ missing]
R <TriviaKind> <start> <end>        -- each trailing trivia
```

all at the token's depth. The span of a node or token excludes its leading
and trailing trivia (Roslyn's `Span`); `missing` marks `IsMissing` (a missing
token has an empty span). A trivia's span is Roslyn's `SyntaxTrivia.Span`;
for a structured trivia (a directive, skipped tokens, a doc comment) the
structure's node follows at depth + 1, dumped the same way.

## Diagnostics

`tree.GetDiagnostics()`, in Roslyn's order (the order they sit in the tree):
the id (`CS1002`), `error` / `warning` / `info` (hidden counts as info),
the span, and the message (`GetMessage(InvariantCulture)`) with `\`, newline,
CR, tab and other control characters escaped as `\\`, `\n`, `\r`, `\t`,
`\xHH`.
