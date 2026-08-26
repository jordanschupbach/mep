#pragma once

// A compact replicated growable array (RGA) for editor text. Each inserted
// byte has a stable actor/counter id, so concurrent inserts/deletes commute;
// a relay can therefore forward operations without interpreting document data.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

class Json;

namespace mep::collab {

struct CrdtId {
    std::string actor;
    uint64_t counter = 0;
    bool operator<(const CrdtId &other) const { return actor != other.actor ? actor < other.actor : counter < other.counter; }
    bool operator==(const CrdtId &other) const { return actor == other.actor && counter == other.counter; }
};

struct CrdtOperation {
    enum class Kind { Insert, Delete };
    Kind kind = Kind::Insert;
    CrdtId id;       // inserted byte, or byte being tombstoned
    CrdtId after;    // only used by insert
    unsigned char value = 0;
};

Json CrdtOperationToJson(const CrdtOperation &operation);
bool CrdtOperationFromJson(const Json &json, CrdtOperation *operation);

class TextCrdt {
public:
    explicit TextCrdt(std::string actor_id = "");
    const std::string &actor_id() const { return actor_id_; }
    uint64_t clock() const { return clock_; }

    // Local splices return wire-ready operations. `offset`/`count` are byte
    // offsets, matching the editor's string storage and preserving UTF-8.
    std::vector<CrdtOperation> Insert(size_t offset, const std::string &text);
    std::vector<CrdtOperation> Erase(size_t offset, size_t count);
    bool Apply(const CrdtOperation &operation);
    void Apply(const std::vector<CrdtOperation> &operations);

    std::string Text() const;
    size_t VisibleSize() const;
    Json Snapshot() const;
    bool Restore(const Json &snapshot);

private:
    struct Node { CrdtId id, after; unsigned char value = 0; bool deleted = false; };
    std::string actor_id_;
    uint64_t clock_ = 0;
    std::map<CrdtId, Node> nodes_;
    std::map<CrdtId, std::vector<CrdtId>> children_;
    std::map<CrdtId, std::vector<CrdtOperation>> pending_inserts_;
    std::set<CrdtId> pending_deletes_;

    static CrdtId Root();
    bool Has(const CrdtId &id) const;
    void IntegrateInsert(const CrdtOperation &operation);
    void Visit(const CrdtId &id, std::vector<CrdtId> *ids) const;
    std::vector<CrdtId> VisibleIds() const;
};

}  // namespace mep::collab
