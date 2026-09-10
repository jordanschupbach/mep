#pragma once

// A compact replicated growable array (RGA) for editor text. Each inserted
// byte has a stable actor/counter id, so concurrent inserts/deletes commute;
// a relay can therefore forward operations without interpreting document data.

#include <stddef.h>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class Json;

namespace mep::collab {

struct CrdtId {
    std::string actor;
    uint64_t counter = 0;
    /**
     * @brief Orders ids first by actor string, then by counter, giving a total order usable as a map key.
     * @param other the id to compare against
     * @return true if this id sorts strictly before `other`
     */
    bool operator<(const CrdtId &other) const { return actor != other.actor ? actor < other.actor : counter < other.counter; }
    /**
     * @brief Checks whether two ids refer to the same actor/counter pair.
     * @param other the id to compare against
     * @return true if both the actor and counter match
     */
    bool operator==(const CrdtId &other) const { return actor == other.actor && counter == other.counter; }
};

}  // namespace mep::collab

template <>
struct std::hash<mep::collab::CrdtId> {
    /**
     * @brief Combines the actor string's and counter's own std::hash values -- lets CrdtId key an
     * unordered_map/unordered_set (used by TextCrdt's id->treap-node index, CRDT_PERFORMANCE_PLAN.md Phase 2).
     * @param id the id to hash
     * @return a combined hash value
     */
    size_t operator()(const mep::collab::CrdtId &id) const noexcept {
        return std::hash<std::string>()(id.actor) ^ (std::hash<uint64_t>()(id.counter) << 1);
    }
};

namespace mep::collab {

struct CrdtOperation {
    enum class Kind { Insert, Delete };
    Kind kind = Kind::Insert;
    CrdtId id;       // inserted byte, or byte being tombstoned
    CrdtId after;    // only used by insert
    unsigned char value = 0;
};

/**
 * @brief Serializes a single insert or delete CRDT operation to its wire JSON form.
 * @param operation the operation to serialize
 * @return a JSON object describing the operation
 */
Json CrdtOperationToJson(const CrdtOperation &operation);
/**
 * @brief Parses a CRDT operation from its wire JSON form, validating required fields.
 * @param json the JSON value to parse
 * @param operation output operation populated on success
 * @return true if `json` was a well-formed operation and `operation` was filled in
 */
bool CrdtOperationFromJson(const Json &json, CrdtOperation *operation);

class TextCrdt {
public:
    /**
     * @brief Constructs a text CRDT for the given actor, generating a random actor id if none is supplied.
     * @param actor_id stable identifier for this replica's edits; a random id is used when empty
     */
    explicit TextCrdt(std::string actor_id = "");
    // The document owns a heap-allocated treap (CRDT_PERFORMANCE_PLAN.md
    // Phase 2) -- movable (cheap, just transfers the root pointer),
    // deliberately not copyable (a deep copy has never been needed by
    // any caller; Restore() -- the one place that used to rebuild a
    // whole replica and swap it in -- moves the rebuilt temporary in
    // instead).
    TextCrdt(const TextCrdt &) = delete;
    TextCrdt &operator=(const TextCrdt &) = delete;
    TextCrdt(TextCrdt &&other) noexcept;
    TextCrdt &operator=(TextCrdt &&other) noexcept;
    ~TextCrdt();
    /**
     * @brief Returns this replica's actor id.
     * @return the actor id string
     */
    const std::string &actor_id() const { return actor_id_; }
    /**
     * @brief Returns the highest operation counter observed so far by this replica.
     * @return the current logical clock value
     */
    uint64_t clock() const { return clock_; }

    // Local splices return wire-ready operations. `offset`/`count` are byte
    // offsets, matching the editor's string storage and preserving UTF-8.
    /**
     * @brief Inserts text locally at a byte offset, generating and applying one insert operation per byte.
     * @param offset byte offset into the visible text where insertion begins
     * @param text bytes to insert
     * @return the operations generated, in order, ready to broadcast to other replicas
     */
    std::vector<CrdtOperation> Insert(size_t offset, const std::string &text);
    /**
     * @brief Deletes a run of visible bytes locally, generating and applying one delete (tombstone) operation per byte.
     * @param offset byte offset into the visible text where deletion begins
     * @param count number of visible bytes to delete
     * @return the operations generated, in order, ready to broadcast to other replicas
     */
    std::vector<CrdtOperation> Erase(size_t offset, size_t count);
    /**
     * @brief Applies a single remote or local operation, integrating an insert or tombstoning a delete, buffering it if its dependency hasn't arrived yet.
     * @param operation the operation to apply
     * @return true if the operation changed CRDT state (including being buffered as pending)
     */
    bool Apply(const CrdtOperation &operation);
    /**
     * @brief Applies a sequence of operations in order by calling the single-operation Apply for each.
     * @param operations the operations to apply
     */
    void Apply(const std::vector<CrdtOperation> &operations);

    /**
     * @brief Renders the current visible (non-deleted) text by walking the CRDT tree in order.
     * @return the reconstructed document text
     */
    std::string Text() const;
    /**
     * @brief Counts the visible (non-deleted) bytes in the document.
     * @return the number of visible bytes
     */
    size_t VisibleSize() const;
    /**
     * @brief Serializes the full replica state (actor, clock, and every node including tombstones) to JSON.
     * @return a JSON object suitable for persistence or transfer
     */
    Json Snapshot() const;
    /**
     * @brief Rebuilds this replica's state from a previously captured snapshot, replacing current state only on success.
     * @param snapshot the JSON snapshot to restore from
     * @return true if the snapshot was well-formed and fully applied with no operations left pending
     */
    bool Restore(const Json &snapshot);

private:
    // CRDT_PERFORMANCE_PLAN.md Phase 2: the document is a single
    // balanced, position-ordered implicit treap of Node objects
    // (heap-allocated, owned by this TextCrdt via root_/~TextCrdt) --
    // in-order traversal gives the exact same visible-text sequence the
    // old std::map<CrdtId,Node> + children_ DFS walk produced, but
    // position<->id conversion is O(log N) instead of the old O(N) full
    // walk (collab_crdt.cpp's own top comment has the full design
    // writeup). `children_` (semantic sibling order, sorted by id) is
    // kept exactly as before -- it still decides WHERE a new concurrent
    // sibling belongs relative to existing ones; only the "how do we
    // find/maintain positions fast" part changed.
    struct Node {
        CrdtId id, after;
        unsigned char value = 0;
        bool deleted = false;
        Node *left = nullptr, *right = nullptr, *parent = nullptr;
        uint32_t priority = 0;
        int total_size = 1;    // this node + both subtrees, visible or tombstoned
        int visible_size = 1;  // same, counting only non-deleted nodes
        int depth = 0;         // distance from Root() in the SEMANTIC (children_) tree
    };
    std::string actor_id_;
    uint64_t clock_ = 0;
    Node *root_ = nullptr;  // treap root; nullptr means an empty document
    std::unordered_map<CrdtId, Node *> index_;  // O(1) average id -> node
    std::map<CrdtId, std::vector<CrdtId>> children_;
    std::map<CrdtId, std::vector<CrdtOperation>> pending_inserts_;
    std::set<CrdtId> pending_deletes_;
    std::mt19937 priority_rng_;

    /**
     * @brief Returns the sentinel id used as the implicit parent of the first character in the document.
     * @return the root CRDT id (empty actor, counter 0)
     */
    static CrdtId Root();
    /**
     * @brief Checks whether an id is known to this replica, either as the root or as an integrated node.
     * @param id the id to look up
     * @return true if `id` is the root or already present in the treap
     */
    bool Has(const CrdtId &id) const;
    /**
     * @brief Integrates a ready insert operation into the treap at its correct position, then recursively
     * integrates any operations that were waiting on it.
     * @param operation the insert operation whose `after` dependency is already present
     */
    void IntegrateInsert(const CrdtOperation &operation);
    /**
     * @brief Computes the ordered list of currently visible (non-deleted) ids via an in-order treap walk.
     * @return the visible ids in document order
     */
    std::vector<CrdtId> VisibleIds() const;

    // -- Treap (rope) primitives; all O(log N) except where noted. --
    static void Update(Node *n);
    static Node *Merge(Node *a, Node *b);
    static void Split(Node *n, int pos, Node **out_left, Node **out_right);
    int PositionOf(const Node *n) const;
    Node *SelectVisible(int k) const;
    static Node *Successor(Node *n);
    /**
     * @brief Finds the position immediately after `n`'s entire semantic subtree (`n` itself plus every
     * descendant reachable via `children_`), by walking successors while their depth exceeds `n`'s -- bounded by
     * the size of `n`'s own semantic subtree (i.e. by how many concurrent/nested inserts happened at/under `n`),
     * not by total document size.
     * @param n the node whose subtree's end position is sought
     * @return the position one past the last node in `n`'s semantic subtree
     */
    int EndOfSubtree(Node *n) const;
    void InsertAt(int pos, Node *n);
    void MarkDeleted(Node *n);
    static void DeleteSubtree(Node *n);
};

}  // namespace mep::collab
