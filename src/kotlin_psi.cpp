// The Kotlin PSI tree (kotlin_psi.h): type names, node helpers, the dump.
//
// Ported from the Kotlin compiler (JetBrains, Apache License 2.0),
// KtTokens.java, KtNodeTypes.java, version 2.4.10.
#include "kotlin_psi.h"

#include <algorithm>
#include <cstdio>

namespace kt {

namespace {

const char *const kNames[kKtTypeCount] = {
    "<none>",
#define KT_X_TOKEN(e, s, k) s,
    KT_TOKEN_TYPES(KT_X_TOKEN)
#undef KT_X_TOKEN
#define KT_X_ELEMENT(e, s) s,
    KT_ELEMENT_TYPES(KT_X_ELEMENT)
#undef KT_X_ELEMENT
};

const unsigned char kKinds[kKtTypeCount] = {
    0,
#define KT_X_TOKEN(e, s, k) k,
    KT_TOKEN_TYPES(KT_X_TOKEN)
#undef KT_X_TOKEN
};

size_t Idx(KtType t) { return static_cast<size_t>(t); }

}  // namespace

const char *KtTypeName(KtType t) { return Idx(t) < kKtTypeCount ? kNames[Idx(t)] : "<bad>"; }

bool KtIsComment(KtType t) {
    return t == KtType::EOL_COMMENT || t == KtType::BLOCK_COMMENT || t == KtType::DOC_COMMENT ||
           t == KtType::SHEBANG_COMMENT;
}

bool KtIsWhitespaceOrComment(KtType t) { return t == KtType::WHITE_SPACE || KtIsComment(t); }

bool KtIsKeyword(KtType t) { return KtIsToken(t) && (kKinds[Idx(t)] == 1 || kKinds[Idx(t)] == 2); }

bool KtIsSoftKeyword(KtType t) { return KtIsToken(t) && kKinds[Idx(t)] == 2; }

bool KtIsModifierKeyword(KtType t) {
    switch (t) {
    case KtType::PUBLIC_KEYWORD: case KtType::PROTECTED_KEYWORD: case KtType::PRIVATE_KEYWORD:
    case KtType::INTERNAL_KEYWORD: case KtType::EXPECT_KEYWORD: case KtType::ACTUAL_KEYWORD:
    case KtType::FINAL_KEYWORD: case KtType::OPEN_KEYWORD: case KtType::ABSTRACT_KEYWORD:
    case KtType::SEALED_KEYWORD: case KtType::CONST_KEYWORD: case KtType::EXTERNAL_KEYWORD:
    case KtType::OVERRIDE_KEYWORD: case KtType::LATEINIT_KEYWORD: case KtType::TAILREC_KEYWORD:
    case KtType::VARARG_KEYWORD: case KtType::SUSPEND_KEYWORD: case KtType::INNER_KEYWORD:
    case KtType::ENUM_KEYWORD: case KtType::ANNOTATION_KEYWORD: case KtType::FUN_KEYWORD:
    case KtType::COMPANION_KEYWORD: case KtType::INLINE_KEYWORD: case KtType::VALUE_KEYWORD:
    case KtType::INFIX_KEYWORD: case KtType::OPERATOR_KEYWORD: case KtType::DATA_KEYWORD:
    case KtType::OUT_KEYWORD: case KtType::IN_KEYWORD: case KtType::REIFIED_KEYWORD:
    case KtType::NOINLINE_KEYWORD: case KtType::CROSSINLINE_KEYWORD:
        return true;
    default:
        return false;
    }
}

std::string_view KtNode::Text() const {
    if (!tree || end < start || end > tree->text.size()) return {};
    return std::string_view(tree->text).substr(start, end - start);
}

std::string_view KtNode::Message() const {
    if (!tree || message < 0 || static_cast<size_t>(message) >= tree->messages.size()) return {};
    return tree->messages[static_cast<size_t>(message)];
}

KtNode *KtNode::Child(KtType t) const {
    for (KtNode *c = first_child; c; c = c->next)
        if (c->type == t) return c;
    return nullptr;
}

KtNode *KtNode::LastChild(KtType t) const {
    for (KtNode *c = last_child; c; c = c->prev)
        if (c->type == t) return c;
    return nullptr;
}

std::vector<KtNode *> KtNode::Children(KtType t) const {
    std::vector<KtNode *> out;
    for (KtNode *c = first_child; c; c = c->next)
        if (c->type == t) out.push_back(c);
    return out;
}

std::vector<KtNode *> KtNode::Children() const {
    std::vector<KtNode *> out;
    for (KtNode *c = first_child; c; c = c->next) out.push_back(c);
    return out;
}

KtNode *KtNode::NextNonTrivia() const {
    KtNode *n = next;
    while (n && n->IsTrivia()) n = n->next;
    return n;
}

KtNode *KtNode::PrevNonTrivia() const {
    KtNode *n = prev;
    while (n && n->IsTrivia()) n = n->prev;
    return n;
}

KtNode *KtNode::FirstNonTriviaChild() const {
    KtNode *n = first_child;
    while (n && n->IsTrivia()) n = n->next;
    return n;
}

KtNode *KtNode::LastNonTriviaChild() const {
    KtNode *n = last_child;
    while (n && n->IsTrivia()) n = n->prev;
    return n;
}

KtNode *KtNode::Ancestor(KtType t) const {
    for (KtNode *p = parent; p; p = p->parent)
        if (p->type == t) return p;
    return nullptr;
}

const KtNode *KtNode::FirstLeaf() const {
    if (IsLeaf()) return this;
    for (const KtNode *c = first_child; c; c = c->next)
        if (const KtNode *l = c->FirstLeaf()) return l;
    return nullptr;
}

const KtNode *KtNode::LastLeaf() const {
    if (IsLeaf()) return this;
    for (const KtNode *c = last_child; c; c = c->prev)
        if (const KtNode *l = c->LastLeaf()) return l;
    return nullptr;
}

std::string KtTree::Dump() const {
    std::string out;
    if (nodes.empty()) return out;
    // Iterative pre-order walk (trees can be deep).
    std::vector<std::pair<const KtNode *, int>> stack;
    stack.push_back({&nodes[0], 0});
    char buf[64];
    while (!stack.empty()) {
        auto [n, depth] = stack.back();
        stack.pop_back();
        out.append(static_cast<size_t>(depth) * 2, ' ');
        out += KtTypeName(n->type);
        std::snprintf(buf, sizeof buf, " %u %u", n->start, n->end);
        out += buf;
        if (n->type == KtType::ERROR_ELEMENT) {
            out += ' ';
            out += n->Message();
        }
        out += '\n';
        for (const KtNode *c = n->last_child; c; c = c->prev) stack.push_back({c, depth + 1});
    }
    return out;
}

void KtTree::LineCol(uint32_t offset, int *line, int *col) const {
    int l = 1, c = 1;
    size_t end = std::min<size_t>(offset, text.size());
    for (size_t i = 0; i < end;) {
        unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch == '\n') {
            l++;
            c = 1;
            i++;
            continue;
        }
        size_t len = ch < 0x80 ? 1 : ch < 0xE0 ? 2 : ch < 0xF0 ? 3 : 4;
        if (ch >= 0x80 && ch < 0xC0) len = 1;  // stray continuation byte
        c += len == 4 ? 2 : 1;
        i += len;
    }
    *line = l;
    *col = c;
}

uint32_t KtTree::ToOriginal(uint32_t offset) const {
    if (original_offsets.empty()) return offset;
    if (offset >= original_offsets.size()) return original_offsets.back();
    return original_offsets[offset];
}

const KtNode *KtTree::LeafAt(uint32_t offset) const {
    const KtNode *n = Root();
    while (n && !n->IsLeaf()) {
        const KtNode *pick = nullptr;
        for (const KtNode *c = n->first_child; c; c = c->next) {
            if (c->start <= offset && (offset < c->end || (c->end == offset && !c->next))) {
                if (c->start == c->end) continue;
                pick = c;
                break;
            }
        }
        if (!pick) return n == Root() ? nullptr : n;
        n = pick;
    }
    return n;
}

std::string KtNormalizeLineEnds(std::string_view in, std::vector<uint32_t> *map) {
    std::string out;
    out.reserve(in.size());
    if (map) {
        map->clear();
        map->reserve(in.size() + 1);
    }
    for (size_t i = 0; i < in.size(); i++) {
        char ch = in[i];
        if (map) map->push_back(static_cast<uint32_t>(i));
        if (ch == '\r') {
            out += '\n';
            if (i + 1 < in.size() && in[i + 1] == '\n') i++;
        } else {
            out += ch;
        }
    }
    if (map) map->push_back(static_cast<uint32_t>(in.size()));
    return out;
}

}  // namespace kt
