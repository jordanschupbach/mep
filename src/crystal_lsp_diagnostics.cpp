// mep-crystal-lsp's diagnostics (CrLspDiagnostics in crystal_lsp.h).
//
// The syntax error and the warnings are the compiler's own (crystal_parser.h
// is a port of its parser). The lints are what one file shows without the
// compiler's type inference, and are kept quiet on code the compiler
// accepts: Crystal's own compiler and standard library raise none of the
// warnings (the unused-argument and unused-block-parameter hints aside).

#include <sys/stat.h>

#include <cctype>
#include <cstdio>

#include "crystal_lsp.h"
#include "crystal_lsp_analysis.h"

using namespace crlsp;

namespace {

bool Exists(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

bool IsDir(const std::string &p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string DirName(const std::string &path) {
    const size_t s = path.find_last_of('/');
    if (s == std::string::npos) return ".";
    return s == 0 ? "/" : path.substr(0, s);
}

// A relative require resolves the way Crystal's CrystalPath does it:
// `./x` is x.cr, or x/x.cr; `./x/*` and `./x/**` need the directory.
bool RequireResolves(const std::string &dir, const std::string &req) {
    std::string p = dir + "/" + req;
    if (req.size() >= 2 && req.compare(req.size() - 2, 2, "/*") == 0) return IsDir(dir + "/" + req.substr(0, req.size() - 2));
    if (req.size() >= 3 && req.compare(req.size() - 3, 3, "/**") == 0) return IsDir(dir + "/" + req.substr(0, req.size() - 3));
    if (req.size() > 3 && req.compare(req.size() - 3, 3, ".cr") == 0) return Exists(p) && !IsDir(p);
    if (Exists(p + ".cr")) return true;
    const size_t slash = req.find_last_of('/');
    const std::string base = slash == std::string::npos ? req : req.substr(slash + 1);
    return Exists(p + "/" + base + ".cr");
}

CrRange WordRange(const std::vector<std::string> &lines, int line, int col) {
    CrRange r;
    r.start_line = r.end_line = line;
    r.start_col = col;
    r.end_col = col;
    if (line >= 0 && line < static_cast<int>(lines.size())) {
        const std::string &s = lines[static_cast<size_t>(line)];
        int e = col;
        while (e < static_cast<int>(s.size()) && (std::isalnum(static_cast<unsigned char>(s[static_cast<size_t>(e)])) ||
                                                   s[static_cast<size_t>(e)] == '_')) {
            e++;
        }
        if (e == col && col < static_cast<int>(s.size())) e = col + 1;
        r.end_col = e;
    }
    return r;
}

}  // namespace

std::vector<CrLspDiagnostic> CrLspDiagnostics(const std::vector<std::string> &lines, const std::string &path) {
    std::vector<CrLspDiagnostic> out;
    std::unique_ptr<Analysis> a = Analyze(lines);
    for (const auto &w : a->warnings) {
        CrLspDiagnostic d;
        d.range = w.first;
        d.severity = CrLspSeverity::Warning;
        d.code = "parser-warning";
        d.message = w.second;
        out.push_back(d);
    }
    if (!a->ok) {
        CrLspDiagnostic d;
        d.range = WordRange(lines, a->error_line, a->error_col);
        d.severity = CrLspSeverity::Error;
        d.code = "syntax";
        d.message = a->error;
        out.push_back(d);
        return out;  // the lints wait for code that parses
    }

    auto scope_has_macro = [&](int s) {
        for (; s >= 0; s = a->scopes[static_cast<size_t>(s)].parent) {
            if (a->scopes[static_cast<size_t>(s)].has_macro) return true;
            const ScopeKind k = a->scopes[static_cast<size_t>(s)].kind;
            if (k != ScopeKind::Block && k != ScopeKind::Proc) break;
        }
        return false;
    };

    for (const Local &l : a->locals) {
        if (l.reads > 0 || l.name.empty() || l.name[0] == '_' || l.name[0] == '$') continue;
        if (scope_has_macro(l.scope)) continue;
        // Any block or proc inside the local's scope that holds macro code may read it.
        bool macro_inside = false;
        for (const Scope &sc : a->scopes) {
            if (sc.has_macro) {
                for (int p = sc.parent; p >= 0; p = a->scopes[static_cast<size_t>(p)].parent) {
                    if (p == l.scope) macro_inside = true;
                }
            }
        }
        if (macro_inside) continue;
        CrLspDiagnostic d;
        d.range = l.def_range;
        d.unnecessary = true;
        if (l.is_block_param) {
            d.severity = CrLspSeverity::Hint;
            d.code = "unused-block-parameter";
            d.message = "Unused block parameter '" + l.name + "' (name it _" + l.name + " or _ to say so)";
        } else if (l.is_param) {
            if (l.abstract_owner) continue;
            d.severity = CrLspSeverity::Hint;
            d.code = "unused-argument";
            d.message = "Unused argument '" + l.name + "' (name it _" + l.name + " to say so)";
        } else if (l.bind_kind == 0) {
            d.severity = CrLspSeverity::Warning;
            d.code = "unused-variable";
            d.message = "Useless assignment to variable '" + l.name + "'";
        } else {
            // One part of `a, b = ...`, a rescued exception, an `out` argument: often
            // left unread on purpose.
            d.severity = CrLspSeverity::Hint;
            d.code = "unused-variable";
            d.message = "Variable '" + l.name + "' is never read";
        }
        out.push_back(d);
    }

    for (const auto &sh : a->shadows) {
        CrLspDiagnostic d;
        d.range = sh.first;
        d.severity = CrLspSeverity::Hint;  // `x.try { |x| ... }` is idiomatic Crystal
        d.code = "shadowed-variable";
        d.message = "Block parameter '" + sh.second + "' shadows the local variable of the same name";
        out.push_back(d);
    }

    for (const CrRange &r : a->unreachable_ranges) {
        CrLspDiagnostic d;
        d.range = r;
        d.severity = CrLspSeverity::Warning;
        d.code = "unreachable-code";
        d.message = "Unreachable code";
        d.unnecessary = true;
        out.push_back(d);
    }

    if (!path.empty() && path[0] == '/') {
        const std::string dir = DirName(path);
        for (const Node *req : a->require_nodes) {
            const std::string &v = req->value;
            if (!(v.rfind("./", 0) == 0 || v.rfind("../", 0) == 0)) continue;
            if (RequireResolves(dir, v)) continue;
            CrLspDiagnostic d;
            d.range = a->NodeRange(req);
            d.severity = CrLspSeverity::Error;
            d.code = "require-not-found";
            d.message = "can't find file '" + v + "' relative to '" + dir + "'";
            out.push_back(d);
        }
    }
    return out;
}

std::string CrLspFormatDiagnostic(const std::string &path, const CrLspDiagnostic &d) {
    const char *sev = d.severity == CrLspSeverity::Error     ? "error"
                      : d.severity == CrLspSeverity::Warning ? "warning"
                      : d.severity == CrLspSeverity::Hint    ? "hint"
                                                             : "info";
    char buf[64];
    std::snprintf(buf, sizeof buf, ":%d:%d: ", d.range.start_line + 1, d.range.start_col + 1);
    return path + buf + sev + ": " + d.message + " [" + d.code + "]";
}

bool CrLspDiagnosticFails(const CrLspDiagnostic &d) {
    return d.severity == CrLspSeverity::Error || d.severity == CrLspSeverity::Warning;
}
