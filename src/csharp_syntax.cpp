// mep's C# green trees: see csharp_syntax.h. A port of Roslyn's GreenNode,
// InternalSyntax SyntaxToken / SyntaxTrivia / SyntaxList / SyntaxListBuilder
// and the hand-written half of InternalSyntax.SyntaxFactory (MIT, (c) .NET
// Foundation and Contributors).
#include "csharp_syntax.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "csharp_syntax_factory.h"

namespace csharp {

// --- Kinds -------------------------------------------------------------------------

const char *KindName(SyntaxKind kind) {
    static const std::unordered_map<uint16_t, const char *> names = [] {
        std::unordered_map<uint16_t, const char *> m;
#define CS_KIND(name, value) m.emplace(static_cast<uint16_t>(value), #name);
#include "csharp_syntax_kinds.inc"
#undef CS_KIND
        return m;
    }();
    auto it = names.find(static_cast<uint16_t>(kind));
    return it == names.end() ? "?" : it->second;
}

// --- Errors --------------------------------------------------------------------------

namespace {

struct ErrorEntry {
    DiagnosticSeverity severity;
    const char *message;
};

const std::unordered_map<uint16_t, ErrorEntry> &ErrorTable() {
    static const std::unordered_map<uint16_t, ErrorEntry> table = [] {
        std::unordered_map<uint16_t, ErrorEntry> m;
        using S = DiagnosticSeverity;
        (void)sizeof(S);
#define CS_ERROR(name, number, severity, message) m.emplace(static_cast<uint16_t>(number), ErrorEntry{S::severity, message});
#include "csharp_errors.inc"
#undef CS_ERROR
        return m;
    }();
    return table;
}

}  // namespace

std::string ErrorId(ErrorCode code) {
    std::string digits = std::to_string(static_cast<int>(code));
    while (digits.size() < 4) digits.insert(digits.begin(), '0');
    return "CS" + digits;
}

const char *ErrorMessageFormat(ErrorCode code) {
    auto it = ErrorTable().find(static_cast<uint16_t>(code));
    return it == ErrorTable().end() ? "" : it->second.message;
}

DiagnosticSeverity ErrorSeverity(ErrorCode code) {
    auto it = ErrorTable().find(static_cast<uint16_t>(code));
    return it == ErrorTable().end() ? DiagnosticSeverity::Error : it->second.severity;
}

std::string FormatDiagnosticMessage(const SyntaxDiagnosticInfo &d) {
    // String.Format with the arguments: {n} substituted, {{ and }} unescaped.
    const char *fmt = ErrorMessageFormat(d.code);
    std::string out;
    for (const char *p = fmt; *p; p++) {
        if (p[0] == '{' && p[1] == '{') {
            out += '{';
            p++;
        } else if (p[0] == '}' && p[1] == '}') {
            out += '}';
            p++;
        } else if (p[0] == '{' && p[1] >= '0' && p[1] <= '9') {
            const char *q = p + 1;
            int n = 0;
            while (*q >= '0' && *q <= '9') n = n * 10 + (*q++ - '0');
            while (*q && *q != '}') q++;  // alignment / format specifiers are not used by these messages
            if (n < d.arg_count) out += ToUtf8(d.args[n]);
            p = *q ? q : q - 1;
        } else {
            out += *p;
        }
    }
    return out;
}

// --- Text ----------------------------------------------------------------------------

std::string ToUtf8(Text text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        uint32_t c = text[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (text[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;  // a lone surrogate has no UTF-8 form
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::u16string ToUtf16(std::string_view s) {
    // Decodes like .NET's UTF8Encoding with replacement: each maximal invalid subsequence becomes one U+FFFD.
    std::u16string out;
    out.reserve(s.size());
    size_t i = 0;
    const size_t n = s.size();
    auto byte = [&](size_t k) { return static_cast<unsigned char>(s[k]); };
    while (i < n) {
        unsigned char b0 = byte(i);
        if (b0 < 0x80) {
            out += static_cast<char16_t>(b0);
            i++;
            continue;
        }
        int need = 0;
        uint32_t c = 0;
        unsigned char lo = 0x80, hi = 0xBF;
        if (b0 >= 0xC2 && b0 <= 0xDF) {
            need = 1;
            c = b0 & 0x1F;
        } else if (b0 >= 0xE0 && b0 <= 0xEF) {
            need = 2;
            c = b0 & 0x0F;
            if (b0 == 0xE0) lo = 0xA0;
            if (b0 == 0xED) hi = 0x9F;
        } else if (b0 >= 0xF0 && b0 <= 0xF4) {
            need = 3;
            c = b0 & 0x07;
            if (b0 == 0xF0) lo = 0x90;
            if (b0 == 0xF4) hi = 0x8F;
        } else {
            out += u'�';
            i++;
            continue;
        }
        size_t j = i + 1;
        int got = 0;
        bool bad = false;
        while (got < need) {
            if (j >= n) {
                bad = true;
                break;
            }
            unsigned char b = byte(j);
            unsigned char l = got == 0 ? lo : 0x80, h = got == 0 ? hi : 0xBF;
            if (b < l || b > h) {
                bad = true;
                break;
            }
            c = (c << 6) | (b & 0x3F);
            j++;
            got++;
        }
        if (bad) {
            out += u'�';
            i = j;
            continue;
        }
        if (c >= 0x10000) {
            c -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (c >> 10));
            out += static_cast<char16_t>(0xDC00 + (c & 0x3FF));
        } else {
            out += static_cast<char16_t>(c);
        }
        i = j;
    }
    return out;
}

// --- Arena ---------------------------------------------------------------------------

namespace {
thread_local Arena *g_current_arena = nullptr;
constexpr size_t kBlockSize = 64 * 1024;
}  // namespace

Arena::Arena() = default;
Arena::~Arena() = default;

void *Arena::Allocate(size_t bytes, size_t align) {
    uintptr_t p = reinterpret_cast<uintptr_t>(cur_);
    size_t pad = (align - (p % align)) % align;
    if (cur_ == nullptr || pad + bytes > left_) {
        size_t size = std::max(kBlockSize, bytes + align);
        blocks_.emplace_back(new char[size]);
        cur_ = blocks_.back().get();
        left_ = size;
        p = reinterpret_cast<uintptr_t>(cur_);
        pad = (align - (p % align)) % align;
    }
    char *result = cur_ + pad;
    cur_ += pad + bytes;
    left_ -= pad + bytes;
    return result;
}

Text Arena::CopyText(Text text) {
    if (text.empty()) return Text();
    char16_t *p = static_cast<char16_t *>(Allocate(text.size() * sizeof(char16_t), alignof(char16_t)));
    std::memcpy(p, text.data(), text.size() * sizeof(char16_t));
    return Text(p, text.size());
}

std::vector<GreenNode *> *Arena::NewBuilderStorage() {
    builders_.emplace_back(new std::vector<GreenNode *>());
    return builders_.back().get();
}

Arena *CurrentArena() { return g_current_arena; }

ArenaScope::ArenaScope(Arena *arena) : saved_(g_current_arena) { g_current_arena = arena; }
ArenaScope::~ArenaScope() { g_current_arena = saved_; }

namespace {

GreenNode *NewNode() {
    void *mem = CurrentArena()->Allocate(sizeof(GreenNode), alignof(GreenNode));
    return new (mem) GreenNode();
}

GreenNode *Clone(const GreenNode *n) {
    GreenNode *c = NewNode();
    *c = *n;
    return c;
}

void Adjust(GreenNode *parent, const GreenNode *child) {
    parent->flags |= child->flags & kInheritMask;
    parent->full_width += child->full_width;
}

}  // namespace

// --- GreenNode ---------------------------------------------------------------------------

bool GreenNode::IsStructuredTrivia() const { return type == GreenType::Node && IsStructuredTriviaKind(kind); }
bool GreenNode::IsDirective() const { return type == GreenType::Node && IsDirectiveTriviaKind(kind); }

int GreenNode::GetLeadingTriviaWidth() const {
    if (type == GreenType::Token) return leading ? leading->full_width : 0;
    if (type == GreenType::Trivia) return 0;
    if (full_width == 0) return 0;
    const GreenNode *t = GetFirstTerminal();
    return t ? t->GetLeadingTriviaWidth() : 0;
}

int GreenNode::GetTrailingTriviaWidth() const {
    if (type == GreenType::Token) return trailing ? trailing->full_width : 0;
    if (type == GreenType::Trivia) return 0;
    if (full_width == 0) return 0;
    const GreenNode *t = GetLastTerminal();
    return t ? t->GetTrailingTriviaWidth() : 0;
}

GreenNode *GreenNode::GetLeadingTrivia() const { return type == GreenType::Token ? leading : nullptr; }
GreenNode *GreenNode::GetTrailingTrivia() const { return type == GreenType::Token ? trailing : nullptr; }

const GreenNode *GreenNode::GetFirstTerminal() const {
    const GreenNode *node = this;
    do {
        const GreenNode *first = nullptr;
        for (int i = 0; i < node->slot_count; i++) {
            if (node->slots[i]) {
                first = node->slots[i];
                break;
            }
        }
        node = first;
    } while (node && node->slot_count > 0);
    return node;
}

const GreenNode *GreenNode::GetLastTerminal() const {
    const GreenNode *node = this;
    do {
        const GreenNode *last = nullptr;
        for (int i = node->slot_count - 1; i >= 0; i--) {
            if (node->slots[i]) {
                last = node->slots[i];
                break;
            }
        }
        node = last;
    } while (node && node->slot_count > 0);
    return node;
}

GreenNode *GreenNode::GetFirstToken() const {
    if (type == GreenType::Token) return const_cast<GreenNode *>(this);
    return const_cast<GreenNode *>(GetFirstTerminal());
}

GreenNode *GreenNode::GetLastToken() const {
    if (type == GreenType::Token) return const_cast<GreenNode *>(this);
    return const_cast<GreenNode *>(GetLastTerminal());
}

GreenNode *GreenNode::GetLastNonmissingToken() const {
    const GreenNode *node = this;
    do {
        const GreenNode *last = nullptr;
        for (int i = node->slot_count - 1; i >= 0; i--) {
            const GreenNode *c = node->slots[i];
            if (c && !c->IsMissing()) {
                last = c;
                break;
            }
        }
        node = last;
    } while (node && node->slot_count > 0);
    return const_cast<GreenNode *>(node);
}

GreenNode *WithDiagnosticsGreen(const GreenNode *node, const SyntaxDiagnosticInfo *diags, int count) {
    GreenNode *c = Clone(node);
    SyntaxDiagnosticInfo *copy = CurrentArena()->NewArray<SyntaxDiagnosticInfo>(static_cast<size_t>(count));
    for (int i = 0; i < count; i++) copy[i] = diags[i];
    c->diags = copy;
    c->diag_count = count;
    // GreenNode's constructors set ContainsDiagnostics when given diagnostics; the flag is never cleared.
    if (count > 0) c->flags |= kContainsDiagnostics;
    return c;
}

GreenNode *WithAdditionalDiagnostics(const GreenNode *node, const SyntaxDiagnosticInfo &extra) {
    std::vector<SyntaxDiagnosticInfo> all(node->diags, node->diags + node->diag_count);
    all.push_back(extra);
    return WithDiagnosticsGreen(node, all.data(), static_cast<int>(all.size()));
}

std::vector<GreenNode *> EnumerateNodes(GreenNode *node) {
    std::vector<GreenNode *> out;
    std::vector<GreenNode *> stack{node};
    while (!stack.empty()) {
        GreenNode *n = stack.back();
        stack.pop_back();
        out.push_back(n);
        if (n->IsToken()) continue;
        // ChildNodesAndTokens: lists are flattened into their items.
        std::vector<GreenNode *> kids;
        for (int i = 0; i < n->slot_count; i++) {
            GreenNode *c = n->slots[i];
            if (!c) continue;
            if (c->IsList()) {
                for (int j = 0; j < c->slot_count; j++) {
                    if (c->slots[j]) kids.push_back(c->slots[j]);
                }
            } else {
                kids.push_back(c);
            }
        }
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back(*it);
    }
    return out;
}

// --- Lists ------------------------------------------------------------------------------------

bool SyntaxList::Any(SyntaxKind kind) const {
    for (int i = 0, n = Count(); i < n; i++) {
        if ((*this)[i]->kind == kind) return true;
    }
    return false;
}

SyntaxListBuilder SyntaxListBuilder::Create() { return SyntaxListBuilder(CurrentArena()->NewBuilderStorage()); }

SyntaxListBuilder &SyntaxListBuilder::Add(GreenNode *item) {
    if (item == nullptr) return *this;
    if (item->IsList()) {
        for (int i = 0; i < item->slot_count; i++) Add(item->slots[i]);
    } else {
        items_->push_back(item);
    }
    return *this;
}

void SyntaxListBuilder::AddRange(SyntaxList list, int offset, int length) {
    for (int i = offset; i < length; i++) Add(list[i]);
}

void SyntaxListBuilder::AddRange(const SyntaxListBuilder &other) {
    if (other.IsNull()) return;
    std::vector<GreenNode *> copy = *other.items_;
    for (GreenNode *n : copy) Add(n);
}

void SyntaxListBuilder::AddRangeSeparated(SeparatedSyntaxList nodes, int count) {
    SyntaxList list = nodes.GetWithSeparators();
    AddRange(list, Count(), std::min(count * 2, list.Count()));
}

bool SyntaxListBuilder::Any(SyntaxKind kind) const {
    for (GreenNode *n : *items_) {
        if (n->kind == kind) return true;
    }
    return false;
}

GreenNode *SyntaxListBuilder::ToListNode() const {
    switch (items_->size()) {
    case 0:
        return nullptr;
    case 1:
        return (*items_)[0];
    default:
        return SyntaxFactory::MakeList(items_->data(), static_cast<int>(items_->size()));
    }
}

SyntaxListBuilder SyntaxListPool::Allocate() {
    if (!free_.empty()) {
        std::vector<GreenNode *> *v = free_.back();
        free_.pop_back();
        v->clear();
        return SyntaxListBuilder(v);
    }
    return SyntaxListBuilder::Create();
}

void SyntaxListPool::Free(SyntaxListBuilder builder) {
    if (builder.IsNull()) return;
    builder.Clear();
    free_.push_back(builder.Storage());  // the storage belongs to the arena; keep it for reuse
}

// --- SyntaxFactory (hand-written half) -------------------------------------------------------

namespace SyntaxFactory {

GreenNode *MakeNode(SyntaxKind kind, GreenNode *const *slots, int count) {
    GreenNode *n = NewNode();
    n->kind = kind;
    n->type = GreenType::Node;
    n->slot_count = count;
    n->slots = CurrentArena()->NewArray<GreenNode *>(static_cast<size_t>(count));
    for (int i = 0; i < count; i++) {
        n->slots[i] = slots[i];
        if (slots[i]) Adjust(n, slots[i]);
    }
    if (IsStructuredTriviaKind(kind)) {
        n->flags |= kContainsStructuredTrivia;
        if (kind == SyntaxKind::SkippedTokensTrivia) n->flags |= kContainsSkippedText;
        if (IsDirectiveTriviaKind(kind)) n->flags |= kContainsDirectives;
    }
    return n;
}

GreenNode *MakeList(GreenNode *const *items, int count) {
    GreenNode *n = NewNode();
    n->kind = SyntaxKind::List;
    n->type = GreenType::List;
    n->slot_count = count;
    n->slots = CurrentArena()->NewArray<GreenNode *>(static_cast<size_t>(count));
    for (int i = 0; i < count; i++) {
        n->slots[i] = items[i];
        if (items[i]) Adjust(n, items[i]);
    }
    return n;
}

GreenNode *List(GreenNode *a, GreenNode *b) {
    GreenNode *items[] = {a, b};
    return MakeList(items, 2);
}

GreenNode *List(GreenNode *a, GreenNode *b, GreenNode *c) {
    GreenNode *items[] = {a, b, c};
    return MakeList(items, 3);
}

GreenNode *Trivia(SyntaxKind kind, Text text) {
    GreenNode *n = NewNode();
    n->kind = kind;
    n->type = GreenType::Trivia;
    n->text = text;
    n->full_width = static_cast<int>(text.size());
    if (kind == SyntaxKind::SkippedTokensTrivia) n->flags |= kContainsSkippedText;
    return n;
}

namespace {
GreenNode *MakeSpace() {
    static GreenNode space;
    space.kind = SyntaxKind::WhitespaceTrivia;
    space.type = GreenType::Trivia;
    space.text = u" ";
    space.full_width = 1;
    return &space;
}
}  // namespace

GreenNode *const Space = MakeSpace();

GreenNode *Whitespace(Text text) { return Trivia(SyntaxKind::WhitespaceTrivia, text); }
GreenNode *EndOfLine(Text text) { return Trivia(SyntaxKind::EndOfLineTrivia, text); }
GreenNode *Comment(Text text) {
    if (text.size() >= 2 && (text[0] == u'/' || text[0] == u'@') && text[1] == u'*') {
        return Trivia(SyntaxKind::MultiLineCommentTrivia, text);
    }
    return Trivia(SyntaxKind::SingleLineCommentTrivia, text);
}
GreenNode *ConflictMarker(Text text) { return Trivia(SyntaxKind::ConflictMarkerTrivia, text); }
GreenNode *DisabledText(Text text) { return Trivia(SyntaxKind::DisabledTextTrivia, text); }
GreenNode *PreprocessingMessage(Text text) { return Trivia(SyntaxKind::PreprocessingMessageTrivia, text); }
GreenNode *DocumentationCommentExteriorTrivia(Text text) {
    return Trivia(SyntaxKind::DocumentationCommentExteriorTrivia, text);
}

namespace {

constexpr SyntaxKind kFirstTokenWithWellKnownText = SyntaxKind::TildeToken;
constexpr SyntaxKind kLastTokenWithWellKnownText = SyntaxKind::EndOfFileToken;

bool HasWellKnownText(SyntaxKind kind) {
    return kind >= kFirstTokenWithWellKnownText && kind <= kLastTokenWithWellKnownText;
}

GreenNode *NewToken(SyntaxKind kind, GreenNode *leading, Text text, Text value_text, GreenNode *trailing,
                    bool missing) {
    GreenNode *t = NewNode();
    t->kind = kind;
    t->contextual_kind = kind;
    t->type = GreenType::Token;
    t->text = text;
    t->value_text = value_text;
    t->full_width = static_cast<int>(text.size());
    t->flags = kIsNotMissing;
    if (leading) {
        t->leading = leading;
        Adjust(t, leading);
    }
    if (trailing) {
        t->trailing = trailing;
        Adjust(t, trailing);
    }
    if (missing) t->flags &= static_cast<uint16_t>(~kIsNotMissing);
    return t;
}

}  // namespace

GreenNode *Token(SyntaxKind kind) { return Token(nullptr, kind, nullptr); }

GreenNode *Token(GreenNode *leading, SyntaxKind kind, GreenNode *trailing) {
    if (!HasWellKnownText(kind)) return MissingToken(leading, kind, trailing);
    Text text = SyntaxFacts::GetText(kind);
    return NewToken(kind, leading, text, text, trailing, false);
}

GreenNode *Token(GreenNode *leading, SyntaxKind kind, Text text, GreenNode *trailing) {
    return Token(leading, kind, text, text, trailing);
}

GreenNode *Token(GreenNode *leading, SyntaxKind kind, Text text, Text value_text, GreenNode *trailing) {
    Text def = SyntaxFacts::GetText(kind);
    if (HasWellKnownText(kind) && text == def && value_text == def) return Token(leading, kind, trailing);
    return TokenWithValue(kind, leading, text, value_text, nullptr, trailing);
}

GreenNode *MissingToken(SyntaxKind kind) { return MissingToken(nullptr, kind, nullptr); }

GreenNode *MissingToken(GreenNode *leading, SyntaxKind kind, GreenNode *trailing) {
    // A missing token has no text, whatever its kind (SyntaxToken.MissingTokenWithTrivia.Text is "").
    return NewToken(kind, leading, Text(), Text(), trailing, true);
}

GreenNode *Identifier(Text text) { return Identifier(SyntaxKind::IdentifierToken, nullptr, text, text, nullptr); }

GreenNode *Identifier(GreenNode *leading, Text text, GreenNode *trailing) {
    return Identifier(SyntaxKind::IdentifierToken, leading, text, text, trailing);
}

GreenNode *Identifier(SyntaxKind contextual_kind, GreenNode *leading, Text text, Text value_text, GreenNode *trailing) {
    GreenNode *t = NewToken(SyntaxKind::IdentifierToken, leading, text, value_text, trailing, false);
    t->contextual_kind = contextual_kind;
    return t;
}

GreenNode *TokenWithValue(SyntaxKind kind, GreenNode *leading, Text text, Text value_text, const LiteralValue *value,
                          GreenNode *trailing) {
    GreenNode *t = NewToken(kind, leading, text, value_text, trailing, false);
    t->value = value;
    return t;
}

GreenNode *Literal(GreenNode *leading, Text text, SyntaxKind kind, Text value, GreenNode *trailing) {
    return TokenWithValue(kind, leading, text, value, nullptr, trailing);
}

GreenNode *BadToken(GreenNode *leading, Text text, GreenNode *trailing) {
    return TokenWithValue(SyntaxKind::BadToken, leading, text, text, nullptr, trailing);
}

GreenNode *XmlTextLiteral(GreenNode *leading, Text text, Text value, GreenNode *trailing) {
    return TokenWithValue(SyntaxKind::XmlTextLiteralToken, leading, text, value, nullptr, trailing);
}

GreenNode *XmlTextNewLine(GreenNode *leading, Text text, Text value, GreenNode *trailing) {
    return TokenWithValue(SyntaxKind::XmlTextLiteralNewLineToken, leading, text, value, nullptr, trailing);
}

GreenNode *XmlTextNewLine(Text text) { return XmlTextNewLine(nullptr, text, text, nullptr); }

GreenNode *XmlEntity(GreenNode *leading, Text text, Text value, GreenNode *trailing) {
    return TokenWithValue(SyntaxKind::XmlEntityLiteralToken, leading, text, value, nullptr, trailing);
}

}  // namespace SyntaxFactory

namespace {

GreenNode *RebuildToken(const GreenNode *token, GreenNode *leading, GreenNode *trailing) {
    GreenNode *t = Clone(token);
    t->leading = leading;
    t->trailing = trailing;
    t->full_width = static_cast<int>(token->text.size());
    // Flags: the token's own (missing-ness, its diagnostics) plus what the trivia contain.
    uint16_t own = token->IsMissing() ? 0 : kIsNotMissing;
    if (token->diag_count > 0) own |= kContainsDiagnostics;
    t->flags = own;
    if (leading) Adjust(t, leading);
    if (trailing) Adjust(t, trailing);
    if (token->IsMissing()) t->flags &= static_cast<uint16_t>(~kIsNotMissing);
    return t;
}

}  // namespace

GreenNode *TokenWithLeadingTrivia(const GreenNode *token, GreenNode *trivia) {
    return RebuildToken(token, trivia, token->trailing);
}

GreenNode *TokenWithTrailingTrivia(const GreenNode *token, GreenNode *trivia) {
    return RebuildToken(token, token->leading, trivia);
}

GreenNode *WithSlot(const GreenNode *node, int index, GreenNode *child) {
    std::vector<GreenNode *> slots(node->slots, node->slots + node->slot_count);
    slots[static_cast<size_t>(index)] = child;
    GreenNode *n = node->IsList() ? SyntaxFactory::MakeList(slots.data(), node->slot_count)
                                  : SyntaxFactory::MakeNode(node->kind, slots.data(), node->slot_count);
    if (node->diag_count > 0) n = WithDiagnosticsGreen(n, node->diags, node->diag_count);
    return n;
}

}  // namespace csharp
