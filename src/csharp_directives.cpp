// Directives.cs (Roslyn, MIT, (c) .NET Foundation and Contributors): Directive and DirectiveStack, plus
// the ApplyDirectives walk of CSharpSyntaxNode / SyntaxToken / DirectiveTriviaSyntax.
//
// Roslyn's DirectiveStack is a struct wrapping a nullable ConsList<Directive>; ConsList<T>.Empty is a single
// sentinel cell whose Any() is false. Here the list is a chain of immutable `Cons` cells held by shared_ptr,
// ending in the one shared sentinel (EmptyCons()); a null shared_ptr is the C# null list. As in Roslyn, a
// default DirectiveStack is null (IsNull), DirectiveStack::Empty() holds the sentinel (IsEmpty), and Add on a
// null stack starts from the sentinel (`_directives ?? ConsList<Directive>.Empty`).
//
// Not ported: IncrementallyEquivalent, SkipInsignificantDirectives (incremental parsing only),
// InterlockedInitialize, GetDebuggerDisplay.

#include "csharp_parser_internal.h"

namespace csharp {

// --- Directive -------------------------------------------------------------------

std::u16string Directive::GetIdentifier() const {
    switch (node->kind) {
        case SyntaxKind::DefineDirectiveTrivia:
        case SyntaxKind::UndefDirectiveTrivia: {
            // ((DefineDirectiveTriviaSyntax)_node).Name.ValueText
            GreenNode *name = GetField(node, Field::Name);
            return name ? std::u16string(name->value_text) : std::u16string();
        }
        default:
            return std::u16string();  // C# null
    }
}

bool Directive::HasIdentifier() const {
    return node->kind == SyntaxKind::DefineDirectiveTrivia || node->kind == SyntaxKind::UndefDirectiveTrivia;
}

bool Directive::IsActive() const { return csharp::IsActive(node); }

bool Directive::BranchTaken() const {
    // _node as BranchingDirectiveTriviaSyntax
    switch (node->kind) {
        case SyntaxKind::IfDirectiveTrivia:
        case SyntaxKind::ElifDirectiveTrivia:
        case SyntaxKind::ElseDirectiveTrivia:
            return csharp::BranchTaken(node);
        default:
            return false;
    }
}

// --- DirectiveStack ----------------------------------------------------------------

/** @brief A ConsList<Directive> cell. Immutable once built; the sentinel EmptyCons() ends every chain. */
struct DirectiveStack::Cons {
    Directive head;
    // `mutable` only so the destructor can unlink a long chain iteratively (a recursive shared_ptr release of
    // a stack of many thousand directives could overflow the C++ stack).
    mutable std::shared_ptr<const Cons> tail;

    Cons() = default;
    Cons(Directive h, std::shared_ptr<const Cons> t) : head(h), tail(std::move(t)) {}
    Cons(const Cons &) = delete;
    Cons &operator=(const Cons &) = delete;
    ~Cons() {
        std::shared_ptr<const Cons> next = std::move(tail);
        while (next && next.use_count() == 1) {
            std::shared_ptr<const Cons> after = std::move(next->tail);
            next = std::move(after);
        }
    }
};

namespace {

using ConsPtr = std::shared_ptr<const DirectiveStack::Cons>;

/** @brief ConsList<Directive>.Empty: the one sentinel. */
const ConsPtr &EmptyCons() {
    static const ConsPtr empty = std::make_shared<const DirectiveStack::Cons>();
    return empty;
}

/** @brief ConsList.Any(): false for the sentinel. (Callers check for null first, as the C# does.) */
bool Any(const ConsPtr &list) { return list.get() != EmptyCons().get(); }

ConsPtr MakeCons(Directive head, ConsPtr tail) {
    return std::make_shared<const DirectiveStack::Cons>(head, std::move(tail));
}

// removes unfinished if & related directives from stack and leaves active branch directives
ConsPtr CompleteIf(const ConsPtr &stack, bool &include) {
    // if we get to the top, the default rule is to include anything that follows
    if (!Any(stack)) {
        include = true;
        return stack;
    }

    // if we reach the #if directive, then we stop unwinding and start
    // rebuilding the stack w/o the #if/#elif/#else/#endif directives
    // only including content from sections that are considered included
    if (stack->head.Kind() == SyntaxKind::IfDirectiveTrivia) {
        include = stack->head.BranchTaken();
        return stack->tail;
    }

    ConsPtr newStack = CompleteIf(stack->tail, include);
    switch (stack->head.Kind()) {
        case SyntaxKind::ElifDirectiveTrivia:
        case SyntaxKind::ElseDirectiveTrivia:
            include = stack->head.BranchTaken();
            break;
        default:
            if (include) {
                newStack = MakeCons(stack->head, newStack);
            }
            break;
    }

    return newStack;
}

// removes region directives from stack but leaves everything else
ConsPtr CompleteRegion(const ConsPtr &stack) {
    // if we get to the top, the default rule is to include anything that follows
    if (!Any(stack)) {
        return stack;
    }

    if (stack->head.Kind() == SyntaxKind::RegionDirectiveTrivia) {
        return stack->tail;
    }

    ConsPtr newStack = CompleteRegion(stack->tail);
    newStack = MakeCons(stack->head, newStack);
    return newStack;
}

const DirectiveStack::Cons *GetPreviousIf(const DirectiveStack::Cons *current) {
    while (current != nullptr && current != EmptyCons().get()) {
        switch (current->head.Kind()) {
            case SyntaxKind::IfDirectiveTrivia:
                return current;
            default:
                break;
        }

        current = current->tail.get();
    }

    return current;
}

const DirectiveStack::Cons *GetPreviousIfElifElseOrRegion(const DirectiveStack::Cons *current) {
    while (current != nullptr && current != EmptyCons().get()) {
        switch (current->head.Kind()) {
            case SyntaxKind::IfDirectiveTrivia:
            case SyntaxKind::ElifDirectiveTrivia:
            case SyntaxKind::ElseDirectiveTrivia:
            case SyntaxKind::RegionDirectiveTrivia:
                return current;
            default:
                break;
        }

        current = current->tail.get();
    }

    return current;
}

const DirectiveStack::Cons *GetPreviousRegion(const DirectiveStack::Cons *current) {
    while (current != nullptr && current != EmptyCons().get() &&
           current->head.Kind() != SyntaxKind::RegionDirectiveTrivia) {
        current = current->tail.get();
    }

    return current;
}

/** @brief `prev != null && prev.Any()` on a raw cell pointer. */
bool NotNullAndAny(const DirectiveStack::Cons *c) { return c != nullptr && c != EmptyCons().get(); }

}  // namespace

DirectiveStack DirectiveStack::Empty() { return DirectiveStack(EmptyCons(), false); }

bool DirectiveStack::IsEmpty() const { return list_.get() == EmptyCons().get(); }

DefineState DirectiveStack::IsDefined(Text id) const {
    for (const Cons *current = list_.get(); NotNullAndAny(current); current = current->tail.get()) {
        switch (current->head.Kind()) {
            case SyntaxKind::DefineDirectiveTrivia:
                if (current->head.GetIdentifier() == id) {
                    return DefineState::Defined;
                }

                break;
            case SyntaxKind::UndefDirectiveTrivia:
                if (current->head.GetIdentifier() == id) {
                    return DefineState::Undefined;
                }

                break;

            case SyntaxKind::ElifDirectiveTrivia:
            case SyntaxKind::ElseDirectiveTrivia:
                // Skip directives from previous branches of the same #if.
                do {
                    current = current->tail.get();

                    if (!NotNullAndAny(current)) {
                        return DefineState::Unspecified;
                    }
                } while (current->head.Kind() != SyntaxKind::IfDirectiveTrivia);

                break;
            default:
                break;
        }
    }

    return DefineState::Unspecified;
}

// true if any previous section of the closest #if has its branch taken
bool DirectiveStack::PreviousBranchTaken() const {
    for (const Cons *current = list_.get(); NotNullAndAny(current); current = current->tail.get()) {
        if (current->head.BranchTaken()) {
            return true;
        } else if (current->head.Kind() == SyntaxKind::IfDirectiveTrivia) {
            return false;
        }
    }

    return false;
}

bool DirectiveStack::HasUnfinishedIf() const {
    const Cons *prev = GetPreviousIfElifElseOrRegion(list_.get());
    return NotNullAndAny(prev) && prev->head.Kind() != SyntaxKind::RegionDirectiveTrivia;
}

bool DirectiveStack::HasPreviousIfOrElif() const {
    const Cons *prev = GetPreviousIfElifElseOrRegion(list_.get());
    return NotNullAndAny(prev) &&
           (prev->head.Kind() == SyntaxKind::IfDirectiveTrivia || prev->head.Kind() == SyntaxKind::ElifDirectiveTrivia);
}

bool DirectiveStack::HasUnfinishedRegion() const {
    const Cons *prev = GetPreviousIfElifElseOrRegion(list_.get());
    return NotNullAndAny(prev) && prev->head.Kind() == SyntaxKind::RegionDirectiveTrivia;
}

DirectiveStack DirectiveStack::Add(Directive directive) const {
    switch (directive.Kind()) {
        case SyntaxKind::EndIfDirectiveTrivia: {
            const Cons *prevIf = GetPreviousIf(list_.get());
            if (!NotNullAndAny(prevIf)) {
                goto default_;  // no matching if directive !! leave directive alone
            }

            bool include = false;
            return DirectiveStack(CompleteIf(list_, include), seen_any_if_);
        }
        case SyntaxKind::EndRegionDirectiveTrivia: {
            const Cons *prevRegion = GetPreviousRegion(list_.get());
            if (!NotNullAndAny(prevRegion)) {
                goto default_;  // no matching region directive !! leave directive alone
            }

            // remove region directives from stack but leave everything else
            return DirectiveStack(CompleteRegion(list_), seen_any_if_);
        }
        default:
            break;
    }
default_:
    return DirectiveStack(MakeCons(directive, list_ ? list_ : EmptyCons()),
                          seen_any_if_ || directive.Kind() == SyntaxKind::IfDirectiveTrivia);
}

// --- ApplyDirectives (CSharpSyntaxNode / SyntaxToken / DirectiveTriviaSyntax) -----------------------------

namespace {

DirectiveStack ApplyDirectivesToListOrNode(const GreenNode *listOrNode, DirectiveStack stack);

// CSharpSyntaxNode.ApplyDirectives(GreenNode node, DirectiveStack stack): also used for lists.
DirectiveStack ApplyDirectivesToSlots(const GreenNode *node, DirectiveStack stack) {
    if (node->ContainsDirectives()) {
        for (int i = 0, n = node->SlotCount(); i < n; i++) {
            GreenNode *child = node->GetSlot(i);
            if (child != nullptr) {
                stack = ApplyDirectivesToListOrNode(child, std::move(stack));
            }
        }
    }

    return stack;
}

// SyntaxToken.ApplyDirectivesToTrivia
DirectiveStack ApplyDirectivesToTrivia(const GreenNode *triviaList, DirectiveStack stack) {
    if (triviaList != nullptr && triviaList->ContainsDirectives()) {
        return ApplyDirectivesToListOrNode(triviaList, std::move(stack));
    }

    return stack;
}

// The virtual CSharpSyntaxNode.ApplyDirectives(stack), dispatched on what the node is.
DirectiveStack ApplyDirectivesVirtual(const GreenNode *node, DirectiveStack stack) {
    if (node->IsToken()) {
        // SyntaxToken.ApplyDirectives
        if (node->ContainsDirectives()) {
            stack = ApplyDirectivesToTrivia(node->GetLeadingTrivia(), std::move(stack));
            stack = ApplyDirectivesToTrivia(node->GetTrailingTrivia(), std::move(stack));
        }

        return stack;
    }
    if (node->IsDirective()) {
        // DirectiveTriviaSyntax.ApplyDirectives
        return stack.Add(Directive(const_cast<GreenNode *>(node)));
    }
    // CSharpSyntaxNode.ApplyDirectives (plain trivia has no slots and never contains directives)
    return ApplyDirectivesToSlots(node, std::move(stack));
}

// CSharpSyntaxNode.ApplyDirectivesToListOrNode
DirectiveStack ApplyDirectivesToListOrNode(const GreenNode *listOrNode, DirectiveStack stack) {
    if (listOrNode->IsList()) {
        return ApplyDirectivesToSlots(listOrNode, std::move(stack));
    } else {
        return ApplyDirectivesVirtual(listOrNode, std::move(stack));
    }
}

}  // namespace

DirectiveStack ApplyDirectives(const GreenNode *node, DirectiveStack stack) {
    return ApplyDirectivesToListOrNode(node, std::move(stack));
}

}  // namespace csharp
