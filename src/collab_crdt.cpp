#include "collab_crdt.h"

#include "json.h"

#include <algorithm>
#include <random>
#include <utility>

namespace mep::collab {
namespace {
/**
 * @brief Converts a CrdtId to its JSON representation.
 * @param id the id to serialize
 * @return a JSON object with "actor" and "counter" fields
 */
Json IdToJson(const CrdtId &id) { Json out = Json::Object(); out["actor"] = id.actor; out["counter"] = static_cast<long long>(id.counter); return out; }
/**
 * @brief Parses a CrdtId from JSON, rejecting missing actors or non-integer/negative counters.
 * @param json the JSON value to parse
 * @param id output id populated on success
 * @return true if `json` held a valid actor/counter pair
 */
bool IdFromJson(const Json &json, CrdtId *id) {
    if (!json.is_object()) return false;
    const std::string actor = json.get("actor").as_string();
    const double counter = json.get("counter").as_double(-1);
    // An empty actor is only valid for Root() ({"", 0}) -- the sentinel
    // used as an insert's `after` for the very first character in any
    // causal chain, and thus always present in a real snapshot's "after"
    // fields. Rejecting it unconditionally (as this used to) made every
    // snapshot containing that first character fail to round-trip through
    // Restore(): CrdtOperationFromJson would refuse to parse its `after`
    // field, well before it's caught by any error assert -- assert() is
    // compiled out under this project's default Release build's NDEBUG,
    // which is exactly why collab_crdt_test.cpp's Restore() coverage never
    // caught it (see that file's own CHECK() fix for the same reason).
    // A non-Root id must still have a non-empty actor and counter >= 1.
    const bool is_root = actor.empty() && counter == 0;
    if ((actor.empty() && !is_root) || counter < 0 || counter != static_cast<double>(static_cast<uint64_t>(counter))) return false;
    *id = {actor, static_cast<uint64_t>(counter)}; return true;
}
/**
 * @brief Generates a random actor id of the form "mep-" followed by 16 random alphanumeric characters.
 * @return a newly generated actor id
 */
std::string DefaultActor() {
    static constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::random_device rd; std::mt19937_64 rng(rd());
    std::string actor = "mep-";
    for (int i = 0; i < 16; ++i) actor.push_back(alphabet[rng() % (sizeof(alphabet) - 1)]);
    return actor;
}
}  // namespace

Json CrdtOperationToJson(const CrdtOperation &op) {
    Json out = Json::Object(); out["kind"] = op.kind == CrdtOperation::Kind::Insert ? "insert" : "delete"; out["id"] = IdToJson(op.id);
    if (op.kind == CrdtOperation::Kind::Insert) { out["after"] = IdToJson(op.after); out["value"] = static_cast<int>(op.value); }
    return out;
}
bool CrdtOperationFromJson(const Json &json, CrdtOperation *op) {
    if (!json.is_object() || !IdFromJson(json.get("id"), &op->id)) return false;
    const std::string kind = json.get("kind").as_string();
    if (kind == "delete") { op->kind = CrdtOperation::Kind::Delete; return true; }
    if (kind != "insert" || !IdFromJson(json.get("after"), &op->after)) return false;
    const int value = json.get("value").as_int(-1); if (value < 0 || value > 255) return false;
    op->kind = CrdtOperation::Kind::Insert; op->value = static_cast<unsigned char>(value); return true;
}

// ---------------------------------------------------------------------
// CRDT_PERFORMANCE_PLAN.md Phase 2: the document's visible+tombstoned
// sequence lives in one balanced, position-ordered *implicit treap*
// (random priorities, standard split/merge-by-size primitives -- the
// same family of data structure as a rope, just keyed by position
// instead of by contiguous string chunks). This replaced the original
// std::map<CrdtId,Node> + a `children_` adjacency map walked via
// recursive DFS for every single Insert()/Erase() call
// (VisibleIds() used to be O(visible size) -- and, worse, that DFS
// recursed one call frame per character for a straight run of
// sequential inserts, so it didn't just get slow at realistic document
// sizes, it crashed with a stack overflow well under 100k characters,
// found live via mep-crdt-bench).
//
// `children_` itself is UNCHANGED from the original design: it still
// answers "which existing child of X does a new concurrent sibling
// insert before/after" (sorted by CrdtId, exactly as before) -- that
// decision is local to however many concurrent edits happened at the
// very same anchor, which is bounded by actual concurrency, not
// document size, so keeping it as a small std::map entry per anchor is
// still the right structure for it. What changed is how a decision
// made via `children_` gets turned into an actual *position* in the
// document:
//
//   - The FAST, overwhelmingly common path (sequential typing, or any
//     insert whose anchor has no existing children yet -- true for
//     every keystroke with no concurrent edit at that exact spot) is
//     `PositionOf(after) + 1`: one O(log N) treap rank lookup, no scan
//     of any kind.
//   - The SLOW path (the new character has to slot in among existing
//     concurrent siblings of the same anchor) needs "the position right
//     after the previous sibling's own entire subtree" --
//     `EndOfSubtree()` -- which walks forward via in-order successors,
//     each O(log N), for as many steps as that one sibling's own
//     subtree is deep. This is bounded by the size of *that* concurrent
//     edit cluster, not by the document as a whole, so it stays cheap
//     for realistic collaboration (a handful of people editing near
//     each other), even though it isn't a strict worst-case O(log N)
//     guarantee the way a from-scratch algorithm like Fugue's would be.
//     That tradeoff -- much simpler to implement and verify correct,
//     in exchange for a bound on concurrent-cluster size rather than a
//     universal one -- is deliberate; see the plan doc's own Non-goals.
//
// Every node still carries a `depth` (its distance from Root() in the
// *semantic* children_ tree, not the treap's own balanced shape) --
// EndOfSubtree uses it to recognize where a subtree ends (the first
// successor whose depth drops back to the anchor's own level).
//
// Tombstones behave exactly as before: marked, never physically
// removed, still counted in `total_size` (the treap's structural size)
// but excluded from `visible_size` (a second, separately-maintained
// count per subtree) -- so VisibleSize() is now an O(1) root lookup
// instead of an O(N) walk, and deleting a node is an O(log N) walk up
// through its ancestors decrementing `visible_size`, not a full
// recompute.

CrdtId TextCrdt::Root() { return {"", 0}; }

void TextCrdt::Update(Node *n) {
    const int lt = n->left ? n->left->total_size : 0;
    const int rt = n->right ? n->right->total_size : 0;
    const int lv = n->left ? n->left->visible_size : 0;
    const int rv = n->right ? n->right->visible_size : 0;
    n->total_size = lt + rt + 1;
    n->visible_size = lv + rv + (n->deleted ? 0 : 1);
}

TextCrdt::Node *TextCrdt::Merge(Node *a, Node *b) {
    if (!a) return b;
    if (!b) return a;
    if (a->priority > b->priority) {
        Node *r = Merge(a->right, b);
        a->right = r;
        if (r) r->parent = a;
        Update(a);
        return a;
    }
    Node *l = Merge(a, b->left);
    b->left = l;
    if (l) l->parent = b;
    Update(b);
    return b;
}

void TextCrdt::Split(Node *n, int pos, Node **out_left, Node **out_right) {
    if (!n) { *out_left = nullptr; *out_right = nullptr; return; }
    const int left_size = n->left ? n->left->total_size : 0;
    if (pos <= left_size) {
        Node *ll = nullptr, *lr = nullptr;
        Split(n->left, pos, &ll, &lr);
        n->left = lr;
        if (lr) lr->parent = n;
        Update(n);
        *out_left = ll;
        if (ll) ll->parent = nullptr;
        *out_right = n;
    } else {
        Node *rl = nullptr, *rr = nullptr;
        Split(n->right, pos - left_size - 1, &rl, &rr);
        n->right = rl;
        if (rl) rl->parent = n;
        Update(n);
        *out_left = n;
        *out_right = rr;
        if (rr) rr->parent = nullptr;
    }
    n->parent = nullptr;
}

int TextCrdt::PositionOf(const Node *n) const {
    int pos = n->left ? n->left->total_size : 0;
    const Node *cur = n;
    while (cur->parent) {
        const Node *p = cur->parent;
        if (p->right == cur) pos += (p->left ? p->left->total_size : 0) + 1;
        cur = p;
    }
    return pos;
}

TextCrdt::Node *TextCrdt::SelectVisible(int k) const {
    Node *cur = root_;
    while (cur) {
        const int left_visible = cur->left ? cur->left->visible_size : 0;
        if (k < left_visible) { cur = cur->left; continue; }
        k -= left_visible;
        if (!cur->deleted) {
            if (k == 0) return cur;
            k -= 1;
        }
        cur = cur->right;
    }
    return nullptr;
}

TextCrdt::Node *TextCrdt::Successor(Node *n) {
    if (n->right) {
        n = n->right;
        while (n->left) n = n->left;
        return n;
    }
    while (n->parent && n->parent->right == n) n = n->parent;
    return n->parent;
}

int TextCrdt::EndOfSubtree(Node *n) const {
    const int base_depth = n->depth;
    Node *cur = Successor(n);
    while (cur && cur->depth > base_depth) cur = Successor(cur);
    return cur ? PositionOf(cur) : (root_ ? root_->total_size : 0);
}

void TextCrdt::InsertAt(int pos, Node *n) {
    Node *left = nullptr, *right = nullptr;
    Split(root_, pos, &left, &right);
    root_ = Merge(Merge(left, n), right);
    root_->parent = nullptr;
}

void TextCrdt::MarkDeleted(Node *n) {
    if (n->deleted) return;
    n->deleted = true;
    for (Node *cur = n; cur; cur = cur->parent) cur->visible_size -= 1;
}

void TextCrdt::DeleteSubtree(Node *n) {
    if (!n) return;
    DeleteSubtree(n->left);
    DeleteSubtree(n->right);
    delete n;
}

// ---------------------------------------------------------------------

// Seeded from the actor id's own hash, not std::random_device -- a
// deliberate choice, not a corner cut: it makes every replica's treap
// priority sequence fully reproducible from its (already-unique) actor
// id, which is what let a flaky-looking convergence failure in
// CRDT_PERFORMANCE_PLAN.md Phase 2's randomized stress testing turn
// into a reliably reproducible one instead of a hard-to-pin-down
// once-in-a-while failure (found and fixed a real bug this way -- see
// IntegrateInsert's own comment on newly-tombstoned nodes). Doesn't
// weaken real-world randomness either: DefaultActor() below already
// generates a random actor id for every session that doesn't supply
// its own, so production priority sequences are still effectively
// random -- this only removes randomness ON TOP of an id that's
// already random.
TextCrdt::TextCrdt(std::string actor_id)
    : actor_id_(actor_id.empty() ? DefaultActor() : std::move(actor_id)),
      priority_rng_(static_cast<uint32_t>(std::hash<std::string>()(actor_id_))) {}

TextCrdt::TextCrdt(TextCrdt &&other) noexcept
    : actor_id_(std::move(other.actor_id_)), clock_(other.clock_), root_(other.root_), index_(std::move(other.index_)),
      children_(std::move(other.children_)), pending_inserts_(std::move(other.pending_inserts_)),
      pending_deletes_(std::move(other.pending_deletes_)), priority_rng_(std::move(other.priority_rng_)) {
    other.root_ = nullptr;
}

TextCrdt &TextCrdt::operator=(TextCrdt &&other) noexcept {
    if (this == &other) return *this;
    DeleteSubtree(root_);
    actor_id_ = std::move(other.actor_id_);
    clock_ = other.clock_;
    root_ = other.root_;
    index_ = std::move(other.index_);
    children_ = std::move(other.children_);
    pending_inserts_ = std::move(other.pending_inserts_);
    pending_deletes_ = std::move(other.pending_deletes_);
    priority_rng_ = std::move(other.priority_rng_);
    other.root_ = nullptr;
    return *this;
}

TextCrdt::~TextCrdt() { DeleteSubtree(root_); }

bool TextCrdt::Has(const CrdtId &id) const { return id == Root() || index_.find(id) != index_.end(); }

void TextCrdt::IntegrateInsert(const CrdtOperation &op) {
    if (index_.find(op.id) != index_.end()) return;

    // RGA tie-break for multiple children of the same anchor: ordered by *descending* id, not ascending, so
    // the most-recently-created sibling ends up closest to the anchor and older ones get pushed away -- not
    // an arbitrary choice, this is what makes a second insert at an anchor already used by an older one (the
    // overwhelmingly common non-concurrent case: e.g. two separate edits at buffer position 0) land where it
    // was actually asked to, immediately after the anchor, instead of getting sequenced after that older
    // sibling's *entire* subtree (which, for something like the very first character ever inserted, can be
    // most of the document). Originally had this backwards (ascending order, i.e. newer same-anchor inserts
    // kept getting pushed toward the end of whatever the earliest sibling's subtree had grown to) -- silently
    // wrong (not a crash), invisible to every existing convergence test since those only check that replicas
    // agree with each other, not that Insert(pos, ...) honors the requested position; found via
    // CRDT_PERFORMANCE_PLAN.md Phase 3's own regression test, the first one to check positional fidelity
    // directly (two disjoint edits between two CollabSession::Synchronize calls, one of them landing before
    // an already-tombstoned first line -- the exact shape that exposes this).
    auto &siblings = children_[op.after];
    const auto sib_it = std::upper_bound(siblings.begin(), siblings.end(), op.id);  // first existing sibling with a larger id

    Node *after_node = (op.after == Root()) ? nullptr : index_.at(op.after);
    int insert_pos;
    if (sib_it == siblings.end()) {
        // No existing sibling outranks this one -- it becomes the new closest child of the anchor.
        insert_pos = after_node ? PositionOf(after_node) + 1 : 0;
    } else {
        // *sib_it is the smallest sibling that's still larger than op.id -- i.e. the one immediately
        // preceding op.id in descending (position) order.
        Node *prev_sibling = index_.at(*sib_it);
        insert_pos = EndOfSubtree(prev_sibling);
    }
    siblings.insert(sib_it, op.id);

    auto *node = new Node{};
    node->id = op.id;
    node->after = op.after;
    node->value = op.value;
    // A delete for this id may have arrived (and been buffered in
    // pending_deletes_) before this insert did -- out-of-order delivery
    // is normal (a relay/network gives no ordering guarantee). When
    // that happens the node must be born already tombstoned, AND
    // visible_size must reflect that from the start: Node's default
    // member initializers (total_size=1, visible_size=1) assume a
    // freshly-integrated *visible* node, which is wrong here. Found
    // live via a randomized out-of-order-delivery stress test
    // (collab_crdt_test.cpp's TestOutOfOrderDelivery) -- without this,
    // a node born deleted still counted as +1 toward every ancestor's
    // visible_size, so VisibleSize()/Text() silently included bytes
    // that should have been tombstoned, whenever delete-before-insert
    // ordering happened to occur.
    node->deleted = pending_deletes_.erase(op.id) > 0;
    node->total_size = 1;
    node->visible_size = node->deleted ? 0 : 1;
    node->priority = static_cast<uint32_t>(priority_rng_());
    node->depth = after_node ? after_node->depth + 1 : 0;

    InsertAt(insert_pos, node);
    index_[op.id] = node;

    const auto pending = pending_inserts_.find(op.id);
    if (pending == pending_inserts_.end()) return;
    std::vector<CrdtOperation> waiting = std::move(pending->second);
    pending_inserts_.erase(pending);
    for (const CrdtOperation &next : waiting) IntegrateInsert(next);
}

bool TextCrdt::Apply(const CrdtOperation &op) {
    clock_ = std::max(clock_, op.id.counter);
    if (op.kind == CrdtOperation::Kind::Delete) {
        const auto it = index_.find(op.id);
        if (it == index_.end()) return pending_deletes_.insert(op.id).second;
        if (it->second->deleted) return false;
        MarkDeleted(it->second);
        return true;
    }
    if (index_.find(op.id) != index_.end()) return false;
    if (!Has(op.after)) { pending_inserts_[op.after].push_back(op); return true; }
    IntegrateInsert(op);
    return true;
}
void TextCrdt::Apply(const std::vector<CrdtOperation> &operations) { for (const CrdtOperation &op : operations) Apply(op); }

std::vector<CrdtId> TextCrdt::VisibleIds() const {
    std::vector<CrdtId> ids;
    ids.reserve(root_ ? static_cast<size_t>(root_->visible_size) : 0);
    std::vector<Node *> stack;
    Node *cur = root_;
    while (cur || !stack.empty()) {
        while (cur) { stack.push_back(cur); cur = cur->left; }
        cur = stack.back(); stack.pop_back();
        if (!cur->deleted) ids.push_back(cur->id);
        cur = cur->right;
    }
    return ids;
}
std::string TextCrdt::Text() const {
    std::string text;
    text.reserve(root_ ? static_cast<size_t>(root_->visible_size) : 0);
    std::vector<Node *> stack;
    Node *cur = root_;
    while (cur || !stack.empty()) {
        while (cur) { stack.push_back(cur); cur = cur->left; }
        cur = stack.back(); stack.pop_back();
        if (!cur->deleted) text.push_back(static_cast<char>(cur->value));
        cur = cur->right;
    }
    return text;
}
size_t TextCrdt::VisibleSize() const { return root_ ? static_cast<size_t>(root_->visible_size) : 0; }

std::vector<CrdtOperation> TextCrdt::Insert(size_t offset, const std::string &text) {
    std::vector<CrdtOperation> operations;
    const size_t visible = VisibleSize();
    const size_t clamped = std::min(offset, visible);
    CrdtId after = Root();
    if (clamped > 0) {
        Node *n = SelectVisible(static_cast<int>(clamped) - 1);
        after = n->id;
    }
    for (char raw : text) {
        const unsigned char value = static_cast<unsigned char>(raw);
        CrdtOperation op{CrdtOperation::Kind::Insert, {actor_id_, ++clock_}, after, value};
        Apply(op);
        operations.push_back(op);
        after = op.id;
    }
    return operations;
}
std::vector<CrdtOperation> TextCrdt::Erase(size_t offset, size_t count) {
    std::vector<CrdtOperation> operations;
    const size_t visible = VisibleSize();
    const size_t start = std::min(offset, visible);
    const size_t last = std::min(visible, offset + count);
    // Every delete shrinks the visible sequence, so the node that was at
    // `start+1` becomes the new occupant of position `start` -- re-select
    // at `start` each iteration rather than computing positions/ids up
    // front against a visible sequence that's changing underneath them.
    for (size_t i = start; i < last; ++i) {
        Node *n = SelectVisible(static_cast<int>(start));
        CrdtOperation op{CrdtOperation::Kind::Delete, n->id, Root(), 0};
        Apply(op);
        operations.push_back(op);
    }
    return operations;
}

Json TextCrdt::Snapshot() const {
    Json result = Json::Object(); result["actor"] = actor_id_; result["clock"] = static_cast<long long>(clock_);
    Json entries = Json::Array();
    std::vector<Node *> stack;
    Node *cur = root_;
    while (cur || !stack.empty()) {
        while (cur) { stack.push_back(cur); cur = cur->left; }
        cur = stack.back(); stack.pop_back();
        Json entry = CrdtOperationToJson({CrdtOperation::Kind::Insert, cur->id, cur->after, cur->value});
        entry["deleted"] = cur->deleted;
        entries.push_back(std::move(entry));
        cur = cur->right;
    }
    result["nodes"] = std::move(entries); return result;
}
bool TextCrdt::Restore(const Json &snapshot) {
    if (!snapshot.is_object() || snapshot.get("actor").as_string().empty() || !snapshot.get("nodes").is_array()) return false;
    TextCrdt rebuilt(snapshot.get("actor").as_string()); rebuilt.clock_ = static_cast<uint64_t>(snapshot.get("clock").as_double(0));
    std::vector<CrdtOperation> deletes;
    for (const Json &entry : snapshot.get("nodes").items()) { CrdtOperation op; if (!CrdtOperationFromJson(entry, &op) || op.kind != CrdtOperation::Kind::Insert) return false; rebuilt.Apply(op); if (entry.get("deleted").as_bool()) deletes.push_back({CrdtOperation::Kind::Delete, op.id, {}, 0}); }
    rebuilt.Apply(deletes); if (!rebuilt.pending_inserts_.empty()) return false; *this = std::move(rebuilt); return true;
}

}  // namespace mep::collab
