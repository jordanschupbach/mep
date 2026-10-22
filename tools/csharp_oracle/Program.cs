// mep's C# oracle: Roslyn's parser (the copy the .NET SDK ships).
//
//   csharp-oracle check FILE...      exit 1 if any file has a syntax error;
//                                    prints FILE:LINE:COL: CODE message
//   csharp-oracle check --stdin-list same, file names on stdin; prints
//                                    "ok FILE" or "error FILE ..." per file
//   csharp-oracle dump FILE...       the syntax tree and every diagnostic of
//   csharp-oracle dump --stdin-list  each file, in the text format of
//                                    tools/csharp_oracle/DUMP.md (the format
//                                    mep-csharp-lsp-test --parse prints, so
//                                    the two can be diffed)
//   csharp-oracle compile REF_DIR FILE...  compile the files together against
//                                    the reference assemblies; print the
//                                    compiler's diagnostics (lint calibration)
//   csharp-oracle unicode-categories .NET's char.GetUnicodeCategory as ranges
//                                    (input to tools/gen_csharp_syntax.py)
//   csharp-oracle format FILE        the file formatted by Roslyn's formatting
//                                    engine with default options, on stdout --
//                                    what `dotnet format whitespace --folder`
//                                    writes back with no .editorconfig
//   csharp-oracle format --out DIR FILE... | --out DIR --stdin-list
//                                    each result written to DIR/<the file's
//                                    absolute path>; "ok FILE" per file
//
// Parsed as C# preview with no preprocessor symbols defined, the way an
// editor without a project sees a file.
using System.Globalization;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Formatting;
using Microsoft.CodeAnalysis.Text;

static class Oracle {
    static readonly CSharpParseOptions Options = new(LanguageVersion.Preview, DocumentationMode.Parse);

    static IEnumerable<Diagnostic> Errors(string path) {
        var text = File.ReadAllText(path);
        var tree = CSharpSyntaxTree.ParseText(text, Options, path);
        return tree.GetDiagnostics().Where(d => d.Severity == DiagnosticSeverity.Error);
    }

    static string Describe(Diagnostic d) {
        var p = d.Location.GetLineSpan().StartLinePosition;
        return $"{p.Line + 1}:{p.Character + 1}: {d.Id} {d.GetMessage()}";
    }

    // --- dump -------------------------------------------------------------

    static string Escape(string s) {
        var sb = new StringBuilder();
        foreach (var c in s) {
            if (c == '\\') sb.Append("\\\\");
            else if (c == '\n') sb.Append("\\n");
            else if (c == '\r') sb.Append("\\r");
            else if (c == '\t') sb.Append("\\t");
            else if (c < 0x20 || c == 0x7f) sb.Append($"\\x{(int)c:x2}");
            else sb.Append(c);
        }
        return sb.ToString();
    }

    static void Indent(StringBuilder o, int depth) => o.Append(' ', depth * 2);

    static void DumpTrivia(StringBuilder o, SyntaxTrivia t, char side, int depth) {
        Indent(o, depth);
        o.Append(side).Append(' ').Append(t.Kind()).Append(' ')
         .Append(t.Span.Start).Append(' ').Append(t.Span.End).Append('\n');
        if (t.HasStructure) DumpNode(o, t.GetStructure()!, depth + 1);
    }

    static void DumpToken(StringBuilder o, SyntaxToken t, int depth) {
        foreach (var tr in t.LeadingTrivia) DumpTrivia(o, tr, 'L', depth);
        Indent(o, depth);
        o.Append("T ").Append(t.Kind()).Append(' ').Append(t.Span.Start).Append(' ').Append(t.Span.End);
        if (t.IsMissing) o.Append(" missing");
        o.Append('\n');
        foreach (var tr in t.TrailingTrivia) DumpTrivia(o, tr, 'R', depth);
    }

    static void DumpNode(StringBuilder o, SyntaxNode n, int depth) {
        Indent(o, depth);
        o.Append("N ").Append(n.Kind()).Append(' ').Append(n.Span.Start).Append(' ').Append(n.Span.End);
        if (n.IsMissing) o.Append(" missing");
        o.Append('\n');
        foreach (var c in n.ChildNodesAndTokens()) {
            if (c.IsToken) DumpToken(o, c.AsToken(), depth + 1);
            else DumpNode(o, c.AsNode()!, depth + 1);
        }
    }

    static string Dump(string path) {
        var o = new StringBuilder();
        o.Append("=== ").Append(path).Append('\n');
        string text;
        try {
            text = File.ReadAllText(path);
        } catch (Exception e) {
            o.Append("UNREADABLE ").Append(e.GetType().Name).Append('\n');
            return o.ToString();
        }
        var tree = CSharpSyntaxTree.ParseText(text, Options, path);
        var root = tree.GetRoot();
        DumpNode(o, root, 0);
        foreach (var d in tree.GetDiagnostics()) {
            var span = d.Location.SourceSpan;
            o.Append("D ").Append(d.Id).Append(' ')
             .Append(d.Severity == DiagnosticSeverity.Error ? "error" : d.Severity == DiagnosticSeverity.Warning ? "warning" : "info")
             .Append(' ').Append(span.Start).Append(' ').Append(span.End).Append(' ')
             .Append(Escape(d.GetMessage(System.Globalization.CultureInfo.InvariantCulture))).Append('\n');
        }
        return o.ToString();
    }

    // --- format -----------------------------------------------------------

    static AdhocWorkspace? workspace;
    static ProjectId? projectId;

    // What `dotnet format whitespace --folder` does to one document with no .editorconfig:
    // Formatter.FormatAsync with the default options (4-space indent, "\n" new lines on Linux,
    // Allman braces). Its final-newline and end-of-line passes only act when an .editorconfig
    // asks for them. The text is read as File.ReadAllText reads it, and a byte-order mark is
    // kept as dotnet format keeps it (it writes the file back in the encoding it detected).
    static string Format(string path) {
        if (workspace == null) {
            workspace = new AdhocWorkspace();
            var info = ProjectInfo.Create(ProjectId.CreateNewId(), VersionStamp.Create(), "fmt", "fmt",
                LanguageNames.CSharp, parseOptions: Options);
            projectId = workspace.AddProject(info).Id;
        }
        var text = File.ReadAllText(path);
        var doc = workspace.CurrentSolution.GetProject(projectId!)!.AddDocument(Path.GetFileName(path), SourceText.From(text));
        var formatted = Formatter.FormatAsync(doc).GetAwaiter().GetResult();
        var result = formatted.GetTextAsync().GetAwaiter().GetResult().ToString();
        var head = new byte[3];
        using (var f = File.OpenRead(path)) f.ReadExactly(head, 0, Math.Min(3, (int)f.Length));
        return head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF ? "\uFEFF" + result : result;
    }

    static int FormatMain(string[] args) {
        var stdout = new StreamWriter(Console.OpenStandardOutput(), new UTF8Encoding(false)) { AutoFlush = false };
        if (args.Length == 2) {
            stdout.Write(Format(args[1]));
            stdout.Flush();
            return 0;
        }
        if (args.Length < 4 || args[1] != "--out") {
            Console.Error.WriteLine("usage: csharp-oracle format FILE | format --out DIR FILE... | format --out DIR --stdin-list");
            return 2;
        }
        var outDir = args[2];
        IEnumerable<string> files = args.Skip(3);
        if (args.Length == 4 && args[3] == "--stdin-list") {
            var list = new List<string>();
            string? line;
            while ((line = Console.In.ReadLine()) != null) if (line.Length > 0) list.Add(line);
            files = list;
        }
        foreach (var path in files) {
            try {
                var result = Format(path);
                var dest = Path.Join(outDir, Path.GetFullPath(path).TrimStart('/'));
                Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                File.WriteAllText(dest, result, new UTF8Encoding(false));
                stdout.WriteLine($"ok {path}");
            } catch (Exception e) {
                stdout.WriteLine($"crash {path} {e.GetType().Name}");
            }
        }
        stdout.Flush();
        return 0;
    }

    static int Main(string[] args) {
        Console.OutputEncoding = new UTF8Encoding(false);
        if (args.Length >= 2 && args[0] == "format") return FormatMain(args);
        if (args.Length == 1 && args[0] == "unicode-categories") {
            // .NET's char.GetUnicodeCategory over every UTF-16 unit, as ranges "first last category"
            // (tools/gen_csharp_syntax.py turns them into src/csharp_unicode.inc).
            int start = 0;
            var cat = CharUnicodeInfo.GetUnicodeCategory((char)0);
            for (int c = 1; c <= 0x10000; c++) {
                var cc = c < 0x10000 ? CharUnicodeInfo.GetUnicodeCategory((char)c) : (UnicodeCategory)(-1);
                if (cc != cat) {
                    Console.WriteLine($"{start} {c - 1} {(int)cat}");
                    start = c;
                    cat = cc;
                }
            }
            return 0;
        }
        if (args.Length == 2 && args[0] == "compile-each") {
            // compile-each REF_DIR, file names on stdin: each file compiled on its own; per file
            // "=== FILE" then its diagnostics (lint calibration over a corpus).
            var refs = Directory.GetFiles(args[1], "*.dll").Select(p => (MetadataReference)MetadataReference.CreateFromFile(p)).ToList();
            var opts = new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, allowUnsafe: true, nullableContextOptions: NullableContextOptions.Disable);
            string? line;
            while ((line = Console.In.ReadLine()) != null) {
                if (line.Length == 0) continue;
                Console.WriteLine("=== " + line);
                try {
                    var tree = CSharpSyntaxTree.ParseText(File.ReadAllText(line), Options, line);
                    var comp = CSharpCompilation.Create("check", new[] { tree }, refs, opts);
                    foreach (var d in comp.GetDiagnostics()) {
                        if (d.Severity == DiagnosticSeverity.Hidden && d.Id != "CS8019") continue;
                        var p = d.Location.GetLineSpan();
                        Console.WriteLine($"{p.StartLinePosition.Line + 1}:{p.StartLinePosition.Character + 1} {d.Severity.ToString().ToLowerInvariant()} {d.Id}");
                    }
                } catch (Exception e) {
                    Console.WriteLine("CRASH " + e.GetType().Name);
                }
            }
            return 0;
        }
        if (args.Length >= 3 && args[0] == "compile") {
            // compile REF_DIR FILE...: one compilation of the files against the reference assemblies;
            // prints every warning (and error) the compiler reports, for calibrating mep's lints.
            var refs = Directory.GetFiles(args[1], "*.dll").Select(p => (MetadataReference)MetadataReference.CreateFromFile(p)).ToList();
            var trees = args.Skip(2).Select(p => CSharpSyntaxTree.ParseText(File.ReadAllText(p), Options, p)).ToList();
            var comp = CSharpCompilation.Create("check", trees, refs,
                new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, allowUnsafe: true, nullableContextOptions: NullableContextOptions.Disable));
            foreach (var d in comp.GetDiagnostics()) {
                if (d.Severity == DiagnosticSeverity.Hidden) continue;
                var p = d.Location.GetLineSpan();
                Console.WriteLine($"{p.Path}:{p.StartLinePosition.Line + 1}:{p.StartLinePosition.Character + 1}: {d.Severity.ToString().ToLowerInvariant()} {d.Id} {d.GetMessage()}");
            }
            return 0;
        }
        if (args.Length >= 1 && args[0] == "dump") {
            var stdout = new StreamWriter(Console.OpenStandardOutput(), new UTF8Encoding(false)) { AutoFlush = false };
            if (args.Length == 2 && args[1] == "--stdin-list") {
                string? line;
                while ((line = Console.In.ReadLine()) != null) {
                    if (line.Length == 0) continue;
                    stdout.Write(Dump(line));
                }
            } else {
                foreach (var path in args.Skip(1)) stdout.Write(Dump(path));
            }
            stdout.Flush();
            return 0;
        }
        if (args.Length < 1 || args[0] != "check") {
            Console.Error.WriteLine("usage: csharp-oracle check FILE... | check --stdin-list | dump FILE... | dump --stdin-list | format FILE | format --out DIR FILE...");
            return 2;
        }
        if (args.Length == 2 && args[1] == "--stdin-list") {
            string? line;
            while ((line = Console.In.ReadLine()) != null) {
                if (line.Length == 0) continue;
                var first = Errors(line).FirstOrDefault();
                Console.WriteLine(first == null ? $"ok {line}" : $"error {line} {Describe(first)}");
            }
            return 0;
        }
        int status = 0;
        foreach (var path in args.Skip(1)) {
            foreach (var d in Errors(path)) {
                Console.WriteLine($"{path}:{Describe(d)}");
                status = 1;
            }
        }
        return status;
    }
}
