//go:build ignore

// gen_go_vocab generates src/go_lsp_vocab_data.cpp: the Go standard
// library's vocabulary for mep-go-lsp (hover, completion, signature help).
//
//	just go-lsp-vocab
//	go run tools/gen_go_vocab.go > src/go_lsp_vocab_data.cpp
//
// It reads $(go env GOROOT)/src with go/build (the files a linux/amd64
// build would compile) and go/doc, and writes one row per package and per
// exported const, var, func, type, method (Type.Method), struct field and
// interface method (Type.Name): its kind, a signature as go/printer prints
// the declaration (bodies dropped, long type definitions cut to their
// head), the doc text as go/doc renders it (capped at a paragraph boundary
// near 1,500 bytes) and whether it has a "Deprecated:" paragraph. The
// predeclared identifiers come from the documentation-only package
// "builtin". Go's documentation is BSD-licensed; the notice is embedded in
// the output.
package main

import (
	"bytes"
	"fmt"
	"go/ast"
	"go/build"
	"go/doc"
	"go/parser"
	"go/printer"
	"go/token"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
)

type row struct {
	pkg, name  string
	kind       byte
	sig, doc   string
	deprecated bool
}

var (
	rows []row
	fset = token.NewFileSet()
)

const maxDoc = 1500

const license = `Copyright 2009 The Go Authors.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

   * Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
   * Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following disclaimer
in the documentation and/or other materials provided with the
distribution.
   * Neither the name of Google LLC nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.`

func node(n any) string {
	var b bytes.Buffer
	cfg := printer.Config{Mode: printer.UseSpaces | printer.TabIndent, Tabwidth: 8}
	if err := cfg.Fprint(&b, fset, n); err != nil {
		return ""
	}
	return b.String()
}

func isDeprecated(raw string) bool {
	for _, para := range strings.Split(raw, "\n\n") {
		if strings.HasPrefix(strings.TrimSpace(para), "Deprecated:") {
			return true
		}
	}
	return false
}

// text renders a doc comment as plain text, capped at a paragraph boundary.
func text(p *doc.Package, raw string) string {
	t := strings.TrimRight(string(p.Text(raw)), "\n")
	if len(t) <= maxDoc {
		return t
	}
	cut := strings.LastIndex(t[:maxDoc], "\n\n")
	if cut < maxDoc/3 {
		cut = strings.LastIndex(t[:maxDoc], "\n")
	}
	if cut < maxDoc/3 {
		cut = maxDoc
		for cut > 0 && t[cut]&0xC0 == 0x80 { // not inside a UTF-8 sequence
			cut--
		}
	}
	return strings.TrimRight(t[:cut], "\n ") + "\n\n..."
}

func add(p *doc.Package, pkg, name string, kind byte, sig, raw string) {
	rows = append(rows, row{pkg, name, kind, strings.TrimSpace(sig), text(p, raw), isDeprecated(raw)})
}

func funcSig(d *ast.FuncDecl) string {
	c := *d
	c.Body, c.Doc = nil, nil
	return node(&c)
}

// typeSig prints `type T ...`, cut to its head when the definition is long.
func typeSig(decl *ast.GenDecl, s *ast.TypeSpec) string {
	c := *s
	c.Doc, c.Comment = nil, nil
	full := "type " + node(&c)
	if strings.Count(full, "\n") < 12 {
		return full
	}
	head := "type " + s.Name.Name
	if s.TypeParams != nil {
		tp := node(&ast.FuncType{Func: token.NoPos, TypeParams: s.TypeParams, Params: &ast.FieldList{}})
		// "func[T any]()" -> "[T any]"
		tp = strings.TrimPrefix(tp, "func")
		tp = strings.TrimSuffix(tp, "()")
		head += tp
	}
	if s.Assign.IsValid() {
		head += " ="
	}
	switch s.Type.(type) {
	case *ast.StructType:
		return head + " struct { ... }"
	case *ast.InterfaceType:
		return head + " interface { ... }"
	}
	return full
}

func valueSigs(p *doc.Package, pkg string, v *doc.Value) {
	kw := "var"
	kind := byte('v')
	if v.Decl.Tok == token.CONST {
		kw, kind = "const", 'c'
	}
	for _, spec := range v.Decl.Specs {
		vs := spec.(*ast.ValueSpec)
		c := *vs
		c.Doc, c.Comment = nil, nil
		sig := kw + " " + node(&c)
		raw := v.Doc
		if vs.Doc != nil {
			raw = vs.Doc.Text()
		} else if vs.Comment != nil && v.Doc == "" {
			raw = vs.Comment.Text()
		} else if vs.Comment != nil {
			raw = vs.Comment.Text() + "\n" + v.Doc
		}
		for _, n := range vs.Names {
			if !n.IsExported() && pkg != "builtin" {
				continue
			}
			add(p, pkg, n.Name, kind, sig, raw)
		}
	}
}

func fieldDoc(f *ast.Field) string {
	if f.Doc != nil {
		return f.Doc.Text()
	}
	if f.Comment != nil {
		return f.Comment.Text()
	}
	return ""
}

func members(p *doc.Package, pkg, tname string, s *ast.TypeSpec) {
	switch t := s.Type.(type) {
	case *ast.StructType:
		for _, f := range t.Fields.List {
			ft := node(f.Type)
			if len(f.Names) == 0 { // embedded
				name := ft
				name = strings.TrimPrefix(name, "*")
				if i := strings.IndexAny(name, "["); i >= 0 {
					name = name[:i]
				}
				if i := strings.LastIndex(name, "."); i >= 0 {
					name = name[i+1:]
				}
				if ast.IsExported(name) {
					add(p, pkg, tname+"."+name, 'F', ft, fieldDoc(f))
				}
				continue
			}
			for _, n := range f.Names {
				if n.IsExported() {
					add(p, pkg, tname+"."+n.Name, 'F', n.Name+" "+ft, fieldDoc(f))
				}
			}
		}
	case *ast.InterfaceType:
		for _, f := range t.Methods.List {
			if len(f.Names) == 0 {
				continue // embedded interface or type set
			}
			ft, ok := f.Type.(*ast.FuncType)
			if !ok {
				continue
			}
			sig := strings.TrimPrefix(node(ft), "func")
			for _, n := range f.Names {
				if n.IsExported() {
					add(p, pkg, tname+"."+n.Name, 'm', n.Name+sig, fieldDoc(f))
				}
			}
		}
	}
}

var docTypes = map[string]bool{"Type": true, "Type1": true, "IntegerType": true, "FloatType": true,
	"ComplexType": true, "TypeOrExpr": true}

func pkgDoc(dir, path string) {
	bp, err := build.Default.ImportDir(dir, build.ImportComment)
	if err != nil || (bp.Name == "main" && path != "builtin") {
		return
	}
	files := append([]string{}, bp.GoFiles...)
	files = append(files, bp.CgoFiles...)
	if path == "builtin" {
		files = append(files, bp.IgnoredGoFiles...)
	}
	var parsed []*ast.File
	for _, f := range files {
		af, err := parser.ParseFile(fset, filepath.Join(dir, f), nil, parser.ParseComments)
		if err != nil {
			fmt.Fprintln(os.Stderr, "skip", path, err)
			return
		}
		parsed = append(parsed, af)
	}
	if len(parsed) == 0 {
		return
	}
	mode := doc.Mode(0)
	if path == "builtin" {
		mode = doc.AllDecls
	}
	p, err := doc.NewFromFiles(fset, parsed, path, mode)
	if err != nil {
		fmt.Fprintln(os.Stderr, "skip", path, err)
		return
	}
	add(p, path, "", 'p', "package "+p.Name, p.Doc)
	for _, v := range p.Consts {
		valueSigs(p, path, v)
	}
	for _, v := range p.Vars {
		valueSigs(p, path, v)
	}
	for _, f := range p.Funcs {
		add(p, path, f.Name, 'f', funcSig(f.Decl), f.Doc)
	}
	for _, t := range p.Types {
		if path == "builtin" && docTypes[t.Name] {
			// A documentation-only type; append, make, new, nil... are
			// attached to it as its "constructors" and values.
			for _, v := range t.Vars {
				valueSigs(p, path, v)
			}
			for _, f := range t.Funcs {
				add(p, path, f.Name, 'f', funcSig(f.Decl), f.Doc)
			}
			continue
		}
		var spec *ast.TypeSpec
		for _, s := range t.Decl.Specs {
			if ts := s.(*ast.TypeSpec); ts.Name.Name == t.Name {
				spec = ts
			}
		}
		if spec == nil {
			continue
		}
		add(p, path, t.Name, 't', typeSig(t.Decl, spec), t.Doc)
		for _, v := range t.Consts {
			valueSigs(p, path, v)
		}
		for _, v := range t.Vars {
			valueSigs(p, path, v)
		}
		for _, f := range t.Funcs {
			add(p, path, f.Name, 'f', funcSig(f.Decl), f.Doc)
		}
		for _, m := range t.Methods {
			if m.Level > 0 {
				continue // promoted from an embedded field
			}
			add(p, path, t.Name+"."+m.Name, 'm', funcSig(m.Decl), m.Doc)
		}
		members(p, path, t.Name, spec)
	}
}

func cstr(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '\\':
			b.WriteString(`\\`)
		case c == '"':
			b.WriteString(`\"`)
		case c == '\n':
			b.WriteString(`\n`)
		case c == '\t':
			b.WriteString(`\t`)
		case c == '?':
			b.WriteString(`\?`) // never a trigraph
		case c < 0x20 || c >= 0x7f:
			fmt.Fprintf(&b, `\%03o`, c) // octal stops at three digits
		default:
			b.WriteByte(c)
		}
	}
	b.WriteByte('"')
	return b.String()
}

func main() {
	root := filepath.Join(runtime.GOROOT(), "src")
	build.Default.GOOS, build.Default.GOARCH = "linux", "amd64"
	build.Default.CgoEnabled = true
	filepath.WalkDir(root, func(dir string, d fs.DirEntry, err error) error {
		if err != nil || !d.IsDir() {
			return nil
		}
		rel, _ := filepath.Rel(root, dir)
		rel = filepath.ToSlash(rel)
		if rel == "." {
			return nil
		}
		for _, part := range strings.Split(rel, "/") {
			if part == "internal" || part == "vendor" || part == "testdata" || part == "cmd" ||
				strings.HasPrefix(part, ".") || strings.HasPrefix(part, "_") {
				return filepath.SkipDir
			}
		}
		pkgDoc(dir, rel)
		return nil
	})
	sort.Slice(rows, func(i, j int) bool {
		if rows[i].pkg != rows[j].pkg {
			return rows[i].pkg < rows[j].pkg
		}
		return rows[i].name < rows[j].name
	})
	// One row per (pkg, name): the first wins.
	out := rows[:0]
	for i, r := range rows {
		if i > 0 && r.pkg == out[len(out)-1].pkg && r.name == out[len(out)-1].name {
			continue
		}
		out = append(out, r)
	}
	rows = out

	w := os.Stdout
	fmt.Fprintf(w, "// Generated by tools/gen_go_vocab.go (just go-lsp-vocab) -- do not edit.\n")
	fmt.Fprintf(w, "// The Go %s standard library: exported names, signatures and documentation\n", runtime.Version())
	fmt.Fprintf(w, "// read with go/doc from the Go source tree, which comes with this notice:\n//\n")
	for _, l := range strings.Split(license, "\n") {
		fmt.Fprintf(w, "%s\n", strings.TrimRight("// "+l, " "))
	}
	fmt.Fprintf(w, "\n#include \"go_lsp_vocab.h\"\n\n")
	fmt.Fprintf(w, "extern const GoVocabEntry kGoVocab[];\nextern const size_t kGoVocabCount;\n\n")
	fmt.Fprintf(w, "const GoVocabEntry kGoVocab[] = {\n")
	for _, r := range rows {
		dep := "false"
		if r.deprecated {
			dep = "true"
		}
		fmt.Fprintf(w, "    {%s, %s, GoVocabKind('%c'), %s,\n     %s, %s},\n", cstr(r.pkg), cstr(r.name), r.kind,
			cstr(r.sig), cstr(r.doc), dep)
	}
	fmt.Fprintf(w, "};\n\nconst size_t kGoVocabCount = sizeof(kGoVocab) / sizeof(kGoVocab[0]);\n")
}
