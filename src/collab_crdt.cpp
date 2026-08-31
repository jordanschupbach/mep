#include "collab_crdt.h"

#include "json.h"

#include <algorithm>
#include <random>

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
    if (actor.empty() || counter < 0 || counter != static_cast<uint64_t>(counter)) return false;
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

TextCrdt::TextCrdt(std::string actor_id) : actor_id_(actor_id.empty() ? DefaultActor() : std::move(actor_id)) {}
CrdtId TextCrdt::Root() { return {"", 0}; }
bool TextCrdt::Has(const CrdtId &id) const { return id == Root() || nodes_.find(id) != nodes_.end(); }

void TextCrdt::IntegrateInsert(const CrdtOperation &op) {
    if (nodes_.find(op.id) != nodes_.end()) return;
    nodes_.emplace(op.id, Node{op.id, op.after, op.value, pending_deletes_.erase(op.id) > 0});
    auto &children = children_[op.after]; children.push_back(op.id); std::sort(children.begin(), children.end());
    auto pending = pending_inserts_.find(op.id);
    if (pending == pending_inserts_.end()) return;
    std::vector<CrdtOperation> waiting = std::move(pending->second); pending_inserts_.erase(pending);
    for (const CrdtOperation &next : waiting) IntegrateInsert(next);
}
bool TextCrdt::Apply(const CrdtOperation &op) {
    clock_ = std::max(clock_, op.id.counter);
    if (op.kind == CrdtOperation::Kind::Delete) {
        auto node = nodes_.find(op.id);
        if (node == nodes_.end()) return pending_deletes_.insert(op.id).second;
        if (node->second.deleted) return false;
        node->second.deleted = true; return true;
    }
    if (nodes_.find(op.id) != nodes_.end()) return false;
    if (!Has(op.after)) { pending_inserts_[op.after].push_back(op); return true; }
    IntegrateInsert(op); return true;
}
void TextCrdt::Apply(const std::vector<CrdtOperation> &operations) { for (const CrdtOperation &op : operations) Apply(op); }

void TextCrdt::Visit(const CrdtId &id, std::vector<CrdtId> *ids) const {
    auto children = children_.find(id); if (children == children_.end()) return;
    for (const CrdtId &child : children->second) { const auto node = nodes_.find(child); if (node == nodes_.end()) continue; if (!node->second.deleted) ids->push_back(child); Visit(child, ids); }
}
std::vector<CrdtId> TextCrdt::VisibleIds() const { std::vector<CrdtId> ids; Visit(Root(), &ids); return ids; }
std::string TextCrdt::Text() const { std::string text; for (const CrdtId &id : VisibleIds()) text.push_back(static_cast<char>(nodes_.at(id).value)); return text; }
size_t TextCrdt::VisibleSize() const { return VisibleIds().size(); }

std::vector<CrdtOperation> TextCrdt::Insert(size_t offset, const std::string &text) {
    std::vector<CrdtOperation> operations; const std::vector<CrdtId> visible = VisibleIds();
    CrdtId after = offset == 0 ? Root() : visible[std::min(offset, visible.size()) - 1];
    for (unsigned char value : text) { CrdtOperation op{CrdtOperation::Kind::Insert, {actor_id_, ++clock_}, after, value}; Apply(op); operations.push_back(op); after = op.id; }
    return operations;
}
std::vector<CrdtOperation> TextCrdt::Erase(size_t offset, size_t count) {
    const std::vector<CrdtId> visible = VisibleIds(); std::vector<CrdtOperation> operations;
    const size_t last = std::min(visible.size(), offset + count);
    for (size_t i = std::min(offset, visible.size()); i < last; ++i) { CrdtOperation op{CrdtOperation::Kind::Delete, visible[i], Root(), 0}; Apply(op); operations.push_back(op); }
    return operations;
}

Json TextCrdt::Snapshot() const {
    Json result = Json::Object(); result["actor"] = actor_id_; result["clock"] = static_cast<long long>(clock_); Json entries = Json::Array();
    for (const auto &[id, node] : nodes_) { Json entry = CrdtOperationToJson({CrdtOperation::Kind::Insert, id, node.after, node.value}); entry["deleted"] = node.deleted; entries.push_back(std::move(entry)); }
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
