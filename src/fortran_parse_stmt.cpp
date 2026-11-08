// The Fortran reader, part 2: statements (classification, the names each
// declares and refers to, the syntax checks that are certain) and the
// construct structure, with the errors gfortran reports for them. See
// fortran_parse.h.
//
// The reader is deliberately lenient: a statement it does not fully model
// is accepted, and only what gfortran -fsyntax-only certainly rejects is
// reported -- unbalanced parentheses, an unclassifiable statement, a
// malformed program-unit or END statement, a block statement out of place,
// a construct left open. tools/fortran_parse_check.py measures this.
#include <algorithm>
#include <cstring>
#include <set>

#include "fortran_parse.h"
#include "fortran_parse_internal.h"

namespace fortran {

namespace {

bool Has(const char *const *list, const std::string &w) {
    for (const char *const *p = list; *p; p++) {
        if (w == *p) return true;
    }
    return false;
}

const char *const kTypeWords[] = {"integer", "real", "complex", "logical", "character", "doubleprecision",
                                  "doublecomplex", "byte", nullptr};
const char *const kPrefixWords[] = {"recursive", "pure", "elemental", "impure", "non_recursive", "simple",
                                    "module", nullptr};

// Every keyword a statement can begin with (joined spellings included), for
// fixed form's blank-free statements (`CALLFOO(X)`, `GOTO10`, `DO10I=1,N`).
const char *const kLeadWords[] = {
    "doubleprecision", "doublecomplex", "blockdata", "selectcase", "selecttype", "selectrank", "errorstop",
    "elseif", "elsewhere", "endfile", "implicit", "subroutine", "function", "program", "module", "submodule",
    "character", "integer", "logical", "complex", "real", "byte", "dimension", "parameter", "equivalence",
    "common", "data", "external", "intrinsic", "save", "entry", "format", "include", "assign", "goto",
    "continue", "call", "return", "stop", "pause", "print", "read", "write", "open", "close", "inquire",
    "rewind", "backspace", "do", "if", "else", "end", "namelist", "allocatable", "allocate", "deallocate",
    "pointer", "target", "optional", "intent", "contains", "interface", "use", "cycle", "exit", "where",
    "nullify", "recursive", "pure", "elemental", nullptr};
// Words that begin a statement but are not split off (they are whole words already,
// or begin a two-word keyword: `double precision`, `go to`, `block data`).
const char *const kWholeWords[] = {
    "double", "go", "block", "select", "error", "type", "class", "procedure", "abstract", "generic", "final",
    "sequence", "import", "enum", "enumerator", "associate", "critical", "change", "sync", "lock", "unlock",
    "event", "fail", "form", "rank", "case", "flush", "wait", "value", "volatile", "protected", "public",
    "private", "contiguous", "asynchronous", "bind", "codimension", "impure", "non_recursive", "forall",
    "casedefault", "typeis", "classis", "classdefault", "dowhile", "doconcurrent", "elseif", "automatic",
    "static", "structure", "record", "union", "map", "virtual", "options", "dimension", nullptr};

std::string StripZeros(const std::string &s) {
    size_t z = 0;
    while (z + 1 < s.size() && s[z] == '0') z++;
    return s.substr(z);
}

bool IsUnitKind(StmtKind k) {
    return k == StmtKind::Program || k == StmtKind::Module || k == StmtKind::Submodule ||
           k == StmtKind::Subroutine || k == StmtKind::Function || k == StmtKind::BlockData ||
           k == StmtKind::ModuleProcedureUnit;
}

// The keyword an END statement names for each construct kind.
const char *EndWord(StmtKind k) {
    switch (k) {
        case StmtKind::Program: return "program";
        case StmtKind::Module: return "module";
        case StmtKind::Submodule: return "submodule";
        case StmtKind::Subroutine: return "subroutine";
        case StmtKind::Function: return "function";
        case StmtKind::BlockData: return "blockdata";
        case StmtKind::ModuleProcedureUnit: return "procedure";
        case StmtKind::DerivedType: return "type";
        case StmtKind::Interface: return "interface";
        case StmtKind::Enum: return "enum";
        case StmtKind::If: return "if";
        case StmtKind::Do: return "do";
        case StmtKind::SelectCase:
        case StmtKind::SelectType:
        case StmtKind::SelectRank: return "select";
        case StmtKind::Where: return "where";
        case StmtKind::Forall: return "forall";
        case StmtKind::Associate: return "associate";
        case StmtKind::Block: return "block";
        case StmtKind::Critical: return "critical";
        case StmtKind::ChangeTeam: return "team";
        default: return "";
    }
}

std::string Upper(std::string s) {
    for (char &c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

// "END SUBROUTINE", "END BLOCK DATA", "END SELECT" ...
std::string EndName(StmtKind k) {
    std::string w = EndWord(k);
    if (w == "blockdata") return "END BLOCK DATA";
    return "END " + Upper(w);
}

// ---- one statement ----------------------------------------------------------------

class StmtParser {
public:
    StmtParser(Stmt &st, Form form, bool lenient_names) : st_(st), t_(st.toks), form_(form), lenient_(lenient_names) {}

    // Classifies and parses; returns false (with message/pos) on a syntax error.
    bool Run();

    std::string msg;
    Pos pos;

private:
    size_t N() const { return t_.size(); }
    const Token *Tok(size_t k) const { return k < N() ? &t_[k] : nullptr; }
    bool IsOp(size_t k, const char *s) const {
        const Token *t = Tok(k);
        return t && t->kind == TokKind::Op && t->text == s;
    }
    bool IsKw(size_t k, const char *w) const {
        const Token *t = Tok(k);
        return t && t->kind == TokKind::Name && t->lower == w;
    }
    bool IsName(size_t k) const {
        const Token *t = Tok(k);
        return t && t->kind == TokKind::Name;
    }
    // `a b` or `ab` at k: returns the number of tokens matched (0 if none).
    size_t Kw2(size_t k, const char *a, const char *b) const {
        std::string joined = std::string(a) + b;
        if (IsKw(k, joined.c_str())) return 1;
        if (IsKw(k, a) && IsKw(k + 1, b)) return 2;
        return 0;
    }
    Pos At(size_t k) const { return k < N() ? t_[k].pos : st_.end; }

    bool Fail(size_t k, const std::string &m) {
        if (msg.empty()) {
            msg = m;
            pos = At(k);
        }
        return false;
    }
    bool FailStmt(size_t k, const char *what) { return Fail(k, std::string("Syntax error in ") + what + " statement at (1)"); }

    // The matching close of the group opening at k ("(" "(/" "["), or N() if unbalanced.
    size_t Close(size_t k) const {
        int d = 0;
        for (size_t j = k; j < N(); j++) {
            const Token &x = t_[j];
            if (x.kind != TokKind::Op) continue;
            if (x.text == "(" || x.text == "(/" || x.text == "[") d++;
            else if (x.text == ")" || x.text == "/)" || x.text == "]") {
                if (--d == 0) return j;
                if (d < 0) return N();
            }
        }
        return N();
    }
    // Index of the first depth-0 Op token `s` in [from, to), or `to`.
    size_t FindTop(size_t from, size_t to, const char *s) const {
        int d = 0;
        for (size_t j = from; j < to && j < N(); j++) {
            const Token &x = t_[j];
            if (x.kind != TokKind::Op) continue;
            if (x.text == "(" || x.text == "(/" || x.text == "[") d++;
            else if (x.text == ")" || x.text == "/)" || x.text == "]") d--;
            else if (d == 0 && x.text == s) return j;
        }
        return to;
    }

    int AddRef(size_t k, RefCtx ctx) {
        NameRef r;
        r.name = t_[k].lower;
        r.pos = t_[k].pos;
        r.end = t_[k].end;
        r.ctx = ctx;
        st_.refs.push_back(r);
        return static_cast<int>(st_.refs.size()) - 1;
    }
    void AddLabel(size_t k) {
        st_.label_refs.push_back(StripZeros(t_[k].text));
        st_.label_ref_pos.push_back(t_[k].pos);
    }
    Entity MakeEntity(size_t k) const {
        Entity e;
        e.name = t_[k].lower;
        e.spelling = t_[k].text;
        e.pos = t_[k].pos;
        e.end = t_[k].end;
        return e;
    }
    std::string Text(size_t from, size_t to) const {
        std::string s;
        for (size_t j = from; j < to && j < N(); j++) {
            if (!s.empty() && t_[j].kind == TokKind::Name && t_[j - 1].kind == TokKind::Name) s += ' ';
            s += t_[j].kind == TokKind::Name ? t_[j].lower : t_[j].text;
        }
        return s;
    }

    // Balanced parentheses over [from, to); refs collected. first_write: the
    // first name is assigned. Returns false on imbalance.
    bool Scan(size_t from, size_t to, RefCtx first_ctx = RefCtx::Read);
    bool ScanAll(size_t from) { return Scan(from, N()); }

    bool Assignment(size_t k);
    bool Classify(size_t k, bool action);
    bool Decl(size_t k, size_t type_len);
    size_t TypeSpecLen(size_t k) const;
    bool Unit(size_t k);
    bool End(size_t k);
    bool Use(size_t k);
    bool If(size_t k);
    bool Do(size_t k);
    bool Io(size_t k, const std::string &w);
    bool EntityNames(size_t k, size_t to, bool decl);
    bool DerivedType(size_t k);
    bool Interface(size_t k, bool abstract);
    bool LabelList(size_t k, size_t to);
    bool SimpleBlock(size_t k, StmtKind kind, bool need_parens);

    Stmt &st_;
    std::vector<Token> &t_;
    Form form_;
    bool lenient_;
};

bool StmtParser::Scan(size_t from, size_t to, RefCtx first_ctx) {
    // Stack of open groups: whether each is a call/array argument list.
    std::vector<bool> arglist;
    std::vector<int> argref;
    std::vector<int> args;
    int prev_ref = -1;
    bool first = true;
    for (size_t j = from; j < to && j < N(); j++) {
        const Token &x = t_[j];
        if (x.kind == TokKind::Name) {
            RefCtx ctx = RefCtx::Read;
            bool comp = j > from && IsOp(j - 1, "%");
            bool after_sep = j > from && (IsOp(j - 1, "(") || IsOp(j - 1, ","));
            bool eq = IsOp(j + 1, "=");
            if (comp) {
                ctx = RefCtx::Component;
            } else if (eq && after_sep && !arglist.empty()) {
                ctx = arglist.back() ? RefCtx::Keyword : RefCtx::Write;
            } else if (first) {
                ctx = first_ctx;
            }
            first = false;
            int r = AddRef(j, ctx);
            if (comp) st_.refs[static_cast<size_t>(r)].base = prev_ref;
            prev_ref = r;
            if (IsOp(j + 1, "(")) {
                st_.refs[static_cast<size_t>(r)].has_args = true;
            }
            continue;
        }
        if ((x.kind == TokKind::Int || x.kind == TokKind::Real || x.kind == TokKind::Logical) &&
            x.pos.line == x.end.line) {
            // a kind parameter named after the literal: 1.0_dp, 42_ik, .true._lk
            size_t us = x.text.find('_');
            if (us != std::string::npos && us + 1 < x.text.size() &&
                ((x.text[us + 1] >= 'a' && x.text[us + 1] <= 'z') || (x.text[us + 1] >= 'A' && x.text[us + 1] <= 'Z'))) {
                NameRef r;
                for (size_t c = us + 1; c < x.text.size(); c++) {
                    char ch = x.text[c];
                    r.name += (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
                }
                r.pos = {x.pos.line, x.pos.col + static_cast<int>(us) + 1};
                r.end = x.end;
                st_.refs.push_back(r);
            }
            continue;
        }
        if (x.kind != TokKind::Op) continue;
        if (x.text == "(" || x.text == "(/" || x.text == "[") {
            bool al = x.text == "(" && j > from && IsName(j - 1) && !st_.refs.empty() &&
                      st_.refs.back().pos == t_[j - 1].pos;
            // `a(i)(1:2)`, `f(x)%y(...)`: still subscripts
            if (x.text == "(" && j > from && IsOp(j - 1, ")")) al = true;
            arglist.push_back(al);
            argref.push_back(al && IsName(j - 1) ? static_cast<int>(st_.refs.size()) - 1 : -1);
            args.push_back(IsOp(j + 1, ")") ? 0 : 1);
            if (static_cast<int>(arglist.size()) > kMaxNesting) return Fail(j, "Expression nested too deeply at (1)");
        } else if (x.text == ")" || x.text == "/)" || x.text == "]") {
            if (arglist.empty()) return false;
            if (argref.back() >= 0) st_.refs[static_cast<size_t>(argref.back())].args = args.back();
            arglist.pop_back();
            argref.pop_back();
            args.pop_back();
            if (!IsOp(j + 1, "%")) prev_ref = -1;
        } else if (x.text == "," && !args.empty()) {
            args.back()++;
        } else if (x.text != "%") {
            prev_ref = -1;
        }
    }
    return arglist.empty();
}

// The type-spec at k: `integer`, `integer(8)`, `integer*8`, `real(kind=dp)`,
// `character(len=*)`, `character*(*)`, `double precision`, `type(t)`,
// `class(*)`, `procedure(iface)`. Returns its token count, or 0.
size_t StmtParser::TypeSpecLen(size_t k) const {
    const Token *t = Tok(k);
    if (!t || t->kind != TokKind::Name) return 0;
    size_t n = 0;
    if (Kw2(k, "double", "precision")) n = Kw2(k, "double", "precision");
    else if (Kw2(k, "double", "complex")) n = Kw2(k, "double", "complex");
    else if (Has(kTypeWords, t->lower)) n = 1;
    else if ((t->lower == "type" || t->lower == "class" || t->lower == "procedure") && IsOp(k + 1, "(")) {
        size_t c = Close(k + 1);
        return c < N() ? c - k + 1 : 0;
    } else {
        return 0;
    }
    size_t j = k + n;
    if (IsOp(j, "(")) {
        size_t c = Close(j);
        if (c >= N()) return 0;
        j = c + 1;
    } else if (IsOp(j, "*")) {
        if (Tok(j + 1) && Tok(j + 1)->kind == TokKind::Int) {
            j += 2;
        } else if (IsOp(j + 1, "(")) {
            size_t c = Close(j + 1);
            if (c >= N()) return 0;
            j = c + 1;
        }
    }
    return j - k;
}

bool StmtParser::EntityNames(size_t k, size_t to, bool decl) {
    // name [(spec)] [[cospec]] [*len] [= expr | => expr] {, ...}
    size_t j = k;
    while (j < to) {
        if (!IsName(j)) return false;
        Entity e = MakeEntity(j);
        j++;
        if (IsOp(j, "(")) {
            size_t c = Close(j);
            if (c >= to) return false;
            e.array = true;
            if (!Scan(j + 1, c)) return false;
            j = c + 1;
        }
        if (IsOp(j, "[")) {
            size_t c = Close(j);
            if (c >= to) return false;
            Scan(j + 1, c);
            j = c + 1;
        }
        if (decl && IsOp(j, "*")) {
            if (IsOp(j + 1, "(")) {
                size_t c = Close(j + 1);
                if (c >= to) return false;
                Scan(j + 2, c);
                j = c + 1;
            } else if (Tok(j + 1) && Tok(j + 1)->kind == TokKind::Int) {
                j += 2;
            } else {
                return false;
            }
        }
        if (decl && (IsOp(j, "=") || IsOp(j, "=>"))) {
            e.init = true;
            size_t comma = FindTop(j + 1, to, ",");
            if (comma == j + 1) return false;
            if (!Scan(j + 1, comma)) return false;
            j = comma;
        } else if (decl && IsOp(j, "/")) {
            // old-style initialisation: integer x /3/
            e.init = true;
            size_t s = j + 1;
            while (s < to && !IsOp(s, "/")) s++;
            if (s >= to) return false;
            j = s + 1;
        }
        st_.entities.push_back(e);
        if (j >= to) return true;
        if (!IsOp(j, ",")) return false;
        j++;
        if (j >= to) return false;
    }
    return true;
}

bool StmtParser::Decl(size_t k, size_t tl) {
    st_.kind = StmtKind::TypeDecl;
    st_.type = Text(k, k + tl);
    // type-spec contents: kind/len expressions and the type's name
    if (tl > 1) {
        if (IsKw(k, "type") || IsKw(k, "class")) {
            if (IsName(k + 2) && IsOp(k + 3, ")")) AddRef(k + 2, RefCtx::Type);
        } else if (IsKw(k, "procedure")) {
            if (IsName(k + 2) && IsOp(k + 3, ")")) AddRef(k + 2, RefCtx::Call);
        } else {
            Scan(k + 1, k + tl);
        }
    }
    const char *what = "data declaration";
    if (IsKw(k, "character")) what = "CHARACTER declaration";
    if (IsKw(k, "procedure")) what = "PROCEDURE";
    size_t j = k + tl;
    size_t dc = FindTop(j, N(), "::");
    if (dc < N()) {
        // attributes
        while (j < dc) {
            if (!IsOp(j, ",")) return FailStmt(j, what);
            j++;
            if (!IsName(j)) return FailStmt(j, what);
            const std::string &a = t_[j].lower;
            static const struct {
                const char *w;
                uint32_t bit;
            } kAttrs[] = {{"parameter", kAttrParameter},   {"allocatable", kAttrAllocatable},
                          {"pointer", kAttrPointer},       {"target", kAttrTarget},
                          {"dimension", kAttrDimension},   {"optional", kAttrOptional},
                          {"save", kAttrSave},             {"external", kAttrExternal},
                          {"intrinsic", kAttrIntrinsic},   {"public", kAttrPublic},
                          {"private", kAttrPrivate},       {"value", kAttrValue},
                          {"volatile", kAttrVolatile},     {"protected", kAttrProtected},
                          {"bind", kAttrBind},             {"contiguous", kAttrContiguous},
                          {"asynchronous", kAttrAsynchronous}, {"codimension", kAttrCodimension}};
            uint32_t bit = 0;
            for (const auto &x : kAttrs) {
                if (a == x.w) bit = x.bit;
            }
            if (a == "intent") {
                if (IsOp(j + 1, "(")) {
                    std::string in = Text(j + 2, Close(j + 1));
                    if (in.find("in") == 0) bit |= kAttrIntentIn;
                    if (in.find("out") != std::string::npos) bit |= kAttrIntentOut;
                }
            }
            if (!bit && a != "intent" && a != "kind" && a != "len" && a != "nopass" && a != "pass" &&
                a != "deferred" && a != "non_overridable" && a != "automatic" && a != "static" && a != "device" &&
                a != "managed" && a != "pinned" && a != "shared" && a != "constant" && a != "texture" &&
                a != "rank")
                return FailStmt(j, what);
            st_.attrs |= bit;
            j++;
            if (IsOp(j, "[")) {
                size_t c = Close(j);
                if (c >= dc) return FailStmt(j, what);
                j = c + 1;
            }
            if (IsOp(j, "(")) {
                size_t c = Close(j);
                if (c >= dc) return FailStmt(j, what);
                Scan(j + 1, c);
                j = c + 1;
            }
        }
        j = dc + 1;
        if (j >= N()) return FailStmt(j, what);
    } else if (IsOp(j, ",")) {
        // `character*1, c`: gfortran takes the comma without `::`
        j++;
    }
    if (!EntityNames(j, N(), true)) return FailStmt(j, what);
    return true;
}

// PROGRAM / MODULE / SUBMODULE / [prefix] SUBROUTINE / FUNCTION / BLOCK DATA /
// MODULE PROCEDURE (separate module subprogram).
bool StmtParser::Unit(size_t k) {
    size_t j = k;
    std::string prefix;
    std::string type;
    // prefixes: recursive pure elemental impure module non_recursive, a type-spec
    for (;;) {
        if (IsName(j) && Has(kPrefixWords, t_[j].lower) &&
            !(t_[j].lower == "module" && (IsKw(j + 1, "procedure") ? false : !IsName(j + 1)))) {
            if (t_[j].lower == "module" && IsKw(j + 1, "procedure")) {
                st_.kind = StmtKind::ModuleProcedureUnit;
                prefix += prefix.empty() ? "module" : " module";
                j += 2;
                if (!IsName(j) || j + 1 != N()) return FailStmt(j, "MODULE PROCEDURE");
                st_.unit.name = t_[j].lower;
                st_.unit.spelling = t_[j].text;
                st_.unit.name_pos = t_[j].pos;
                st_.unit.name_end = t_[j].end;
                st_.unit.prefix = prefix;
                return true;
            }
            prefix += (prefix.empty() ? "" : " ") + t_[j].lower;
            j++;
            continue;
        }
        if (type.empty()) {
            size_t tl = TypeSpecLen(j);
            if (tl && (IsKw(j + tl, "function") || (IsName(j + tl) && Has(kPrefixWords, t_[j + tl].lower)))) {
                type = Text(j, j + tl);
                Scan(j + 1, j + tl);
                j += tl;
                continue;
            }
        }
        break;
    }
    st_.unit.prefix = prefix;
    st_.unit.type = type;
    const char *what = "SUBROUTINE";
    if (IsKw(j, "subroutine")) {
        st_.kind = StmtKind::Subroutine;
    } else if (IsKw(j, "function")) {
        st_.kind = StmtKind::Function;
        what = "FUNCTION";
    } else {
        return Fail(j, "Unclassifiable statement at (1)");
    }
    j++;
    if (!IsName(j)) return FailStmt(j, what);
    st_.unit.name = t_[j].lower;
    st_.unit.spelling = t_[j].text;
    st_.unit.name_pos = t_[j].pos;
    st_.unit.name_end = t_[j].end;
    j++;
    if (IsOp(j, "(")) {
        st_.unit.has_args = true;
        size_t c = Close(j);
        if (c >= N()) return FailStmt(j, what);
        j++;
        while (j < c) {
            if (IsName(j)) {
                st_.unit.dummies.push_back(MakeEntity(j));
            } else if (IsOp(j, "*") && st_.kind == StmtKind::Subroutine) {
                Entity e = MakeEntity(j);
                e.name = "*";
                st_.unit.dummies.push_back(e);
            } else {
                return FailStmt(j, what);
            }
            j++;
            if (j < c && !IsOp(j, ",")) return FailStmt(j, what);
            if (IsOp(j, ",")) {
                j++;
                if (j >= c) return FailStmt(j, what);
            }
        }
        j = c + 1;
    } else if (st_.kind == StmtKind::Function) {
        return Fail(j, "Expected formal argument list in function definition at (1)");
    }
    // suffix: RESULT(name), BIND(C[, name=...]) in any order
    while (j < N()) {
        if (st_.kind == StmtKind::Function && IsKw(j, "result") && IsOp(j + 1, "(") && IsName(j + 2) &&
            IsOp(j + 3, ")")) {
            st_.unit.result = t_[j + 2].lower;
            Entity e = MakeEntity(j + 2);
            st_.entities.push_back(e);
            j += 4;
        } else if (IsKw(j, "bind") && IsOp(j + 1, "(")) {
            size_t c = Close(j + 1);
            if (c >= N()) return FailStmt(j, what);
            j = c + 1;
        } else {
            return FailStmt(j, what);
        }
    }
    if (st_.kind == StmtKind::Function && st_.unit.result.empty()) st_.unit.result = st_.unit.name;
    return true;
}

bool StmtParser::End(size_t k) {
    // end [kind [name]] -- kinds may be joined (enddo, endsubroutine, end blockdata)
    st_.kind = StmtKind::End;
    static const char *const kEnds[] = {"subroutine", "function", "program", "module", "submodule", "procedure",
                                        "type", "interface", "enum", "if", "do", "select", "where", "forall",
                                        "associate", "block", "critical", "team", "blockdata", nullptr};
    size_t j = k;
    const std::string &w = t_[j].lower;
    std::string kind;
    if (w == "end") {
        j++;
        if (IsName(j)) {
            if (IsKw(j, "block") && IsKw(j + 1, "data")) {
                kind = "blockdata";
                j += 2;
            } else if (Has(kEnds, t_[j].lower)) {
                kind = t_[j].lower;
                j++;
            } else {
                return Fail(j, "Syntax error in END statement at (1)");
            }
        }
    } else {
        kind = w.substr(3);
        j++;
        if (kind == "block" && IsKw(j, "data")) {
            kind = "blockdata";
            j++;
        }
    }
    st_.end_of = kind;
    if (j < N()) {
        if (kind.empty() || !IsName(j) || j + 1 != N()) {
            // `end type t`, `end interface operator(+)`, `end interface assignment(=)`
            if (kind == "interface" && j < N()) return true;
            std::string what = kind.empty() ? "END" : "END " + Upper(kind == "blockdata" ? "block data" : kind);
            return Fail(j, "Syntax error in " + what + " statement at (1)");
        }
        st_.unit.name = t_[j].lower;
        st_.unit.spelling = t_[j].text;
        st_.unit.name_pos = t_[j].pos;
        st_.unit.name_end = t_[j].end;
        AddRef(j, RefCtx::Construct);
    }
    return true;
}

bool StmtParser::Use(size_t k) {
    st_.kind = StmtKind::Use;
    size_t j = k + 1;
    if (IsOp(j, ",")) {
        if (IsKw(j + 1, "intrinsic")) st_.use_intrinsic = true;
        else if (!IsKw(j + 1, "non_intrinsic")) return FailStmt(j + 1, "USE");
        j += 2;
        if (!IsOp(j, "::")) return FailStmt(j, "USE");
        j++;
    } else if (IsOp(j, "::")) {
        j++;
    }
    if (!IsName(j)) return FailStmt(j, "USE");
    st_.group = t_[j].lower;
    AddRef(j, RefCtx::Module);
    j++;
    if (j == N()) return true;
    if (!IsOp(j, ",")) return FailStmt(j, "USE");
    j++;
    if (IsKw(j, "only") && IsOp(j + 1, ":")) {
        st_.use_only = true;
        j += 2;
        if (j == N()) return true;
    }
    while (j < N()) {
        // [local =>] name | operator(.x.) [=> operator(.y.)] | assignment(=)
        if ((IsKw(j, "operator") || IsKw(j, "assignment") || IsKw(j, "read") || IsKw(j, "write")) &&
            IsOp(j + 1, "(")) {
            size_t c = Close(j + 1);
            if (c >= N()) return FailStmt(j, "USE");
            j = c + 1;
            if (IsOp(j, "=>")) {
                j++;
                if (!IsKw(j, "operator") || !IsOp(j + 1, "(")) return FailStmt(j, "USE");
                c = Close(j + 1);
                if (c >= N()) return FailStmt(j, "USE");
                j = c + 1;
            }
        } else if (IsName(j)) {
            Entity e = MakeEntity(j);
            j++;
            if (IsOp(j, "=>")) {
                j++;
                if (IsKw(j, "operator") && IsOp(j + 1, "(")) {
                    size_t c = Close(j + 1);
                    if (c >= N()) return FailStmt(j, "USE");
                    j = c + 1;
                } else {
                    if (!IsName(j)) return FailStmt(j, "USE");
                    e.rename = t_[j].lower;
                    j++;
                }
            }
            st_.entities.push_back(e);
        } else {
            return FailStmt(j, "USE");
        }
        if (j == N()) return true;
        if (!IsOp(j, ",")) return FailStmt(j, "USE");
        j++;
        if (j == N()) return FailStmt(j, "USE");
    }
    return true;
}

bool StmtParser::LabelList(size_t k, size_t to) {
    for (size_t j = k; j < to; j++) {
        if (Tok(j)->kind == TokKind::Int) {
            AddLabel(j);
        } else if (!IsOp(j, ",")) {
            return false;
        }
    }
    return true;
}

bool StmtParser::If(size_t k) {
    size_t j = k + 1;
    if (!IsOp(j, "(")) return Fail(j, "Unclassifiable statement at (1)");
    size_t c = Close(j);
    if (c >= N()) return Fail(N(), "Syntax error in IF-expression at (1)");
    if (c == j + 1) return Fail(j + 1, "Syntax error in IF-expression at (1)");
    if (!Scan(j + 1, c)) return Fail(j, "Syntax error in IF-expression at (1)");
    j = c + 1;
    if (j == N()) return Fail(j, "Syntax error in IF-clause after (1)");
    if (IsKw(j, "then") && j + 1 == N()) {
        st_.kind = StmtKind::If;
        return true;
    }
    if (Tok(j)->kind == TokKind::Int) {
        st_.kind = StmtKind::ArithmeticIf;
        if (!LabelList(j, N())) return Fail(j, "Syntax error in IF-clause after (1)");
        return true;
    }
    st_.kind = StmtKind::LogicalIf;
    // the action statement
    StmtKind save = st_.kind;
    if (!Classify(j, true)) return false;
    StmtKind action = st_.kind;
    st_.kind = save;
    if (action == StmtKind::If || action == StmtKind::LogicalIf || action == StmtKind::Do ||
        action == StmtKind::End || IsUnitKind(action) || action == StmtKind::TypeDecl ||
        action == StmtKind::SelectCase || action == StmtKind::Else || action == StmtKind::ElseIf) {
        return Fail(j, "Syntax error in IF-clause after (1)");
    }
    return true;
}

bool StmtParser::Do(size_t k) {
    st_.kind = StmtKind::Do;
    size_t j = k + 1;
    if (Tok(j) && Tok(j)->kind == TokKind::Int) {
        st_.do_label = StripZeros(t_[j].text);
        AddLabel(j);
        j++;
        if (IsOp(j, ",")) j++;
    } else if (IsOp(j, ",")) {
        j++;  // `do, i = 1, n`
    }
    if (j == N()) return true;
    if (IsKw(j, "while") || IsKw(j, "concurrent")) {
        bool conc = IsKw(j, "concurrent");
        j++;
        if (!IsOp(j, "(")) return FailStmt(j, "DO");
        size_t c = Close(j);
        if (c >= N()) return FailStmt(j, "DO");
        if (conc) {
            // (i = 1:n, j = 1:m, mask): index names are written
            size_t s = j + 1;
            if (TypeSpecLen(s) && IsOp(s + TypeSpecLen(s), "::")) s += TypeSpecLen(s) + 1;
            for (size_t q = s; q < c; q++) {
                if (IsName(q) && IsOp(q + 1, "=") && (q == s || IsOp(q - 1, ","))) {
                    AddRef(q, RefCtx::Write);
                    q++;
                    size_t comma = FindTop(q + 1, c, ",");
                    Scan(q + 1, comma);
                    q = comma;
                } else {
                    size_t comma = FindTop(q, c, ",");
                    Scan(q, comma);
                    q = comma;
                }
            }
        } else if (!Scan(j + 1, c)) {
            return FailStmt(j, "DO WHILE");
        }
        j = c + 1;
        while (j < N() && IsName(j)) {  // locality specs: local(x) shared(y) default(none) reduce(+:s)
            if (!IsOp(j + 1, "(")) return FailStmt(j, "DO");
            c = Close(j + 1);
            if (c >= N()) return FailStmt(j, "DO");
            j = c + 1;
        }
        if (j != N()) return FailStmt(j, "DO");
        return true;
    }
    // var = e1, e2 [, e3]
    if (!IsName(j) || !IsOp(j + 1, "=")) return FailStmt(j, "DO");
    AddRef(j, RefCtx::Write);
    size_t c1 = FindTop(j + 2, N(), ",");
    if (c1 >= N() || c1 == j + 2) return Fail(c1, "Syntax error in iterator at (1)");
    if (!Scan(j + 2, N())) return Fail(j, "Syntax error in iterator at (1)");
    return true;
}

bool StmtParser::Io(size_t k, const std::string &w) {
    st_.kind = w == "print" ? StmtKind::Print : StmtKind::Io;
    size_t j = k + 1;
    std::string up = Upper(w);
    if (w == "print" || (w == "read" && !IsOp(j, "("))) {
        // format [, items]
        if (j == N()) return FailStmt(j, up.c_str());
        if (Tok(j)->kind == TokKind::Int) {
            AddLabel(j);
            j++;
        } else if (IsOp(j, "*")) {
            j++;
        } else {
            size_t c = FindTop(j, N(), ",");
            if (!Scan(j, c)) return FailStmt(j, up.c_str());
            j = c;
        }
        if (j == N()) return true;
        if (!IsOp(j, ",")) return FailStmt(j, up.c_str());
        if (j + 1 == N()) return FailStmt(j + 1, up.c_str());
        if (!Scan(j + 1, N(), w == "read" ? RefCtx::Write : RefCtx::Read)) return FailStmt(j, up.c_str());
        return true;
    }
    bool paren_form = w == "read" || w == "write" || w == "open" || w == "close" || w == "inquire" || w == "wait" ||
                      w == "flush" || w == "rewind" || w == "backspace" || w == "endfile";
    if (!paren_form) return ScanAll(j) ? true : FailStmt(j, up.c_str());
    if (!IsOp(j, "(")) {
        if (w == "write" || w == "open" || w == "close" || w == "inquire" || w == "wait")
            return FailStmt(j, up.c_str());
        // rewind 10, flush u
        return ScanAll(j) ? true : FailStmt(j, up.c_str());
    }
    size_t c = Close(j);
    if (c >= N()) return FailStmt(j, up.c_str());
    // control list: labels as ERR=, END=, EOR=, FMT=, or a bare label in 2nd position
    for (size_t q = j + 1; q < c; q++) {
        if (IsName(q) && IsOp(q + 1, "=") && (IsOp(q - 1, "(") || IsOp(q - 1, ","))) {
            const std::string &kw = t_[q].lower;
            if ((kw == "err" || kw == "end" || kw == "eor" || kw == "fmt") && Tok(q + 2) &&
                Tok(q + 2)->kind == TokKind::Int)
                AddLabel(q + 2);
        } else if (Tok(q)->kind == TokKind::Int && IsOp(q - 1, ",") && (IsOp(q + 1, ",") || q + 1 == c) &&
                   FindTop(j + 1, c, ",") == q - 1) {
            // the bare format label: second item of the control list, at its top level
            AddLabel(q);
        }
    }
    if (!Scan(j, c + 1)) return FailStmt(j, up.c_str());
    // control-list keywords are not variables
    for (NameRef &r : st_.refs) {
        if (r.ctx == RefCtx::Write) r.ctx = RefCtx::Keyword;
    }
    j = c + 1;
    if (j == N()) return true;
    if (w != "read" && w != "write") {
        if (w == "inquire" && st_.refs.size() > 0) return ScanAll(j) ? true : FailStmt(j, up.c_str());
        return FailStmt(j, up.c_str());
    }
    if (IsOp(j, ",")) j++;  // extension: read(...) , x
    if (j == N()) return FailStmt(j, up.c_str());
    if (!Scan(j, N(), w == "read" ? RefCtx::Write : RefCtx::Read)) return FailStmt(j, up.c_str());
    if (w == "read") {
        // every top-level item is written
        int d = 0;
        for (size_t q = j; q < N(); q++) {
            if (IsOp(q, "(")) d++;
            else if (IsOp(q, ")")) d--;
            else if (d == 0 && IsName(q) && (q == j || IsOp(q - 1, ","))) {
                for (NameRef &r : st_.refs) {
                    if (r.pos == t_[q].pos) r.ctx = RefCtx::Write;
                }
            }
        }
    }
    return true;
}

bool StmtParser::DerivedType(size_t k) {
    // type [, attrs ::] name [(params)]   |   type :: name
    st_.kind = StmtKind::DerivedType;
    size_t j = k + 1;
    if (IsOp(j, ",") || IsOp(j, "::")) {
        size_t dc = FindTop(j, N(), "::");
        if (dc >= N()) return Fail(j, "Mangled derived type definition at (1)");
        for (size_t q = j; q < dc; q++) {
            if (IsKw(q, "extends") && IsOp(q + 1, "(") && IsName(q + 2)) AddRef(q + 2, RefCtx::Type);
            if (IsKw(q, "abstract")) st_.abstract = true;
        }
        j = dc + 1;
    }
    if (!IsName(j)) return Fail(j, "Mangled derived type definition at (1)");
    st_.unit.name = t_[j].lower;
    st_.unit.spelling = t_[j].text;
    st_.unit.name_pos = t_[j].pos;
    st_.unit.name_end = t_[j].end;
    j++;
    if (IsOp(j, "(")) {
        size_t c = Close(j);
        if (c >= N()) return Fail(j, "Mangled derived type definition at (1)");
        j = c + 1;
    }
    if (j != N()) return Fail(j, "Mangled derived type definition at (1)");
    return true;
}

bool StmtParser::Interface(size_t k, bool abstract) {
    st_.kind = StmtKind::Interface;
    st_.abstract = abstract;
    size_t j = k + 1;
    if (j == N()) return true;
    if (IsKw(j, "operator") && IsOp(j + 1, "(/")) {
        // `interface operator (/ /)`: the tokens read `(/` ... `)`
        st_.generic_spec = true;
        st_.unit.name = "operator(/)";
        st_.unit.name_pos = t_[j].pos;
        st_.unit.name_end = t_[N() - 1].end;
        return true;
    }
    if (IsKw(j, "operator") || IsKw(j, "assignment") || IsKw(j, "read") || IsKw(j, "write")) {
        st_.generic_spec = true;
        if (!IsOp(j + 1, "(")) return FailStmt(j, "INTERFACE");
        size_t c = Close(j + 1);
        if (c >= N()) return FailStmt(j, "INTERFACE");
        st_.unit.name = Text(j, c + 1);
        st_.unit.name_pos = t_[j].pos;
        st_.unit.name_end = t_[c].end;
        return c + 1 == N() ? true : FailStmt(c + 1, "INTERFACE");
    }
    if (!IsName(j) || j + 1 != N()) return FailStmt(j, "INTERFACE");
    st_.unit.name = t_[j].lower;
    st_.unit.spelling = t_[j].text;
    st_.unit.name_pos = t_[j].pos;
    st_.unit.name_end = t_[j].end;
    return true;
}

// The whole statement is `designator = expr` or `designator => expr`?
bool StmtParser::Assignment(size_t k) {
    if (!IsName(k)) return false;
    size_t j = k + 1;
    for (;;) {
        if (IsOp(j, "(") || IsOp(j, "[")) {
            size_t c = Close(j);
            if (c >= N()) return false;
            j = c + 1;
        } else if (IsOp(j, "%") && IsName(j + 1)) {
            j += 2;
        } else {
            break;
        }
    }
    if (!IsOp(j, "=") && !IsOp(j, "=>")) return false;
    bool ptr = IsOp(j, "=>");
    // `do10i=1,10` / `DO 10 I = 1, 10` are DO statements (fixed form)
    if (FindTop(j + 1, N(), ",") < N() && !ptr) return false;
    st_.kind = ptr ? StmtKind::PointerAssignment : StmtKind::Assignment;
    if (j + 1 == N()) return Fail(j + 1, "Syntax error in expression at (1)");  // nothing on the right
    Scan(k, j, RefCtx::Write);
    if (!Scan(j + 1, N())) return Fail(j + 1, "Syntax error in expression at (1)");
    return true;
}

bool StmtParser::SimpleBlock(size_t k, StmtKind kind, bool need_parens) {
    st_.kind = kind;
    size_t j = k;
    if (need_parens) {
        if (!IsOp(j, "(")) return Fail(j, "Unclassifiable statement at (1)");
        size_t c = Close(j);
        if (c >= N()) return Fail(N(), "Expected a right parenthesis in expression at (1)");
        Scan(j + 1, c);
        j = c + 1;
    }
    if (j != N()) {
        if (!ScanAll(j)) return Fail(j, "Unclassifiable statement at (1)");
    }
    return true;
}

bool StmtParser::Classify(size_t k, bool action) {
    if (k >= N()) return Fail(k, "Unclassifiable statement at (1)");
    // fixed form: blanks inside a name don't count (`MAX WRK = 1`)
    if (form_ == Form::Fixed && IsName(k) && Tok(k + 1) &&
        (Tok(k + 1)->kind == TokKind::Name || Tok(k + 1)->kind == TokKind::Int) && !Has(kLeadWords, t_[k].lower) &&
        !Has(kWholeWords, t_[k].lower)) {
        bool starts_kw = false;
        for (const char *const *p = kLeadWords; *p && !starts_kw; p++) {
            starts_kw = t_[k].lower.compare(0, std::strlen(*p), *p) == 0;
        }
        size_t e = k + 1;
        while (e < N() && (t_[e].kind == TokKind::Name || t_[e].kind == TokKind::Int)) e++;
        if (!starts_kw && (IsOp(e, "=") || IsOp(e, "(") || IsOp(e, "%"))) {
            Token merged = t_[k];
            for (size_t q = k + 1; q < e; q++) {
                merged.text += t_[q].text;
                merged.lower += t_[q].lower;
            }
            merged.end = t_[e - 1].end;
            std::vector<Token> saved(t_.begin() + static_cast<std::ptrdiff_t>(k), t_.begin() + static_cast<std::ptrdiff_t>(e));
            const size_t nrefs = st_.refs.size();
            t_.erase(t_.begin() + static_cast<std::ptrdiff_t>(k + 1), t_.begin() + static_cast<std::ptrdiff_t>(e));
            t_[k] = merged;
            if (Assignment(k)) return true;
            if (!msg.empty()) return false;
            // not an assignment after all: put the words back
            t_.erase(t_.begin() + static_cast<std::ptrdiff_t>(k));
            t_.insert(t_.begin() + static_cast<std::ptrdiff_t>(k), saved.begin(), saved.end());
            st_.refs.resize(nrefs);
        }
    }
    if (Assignment(k)) return true;
    if (!msg.empty()) return false;
    const Token &t0 = t_[k];
    if (t0.kind != TokKind::Name) {
        if (t0.kind == TokKind::Op && t0.text == ";") return Fail(k, "Semicolon at (1) needs to be preceded by statement");
        return Fail(k, "Unclassifiable statement at (1)");
    }
    std::string w = t0.lower;
    // fixed form: blanks are insignificant, `CALLFOO(X)` `GOTO10` `DO10I=1,N` `REAL*8X`
    if (form_ == Form::Fixed) {
        bool known = Has(kLeadWords, w) || Has(kWholeWords, w);
        if (!known && !w.empty() && !(w.size() > 3 && w.compare(0, 3, "end") == 0 && Has(kLeadWords, w.substr(3)))) {
            for (const char *const *p = kLeadWords; *p; p++) {
                size_t n = std::strlen(*p);
                if (w.size() > n && w.compare(0, n, *p) == 0) {
                    Token kw = t0;
                    kw.text = t0.text.substr(0, n);
                    kw.lower = w.substr(0, n);
                    kw.end = {t0.pos.line, t0.pos.col + static_cast<int>(n)};
                    Token rest = t0;
                    rest.text = t0.text.substr(n);
                    rest.lower = w.substr(n);
                    rest.pos = kw.end;
                    std::vector<Token> split{kw};
                    size_t digits = 0;
                    while (digits < rest.text.size() && rest.text[digits] >= '0' && rest.text[digits] <= '9') digits++;
                    if (digits > 0) {
                        Token lab = rest;
                        lab.kind = TokKind::Int;
                        lab.text = lab.lower = rest.text.substr(0, digits);
                        lab.end = {rest.pos.line, rest.pos.col + static_cast<int>(digits)};
                        split.push_back(lab);
                        if (digits < rest.text.size()) {
                            Token nm = rest;
                            nm.text = rest.text.substr(digits);
                            nm.lower = rest.lower.substr(digits);
                            nm.pos = lab.end;
                            split.push_back(nm);
                        }
                    } else {
                        split.push_back(rest);
                    }
                    t_.erase(t_.begin() + static_cast<std::ptrdiff_t>(k));
                    t_.insert(t_.begin() + static_cast<std::ptrdiff_t>(k), split.begin(), split.end());
                    w = t_[k].lower;
                    break;
                }
            }
        }
    }
    size_t n2 = 0;
    // ---- program units ----
    if (!action) {
        if (w == "program") {
            st_.kind = StmtKind::Program;
            if (k + 1 < N()) {
                if (!IsName(k + 1) || k + 2 != N()) return FailStmt(k + 1, "PROGRAM");
                st_.unit.name = t_[k + 1].lower;
                st_.unit.spelling = t_[k + 1].text;
                st_.unit.name_pos = t_[k + 1].pos;
                st_.unit.name_end = t_[k + 1].end;
            }
            return true;
        }
        if (w == "module" && IsName(k + 1) && k + 2 == N() && !IsKw(k + 1, "procedure")) {
            st_.kind = StmtKind::Module;  // `module complex` names a module
            st_.unit.name = t_[k + 1].lower;
            st_.unit.spelling = t_[k + 1].text;
            st_.unit.name_pos = t_[k + 1].pos;
            st_.unit.name_end = t_[k + 1].end;
            return true;
        }
        if (w == "module" && !IsKw(k + 1, "procedure") && !(IsName(k + 1) && (Has(kPrefixWords, t_[k + 1].lower) ||
                                                                             IsKw(k + 1, "subroutine") ||
                                                                             IsKw(k + 1, "function") ||
                                                                             TypeSpecLen(k + 1)))) {
            st_.kind = StmtKind::Module;
            if (!IsName(k + 1) || k + 2 != N()) return Fail(k + 1, "Unclassifiable statement at (1)");
            st_.unit.name = t_[k + 1].lower;
            st_.unit.spelling = t_[k + 1].text;
            st_.unit.name_pos = t_[k + 1].pos;
            st_.unit.name_end = t_[k + 1].end;
            return true;
        }
        if (w == "module" && IsKw(k + 1, "procedure") && (IsOp(k + 2, "::") || IsOp(FindTop(k + 2, N(), ","), ","))) {
            st_.kind = StmtKind::ModuleProcedureList;
            size_t j = IsOp(k + 2, "::") ? k + 3 : k + 2;
            if (!EntityNames(j, N(), false)) return FailStmt(j, "MODULE PROCEDURE");
            return true;
        }
        if (w == "submodule") {
            st_.kind = StmtKind::Submodule;
            if (!IsOp(k + 1, "(")) return FailStmt(k + 1, "SUBMODULE");
            size_t c = Close(k + 1);
            if (c >= N() || !IsName(c + 1) || c + 2 != N()) return FailStmt(k + 1, "SUBMODULE");
            if (IsName(k + 2)) {
                st_.unit.parent = t_[k + 2].lower;
                AddRef(k + 2, RefCtx::Module);
            }
            st_.unit.name = t_[c + 1].lower;
            st_.unit.spelling = t_[c + 1].text;
            st_.unit.name_pos = t_[c + 1].pos;
            st_.unit.name_end = t_[c + 1].end;
            return true;
        }
        if ((n2 = Kw2(k, "block", "data")) != 0) {
            st_.kind = StmtKind::BlockData;
            size_t j = k + n2;
            if (j < N()) {
                if (!IsName(j) || j + 1 != N()) return FailStmt(j, "BLOCK DATA");
                st_.unit.name = t_[j].lower;
                st_.unit.spelling = t_[j].text;
                st_.unit.name_pos = t_[j].pos;
                st_.unit.name_end = t_[j].end;
            }
            return true;
        }
        if (w == "subroutine" || w == "function" || Has(kPrefixWords, w)) {
            // a prefix word could also start a declaration: `module` handled above
            return Unit(k);
        }
        {
            size_t tl = TypeSpecLen(k);
            if (tl && (IsKw(k + tl, "function") || (IsName(k + tl) && Has(kPrefixWords, t_[k + tl].lower) &&
                                                    FindTop(k, N(), "::") == N())))
                return Unit(k);
        }
    }
    // ---- END ----
    if (w == "end" || (w.size() > 3 && w.compare(0, 3, "end") == 0 && w != "endfile" && w != "entry" &&
                       w != "enum" && w != "enumerator")) {
        if (w == "end" && IsKw(k + 1, "file")) return Io(k + 1, "endfile");
        if (w != "end") {
            static const char *const kJoined[] = {"endsubroutine", "endfunction", "endprogram", "endmodule",
                                                  "endsubmodule", "endprocedure", "endtype", "endinterface",
                                                  "endenum", "endif", "enddo", "endselect", "endwhere",
                                                  "endforall", "endassociate", "endblock", "endcritical",
                                                  "endteam", "endblockdata", nullptr};
            if (!Has(kJoined, w)) {
                return Fail(k, "Unclassifiable statement at (1)");
            }
        }
        if (action) return Fail(k, "Unclassifiable statement at (1)");
        return End(k);
    }
    // ---- specification statements ----
    if (w == "use" && !action) return Use(k);
    if (w == "import" && !action) {
        st_.kind = StmtKind::Import;
        size_t j = k + 1;
        if (IsOp(j, ",")) j += 2;
        if (IsOp(j, "::")) j++;
        if (j < N() && !EntityNames(j, N(), false)) return FailStmt(j, "IMPORT");
        for (const Entity &e : st_.entities) {
            NameRef r;
            r.name = e.name;
            r.pos = e.pos;
            r.end = e.end;
            st_.refs.push_back(r);
        }
        st_.entities.clear();
        return true;
    }
    if (w == "implicit" && !action) {
        st_.kind = StmtKind::Implicit;
        if (IsKw(k + 1, "none")) {
            st_.implicit_none = true;
            size_t j = k + 2;
            if (j == N()) return true;
            if (!IsOp(j, "(")) return FailStmt(j, "IMPLICIT");
            size_t c = Close(j);
            if (c + 1 != N()) return FailStmt(j, "IMPLICIT");
            if (Text(j, c).find("external") != std::string::npos) st_.implicit_none_external = true;
            return true;
        }
        // implicit real*8 (a-h, o-z), integer (i-n)
        size_t j = k + 1;
        while (j < N()) {
            size_t tl = TypeSpecLen(j);
            if (!tl) {
                if (IsKw(j, "type") && IsOp(j + 1, "(")) tl = Close(j + 1) - j + 1;
                else return FailStmt(j, "IMPLICIT");
            }
            j += tl;
            // the letter list is the last (...) group
            if (!IsOp(j, "(")) {
                // `implicit real (a-h)`: TypeSpecLen ate the letters as a kind selector
                if (j > k && IsOp(j - 1, ")")) {
                    if (j == N() || IsOp(j, ",")) {
                        if (j < N()) j++;
                        continue;
                    }
                }
                return FailStmt(j, "IMPLICIT");
            }
            size_t c = Close(j);
            if (c >= N()) return FailStmt(j, "IMPLICIT");
            j = c + 1;
            if (j < N()) {
                if (!IsOp(j, ",")) return FailStmt(j, "IMPLICIT");
                j++;
            }
        }
        return true;
    }
    if (!action) {
        if (w == "type" && (IsKw(k + 1, "is"))) {
            st_.kind = StmtKind::TypeGuard;
            return SimpleBlock(k + 2, StmtKind::TypeGuard, true);
        }
        if (w == "typeis") return SimpleBlock(k + 1, StmtKind::TypeGuard, true);
        if (w == "class" && (IsKw(k + 1, "is") || IsKw(k + 1, "default"))) {
            if (IsKw(k + 1, "default")) return SimpleBlock(k + 2, StmtKind::TypeGuard, false);
            return SimpleBlock(k + 2, StmtKind::TypeGuard, true);
        }
        if (w == "classis") return SimpleBlock(k + 1, StmtKind::TypeGuard, true);
        if (w == "classdefault") return SimpleBlock(k + 1, StmtKind::TypeGuard, false);
        if (w == "type" && !IsOp(k + 1, "(")) return DerivedType(k);
        size_t tl = TypeSpecLen(k);
        if (tl) return Decl(k, tl);
        if (w == "procedure" && (IsOp(k + 1, "::") || IsOp(k + 1, ",") || IsName(k + 1))) {
            st_.kind = StmtKind::Procedure;
            size_t j = k + 1;
            size_t dc = FindTop(j, N(), "::");
            if (dc < N()) j = dc + 1;
            // binding: name [=> target] {, ...}
            while (j < N()) {
                if (!IsName(j)) return FailStmt(j, "PROCEDURE");
                st_.entities.push_back(MakeEntity(j));
                j++;
                if (IsOp(j, "=>")) {
                    if (!IsName(j + 1)) return FailStmt(j + 1, "PROCEDURE");
                    AddRef(j + 1, RefCtx::Call);
                    j += 2;
                }
                if (j < N()) {
                    if (!IsOp(j, ",")) return FailStmt(j, "PROCEDURE");
                    j++;
                }
            }
            return true;
        }
        if (w == "generic") {
            st_.kind = StmtKind::Generic;
            size_t dc = FindTop(k, N(), "::");
            size_t ar = FindTop(k, N(), "=>");
            if (dc >= N() || ar >= N()) return FailStmt(k + 1, "GENERIC");
            if (IsName(dc + 1)) {
                st_.unit.name = t_[dc + 1].lower;
                st_.unit.name_pos = t_[dc + 1].pos;
                st_.unit.name_end = t_[dc + 1].end;
            }
            for (size_t q = ar + 1; q < N(); q++) {
                if (IsName(q)) AddRef(q, RefCtx::Generic);
            }
            return true;
        }
        if (w == "final") {
            st_.kind = StmtKind::Final;
            size_t j = k + 1;
            if (IsOp(j, "::")) j++;
            for (; j < N(); j++) {
                if (IsName(j)) AddRef(j, RefCtx::Call);
            }
            return true;
        }
        if (w == "contains") {
            st_.kind = StmtKind::Contains;
            return k + 1 == N() ? true : FailStmt(k + 1, "CONTAINS");
        }
        if (w == "sequence") {
            st_.kind = StmtKind::Sequence;
            return true;
        }
        if (w == "interface") return Interface(k, false);
        if (w == "abstract" && IsKw(k + 1, "interface")) return Interface(k + 1, true);
        if (w == "enum") {
            st_.kind = StmtKind::Enum;
            return true;
        }
        if (w == "enumerator") {
            st_.kind = StmtKind::Enumerator;
            size_t j = k + 1;
            if (IsOp(j, "::")) j++;
            if (!EntityNames(j, N(), true)) return FailStmt(j, "ENUMERATOR");
            return true;
        }
        if (w == "parameter") {
            st_.kind = StmtKind::Parameter;
            if (!IsOp(k + 1, "(")) {
                // legacy `parameter pi = 3.14, e = 2.7` (no parentheses)
                if (!IsName(k + 1) || !IsOp(k + 2, "=")) return FailStmt(k + 1, "PARAMETER");
                for (size_t q = k + 1; q < N();) {
                    if (!IsName(q) || !IsOp(q + 1, "=")) return FailStmt(q, "PARAMETER");
                    Entity e = MakeEntity(q);
                    e.init = true;
                    st_.entities.push_back(e);
                    size_t comma = FindTop(q + 2, N(), ",");
                    if (comma == q + 2 || !Scan(q + 2, comma)) return FailStmt(q, "PARAMETER");
                    q = comma + 1;
                }
                return true;
            }
            size_t c = Close(k + 1);
            if (c >= N() || c + 1 != N()) return FailStmt(k + 1, "PARAMETER");
            for (size_t q = k + 2; q < c;) {
                if (!IsName(q) || !IsOp(q + 1, "=")) return FailStmt(q, "PARAMETER");
                Entity e = MakeEntity(q);
                e.init = true;
                st_.entities.push_back(e);
                size_t comma = FindTop(q + 2, c, ",");
                if (comma == q + 2 || !Scan(q + 2, comma)) return FailStmt(q, "PARAMETER");
                q = comma + 1;
            }
            // the names are refs too (they are declared elsewhere with a type)
            return true;
        }
        if (w == "common") {
            st_.kind = StmtKind::Common;
            size_t j = k + 1;
            while (j < N()) {
                if (IsOp(j, "//")) {
                    st_.group = "";
                    j++;
                } else if (IsOp(j, "/")) {
                    if (IsName(j + 1) && IsOp(j + 2, "/")) {
                        st_.group = t_[j + 1].lower;
                        j += 3;
                    } else if (IsOp(j + 1, "/")) {
                        j += 2;
                    } else {
                        return FailStmt(j, "COMMON");
                    }
                }
                if (!IsName(j)) return FailStmt(j, "COMMON");
                Entity e = MakeEntity(j);
                j++;
                if (IsOp(j, "(")) {
                    size_t c = Close(j);
                    if (c >= N()) return FailStmt(j, "COMMON");
                    e.array = true;
                    Scan(j + 1, c);
                    j = c + 1;
                }
                st_.entities.push_back(e);
                if (IsOp(j, ",")) j++;
                else if (j < N() && !IsOp(j, "/") && !IsOp(j, "//")) return FailStmt(j, "COMMON");
            }
            return true;
        }
        if (w == "namelist") {
            st_.kind = StmtKind::Namelist;
            size_t j = k + 1;
            while (j < N()) {
                if (!IsOp(j, "/") || !IsName(j + 1) || !IsOp(j + 2, "/")) return FailStmt(j, "NAMELIST");
                if (st_.group.empty()) st_.group = t_[j + 1].lower;
                j += 3;
                while (j < N() && IsName(j)) {
                    AddRef(j, RefCtx::Read);
                    j++;
                    if (IsOp(j, ",")) j++;
                }
            }
            return true;
        }
        if (w == "equivalence") {
            st_.kind = StmtKind::Equivalence;
            return ScanAll(k + 1) ? true : FailStmt(k + 1, "EQUIVALENCE");
        }
        if (w == "dimension" || w == "allocatable" || w == "pointer" || w == "target" || w == "optional" ||
            w == "save" || w == "external" || w == "intrinsic" || w == "public" || w == "private" || w == "value" ||
            w == "volatile" || w == "protected" || w == "contiguous" || w == "asynchronous" || w == "intent" ||
            w == "bind" || w == "codimension" || w == "static" || w == "automatic") {
            st_.kind = StmtKind::Attribute;
            size_t j = k + 1;
            if ((w == "intent" || w == "bind") && IsOp(j, "(")) {
                size_t c = Close(j);
                if (c >= N()) return FailStmt(j, Upper(w).c_str());
                j = c + 1;
            }
            if (IsOp(j, "::")) j++;
            if (j == N()) {
                if (w == "save" || w == "public" || w == "private" || w == "sequence") return true;
                return FailStmt(j, Upper(w).c_str());
            }
            if (w == "private" && st_.kind == StmtKind::Attribute && j == k + 1 && j == N())
                st_.kind = StmtKind::PrivateComponents;
            // save /blk/; public operator(+)
            for (size_t q = j; q < N(); q++) {
                if (IsName(q) && !(IsKw(q, "operator") || IsKw(q, "assignment")) && !(q > j && IsOp(q - 1, "/"))) {
                    if (q == j || IsOp(q - 1, ",")) st_.entities.push_back(MakeEntity(q));
                }
            }
            if (w == "dimension" || w == "allocatable" || w == "pointer" || w == "target" || w == "codimension") {
                if (!EntityNames(j, N(), false)) {
                    // pointer (p, t): Cray pointers
                    if (!Scan(j, N())) return FailStmt(j, Upper(w).c_str());
                }
                st_.entities.erase(std::unique(st_.entities.begin(), st_.entities.end(),
                                               [](const Entity &a, const Entity &b) { return a.pos == b.pos; }),
                                   st_.entities.end());
            } else if (!Scan(j, N())) {
                return FailStmt(j, Upper(w).c_str());
            }
            return true;
        }
        if (w == "data") {
            st_.kind = StmtKind::Data;
            // data obj-list /values/ [,] ...
            size_t j = k + 1;
            while (j < N()) {
                size_t s = j;
                int d = 0;
                while (j < N() && !(d == 0 && IsOp(j, "/"))) {
                    if (IsOp(j, "(")) d++;
                    else if (IsOp(j, ")")) d--;
                    j++;
                }
                if (j >= N() || j == s) return FailStmt(j, "DATA");
                Scan(s, j, RefCtx::Write);
                j++;
                size_t e = j;
                while (e < N() && !IsOp(e, "/")) e++;
                if (e >= N()) return FailStmt(e, "DATA");
                Scan(j, e);
                j = e + 1;
                if (IsOp(j, ",")) j++;
            }
            return true;
        }
        if (w == "format") {
            st_.kind = StmtKind::Format;
            if (!IsOp(k + 1, "(") && !IsOp(k + 1, "(/"))
                return Fail(k + 1, "Missing leading left parenthesis in format string at (1)");
            return true;
        }
        if (w == "entry") {
            st_.kind = StmtKind::Entry;
            if (!IsName(k + 1)) return FailStmt(k + 1, "ENTRY");
            st_.unit.name = t_[k + 1].lower;
            st_.unit.spelling = t_[k + 1].text;
            st_.unit.name_pos = t_[k + 1].pos;
            st_.unit.name_end = t_[k + 1].end;
            return true;
        }
        if (w == "include") {
            st_.kind = StmtKind::Include;
            return true;
        }
    }
    // ---- executable statements ----
    if (w == "call") {
        st_.kind = StmtKind::Call;
        size_t j = k + 1;
        if (!IsName(j)) return FailStmt(j, "CALL");
        size_t e = j + 1;
        for (;;) {
            if (IsOp(e, "(") || IsOp(e, "[")) {
                size_t c = Close(e);
                if (c >= N()) return FailStmt(e, "CALL");
                e = c + 1;
            } else if (IsOp(e, "%") && IsName(e + 1)) {
                e += 2;
            } else {
                break;
            }
        }
        if (e != N()) return FailStmt(e, "CALL");
        Scan(j, N(), RefCtx::Call);
        // alternate-return labels: call s(x, *10)
        for (size_t q = j; q + 1 < N(); q++) {
            if (IsOp(q, "*") && Tok(q + 1)->kind == TokKind::Int && (IsOp(q - 1, ",") || IsOp(q - 1, "(")))
                AddLabel(q + 1);
        }
        // the procedure itself may be a component: call obj%method()
        return true;
    }
    if (w == "if") return If(k);
    if (w == "elseif" || (w == "else" && IsKw(k + 1, "if"))) {
        if (action) return Fail(k, "Unclassifiable statement at (1)");
        size_t j = w == "elseif" ? k : k + 1;
        if (!IsOp(j + 1, "(")) return FailStmt(j + 1, "ELSE IF");
        size_t c = Close(j + 1);
        if (c >= N()) return FailStmt(j + 1, "ELSE IF");
        Scan(j + 2, c);
        st_.kind = StmtKind::ElseIf;
        if (c + 1 == N()) return Fail(c + 1, "Syntax error in ELSE IF statement at (1)");
        if (!IsKw(c + 1, "then") || (c + 2 != N() && !IsName(c + 2))) return Fail(c + 1, "Syntax error in ELSE IF statement at (1)");
        return true;
    }
    if (w == "else" && !action) {
        if (IsKw(k + 1, "where") || IsKw(k + 1, "elsewhere")) return SimpleBlock(k + 2, StmtKind::ElseWhere, false);
        st_.kind = StmtKind::Else;
        if (k + 1 != N() && !(IsName(k + 1) && k + 2 == N())) return Fail(k + 1, "Unexpected junk after ELSE statement at (1)");
        return true;
    }
    if (w == "elsewhere" && !action) return SimpleBlock(k + 1, StmtKind::ElseWhere, false);
    if ((w == "do" || w == "dowhile" || w == "doconcurrent") && !action) {
        if (w != "do") {
            // split the joined word
            Token rest = t_[k];
            rest.lower = w.substr(2);
            rest.text = rest.text.substr(2);
            t_[k].lower = "do";
            t_[k].text = t_[k].text.substr(0, 2);
            t_.insert(t_.begin() + static_cast<std::ptrdiff_t>(k + 1), rest);
        }
        return Do(k);
    }
    if (w == "cycle" || w == "exit") {
        st_.kind = w == "cycle" ? StmtKind::Cycle : StmtKind::Exit;
        if (k + 1 < N()) {
            if (!IsName(k + 1) || k + 2 != N()) return FailStmt(k + 1, Upper(w).c_str());
            AddRef(k + 1, RefCtx::Construct);
        }
        return true;
    }
    if (w == "continue") {
        st_.kind = StmtKind::Continue;
        return k + 1 == N() ? true : Fail(k + 1, "Unclassifiable statement at (1)");
    }
    if ((n2 = Kw2(k, "go", "to")) != 0) {
        size_t j = k + n2;
        if (Tok(j) && Tok(j)->kind == TokKind::Int) {
            st_.kind = StmtKind::Goto;
            AddLabel(j);
            return j + 1 == N() ? true : FailStmt(j + 1, "GOTO");
        }
        if (IsOp(j, "(")) {
            st_.kind = StmtKind::ComputedGoto;
            size_t c = Close(j);
            if (c >= N() || !LabelList(j + 1, c)) return FailStmt(j, "GOTO");
            j = c + 1;
            if (IsOp(j, ",")) j++;
            if (j == N() || !Scan(j, N())) return FailStmt(j, "GOTO");
            return true;
        }
        if (IsName(j)) {
            st_.kind = StmtKind::AssignedGoto;
            AddRef(j, RefCtx::Read);
            return true;
        }
        return FailStmt(j, "GOTO");
    }
    if (w == "assign") {
        st_.kind = StmtKind::Assign;
        if (Tok(k + 1) && Tok(k + 1)->kind == TokKind::Int) AddLabel(k + 1);
        // assign 10 to i (fixed form may glue `TOI`)
        for (size_t q = k + 2; q < N(); q++) {
            if (IsName(q) && t_[q].lower != "to") AddRef(q, RefCtx::Write);
        }
        return true;
    }
    if (((n2 = Kw2(k, "select", "case")) != 0) && !action) {
        if (!IsOp(k + n2, "(")) return Fail(k + n2, "Syntax error in SELECT statement at (1)");
        return SimpleBlock(k + n2, StmtKind::SelectCase, true);
    }
    if (((n2 = Kw2(k, "select", "type")) != 0) && !action) {
        st_.kind = StmtKind::SelectType;
        size_t j = k + n2;
        if (!IsOp(j, "(")) return FailStmt(j, "SELECT TYPE");
        size_t c = Close(j);
        if (c >= N() || c + 1 != N()) return FailStmt(j, "SELECT TYPE");
        if (IsName(j + 1) && IsOp(j + 2, "=>")) {
            st_.entities.push_back(MakeEntity(j + 1));
            Scan(j + 3, c);
        } else {
            Scan(j + 1, c);
        }
        return true;
    }
    if (((n2 = Kw2(k, "select", "rank")) != 0) && !action) {
        st_.kind = StmtKind::SelectRank;
        size_t j = k + n2;
        if (!IsOp(j, "(")) return FailStmt(j, "SELECT RANK");
        size_t c = Close(j);
        if (c >= N()) return FailStmt(j, "SELECT RANK");
        if (IsName(j + 1) && IsOp(j + 2, "=>")) {
            st_.entities.push_back(MakeEntity(j + 1));
            Scan(j + 3, c);
        } else {
            Scan(j + 1, c);
        }
        return true;
    }
    if (w == "case" && !action) {
        st_.kind = StmtKind::Case;
        if (IsKw(k + 1, "default")) return k + 2 == N() || (IsName(k + 2) && k + 3 == N()) ? true : FailStmt(k + 2, "CASE");
        if (!IsOp(k + 1, "(")) return FailStmt(k + 1, "CASE");
        size_t c = Close(k + 1);
        if (c >= N()) return FailStmt(k + 1, "CASE");
        Scan(k + 2, c);
        if (c + 1 != N() && !(IsName(c + 1) && c + 2 == N())) return FailStmt(c + 1, "CASE");
        return true;
    }
    if (w == "casedefault" && !action) {
        st_.kind = StmtKind::Case;
        return true;
    }
    if (w == "rank" && IsOp(k + 1, "(") && !action) return SimpleBlock(k + 1, StmtKind::RankGuard, true);
    if (w == "rank" && IsKw(k + 1, "default") && !action) return SimpleBlock(k + 2, StmtKind::RankGuard, false);
    if (w == "where") {
        if (!IsOp(k + 1, "(")) return FailStmt(k + 1, "WHERE");
        size_t c = Close(k + 1);
        if (c >= N()) return FailStmt(k + 1, "WHERE");
        Scan(k + 2, c);
        if (c + 1 == N()) {
            if (action) return FailStmt(c + 1, "WHERE");
            st_.kind = StmtKind::Where;
            return true;
        }
        st_.kind = StmtKind::WhereStmt;
        if (!Assignment(c + 1)) return msg.empty() ? FailStmt(c + 1, "WHERE") : false;
        st_.kind = StmtKind::WhereStmt;
        return true;
    }
    if (w == "forall") {
        if (!IsOp(k + 1, "(")) return FailStmt(k + 1, "FORALL");
        size_t c = Close(k + 1);
        if (c >= N()) return FailStmt(k + 1, "FORALL");
        for (size_t q = k + 2; q < c; q++) {
            if (IsName(q) && IsOp(q + 1, "=") && (q == k + 2 || IsOp(q - 1, ","))) AddRef(q, RefCtx::Write);
        }
        Scan(k + 2, c);
        if (c + 1 == N()) {
            if (action) return FailStmt(c + 1, "FORALL");
            st_.kind = StmtKind::Forall;
            return true;
        }
        st_.kind = StmtKind::ForallStmt;
        if (!Assignment(c + 1)) return msg.empty() ? FailStmt(c + 1, "FORALL") : false;
        st_.kind = StmtKind::ForallStmt;
        return true;
    }
    if (w == "associate" && !action) {
        st_.kind = StmtKind::Associate;
        if (!IsOp(k + 1, "(")) return FailStmt(k + 1, "ASSOCIATE");
        size_t c = Close(k + 1);
        if (c >= N() || c + 1 != N()) return FailStmt(k + 1, "ASSOCIATE");
        for (size_t q = k + 2; q < c;) {
            if (!IsName(q) || !IsOp(q + 1, "=>")) return Fail(q, "Invalid association target at (1)");
            st_.entities.push_back(MakeEntity(q));
            size_t comma = FindTop(q + 2, c, ",");
            if (comma == q + 2) return Fail(q, "Invalid association target at (1)");
            Scan(q + 2, comma);
            q = comma + 1;
        }
        return true;
    }
    if (w == "block" && !action) {
        st_.kind = StmtKind::Block;
        return k + 1 == N() ? true : Fail(k + 1, "Unclassifiable statement at (1)");
    }
    if (w == "critical" && !action) return SimpleBlock(k + 1, StmtKind::Critical, false);
    if ((n2 = Kw2(k, "change", "team")) != 0 && !action) return SimpleBlock(k + n2, StmtKind::ChangeTeam, true);
    if ((n2 = Kw2(k, "error", "stop")) != 0 || w == "stop" || w == "pause") {
        st_.kind = w == "pause" ? StmtKind::Pause : n2 ? StmtKind::ErrorStop : StmtKind::Stop;
        size_t j = k + (n2 ? n2 : 1);
        if (j < N() && !ScanAll(j)) return FailStmt(j, w == "pause" ? "PAUSE" : "STOP");
        return true;
    }
    if (w == "return") {
        st_.kind = StmtKind::Return;
        if (k + 1 < N() && !ScanAll(k + 1)) return FailStmt(k + 1, "RETURN");
        return true;
    }
    if (w == "allocate" || w == "deallocate" || w == "nullify") {
        st_.kind = w == "allocate" ? StmtKind::Allocate : w == "deallocate" ? StmtKind::Deallocate : StmtKind::Nullify;
        if (!IsOp(k + 1, "(")) return FailStmt(k + 1, Upper(w).c_str());
        size_t c = Close(k + 1);
        if (c >= N() || c + 1 != N()) return FailStmt(k + 1, Upper(w).c_str());
        size_t s = k + 2;
        if (w == "allocate") {
            size_t tl = TypeSpecLen(s);
            size_t dc = FindTop(s, c, "::");
            if (dc < c) s = dc + 1;
            else if (tl && IsOp(s + tl, "::")) s += tl + 1;
        }
        // objects are written; stat=/errmsg=/source=/mold= keywords
        Scan(s, c, RefCtx::Write);
        for (size_t q = s; q < c; q++) {
            if (IsName(q) && (q == s || IsOp(q - 1, ","))) {
                bool kw = IsOp(q + 1, "=");
                for (NameRef &r : st_.refs) {
                    if (r.pos == t_[q].pos) r.ctx = kw ? RefCtx::Keyword : RefCtx::Write;
                }
            }
        }
        return true;
    }
    static const char *const kIo[] = {"read", "write", "print", "open", "close", "inquire", "backspace",
                                      "endfile", "rewind", "flush", "wait", nullptr};
    if (Has(kIo, w)) return Io(k, w);
    if (w == "sync" || w == "lock" || w == "unlock" || w == "event" || w == "fail" || w == "form" ||
        w == "notify" || w == "syncall" || w == "syncimages" || w == "syncmemory") {
        st_.kind = w == "lock" || w == "unlock" ? StmtKind::Lock : w == "event" ? StmtKind::Event
                 : w == "fail" ? StmtKind::FailImage : w == "form" ? StmtKind::FormTeam : StmtKind::Sync;
        return ScanAll(k + 1) ? true : FailStmt(k + 1, Upper(w).c_str());
    }
    if (action) {
        // a declaration or block statement after IF (...)
        if (TypeSpecLen(k) || w == "do" || w == "select" || w == "case" || w == "else" || w == "block" ||
            w == "use" || w == "implicit" || w == "contains" || w == "program" || w == "subroutine" ||
            w == "function" || w == "module")
            return Fail(k, "Syntax error in IF-clause after (1)");
    }
    // A bare name: a macro the reader could not see (an unresolved #include)?
    if (lenient_) {
        ScanAll(k);
        st_.kind = StmtKind::Unknown;
        return true;
    }
    return Fail(k, "Unclassifiable statement at (1)");
}

bool StmtParser::Run() {
    size_t k = st_.first;
    if (k >= N()) {
        st_.kind = StmtKind::Unknown;
        return true;
    }
    // `name:` construct name
    if (IsName(k) && IsOp(k + 1, ":") && !IsOp(k + 2, ":")) {
        st_.construct = t_[k].lower;
        st_.construct_pos = t_[k].pos;
        k += 2;
        st_.first = k;
        if (k >= N()) return Fail(k, "Unclassifiable statement at (1)");
    }
    return Classify(k, false);
}

// ---- structure ----------------------------------------------------------------------

enum Phase { kPhaseUse, kPhaseImport, kPhaseImplicit, kPhaseSpec, kPhaseExec, kPhaseContains };

struct Frame {
    int construct = -1;
    StmtKind kind = StmtKind::Unknown;
    int phase = kPhaseUse;
    std::string name;      // unit name / construct name (lower)
    std::string do_label;  // nonblock DO: the terminating label
    bool implicit_program = false;
    bool saw_else = false;
    bool saw_case = false;
    std::set<std::string> arrays;  // names declared with an array spec (not statement functions)
};

class Structure {
public:
    explicit Structure(Source *src) : s_(src) {}

    void Run();

private:
    void Diag(const Stmt &st, Pos at, const std::string &m) {
        // An included file the reader cannot see may open or close anything.
        if (s_->unresolved_include) return;
        fortran::Diag d;
        d.pos = at;
        d.end = at.line == st.end.line && at.col < st.end.col ? st.end : Pos{at.line, at.col + 1};
        d.message = m;
        d.code = "syntax-error";
        s_->diags.push_back(d);
    }
    int Open(StmtKind kind, int si, const std::string &name) {
        Construct c;
        c.kind = kind;
        c.open = si;
        c.parent = stack_.empty() ? 0 : stack_.back().construct;
        int id = static_cast<int>(s_->constructs.size());
        s_->constructs.push_back(c);
        s_->constructs[static_cast<size_t>(c.parent)].children.push_back(id);
        Frame f;
        f.construct = id;
        f.kind = kind;
        f.name = name;
        stack_.push_back(f);
        return id;
    }
    void Close(int si) {
        s_->constructs[static_cast<size_t>(stack_.back().construct)].close = si;
        stack_.pop_back();
    }
    Frame *Unit() {
        for (size_t i = stack_.size(); i-- > 0;) {
            StmtKind k = stack_[i].kind;
            if (IsUnitKind(k) || k == StmtKind::Interface || k == StmtKind::DerivedType || k == StmtKind::Block)
                return &stack_[i];
        }
        return nullptr;
    }
    void Statement(int si);
    void EndStmt(int si);
    void TerminateDo(int si);

    Source *s_;
    std::vector<Frame> stack_;
};

void Structure::Run() {
    Construct file;
    file.kind = StmtKind::Unknown;
    file.open = 0;
    s_->constructs.push_back(file);
    s_->construct_of.assign(s_->stmts.size(), 0);
    for (size_t i = 0; i < s_->stmts.size(); i++) {
        int before = stack_.empty() ? 0 : stack_.back().construct;
        Statement(static_cast<int>(i));
        // an END statement belongs to the construct it closes
        s_->construct_of[i] = s_->stmts[i].kind == StmtKind::End ? before
                                                                 : (stack_.empty() ? 0 : stack_.back().construct);
    }
    // a file of declarations and nothing else is an include fragment, not a program
    bool fragment = stack_.size() == 1 && stack_.back().implicit_program && stack_.back().phase < kPhaseExec;
    if (!stack_.empty() && !s_->unresolved_include && !fragment) {
        const Frame &f = stack_.back();
        Pos end;
        if (!s_->stmts.empty()) end = s_->stmts.back().end;
        fortran::Diag d;
        d.pos = end;
        d.end = end;
        if (IsUnitKind(f.kind) || f.implicit_program) {
            d.message = "Unexpected end of file";
        } else {
            d.message = "Unexpected end of file";
        }
        d.code = "syntax-error";
        s_->diags.push_back(d);
        s_->constructs[0].close = static_cast<int>(s_->stmts.size()) - 1;
    }
}

void Structure::TerminateDo(int si) {
    const Stmt &st = s_->stmts[static_cast<size_t>(si)];
    if (st.label.empty()) return;
    std::string lab = StripZeros(st.label);
    // A labeled statement ends every nonblock DO with that label (they may share it).
    bool any = false;
    for (const Frame &f : stack_) any = any || f.do_label == lab;
    if (!any) return;
    while (!stack_.empty() && stack_.back().kind == StmtKind::Do && stack_.back().do_label == lab) Close(si);
}

void Structure::Statement(int si) {
    Stmt &st = s_->stmts[static_cast<size_t>(si)];
    StmtKind k = st.kind;
    if (k == StmtKind::Unknown && st.toks.empty()) return;
    // `module procedure a` inside an interface block lists, it does not open
    if (k == StmtKind::ModuleProcedureUnit && !stack_.empty() && stack_.back().kind == StmtKind::Interface) {
        st.kind = StmtKind::ModuleProcedureList;
        Entity e;
        e.name = st.unit.name;
        e.spelling = st.unit.spelling;
        e.pos = st.unit.name_pos;
        e.end = st.unit.name_end;
        st.entities.push_back(e);
        return;
    }
    // Program units at file level (or after CONTAINS / in an interface body)
    if (IsUnitKind(k)) {
        Frame *u = Unit();
        bool ok = stack_.empty() || (u && u == &stack_.back() &&
                                     ((IsUnitKind(u->kind) && u->phase == kPhaseContains) ||
                                      u->kind == StmtKind::Interface));
        if (k == StmtKind::Program || k == StmtKind::Module || k == StmtKind::Submodule || k == StmtKind::BlockData) {
            if (!stack_.empty()) {
                // gfortran: a module/program inside another unit means the outer one never ended
                Diag(st, st.pos, "Unexpected " + std::string(StmtKindName(k)) + " statement at (1)");
                return;
            }
        } else if (!ok) {
            if (u && u == &stack_.back() && IsUnitKind(u->kind)) {
                Diag(st, st.pos, std::string("Unexpected ") + StmtKindName(k) + " statement at (1)");
            } else if (!stack_.empty()) {
                Diag(st, st.pos, "Unexpected " + std::string(StmtKindName(k)) + " statement at (1)");
            }
            return;
        }
        Open(k, si, st.unit.name);
        return;
    }
    if (k == StmtKind::End) {
        EndStmt(si);
        return;
    }
    if (k == StmtKind::Include) return;  // the reader does not open included files
    // Anything else at file level starts a main program without a PROGRAM statement.
    if (stack_.empty()) {
        Open(StmtKind::Program, si, "");
        stack_.back().implicit_program = true;
    }
    Frame *u = Unit();
    Frame &top = stack_.back();
    // Inside a derived type / interface body / enum
    if (top.kind == StmtKind::DerivedType) {
        if (k == StmtKind::Contains) {
            top.phase = kPhaseContains;
        }
        return;  // components, bindings: not checked
    }
    if (top.kind == StmtKind::Interface) {
        if (k == StmtKind::ModuleProcedureList || k == StmtKind::Procedure) return;
        Diag(st, st.pos, "Unexpected " + std::string(k == StmtKind::TypeDecl ? "data declaration" : "") +
                             (k == StmtKind::TypeDecl ? "" : "") + " statement in INTERFACE block at (1)");
        return;
    }
    if (top.kind == StmtKind::Enum) {
        if (k != StmtKind::Enumerator) Diag(st, st.pos, "Unexpected statement in ENUM block at (1)");
        return;
    }
    // `f(x, y) = expr` before the first executable statement: a statement function
    if (k == StmtKind::Assignment && u && IsUnitKind(u->kind) && u->phase < kPhaseExec && &top == u) {
        size_t j = st.first + 1;
        bool sf = j < st.toks.size() && st.toks[j].text == "(";
        for (j++; sf && j < st.toks.size(); j += 2) {
            if (st.toks[j].kind != TokKind::Name && !(st.toks[j].text == ")" && j == st.first + 2)) sf = false;
            else if (st.toks[j].text == ")") break;
            else if (j + 1 < st.toks.size() && st.toks[j + 1].text == ")") {
                j++;
                break;
            } else if (j + 1 >= st.toks.size() || st.toks[j + 1].text != ",") sf = false;
        }
        if (sf && j + 1 < st.toks.size() && st.toks[j + 1].text == "=" && !u->arrays.count(st.toks[st.first].lower)) {
            st.kind = k = StmtKind::StatementFunction;
        }
    }
    if (u && (k == StmtKind::TypeDecl || k == StmtKind::Attribute || k == StmtKind::Common)) {
        for (const Entity &e : st.entities) {
            if (e.array || st.attrs & kAttrDimension) u->arrays.insert(e.name);
        }
    }
    // phases
    bool spec = k == StmtKind::Use || k == StmtKind::Import || k == StmtKind::Implicit || k == StmtKind::TypeDecl ||
                k == StmtKind::DerivedType || k == StmtKind::Procedure || k == StmtKind::Interface ||
                k == StmtKind::Parameter || k == StmtKind::Common || k == StmtKind::Equivalence ||
                k == StmtKind::Namelist || k == StmtKind::Attribute || k == StmtKind::Enum ||
                k == StmtKind::StatementFunction || k == StmtKind::Generic;
    bool anywhere = k == StmtKind::Format || k == StmtKind::Entry || k == StmtKind::Data || k == StmtKind::Include ||
                    k == StmtKind::Unknown;
    if (u && u->phase == kPhaseContains && k != StmtKind::Contains) {
        if (k == StmtKind::TypeDecl) Diag(st, st.pos, "Unexpected data declaration statement in CONTAINS section at (1)");
        else if (k == StmtKind::Use) Diag(st, st.pos, "Unexpected USE statement in CONTAINS section at (1)");
        else if (k == StmtKind::Assignment) Diag(st, st.pos, "Unexpected assignment statement in CONTAINS section at (1)");
        else if (!anywhere) Diag(st, st.pos, "Unexpected statement in CONTAINS section at (1)");
        return;
    }
    if (u && spec) {
        bool in_block_construct = &top != u && !IsUnitKind(top.kind);
        if (k == StmtKind::Use) {
            if (u->phase > kPhaseUse && !in_block_construct) Diag(st, st.pos, "Unexpected USE statement at (1)");
        } else if (k == StmtKind::Implicit) {
            if (u->phase > kPhaseImplicit) {
                Diag(st, st.pos, "Unexpected IMPLICIT statement at (1)");
            } else {
                u->phase = kPhaseImplicit;
            }
        } else if (k == StmtKind::Import) {
            if (u->phase < kPhaseImport) u->phase = kPhaseImport;
        } else if (u->phase == kPhaseExec || in_block_construct) {
            if (k == StmtKind::TypeDecl) Diag(st, st.pos, "Unexpected data declaration statement at (1)");
        } else {
            u->phase = kPhaseSpec;
        }
        if (k == StmtKind::Use && u->phase < kPhaseUse) u->phase = kPhaseUse;
    }
    if (k == StmtKind::Contains) {
        if (!u || !IsUnitKind(u->kind) || &top != u) {
            Diag(st, st.pos, "Unexpected CONTAINS statement at (1)");
            return;
        }
        u->phase = kPhaseContains;
        s_->constructs[static_cast<size_t>(u->construct)].contains = si;
        return;
    }
    if (!spec && !anywhere && u && IsUnitKind(u->kind)) {
        if (u->kind == StmtKind::Module || u->kind == StmtKind::Submodule || u->kind == StmtKind::BlockData) {
            Diag(st, st.pos, std::string("Unexpected ") + (k == StmtKind::Assignment ? "assignment" : "executable") +
                                 " statement at (1)");
            return;
        }
        u->phase = kPhaseExec;
    }
    // block constructs
    switch (k) {
        case StmtKind::If:
        case StmtKind::SelectCase:
        case StmtKind::SelectType:
        case StmtKind::SelectRank:
        case StmtKind::Where:
        case StmtKind::Forall:
        case StmtKind::Associate:
        case StmtKind::Critical:
        case StmtKind::ChangeTeam:
        case StmtKind::Interface:
        case StmtKind::Enum:
        case StmtKind::DerivedType:
            Open(k, si, st.construct.empty() ? st.unit.name : st.construct);
            if (k == StmtKind::DerivedType || k == StmtKind::Interface) stack_.back().name = st.unit.name;
            else stack_.back().name = st.construct;
            return;
        case StmtKind::Block:
            Open(k, si, st.construct);
            stack_.back().phase = kPhaseUse;
            return;
        case StmtKind::Do:
            Open(k, si, st.construct);
            stack_.back().do_label = st.do_label;
            return;
        case StmtKind::ElseIf:
        case StmtKind::Else:
            if (top.kind != StmtKind::If) {
                Diag(st, st.pos, k == StmtKind::Else ? "Unexpected ELSE statement at (1)"
                                                     : "Unexpected ELSE IF statement at (1)");
                return;
            }
            if (top.saw_else) {
                Diag(st, st.pos, k == StmtKind::Else ? "Duplicate ELSE statements at (1) and (2)"
                                                     : "ELSE IF statement at (1) cannot follow ELSE statement at (2)");
                return;
            }
            if (k == StmtKind::Else) top.saw_else = true;
            break;
        case StmtKind::ElseWhere:
            if (top.kind != StmtKind::Where) Diag(st, st.pos, "Unexpected ELSEWHERE statement at (1)");
            break;
        case StmtKind::Case:
            if (top.kind != StmtKind::SelectCase) Diag(st, st.pos, "Unexpected CASE statement at (1)");
            top.saw_case = true;
            break;
        case StmtKind::TypeGuard:
            if (top.kind != StmtKind::SelectType) Diag(st, st.pos, "Unexpected TYPE IS statement at (1)");
            top.saw_case = true;
            break;
        case StmtKind::RankGuard:
            if (top.kind != StmtKind::SelectRank) Diag(st, st.pos, "Unexpected RANK statement at (1)");
            top.saw_case = true;
            break;
        default:
            if ((top.kind == StmtKind::SelectCase || top.kind == StmtKind::SelectType ||
                 top.kind == StmtKind::SelectRank) && !top.saw_case) {
                Diag(st, st.pos,
                     top.kind == StmtKind::SelectCase
                         ? "Expected a CASE or END SELECT statement following SELECT CASE at (1)"
                         : top.kind == StmtKind::SelectType
                               ? "Expected TYPE IS, CLASS IS or END SELECT statement following SELECT TYPE at (1)"
                               : "Expected RANK or RANK DEFAULT following SELECT RANK at (1)");
                return;
            }
            break;
    }
    TerminateDo(si);
}

void Structure::EndStmt(int si) {
    Stmt &st = s_->stmts[static_cast<size_t>(si)];
    if (stack_.empty()) return;  // a bare END is a whole (empty) main program
    Frame &top = stack_.back();
    const std::string &of = st.end_of;
    std::string want = EndWord(top.kind);
    bool unit = IsUnitKind(top.kind);
    bool match = of == want || (of.empty() && unit);
    // module procedure units may end with END PROCEDURE, END FUNCTION or END SUBROUTINE
    if (top.kind == StmtKind::ModuleProcedureUnit && (of == "function" || of == "subroutine")) match = true;
    if (top.kind == StmtKind::Program && top.implicit_program && of.empty()) match = true;
    if (!match) {
        if (top.kind == StmtKind::Do && !top.do_label.empty() && !st.label.empty() &&
            StripZeros(st.label) == top.do_label) {
            // `10 end` of a nonblock DO? not valid either
        }
        std::string expect = unit && top.implicit_program ? "END PROGRAM" : EndName(top.kind);
        if (top.kind == StmtKind::Program && top.implicit_program) expect = "END PROGRAM";
        Diag(st, st.pos, "Expecting " + expect + " statement at (1)");
        // gfortran treats an END of an enclosing unit as closing the open constructs? No:
        // it reports and carries on as if the statement were not there. If the END names
        // a unit that is open further out, close down to it to limit the cascade.
        if (IsUnitKind(StmtKind::Program) && (of == "subroutine" || of == "function" || of == "program" ||
                                              of == "module" || of.empty())) {
            for (size_t i = stack_.size(); i-- > 0;) {
                if (IsUnitKind(stack_[i].kind) &&
                    (of.empty() || of == EndWord(stack_[i].kind))) {
                    while (stack_.size() > i) Close(si);
                    break;
                }
            }
        }
        return;
    }
    // names
    if (!st.unit.name.empty()) {
        if (unit || top.kind == StmtKind::DerivedType || top.kind == StmtKind::Interface) {
            if (top.name.empty() && unit && top.kind != StmtKind::Program) {
            } else if (st.unit.name != top.name && !top.name.empty()) {
                std::string what = EndName(top.kind);
                if (unit) {
                    Diag(st, st.unit.name_pos, "Expected label '" + top.name + "' for " + what + " statement at (1)");
                } else if (top.kind == StmtKind::DerivedType) {
                    Diag(st, st.unit.name_pos, "Expected label '" + top.name + "' for END TYPE statement at (1)");
                }
            }
        } else if (top.name.empty()) {
            Diag(st, st.unit.name_pos, "Syntax error in " + EndName(top.kind) + " statement at (1)");
        } else if (st.unit.name != top.name) {
            Diag(st, st.unit.name_pos, "Expected label '" + top.name + "' for " + EndName(top.kind) + " statement at (1)");
        }
    } else if (!unit && !top.name.empty() && top.kind != StmtKind::DerivedType && top.kind != StmtKind::Interface &&
               top.kind != StmtKind::Enum) {
        Diag(st, st.pos, "Expected block name of '" + top.name + "' in " + EndName(top.kind) + " statement at (1)");
    }
    if (top.kind == StmtKind::Do && !top.do_label.empty()) {
        // a labelled END DO closes the nonblock DO(s) with that label
        Close(si);
        TerminateDo(si);
        return;
    }
    Close(si);
    TerminateDo(si);
}

}  // namespace

void ParseStatements(Source *src) {
    // Statement functions and a bare name in a file with an unresolved #include
    // (a macro the reader cannot see) are accepted.
    bool lenient = src->unresolved_include;
    for (Stmt &st : src->stmts) {
        if (st.toks.empty()) {
            if (!st.label.empty()) {
                Diag d;
                d.pos = st.label_pos;
                d.end = {st.label_pos.line, st.label_pos.col + static_cast<int>(st.label.size())};
                d.message = "Statement label without statement at (1)";
                d.code = "syntax-error";
                src->diags.push_back(d);
            }
            continue;
        }
        StmtParser p(st, src->form, lenient);
        if (!p.Run()) {
            Diag d;
            d.pos = p.pos;
            d.end = p.pos;
            d.end.col++;
            d.message = p.msg;
            d.code = "syntax-error";
            src->diags.push_back(d);
            // A failed statement still counts structurally when its kind is known
            // (gfortran opens the unit even after "Syntax error in SUBROUTINE statement"?
            // No: it drops the statement). Keep only block statements whose header
            // was recognised before the error.
            bool keep = st.kind == StmtKind::Do || st.kind == StmtKind::If || st.kind == StmtKind::SelectCase ||
                        st.kind == StmtKind::End;
            if (!keep) st.kind = StmtKind::Unknown;
            if (st.kind == StmtKind::Unknown) st.toks.clear();
        }
    }
    // Statement functions: `f(x, y) = expr` in the specification part, where f
    // is not a declared array. Resolved by the structure pass's phase.
    Structure s(src);
    s.Run();
    std::stable_sort(src->diags.begin(), src->diags.end(),
                     [](const Diag &a, const Diag &b) { return a.pos < b.pos; });
}

}  // namespace fortran
