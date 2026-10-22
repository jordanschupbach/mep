// mep's C# vocabulary generator: reads the .NET reference assemblies and
// their XML documentation with the SDK's own Roslyn and writes
// src/csharp_lsp_bcl.cpp -- the table mep-csharp-lsp completes, hovers and
// signs from, so neither building nor running mep needs .NET.
//
//   csharp-vocab REF_DIR OUT.cpp
//
// REF_DIR is packs/Microsoft.NETCore.App.Ref/<version>/ref/net<N>
// (`just csharp-vocab` finds it). Only public (and protected) API of the
// namespaces in `Namespaces` below is kept; doc comments are reduced to
// their <summary>, as plain text.
using System.Text;
using System.Xml.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

/** The members of one reference assembly's XML doc file, by documentation id. */
sealed class XmlDocs : DocumentationProvider {
    readonly Dictionary<string, string> _members = new();
    readonly string _path;

    public XmlDocs(string path) {
        _path = path;
        var doc = XDocument.Load(path);
        foreach (var m in doc.Descendants("member")) {
            var name = (string?)m.Attribute("name");
            if (name != null) _members[name] = m.ToString();
        }
    }

    protected override string? GetDocumentationForSymbol(string documentationMemberID,
                                                         System.Globalization.CultureInfo preferredCulture,
                                                         CancellationToken cancellationToken = default) =>
        _members.TryGetValue(documentationMemberID, out var xml) ? xml : null;

    public override bool Equals(object? obj) => obj is XmlDocs other && other._path == _path;
    public override int GetHashCode() => _path.GetHashCode();
}

static class Vocab {
    // The namespaces everyday C# code imports. A namespace listed here brings
    // all its types; nested namespaces are listed one by one.
    static readonly HashSet<string> Namespaces = new() {
        "System", "System.Buffers", "System.Collections", "System.Collections.Concurrent",
        "System.Collections.Generic", "System.Collections.Immutable", "System.Collections.ObjectModel",
        "System.Collections.Specialized", "System.ComponentModel", "System.ComponentModel.DataAnnotations",
        "System.Diagnostics", "System.Diagnostics.CodeAnalysis", "System.Globalization", "System.IO",
        "System.IO.Compression", "System.Linq", "System.Net",
        "System.Net.Http", "System.Net.Http.Headers", "System.Net.Http.Json", 
        "System.Numerics", "System.Reflection", "System.Runtime.CompilerServices", "System.Runtime.InteropServices",
        "System.Security", "System.Text",
        "System.Text.Encodings.Web", "System.Text.Json", "System.Text.Json.Nodes", "System.Text.Json.Serialization",
        "System.Text.RegularExpressions", "System.Threading", "System.Threading.Channels", "System.Threading.Tasks",
        "System.Timers", "System.Xml.Linq", 
        "System.Resources",
        "System.Runtime", "System.Runtime.Versioning", 
        "Microsoft.Win32", 
        
    };

    static readonly SymbolDisplayFormat Full = new(
        globalNamespaceStyle: SymbolDisplayGlobalNamespaceStyle.Omitted,
        typeQualificationStyle: SymbolDisplayTypeQualificationStyle.NameAndContainingTypesAndNamespaces,
        genericsOptions: SymbolDisplayGenericsOptions.IncludeTypeParameters,
        miscellaneousOptions: SymbolDisplayMiscellaneousOptions.UseSpecialTypes |
                              SymbolDisplayMiscellaneousOptions.IncludeNullableReferenceTypeModifier |
                              SymbolDisplayMiscellaneousOptions.ExpandNullable);

    // A type's own name: never a keyword ("System.String", not "string"), so names and owners are full names.
    static readonly SymbolDisplayFormat Name = new(
        globalNamespaceStyle: SymbolDisplayGlobalNamespaceStyle.Omitted,
        typeQualificationStyle: SymbolDisplayTypeQualificationStyle.NameAndContainingTypesAndNamespaces,
        genericsOptions: SymbolDisplayGenericsOptions.IncludeTypeParameters);

    static readonly SymbolDisplayFormat Short = new(
        globalNamespaceStyle: SymbolDisplayGlobalNamespaceStyle.Omitted,
        typeQualificationStyle: SymbolDisplayTypeQualificationStyle.NameAndContainingTypes,
        genericsOptions: SymbolDisplayGenericsOptions.IncludeTypeParameters |
                         SymbolDisplayGenericsOptions.IncludeTypeConstraints |
                         SymbolDisplayGenericsOptions.IncludeVariance,
        memberOptions: SymbolDisplayMemberOptions.IncludeParameters | SymbolDisplayMemberOptions.IncludeType |
                       SymbolDisplayMemberOptions.IncludeRef | SymbolDisplayMemberOptions.IncludeModifiers |
                       SymbolDisplayMemberOptions.IncludeConstantValue,
        parameterOptions: SymbolDisplayParameterOptions.IncludeType | SymbolDisplayParameterOptions.IncludeName |
                          SymbolDisplayParameterOptions.IncludeParamsRefOut |
                          SymbolDisplayParameterOptions.IncludeDefaultValue |
                          SymbolDisplayParameterOptions.IncludeExtensionThis,
        propertyStyle: SymbolDisplayPropertyStyle.ShowReadWriteDescriptor,
        kindOptions: SymbolDisplayKindOptions.IncludeTypeKeyword,
        miscellaneousOptions: SymbolDisplayMiscellaneousOptions.UseSpecialTypes |
                              SymbolDisplayMiscellaneousOptions.EscapeKeywordIdentifiers |
                              SymbolDisplayMiscellaneousOptions.IncludeNullableReferenceTypeModifier);

    static readonly SymbolDisplayFormat Params = new(
        typeQualificationStyle: SymbolDisplayTypeQualificationStyle.NameAndContainingTypes,
        genericsOptions: SymbolDisplayGenericsOptions.IncludeTypeParameters,
        miscellaneousOptions: SymbolDisplayMiscellaneousOptions.UseSpecialTypes |
                              SymbolDisplayMiscellaneousOptions.EscapeKeywordIdentifiers |
                              SymbolDisplayMiscellaneousOptions.IncludeNullableReferenceTypeModifier,
        parameterOptions: SymbolDisplayParameterOptions.IncludeType | SymbolDisplayParameterOptions.IncludeName |
                          SymbolDisplayParameterOptions.IncludeParamsRefOut |
                          SymbolDisplayParameterOptions.IncludeDefaultValue |
                          SymbolDisplayParameterOptions.IncludeExtensionThis);

    static string C(string s) {
        var sb = new StringBuilder("\"");
        foreach (var ch in s) {
            if (ch == '\\') sb.Append("\\\\");
            else if (ch == '"') sb.Append("\\\"");
            else if (ch == '\n') sb.Append("\\n");
            else if (ch < 0x20) sb.Append(' ');
            else if (ch == '?') sb.Append("\\?");  // no trigraphs
            else sb.Append(ch);
        }
        return sb.Append('"').ToString();
    }

    static string CrefName(string cref) {
        // "T:System.String" -> "String"; "M:System.String.Split(System.Char)" -> "String.Split"
        var s = cref.Length > 2 && cref[1] == ':' ? cref[2..] : cref;
        var paren = s.IndexOf('(');
        if (paren >= 0) s = s[..paren];
        var parts = s.Split('.');
        var name = parts.Length >= 2 && char.IsUpper(cref.Length > 0 ? cref[0] : 'T') && cref.StartsWith("T:")
            ? parts[^1] : string.Join(".", parts.Skip(Math.Max(0, parts.Length - 2)));
        var tick = name.IndexOf('`');
        return tick >= 0 ? name[..tick] : name;
    }

    static string PlainText(XElement e) {
        var sb = new StringBuilder();
        foreach (var node in e.Nodes()) {
            if (node is XText t) sb.Append(t.Value);
            else if (node is XElement x) {
                switch (x.Name.LocalName) {
                    case "see":
                    case "seealso":
                        var cref = (string?)x.Attribute("cref");
                        var lang = (string?)x.Attribute("langword");
                        if (!x.IsEmpty && x.Value.Length > 0) sb.Append(x.Value);
                        else if (cref != null) sb.Append('`').Append(CrefName(cref)).Append('`');
                        else if (lang != null) sb.Append('`').Append(lang).Append('`');
                        break;
                    case "paramref":
                    case "typeparamref":
                        sb.Append('`').Append((string?)x.Attribute("name")).Append('`');
                        break;
                    case "c":
                    case "code":
                        sb.Append('`').Append(x.Value).Append('`');
                        break;
                    default:
                        sb.Append(PlainText(x));
                        break;
                }
            }
        }
        return sb.ToString();
    }

    static string Summary(ISymbol s) {
        var xml = s.GetDocumentationCommentXml();
        if (string.IsNullOrEmpty(xml)) return "";
        try {
            var doc = XElement.Parse(xml);
            var summary = doc.Element("summary");
            if (summary == null) return "";
            var text = System.Text.RegularExpressions.Regex.Replace(PlainText(summary), @"\s+", " ").Trim();
            if (text.Length > 300) text = text[..297] + "...";
            return text;
        } catch {
            return "";
        }
    }

    static bool Visible(ISymbol s) =>
        s.DeclaredAccessibility is Accessibility.Public or Accessibility.Protected or Accessibility.ProtectedOrInternal;

    static int Main(string[] args) {
        if (args.Length != 2) {
            Console.Error.WriteLine("usage: csharp-vocab REF_DIR OUT.cpp");
            return 2;
        }
        var refDir = args[0];
        var refs = new List<MetadataReference>();
        foreach (var dll in Directory.GetFiles(refDir, "*.dll").OrderBy(p => p, StringComparer.Ordinal)) {
            var xml = Path.ChangeExtension(dll, ".xml");
            refs.Add(File.Exists(xml)
                ? MetadataReference.CreateFromFile(dll, documentation: new XmlDocs(xml))
                : MetadataReference.CreateFromFile(dll));
        }
        var comp = CSharpCompilation.Create("vocab", references: refs);
        var types = new List<INamedTypeSymbol>();
        void Walk(INamespaceSymbol ns) {
            foreach (var t in ns.GetTypeMembers()) AddType(t);
            foreach (var child in ns.GetNamespaceMembers()) Walk(child);
        }
        void AddType(INamedTypeSymbol t) {
            if (!Visible(t)) return;
            var ns = t.ContainingNamespace.ToDisplayString();
            if (!Namespaces.Contains(ns)) return;
            if (t.GetAttributes().Any(a => a.AttributeClass?.Name == "ObsoleteAttribute" &&
                                           a.ConstructorArguments.Length >= 2 &&
                                           a.ConstructorArguments[1].Value is true)) return;
            types.Add(t);
            foreach (var nested in t.GetTypeMembers()) AddType(nested);
        }
        Walk(comp.GlobalNamespace);
        // One copy of each type (type-forwarding facades name the same symbol again).
        types = types.GroupBy(t => t.ToDisplayString(Name)).Select(g => g.First())
                     .OrderBy(t => t.ToDisplayString(Name), StringComparer.Ordinal).ToList();

        var o = new StringBuilder();
        var sdk = Path.GetFileName(Path.GetDirectoryName(Path.GetDirectoryName(refDir.TrimEnd('/'))));
        int memberCount = 0;
        var members = new StringBuilder();
        var typeRows = new StringBuilder();
        foreach (var t in types) {
            var kind = t.TypeKind switch {
                TypeKind.Class => t.IsRecord ? "record" : (t.IsStatic ? "static class" : "class"),
                TypeKind.Struct => t.IsRecord ? "record struct" : "struct",
                TypeKind.Interface => "interface",
                TypeKind.Enum => "enum",
                TypeKind.Delegate => "delegate",
                _ => "class",
            };
            var name = t.ToDisplayString(Name);
            var baseName = t.BaseType == null || t.TypeKind is TypeKind.Enum or TypeKind.Delegate
                ? (t.TypeKind == TypeKind.Enum ? (t.EnumUnderlyingType?.ToDisplayString(Full) ?? "") : "")
                : t.BaseType.ToDisplayString(Full);
            var ifaces = string.Join(";", t.AllInterfaces.Where(Visible).Select(i => i.ToDisplayString(Full)));
            var sig = t.TypeKind == TypeKind.Delegate && t.DelegateInvokeMethod is { } inv
                ? $"delegate {inv.ReturnType.ToDisplayString(Params)} {t.ToDisplayString(Params)}({string.Join(", ", inv.Parameters.Select(p => p.ToDisplayString(Params)))})"
                : t.ToDisplayString(Short);
            typeRows.Append($"    {{{C(name)}, {C(kind)}, {C(baseName)}, {C(ifaces)}, {C(sig)}, {C(Summary(t))}}},\n");

            foreach (var m in t.GetMembers()) {
                if (!Visible(m) || m.IsImplicitlyDeclared && m is not IMethodSymbol { MethodKind: MethodKind.Constructor }) continue;
                int mk;
                string type = "";
                string param = "";
                switch (m) {
                    case IMethodSymbol ms when ms.MethodKind == MethodKind.Ordinary:
                        mk = 0;
                        type = ms.ReturnType.ToDisplayString(Full);
                        param = "(" + string.Join(", ", ms.Parameters.Select(p => p.ToDisplayString(Params))) + ")";
                        break;
                    case IMethodSymbol ms when ms.MethodKind == MethodKind.Constructor && !ms.IsStatic:
                        mk = 1;
                        param = "(" + string.Join(", ", ms.Parameters.Select(p => p.ToDisplayString(Params))) + ")";
                        break;
                    case IPropertySymbol ps:
                        mk = ps.IsIndexer ? 6 : 2;
                        type = ps.Type.ToDisplayString(Full);
                        if (ps.IsIndexer) param = "[" + string.Join(", ", ps.Parameters.Select(p => p.ToDisplayString(Params))) + "]";
                        break;
                    case IFieldSymbol fs:
                        mk = t.TypeKind == TypeKind.Enum ? 5 : 3;
                        type = fs.Type.ToDisplayString(Full);
                        break;
                    case IEventSymbol es:
                        mk = 4;
                        type = es.Type.ToDisplayString(Full);
                        break;
                    default:
                        continue;
                }
                int flags = (m.IsStatic ? 1 : 0) | (m is IMethodSymbol { IsExtensionMethod: true } ? 2 : 0) |
                            (m is IFieldSymbol { IsConst: true } ? 4 : 0) |
                            (m.DeclaredAccessibility is Accessibility.Protected or Accessibility.ProtectedOrInternal ? 8 : 0);
                var display = m.ToDisplayString(Short);
                var mname = mk == 1 ? t.Name : m.Name;
                members.Append($"    {{{C(name)}, {C(mname)}, {mk}, {flags}, {C(display)}, {C(param)}, {C(type)}, {C(Summary(m))}}},\n");
                memberCount++;
            }
        }
        o.Append($@"// GENERATED by tools/csharp_vocab (Program.cs) from the .NET reference assemblies
// and their XML docs (Microsoft.NETCore.App.Ref {sdk}) -- do not edit; `just csharp-vocab`
// regenerates it.
//
// The public API of the namespaces everyday C# imports: every type with its
// base type and interfaces, every member with its signature, type and the
// <summary> of its doc comment. Read by mep's own C# language server
// (csharp_lsp*.cpp) for completion, hover and signature help.
//
// {types.Count} types, {memberCount} members.

#include ""csharp_lsp_bcl.h""

namespace {{

const CsBclType kTypes[] = {{
");
        o.Append(typeRows);
        o.Append("};\n\nconst CsBclMember kMembers[] = {\n");
        o.Append(members);
        o.Append($@"}};

}}  // namespace

const char *CsBclVersion() {{ return {C(sdk)}; }}

const CsBclType *CsBclTypes(size_t *count) {{
    *count = sizeof(kTypes) / sizeof(kTypes[0]);
    return kTypes;
}}

const CsBclMember *CsBclMembers(size_t *count) {{
    *count = sizeof(kMembers) / sizeof(kMembers[0]);
    return kMembers;
}}
");
        File.WriteAllText(args[1], o.ToString(), new UTF8Encoding(false));
        Console.Error.WriteLine($"{types.Count} types, {memberCount} members, {o.Length / 1024} KiB");
        return 0;
    }
}
