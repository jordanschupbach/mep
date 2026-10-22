// C# language UI mode (<leader>uu in a .cs buffer), kBuiltinLanguageUi's
// C# consumer, built on kBuiltinLanguageUiCommon like the Crystal and Clojure
// modes. Included by main.cpp only; BuiltinLanguageUiCsharp() is the Lua
// chunk (help/csharp-mode.org is the user's page).
//
// The .NET SDK has no C# REPL and no Microsoft.CodeAnalysis.CSharp.Scripting,
// but it ships Roslyn, and Roslyn does interactive submissions natively: each
// console input is a script compilation chained to the previous one
// (CSharpCompilation.CreateScriptCompilation(..., previousScriptCompilation)),
// emitted to memory, loaded and run through its <Factory> entry point with
// the shared submission array -- what CSharp.Scripting does internally
// (ScriptBuilder.cs, ScriptExecutionState.cs). So variables, usings, methods
// and types persist, and nothing is re-run. kReplProgram is that console, a
// small .NET program the launcher (in kLua) builds once per SDK version into
// ~/.cache/mep/csharp-ui/<sdk version>/ against the SDK's own Roslyn
// ($(MSBuildBinPath)/Roslyn/bincore: no NuGet, works offline). After each
// input it writes the session's variables (Roslyn's declared types, values),
// definitions, kept inputs and history to files the panels poll, as in the
// other modes. gh / :help reads the reference pack's XML docs
// (packs/Microsoft.NETCore.App.Ref/<ver>/ref/net<N>.0/*.xml) through Roslyn's
// symbols, no network.
#pragma once

#include <string>

// The program is longer than the 64K a string literal is guaranteed.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif

namespace mep_csharp_ui {

inline const char *kReplProgram = R"MEPCS(// mep's C# console: a stateful REPL over Roslyn's own interactive
// submissions -- the machinery Microsoft.CodeAnalysis.CSharp.Scripting (and
// `csi`) is built on, used directly, since the .NET SDK ships Roslyn but not
// the scripting package. Each input is one submission: a script compilation
// chained to the previous one (CSharpCompilation.CreateScriptCompilation with
// previousScriptCompilation), emitted to memory, loaded, and run through its
// <Factory> entry point with the shared submission array, so variables,
// using directives, methods and types persist from input to input and
// nothing is ever re-run. After each input the program writes the session's
// variables, definitions, kept inputs and history to files mep's panels poll.
// Help is the reference pack's XML documentation (no network).
//
// Usage: dotnet mep-csharp-repl.dll SESSION_DIR
using System.Collections;
using System.Collections.Immutable;
using System.Globalization;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Emit;
using Microsoft.CodeAnalysis.Text;

namespace MepCsharpRepl;

// The script's host object: its members are in scope in every input, so a
// program's top-level `args` works.
public class Globals {
    public string[] args = Array.Empty<string>();
}

sealed class Sub {
    public int Input;
    public string Text = "";          // what was evaluated (file lines padded)
    public string Label = "";         // "[7]" or "file.cs:12"
    public string? Origin;            // the source file, if any
    public int Line = 1;              // its first line in Origin
    public CSharpCompilation Comp = null!;
    public SyntaxTree Tree = null!;
    public object? Instance;          // the submission object (its fields are the variables)
    public bool HasSlot;
    public List<IFieldSymbol> Fields = new();
}

// The XML documentation file beside a reference assembly, read the first
// time one of its members is asked for.
sealed class XmlDocs : DocumentationProvider {
    readonly string _path;
    Dictionary<string, string>? _docs;
    public XmlDocs(string path) { _path = path; }
    public override bool Equals(object? obj) => obj is XmlDocs o && o._path == _path;
    public override int GetHashCode() => _path.GetHashCode();
    protected override string GetDocumentationForSymbol(string documentationMemberID, CultureInfo preferredCulture, CancellationToken cancellationToken = default) {
        lock (this) {
            if (_docs == null) {
                _docs = new Dictionary<string, string>(StringComparer.Ordinal);
                try {
                    using var reader = System.Xml.XmlReader.Create(_path, new System.Xml.XmlReaderSettings { DtdProcessing = System.Xml.DtdProcessing.Ignore });
                    reader.MoveToContent();
                    while (!reader.EOF) {
                        if (reader.NodeType == System.Xml.XmlNodeType.Element && reader.Name == "member") {
                            string? name = reader.GetAttribute("name");
                            string xml = reader.ReadOuterXml();
                            if (name != null) _docs[name] = xml;
                        } else reader.Read();
                    }
                } catch { }
            }
            return _docs.TryGetValue(documentationMemberID, out var d) ? d : "";
        }
    }
}

sealed class RefResolver : MetadataReferenceResolver {
    public override bool Equals(object? other) => other is RefResolver;
    public override int GetHashCode() => 1;
    public override bool ResolveMissingAssemblies => false;
    public override ImmutableArray<PortableExecutableReference> ResolveReference(string reference, string? baseFilePath, MetadataReferenceProperties properties) {
        if (reference.StartsWith("nuget:", StringComparison.OrdinalIgnoreCase)) return ImmutableArray<PortableExecutableReference>.Empty;
        string dir = baseFilePath != null && File.Exists(baseFilePath) ? Path.GetDirectoryName(baseFilePath)! : Environment.CurrentDirectory;
        string path = Path.GetFullPath(Path.Combine(dir, reference));
        if (!File.Exists(path) && File.Exists(path + ".dll")) path += ".dll";
        if (!File.Exists(path)) return ImmutableArray<PortableExecutableReference>.Empty;
        Repl.DepPaths[AssemblyName.GetAssemblyName(path).Name ?? Path.GetFileNameWithoutExtension(path)] = path;
        return ImmutableArray.Create(MetadataReference.CreateFromFile(path, properties));
    }
}

static class Repl {
    static string SessionDir = "";
    static readonly List<Sub> Subs = new();
    static readonly List<string> History = new();
    static object?[] States = new object?[8];
    static int StateCount = 1;
    static readonly Globals Host = new();
    static CSharpCompilation? Prev;
    static ImmutableArray<MetadataReference> Refs;
    static CSharpParseOptions ParseOpts = null!;
    static CSharpCompilationOptions Opts = null!;
    static int Counter;
    static int AsmCounter;
    static readonly string Tag = Environment.ProcessId.ToString(CultureInfo.InvariantCulture);
    static readonly Dictionary<string, Assembly> Loaded = new();
    public static readonly Dictionary<string, string> DepPaths = new();
    static readonly bool Tty = !Console.IsOutputRedirected;
    static volatile bool Running;
    static volatile bool Interrupted;
    static string RefDir = "";

    public static readonly string[] DefaultUsings = {
        "System", "System.Collections.Generic", "System.IO", "System.Linq", "System.Net.Http",
        "System.Threading", "System.Threading.Tasks", "System.Text",
    };

    static string Dim(string s) => Tty ? $"\x1b[2m{s}\x1b[0m" : s;
    static string Red(string s) => Tty ? $"\x1b[31m{s}\x1b[0m" : s;
    static string Yellow(string s) => Tty ? $"\x1b[33m{s}\x1b[0m" : s;

    static void WriteAtomic(string name, string text) {
        try {
            string path = Path.Combine(SessionDir, name), tmp = path + ".tmp";
            File.WriteAllText(tmp, text);
            File.Move(tmp, path, true);
        } catch { }
    }

    // --- setup ----------------------------------------------------------------

    // The reference pack beside the running runtime: compile against its
    // reference assemblies (their XML docs are the Help panel's source).
    static string FindRefDir() {
        string rt = RuntimeEnvironment.GetRuntimeDirectory();
        string root = Path.GetFullPath(Path.Combine(rt, "..", "..", ".."));
        string packs = Path.Combine(root, "packs", "Microsoft.NETCore.App.Ref");
        string tfm = $"net{Environment.Version.Major}.0";
        if (Directory.Exists(packs)) {
            var best = Directory.GetDirectories(packs)
                .Where(d => Directory.Exists(Path.Combine(d, "ref", tfm)))
                .Select(d => (d, v: Version.TryParse(Path.GetFileName(d).Split('-')[0], out var v) ? v : new Version(0, 0)))
                .Where(x => x.v.Major == Environment.Version.Major)
                .OrderByDescending(x => x.v).FirstOrDefault();
            if (best.d != null) return Path.Combine(best.d, "ref", tfm);
        }
        return "";
    }

    static void Setup() {
        RefDir = FindRefDir();
        var refs = new List<MetadataReference>();
        IEnumerable<string> dlls = RefDir != ""
            ? Directory.GetFiles(RefDir, "*.dll")
            : ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
                .Where(p => Path.GetFileName(p).StartsWith("System") || Path.GetFileName(p) is "mscorlib.dll" or "netstandard.dll" or "Microsoft.CSharp.dll" or "Microsoft.VisualBasic.Core.dll");
        foreach (var dll in dlls) {
            string xml = Path.ChangeExtension(dll, ".xml");
            try {
                refs.Add(MetadataReference.CreateFromFile(dll, documentation: File.Exists(xml) ? new XmlDocs(xml) : null));
            } catch { }
        }
        refs.Add(MetadataReference.CreateFromFile(typeof(Globals).Assembly.Location));
        Refs = refs.ToImmutableArray();
        ParseOpts = new CSharpParseOptions(LanguageVersion.Preview, DocumentationMode.Parse, SourceCodeKind.Script);
        Opts = new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary,
            usings: DefaultUsings, optimizationLevel: OptimizationLevel.Debug, allowUnsafe: true,
            metadataReferenceResolver: new RefResolver(),
            sourceReferenceResolver: new SourceFileResolver(ImmutableArray<string>.Empty, Environment.CurrentDirectory),
            nullableContextOptions: NullableContextOptions.Disable,
            // CS7022: a file's Main is ignored for the script's own entry
            // point -- :run calls it itself.
            specificDiagnosticOptions: new Dictionary<string, ReportDiagnostic> { ["CS7022"] = ReportDiagnostic.Suppress });
        States[0] = Host;
        AssemblyLoadContext.Default.Resolving += (ctx, name) => {
            if (name.Name != null && Loaded.TryGetValue(name.Name, out var a)) return a;
            if (name.Name != null && DepPaths.TryGetValue(name.Name, out var p)) return ctx.LoadFromAssemblyPath(p);
            return null;
        };
    }

    // --- evaluation -------------------------------------------------------------

    static bool HasResult(CompilationUnitSyntax root, out ExpressionSyntax? expr) {
        expr = null;
        var last = root.Members.LastOrDefault() as GlobalStatementSyntax;
        if (last?.Statement is ExpressionStatementSyntax es && es.SemicolonToken.IsMissing) {
            expr = es.Expression;
            return true;
        }
        return false;
    }

    static string Where(Diagnostic d) {
        var span = d.Location.GetMappedLineSpan();
        if (!span.IsValid) return "";
        var p = span.StartLinePosition;
        return $"{span.Path}({p.Line + 1},{p.Character + 1}): ";
    }

    static void PrintDiagnostics(IEnumerable<Diagnostic> diags) {
        var list = diags.Where(d => d.Severity >= DiagnosticSeverity.Warning && !d.IsSuppressed)
            .OrderByDescending(d => d.Severity).ThenBy(d => d.Location.SourceSpan.Start).ToList();
        int shown = 0;
        foreach (var d in list) {
            if (shown++ == 12) { Console.WriteLine(Dim($"... and {list.Count - 12} more")); break; }
            string sev = d.Severity == DiagnosticSeverity.Error ? "error" : "warning";
            string line = $"{Where(d)}{sev} {d.Id}: {d.GetMessage(CultureInfo.InvariantCulture)}";
            Console.WriteLine(d.Severity == DiagnosticSeverity.Error ? Red(line) : Yellow(line));
        }
        if (list.Any(d => d.Id == "CS0006" && d.GetMessage(CultureInfo.InvariantCulture).Contains("nuget:", StringComparison.OrdinalIgnoreCase)))
            Console.WriteLine(Dim("# NuGet packages are not supported in the console: #r a .dll by its path instead"));
    }

    // Runs `body` on its own thread so Ctrl-C can give the prompt back while
    // it runs (a .NET thread cannot be killed: an abandoned input finishes, or
    // not, in the background).
    static (object? result, Exception? error, bool abandoned) RunGuarded(Func<object?> body) {
        object? result = null;
        Exception? error = null;
        var th = new Thread(() => {
            try { result = body(); }
            catch (TargetInvocationException tie) { error = tie.InnerException ?? tie; }
            catch (AggregateException ae) when (ae.InnerExceptions.Count == 1) { error = ae.InnerExceptions[0]; }
            catch (Exception e) { error = e; }
        }, 256 * 1024 * 1024) { IsBackground = true, Name = "mep-csharp-input" };
        Interrupted = false;
        Running = true;
        th.Start();
        while (!th.Join(20)) {
            if (Interrupted) break;
        }
        Running = false;
        Console.Out.Flush();
        return (result, error, Interrupted && th.IsAlive);
    }

    static string CleanFrame(string f) {
        f = Regex.Replace(f, @"(?<![\w.])(Submission#\d+|Script)[.+]", "");
        f = f.Replace(" in " + Environment.CurrentDirectory + Path.DirectorySeparatorChar + "[", " in [");
        f = Regex.Replace(f, @"<<Initialize>>d__\d+\.MoveNext\(\)", "<top level>");
        f = Regex.Replace(f, @"<(\w+)>d__\d+\.MoveNext\(\)", "$1()");
        return f;
    }

    static void PrintException(Exception e) {
        Console.WriteLine(Red($"{e.GetType().FullName}: {e.Message}"));
        var frames = (e.StackTrace ?? "").Split('\n').Select(l => l.TrimEnd())
            .Where(l => l.Length > 0 && !l.Contains("MepCsharpRepl.") && !l.Contains("System.RuntimeMethodHandle")
                     && !l.Contains("System.Reflection.") && !l.Contains("--- End of stack trace") && !l.Contains("<Factory>"))
            .Take(12).ToList();
        foreach (var f in frames) Console.WriteLine(Red(CleanFrame(f)));
        if (e.InnerException != null) Console.WriteLine(Red($" ---> {e.InnerException.GetType().FullName}: {e.InnerException.Message}"));
    }

    static readonly object CompileLock = new();

    // Evaluates one input: `text` (padded with blank lines so its line numbers
    // are the file's), named `path` in messages. Returns whether it was kept.
    public static bool Evaluate(string text, string path, string label, string? origin, int line, bool runMain = false) {
        Sub sub;
        CSharpCompilation comp;
        lock (CompileLock) {
            var tree = CSharpSyntaxTree.ParseText(text, ParseOpts, path, Encoding.UTF8);
            string name = $"mep-cs-{Tag}-{++AsmCounter}";
            comp = CSharpCompilation.CreateScriptCompilation(name, tree, Refs, Opts, Prev, null, typeof(Globals));
            var diags = comp.GetDiagnostics();
            if (diags.Any(d => d.Severity == DiagnosticSeverity.Error)) {
                PrintDiagnostics(diags);
                return false;
            }
            sub = new Sub { Input = Counter, Text = text, Label = label, Origin = origin, Line = line, Comp = comp, Tree = tree };
            PrintDiagnostics(diags);
            var entry = comp.GetEntryPoint(default);
            var root = tree.GetCompilationUnitRoot();
            sub.Fields = comp.ScriptClass?.GetMembers().OfType<IFieldSymbol>()
                .Where(f => !f.IsImplicitlyDeclared && f.CanBeReferencedByName).ToList() ?? new();
            if (entry == null) {
                // Only using directives (or nothing to run): kept, nothing to execute.
                Subs.Add(sub);
                Prev = comp;
                return true;
            }
            using var pe = new MemoryStream();
            using var pdb = new MemoryStream();
            var er = comp.Emit(pe, pdb, options: new EmitOptions(debugInformationFormat: DebugInformationFormat.PortablePdb));
            if (!er.Success) {
                if (!er.Diagnostics.Any(d => d.Severity == DiagnosticSeverity.Error)) {
                    // Nothing to emit (only using directives): kept, nothing runs.
                    Subs.Add(sub);
                    Prev = comp;
                    return true;
                }
                PrintDiagnostics(er.Diagnostics);
                return false;
            }
            pe.Position = 0;
            pdb.Position = 0;
            var asm = AssemblyLoadContext.Default.LoadFromStream(pe, pdb);
            Loaded[name] = asm;
            var type = asm.GetType(entry.ContainingType.MetadataName, true)!;
            var factory = type.GetMethod(entry.MetadataName, BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic)!;
            if (StateCount >= States.Length) Array.Resize(ref States, States.Length * 2);
            int slot = StateCount;
            var states = States;
            bool hasResult = HasResult(root, out var expr);
            bool printable = hasResult && expr != null;
            if (printable) {
                var t = comp.GetSemanticModel(tree).GetTypeInfo(expr!).Type;
                if (t == null || t.SpecialType == SpecialType.System_Void) printable = false;
            }
            var (result, error, abandoned) = RunGuarded(() => ((Task<object>)factory.Invoke(null, new object[] { states })!).GetAwaiter().GetResult());
            if (abandoned || error != null) {
                if (abandoned) Console.WriteLine(Red($"interrupted: {label} dropped (it runs on in the background until it finishes)"));
                else PrintException(error!);
                States[slot] = null;
                return false;
            }
            if (States[slot] != null) {
                sub.Instance = States[slot];
                sub.HasSlot = true;
                StateCount++;
            }
            Subs.Add(sub);
            Prev = comp;
            if (printable) Console.WriteLine("=> " + Formatter.OneLine(result, 2000, enumerate: true));
            if (runMain) RunMain(sub, asm);
            return true;
        }
    }

    // A file with a Main method and no top-level statements: run Main, as
    // `dotnet run` would.
    static void RunMain(Sub sub, Assembly asm) {
        var root = sub.Tree.GetCompilationUnitRoot();
        if (root.Members.OfType<GlobalStatementSyntax>().Any()) return;
        var scriptClass = sub.Comp.ScriptClass;
        if (scriptClass == null) return;
        foreach (var t in scriptClass.GetTypeMembers()) {
            var main = t.GetMembers("Main").OfType<IMethodSymbol>().FirstOrDefault(m => m.IsStatic && m.Parameters.Length <= 1);
            if (main == null) continue;
            var rt = asm.GetType(scriptClass.MetadataName + "+" + t.MetadataName);
            var mi = rt?.GetMethod("Main", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
            if (mi == null) continue;
            Console.WriteLine(Dim($"# {t.Name}.Main()"));
            var (result, error, abandoned) = RunGuarded(() => {
                object? r = mi.Invoke(null, mi.GetParameters().Length == 1 ? new object[] { Host.args } : Array.Empty<object>());
                if (r is Task task) {
                    task.GetAwaiter().GetResult();
                    r = task.GetType().IsGenericType ? task.GetType().GetProperty("Result")?.GetValue(task) : null;
                }
                return r;
            });
            if (abandoned) Console.WriteLine(Red("interrupted (Main runs on in the background until it finishes)"));
            else if (error != null) PrintException(error);
            else if (result is int code && code != 0) Console.WriteLine(Dim($"# exit code {code}"));
            return;
        }
    }

    // --- files ------------------------------------------------------------------

    // A C# file as script code: `#:package`-style directives and a `#!` line
    // dropped (with a note), namespace declarations unwrapped (a script cannot
    // declare a namespace) and `using`s of the file's own namespaces dropped.
    // Blanked in place, so line and column numbers stay the file's.
    public static string PrepareFile(string text, List<string> notes) {
        var chars = text.ToCharArray();
        void Blank(int start, int end) {
            for (int i = start; i < end && i < chars.Length; i++)
                if (chars[i] != '\n' && chars[i] != '\r') chars[i] = ' ';
        }
        int pos = 0;
        foreach (var rawLine in text.Split('\n')) {
            string t = rawLine.TrimStart();
            if (t.StartsWith("#:") || (pos == 0 && t.StartsWith("#!"))) {
                if (t.StartsWith("#:")) notes.Add($"{t.TrimEnd()} -- not supported in the console (ignored)");
                Blank(pos, pos + rawLine.Length);
            }
            pos += rawLine.Length + 1;
        }
        var tree = CSharpSyntaxTree.ParseText(new string(chars), ParseOpts);
        var root = tree.GetCompilationUnitRoot();
        var namespaces = new HashSet<string>();
        foreach (var ns in root.DescendantNodes().OfType<BaseNamespaceDeclarationSyntax>()) {
            namespaces.Add(ns.Name.ToString());
            if (ns is NamespaceDeclarationSyntax b) {
                Blank(b.NamespaceKeyword.SpanStart, b.OpenBraceToken.Span.End);
                Blank(b.CloseBraceToken.SpanStart, b.CloseBraceToken.Span.End);
                if (b.SemicolonToken.Span.Length > 0) Blank(b.SemicolonToken.SpanStart, b.SemicolonToken.Span.End);
            } else if (ns is FileScopedNamespaceDeclarationSyntax f) {
                Blank(f.NamespaceKeyword.SpanStart, f.SemicolonToken.Span.End);
            }
        }
        if (namespaces.Count > 0) {
            foreach (var u in root.DescendantNodes().OfType<UsingDirectiveSyntax>()) {
                string n = u.NamespaceOrType?.ToString() ?? "";
                if (u.StaticKeyword.IsKind(SyntaxKind.None) && u.Alias == null &&
                    namespaces.Any(ns => n == ns || n.StartsWith(ns + ".") || ns.StartsWith(n + ".")))
                    Blank(u.SpanStart, u.Span.End);
            }
        }
        return new string(chars);
    }

    static void RunFile(string path) {
        string text;
        try { text = File.ReadAllText(path); }
        catch (Exception e) { Console.WriteLine(Red($"cannot read {path}: {e.Message}")); return; }
        RecordHistory($":run {path}");
        var notes = new List<string>();
        string prepared = PrepareFile(text, notes);
        Counter++;
        Console.WriteLine(Dim($"# {path}"));
        foreach (var n in notes) Console.WriteLine(Dim($"# {n}"));
        Evaluate(prepared, path, Path.GetFileName(path), path, 1, runMain: true);
        WritePanels();
    }

    // The top-level member (statement, type, method, using) at LINE of the
    // file at PATH (the buffer's text), evaluated with the file's using
    // directives, at their own line numbers.
    static void EvalAt(string path, int line, string? src) {
        string text;
        try { text = File.ReadAllText(path); }
        catch { Console.WriteLine(Red($"cannot read {path}")); return; }
        var notes = new List<string>();
        string prepared = PrepareFile(text, notes);
        var tree = CSharpSyntaxTree.ParseText(prepared, ParseOpts);
        var root = tree.GetCompilationUnitRoot();
        var lines = tree.GetText().Lines;
        int StartLine(SyntaxNode n) {
            // Doc comments and comments right above it come along.
            int start = n.SpanStart;
            foreach (var tr in n.GetLeadingTrivia().Reverse()) {
                if (tr.IsKind(SyntaxKind.EndOfLineTrivia) || tr.IsKind(SyntaxKind.WhitespaceTrivia)) continue;
                if (tr.IsKind(SyntaxKind.SingleLineCommentTrivia) || tr.IsKind(SyntaxKind.MultiLineCommentTrivia) ||
                    tr.IsKind(SyntaxKind.SingleLineDocumentationCommentTrivia) || tr.IsKind(SyntaxKind.MultiLineDocumentationCommentTrivia))
                    start = tr.SpanStart;
                else break;
            }
            return lines.GetLineFromPosition(start).LineNumber + 1;
        }
        int EndLine(SyntaxNode n) => lines.GetLineFromPosition(Math.Max(n.SpanStart, n.Span.End - 1)).LineNumber + 1;
        var items = new List<SyntaxNode>();
        items.AddRange(root.Usings);
        items.AddRange(root.Members);
        items.AddRange(root.AttributeLists);
        items = items.OrderBy(n => n.SpanStart).ToList();
        SyntaxNode? pick = items.FirstOrDefault(n => StartLine(n) <= line && line <= EndLine(n))
            ?? items.FirstOrDefault(n => StartLine(n) > line) ?? items.LastOrDefault();
        if (pick == null) { Console.WriteLine(Dim("nothing to send")); return; }
        int s = StartLine(pick), e = EndLine(pick);
        // Keep the chosen lines and the file's usings; blank the rest.
        var keep = new bool[lines.Count + 2];
        for (int i = s; i <= e; i++) keep[i] = true;
        if (pick is not UsingDirectiveSyntax)
            foreach (var u in root.Usings) for (int i = StartLine(u); i <= EndLine(u); i++) keep[i] = true;
        var sb = new StringBuilder();
        foreach (var l in lines) {
            int n = l.LineNumber + 1;
            if (keep[n]) sb.Append(l.ToString());
            sb.Append('\n');
            if (n > e) break;
        }
        string label = src != null ? $"{Path.GetFileName(src)}:{s}" : $"[{Counter + 1}]";
        string chunk = string.Join("\n", Enumerable.Range(s, e - s + 1).Select(i => lines[i - 1].ToString()));
        RecordHistory(chunk);
        Counter++;
        Console.WriteLine(Dim(s == e ? $"# {label}" : $"# {label}-{e}"));
        Evaluate(sb.ToString(), src ?? $"[{Counter}]", label, src, s);
        WritePanels();
    }

    // --- panels -----------------------------------------------------------------

    static readonly SymbolDisplayFormat TypeFormat = SymbolDisplayFormat.MinimallyQualifiedFormat
        .WithMiscellaneousOptions(SymbolDisplayFormat.MinimallyQualifiedFormat.MiscellaneousOptions | SymbolDisplayMiscellaneousOptions.UseSpecialTypes);

    record VarInfo(string Name, string Type, object? Value, bool Ok);

    static List<VarInfo> Variables() {
        var map = new Dictionary<string, VarInfo>();
        var order = new List<string>();
        foreach (var sub in Subs) {
            foreach (var f in sub.Fields) {
                object? value = null;
                bool ok = false;
                try {
                    var fi = (sub.Instance?.GetType() ?? null)?.GetField(f.Name, BindingFlags.Instance | BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
                    if (fi != null) { value = fi.GetValue(fi.IsStatic ? null : sub.Instance); ok = true; }
                } catch { }
                order.Remove(f.Name);
                order.Add(f.Name);
                map[f.Name] = new VarInfo(f.Name, f.Type.ToDisplayString(TypeFormat), value, ok);
            }
        }
        return order.Select(n => map[n]).ToList();
    }

    static string OneLineSafe(string s) => s.Replace("\t", " ").Replace("\r", "").Replace("\n", " ");

    record Def(string Kind, string Name, string File, int Line, int Input, string Key, string? Owner);

    static string TypeKind(BaseTypeDeclarationSyntax t) => t switch {
        RecordDeclarationSyntax r => r.ClassOrStructKeyword.IsKind(SyntaxKind.StructKeyword) ? "record struct" : "record",
        ClassDeclarationSyntax => "class",
        StructDeclarationSyntax => "struct",
        InterfaceDeclarationSyntax => "interface",
        EnumDeclarationSyntax => "enum",
        _ => "type",
    };

    static string TypeName(BaseTypeDeclarationSyntax t) =>
        t.Identifier.Text + (t is TypeDeclarationSyntax td && td.TypeParameterList != null ? td.TypeParameterList.ToString() : "");

    static string MethodSig(MethodDeclarationSyntax m) =>
        m.Identifier.Text + (m.TypeParameterList?.ToString() ?? "") + "(" + string.Join(", ", m.ParameterList.Parameters.Select(p => p.Type?.ToString() ?? p.Identifier.Text)) + ")";

    static List<Def> Definitions() {
        var defs = new List<Def>();
        foreach (var sub in Subs) {
            var root = sub.Tree.GetCompilationUnitRoot();
            int Line(SyntaxNode n) => n.GetLocation().GetLineSpan().StartLinePosition.Line + 1;
            string file = sub.Origin ?? "";
            void Add(string kind, string name, SyntaxNode n, string key, string? owner) {
                defs.RemoveAll(d => d.Key == key || (kind != "using" && owner == null && d.Owner == name.Split('<')[0] && key.StartsWith("type ")));
                defs.Add(new Def(kind, name, file, Line(n), sub.Input, key, owner));
            }
            foreach (var u in root.Usings) Add("using", u.ToString().Replace("using ", "").TrimEnd(';').Trim(), u, "using " + u, null);
            foreach (var m in root.Members) {
                switch (m) {
                    case BaseTypeDeclarationSyntax t: {
                        string tn = t.Identifier.Text;
                        defs.RemoveAll(d => d.Owner == tn);
                        Add(TypeKind(t), TypeName(t), t, "type " + tn, null);
                        if (t is TypeDeclarationSyntax td) {
                            if (td.ParameterList != null && t is RecordDeclarationSyntax)
                                foreach (var p in td.ParameterList.Parameters)
                                    defs.Add(new Def("property", $"{tn}.{p.Identifier.Text}", file, Line(p), sub.Input, $"m {tn}.{p.Identifier.Text}", tn));
                            foreach (var mm in td.Members) {
                                (string kind, string name)? row = mm switch {
                                    MethodDeclarationSyntax md => ("method", $"{tn}.{MethodSig(md)}"),
                                    PropertyDeclarationSyntax pd => ("property", $"{tn}.{pd.Identifier.Text}"),
                                    FieldDeclarationSyntax fd => ("field", $"{tn}.{string.Join(", ", fd.Declaration.Variables.Select(v => v.Identifier.Text))}"),
                                    ConstructorDeclarationSyntax cd => ("ctor", $"{tn}({string.Join(", ", cd.ParameterList.Parameters.Select(p => p.Type?.ToString()))})"),
                                    EventDeclarationSyntax ed => ("event", $"{tn}.{ed.Identifier.Text}"),
                                    EventFieldDeclarationSyntax ef => ("event", $"{tn}.{string.Join(", ", ef.Declaration.Variables.Select(v => v.Identifier.Text))}"),
                                    BaseTypeDeclarationSyntax nt => (TypeKind(nt), $"{tn}.{TypeName(nt)}"),
                                    _ => null,
                                };
                                if (row is { } r) defs.Add(new Def(r.kind, r.name, file, Line(mm), sub.Input, "m " + r.name, tn));
                            }
                        }
                        break;
                    }
                    case DelegateDeclarationSyntax dd:
                        Add("delegate", dd.Identifier.Text, dd, "type " + dd.Identifier.Text, null);
                        break;
                    case MethodDeclarationSyntax md:
                        Add("method", MethodSig(md), md, $"method {md.Identifier.Text}/{md.ParameterList.Parameters.Count}", null);
                        break;
                    case PropertyDeclarationSyntax pd:
                        Add("property", pd.Identifier.Text, pd, "property " + pd.Identifier.Text, null);
                        break;
                }
            }
        }
        return defs;
    }

    static void WritePanels() {
        try {
            var vars = Variables();
            var vt = new StringBuilder();
            var vv = new StringBuilder();
            foreach (var v in vars) {
                string one = v.Ok ? Formatter.OneLine(v.Value, 160, enumerate: false) : "(unavailable)";
                vt.Append(v.Name).Append('\t').Append(OneLineSafe(v.Type)).Append('\t').Append(OneLineSafe(one)).Append('\n');
                string actual = v.Value == null ? "" : Formatter.TypeName(v.Value.GetType());
                vv.Append('\x01').Append(v.Name).Append(" : ").Append(v.Type);
                if (actual != "" && actual != v.Type) vv.Append("  (").Append(actual).Append(')');
                vv.Append('\n').Append(v.Ok ? Formatter.Pretty(v.Value) : "(unavailable)").Append('\n');
            }
            WriteAtomic("vars.txt", vt.ToString());
            WriteAtomic("values.txt", vv.ToString());
            var dt = new StringBuilder();
            foreach (var d in Definitions())
                dt.Append(d.Kind).Append('\t').Append(OneLineSafe(d.Name)).Append('\t').Append(d.File).Append('\t').Append(d.Line).Append('\t').Append(d.Input).Append('\n');
            WriteAtomic("defs.txt", dt.ToString());
            var st = new StringBuilder();
            foreach (var s in Subs) {
                st.Append("// ").Append(s.Origin != null ? $"{s.Label} [{s.Input}]" : s.Label).Append('\n');
                st.Append(Unpad(s.Text)).Append("\n\n");
            }
            WriteAtomic("session.csx", st.ToString());
        } catch (Exception e) {
            Console.WriteLine(Dim($"(panels: {e.Message})"));
        }
    }

    static string Unpad(string text) {
        var ls = text.Replace("\r", "").Split('\n').ToList();
        while (ls.Count > 0 && ls[0].Trim().Length == 0) ls.RemoveAt(0);
        while (ls.Count > 0 && ls[^1].Trim().Length == 0) ls.RemoveAt(ls.Count - 1);
        return string.Join("\n", ls);
    }

    static void RecordHistory(string text) {
        string t = text.Trim();
        if (t.Length == 0) return;
        History.Add(t);
        if (History.Count > 200) History.RemoveAt(0);
        WriteAtomic("history.txt", string.Join("\x1e", History));
    }

    // --- help -------------------------------------------------------------------

    static void ShowHelp(string query) {
        query = query.Trim();
        if (query.Length == 0) {
            Console.WriteLine(Dim("usage: :help NAME  (String, string.Split, Console.WriteLine, Enumerable.Select, my_var.Method)"));
            return;
        }
        string text;
        lock (CompileLock) {
            try { text = Help.Lookup(query, Prev ?? BaseCompilation(), Subs); }
            catch (Exception e) { text = $"{query}\n\n(help failed: {e.Message})\n"; }
        }
        WriteAtomic("help.txt", text);
        Console.WriteLine(Dim($"# {query}: in the Help tab"));
    }

    static CSharpCompilation? _base;
    public static CSharpCompilation BaseCompilation() =>
        _base ??= CSharpCompilation.CreateScriptCompilation("mep-cs-help", null, Refs, Opts, null, null, typeof(Globals));

    // --- commands -----------------------------------------------------------------

    static void Usage() {
        Console.WriteLine(
            "  :help NAME             documentation for NAME in the Help tab (String, string.Split, Console.WriteLine, xs.Select)\n" +
            "  :run FILE              load a C# file (top-level statements run; a Main runs when there are none)\n" +
            "  :evalat PATH LINE [SRC]  evaluate the top-level member at LINE of PATH (what <leader>re sends)\n" +
            "  :undo                  drop the last kept input (what it already did stays done)\n" +
            "  :reset                 forget everything\n" +
            "  :session               print the kept inputs\n" +
            "  :quit                  leave (or Ctrl-D)");
    }

    static bool Command(string line) {
        string trimmed = line.Trim();
        int sp = trimmed.IndexOfAny(new[] { ' ', '\t' });
        string cmd = sp < 0 ? trimmed : trimmed[..sp];
        string arg = sp < 0 ? "" : trimmed[(sp + 1)..].Trim();
        switch (cmd) {
            case ":help": case ":h": case ":doc":
                ShowHelp(arg);
                break;
            case ":run": case ":load":
                RunFile(Path.GetFullPath(arg));
                break;
            case ":evalat": {
                // PATH LINE [SRC]: the source's name may have spaces.
                var parts = arg.Split(' ', 3, StringSplitOptions.RemoveEmptyEntries);
                if (parts.Length < 2 || !int.TryParse(parts[1], out int ln)) { Console.WriteLine(Red("usage: :evalat PATH LINE [SRC]")); break; }
                EvalAt(parts[0], ln, parts.Length > 2 ? parts[2] : null);
                break;
            }
            case ":undo":
                if (Subs.Count == 0) { Console.WriteLine(Dim("nothing to undo")); break; }
                var last = Subs[^1];
                Subs.RemoveAt(Subs.Count - 1);
                if (last.HasSlot) { StateCount--; States[StateCount] = null; }
                Prev = Subs.Count > 0 ? Subs[^1].Comp : null;
                Console.WriteLine(Dim($"dropped {last.Label} (what it did is not undone)"));
                WritePanels();
                break;
            case ":reset":
                Subs.Clear();
                Prev = null;
                States = new object?[8];
                States[0] = Host;
                StateCount = 1;
                WritePanels();
                Console.WriteLine(Dim("session cleared"));
                break;
            case ":session":
                foreach (var s in Subs) Console.WriteLine(Dim("// " + s.Label) + "\n" + Unpad(s.Text));
                break;
            case ":quit": case ":q": case ":exit":
                Environment.Exit(0);
                break;
            case ":?": case ":commands":
                Usage();
                break;
            default:
                return false;
        }
        return true;
    }

    static string Prompt(List<string> pending) =>
        pending.Count == 0 ? $"cs[{Counter + 1}]> " : new string(' ', (Counter + 1).ToString(CultureInfo.InvariantCulture).Length) + "  ...> ";

    public static int Main(string[] argv) {
        // Standard output and error as plain file streams: writing through
        // .NET's console streams initializes its terminal handling, which
        // takes the terminal out of canonical mode -- and rlwrap, seeing a
        // program that reads single keys, steps aside.
        var utf8 = new UTF8Encoding(false);
        Console.SetOut(new StreamWriter(new FileStream(new Microsoft.Win32.SafeHandles.SafeFileHandle(1, false), FileAccess.Write, 1), utf8) { AutoFlush = true });
        Console.SetError(new StreamWriter(new FileStream(new Microsoft.Win32.SafeHandles.SafeFileHandle(2, false), FileAccess.Write, 1), utf8) { AutoFlush = true });
        SessionDir = Path.GetFullPath(argv.Length > 0 ? argv[0] : Path.GetTempPath());
        Directory.CreateDirectory(SessionDir);
        foreach (var f in new[] { "vars.txt", "values.txt", "defs.txt", "session.csx", "history.txt", "help.txt" })
            if (!File.Exists(Path.Combine(SessionDir, f))) WriteAtomic(f, "");
        Setup();
        var pending = new List<string>();
        // Lines are read from the raw stream, not Console.In: .NET's own
        // terminal line reader takes the terminal out of canonical mode,
        // which would turn rlwrap's line editing off.
        var input = new StreamReader(new FileStream(new Microsoft.Win32.SafeHandles.SafeFileHandle(0, false), FileAccess.Read, 1), utf8);
        // The session's own Console.ReadLine reads the same stream (Console.In
        // would initialize the terminal handling too).
        Console.SetIn(input);
        // SIGINT through PosixSignalRegistration, not Console.CancelKeyPress:
        // that one initializes .NET's terminal handling, which switches the
        // terminal out of canonical mode (and rlwrap off).
        using var sigint = System.Runtime.InteropServices.PosixSignalRegistration.Create(System.Runtime.InteropServices.PosixSignal.SIGINT, ctx => {
            ctx.Cancel = true;
            if (Running) { Interrupted = true; return; }
            pending.Clear();
            Console.Write("\n" + Prompt(pending));
        });
        // Warm the compiler up while the first input is typed (the first
        // compilation JIT-compiles much of Roslyn).
        if (Environment.GetEnvironmentVariable("MEP_CSHARP_UI_NO_WARMUP") == null) {
            new Thread(() => {
                lock (CompileLock) {
                    try {
                        // The whole path an input takes, on a throwaway submission.
                        var tree = CSharpSyntaxTree.ParseText(PrepareFile("record __MepW(int A);\nvar __mepw = 1;\nnew List<int> { 1 }.Select(x => new __MepW(x)).ToList()", new List<string>()), ParseOpts, "[warmup]", Encoding.UTF8);
                        var c = CSharpCompilation.CreateScriptCompilation("mep-cs-warmup", tree, Refs, Opts, null, null, typeof(Globals));
                        c.GetDiagnostics();
                        if (HasResult(tree.GetCompilationUnitRoot(), out var expr)) c.GetSemanticModel(tree).GetTypeInfo(expr!);
                        _ = c.ScriptClass?.GetMembers().OfType<IFieldSymbol>().Select(f => f.Type.ToDisplayString(TypeFormat)).ToList();
                        using var pe = new MemoryStream();
                        using var pdb = new MemoryStream();
                        if (c.Emit(pe, pdb, options: new EmitOptions(debugInformationFormat: DebugInformationFormat.PortablePdb)).Success) {
                            pe.Position = 0;
                            pdb.Position = 0;
                            var asm = AssemblyLoadContext.Default.LoadFromStream(pe, pdb);
                            var entry = c.GetEntryPoint(default)!;
                            var f = asm.GetType(entry.ContainingType.MetadataName, true)!.GetMethod(entry.MetadataName, BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic)!;
                            var r = ((Task<object>)f.Invoke(null, new object?[] { new object?[] { Host, null } })!).GetAwaiter().GetResult();
                            Formatter.OneLine(r, 200, true);
                            Formatter.Pretty(r);
                        }
                    } catch { }
                }
            }) { IsBackground = true }.Start();
        }
        Console.WriteLine($"C# {RuntimeInformation.FrameworkDescription.Replace(".NET ", "/ .NET ")}, Roslyn {typeof(CSharpCompilation).Assembly.GetName().Version} -- mep's console: each input is a Roslyn submission; state persists. :help NAME, :undo, :reset, :? for more.");
        while (true) {
            Console.Write(Prompt(pending));
            Console.Out.Flush();
            string? line = input.ReadLine();
            if (line == null) { Console.WriteLine(); break; }
            if (pending.Count == 0) {
                if (line.Trim().Length == 0) continue;
                if (line.TrimStart().StartsWith(':') && Command(line)) continue;
            }
            pending.Add(line);
            string text = string.Join("\n", pending);
            var tree = CSharpSyntaxTree.ParseText(text, ParseOpts);
            // An empty line ends an input that is still open (an `if` waiting
            // for its `else`, say) -- unless it is inside a block or string.
            if (!SyntaxFactory.IsCompleteSubmission(tree) && !(line.Trim().Length == 0 && pending.Count > 1 && OnlyTrailingOpen(tree)))
                continue;
            pending.Clear();
            RecordHistory(text);
            Counter++;
            Evaluate(text, $"[{Counter}]", $"[{Counter}]", null, 1);
            WritePanels();
        }
        return 0;
    }

    // An incomplete submission whose only missing piece is a statement that
    // could still continue (no unclosed brace, paren or string).
    static bool OnlyTrailingOpen(SyntaxTree tree) {
        var root = tree.GetCompilationUnitRoot();
        return !root.DescendantTokens().Any(t => t.IsMissing && (t.IsKind(SyntaxKind.CloseBraceToken) || t.IsKind(SyntaxKind.CloseParenToken) || t.IsKind(SyntaxKind.CloseBracketToken)))
            && !root.GetDiagnostics().Any(d => d.Id is "CS1010" or "CS1039" or "CS8997");
    }
}

// Values, the way C# interactive (csi) prints them: `List<int>(3) { 1, 2, 3 }`,
// `"text"`, records and anything else that overrides ToString through it,
// other objects as `Type { Member = value, ... }`.
static class Formatter {
    static readonly Dictionary<Type, string> Aliases = new() {
        [typeof(int)] = "int", [typeof(long)] = "long", [typeof(short)] = "short", [typeof(byte)] = "byte",
        [typeof(uint)] = "uint", [typeof(ulong)] = "ulong", [typeof(ushort)] = "ushort", [typeof(sbyte)] = "sbyte",
        [typeof(float)] = "float", [typeof(double)] = "double", [typeof(decimal)] = "decimal", [typeof(bool)] = "bool",
        [typeof(char)] = "char", [typeof(string)] = "string", [typeof(object)] = "object", [typeof(void)] = "void",
        [typeof(nint)] = "nint", [typeof(nuint)] = "nuint",
    };

    public static string TypeName(Type t) {
        if (Aliases.TryGetValue(t, out var a)) return a;
        if (t.IsArray) return TypeName(t.GetElementType()!) + "[" + new string(',', t.GetArrayRank() - 1) + "]";
        if (t.IsPointer) return TypeName(t.GetElementType()!) + "*";
        var nullable = Nullable.GetUnderlyingType(t);
        if (nullable != null) return TypeName(nullable) + "?";
        string name = t.Name;
        int tick = name.IndexOf('`');
        if (tick >= 0) name = name[..tick];
        if (name.StartsWith("<>f__AnonymousType")) return "<anonymous type>";
        if (t.IsGenericType && !t.IsGenericTypeDefinition && t.FullName?.StartsWith("System.ValueTuple") == true)
            return "(" + string.Join(", ", t.GetGenericArguments().Select(TypeName)) + ")";
        if (t.IsGenericType) {
            var args = t.GetGenericArguments();
            // Only this type's own arguments (a nested generic shares its parent's).
            int own = args.Length - (t.DeclaringType?.GetGenericArguments().Length ?? 0);
            if (own > 0) name += "<" + string.Join(", ", args.Skip(args.Length - own).Select(TypeName)) + ">";
        }
        if (t.IsNested && t.DeclaringType != null && !t.DeclaringType.Name.StartsWith("Submission#") && t.DeclaringType.Name != "Script" && !t.IsGenericParameter)
            name = TypeName(t.DeclaringType) + "." + name;
        return name;
    }

    static string Quote(string s, char q) {
        var sb = new StringBuilder();
        sb.Append(q);
        foreach (char c in s) {
            switch (c) {
                case '\\': sb.Append("\\\\"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                case '\0': sb.Append("\\0"); break;
                default:
                    if (c == q) sb.Append('\\').Append(c);
                    else if (char.IsControl(c)) sb.Append($"\\u{(int)c:x4}");
                    else sb.Append(c);
                    break;
            }
        }
        return sb.Append(q).ToString();
    }

    static bool OverridesToString(Type t) {
        try {
            var m = t.GetMethod("ToString", BindingFlags.Public | BindingFlags.Instance, Type.EmptyTypes);
            return m != null && m.DeclaringType != typeof(object) && m.DeclaringType != typeof(ValueType);
        } catch { return false; }
    }

    static string Scalar(object v) => v switch {
        string s => Quote(s, '"'),
        char c => Quote(c.ToString(), '\''),
        bool b => b ? "true" : "false",
        float f => f.ToString("R", CultureInfo.InvariantCulture) + "F",
        double d => d.ToString("R", CultureInfo.InvariantCulture),
        decimal m => m.ToString(CultureInfo.InvariantCulture) + "M",
        long l => l.ToString(CultureInfo.InvariantCulture) + "L",
        ulong ul => ul.ToString(CultureInfo.InvariantCulture) + "UL",
        uint ui => ui.ToString(CultureInfo.InvariantCulture) + "U",
        IFormattable fm => fm.ToString(null, CultureInfo.InvariantCulture),
        _ => v.ToString() ?? "",
    };

    static bool IsScalar(object v) => v is string || v.GetType().IsPrimitive || v is decimal || v is Enum ||
        v is DateTime || v is DateTimeOffset || v is TimeSpan || v is Guid || v is DateOnly || v is TimeOnly ||
        v is System.Numerics.BigInteger || v is Type || v is Delegate;

    static bool IsMaterialized(object v) => v is Array || v is ICollection || IsGenericCollection(v.GetType());

    static bool IsGenericCollection(Type t) => t.GetInterfaces().Any(i => i.IsGenericType &&
        (i.GetGenericTypeDefinition() == typeof(IReadOnlyCollection<>) || i.GetGenericTypeDefinition() == typeof(ICollection<>)));

    static int? CountOf(object v) {
        if (v is Array a) return a.Length;
        if (v is ICollection c) return c.Count;
        try { return v.GetType().GetProperty("Count")?.GetValue(v) as int?; } catch { return null; }
    }

    static IEnumerable<(string name, Func<object?> get)> Members(object v) {
        var t = v.GetType();
        foreach (var p in t.GetProperties(BindingFlags.Public | BindingFlags.Instance)) {
            if (p.GetIndexParameters().Length > 0 || !p.CanRead || p.Name == "EqualityContract") continue;
            yield return (p.Name, () => p.GetValue(v));
        }
        foreach (var f in t.GetFields(BindingFlags.Public | BindingFlags.Instance))
            yield return (f.Name, () => f.GetValue(v));
    }

    // One line, at most `max` characters. `enumerate`: whether a lazy sequence
    // (a LINQ query, an iterator) is run to show its items -- yes for an
    // input's own value, no in the Variables panel (it would run it again on
    // every input).
    public static string OneLine(object? v, int max, bool enumerate) {
        var sb = new StringBuilder();
        try { Write(sb, v, 0, max, enumerate); } catch (Exception e) { sb.Append($"<{e.GetType().Name}: {e.Message}>"); }
        string s = sb.ToString();
        return s.Length > max ? s[..(max - 3)] + "..." : s;
    }

    static void Write(StringBuilder sb, object? v, int depth, int max, bool enumerate) {
        if (sb.Length > max) return;
        if (v == null) { sb.Append("null"); return; }
        var t = v.GetType();
        if (v is Enum) { sb.Append(TypeName(t)).Append('.').Append(v.ToString()!.Replace(", ", " | " + TypeName(t) + ".")); return; }
        if (IsScalar(v)) { sb.Append(Scalar(v)); return; }
        if (t.IsGenericType && t.FullName?.StartsWith("System.ValueTuple") == true) {
            sb.Append('(');
            var fields = t.GetFields();
            for (int i = 0; i < fields.Length; i++) {
                if (i > 0) sb.Append(", ");
                Write(sb, fields[i].GetValue(v), depth + 1, max, enumerate);
            }
            sb.Append(')');
            return;
        }
        if (t.IsGenericType && t.GetGenericTypeDefinition() == typeof(KeyValuePair<,>)) {
            sb.Append("[");
            Write(sb, t.GetProperty("Key")!.GetValue(v), depth + 1, max, enumerate);
            sb.Append(", ");
            Write(sb, t.GetProperty("Value")!.GetValue(v), depth + 1, max, enumerate);
            sb.Append(']');
            return;
        }
        if (v is IEnumerable seq) {
            bool materialized = IsMaterialized(v);
            int? count = CountOf(v);
            sb.Append(TypeName(t));
            if (v is Array arr) sb.Remove(sb.Length - 2, 2).Append('[').Append(arr.Length).Append(']');
            else if (count != null) sb.Append('(').Append(count).Append(')');
            if (!materialized && !enumerate) { sb.Append(" { (not enumerated) }"); return; }
            if (depth > 2) { sb.Append(" { ... }"); return; }
            sb.Append(" { ");
            int i = 0;
            bool dict = v is IDictionary;
            foreach (var item in seq) {
                if (sb.Length > max || i == 100) { sb.Append("..."); break; }
                if (i++ > 0) sb.Append(", ");
                if (dict && item is DictionaryEntry de) {
                    sb.Append("{ "); Write(sb, de.Key, depth + 1, max, enumerate); sb.Append(", "); Write(sb, de.Value, depth + 1, max, enumerate); sb.Append(" }");
                } else if (dict && item != null && item.GetType().IsGenericType && item.GetType().GetGenericTypeDefinition() == typeof(KeyValuePair<,>)) {
                    var it = item.GetType();
                    sb.Append("{ "); Write(sb, it.GetProperty("Key")!.GetValue(item), depth + 1, max, enumerate);
                    sb.Append(", "); Write(sb, it.GetProperty("Value")!.GetValue(item), depth + 1, max, enumerate); sb.Append(" }");
                } else Write(sb, item, depth + 1, max, enumerate);
            }
            sb.Append(i > 0 ? " }" : "}");
            return;
        }
        if (v is Task task) {
            sb.Append(TypeName(t)).Append(" { Status = ").Append(task.Status).Append(" }");
            return;
        }
        if (OverridesToString(t)) {
            string s = v.ToString() ?? "";
            // A record prints `Point { X = 1 }`; drop the submission class's name.
            sb.Append(Regex.Replace(s, @"(Submission#\d+|Script)\+", ""));
            return;
        }
        sb.Append(TypeName(t));
        if (depth > 1) { sb.Append(" { ... }"); return; }
        var members = Members(v).ToList();
        if (members.Count == 0) return;
        sb.Append(" { ");
        int n = 0;
        foreach (var (name, get) in members) {
            if (sb.Length > max) break;
            if (n++ > 0) sb.Append(", ");
            sb.Append(name).Append(" = ");
            object? mv;
            try { mv = get(); } catch (Exception e) { sb.Append($"<threw {(e.InnerException ?? e).GetType().Name}>"); continue; }
            Write(sb, mv, depth + 1, max, enumerate);
        }
        sb.Append(" }");
    }

    // The Inspect panel: one item or member per line.
    public static string Pretty(object? v) {
        try {
            if (v == null || IsScalar(v) || v is Enum) return OneLine(v, 100000, false);
            var t = v.GetType();
            var sb = new StringBuilder();
            if (v is IEnumerable seq && !(t.IsGenericType && t.FullName?.StartsWith("System.ValueTuple") == true)) {
                if (!IsMaterialized(v)) return OneLine(v, 400, false) + "\n\n(a lazy sequence: evaluate it, or `.ToList()`, to see its items)";
                int? count = CountOf(v);
                sb.Append(TypeName(t)).Append(count != null ? $", {count} item{(count == 1 ? "" : "s")}" : "").Append('\n');
                int i = 0;
                foreach (var item in seq) {
                    if (i == 500) { sb.Append("...\n"); break; }
                    sb.Append('[').Append(i++).Append("] ").Append(OneLine(item, 400, false)).Append('\n');
                }
                return sb.ToString();
            }
            sb.Append(OneLine(v, 400, false)).Append("\n\n");
            foreach (var (name, get) in Members(v)) {
                string s;
                try { s = OneLine(get(), 300, false); } catch (Exception e) { s = $"<threw {(e.InnerException ?? e).GetType().Name}>"; }
                sb.Append(name).Append(" = ").Append(s).Append('\n');
            }
            return sb.ToString();
        } catch (Exception e) {
            return $"<{e.GetType().Name}: {e.Message}>";
        }
    }
}

// :help / gh -- documentation from the reference pack's XML doc comments
// (through Roslyn's symbols, so signatures print as C#), or the session's own
// source for what it defines.
static class Help {
    static readonly SymbolDisplayFormat Sig = new(
        globalNamespaceStyle: SymbolDisplayGlobalNamespaceStyle.Omitted,
        typeQualificationStyle: SymbolDisplayTypeQualificationStyle.NameAndContainingTypes,
        genericsOptions: SymbolDisplayGenericsOptions.IncludeTypeParameters | SymbolDisplayGenericsOptions.IncludeTypeConstraints,
        memberOptions: SymbolDisplayMemberOptions.IncludeParameters | SymbolDisplayMemberOptions.IncludeType | SymbolDisplayMemberOptions.IncludeContainingType |
                       SymbolDisplayMemberOptions.IncludeModifiers | SymbolDisplayMemberOptions.IncludeAccessibility | SymbolDisplayMemberOptions.IncludeConstantValue,
        kindOptions: SymbolDisplayKindOptions.IncludeTypeKeyword,
        propertyStyle: SymbolDisplayPropertyStyle.ShowReadWriteDescriptor,
        parameterOptions: SymbolDisplayParameterOptions.IncludeType | SymbolDisplayParameterOptions.IncludeName | SymbolDisplayParameterOptions.IncludeParamsRefOut |
                          SymbolDisplayParameterOptions.IncludeDefaultValue | SymbolDisplayParameterOptions.IncludeExtensionThis,
        miscellaneousOptions: SymbolDisplayMiscellaneousOptions.UseSpecialTypes | SymbolDisplayMiscellaneousOptions.EscapeKeywordIdentifiers |
                              SymbolDisplayMiscellaneousOptions.IncludeNullableReferenceTypeModifier);

    static readonly Dictionary<string, SpecialType> Keywords = new() {
        ["int"] = SpecialType.System_Int32, ["long"] = SpecialType.System_Int64, ["short"] = SpecialType.System_Int16,
        ["byte"] = SpecialType.System_Byte, ["sbyte"] = SpecialType.System_SByte, ["uint"] = SpecialType.System_UInt32,
        ["ulong"] = SpecialType.System_UInt64, ["ushort"] = SpecialType.System_UInt16, ["float"] = SpecialType.System_Single,
        ["double"] = SpecialType.System_Double, ["decimal"] = SpecialType.System_Decimal, ["bool"] = SpecialType.System_Boolean,
        ["char"] = SpecialType.System_Char, ["string"] = SpecialType.System_String, ["object"] = SpecialType.System_Object,
        ["nint"] = SpecialType.System_IntPtr, ["nuint"] = SpecialType.System_UIntPtr,
    };

    static Dictionary<string, List<INamedTypeSymbol>>? _types;
    static CSharpCompilation? _typesFor;

    // Every public type of the references, by simple name.
    static Dictionary<string, List<INamedTypeSymbol>> Types(CSharpCompilation comp) {
        var baseComp = Repl.BaseCompilation();
        if (_types != null && _typesFor == baseComp) return _types;
        var map = new Dictionary<string, List<INamedTypeSymbol>>(StringComparer.Ordinal);
        void Walk(INamespaceSymbol ns) {
            foreach (var t in ns.GetTypeMembers()) AddType(t);
            foreach (var n in ns.GetNamespaceMembers()) Walk(n);
        }
        void AddType(INamedTypeSymbol t) {
            if (t.DeclaredAccessibility != Accessibility.Public) return;
            if (!map.TryGetValue(t.Name, out var l)) map[t.Name] = l = new();
            l.Add(t);
            foreach (var n in t.GetTypeMembers()) AddType(n);
        }
        Walk(baseComp.GlobalNamespace);
        _typesFor = baseComp;
        return _types = map;
    }

    static int NamespaceRank(ISymbol s, HashSet<string> usings) {
        string ns = s.ContainingNamespace?.ToDisplayString() ?? "";
        if (usings.Contains(ns)) return ns == "System" ? 0 : 1;
        if (ns.StartsWith("System")) return 2 + ns.Count(c => c == '.');
        return 10;
    }

    static HashSet<string> Usings(List<Sub> subs) {
        var set = new HashSet<string>(Repl.DefaultUsings);
        foreach (var s in subs)
            foreach (var u in s.Tree.GetCompilationUnitRoot().Usings)
                if (u.Alias == null && u.StaticKeyword.IsKind(SyntaxKind.None) && u.NamespaceOrType != null) set.Add(u.NamespaceOrType.ToString());
        return set;
    }

    // The types named `name` the session can see: its own first, latest first.
    static List<INamedTypeSymbol> FindTypes(string name, CSharpCompilation comp, List<Sub> subs, HashSet<string> usings) {
        var result = new List<INamedTypeSymbol>();
        for (int i = subs.Count - 1; i >= 0; i--) {
            var sc = subs[i].Comp.ScriptClass;
            if (sc == null) continue;
            var own = sc.GetTypeMembers(name);
            if (own.Length > 0) { result.AddRange(own); return result; }
        }
        if (Keywords.TryGetValue(name, out var st)) { result.Add(comp.GetSpecialType(st)); return result; }
        // Qualified: System.Text.StringBuilder
        if (name.Contains('.')) {
            for (int arity = 0; arity <= 4; arity++) {
                var t = comp.GetTypeByMetadataName(arity == 0 ? name : $"{name}`{arity}");
                if (t != null) result.Add(t);
            }
            return result;
        }
        if (Types(comp).TryGetValue(name, out var l))
            result.AddRange(l.OrderBy(t => NamespaceRank(t, usings)).ThenBy(t => t.ToDisplayString().Length));
        return result;
    }

    static IEnumerable<ITypeSymbol> SelfAndBases(ITypeSymbol t) {
        var seen = new HashSet<ITypeSymbol>(SymbolEqualityComparer.Default);
        for (var b = t; b != null; b = b.BaseType) if (seen.Add(b)) yield return b;
        foreach (var i in t.AllInterfaces) if (seen.Add(i)) yield return i;
        if (t.TypeKind == TypeKind.Interface) {
            var obj = Repl.BaseCompilation().GetSpecialType(SpecialType.System_Object);
            if (seen.Add(obj)) yield return obj;
        }
    }

    static List<ISymbol> MembersNamed(ITypeSymbol t, string name, CSharpCompilation comp, HashSet<string> usings) {
        var found = new List<ISymbol>();
        // A class's or struct's own chain first; its interfaces only when that
        // has nothing by the name (IList.Add is not what list.Add calls).
        var chain = SelfAndBases(t).ToList();
        if (t.TypeKind != TypeKind.Interface) {
            var classes = chain.Where(c => c.TypeKind != TypeKind.Interface).ToList();
            if (classes.Any(c => c.GetMembers(name).Any(m => m.DeclaredAccessibility == Accessibility.Public || IsSession(m)))) chain = classes;
        }
        foreach (var b in chain) {
            foreach (var m in b.GetMembers(name)) {
                if (m.DeclaredAccessibility != Accessibility.Public && m.DeclaredAccessibility != Accessibility.Protected && !IsSession(m)) continue;
                // An override is documented where it is declared.
                if (found.Any(f => SameSignature(f, m))) continue;
                found.Add(m);
            }
        }
        if (found.Count == 0) {
            // Extension methods in scope: xs.Select is Enumerable.Select.
            foreach (var ns in usings) {
                var nsSym = Namespace(comp, ns);
                if (nsSym == null) continue;
                foreach (var st in nsSym.GetTypeMembers().Where(x => x.IsStatic && x.MightContainExtensionMethods && x.DeclaredAccessibility == Accessibility.Public)) {
                    foreach (var m in st.GetMembers(name).OfType<IMethodSymbol>()) {
                        if (!m.IsExtensionMethod) continue;
                        try { if (m.ReduceExtensionMethod(t) == null) continue; } catch { continue; }
                        found.Add(m);
                    }
                }
            }
        }
        return found;
    }

    static bool IsSession(ISymbol s) {
        for (var t = s.ContainingType; t != null; t = t.ContainingType) if (t.IsScriptClass) return true;
        return false;
    }

    static bool SameSignature(ISymbol a, ISymbol b) {
        if (a is IMethodSymbol ma && b is IMethodSymbol mb)
            return ma.Parameters.Length == mb.Parameters.Length && ma.Parameters.Zip(mb.Parameters).All(p => SymbolEqualityComparer.Default.Equals(p.First.Type, p.Second.Type));
        return a.Kind == b.Kind && a.Kind == SymbolKind.Property;
    }

    static INamespaceSymbol? Namespace(CSharpCompilation comp, string name) {
        INamespaceSymbol? ns = comp.GlobalNamespace;
        foreach (var part in name.Split('.')) {
            ns = ns?.GetNamespaceMembers().FirstOrDefault(n => n.Name == part);
            if (ns == null) return null;
        }
        return ns;
    }

    static ITypeSymbol? MemberType(ISymbol m) => m switch {
        IPropertySymbol p => p.Type,
        IFieldSymbol f => f.Type,
        IMethodSymbol me => me.ReturnType,
        IEventSymbol e => e.Type,
        INamedTypeSymbol nt => nt,
        _ => null,
    };

    public static string Lookup(string query, CSharpCompilation comp, List<Sub> subs) {
        var sb = new StringBuilder();
        sb.Append(query).Append("\n\n");
        string q = query.Trim().TrimEnd(';').Replace("()", "");
        while (Regex.IsMatch(q, "<[^<>]*>")) q = Regex.Replace(q, "<[^<>]*>", "");
        q = Regex.Replace(q, @"`\d+", "").Replace("?.", ".").Replace("#", ".").Trim('.');
        if (q.Length == 0) return sb.Append("(nothing to look up)\n").ToString();
        var parts = q.Split('.', StringSplitOptions.RemoveEmptyEntries);
        var usings = Usings(subs);

        bool sessionShown = SessionSource(sb, parts[^1], parts.Length > 1 ? parts[^2] : null, subs);

        // The receiver: a session variable, a type (or namespace-qualified
        // type), or nothing.
        ITypeSymbol? recv = null;
        string? recvVar = null;
        int i = 0;
        if (parts.Length > 0) {
            for (int k = subs.Count - 1; k >= 0 && recv == null; k--) {
                var f = subs[k].Fields.FirstOrDefault(x => x.Name == parts[0]);
                if (f != null) { recv = f.Type; recvVar = parts[0]; i = 1; }
            }
        }
        if (recv == null) {
            for (int k = parts.Length; k >= 1 && recv == null; k--) {
                string name = string.Join(".", parts.Take(k));
                var ts = FindTypes(name, comp, subs, usings);
                if (ts.Count > 0) {
                    if (k == parts.Length) {
                        // A type itself.
                        DescribeType(sb, ts[0]);
                        if (ts.Count > 1) {
                            sb.Append("\nAlso named ").Append(parts[^1]).Append(":\n");
                            foreach (var o in ts.Skip(1).Take(20)) sb.Append("  ").Append(o.ToDisplayString()).Append('\n');
                        }
                        return sb.ToString();
                    }
                    recv = ts[0];
                    i = k;
                }
            }
        }
        if (recv == null && parts.Length >= 1) {
            // A namespace?
            var ns = Namespace(comp, q);
            if (ns != null && q.Length > 0) {
                sb.Append("namespace ").Append(q).Append("\n\n");
                var types = ns.GetTypeMembers().Where(t => t.DeclaredAccessibility == Accessibility.Public).Select(t => t.Name).Distinct().OrderBy(x => x).ToList();
                var nss = ns.GetNamespaceMembers().Select(n => n.Name).OrderBy(x => x).ToList();
                if (nss.Count > 0) sb.Append("Namespaces: ").Append(string.Join(", ", nss)).Append("\n\n");
                sb.Append("Types (").Append(types.Count).Append("):\n").Append(string.Join(", ", types)).Append('\n');
                return sb.ToString();
            }
        }
        if (recv == null) {
            // A bare member name: every public member so named in the types the
            // session's usings bring in.
            if (parts.Length == 1) {
                var hits = new List<ISymbol>();
                foreach (var list in Types(comp).Values)
                    foreach (var t in list) {
                        if (!usings.Contains(t.ContainingNamespace?.ToDisplayString() ?? "")) continue;
                        foreach (var m in t.GetMembers(parts[0]))
                            if (m.DeclaredAccessibility == Accessibility.Public && m is not INamedTypeSymbol) hits.Add(m);
                    }
                if (hits.Count > 0) {
                    var byType = hits.OrderBy(h => NamespaceRank(h.ContainingType, usings)).ThenBy(h => h.ContainingType.Name).ToList();
                    DescribeMembers(sb, byType, null);
                    return sb.ToString();
                }
            }
            if (!sessionShown) sb.Append("Nothing named ").Append(query).Append(" in the session or the reference documentation (the session's usings: ")
                .Append(string.Join(", ", usings.OrderBy(x => x))).Append(").\n");
            return sb.ToString();
        }
        // Walk the remaining parts as members.
        List<ISymbol> members = new();
        for (; i < parts.Length; i++) {
            members = MembersNamed(recv!, parts[i], comp, usings);
            if (members.Count == 0) {
                if (!sessionShown) sb.Append("Nothing named ").Append(parts[i]).Append(" in ").Append(recv!.ToDisplayString())
                    .Append(" or what it inherits.\n\n");
                DescribeType(sb, recv!);
                return sb.ToString();
            }
            if (i < parts.Length - 1) {
                var next = MemberType(members[0]);
                if (next == null) break;
                recv = next;
            }
        }
        if (members.Count == 0) {
            if (recvVar != null) sb.Append(recvVar).Append(" is a session variable of type ").Append(recv!.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat)).Append(".\n\n");
            DescribeType(sb, recv!);
            return sb.ToString();
        }
        if (recvVar != null) sb.Append("(").Append(recvVar).Append(" : ").Append(recv!.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat)).Append(")\n\n");
        DescribeMembers(sb, members, recv);
        return sb.ToString();
    }

    static bool SessionSource(StringBuilder sb, string name, string? owner, List<Sub> subs) {
        bool found = false;
        foreach (var sub in subs) {
            var root = sub.Tree.GetCompilationUnitRoot();
            foreach (var m in root.Members) {
                bool hit = m switch {
                    BaseTypeDeclarationSyntax t => t.Identifier.Text == name ||
                        (owner == t.Identifier.Text && t is TypeDeclarationSyntax td && td.Members.Any(mm => MemberName(mm) == name)),
                    MethodDeclarationSyntax md => md.Identifier.Text == name && owner == null,
                    PropertyDeclarationSyntax pd => pd.Identifier.Text == name && owner == null,
                    DelegateDeclarationSyntax dd => dd.Identifier.Text == name,
                    _ => false,
                };
                if (!hit) continue;
                if (!found) sb.Append("Defined in this session:\n\n");
                found = true;
                int line = m.GetLocation().GetLineSpan().StartLinePosition.Line + 1;
                sb.Append("// ").Append(sub.Origin != null ? $"{Path.GetFileName(sub.Origin)}:{line} [{sub.Input}]" : sub.Label).Append('\n');
                sb.Append(m.ToString().TrimEnd()).Append("\n\n");
            }
        }
        return found;
    }

    static string? MemberName(MemberDeclarationSyntax m) => m switch {
        MethodDeclarationSyntax md => md.Identifier.Text,
        PropertyDeclarationSyntax pd => pd.Identifier.Text,
        BaseTypeDeclarationSyntax t => t.Identifier.Text,
        FieldDeclarationSyntax f => f.Declaration.Variables.FirstOrDefault()?.Identifier.Text,
        _ => null,
    };

    static string Where(ISymbol s) {
        if (IsSession(s)) return "defined in this session";
        string container = s.ContainingType != null ? s.ContainingType.ToDisplayString() : s.ContainingNamespace?.ToDisplayString() ?? "";
        string asm = s.ContainingAssembly?.Name ?? "";
        return (s is INamedTypeSymbol ? $"namespace {s.ContainingNamespace?.ToDisplayString()}" : $"in {container}") + (asm != "" ? $" -- {asm}" : "");
    }

    static void DescribeType(StringBuilder sb, ITypeSymbol t) {
        sb.Append(t.ToDisplayString(Sig)).Append('\n');
        sb.Append("  ").Append(Where(t)).Append('\n');
        if (t.BaseType != null && t.BaseType.SpecialType != SpecialType.System_Object && t.TypeKind == TypeKind.Class)
            sb.Append("  base: ").Append(t.BaseType.ToDisplayString()).Append('\n');
        var ifaces = t.Interfaces.Where(x => x.DeclaredAccessibility == Accessibility.Public).Select(x => x.ToDisplayString()).ToList();
        if (ifaces.Count > 0) sb.Append("  implements: ").Append(string.Join(", ", ifaces.Take(12))).Append(ifaces.Count > 12 ? ", ..." : "").Append('\n');
        sb.Append('\n').Append(Doc(t)).Append('\n');
        var groups = new (string title, Func<ISymbol, bool> pick)[] {
            ("Constructors", m => m is IMethodSymbol { MethodKind: MethodKind.Constructor }),
            ("Static properties and fields", m => (m is IPropertySymbol || m is IFieldSymbol) && m.IsStatic),
            ("Static methods", m => m is IMethodSymbol { MethodKind: MethodKind.Ordinary } && m.IsStatic),
            ("Properties", m => m is IPropertySymbol && !m.IsStatic),
            ("Fields", m => m is IFieldSymbol && !m.IsStatic),
            ("Methods", m => m is IMethodSymbol { MethodKind: MethodKind.Ordinary } && !m.IsStatic),
            ("Events", m => m is IEventSymbol),
        };
        var all = t.GetMembers().Where(m => (m.DeclaredAccessibility == Accessibility.Public || IsSession(m)) && !m.IsImplicitlyDeclared || m is IMethodSymbol { MethodKind: MethodKind.Constructor } && m.DeclaredAccessibility == Accessibility.Public).ToList();
        if (t.TypeKind == TypeKind.Enum) {
            var vals = t.GetMembers().OfType<IFieldSymbol>().Where(f => f.HasConstantValue).Select(f => $"{f.Name} = {f.ConstantValue}").ToList();
            sb.Append("\nMembers (").Append(vals.Count).Append("):\n").Append(string.Join(", ", vals)).Append('\n');
            return;
        }
        foreach (var (title, pick) in groups) {
            var names = all.Where(pick).Select(m => m is IMethodSymbol { MethodKind: MethodKind.Constructor } ms
                ? $"({string.Join(", ", ms.Parameters.Select(p => p.Type.ToDisplayString(SymbolDisplayFormat.MinimallyQualifiedFormat)))})"
                : m.Name).Distinct().OrderBy(x => x, StringComparer.Ordinal).ToList();
            if (names.Count == 0) continue;
            sb.Append('\n').Append(title).Append(" (").Append(names.Count).Append("):\n").Append(string.Join(", ", names)).Append('\n');
        }
    }

    // Simplest overloads first: fewer parameters, and spans and pointers last.
    static (int, int) OverloadRank(ISymbol m) => m is IMethodSymbol ms
        ? (ms.Parameters.Count(p => p.Type.Name is "ReadOnlySpan" or "Span" || p.Type.TypeKind == TypeKind.Pointer || p.RefKind != RefKind.None), ms.Parameters.Length)
        : (0, 0);

    static void DescribeMembers(StringBuilder sb, List<ISymbol> members, ITypeSymbol? recv) {
        var owners = members.Select(m => m.ContainingType?.ToDisplayString() ?? "").Distinct().ToList();
        members = members.Select((m, i) => (m, i)).OrderBy(x => owners.IndexOf(x.m.ContainingType?.ToDisplayString() ?? ""))
            .ThenBy(x => OverloadRank(x.m)).ThenBy(x => x.i).Select(x => x.m).ToList();
        int shown = 0;
        foreach (var m in members) {
            if (shown == 8) break;
            if (shown++ > 0) sb.Append("--------\n");
            sb.Append(m.ToDisplayString(Sig)).Append('\n');
            sb.Append("  ").Append(Where(m)).Append('\n');
            sb.Append('\n').Append(Doc(m));
        }
        if (members.Count > 8) {
            sb.Append("\nAlso (").Append(members.Count - 8).Append("):\n");
            foreach (var m in members.Skip(8).Take(60)) sb.Append("  ").Append(m.ToDisplayString(Sig)).Append('\n');
        }
    }

    static string Doc(ISymbol s) {
        string xml = "";
        for (var cur = s; cur != null && xml.Length == 0;) {
            try { xml = cur.GetDocumentationCommentXml(expandIncludes: true) ?? ""; } catch { xml = ""; }
            if (xml.Length > 0) break;
            // Inherited documentation: the member it overrides or implements.
            ISymbol? next = cur switch {
                IMethodSymbol m => (ISymbol?)m.OverriddenMethod ?? m.ExplicitInterfaceImplementations.FirstOrDefault(),
                IPropertySymbol p => (ISymbol?)p.OverriddenProperty ?? p.ExplicitInterfaceImplementations.FirstOrDefault(),
                _ => null,
            };
            if (next == null && cur.ContainingType != null) {
                foreach (var iface in cur.ContainingType.AllInterfaces) {
                    foreach (var im in iface.GetMembers(cur.Name)) {
                        if (SymbolEqualityComparer.Default.Equals(cur.ContainingType.FindImplementationForInterfaceMember(im), cur)) { next = im; break; }
                    }
                    if (next != null) break;
                }
            }
            if (next != null && next.OriginalDefinition is { } od && !SymbolEqualityComparer.Default.Equals(od, cur)) cur = od; else break;
        }
        // The session's own: its source is shown above, from SessionSource.
        if (IsSession(s) && xml.Length == 0) return "";
        if (xml.Length == 0) return "(no documentation)\n";
        try { return Render(XElement.Parse(xml)); }
        catch { return xml + "\n"; }
    }

    static string CrefName(string cref) {
        string c = cref.Length > 2 && cref[1] == ':' ? cref[2..] : cref;
        int paren = c.IndexOf('(');
        if (paren >= 0) c = c[..paren];
        c = Regex.Replace(c, @"`+\d+", "");
        var parts = c.Split('.');
        // Type.Member, or just the type.
        if (cref.StartsWith("T:") || parts.Length < 2) return parts[^1];
        return parts[^2] + "." + parts[^1];
    }

    static string Inline(XNode node) {
        switch (node) {
            case XText t: return Regex.Replace(t.Value, @"\s+", " ");
            case XElement e:
                switch (e.Name.LocalName) {
                    case "see": case "seealso":
                        if (e.Attribute("langword") is { } lw) return lw.Value;
                        if (e.Attribute("cref") is { } cr) return e.Nodes().Any() ? string.Concat(e.Nodes().Select(Inline)) : CrefName(cr.Value);
                        if (e.Attribute("href") is { } href) return e.Nodes().Any() ? string.Concat(e.Nodes().Select(Inline)) + $" <{href.Value}>" : href.Value;
                        return string.Concat(e.Nodes().Select(Inline));
                    case "paramref": case "typeparamref": return e.Attribute("name")?.Value ?? "";
                    case "c": return "`" + e.Value + "`";
                    case "code": return "\n\n" + string.Join("\n", e.Value.Trim('\n').Split('\n').Select(l => "    " + l.TrimEnd())) + "\n\n";
                    case "para": return "\n\n" + string.Concat(e.Nodes().Select(Inline)).Trim() + "\n\n";
                    case "br": return "\n";
                    case "list":
                        return "\n" + string.Concat(e.Elements("item").Select(it =>
                            "\n  - " + string.Join(" -- ", it.Elements().Select(x => string.Concat(x.Nodes().Select(Inline)).Trim()).Where(x => x.Length > 0)))) + "\n\n";
                    default: return string.Concat(e.Nodes().Select(Inline));
                }
            default: return "";
        }
    }

    static string Para(XElement? e) {
        if (e == null) return "";
        string s = string.Concat(e.Nodes().Select(Inline));
        s = Regex.Replace(s, @"[ \t]+\n", "\n");
        s = Regex.Replace(s, @"\n{3,}", "\n\n");
        s = Regex.Replace(s, @"(?m)^ (?=\S)", "");
        return s.Trim();
    }

    static string Render(XElement doc) {
        var sb = new StringBuilder();
        string summary = Para(doc.Element("summary"));
        sb.Append(summary.Length > 0 ? summary : "(no summary)").Append('\n');
        var tps = doc.Elements("typeparam").ToList();
        var ps = doc.Elements("param").ToList();
        if (tps.Count > 0) {
            sb.Append("\nType parameters:\n");
            foreach (var p in tps) sb.Append("  ").Append(p.Attribute("name")?.Value).Append(" -- ").Append(Para(p)).Append('\n');
        }
        if (ps.Count > 0) {
            sb.Append("\nParameters:\n");
            foreach (var p in ps) sb.Append("  ").Append(p.Attribute("name")?.Value).Append(" -- ").Append(Para(p)).Append('\n');
        }
        if (doc.Element("returns") is { } r) sb.Append("\nReturns: ").Append(Para(r)).Append('\n');
        if (doc.Element("value") is { } v) sb.Append("\nValue: ").Append(Para(v)).Append('\n');
        var ex = doc.Elements("exception").ToList();
        if (ex.Count > 0) {
            sb.Append("\nExceptions:\n");
            foreach (var x in ex) sb.Append("  ").Append(CrefName(x.Attribute("cref")?.Value ?? "")).Append(" -- ").Append(Para(x)).Append('\n');
        }
        if (doc.Element("remarks") is { } rm && Para(rm) is { Length: > 0 } rt) sb.Append("\nRemarks:\n").Append(rt).Append('\n');
        if (doc.Element("example") is { } eg && Para(eg) is { Length: > 0 } et) sb.Append("\nExample:\n").Append(et).Append('\n');
        return sb.ToString();
    }
}
)MEPCS";

inline const char *kReplProject = R"MEPCS(<Project Sdk="Microsoft.NET.Sdk">
  <!-- mep's C# console (see Program.cs). Roslyn is the copy the .NET SDK
       ships, referenced in place: no NuGet, works offline. -->
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net$(BundledNETCoreAppTargetFrameworkVersion)</TargetFramework>
    <Nullable>enable</Nullable>
    <ImplicitUsings>enable</ImplicitUsings>
    <AssemblyName>mep-csharp-repl</AssemblyName>
    <RootNamespace>MepCsharpRepl</RootNamespace>
    <UseAppHost>false</UseAppHost>
    <InvariantGlobalization>false</InvariantGlobalization>
    <SatelliteResourceLanguages>en</SatelliteResourceLanguages>
    <GenerateDocumentationFile>false</GenerateDocumentationFile>
    <NoWarn>$(NoWarn);CS8618;CS8600;CS8602;CS8604;CS8619;CS0162</NoWarn>
  </PropertyGroup>
  <ItemGroup>
    <Reference Include="Microsoft.CodeAnalysis">
      <HintPath>$(MSBuildBinPath)/Roslyn/bincore/Microsoft.CodeAnalysis.dll</HintPath>
    </Reference>
    <Reference Include="Microsoft.CodeAnalysis.CSharp">
      <HintPath>$(MSBuildBinPath)/Roslyn/bincore/Microsoft.CodeAnalysis.CSharp.dll</HintPath>
    </Reference>
  </ItemGroup>
</Project>
)MEPCS";

inline const char *kLua = R"MEPLUA(mep.opt = mep.opt or {}
mep.opt.csharp_ui_rlwrap = mep.opt.csharp_ui_rlwrap ~= false
mep.opt.csharp_ui_info_share = mep.opt.csharp_ui_info_share or 0.3
mep.opt.csharp_ui_bottom_share = mep.opt.csharp_ui_bottom_share or 0.4
mep.opt.csharp_ui_bottom_info_share = mep.opt.csharp_ui_bottom_info_share or 0.4
for _, e in ipairs({'cs', 'csx'}) do
  mep.language_ui_extensions[e] = mep.language_ui_extensions[e] or 'csharp'
end
-- The console's launcher: builds the program above once per .NET SDK
-- version (and again whenever mep ships a different one) into the cache,
-- then runs it, under rlwrap when there is one.
local mep_csharp_ui_launcher = [==[
src=$1 proj=$2 sess=$3 use_rlwrap=$4
export DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1
if ! command -v dotnet >/dev/null 2>&1; then
  echo "mep's C# console needs the .NET SDK (dotnet) on PATH."
  exit 1
fi
ver=$(dotnet --version 2>/dev/null)
if [ -z "$ver" ]; then
  echo "mep's C# console needs a .NET SDK: 'dotnet --version' found none."
  exit 1
fi
root=${XDG_CACHE_HOME:-$HOME/.cache}/mep/csharp-ui/$ver
mkdir -p "$root/src" || exit 1
dll=$root/bin/mep-csharp-repl.dll
if [ ! -f "$dll" ] || ! cmp -s "$src" "$root/src/Program.cs" || ! cmp -s "$proj" "$root/src/mep-csharp-repl.csproj"; then
  echo "Building mep's C# console (once per .NET SDK version, a few seconds)..."
  rm -rf "$root/build" "$root/bin.new"
  mkdir -p "$root/build" &&
    cp "$src" "$root/build/Program.cs" && cp "$proj" "$root/build/mep-csharp-repl.csproj" &&
    dotnet build "$root/build/mep-csharp-repl.csproj" -c Release -o "$root/bin.new" -nologo -v q \
      -nodeReuse:false -p:UseSharedCompilation=false >"$root/build.log" 2>&1 &&
    rm -rf "$root/bin" && mv "$root/bin.new" "$root/bin" &&
    cp "$src" "$root/src/Program.cs" && cp "$proj" "$root/src/mep-csharp-repl.csproj" || {
    echo "Building it failed ($root/build.log):"
    tail -n 20 "$root/build.log"
    exit 1
  }
fi
if [ "$use_rlwrap" = 1 ] && command -v rlwrap >/dev/null 2>&1; then
  exec rlwrap -H "$root/history" dotnet "$dll" "$sess"
fi
exec dotnet "$dll" "$sess"
]==]
local mep_csharp_ui_state = {}
local mep_csharp_ui_sidebars = nil
local function mep_csharp_ui_ensure_sidebars()
  if mep_csharp_ui_sidebars then return end
  mep_csharp_ui_sidebars = {
    variables = mep.sidebar_create('Variables', 'right', 44),
    definitions = mep.sidebar_create('Definitions', 'right', 44),
    inspect = mep.sidebar_create('Inspect', 'right', 44),
    session = mep.sidebar_create('Session', 'right', 44),
    history = mep.sidebar_create('History', 'right', 44),
    help = mep.sidebar_create('Help', 'right', 44),
  }
end
-- values.txt: one "\1name : Type" header per variable, then its value laid
-- out one item or member per line.
local function mep_csharp_ui_value_of(st, name)
  for header, body in (st.values_text or ''):gmatch('\1([^\n]*)\n([^\1]*)') do
    if header:match('^(%S+)') == name then return header .. '\n\n' .. body end
  end
  return nil
end
local function mep_csharp_ui_send(st, text)
  mep.terminal_write(st.console_buf, text .. '\n')
end
local function mep_csharp_ui_render_variables()
  local sb = mep_csharp_ui_sidebars
  if not sb then return end
  local st = mep_csharp_ui_state[mep.current_tab_id()]
  local widgets = {}
  for line in ((st and st.vars_text) or ''):gmatch('[^\n]+') do
    local name, ty, val = line:match('^([^\t]*)\t([^\t]*)\t(.*)$')
    if name then
      widgets[#widgets + 1] = {id = name, text = string.format('%s : %s = %s', name, ty, val),
        tooltip = 'inspect ' .. name,
        on_click = function()
          st.inspect_text = mep_csharp_ui_value_of(st, name) or (name .. ': no value recorded')
          mep.language_ui_render_textbox(sb.inspect, st.inspect_text, '')
          if st.inspect_buf then mep.reveal_buffer(st.inspect_buf, true) end
        end}
    end
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = '(no variables yet -- declare one in the console: var x = 1;)'} end
  mep.sidebar_set_sections(sb.variables, {{id = 'variables', title = '', collapsed = false, widgets = widgets}})
end
local function mep_csharp_ui_render_definitions()
  local sb = mep_csharp_ui_sidebars
  if not sb then return end
  local st = mep_csharp_ui_state[mep.current_tab_id()]
  local widgets = {}
  for line in ((st and st.defs_text) or ''):gmatch('[^\n]+') do
    local kind, name, file, lnum, input = line:match('^([^\t]*)\t([^\t]*)\t([^\t]*)\t(%d+)\t(%d+)$')
    if kind then
      local where = file ~= '' and ((file:match('[^/]+$') or file) .. ':' .. lnum) or ('[' .. input .. ']')
      -- A type's members (Point.Dist(), a constructor) sit under it.
      local head = name:match('^[^(]*') or name
      local indent = (kind == 'ctor' or (kind ~= 'using' and head:find('.', 1, true))) and '  ' or ''
      widgets[#widgets + 1] = {id = 'd' .. #widgets, text = string.format('%s%s %s  %s', indent, kind, name, where),
        tooltip = file ~= '' and ('go to ' .. file .. ':' .. lnum) or 'its source, in the Help tab',
        on_click = function()
          if file ~= '' and mep.fs_exists and mep.fs_exists(file) and mep.pane_focus(st.editor_pane) then
            mep.open(file)
            mep.set_cursor(tonumber(lnum), 1)
          elseif kind == 'using' then
            mep_csharp_ui_send(st, ':help ' .. name)
          else
            local short = name:match('^([%w_%.]+)') or name
            mep_csharp_ui_send(st, ':help ' .. short)
          end
        end}
    end
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = '(nothing defined in this session yet -- a class, record, method or using)'} end
  mep.sidebar_set_sections(sb.definitions, {{id = 'definitions', title = '', collapsed = false, widgets = widgets}})
end
local function mep_csharp_ui_render_history()
  local sb = mep_csharp_ui_sidebars
  if not sb then return end
  local st = mep_csharp_ui_state[mep.current_tab_id()]
  local widgets = {}
  for i = (st and #st.history or 0), 1, -1 do
    local code = st.history[i]
    widgets[#widgets + 1] = {id = 'h' .. i, text = code, wrap = true, wrap_indent = 0,
      on_click = function() mep_csharp_ui_send(st, code) end}
  end
  if #widgets == 0 then widgets[1] = {id = 'empty', text = '(no history yet -- evaluate something in the console)'} end
  mep.sidebar_set_sections(sb.history, {{id = 'history', title = '', collapsed = false, widgets = widgets}})
end
local function mep_csharp_ui_render_all()
  local sb = mep_csharp_ui_sidebars
  if not sb then return end
  local st = mep_csharp_ui_state[mep.current_tab_id()]
  mep_csharp_ui_render_variables()
  mep_csharp_ui_render_definitions()
  mep_csharp_ui_render_history()
  mep.language_ui_render_textbox(sb.inspect, st and st.inspect_text, '(nothing inspected yet -- click a Variables row)')
  mep.language_ui_render_textbox(sb.session, st and st.session_text, '(nothing kept yet: every input that compiles and runs is kept here, as one submission)')
  mep.language_ui_render_textbox(sb.help, st and st.help_text, '(no help viewed yet -- gh over a name, or :help string.Split in the console)')
end
-- <leader>re: the console finds the top-level statement or member around
-- the cursor itself, with Roslyn's parser, in a copy of the buffer.
function mep.csharp_ui_eval_block()
  local st = mep_csharp_ui_state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then
    mep.notify('No C# console in this tab -- <leader>uu opens one', 'warn')
    return
  end
  local n = mep.line_count()
  local lines = {}
  for i = 1, n do lines[i] = mep.get_line(i) or '' end
  mep.language_ui_write_file(st.eval_path, table.concat(lines, '\n') .. '\n')
  local row = mep.cursor()
  local fname = mep.filename()
  local src = (fname and fname ~= '') and mep_lsp_abspath(fname) or nil
  mep_csharp_ui_send(st, ':evalat ' .. st.eval_path .. ' ' .. row .. (src and (' ' .. src) or ''))
end
mep.language_ui_eval_handlers.csharp = mep.csharp_ui_eval_block
function mep.csharp_ui_open()
  local session_dir = mep.language_ui_session_dir('csharpui')
  local paths = {
    vars = session_dir .. '/vars.txt', values = session_dir .. '/values.txt', defs = session_dir .. '/defs.txt',
    session = session_dir .. '/session.csx', history = session_dir .. '/history.txt', help = session_dir .. '/help.txt',
  }
  for _, p in pairs(paths) do mep.language_ui_write_file(p, '') end
  local src_path = session_dir .. '/Program.cs'
  local proj_path = session_dir .. '/mep-csharp-repl.csproj'
  mep.language_ui_write_file(src_path, mep_csharp_ui_repl_source)
  mep.language_ui_write_file(proj_path, mep_csharp_ui_project)
  local argv = {'sh', '-c', mep_csharp_ui_launcher, 'mep-csharp-ui', src_path, proj_path, session_dir,
    mep.opt.csharp_ui_rlwrap and '1' or '0'}
  local layout = mep.language_ui_layout({
    info_share = mep.opt.csharp_ui_info_share, bottom_share = mep.opt.csharp_ui_bottom_share,
    bottom_info_share = mep.opt.csharp_ui_bottom_info_share,
    console = function() mep.terminal_here_argv(argv, 'csharp') end,
  })
  local tid = mep.current_tab_id()
  mep_csharp_ui_ensure_sidebars()
  local sb = mep_csharp_ui_sidebars
  local st = {
    paths = paths, console_buf = layout.console_buf, editor_pane = layout.editor_pane,
    eval_path = session_dir .. '/eval.cs',
    vars_text = '', values_text = '', defs_text = '', session_text = '', help_text = '', inspect_text = '',
    history = {}, history_raw = '', help_buf = nil, inspect_buf = nil,
  }
  mep_csharp_ui_state[tid] = st
  mep_csharp_ui_render_all()
  local bufs = mep.language_ui_open_tabs(layout.top_pane, {sb.variables, sb.definitions, sb.inspect, sb.session, sb.history})
  local help_bufs = mep.language_ui_open_tabs(layout.bottom_pane, {sb.help})
  st.help_buf = help_bufs[sb.help]
  st.inspect_buf = bufs[sb.inspect]
  mep.pane_focus(layout.editor_pane)
  mep.notify('C# language UI: console below (Roslyn submissions; state persists); Variables/Definitions/Inspect/Session/History top-right, Help bottom-right; <leader>re sends the statement or member at the cursor, <leader>rr runs the file')
  -- run_source: <leader>rr / the Run button while the mode is open -- writes
  -- the buffer, then runs it in the console (:run).
  local function run_source(fname)
    mep.cmd('write')
    mep_csharp_ui_send(st, ':run ' .. mep_lsp_abspath(fname))
  end
  return {
    run_source = run_source,
    close = function()
      mep_csharp_ui_state[tid] = nil
      mep.language_ui_teardown(layout)
      mep.notify('C# language UI mode closed')
    end,
  }
end
mep.language_ui_modes.csharp = {open = mep.csharp_ui_open}
-- gh: the documentation of the name under the cursor -- string, string.Split,
-- Console.WriteLine, or xs.Select (the variable's type from the session) --
-- from the reference pack's XML docs, or the session's own source.
function mep.csharp_ui_help_at_cursor()
  local st = mep_csharp_ui_state[mep.current_tab_id()]
  if not st or not mep.is_terminal_buffer(st.console_buf) then return false end
  local sym = mep.help_symbol_at_cursor('[%w_%.]')
  if sym then sym = sym:gsub('^[%.]+', ''):gsub('[%.]+$', '') end
  if not sym or sym == '' then mep.notify('gh: no C# name under the cursor', 'warn') return true end
  mep_csharp_ui_send(st, ':help ' .. sym)
  if st.help_buf then mep.jump_to_buffer(st.help_buf) end
  return true
end
mep.help_register_provider('csharp', mep.csharp_ui_help_at_cursor)
do
  local last_poll = 0
  mep.on_frame(function()
    local tid = mep.current_tab_id()
    local st = mep_csharp_ui_state[tid]
    if not st then return end
    local now = mep.now()
    if now - last_poll < mep.opt.language_ui_poll_interval then return end
    last_poll = now
    local changed, help_changed = false, false
    local function take(key, field)
      local text = mep.language_ui_read_file(st.paths[key])
      if text and text ~= st[field] then
        st[field] = text
        changed = true
        return true
      end
      return false
    end
    take('vars', 'vars_text')
    take('values', 'values_text')
    take('defs', 'defs_text')
    take('session', 'session_text')
    help_changed = take('help', 'help_text')
    local hist_raw = mep.language_ui_read_file(st.paths.history)
    if hist_raw and hist_raw ~= st.history_raw then
      st.history_raw = hist_raw
      local hist = {}
      for entry in hist_raw:gmatch('[^\30]+') do hist[#hist + 1] = entry end
      st.history = hist
      changed = true
    end
    if changed then mep_csharp_ui_render_all() end
    if help_changed and st.help_buf then mep.reveal_buffer(st.help_buf, true) end
  end)
end
)MEPLUA";

}  // namespace mep_csharp_ui

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

// The mode's Lua chunk: the program and project as Lua strings, then the mode.
inline std::string BuiltinLanguageUiCsharp() {
    return std::string("local mep_csharp_ui_repl_source = [==[\n") + mep_csharp_ui::kReplProgram + "]==]\n" +
           "local mep_csharp_ui_project = [==[\n" + mep_csharp_ui::kReplProject + "]==]\n" + mep_csharp_ui::kLua;
}
