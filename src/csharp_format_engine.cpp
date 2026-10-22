// mep's C# formatter: the engine. A port of Roslyn's AbstractFormatEngine (+
// OperationApplier), TokenStream, TreeData, FormattingContext (+ its interval
// trees: ContextMutableIntervalTree over MutableIntervalTree) and
// FormattingResult/TriviaRewriter. See csharp_format_internal.h (Roslyn is
// MIT-licensed, Copyright (c) .NET Foundation and Contributors).

#include <algorithm>
#include <climits>
#include <cstring>
#include <mutex>
#include <set>
#include <unordered_set>
#include <utility>

#include "csharp_format_internal.h"

namespace csfmt {

using csharp::Field;

// --- Syntax helpers ------------------------------------------------------------------------------

namespace {

bool HasWidth(Token t) { return t && !t->green->text.empty(); }

/** @brief Token navigation over the red tree (both directions), Roslyn's SyntaxNavigator without trivia. */
Token StepToken(Token tok, int dir, bool include_zero_width) {
    const Node *n = tok;
    while (n) {
        const Node *p = n->parent;
        if (!p) return nullptr;
        int i = n->index + dir;
        while (i >= 0 && i < static_cast<int>(p->children.size())) {
            const Node *c = p->children[static_cast<size_t>(i)];
            Token t = dir > 0 ? FirstToken(c, include_zero_width) : LastToken(c, include_zero_width);
            if (t) return t;
            i += dir;
        }
        n = p;
    }
    return nullptr;
}

}  // namespace

Token FirstToken(const Node *n, bool include_zero_width) {
    if (!n) return nullptr;
    if (n->IsToken()) return (include_zero_width || HasWidth(n)) ? n : nullptr;
    for (const Node *c : n->children) {
        Token t = FirstToken(c, include_zero_width);
        if (t) return t;
    }
    return nullptr;
}

Token LastToken(const Node *n, bool include_zero_width) {
    if (!n) return nullptr;
    if (n->IsToken()) return (include_zero_width || HasWidth(n)) ? n : nullptr;
    for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) {
        Token t = LastToken(*it, include_zero_width);
        if (t) return t;
    }
    return nullptr;
}

const Node *F(const Node *n, Field f) {
    if (!n || n->IsToken()) return nullptr;
    int slot = csharp::FieldSlot(n->Kind(), f);
    if (slot < 0) return nullptr;
    const csharp::GreenNode *g = n->green;
    size_t ci = 0;
    for (int i = 0; i < g->slot_count; i++) {
        const csharp::GreenNode *s = g->slots[i];
        if (!s) {
            if (i == slot) return nullptr;
            continue;
        }
        if (i == slot) return ci < n->children.size() ? n->children[ci] : nullptr;
        ci += s->IsList() ? static_cast<size_t>(s->slot_count) : 1;
    }
    return nullptr;
}

std::vector<const Node *> Items(const Node *n, Field f, bool skip_separators) {
    std::vector<const Node *> out;
    if (!n || n->IsToken()) return out;
    int slot = csharp::FieldSlot(n->Kind(), f);
    if (slot < 0) return out;
    const csharp::GreenNode *g = n->green;
    size_t ci = 0;
    for (int i = 0; i < g->slot_count; i++) {
        const csharp::GreenNode *s = g->slots[i];
        if (!s) continue;
        size_t cnt = s->IsList() ? static_cast<size_t>(s->slot_count) : 1;
        if (i == slot) {
            for (size_t k = 0; k < cnt && ci + k < n->children.size(); k++) {
                const Node *c = n->children[ci + k];
                // Separators of a separated list are tokens between node items.
                if (skip_separators && c->IsToken() && s->IsList() && cnt > 1 && !n->children[ci]->IsToken()) continue;
                out.push_back(c);
            }
            return out;
        }
        ci += cnt;
    }
    return out;
}

uint64_t ClassBits(SyntaxKind kind) {
    static std::once_flag once;
    static std::vector<uint64_t> table;
    std::call_once(once, [] {
        table.assign(10000, 0);
        struct Base {
            const char *cls;
            uint64_t bit;
        };
        static const Base bases[] = {
            {"StatementSyntax", kStatement},
            {"MemberDeclarationSyntax", kMemberDeclaration},
            {"ExpressionSyntax", kExpression},
            {"BaseTypeDeclarationSyntax", kBaseTypeDeclaration},
            {"AnonymousFunctionExpressionSyntax", kAnonymousFunctionExpression},
            {"BaseObjectCreationExpressionSyntax", kBaseObjectCreationExpression},
            {"InitializerExpressionSyntax", kInitializerExpression},
            {"PrefixUnaryExpressionSyntax", kPrefixUnaryExpression},
            {"PostfixUnaryExpressionSyntax", kPostfixUnaryExpression},
            {"BinaryExpressionSyntax", kBinaryExpression},
            {"AssignmentExpressionSyntax", kAssignmentExpression},
            {"SwitchLabelSyntax", kSwitchLabel},
            {"StructuredTriviaSyntax", kStructuredTrivia},
            {"BranchingDirectiveTriviaSyntax", kBranchingDirective},
            {"AttributeListSyntax", kAttributeList},
            {"AccessorDeclarationSyntax", kAccessorDeclaration},
            {"PointerTypeSyntax", kPointerType},
            {"DestructorDeclarationSyntax", kDestructorDeclaration},
            {"QueryClauseSyntax", kQueryClause},
            {"SelectOrGroupClauseSyntax", kSelectOrGroupClause},
        };
        for (int k = 0; k < 10000; k++) {
            const char *cls = csharp::NodeClassName(static_cast<SyntaxKind>(k));
            if (!cls || !*cls) continue;
            uint64_t bits = 0;
            for (const Base &b : bases) {
                if (csharp::KindDerivesFrom(static_cast<SyntaxKind>(k), b.cls)) bits |= b.bit;
            }
            table[static_cast<size_t>(k)] = bits;
        }
    });
    size_t k = static_cast<size_t>(kind);
    return k < table.size() ? table[k] : 0;
}

// --- Tree ----------------------------------------------------------------------------------------

namespace {

void CollectTokens(const Node *n, std::vector<Token> &out) {
    if (n->IsToken()) {
        out.push_back(n);
        return;
    }
    for (const Node *c : n->children) CollectTokens(c, out);
}

}  // namespace

Tree::Tree(const csharp::SyntaxTree *tree) : tree_(tree) {
    CollectTokens(tree->Root(), tokens_);
    index_.reserve(tokens_.size() * 2);
    for (size_t i = 0; i < tokens_.size(); i++) index_[tokens_[i]] = static_cast<int>(i);
    // Region pairs (DirectiveSyntaxExtensions.GetDirectiveInfoForRoot): leading trivia of every token, in order.
    std::vector<const Node *> regions;
    for (Token t : tokens_) {
        for (const csharp::SyntaxTrivia &tr : t->leading) {
            if (tr.Kind() == SyntaxKind::RegionDirectiveTrivia) {
                regions.push_back(tr.structure);
            } else if (tr.Kind() == SyntaxKind::EndRegionDirectiveTrivia && !regions.empty()) {
                region_pairs_[tr.structure] = regions.back();
                regions.pop_back();
            }
        }
    }
}

int Tree::IndexOf(Token t) const {
    auto it = index_.find(t);
    return it == index_.end() ? -1 : it->second;
}

Token Tree::NextToken(Token t, bool include_zero_width) const {
    if (!t) return nullptr;
    int i = IndexOf(t);
    if (i < 0) return StepToken(t, +1, include_zero_width);
    for (size_t j = static_cast<size_t>(i) + 1; j < tokens_.size(); j++) {
        if (include_zero_width || HasWidth(tokens_[j])) return tokens_[j];
    }
    return nullptr;
}

Token Tree::PreviousToken(Token t, bool include_zero_width) const {
    if (!t) return nullptr;
    int i = IndexOf(t);
    if (i < 0) return StepToken(t, -1, include_zero_width);
    for (int j = i - 1; j >= 0; j--) {
        if (include_zero_width || HasWidth(tokens_[static_cast<size_t>(j)])) return tokens_[static_cast<size_t>(j)];
    }
    return nullptr;
}

const Node *Tree::MatchingRegion(const Node *endregion) const {
    auto it = region_pairs_.find(endregion);
    return it == region_pairs_.end() ? nullptr : it->second;
}

Trivia Tree::FindTrivia(int pos) const {
    if (pos < 0) return nullptr;
    Token t = tree_->FindToken(pos);
    if (!t) return nullptr;
    for (const csharp::SyntaxTrivia &tr : t->leading) {
        if (pos >= tr.position && pos < tr.position + tr.green->full_width) return &tr;
    }
    for (const csharp::SyntaxTrivia &tr : t->trailing) {
        if (pos >= tr.position && pos < tr.position + tr.green->full_width) return &tr;
    }
    return nullptr;
}

// --- Strings -------------------------------------------------------------------------------------

U16View TriviaText(const Tree &tree, Trivia t) {
    return U16View(tree.Text()).substr(static_cast<size_t>(t->position), static_cast<size_t>(t->green->full_width));
}

bool IsDocComment(SyntaxKind k) {
    return k == SyntaxKind::SingleLineDocumentationCommentTrivia || k == SyntaxKind::MultiLineDocumentationCommentTrivia;
}

bool IsRegularOrDocComment(SyntaxKind k) {
    return k == SyntaxKind::MultiLineCommentTrivia || k == SyntaxKind::SingleLineCommentTrivia ||
           k == SyntaxKind::ShebangDirectiveTrivia || IsDocComment(k);
}

int ConvertTabToSpace(U16View text, int tab_size, int initial_column, int end_position) {
    int column = initial_column;
    for (int i = 0; i < end_position; i++) {
        if (text[static_cast<size_t>(i)] == u'\t') {
            column += tab_size - column % tab_size;
        } else {
            column++;
        }
    }
    return column - initial_column;
}

int GetColumnFromLineOffset(U16View line, int end_position, int tab_size) {
    return ConvertTabToSpace(line, tab_size, 0, end_position);
}

U16View GetLastLineText(U16View text) {
    size_t lb = text.rfind(u'\n');
    if (lb == U16View::npos) return text;
    return text.substr(lb + 1);
}

bool ContainsLineBreak(U16View text) {
    for (char16_t c : text) {
        if (c == u'\n' || c == u'\r') return true;
    }
    return false;
}

int GetNumberOfLineBreaks(U16View text) {
    int n = 0;
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == u'\n') {
            n++;
        } else if (text[i] == u'\r') {
            if (i + 1 == text.size() || text[i + 1] != u'\n') n++;
        }
    }
    return n;
}

int GetTextColumn(U16View text, int tab_size, int initial_column) {
    U16View line = GetLastLineText(text);
    if (line.size() != text.size()) return GetColumnFromLineOffset(line, static_cast<int>(line.size()), tab_size);
    return ConvertTabToSpace(text, tab_size, initial_column, static_cast<int>(text.size())) + initial_column;
}

int GetFirstNonWhitespaceIndexInString(U16View text) {
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] != u' ' && text[i] != u'\t') return static_cast<int>(i);
    }
    return -1;
}

U16 CreateIndentationString(int desired, bool use_tab, int tab_size) {
    int tabs = 0;
    int spaces = std::max(0, desired);
    if (use_tab) {
        tabs = desired / tab_size;
        spaces -= tabs * tab_size;
    }
    return U16(static_cast<size_t>(std::max(0, tabs)), u'\t') + U16(static_cast<size_t>(std::max(0, spaces)), u' ');
}

bool IsCSharpWhitespace(char16_t c) { return csharp::SyntaxFacts::IsWhitespace(c); }
bool IsCSharpNewLine(char16_t c) { return csharp::SyntaxFacts::IsNewLine(c); }

bool IsDotNetWhiteSpace(char16_t c) {
    // char.IsWhiteSpace: Unicode White_Space (Zs, Zl, Zp, and U+0009-U+000D, U+0085, U+00A0).
    if (c == u' ' || (c >= 0x09 && c <= 0x0D) || c == 0x85 || c == 0xA0) return true;
    if (c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
        c == 0x3000)
        return true;
    return false;
}

std::vector<Trivia> TriviaListBetween(Token token1, Token token2) {
    std::vector<Trivia> list;
    if (token1) {
        for (const csharp::SyntaxTrivia &t : token1->trailing) list.push_back(&t);
    }
    if (token2) {
        for (const csharp::SyntaxTrivia &t : token2->leading) list.push_back(&t);
    }
    return list;
}

// --- Interval tree (MutableIntervalTree + ContextMutableIntervalTree) -----------------------------

namespace {

template <class T>
class IntervalTree {
public:
    using SpanFn = TextSpan (*)(T);
    explicit IntervalTree(SpanFn fn) : span_(fn) {}

    bool Empty() const { return root_ == nullptr; }

    void AddIntervalInPlace(T value) {
        nodes_.push_back(std::make_unique<NodeT>(value));
        NodeT *nn = nodes_.back().get();
        root_ = Insert(root_, nn, span_(value).start);
    }

    T GetSmallestEdgeExclusivelyContainingInterval(int start, int length) const {
        return Worker(start, length, [this](T v, int s, int l) {
            TextSpan sp = span_(v);
            return sp.start < s && s + l < sp.end;
        });
    }

    T GetSmallestContainingInterval(int start, int length) const {
        return Worker(start, length, [this](T v, int s, int l) {
            TextSpan sp = span_(v);
            int other_end = s + l;
            if (l == 0) return sp.start <= s && other_end < sp.end;
            return sp.start <= s && other_end <= sp.end;
        });
    }

    bool HasIntervalThatIntersectsWith(int start, int length) const {
        if (!root_) return false;
        int end = start + length;
        std::vector<NodeT *> candidates{root_};
        while (!candidates.empty()) {
            NodeT *cur = candidates.back();
            candidates.pop_back();
            TextSpan sp = span_(cur->value);
            if (start <= sp.end && end >= sp.start) return true;
            NodeT *right = ExamineRight(start, end, cur);
            if (right) candidates.push_back(right);
            NodeT *left = ExamineLeft(start, cur);
            if (left) candidates.push_back(left);
        }
        return false;
    }

    /** @brief Every value, in order (the NodeEnumerator over the whole tree). */
    std::vector<T> InOrder() const {
        std::vector<T> out;
        std::vector<NodeT *> stack;
        NodeT *cur = root_;
        while (cur || !stack.empty()) {
            while (cur) {
                stack.push_back(cur);
                cur = cur->left;
            }
            cur = stack.back();
            stack.pop_back();
            out.push_back(cur->value);
            cur = cur->right;
        }
        return out;
    }

private:
    struct NodeT {
        explicit NodeT(T v) : value(v), max_end(this) {}
        T value;
        NodeT *left = nullptr;
        NodeT *right = nullptr;
        int height = 1;
        NodeT *max_end;
    };

    int End(T v) const { return span_(v).end; }
    int MaxEnd(const NodeT *n) const { return n ? End(n->max_end->value) : 0; }
    static int Height(const NodeT *n) { return n ? n->height : 0; }

    void SetLeftRight(NodeT *n, NodeT *left, NodeT *right) const {
        n->left = left;
        n->right = right;
        n->height = 1 + std::max(Height(left), Height(right));
        int this_end = End(n->value);
        int left_end = MaxEnd(left);
        int right_end = MaxEnd(right);
        if (this_end >= left_end && this_end >= right_end) {
            n->max_end = n;
        } else if (left_end >= right_end && left) {
            n->max_end = left->max_end;
        } else if (right) {
            n->max_end = right->max_end;
        }
    }

    NodeT *RightRotation(NodeT *n) const {
        NodeT *old_left = n->left;
        SetLeftRight(n, n->left->right, n->right);
        SetLeftRight(old_left, old_left->left, n);
        return old_left;
    }
    NodeT *LeftRotation(NodeT *n) const {
        NodeT *old_right = n->right;
        SetLeftRight(n, n->left, n->right->left);
        SetLeftRight(old_right, n, old_right->right);
        return old_right;
    }
    NodeT *InnerRightOuterLeftRotation(NodeT *n) const {
        NodeT *new_top = n->right->left;
        NodeT *old_right = n->right;
        SetLeftRight(n, n->left, n->right->left->left);
        SetLeftRight(old_right, old_right->left->right, old_right->right);
        SetLeftRight(new_top, n, old_right);
        return new_top;
    }
    NodeT *InnerLeftOuterRightRotation(NodeT *n) const {
        NodeT *new_top = n->left->right;
        NodeT *old_left = n->left;
        SetLeftRight(n, n->left->right->right, n->right);
        SetLeftRight(old_left, old_left->left, old_left->right->left);
        SetLeftRight(new_top, old_left, n);
        return new_top;
    }
    static int BalanceFactor(const NodeT *n) { return n ? Height(n->left) - Height(n->right) : 0; }

    NodeT *Insert(NodeT *root, NodeT *nn, int start) const {
        if (!root) return nn;
        NodeT *left;
        NodeT *right;
        if (start < span_(root->value).start) {
            left = Insert(root->left, nn, start);
            right = root->right;
        } else {
            left = root->left;
            right = Insert(root->right, nn, start);
        }
        SetLeftRight(root, left, right);
        int bf = BalanceFactor(root);
        if (bf == -2) {
            return BalanceFactor(root->right) == -1 ? LeftRotation(root) : InnerRightOuterLeftRotation(root);
        }
        if (bf == 2) {
            return BalanceFactor(root->left) == 1 ? RightRotation(root) : InnerLeftOuterRightRotation(root);
        }
        return root;
    }

    NodeT *ExamineRight(int start, int end, NodeT *cur) const {
        if (span_(cur->value).start <= end) {
            if (cur->right && span_(cur->right->max_end->value).end >= start) return cur->right;
        }
        return nullptr;
    }
    NodeT *ExamineLeft(int start, NodeT *cur) const {
        if (cur->left && span_(cur->left->max_end->value).end >= start) return cur->left;
        return nullptr;
    }

    template <class Pred>
    T Worker(int start, int length, Pred predicate) const {
        T result = nullptr;
        if (!root_ || MaxEnd(root_) < start) return result;
        int end = start + length;
        std::vector<NodeT *> spine{root_};
        while (!spine.empty()) {
            NodeT *current = spine.back();
            if (span_(current->value).start <= start) {
                NodeT *right = current->right;
                if (right && end < MaxEnd(right)) {
                    spine.push_back(right);
                    continue;
                }
            }
            NodeT *left = current->left;
            if (left && end <= MaxEnd(left)) {
                spine.push_back(left);
                continue;
            }
            bool restarted = false;
            while (!spine.empty()) {
                current = spine.back();
                spine.pop_back();
                if (predicate(current->value, start, length)) {
                    if (result == nullptr) {
                        result = current->value;
                    } else {
                        TextSpan rs = span_(result);
                        TextSpan cs = span_(current->value);
                        if (rs.start <= cs.start && cs.Length() < rs.Length()) result = current->value;
                    }
                }
                if (spine.empty()) return result;
                NodeT *parent = spine.back();
                if (parent->left == current) continue;
                if (parent->right == current) {
                    if (parent->left && end <= MaxEnd(parent->left)) {
                        if (result == nullptr || span_(parent->value).start == span_(current->value).start) {
                            spine.push_back(parent->left);
                            restarted = true;
                            break;
                        }
                    }
                    continue;
                }
            }
            (void)restarted;
        }
        return result;
    }

    SpanFn span_;
    NodeT *root_ = nullptr;
    std::vector<std::unique_ptr<NodeT>> nodes_;
};

/** @brief .NET's ArraySortHelper<T>.IntrospectiveSort (List<T>.Sort is not stable; ties land where it puts them). */
template <class T, class Cmp>
void SwapIfGreater(std::vector<T> &k, size_t off, Cmp &cmp, int i, int j) {
    if (cmp(k[off + static_cast<size_t>(i)], k[off + static_cast<size_t>(j)]) > 0)
        std::swap(k[off + static_cast<size_t>(i)], k[off + static_cast<size_t>(j)]);
}

template <class T, class Cmp>
void InsertionSort(std::vector<T> &k, size_t off, int n, Cmp &cmp) {
    for (int i = 0; i < n - 1; i++) {
        T t = k[off + static_cast<size_t>(i + 1)];
        int j = i;
        while (j >= 0 && cmp(t, k[off + static_cast<size_t>(j)]) < 0) {
            k[off + static_cast<size_t>(j + 1)] = k[off + static_cast<size_t>(j)];
            j--;
        }
        k[off + static_cast<size_t>(j + 1)] = t;
    }
}

template <class T, class Cmp>
void DownHeap(std::vector<T> &k, size_t off, int i, int n, Cmp &cmp) {
    T d = k[off + static_cast<size_t>(i - 1)];
    while (i <= n >> 1) {
        int child = 2 * i;
        if (child < n && cmp(k[off + static_cast<size_t>(child - 1)], k[off + static_cast<size_t>(child)]) < 0) child++;
        if (!(cmp(d, k[off + static_cast<size_t>(child - 1)]) < 0)) break;
        k[off + static_cast<size_t>(i - 1)] = k[off + static_cast<size_t>(child - 1)];
        i = child;
    }
    k[off + static_cast<size_t>(i - 1)] = d;
}

template <class T, class Cmp>
void HeapSort(std::vector<T> &k, size_t off, int n, Cmp &cmp) {
    for (int i = n >> 1; i >= 1; i--) DownHeap(k, off, i, n, cmp);
    for (int i = n; i > 1; i--) {
        std::swap(k[off], k[off + static_cast<size_t>(i - 1)]);
        DownHeap(k, off, 1, i - 1, cmp);
    }
}

template <class T, class Cmp>
int PickPivotAndPartition(std::vector<T> &k, size_t off, int n, Cmp &cmp) {
    int hi = n - 1;
    int middle = hi >> 1;
    SwapIfGreater(k, off, cmp, 0, middle);
    SwapIfGreater(k, off, cmp, 0, hi);
    SwapIfGreater(k, off, cmp, middle, hi);
    T pivot = k[off + static_cast<size_t>(middle)];
    std::swap(k[off + static_cast<size_t>(middle)], k[off + static_cast<size_t>(hi - 1)]);
    int left = 0;
    int right = hi - 1;
    while (left < right) {
        while (cmp(k[off + static_cast<size_t>(++left)], pivot) < 0) {
        }
        while (cmp(pivot, k[off + static_cast<size_t>(--right)]) < 0) {
        }
        if (left >= right) break;
        std::swap(k[off + static_cast<size_t>(left)], k[off + static_cast<size_t>(right)]);
    }
    if (left != hi - 1) std::swap(k[off + static_cast<size_t>(left)], k[off + static_cast<size_t>(hi - 1)]);
    return left;
}

template <class T, class Cmp>
void IntroSort(std::vector<T> &k, size_t off, int n, int depth_limit, Cmp &cmp) {
    int partition_size = n;
    while (partition_size > 1) {
        if (partition_size <= 16) {
            if (partition_size == 2) {
                SwapIfGreater(k, off, cmp, 0, 1);
                return;
            }
            if (partition_size == 3) {
                SwapIfGreater(k, off, cmp, 0, 1);
                SwapIfGreater(k, off, cmp, 0, 2);
                SwapIfGreater(k, off, cmp, 1, 2);
                return;
            }
            InsertionSort(k, off, partition_size, cmp);
            return;
        }
        if (depth_limit == 0) {
            HeapSort(k, off, partition_size, cmp);
            return;
        }
        depth_limit--;
        int p = PickPivotAndPartition(k, off, partition_size, cmp);
        IntroSort(k, off + static_cast<size_t>(p + 1), partition_size - (p + 1), depth_limit, cmp);
        partition_size = p;
    }
}

template <class T, class Cmp>
void DotNetSort(std::vector<T> &k, Cmp cmp) {
    if (k.size() <= 1) return;
    int n = static_cast<int>(k.size());
    int log2 = 0;
    for (unsigned v = static_cast<unsigned>(n); v > 1; v >>= 1) log2++;
    IntroSort(k, 0, n, 2 * (log2 + 1), cmp);
}

}  // namespace

// --- TokenStream ---------------------------------------------------------------------------------

TokenStream::TokenStream(Engine &engine, std::vector<Token> tokens) : engine_(engine), tokens_(std::move(tokens)) {
    index_.reserve(tokens_.size() * 2);
    for (size_t i = 0; i < tokens_.size(); i++) index_[tokens_[i]] = static_cast<int>(i);
    original_.assign(tokens_.size() > 0 ? tokens_.size() - 1 : 0, nullptr);
    changes_.assign(original_.size(), nullptr);
}

bool TokenStream::FormatBeginningOfTree() const { return engine_.IsFirstToken(tokens_.front()); }
bool TokenStream::FormatEndOfTree() const { return engine_.IsLastToken(tokens_.back()); }

TokenData TokenStream::GetTokenData(Token token) const {
    auto it = index_.find(token);
    return {it == index_.end() ? -1 : it->second, token};
}

TokenData TokenStream::GetPreviousTokenData(TokenData t) const {
    if (t.index > 0 && t.index < TokenCount()) return {t.index - 1, tokens_[static_cast<size_t>(t.index - 1)]};
    // Outside a structured trivia's stream the walk stops (Roslyn would step out into the main tree; only
    // columns of tokens inside directives depend on it, and nothing formats by them).
    Token prev = engine_.tree.PreviousToken(t.token, true);
    if (prev && tokens_.back() == prev) return {TokenCount() - 1, prev};
    return {-1, prev};
}

TokenData TokenStream::GetNextTokenData(TokenData t) const {
    if (t.index >= 0 && t.index < TokenCount() - 1) return {t.index + 1, tokens_[static_cast<size_t>(t.index + 1)]};
    Token next = engine_.tree.NextToken(t.token, true);
    if (next && tokens_.front() == next) return {0, next};
    return {-1, next};
}

Token TokenStream::FirstTokenOfBaseTokenLine(Token token) const {
    TokenData current = GetTokenData(token);
    while (!IsFirstTokenOnLine(token)) {
        TokenData prev = GetPreviousTokenData(current);
        token = prev.token;
        current = prev;
        if (!token) break;
    }
    return token;
}

bool TokenStream::TwoTokensOnSameLineWorker(Token t1, Token t2, bool original) const {
    if (t1 == t2) return true;
    if (SpanOf(t1).end > SpanStart(t2)) return false;
    TokenData d1 = GetTokenData(t1);
    TokenData d2 = GetTokenData(t2);
    if (d1.index < 0 || d2.index < 0) {
        // Tokens outside the stream (inside structured trivia): their original text decides.
        U16View between = engine_.tree.TextOf(FromBounds(SpanOf(t1).end, SpanStart(t2)));
        return !ContainsLineBreak(between);
    }
    TokenData prev = d1;
    for (TokenData cur = GetNextTokenData(d1); cur.index >= 0 && cur.index < d2.index; cur = GetNextTokenData(cur)) {
        if (Getter(prev, cur, original)->SecondTokenIsFirstTokenOnLine()) return false;
        prev = cur;
    }
    return !Getter(prev, d2, original)->SecondTokenIsFirstTokenOnLine();
}

void TokenStream::ApplyChange(int pair_index, TriviaData *data) {
    bool same = GetOriginalTriviaData(pair_index) == data;
    changes_[static_cast<size_t>(pair_index)] = same ? nullptr : data;
}

int TokenStream::GetColumn(TokenData t, bool original) const {
    TokenData prev = GetPreviousTokenData(t);
    int spaces = 0;
    for (; prev.token; prev = GetPreviousTokenData(prev)) {
        TriviaData *info = Getter(prev, t, original);
        if (info->SecondTokenIsFirstTokenOnLine()) return spaces + info->spaces;
        spaces += info->spaces;
        int len = 0;
        bool multi = false;
        GetTokenLength(prev.token, &len, &multi);
        if (multi) return spaces + len;
        spaces += len;
        t = prev;
    }
    return spaces + Getter(prev, t, original)->spaces;
}

void TokenStream::GetTokenLength(Token token, int *length, bool *on_multiple_lines) const {
    U16View text = TokenText(token);
    int tab = engine_.options.tab_size;
    if (ContainsLineBreak(text)) {
        *on_multiple_lines = true;
        *length = GetTextColumn(text, tab, 0);
        return;
    }
    *on_multiple_lines = false;
    if (text.find(u'\t') != U16View::npos) {
        int initial = engine_.GetOriginalColumn(token);
        *length = ConvertTabToSpace(text, tab, initial, static_cast<int>(text.size()));
        return;
    }
    *length = static_cast<int>(text.size());
}

TriviaData *TokenStream::GetTriviaData(TokenData t1, TokenData t2) const {
    if (engine_.IsFirstToken(t2.token)) {
        return FormatBeginningOfTree() ? GetTriviaDataAtBeginningOfTree() : GetOriginalTriviaData(t1, t2);
    }
    if (engine_.IsLastToken(t1.token)) {
        return FormatEndOfTree() ? GetTriviaDataAtEndOfTree() : GetOriginalTriviaData(t1, t2);
    }
    if (t1.index < 0 || t2.index < 0) return GetOriginalTriviaData(t1, t2);
    return GetTriviaData(t1.index);
}

TriviaData *TokenStream::GetOriginalTriviaData(TokenData t1, TokenData t2) const {
    if (engine_.IsFirstToken(t2.token)) return engine_.factory->CreateLeadingTrivia(t2.token);
    if (engine_.IsLastToken(t1.token)) return engine_.factory->CreateTrailingTrivia(t1.token);
    if (t1.index < 0 || t2.index < 0) return engine_.factory->Create(t1.token, t2.token);
    return GetOriginalTriviaData(t1.index);
}

TriviaData *TokenStream::GetTriviaDataAtBeginningOfTree() const {
    if (begin_change_) return begin_change_;
    return GetOriginalTriviaData(TokenData{-1, nullptr}, FirstTokenInStream());
}

TriviaData *TokenStream::GetTriviaDataAtEndOfTree() const {
    if (end_change_) return end_change_;
    return GetOriginalTriviaData(LastTokenInStream(), TokenData{-1, nullptr});
}

TriviaData *TokenStream::GetTriviaData(int pair_index) const {
    TriviaData *c = changes_[static_cast<size_t>(pair_index)];
    return c ? c : GetOriginalTriviaData(pair_index);
}

TriviaData *TokenStream::GetOriginalTriviaData(int pair_index) const {
    TriviaData *&slot = original_[static_cast<size_t>(pair_index)];
    if (!slot) {
        slot = engine_.factory->Create(tokens_[static_cast<size_t>(pair_index)],
                                       tokens_[static_cast<size_t>(pair_index + 1)]);
    }
    return slot;
}

bool TokenStream::IsFirstTokenOnLine(Token token) const {
    TokenData t = GetTokenData(token);
    TokenData prev = GetPreviousTokenData(t);
    return IsFirstTokenOnLine(prev, t);
}

bool TokenStream::IsFirstTokenOnLine(TokenData t1, TokenData t2) const {
    if (!t1.token) return true;
    return GetTriviaData(t1, t2)->SecondTokenIsFirstTokenOnLine();
}

// --- FormattingContext ---------------------------------------------------------------------------

namespace {

struct IndentationData {
    explicit IndentationData(TextSpan s) : span(s) {}
    virtual ~IndentationData() = default;
    TextSpan span;
    virtual int Indentation() const = 0;
};

struct SimpleIndentationData : IndentationData {
    SimpleIndentationData(TextSpan s, int ind) : IndentationData(s), indentation(ind) {}
    int indentation;
    int Indentation() const override { return indentation; }
};

struct AdjustedIndentationData : IndentationData {
    AdjustedIndentationData(TextSpan s, const IndentationData *b, int adj)
        : IndentationData(s), base(b), adjustment(adj) {}
    const IndentationData *base;
    int adjustment;
    int Indentation() const override { return base->Indentation() + adjustment; }
};

struct RelativeIndentationData : IndentationData {
    RelativeIndentationData(FormattingContext *ctx, int inseparable_start, TextSpan s, const IndentBlockOperation *op,
                            int lazy_delta = INT_MIN)
        : IndentationData(s), context(ctx), operation(op), lazy_delta(lazy_delta),
          inseparable(FromBounds(inseparable_start, s.end)) {}
    FormattingContext *context;
    const IndentBlockOperation *operation;
    mutable int lazy_delta;
    TextSpan inseparable;

    Token EffectiveBaseToken() const {
        if (operation->option & kRelativeToFirstTokenOnBaseTokenLine) {
            return context->Stream().FirstTokenOfBaseTokenLine(operation->base_token);
        }
        return operation->base_token;
    }
    int Delta() const {
        if (lazy_delta == INT_MIN) {
            Token anchor = EffectiveBaseToken();
            int d = operation->delta;
            if (!(operation->option & kAbsolutePosition) && (operation->option & kIndentIfConditionOfAnchorToken)) {
                const Node *cond = ConditionStatementForHeader(context->engine.tree, SpanStart(operation->base_token));
                if (cond && FirstToken(cond) == anchor) d += 1;
            }
            lazy_delta = d * context->Options().indentation_size;
        }
        return lazy_delta;
    }
    int Indentation() const override { return Delta() + context->Stream().GetCurrentColumn(EffectiveBaseToken()); }
};

struct SuppressData {
    TextSpan span;
};

struct AnchorData {
    AnchorIndentationOperation op;
    Token anchor_token;
    int original_column;
};

TextSpan IndentSpan(const IndentationData *d) { return d->span; }
TextSpan RelativeSpan(const RelativeIndentationData *d) { return d->inseparable; }
TextSpan SuppressSpan(const SuppressData *d) { return d->span; }
TextSpan AnchorSpan(const AnchorData *d) { return d->op.span; }

struct SpanKey {
    size_t operator()(TextSpan s) const {
        return std::hash<long long>()((static_cast<long long>(s.start) << 32) ^ static_cast<unsigned>(s.end));
    }
};
struct SpanEqual {
    bool operator()(TextSpan a, TextSpan b) const { return SpanEq(a, b); }
};
using SpanSet = std::unordered_set<TextSpan, SpanKey, SpanEqual>;

}  // namespace

struct FormattingContext::Impl {
    IntervalTree<const RelativeIndentationData *> relative{&RelativeSpan};
    IntervalTree<const IndentationData *> indentation{&IndentSpan};
    IntervalTree<const SuppressData *> suppress_wrapping{&SuppressSpan};
    IntervalTree<const SuppressData *> suppress_spacing{&SuppressSpan};
    IntervalTree<const SuppressData *> suppress_formatting{&SuppressSpan};
    IntervalTree<const AnchorData *> anchors{&AnchorSpan};
    std::unordered_map<Token, const AnchorData *> anchor_base_token_map;
    SpanSet indentation_map, suppress_wrapping_map, suppress_spacing_map, suppress_formatting_map, anchor_map;
    std::vector<std::unique_ptr<IndentationData>> indentation_pool;
    std::vector<std::unique_ptr<SuppressData>> suppress_pool;
    std::vector<std::unique_ptr<AnchorData>> anchor_pool;
    std::vector<std::unique_ptr<IndentBlockOperation>> op_pool;
    std::vector<const RelativeIndentationData *> relative_in_order;  // insertion order (stable by start below)
};

FormattingContext::FormattingContext(Engine &e) : engine(e), impl_(std::make_unique<Impl>()) {}
FormattingContext::~FormattingContext() = default;

void FormattingContext::InitializeWholeDocument() {
    auto data = std::make_unique<SimpleIndentationData>(FullSpanOf(engine.root), 0);
    impl_->indentation.AddIntervalInPlace(data.get());
    impl_->indentation_map.insert(data->span);
    impl_->indentation_pool.push_back(std::move(data));
}

void FormattingContext::AddIndentBlockOperations(std::vector<IndentBlockOperation> &ops) {
    DotNetSort(ops, [](const IndentBlockOperation &o1, const IndentBlockOperation &o2) {
        int d = o1.span.start - o2.span.start;
        if (d != 0) return d;
        d = o2.span.end - o1.span.end;
        if (d != 0) return d;
        if (o1.is_relative && o2.is_relative) {
            d = o2.delta - o1.delta;
            if (d != 0) return d;
        }
        return 0;
    });
    for (const IndentBlockOperation &o : ops) AddIndentBlockOperation(o);
}

void FormattingContext::AddIndentBlockOperation(const IndentBlockOperation &op_in) {
    TextSpan span = op_in.span;
    if (SpanIsEmpty(span) || impl_->indentation_map.count(span)) return;
    impl_->op_pool.push_back(std::make_unique<IndentBlockOperation>(op_in));
    const IndentBlockOperation *op = impl_->op_pool.back().get();

    if (op->is_relative) {
        Token effective = (op->option & kRelativeToFirstTokenOnBaseTokenLine)
                              ? Stream().FirstTokenOfBaseTokenLine(op->base_token)
                              : op->base_token;
        int inseparable_start = FullSpanOf(effective).start;
        auto data = std::make_unique<RelativeIndentationData>(this, inseparable_start, span, op);
        const RelativeIndentationData *raw = data.get();
        impl_->indentation.AddIntervalInPlace(raw);
        impl_->relative.AddIntervalInPlace(raw);
        impl_->relative_in_order.push_back(raw);
        impl_->indentation_map.insert(span);
        impl_->indentation_pool.push_back(std::move(data));
        return;
    }

    if (op->option & kAbsolutePosition) {
        auto data = std::make_unique<SimpleIndentationData>(span, op->delta);
        impl_->indentation.AddIntervalInPlace(data.get());
        impl_->indentation_map.insert(span);
        impl_->indentation_pool.push_back(std::move(data));
        return;
    }

    const IndentationData *indentation_data = impl_->indentation.GetSmallestContainingInterval(span.start, 0);
    if (!indentation_data) {
        auto data = std::make_unique<SimpleIndentationData>(span, op->delta * Options().indentation_size);
        impl_->indentation.AddIntervalInPlace(data.get());
        impl_->indentation_map.insert(span);
        impl_->indentation_pool.push_back(std::move(data));
        return;
    }

    const IndentationData *base_data = indentation_data;
    int total = 0;
    if (auto adjusted = dynamic_cast<const AdjustedIndentationData *>(indentation_data)) {
        base_data = adjusted->base;
        total = adjusted->adjustment;
    }
    total += op->delta * Options().indentation_size;

    std::unique_ptr<IndentationData> derived;
    if (total == 0) {
        if (SpanEq(base_data->span, span)) {
            // WithTextSpan returns the same object: it is added to the tree again.
            impl_->indentation.AddIntervalInPlace(base_data);
            impl_->indentation_map.insert(span);
            return;
        }
        if (auto simple = dynamic_cast<const SimpleIndentationData *>(base_data)) {
            derived = std::make_unique<SimpleIndentationData>(span, simple->indentation);
        } else if (auto rel = dynamic_cast<const RelativeIndentationData *>(base_data)) {
            derived = std::make_unique<RelativeIndentationData>(this, rel->inseparable.start, span, rel->operation,
                                                                rel->lazy_delta);
        } else if (auto adj = dynamic_cast<const AdjustedIndentationData *>(base_data)) {
            derived = std::make_unique<AdjustedIndentationData>(span, adj->base, adj->adjustment);
        }
    } else if (auto simple = dynamic_cast<const SimpleIndentationData *>(base_data)) {
        derived = std::make_unique<SimpleIndentationData>(span, simple->indentation + total);
    } else {
        derived = std::make_unique<AdjustedIndentationData>(span, base_data, total);
    }
    impl_->indentation.AddIntervalInPlace(derived.get());
    impl_->indentation_map.insert(span);
    impl_->indentation_pool.push_back(std::move(derived));
}

void FormattingContext::AddSuppressOperations(const std::vector<SuppressOperation> &ops) {
    for (const SuppressOperation &op : ops) {
        bool on_same_line = Stream().TwoTokensOriginallyOnSameLine(op.start_token, op.end_token);
        // AddSpacingSuppressOperation
        if (!SpanIsEmpty(op.span)) {
            int o = op.option;
            if ((o & kNoSpacing) && !impl_->suppress_spacing_map.count(op.span)) {
                if (((o & kNoSpacingIfOnSingleLine) && on_same_line) ||
                    ((o & kNoSpacingIfOnMultipleLine) && !on_same_line)) {
                    impl_->suppress_pool.push_back(std::make_unique<SuppressData>(SuppressData{op.span}));
                    impl_->suppress_spacing_map.insert(op.span);
                    impl_->suppress_spacing.AddIntervalInPlace(impl_->suppress_pool.back().get());
                }
            }
            // AddFormattingSuppressOperation
            if ((o & kDisableFormatting) && !impl_->suppress_formatting_map.count(op.span)) {
                impl_->suppress_pool.push_back(std::make_unique<SuppressData>(SuppressData{op.span}));
                impl_->suppress_formatting_map.insert(op.span);
                impl_->suppress_formatting.AddIntervalInPlace(impl_->suppress_pool.back().get());
            }
            // AddWrappingSuppressOperation
            if ((o & kNoWrapping) && !impl_->suppress_wrapping_map.count(op.span)) {
                if (((o & kNoWrappingIfOnSingleLine) && on_same_line) ||
                    ((o & kNoWrappingIfOnMultipleLine) && !on_same_line)) {
                    impl_->suppress_pool.push_back(std::make_unique<SuppressData>(SuppressData{op.span}));
                    impl_->suppress_wrapping_map.insert(op.span);
                    impl_->suppress_wrapping.AddIntervalInPlace(impl_->suppress_pool.back().get());
                }
            }
        }
    }
}

void FormattingContext::AddAnchorIndentationOperation(const AnchorIndentationOperation &op) {
    if (SpanIsEmpty(op.span) || impl_->anchor_map.count(op.span) || impl_->anchor_base_token_map.count(op.anchor_token)) {
        return;
    }
    Token anchor = Stream().FirstTokenOfBaseTokenLine(op.anchor_token);
    int original = Stream().GetOriginalColumn(anchor);
    impl_->anchor_pool.push_back(std::make_unique<AnchorData>(AnchorData{op, anchor, original}));
    const AnchorData *data = impl_->anchor_pool.back().get();
    impl_->anchors.AddIntervalInPlace(data);
    impl_->anchor_base_token_map[op.anchor_token] = data;
    impl_->anchor_map.insert(op.span);
}

int FormattingContext::GetBaseIndentation(int position) const {
    const IndentationData *d = impl_->indentation.GetSmallestContainingInterval(position, 0);
    return d ? d->Indentation() : 0;
}

std::vector<const IndentBlockOperation *> FormattingContext::GetAllRelativeIndentBlockOperations() const {
    std::vector<const IndentBlockOperation *> out;
    for (const RelativeIndentationData *d : impl_->relative.InOrder()) out.push_back(d->operation);
    return out;
}

int FormattingContext::GetAnchorDeltaFromOriginalColumn(Token token) const {
    const AnchorData *a = impl_->anchors.GetSmallestContainingInterval(SpanStart(token), 0);
    if (!a) return 0;
    return Stream().GetCurrentColumn(a->anchor_token) - a->original_column;
}

int FormattingContext::GetDeltaFromPreviousChangesMap(Token token, const std::unordered_map<Token, int> &map) const {
    auto it = map.find(token);
    if (it == map.end()) return 0;
    return Stream().GetCurrentColumn(token) - it->second;
}

bool FormattingContext::IsWrappingSuppressed(TextSpan span) const {
    if (IsFormattingDisabled(span)) return true;
    return impl_->suppress_wrapping.GetSmallestEdgeExclusivelyContainingInterval(span.start, span.Length()) != nullptr;
}

bool FormattingContext::IsSpacingSuppressed(TextSpan span) const {
    if (IsFormattingDisabled(span)) return true;
    return impl_->suppress_spacing.GetSmallestEdgeExclusivelyContainingInterval(span.start, span.Length()) != nullptr;
}

bool FormattingContext::IsSpacingSuppressed(int pair_index) const {
    Token t1 = Stream().GetToken(pair_index);
    Token t2 = Stream().GetToken(pair_index + 1);
    return IsSpacingSuppressed(FromBounds(SpanStart(t1), SpanOf(t2).end));
}

bool FormattingContext::IsFormattingDisabled(TextSpan span) const {
    return impl_->suppress_formatting.HasIntervalThatIntersectsWith(span.start, span.Length());
}

bool FormattingContext::IsFormattingDisabled(int pair_index) const {
    Token t1 = Stream().GetToken(pair_index);
    Token t2 = Stream().GetToken(pair_index + 1);
    return IsFormattingDisabled(FromBounds(SpanStart(t1), SpanOf(t2).end));
}

void FormattingContext::Apply(int pair_index, TriviaData *data) {
    if (pair_index == kApplyBeginningOfTree) {
        Stream().ApplyBeginningOfTreeChange(data);
    } else if (pair_index == kApplyEndOfTree) {
        Stream().ApplyEndOfTreeChange(data);
    } else {
        Stream().ApplyChange(pair_index, data);
    }
}

// --- Engine --------------------------------------------------------------------------------------

Engine::Engine(const Tree &t, const Rules &r, const LineOptions &o, const Node *rt, bool s, int ic)
    : tree(t), rules(r), options(o), root(rt), structured(s), initial_column(ic) {
    first_token = FirstToken(root, true);
    last_token = LastToken(root, true);
    start_position = FullSpanOf(root).start;
    end_position = FullSpanOf(root).end;
}

Engine::~Engine() = default;

U16View Engine::GetTextBetween(Token t1, Token t2) const {
    if (!t1) return tree.TextOf(FromBounds(FullSpanOf(t2).start, SpanStart(t2)));
    if (!t2) return tree.TextOf(FromBounds(SpanOf(t1).end, FullSpanOf(t1).end));
    return tree.TextOf(FromBounds(SpanOf(t1).end, SpanStart(t2)));
}

int Engine::GetOriginalColumn(Token t) const {
    const U16 &text = tree.Text();
    int pos = SpanStart(t);
    int line_start = pos;
    while (line_start > 0) {
        char16_t c = text[static_cast<size_t>(line_start - 1)];
        // SourceText lines end at \r, \n, \r\n, U+0085, U+2028, U+2029.
        if (c == u'\n' || c == u'\r' || c == 0x85 || c == 0x2028 || c == 0x2029) break;
        line_start--;
    }
    return GetColumnFromLineOffset(U16View(text).substr(static_cast<size_t>(line_start)), pos - line_start,
                                   options.tab_size);
}

namespace {

void DescendantNodesAndSelf(const Node *n, std::vector<const Node *> &out) {
    if (n->IsToken()) return;
    out.push_back(n);
    for (const Node *c : n->children) DescendantNodesAndSelf(c, out);
}

}  // namespace

void Engine::Format() {
    // Node operations (CreateNodeOperations): none for structured trivia.
    std::vector<IndentBlockOperation> indent_ops;
    std::vector<SuppressOperation> suppress_ops;
    std::vector<AnchorIndentationOperation> anchor_ops;
    if (!structured) {
        std::vector<const Node *> nodes;
        DescendantNodesAndSelf(root, nodes);
        for (const Node *n : nodes) {
            rules.AddIndentBlockOperations(indent_ops, n);
            rules.AddSuppressOperations(suppress_ops, n);
            rules.AddAnchorIndentationOperations(anchor_ops, n);
        }
    }

    std::vector<Token> tokens;
    if (structured) {
        CollectTokens(root, tokens);
    } else {
        tokens = tree.Tokens();
    }
    factory_owner_ = std::make_unique<TriviaFactory>(*this);
    factory = factory_owner_.get();
    stream_owner_ = std::make_unique<TokenStream>(*this, std::move(tokens));
    stream = stream_owner_.get();

    // Token operations (AddTokenOperations).
    struct PairOps {
        SpacesOp space;
        NewLinesOp line;
    };
    std::vector<PairOps> token_ops(static_cast<size_t>(std::max(0, stream->TokenCount() - 1)));
    for (int i = 0; i + 1 < stream->TokenCount(); i++) {
        Token cur = stream->GetToken(i);
        Token next = stream->GetToken(i + 1);
        token_ops[static_cast<size_t>(i)].space = rules.GetAdjustSpacesOperation(cur, next);
        token_ops[static_cast<size_t>(i)].line = rules.GetAdjustNewLinesOperation(cur, next);
    }

    context_owner_ = std::make_unique<FormattingContext>(*this);
    context = context_owner_.get();
    if (!structured && stream->IsFormattingWholeDocument()) context->InitializeWholeDocument();

    for (const AnchorIndentationOperation &a : anchor_ops) context->AddAnchorIndentationOperation(a);

    context->AddIndentBlockOperations(indent_ops);
    context->AddSuppressOperations(suppress_ops);

    // ApplyBeginningOfTreeTriviaOperation.
    if (stream->FormatBeginningOfTree()) {
        TriviaData *info = stream->GetTriviaDataAtBeginningOfTree()->WithIndentation(0, *context);
        info->Format(*context, kApplyBeginningOfTree);
    }

    // ApplyTokenOperations: space and wrapping.
    for (int i = 0; i + 1 < stream->TokenCount(); i++) {
        Token t1 = stream->GetToken(i);
        Token t2 = stream->GetToken(i + 1);
        if (IsMissing(t1) || IsMissing(t2)) continue;
        const PairOps &ops = token_ops[static_cast<size_t>(i)];
        TriviaData *info = stream->GetTriviaData(i);
        TextSpan between = FromBounds(SpanOf(t1).end, SpanStart(t2));
        if (ops.line.valid && !context->IsWrappingSuppressed(between)) {
            bool applied = false;
            int indentation = context->GetBaseIndentation(t2);
            switch (ops.line.option) {
            case NewLinesOption::PreserveLines:
                if (ops.line.line > info->line_breaks) {
                    stream->ApplyChange(i, info->WithLine(ops.line.line, indentation, *context));
                    applied = true;
                } else if (info->SecondTokenIsFirstTokenOnLine() && indentation != info->spaces) {
                    stream->ApplyChange(i, info->WithIndentation(indentation, *context));
                    applied = true;
                } else {
                    applied = ops.line.line > 0;
                }
                break;
            case NewLinesOption::ForceLines:
            case NewLinesOption::ForceLinesIfOnSingleLine:
                if (ops.line.option == NewLinesOption::ForceLinesIfOnSingleLine &&
                    !stream->TwoTokensOnSameLine(t1, t2)) {
                    applied = false;
                    break;
                }
                if (info->line_breaks == ops.line.line && info->spaces == indentation) {
                    applied = true;
                } else {
                    stream->ApplyChange(i, info->WithLine(ops.line.line, indentation, *context));
                    applied = true;
                }
                break;
            }
            if (applied) continue;
        }
        if (ops.space.valid && !context->IsSpacingSuppressed(between)) {
            info = stream->GetTriviaData(i);
            int space = ops.space.space;
            switch (ops.space.option) {
            case SpacesOption::PreserveSpaces:
                if (!info->SecondTokenIsFirstTokenOnLine() && space > info->spaces) {
                    stream->ApplyChange(i, info->WithSpace(space, *context));
                }
                break;
            case SpacesOption::ForceSpaces:
                if (!(info->line_breaks == 0 && info->spaces == space)) {
                    stream->ApplyChange(i, info->WithSpace(space, *context));
                }
                break;
            case SpacesOption::DynamicSpaceToIndentationIfOnSingleLine: {
                if (info->SecondTokenIsFirstTokenOnLine()) break;
                int indentation = context->GetBaseIndentation(t2);
                int len = 0;
                bool multi = false;
                stream->GetTokenLength(t1, &len, &multi);
                int end_col = multi ? len : stream->GetCurrentColumn(t1) + len;
                if (end_col < indentation) {
                    stream->ApplyChange(i, info->WithSpace(indentation - end_col, *context));
                    break;
                }
                if (info->spaces != space) stream->ApplyChange(i, info->WithSpace(space, *context));
                break;
            }
            default:
                if (!info->SecondTokenIsFirstTokenOnLine() && info->spaces != space) {
                    stream->ApplyChange(i, info->WithSpace(space, *context));
                }
                break;
            }
        }
    }

    // ApplyAnchorOperations.
    {
        std::unordered_map<Token, int> previous_changes;
        for (int i = 0; i + 1 < stream->TokenCount(); i++) {
            const PairOps &ops = token_ops[static_cast<size_t>(i)];
            bool candidate = false;
            if (!ops.line.valid) {
                candidate = stream->GetTriviaData(i)->SecondTokenIsFirstTokenOnLine();
            } else if (ops.line.option == NewLinesOption::ForceLinesIfOnSingleLine) {
                candidate = !stream->TwoTokensOriginallyOnSameLine(stream->GetToken(i), stream->GetToken(i + 1)) &&
                            stream->GetTriviaData(i)->SecondTokenIsFirstTokenOnLine();
            }
            if (!candidate) continue;
            // ApplyAnchorIndentation.
            TriviaData *info = stream->GetTriviaData(i);
            if (!info->SecondTokenIsFirstTokenOnLine()) continue;
            if (context->IsSpacingSuppressed(i)) continue;
            Token first_on_line = stream->GetToken(i + 1);
            int indentation = info->spaces + context->GetAnchorDeltaFromOriginalColumn(first_on_line);
            if (info->spaces != indentation) {
                previous_changes.emplace(first_on_line, info->spaces);
                stream->ApplyChange(i, info->WithIndentation(indentation, *context));
            }
        }
        // Relative indent blocks affected by the anchor moves (ApplyBaseTokenIndentationChangesFromTo).
        for (const IndentBlockOperation *o : context->GetAllRelativeIndentBlockOperations()) {
            Token base = (o->option & kRelativeToFirstTokenOnBaseTokenLine)
                             ? stream->FirstTokenOfBaseTokenLine(o->base_token)
                             : o->base_token;
            TokenData base_data = stream->GetTokenData(base);
            TokenData start_data = stream->GetPreviousTokenData(stream->GetTokenData(o->start_token));
            TokenData end_data = stream->GetTokenData(o->end_token);
            if (base_data.index < 0) continue;
            TokenData t = base_data;
            bool found = false;
            while (t.index >= 0) {
                if (previous_changes.count(t.token)) {
                    found = true;
                    break;
                }
                int pi = t.index - 1;
                if (pi < 0 || stream->GetTriviaData(pi)->SecondTokenIsFirstTokenOnLine()) break;
                t = stream->GetPreviousTokenData(t);
            }
            if (!found || t.index < 0) continue;
            int delta = context->GetDeltaFromPreviousChangesMap(t.token, previous_changes);
            if (delta == 0) continue;
            if (start_data.index < 0) start_data = stream->FirstTokenInStream();
            if (end_data.index < 0) end_data = stream->LastTokenInStream();
            for (int pi = start_data.index; pi < end_data.index; pi++) {
                TriviaData *info = stream->GetTriviaData(pi);
                if (!info->SecondTokenIsFirstTokenOnLine()) continue;
                if (context->IsSpacingSuppressed(pi)) continue;
                Token cur = stream->GetToken(pi + 1);
                if (previous_changes.count(cur)) continue;
                int indentation = info->spaces + delta;
                if (info->spaces == indentation) continue;
                previous_changes.emplace(cur, info->spaces);
                stream->ApplyChange(pi, info->WithIndentation(indentation, *context));
            }
        }
    }
    // ApplySpecialOperations: no C# rule adds alignment operations, and with no alignment the relative
    // indent blocks have nothing to follow.

    // ApplyTriviaOperations.
    for (int i = 0; i + 1 < stream->TokenCount(); i++) {
        if (context->IsFormattingDisabled(i)) continue;
        stream->GetTriviaData(i)->Format(*context, i);
    }

    // ApplyEndOfTreeTriviaOperation.
    if (stream->FormatEndOfTree() &&
        !context->IsFormattingDisabled(TextSpan{SpanStart(stream->LastTokenInStream().token),
                                                SpanStart(stream->LastTokenInStream().token)})) {
        TriviaData *info = stream->GetTriviaDataAtEndOfTree()->WithIndentation(0, *context);
        info->Format(*context, kApplyEndOfTree);
    }
}

bool Engine::HasChanges() const {
    if (stream->FormatBeginningOfTree() && stream->GetTriviaDataAtBeginningOfTree()->ContainsChanges()) return true;
    for (int i = 0; i + 1 < stream->TokenCount(); i++) {
        if (stream->GetTriviaData(i)->ContainsChanges()) return true;
    }
    if (stream->FormatEndOfTree() && stream->GetTriviaDataAtEndOfTree()->ContainsChanges()) return true;
    return false;
}

U16 Engine::Result() const {
    // FormattingResult.GetFormattedRoot through the TriviaRewriter: every changed pair's text replaces the
    // text between its two tokens.
    U16 out;
    int pos = start_position;
    auto emit = [&](int from, int to, TriviaData *data) {
        if (from < pos) return;
        out.append(tree.TextOf(FromBounds(pos, from)));
        out.append(data->NewText());
        pos = to;
    };
    if (stream->FormatBeginningOfTree()) {
        TriviaData *d = stream->GetTriviaDataAtBeginningOfTree();
        if (d->ContainsChanges()) emit(start_position, SpanStart(stream->GetToken(0)), d);
    }
    for (int i = 0; i + 1 < stream->TokenCount(); i++) {
        TriviaData *d = stream->GetTriviaData(i);
        if (!d->ContainsChanges()) continue;
        emit(SpanOf(stream->GetToken(i)).end, SpanStart(stream->GetToken(i + 1)), d);
    }
    if (stream->FormatEndOfTree()) {
        TriviaData *d = stream->GetTriviaDataAtEndOfTree();
        if (d->ContainsChanges()) emit(SpanOf(stream->LastTokenInStream().token).end, end_position, d);
    }
    out.append(tree.TextOf(FromBounds(pos, end_position)));
    return out;
}

U16 FormatTree(const Tree &tree, const Rules &rules, const LineOptions &options) {
    if (FullSpanOf(tree.Root()).Length() == 0) return U16();
    Engine engine(tree, rules, options, tree.Root(), false, 0);
    engine.Format();
    return engine.Result();
}

U16 FormatStructuredTrivia(const Tree &tree, const Rules &rules, const LineOptions &options, Trivia trivia,
                           int initial_column, bool *changed) {
    Engine engine(tree, rules, options, trivia->structure, true, initial_column);
    engine.Format();
    *changed = engine.HasChanges();
    return engine.Result();
}

}  // namespace csfmt
