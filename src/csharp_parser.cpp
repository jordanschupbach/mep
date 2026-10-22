// SyntaxTree: parsing (csharp_parser.h) over mep's port of Roslyn's parser,
// the red layer, diagnostics and the tree dump.
#include "csharp_parser.h"

// The parse runs on a thread with a deep stack (pthreads), except where mep_core
// has no pthreads (the same guard as job.h): there it runs on the caller's stack.
#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)
#include <pthread.h>
#define MEP_CSHARP_PARSE_THREAD 1
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "csharp_parser_internal.h"

namespace csharp {

// --- Parsing ---------------------------------------------------------------------------------

namespace {

struct ParseJob {
    Text text;
    GreenNode *root = nullptr;
};

void *RunParse(void *arg) {
    ParseJob *job = static_cast<ParseJob *>(arg);
    Lexer lexer(job->text);
    LanguageParser parser(&lexer);
    job->root = parser.ParseCompilationUnit();
    return nullptr;
}

/** @brief Runs the parse on a thread with a deep stack (the parser recurses as deeply as the code nests). */
GreenNode *ParseOnBigStack(Text text) {
    ParseJob job{text, nullptr};
#ifndef MEP_CSHARP_PARSE_THREAD
    RunParse(&job);
    return job.root;
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, static_cast<size_t>(512) * 1024 * 1024);
    pthread_t thread;
    Arena *arena = CurrentArena();
    struct Wrapper {
        ParseJob *job;
        Arena *arena;
    } w{&job, arena};
    auto entry = [](void *p) -> void * {
        Wrapper *wr = static_cast<Wrapper *>(p);
        ArenaScope scope(wr->arena);
        return RunParse(wr->job);
    };
    if (pthread_create(&thread, &attr, entry, &w) != 0) {
        pthread_attr_destroy(&attr);
        RunParse(&job);  // no thread: parse here
        return job.root;
    }
    pthread_join(thread, nullptr);
    pthread_attr_destroy(&attr);
    return job.root;
#endif
}

}  // namespace

std::unique_ptr<SyntaxTree> SyntaxTree::Parse(std::string_view utf8) {
    if (utf8.size() >= 3 && static_cast<unsigned char>(utf8[0]) == 0xEF &&
        static_cast<unsigned char>(utf8[1]) == 0xBB && static_cast<unsigned char>(utf8[2]) == 0xBF) {
        utf8.remove_prefix(3);
    }
    return ParseUtf16(ToUtf16(utf8));
}

std::unique_ptr<SyntaxTree> SyntaxTree::ParseUtf16(std::u16string text) {
    std::unique_ptr<SyntaxTree> tree(new SyntaxTree());
    tree->text_ = std::move(text);
    tree->arena_ = std::make_unique<Arena>();
    {
        ArenaScope scope(tree->arena_.get());
        tree->green_root_ = ParseOnBigStack(Text(tree->text_));
    }
    tree->Build();
    return tree;
}

SyntaxTree::~SyntaxTree() = default;

// --- Red layer ---------------------------------------------------------------------------------

TextSpan SyntaxNode::Span() const {
    int lead = green->GetLeadingTriviaWidth();
    int trail = green->GetTrailingTriviaWidth();
    return {position + lead, position + green->full_width - trail};
}

const SyntaxNode *SyntaxNode::Field(csharp::Field f) const {
    std::vector<const SyntaxNode *> items = FieldItems(f);
    return items.empty() ? nullptr : items.front();
}

std::vector<const SyntaxNode *> SyntaxNode::FieldItems(csharp::Field f) const {
    std::vector<const SyntaxNode *> out;
    int slot = FieldSlot(green->kind, f);
    if (slot < 0) return out;
    // Children are the slots in order with lists flattened: find the range belonging to `slot`.
    size_t ci = 0;
    for (int i = 0; i < green->slot_count; i++) {
        const GreenNode *s = green->slots[i];
        if (!s) continue;
        size_t n = s->IsList() ? static_cast<size_t>(s->slot_count) : 1;
        if (i == slot) {
            for (size_t k = 0; k < n && ci + k < children.size(); k++) out.push_back(children[ci + k]);
            return out;
        }
        ci += n;
    }
    return out;
}

const SyntaxNode *SyntaxNode::FirstToken() const {
    const SyntaxNode *n = this;
    while (!n->IsToken()) {
        if (n->children.empty()) return nullptr;
        n = n->children.front();
    }
    return n;
}

const SyntaxNode *SyntaxNode::LastToken() const {
    const SyntaxNode *n = this;
    while (!n->IsToken()) {
        if (n->children.empty()) return nullptr;
        n = n->children.back();
    }
    return n;
}

namespace {

const SyntaxNode *StepToken(const SyntaxNode *tok, int dir) {
    const SyntaxNode *n = tok;
    while (n) {
        const SyntaxNode *p = n->parent;
        if (!p) return nullptr;
        int i = n->index + dir;
        while (i >= 0 && i < static_cast<int>(p->children.size())) {
            const SyntaxNode *c = p->children[static_cast<size_t>(i)];
            const SyntaxNode *t = dir > 0 ? c->FirstToken() : c->LastToken();
            // Descend, skipping missing tokens.
            if (t && !t->IsMissing()) return t;
            if (t) {
                const SyntaxNode *u = StepToken(t, dir);
                return u;
            }
            i += dir;
        }
        n = p;
    }
    return nullptr;
}

}  // namespace

const SyntaxNode *SyntaxNode::PreviousToken() const { return StepToken(this, -1); }
const SyntaxNode *SyntaxNode::NextToken() const { return StepToken(this, +1); }

const SyntaxNode *SyntaxTree::BuildNode(const GreenNode *g, const SyntaxNode *parent, const SyntaxTrivia *ptrivia,
                                        int pos, int index) {
    nodes_.push_back(std::make_unique<SyntaxNode>());
    SyntaxNode *n = nodes_.back().get();
    n->green = g;
    n->parent = parent;
    n->parent_trivia = ptrivia;
    n->position = pos;
    n->index = index;
    if (g->IsToken()) {
        int p = pos;
        SyntaxList lead(g->leading);
        n->leading.resize(static_cast<size_t>(lead.Count()));
        for (int i = 0; i < lead.Count(); i++) {
            SyntaxTrivia &t = n->leading[static_cast<size_t>(i)];
            t.green = lead[i];
            t.position = p;
            t.token = n;
            p += lead[i]->full_width;
        }
        p += static_cast<int>(g->text.size());
        SyntaxList trail(g->trailing);
        n->trailing.resize(static_cast<size_t>(trail.Count()));
        for (int i = 0; i < trail.Count(); i++) {
            SyntaxTrivia &t = n->trailing[static_cast<size_t>(i)];
            t.green = trail[i];
            t.position = p;
            t.token = n;
            p += trail[i]->full_width;
        }
        // Structured trivia, built after the vectors are final (their addresses are kept).
        for (SyntaxTrivia &t : n->leading) {
            if (t.green->IsNode()) t.structure = BuildNode(t.green, nullptr, &t, t.position, 0);
        }
        for (SyntaxTrivia &t : n->trailing) {
            if (t.green->IsNode()) t.structure = BuildNode(t.green, nullptr, &t, t.position, 0);
        }
        return n;
    }
    int p = pos;
    int idx = 0;
    for (int i = 0; i < g->slot_count; i++) {
        const GreenNode *s = g->slots[i];
        if (!s) continue;
        if (s->IsList()) {
            for (int j = 0; j < s->slot_count; j++) {
                const GreenNode *item = s->slots[j];
                if (!item) continue;
                n->children.push_back(BuildNode(item, n, nullptr, p, idx++));
                p += item->full_width;
            }
        } else {
            n->children.push_back(BuildNode(s, n, nullptr, p, idx++));
            p += s->full_width;
        }
    }
    return n;
}

namespace {

void CollectTokens(const SyntaxNode *n, std::vector<const SyntaxNode *> &out) {
    if (n->IsToken()) {
        out.push_back(n);
        return;
    }
    for (const SyntaxNode *c : n->children) CollectTokens(c, out);
}

/** @brief SyntaxTreeDiagnosticEnumerator: every diagnostic, in tree order, positioned as Roslyn positions them. */
void CollectDiagnostics(const GreenNode *root, int fullLength, std::vector<Diagnostic> &out) {
    if (!root->ContainsDiagnostics()) return;
    struct Frame {
        const GreenNode *node;
        int slot;
        bool processed;
    };
    std::vector<Frame> stack;
    int position = 0;
    auto push = [&](const GreenNode *node) {
        if (node->IsToken()) {
            // Push in reverse order of processing: trailing, token, leading.
            if (node->trailing) stack.push_back({node->trailing, -1, false});
            stack.push_back({node, -1, false});
            if (node->leading) stack.push_back({node->leading, -1, false});
        } else {
            stack.push_back({node, -1, false});
        }
    };
    push(root);
    while (!stack.empty()) {
        Frame &top = stack.back();
        const GreenNode *node = top.node;
        if (!top.processed) {
            for (int i = 0; i < node->diag_count; i++) {
                const SyntaxDiagnosticInfo &sdi = node->diags[i];
                int leadingWidthToAdd = node->IsToken() ? 0 : node->GetLeadingTriviaWidth();
                int spanStart = std::min(position + leadingWidthToAdd + sdi.offset, fullLength);
                int spanEnd = std::min(spanStart + sdi.width, fullLength);
                Diagnostic d;
                d.code = sdi.code;
                d.severity = ErrorSeverity(sdi.code);
                d.span = {spanStart, spanEnd};
                d.message = FormatDiagnosticMessage(sdi);
                out.push_back(std::move(d));
            }
            top.processed = true;
        }
        if (node->slot_count == 0) {
            position += node->IsToken() ? static_cast<int>(node->text.size()) : node->full_width;
            stack.pop_back();
            continue;
        }
        bool pushed = false;
        for (int next = top.slot + 1; next < node->slot_count; next++) {
            const GreenNode *child = node->slots[next];
            if (!child) continue;
            if (!child->ContainsDiagnostics()) {
                position += child->full_width;
                continue;
            }
            stack.back().slot = next;
            push(child);
            pushed = true;
            break;
        }
        if (!pushed) stack.pop_back();
    }
}

}  // namespace

void SyntaxTree::Build() {
    root_ = BuildNode(green_root_, nullptr, nullptr, 0, 0);
    CollectTokens(root_, tokens_);
    CollectDiagnostics(green_root_, green_root_->full_width, diagnostics_);
    // Offset maps.
    utf8_of_utf16_.assign(text_.size() + 1, 0);
    int u8 = 0;
    for (size_t i = 0; i < text_.size(); i++) {
        utf8_of_utf16_[i] = u8;
        char16_t c = text_[i];
        if (c < 0x80) {
            u8 += 1;
        } else if (c < 0x800) {
            u8 += 2;
        } else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < text_.size() && text_[i + 1] >= 0xDC00 &&
                   text_[i + 1] <= 0xDFFF) {
            utf8_of_utf16_[i + 1] = u8 + 2;  // inside the pair: not a boundary
            u8 += 4;
            i++;
        } else {
            u8 += 3;
        }
    }
    utf8_of_utf16_[text_.size()] = u8;
    line_starts_.assign(1, 0);
    for (size_t i = 0; i < text_.size(); i++) {
        char16_t c = text_[i];
        if (c == u'\r') {
            if (i + 1 < text_.size() && text_[i + 1] == u'\n') i++;
            line_starts_.push_back(static_cast<int>(i + 1));
        } else if (c == u'\n') {
            line_starts_.push_back(static_cast<int>(i + 1));
        }
    }
}

const SyntaxNode *SyntaxTree::FindToken(int pos) const {
    if (tokens_.empty()) return nullptr;
    // The last token whose full start is <= pos.
    auto it = std::upper_bound(tokens_.begin(), tokens_.end(), pos,
                               [](int p, const SyntaxNode *t) { return p < t->position; });
    if (it == tokens_.begin()) return tokens_.front();
    return *(it - 1);
}

const SyntaxNode *SyntaxTree::FindTokenOnLeft(int pos) const {
    const SyntaxNode *t = FindToken(pos);
    if (t && t->Span().start >= pos && pos > 0) {
        const SyntaxNode *prev = FindToken(pos - 1);
        if (prev && prev->Span().end == pos) return prev;
    }
    return t;
}

int SyntaxTree::Utf8Offset(int utf16) const {
    utf16 = std::clamp(utf16, 0, static_cast<int>(text_.size()));
    return utf8_of_utf16_[static_cast<size_t>(utf16)];
}

int SyntaxTree::Utf16Offset(int utf8) const {
    auto it = std::lower_bound(utf8_of_utf16_.begin(), utf8_of_utf16_.end(), utf8);
    return static_cast<int>(it - utf8_of_utf16_.begin());
}

void SyntaxTree::LineCol(int utf16, int *line, int *col) const {
    utf16 = std::clamp(utf16, 0, static_cast<int>(text_.size()));
    auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), utf16);
    int l = static_cast<int>(it - line_starts_.begin()) - 1;
    *line = l;
    *col = Utf8Offset(utf16) - Utf8Offset(line_starts_[static_cast<size_t>(l)]);
}

int SyntaxTree::OffsetOf(int line, int col) const {
    if (line < 0) return 0;
    if (line >= static_cast<int>(line_starts_.size())) return static_cast<int>(text_.size());
    int start = line_starts_[static_cast<size_t>(line)];
    int end = line + 1 < static_cast<int>(line_starts_.size()) ? line_starts_[static_cast<size_t>(line + 1)]
                                                               : static_cast<int>(text_.size());
    int target = Utf8Offset(start) + std::max(0, col);
    int off = Utf16Offset(target);
    return std::min(off, end);
}

std::string SyntaxTree::Utf8(TextSpan span) const {
    span.start = std::clamp(span.start, 0, static_cast<int>(text_.size()));
    span.end = std::clamp(span.end, span.start, static_cast<int>(text_.size()));
    return ToUtf8(Text(text_).substr(static_cast<size_t>(span.start), static_cast<size_t>(span.Length())));
}

// --- Dump (tools/csharp_oracle/DUMP.md) -------------------------------------------------------------

namespace {

void Indent(std::string &o, int depth) { o.append(static_cast<size_t>(depth) * 2, ' '); }

void DumpNode(std::string &o, const SyntaxNode *n, int depth);

void DumpTrivia(std::string &o, const SyntaxTrivia &t, char side, int depth) {
    Indent(o, depth);
    o += side;
    o += ' ';
    o += KindName(t.Kind());
    // Roslyn's trivia Span: a structured trivia's span is its node's span (without the node's own leading /
    // trailing trivia, e.g. a directive's end-of-line).
    TextSpan span = t.structure ? t.structure->Span() : t.Span();
    o += ' ' + std::to_string(span.start) + ' ' + std::to_string(span.end) + '\n';
    if (t.structure) DumpNode(o, t.structure, depth + 1);
}

void DumpNode(std::string &o, const SyntaxNode *n, int depth) {
    if (n->IsToken()) {
        for (const SyntaxTrivia &t : n->leading) DumpTrivia(o, t, 'L', depth);
        Indent(o, depth);
        TextSpan s = n->Span();
        o += "T ";
        o += KindName(n->Kind());
        o += ' ' + std::to_string(s.start) + ' ' + std::to_string(s.end);
        if (n->IsMissing()) o += " missing";
        o += '\n';
        for (const SyntaxTrivia &t : n->trailing) DumpTrivia(o, t, 'R', depth);
        return;
    }
    Indent(o, depth);
    TextSpan s = n->Span();
    o += "N ";
    o += KindName(n->Kind());
    o += ' ' + std::to_string(s.start) + ' ' + std::to_string(s.end);
    if (n->IsMissing()) o += " missing";
    o += '\n';
    for (const SyntaxNode *c : n->children) DumpNode(o, c, depth + 1);
}

std::string Escape(const std::string &s) {
    std::string out;
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c < 0x20 || c == 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

}  // namespace

std::string SyntaxTree::Dump(const std::string &path) const {
    std::string o = "=== " + path + "\n";
    DumpNode(o, root_, 0);
    for (const Diagnostic &d : diagnostics_) {
        o += "D " + d.Id() + ' ';
        o += d.severity == DiagnosticSeverity::Error     ? "error"
             : d.severity == DiagnosticSeverity::Warning ? "warning"
                                                         : "info";
        o += ' ' + std::to_string(d.span.start) + ' ' + std::to_string(d.span.end) + ' ' + Escape(d.message) + '\n';
    }
    return o;
}

}  // namespace csharp
