// jsparse public API: source decoding, Parse/ParseAuto, the acorn-shaped
// JSON writer, child iteration. The parser itself is js_parse_tokenize.cpp,
// js_parse_statement.cpp, js_parse_expression.cpp, js_parse_regexp.cpp and
// js_parse_jsx.cpp.

#include "js_parse.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "js_parse_parser.h"
#include "js_parse_ts.h"

namespace jsparse {

// ---------------------------------------------------------------- basics

const char* NodeTypeName(NodeType t) {
    switch (t) {
#define JSPARSE_NAME_ITEM(n) \
    case NodeType::n:        \
        return #n;
        JSPARSE_NODE_TYPES(JSPARSE_NAME_ITEM)
        JSPARSE_TS_NODE_TYPES(JSPARSE_NAME_ITEM)
#undef JSPARSE_NAME_ITEM
    case NodeType::None:
    case NodeType::Count_:
        break;
    }
    return "None";
}

std::string Error::Full() const {
    return message + " (" + std::to_string(loc.line) + ":" + std::to_string(loc.column) + ")";
}

ParseResult::ParseResult() = default;
ParseResult::~ParseResult() = default;
ParseResult::ParseResult(ParseResult&&) noexcept = default;
ParseResult& ParseResult::operator=(ParseResult&&) noexcept = default;

uint32_t ParseResult::ByteOffset(uint32_t u16) const {
    if (u16_to_byte.empty()) return u16;
    if (u16 >= u16_to_byte.size()) return u16_to_byte.back();
    return u16_to_byte[u16];
}

uint32_t ParseResult::U16Offset(uint32_t byte) const {
    if (u16_to_byte.empty()) return byte;
    auto it = std::upper_bound(u16_to_byte.begin(), u16_to_byte.end(), byte);
    if (it == u16_to_byte.begin()) return 0;
    return static_cast<uint32_t>(it - u16_to_byte.begin() - 1);
}

void CollectChildren(const Node* n, std::vector<Node*>& out) {
    out.clear();
    if (!n) return;
    for (Node* k : n->kid)
        if (k) out.push_back(k);
    for (const NodeList& l : n->list)
        for (Node* k : l)
            if (k) out.push_back(k);
    for (Node* k : n->ts)
        if (k) out.push_back(k);
    for (const NodeList& l : n->tslist)
        for (Node* k : l)
            if (k) out.push_back(k);
    std::stable_sort(out.begin(), out.end(), [](const Node* a, const Node* b) { return a->ustart < b->ustart; });
}

// ---------------------------------------------------------------- decoding

namespace {

// WHATWG UTF-8 decode (maximal subpart -> one U+FFFD), as node's
// readFileSync(f, 'utf8') does. Fills u16 and the offset map.
void DecodeUtf8(std::string_view s, std::u16string& u16, std::vector<uint32_t>& map) {
    u16.clear();
    map.clear();
    u16.reserve(s.size());
    map.reserve(s.size() + 1);
    size_t i = 0;
    const size_t n = s.size();
    auto emit = [&](uint32_t cp, size_t at) {
        uint32_t off = static_cast<uint32_t>(at);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            u16 += static_cast<char16_t>(0xD800 + (cp >> 10));
            u16 += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
            map.push_back(off);
            map.push_back(off);
        } else {
            u16 += static_cast<char16_t>(cp);
            map.push_back(off);
        }
    };
    while (i < n) {
        size_t at = i;
        uint8_t b = static_cast<uint8_t>(s[i]);
        if (b < 0x80) {
            emit(b, at);
            ++i;
            continue;
        }
        int needed;
        uint32_t cp;
        uint8_t lower = 0x80, upper = 0xBF;
        if (b >= 0xC2 && b <= 0xDF) {
            needed = 1;
            cp = b & 0x1Fu;
        } else if (b >= 0xE0 && b <= 0xEF) {
            if (b == 0xE0) lower = 0xA0;
            if (b == 0xED) upper = 0x9F;
            needed = 2;
            cp = b & 0xFu;
        } else if (b >= 0xF0 && b <= 0xF4) {
            if (b == 0xF0) lower = 0x90;
            if (b == 0xF4) upper = 0x8F;
            needed = 3;
            cp = b & 0x7u;
        } else {
            emit(0xFFFD, at);
            ++i;
            continue;
        }
        ++i;
        bool bad = false;
        for (int k = 0; k < needed; ++k) {
            if (i >= n) {
                bad = true;
                break;
            }
            uint8_t c = static_cast<uint8_t>(s[i]);
            if (c < lower || c > upper) {
                bad = true;
                break;
            }
            lower = 0x80;
            upper = 0xBF;
            cp = (cp << 6) | (c & 0x3Fu);
            ++i;
        }
        emit(bad ? 0xFFFD : cp, at);
    }
    map.push_back(static_cast<uint32_t>(n));
}

}  // namespace

// ---------------------------------------------------------------- parsing

namespace {

// isFileProbablyExternalModule: an import or export at the top level.
bool IsTsModule(const Node* program) {
    for (const Node* s : program->body_list()) {
        switch (s->type) {
        case NodeType::ImportDeclaration:
        case NodeType::ExportNamedDeclaration:
        case NodeType::ExportDefaultDeclaration:
        case NodeType::ExportAllDeclaration:
        case NodeType::TSExportAssignment:
            return true;
        case NodeType::TSImportEqualsDeclaration:
            if (s->moduleReference() && s->moduleReference()->type == NodeType::TSExternalModuleReference) return true;
            break;
        default:
            break;
        }
    }
    return false;
}

}  // namespace

ParseResult Parse(std::string_view source, const Options& options) {
    ParseResult r;
    r.arena = std::make_unique<Arena>();
    r.source_type = options.source_type;
    r.jsx = options.jsx;
    r.typescript = options.typescript;
    std::u16string u16;
    DecodeUtf8(source, u16, r.u16_to_byte);
    std::unique_ptr<Parser> p;
    if (options.typescript) {
        auto ts = std::make_unique<TsParser>(options, u16, *r.arena, r);
        ts->topLevelAwait = options.mts;
        r.program = ts->parseRecording();
        if (!options.mts && ts->sawTopLevelAwait && r.program && IsTsModule(r.program)) {
            // tsc's reparseTopLevelAwait: in a module, top-level `await` is
            // an await expression
            ParseResult again;
            again.arena = std::make_unique<Arena>();
            again.source_type = r.source_type;
            again.jsx = r.jsx;
            again.typescript = true;
            again.u16_to_byte = std::move(r.u16_to_byte);
            ts.reset();
            r = std::move(again);
            ts = std::make_unique<TsParser>(options, u16, *r.arena, r);
            ts->topLevelAwait = true;
            r.program = ts->parseRecording();
        }
        if (r.program && !r.has_error) ts->tsCheckPragmas();
        p = std::move(ts);
    } else if (options.jsx) {
        p = std::make_unique<JsxParser>(options, u16, *r.arena, r);
    } else {
        p = std::make_unique<Parser>(options, u16, *r.arena, r);
    }
    if (!options.typescript) r.program = p->parseRecording();
    if (!r.program && options.recover) {
        // An error that escaped every recovery point: an empty Program.
        Node* prog = r.arena->NewNode();
        prog->type = NodeType::Program;
        prog->kind = options.source_type == SourceType::Module ? "module" : "script";
        prog->ustart = 0;
        prog->uend = static_cast<uint32_t>(u16.size());
        prog->start = 0;
        prog->end = static_cast<uint32_t>(source.size());
        r.program = prog;
    }
    return r;
}

ParseResult ParseAuto(std::string_view source, Options options) {
    options.source_type = SourceType::Module;
    ParseResult m = Parse(source, options);
    if (!m.has_error) return m;
    options.source_type = SourceType::Script;
    ParseResult s = Parse(source, options);
    if (!s.has_error) return s;
    return m;
}

// ---------------------------------------------------------------- JSON

std::string JsNumberToString(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
    if (v == 0) return "0";
    std::string sign;
    if (v < 0) {
        sign = "-";
        v = -v;
    }
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);
    std::string sci(buf, res.ptr);
    size_t e = sci.find('e');
    std::string mant = sci.substr(0, e);
    int exp = std::atoi(sci.c_str() + e + 1);
    std::string digits;
    for (char c : mant)
        if (c != '.') digits += c;
    int k = static_cast<int>(digits.size());
    int n = exp + 1;
    std::string o;
    if (k <= n && n <= 21) {
        o = digits + std::string(static_cast<size_t>(n - k), '0');
    } else if (0 < n && n <= 21) {
        o = digits.substr(0, static_cast<size_t>(n)) + "." + digits.substr(static_cast<size_t>(n));
    } else if (-6 < n && n <= 0) {
        o = "0." + std::string(static_cast<size_t>(-n), '0') + digits;
    } else {
        o = digits.substr(0, 1);
        if (k > 1) o += "." + digits.substr(1);
        o += "e";
        o += n - 1 >= 0 ? "+" : "-";
        o += std::to_string(std::abs(n - 1));
    }
    return sign + o;
}

namespace {

class JsonWriter {
public:
    std::string out;

    void Str(std::string_view s) {
        out += '"';
        size_t i = 0;
        while (i < s.size()) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            // a lone surrogate in WTF-8: ED A0..BF xx
            if (c == 0xED && i + 2 < s.size() && (static_cast<unsigned char>(s[i + 1]) & 0xE0) == 0xA0) {
                unsigned cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                              (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", cp);
                out += b;
                i += 3;
                continue;
            }
            switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else {
                    out += static_cast<char>(c);
                }
            }
            ++i;
        }
        out += '"';
    }
    void Key(const char* k) {
        Sep();
        out += '"';
        out += k;
        out += "\":";
        pendingValue_ = true;  // the value follows without a comma
    }
    void Sep() {
        if (pendingValue_) {
            pendingValue_ = false;
            return;
        }
        if (!first_) out += ',';
        first_ = false;
    }
    void Open(char c) {
        Sep();
        out += c;
        first_ = true;
    }
    void Close(char c) {
        out += c;
        first_ = false;
    }
    void Null() {
        Sep();
        out += "null";
    }
    void Bool(bool b) {
        Sep();
        out += b ? "true" : "false";
    }
    void Num(double d) {
        Sep();
        if (std::isfinite(d))
            out += JsNumberToString(d);
        else
            out += "null";
    }
    void UInt(uint32_t v) {
        Sep();
        out += std::to_string(v);
    }
    void String(std::string_view s) {
        Sep();
        Str(s);
    }

private:
    bool first_ = true;
    bool pendingValue_ = false;
};

void WritePos(JsonWriter& w, const Pos& p) {
    w.Open('{');
    w.Key("line");
    w.UInt(p.line);
    w.Key("column");
    w.UInt(p.column);
    w.Close('}');
}

void WriteLoc(JsonWriter& w, const Pos& s, const Pos& e) {
    w.Key("loc");
    w.Open('{');
    w.Key("start");
    WritePos(w, s);
    w.Key("end");
    WritePos(w, e);
    w.Close('}');
}

void WriteNode(JsonWriter& w, const Node* n);

void WriteKid(JsonWriter& w, const char* key, const Node* n) {
    w.Key(key);
    WriteNode(w, n);
}

void WriteList(JsonWriter& w, const char* key, const NodeList& l) {
    w.Key(key);
    w.Open('[');
    for (Node* k : l) WriteNode(w, k);
    w.Close(']');
}

// The fields of a TypeScript node (typescript-estree's names).
void WriteTsFields(JsonWriter& w, const Node* n) {
    using T = NodeType;
    auto flag = [&](const char* k, bool on) {
        w.Key(k);
        w.Bool(on);
    };
    auto str = [&](const char* k, std::string_view s) {
        w.Key(k);
        w.String(s);
    };
    switch (n->type) {
    case T::Decorator:
    case T::JSXSpreadChild:
    case T::TSNonNullExpression:
    case T::TSExternalModuleReference:
    case T::TSExportAssignment:
        WriteKid(w, "expression", n->expression());
        break;
    case T::AccessorProperty:
    case T::TSAbstractPropertyDefinition:
    case T::TSAbstractAccessorProperty:
    case T::TSAbstractMethodDefinition:
        flag("static", n->Has(kFlagStatic));
        flag("computed", n->Has(kFlagComputed));
        WriteKid(w, "key", n->key());
        if (n->type == T::TSAbstractMethodDefinition) str("kind", n->kind);
        WriteKid(w, "value", n->value());
        break;
    case T::TSTypeParameterDeclaration:
    case T::TSTypeParameterInstantiation:
        WriteList(w, "params", n->params());
        break;
    case T::TSTypeParameter:
        WriteKid(w, "name", n->name());
        WriteKid(w, "constraint", n->constraint());
        WriteKid(w, "default", n->defaultType());
        break;
    case T::TSTypeReference:
        WriteKid(w, "typeName", n->typeName());
        break;
    case T::TSQualifiedName:
        WriteKid(w, "left", n->left());
        WriteKid(w, "right", n->right());
        break;
    case T::TSArrayType:
        WriteKid(w, "elementType", n->elementType());
        break;
    case T::TSTupleType:
        WriteList(w, "elementTypes", n->elementTypes());
        break;
    case T::TSNamedTupleMember:
        WriteKid(w, "label", n->label());
        WriteKid(w, "elementType", n->elementType());
        break;
    case T::TSUnionType:
    case T::TSIntersectionType:
        WriteList(w, "types", n->types());
        break;
    case T::TSJSDocNullableType:
    case T::TSJSDocNonNullableType:
        flag("postfix", n->HasTs(kTsPostfix));
        break;
    case T::TSJSDocFunctionType:
    case T::TSFunctionType:
    case T::TSConstructorType:
    case T::TSCallSignatureDeclaration:
    case T::TSConstructSignatureDeclaration:
    case T::TSEmptyBodyFunctionExpression:
    case T::TSDeclareFunction:
        if (n->type == T::TSDeclareFunction || n->type == T::TSEmptyBodyFunctionExpression) {
            WriteKid(w, "id", n->id());
            flag("generator", n->Has(kFlagGenerator));
            flag("async", n->Has(kFlagAsync));
            WriteKid(w, "body", n->body());
        }
        WriteList(w, "params", n->params());
        break;
    case T::TSTypeLiteral:
        WriteList(w, "members", n->members());
        break;
    case T::TSPropertySignature:
    case T::TSMethodSignature:
        flag("computed", n->Has(kFlagComputed));
        WriteKid(w, "key", n->key());
        if (n->type == T::TSMethodSignature) {
            str("kind", n->kind);
            WriteList(w, "params", n->params());
        }
        break;
    case T::TSIndexSignature:
        WriteList(w, "parameters", n->parameters());
        break;
    case T::TSTypeOperator:
        str("operator", n->op);
        break;
    case T::TSIndexedAccessType:
        WriteKid(w, "objectType", n->objectType());
        WriteKid(w, "indexType", n->indexType());
        break;
    case T::TSConditionalType:
        WriteKid(w, "checkType", n->checkType());
        WriteKid(w, "extendsType", n->extendsType());
        WriteKid(w, "trueType", n->trueType());
        WriteKid(w, "falseType", n->falseType());
        break;
    case T::TSInferType:
        WriteKid(w, "typeParameter", n->typeParameter());
        break;
    case T::TSMappedType:
        WriteKid(w, "key", n->key());
        WriteKid(w, "constraint", n->constraint());
        WriteKid(w, "nameType", n->nameType());
        break;
    case T::TSLiteralType:
        WriteKid(w, "literal", n->literal());
        break;
    case T::TSTemplateLiteralType:
        WriteList(w, "quasis", n->quasis());
        WriteList(w, "types", n->template_types());
        break;
    case T::TSTypeQuery:
        WriteKid(w, "exprName", n->exprName());
        break;
    case T::TSImportType:
        WriteKid(w, "source", n->source());
        WriteKid(w, "qualifier", n->qualifier());
        WriteKid(w, "options", n->options());
        break;
    case T::TSTypePredicate:
        WriteKid(w, "parameterName", n->parameterName());
        break;
    case T::TSInterfaceDeclaration:
        WriteKid(w, "id", n->id());
        WriteList(w, "extends", n->extends_list());
        WriteKid(w, "body", n->body());
        break;
    case T::TSInterfaceBody:
    case T::TSModuleBlock:
        WriteList(w, "body", n->body_list());
        break;
    case T::TSInterfaceHeritage:
    case T::TSClassImplements:
    case T::TSInstantiationExpression:
    case T::TSAsExpression:
    case T::TSSatisfiesExpression:
    case T::TSTypeAssertion:
        WriteKid(w, "expression", n->expression());
        break;
    case T::TSTypeAliasDeclaration:
        WriteKid(w, "id", n->id());
        break;
    case T::TSEnumDeclaration:
    case T::TSModuleDeclaration:
        WriteKid(w, "id", n->id());
        WriteKid(w, "body", n->body());
        if (n->type == T::TSModuleDeclaration) str("kind", n->kind);
        break;
    case T::TSEnumBody:
        WriteList(w, "members", n->members());
        break;
    case T::TSEnumMember:
        WriteKid(w, "id", n->id());
        flag("computed", n->Has(kFlagComputed));
        WriteKid(w, "initializer", n->initializer());
        break;
    case T::TSParameterProperty:
        WriteKid(w, "parameter", n->parameter());
        break;
    case T::TSImportEqualsDeclaration:
        WriteKid(w, "id", n->id());
        WriteKid(w, "moduleReference", n->moduleReference());
        break;
    case T::TSNamespaceExportDeclaration:
        WriteKid(w, "id", n->id());
        break;
    default:
        break;
    }
}

// The TypeScript fields any node may carry; written only when present, so
// a JavaScript tree's JSON is acorn's.
void WriteTsExtras(JsonWriter& w, const Node* n) {
    static const char* const kSlots[] = {"typeAnnotation", "typeParameters", "returnType", "typeArguments"};
    for (int i = 0; i < 4; ++i)
        if (n->ts[i]) WriteKid(w, kSlots[i], n->ts[i]);
    if (n->tslist[0].size()) WriteList(w, "decorators", n->tslist[0]);
    if (n->tslist[1].size()) WriteList(w, "implements", n->tslist[1]);
    if (!n->accessibility.empty()) {
        w.Key("accessibility");
        w.String(n->accessibility);
    }
    static const struct {
        uint32_t bit;
        const char* key;
    } kFlags[] = {{kTsOptional, "optional"}, {kTsDefinite, "definite"}, {kTsDeclare, "declare"},
                  {kTsAbstract, "abstract"}, {kTsReadonly, "readonly"}, {kTsOverride, "override"},
                  {kTsConst, "const"},       {kTsIn, "in"},             {kTsOut, "out"},
                  {kTsAsserts, "asserts"},   {kTsTypeOnly, "typeOnly"}, {kTsGlobal, "global"},
                  {kTsStatic, "static"}};
    for (const auto& f : kFlags) {
        if (n->HasTs(f.bit)) {
            w.Key(f.key);
            w.Bool(true);
        }
    }
}

void WriteNode(JsonWriter& w, const Node* n) {
    if (!n) {
        w.Null();
        return;
    }
    w.Open('{');
    w.Key("type");
    w.String(NodeTypeName(n->type));
    w.Key("start");
    w.UInt(n->ustart);
    w.Key("end");
    w.UInt(n->uend);
    WriteLoc(w, n->loc_start, n->loc_end);
    using T = NodeType;
    auto flag = [&](const char* k, uint32_t f) {
        w.Key(k);
        w.Bool(n->Has(f));
    };
    auto str = [&](const char* k, std::string_view s) {
        w.Key(k);
        w.String(s);
    };
    auto function = [&]() {
        WriteKid(w, "id", n->id());
        flag("expression", kFlagExpression);
        flag("generator", kFlagGenerator);
        flag("async", kFlagAsync);
        WriteList(w, "params", n->params());
        WriteKid(w, "body", n->body());
    };
    switch (n->type) {
    case T::Program:
        WriteList(w, "body", n->body_list());
        str("sourceType", n->kind);
        break;
    case T::Identifier:
    case T::PrivateIdentifier:
    case T::JSXIdentifier:
        str("name", n->str);
        break;
    case T::Literal:
        w.Key("value");
        switch (n->lit) {
        case LitKind::Null:
        case LitKind::RegExp:
        case LitKind::BigInt:
        case LitKind::None:
            w.Null();
            break;
        case LitKind::Bool:
            w.Bool(n->Has(kFlagBoolValue));
            break;
        case LitKind::Number:
            w.Num(n->num);
            break;
        case LitKind::String:
            w.String(n->str2);
            break;
        }
        str("raw", n->str);
        if (n->lit == LitKind::RegExp) {
            w.Key("regex");
            w.Open('{');
            str("pattern", n->str2);
            str("flags", n->str3);
            w.Close('}');
        } else if (n->lit == LitKind::BigInt) {
            str("bigint", n->str2);
        }
        break;
    case T::JSXText:
        str("value", n->str2);
        str("raw", n->str);
        if (n->lit == LitKind::BigInt) str("bigint", n->str2);
        break;
    case T::ExpressionStatement:
        WriteKid(w, "expression", n->expression());
        if (n->Has(kFlagHasDirective)) str("directive", n->str);
        break;
    case T::BlockStatement:
    case T::StaticBlock:
    case T::ClassBody:
        WriteList(w, "body", n->body_list());
        break;
    case T::EmptyStatement:
    case T::DebuggerStatement:
    case T::ThisExpression:
    case T::Super:
    case T::JSXEmptyExpression:
    case T::JSXClosingFragment:
        break;
    case T::WithStatement:
        WriteKid(w, "object", n->object());
        WriteKid(w, "body", n->body());
        break;
    case T::ReturnStatement:
    case T::ThrowStatement:
    case T::SpreadElement:
    case T::RestElement:
    case T::AwaitExpression:
    case T::JSXSpreadAttribute:
        WriteKid(w, "argument", n->argument());
        break;
    case T::LabeledStatement:
        WriteKid(w, "body", n->body());
        WriteKid(w, "label", n->label());
        break;
    case T::BreakStatement:
    case T::ContinueStatement:
        WriteKid(w, "label", n->label());
        break;
    case T::IfStatement:
    case T::ConditionalExpression:
        WriteKid(w, "test", n->test());
        WriteKid(w, "consequent", n->consequent());
        WriteKid(w, "alternate", n->alternate());
        break;
    case T::SwitchStatement:
        WriteKid(w, "discriminant", n->discriminant());
        WriteList(w, "cases", n->cases());
        break;
    case T::SwitchCase:
        WriteKid(w, "test", n->test());
        WriteList(w, "consequent", n->consequent_list());
        break;
    case T::TryStatement:
        WriteKid(w, "block", n->block());
        WriteKid(w, "handler", n->handler());
        WriteKid(w, "finalizer", n->finalizer());
        break;
    case T::CatchClause:
        WriteKid(w, "param", n->param());
        WriteKid(w, "body", n->body());
        break;
    case T::WhileStatement:
    case T::DoWhileStatement:
        WriteKid(w, "test", n->test());
        WriteKid(w, "body", n->body());
        break;
    case T::ForStatement:
        WriteKid(w, "init", n->init());
        WriteKid(w, "test", n->test());
        WriteKid(w, "update", n->update());
        WriteKid(w, "body", n->body());
        break;
    case T::ForInStatement:
        WriteKid(w, "left", n->left());
        WriteKid(w, "right", n->right());
        WriteKid(w, "body", n->body());
        break;
    case T::ForOfStatement:
        flag("await", kFlagAwait);
        WriteKid(w, "left", n->left());
        WriteKid(w, "right", n->right());
        WriteKid(w, "body", n->body());
        break;
    case T::FunctionDeclaration:
    case T::FunctionExpression:
    case T::ArrowFunctionExpression:
        function();
        break;
    case T::VariableDeclaration:
        WriteList(w, "declarations", n->declarations());
        str("kind", n->kind);
        break;
    case T::VariableDeclarator:
        WriteKid(w, "id", n->id());
        WriteKid(w, "init", n->init());
        break;
    case T::ArrayExpression:
    case T::ArrayPattern:
        WriteList(w, "elements", n->elements());
        break;
    case T::ObjectExpression:
    case T::ObjectPattern:
        WriteList(w, "properties", n->properties());
        break;
    case T::Property:
        flag("method", kFlagMethod);
        flag("shorthand", kFlagShorthand);
        flag("computed", kFlagComputed);
        WriteKid(w, "key", n->key());
        str("kind", n->kind);
        WriteKid(w, "value", n->value());
        break;
    case T::UnaryExpression:
    case T::UpdateExpression:
        str("operator", n->op);
        flag("prefix", kFlagPrefix);
        WriteKid(w, "argument", n->argument());
        break;
    case T::BinaryExpression:
    case T::LogicalExpression:
    case T::AssignmentExpression:
        WriteKid(w, "left", n->left());
        str("operator", n->op);
        WriteKid(w, "right", n->right());
        break;
    case T::AssignmentPattern:
        WriteKid(w, "left", n->left());
        WriteKid(w, "right", n->right());
        break;
    case T::CallExpression:
        WriteKid(w, "callee", n->callee());
        WriteList(w, "arguments", n->arguments());
        flag("optional", kFlagOptional);
        break;
    case T::NewExpression:
        WriteKid(w, "callee", n->callee());
        WriteList(w, "arguments", n->arguments());
        break;
    case T::SequenceExpression:
        WriteList(w, "expressions", n->expressions());
        break;
    case T::YieldExpression:
        flag("delegate", kFlagDelegate);
        WriteKid(w, "argument", n->argument());
        break;
    case T::TemplateLiteral:
        WriteList(w, "expressions", n->expressions());
        WriteList(w, "quasis", n->quasis());
        break;
    case T::TaggedTemplateExpression:
        WriteKid(w, "tag", n->tag());
        WriteKid(w, "quasi", n->quasi());
        break;
    case T::TemplateElement:
        w.Key("value");
        w.Open('{');
        str("raw", n->str);
        w.Key("cooked");
        if (n->Has(kFlagHasCooked))
            w.String(n->str2);
        else
            w.Null();
        w.Close('}');
        flag("tail", kFlagTail);
        break;
    case T::ClassDeclaration:
    case T::ClassExpression:
        WriteKid(w, "id", n->id());
        WriteKid(w, "superClass", n->superClass());
        WriteKid(w, "body", n->body());
        break;
    case T::MethodDefinition:
    case T::PropertyDefinition:
        flag("static", kFlagStatic);
        flag("computed", kFlagComputed);
        WriteKid(w, "key", n->key());
        if (n->type == T::MethodDefinition) str("kind", n->kind);
        WriteKid(w, "value", n->value());
        break;
    case T::MetaProperty:
        WriteKid(w, "meta", n->meta());
        WriteKid(w, "property", n->property());
        break;
    case T::ImportExpression:
        WriteKid(w, "source", n->source());
        if (n->Has(kFlagHasOptions)) WriteKid(w, "options", n->options());
        break;
    case T::ChainExpression:
    case T::ParenthesizedExpression:
    case T::JSXExpressionContainer:
        WriteKid(w, "expression", n->expression());
        break;
    case T::MemberExpression:
        WriteKid(w, "object", n->object());
        WriteKid(w, "property", n->property());
        flag("computed", kFlagComputed);
        flag("optional", kFlagOptional);
        break;
    case T::ImportDeclaration:
        WriteList(w, "specifiers", n->specifiers());
        WriteKid(w, "source", n->source());
        WriteList(w, "attributes", n->attributes());
        break;
    case T::ImportSpecifier:
        WriteKid(w, "imported", n->imported());
        WriteKid(w, "local", n->local());
        break;
    case T::ImportDefaultSpecifier:
    case T::ImportNamespaceSpecifier:
        WriteKid(w, "local", n->local());
        break;
    case T::ImportAttribute:
        WriteKid(w, "key", n->key());
        WriteKid(w, "value", n->value());
        break;
    case T::ExportNamedDeclaration:
        WriteKid(w, "declaration", n->declaration());
        WriteList(w, "specifiers", n->specifiers());
        WriteKid(w, "source", n->source());
        WriteList(w, "attributes", n->attributes());
        break;
    case T::ExportDefaultDeclaration:
        WriteKid(w, "declaration", n->declaration());
        break;
    case T::ExportAllDeclaration:
        WriteKid(w, "exported", n->exported());
        WriteKid(w, "source", n->source());
        WriteList(w, "attributes", n->attributes());
        break;
    case T::ExportSpecifier:
        WriteKid(w, "local", n->local());
        WriteKid(w, "exported", n->exported());
        break;
    case T::JSXElement:
        WriteKid(w, "openingElement", n->opening());
        WriteKid(w, "closingElement", n->closing());
        WriteList(w, "children", n->children());
        break;
    case T::JSXFragment:
        WriteKid(w, "openingFragment", n->opening());
        WriteKid(w, "closingFragment", n->closing());
        WriteList(w, "children", n->children());
        break;
    case T::JSXOpeningElement:
        WriteList(w, "attributes", n->attributes());
        WriteKid(w, "name", n->name());
        flag("selfClosing", kFlagSelfClosing);
        break;
    case T::JSXOpeningFragment:
        WriteList(w, "attributes", n->attributes());
        flag("selfClosing", kFlagSelfClosing);
        break;
    case T::JSXClosingElement:
        WriteKid(w, "name", n->name());
        break;
    case T::JSXAttribute:
        WriteKid(w, "name", n->name());
        WriteKid(w, "value", n->value());
        break;
    case T::JSXNamespacedName:
        WriteKid(w, "namespace", n->nspace());
        WriteKid(w, "name", n->name());
        break;
    case T::JSXMemberExpression:
        WriteKid(w, "object", n->object());
        WriteKid(w, "property", n->property());
        break;
#define JSPARSE_TS_CASE(x) case T::x:
        JSPARSE_TS_NODE_TYPES(JSPARSE_TS_CASE)
#undef JSPARSE_TS_CASE
        WriteTsFields(w, n);
        break;
    case T::None:
    case T::Count_:
        break;
    }
    WriteTsExtras(w, n);
    w.Close('}');
}

}  // namespace

std::string ToJson(const ParseResult& r, bool with_comments) {
    JsonWriter w;
    if (r.has_error && !r.errors.empty()) {
        const Error& e = r.errors[0];
        w.Open('{');
        w.Key("error");
        w.String(e.Full());
        w.Key("pos");
        w.UInt(e.upos);
        w.Key("line");
        w.UInt(e.loc.line);
        w.Key("column");
        w.UInt(e.loc.column);
        w.Close('}');
        return w.out;
    }
    if (!with_comments) {
        WriteNode(w, r.program);
        return w.out;
    }
    w.Open('{');
    w.Key("ast");
    WriteNode(w, r.program);
    w.Key("comments");
    w.Open('[');
    for (const Comment& c : r.comments) {
        w.Open('{');
        w.Key("type");
        w.String(c.block ? "Block" : "Line");
        w.Key("value");
        w.String(c.value);
        w.Key("start");
        w.UInt(c.ustart);
        w.Key("end");
        w.UInt(c.uend);
        WriteLoc(w, c.loc_start, c.loc_end);
        w.Close('}');
    }
    w.Close(']');
    w.Close('}');
    return w.out;
}

}  // namespace jsparse
