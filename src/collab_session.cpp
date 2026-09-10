#include "collab_session.h"

#include "collab_websocket.h"
#include "json.h"

import mep.diff;

#include <algorithm>
#include <stddef.h>
#include <utility>

namespace mep::collab {
/**
 * @brief Wraps a connected WebSocket so the header can hold a pointer to it without exposing the WebSocket type.
 * @param value The connected WebSocket to take ownership of.
 */
class CollabSession::SocketHolder { public: WebSocket socket; explicit SocketHolder(WebSocket value) : socket(std::move(value)) {} };
namespace {
/**
 * @brief Builds a JSON protocol message carrying a list of CRDT operations.
 * @param type The message "type" field, e.g. "update" or "sync_response".
 * @param operations The CRDT operations to encode into the message's "ops" array.
 * @return The assembled JSON message.
 */
Json OperationsMessage(const char *type, const std::vector<CrdtOperation> &operations) {
    Json message = Json::Object(); message["type"] = type; Json list = Json::Array();
    for (const auto &op : operations) list.push_back(CrdtOperationToJson(op));
    message["ops"] = std::move(list); return message;
}
/**
 * @brief Reconstructs the full sequence of CRDT operations (including tombstoning deletes) needed to replay a document's current state.
 * @param document The CRDT document to snapshot.
 * @return The operations representing the document's complete state.
 */
std::vector<CrdtOperation> StateOperations(const TextCrdt &document) {
    std::vector<CrdtOperation> all;
    const Json snapshot = document.Snapshot();
    for (const Json &entry : snapshot.get("nodes").items()) {
        CrdtOperation op;
        if (!CrdtOperationFromJson(entry, &op)) continue;
        all.push_back(op);
        if (entry.get("deleted").as_bool()) all.push_back({CrdtOperation::Kind::Delete, op.id, {}, 0});
    }
    return all;
}
/**
 * @brief Percent-encodes a display name for safe inclusion in a URL query string, passing alphanumerics, '-', and '_' through unescaped.
 * @param name The display name to encode.
 * @return The percent-encoded name.
 */
std::string EncodeName(const std::string &name) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (char raw : name) {
        unsigned char c = static_cast<unsigned char>(raw);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') result.push_back(static_cast<char>(c));
        else { result += '%'; result += hex[c >> 4]; result += hex[c & 15]; }
    }
    return result;
}
/**
 * @brief Splits `text` into lines on '\n' (the delimiter is dropped; a text with no trailing newline still ends
 * in one final line), recording each line's starting byte offset so a Myers-diff hunk's line range can be
 * translated back into a byte range. editor.cpp/main.cpp each keep their own similarly-shaped splitter rather
 * than share one (see editor.cpp's SplitIntoLines comment) since collab_session.cpp links neither -- this one
 * follows that same precedent, kept file-local since only DiffIntoCrdt below needs it.
 * @param text The text to split.
 * @param offsets Out parameter set to each line's starting byte offset, plus a trailing sentinel equal to
 * `text.size()`.
 * @return The text's lines, with newlines removed.
 */
std::vector<std::string> SplitLinesForDiff(const std::string &text, std::vector<size_t> *offsets) {
    std::vector<std::string> lines;
    offsets->clear();
    size_t start = 0;
    for (;;) {
        offsets->push_back(start);
        const size_t newline = text.find('\n', start);
        if (newline == std::string::npos) { lines.push_back(text.substr(start)); break; }
        lines.push_back(text.substr(start, newline - start));
        start = newline + 1;
    }
    offsets->push_back(text.size());
    return lines;
}
}  // namespace

std::vector<CrdtOperation> DiffIntoCrdt(TextCrdt &document, const std::string &old_text, const std::string &new_text) {
    std::vector<CrdtOperation> ops;
    if (old_text == new_text) return ops;
    std::vector<size_t> old_offsets, new_offsets;
    const std::vector<std::string> old_lines = SplitLinesForDiff(old_text, &old_offsets);
    const std::vector<std::string> new_lines = SplitLinesForDiff(new_text, &new_offsets);
    const std::vector<mep::diff::DiffHunk> hunks = mep::diff::MyersDiffHunks(old_lines, new_lines);
    // Hunks are localized in the original old_text/new_text's coordinate space and, since MyersDiffHunks walks
    // its edit script left to right, come out sorted by position -- so a running byte delta (net bytes inserted
    // minus erased by hunks already applied) is enough to keep translating each later hunk's offset into
    // `document`'s live, shifting coordinate space correctly.
    long long delta = 0;
    for (const auto &hunk : hunks) {
        const size_t old_start = static_cast<size_t>(hunk.old_start - 1), new_start = static_cast<size_t>(hunk.new_start - 1);
        const size_t old_from = old_offsets[old_start];
        const size_t old_to = old_offsets[old_start + static_cast<size_t>(hunk.old_count)];
        const size_t new_from = new_offsets[new_start];
        const size_t new_to = new_offsets[new_start + static_cast<size_t>(hunk.new_count)];
        // Narrow further to the exact changed characters within this hunk (common prefix/suffix), so e.g. a
        // single-character edit inside a long line only erases/reinserts that character, not the whole line.
        const size_t max_prefix = std::min(old_to - old_from, new_to - new_from);
        size_t prefix = 0;
        while (prefix < max_prefix && old_text[old_from + prefix] == new_text[new_from + prefix]) ++prefix;
        size_t old_end = old_to, new_end = new_to;
        while (old_end > old_from + prefix && new_end > new_from + prefix && old_text[old_end - 1] == new_text[new_end - 1]) { --old_end; --new_end; }
        const size_t erase_at = old_from + prefix;
        const size_t erase_len = old_end - erase_at;
        const std::string insert_text = new_text.substr(new_from + prefix, new_end - (new_from + prefix));
        if (erase_len == 0 && insert_text.empty()) continue;
        const size_t doc_at = static_cast<size_t>(static_cast<long long>(erase_at) + delta);
        if (erase_len > 0) { const auto erased = document.Erase(doc_at, erase_len); ops.insert(ops.end(), erased.begin(), erased.end()); }
        if (!insert_text.empty()) { const auto inserted = document.Insert(doc_at, insert_text); ops.insert(ops.end(), inserted.begin(), inserted.end()); }
        delta += static_cast<long long>(insert_text.size()) - static_cast<long long>(erase_len);
    }
    return ops;
}

CollabSession::CollabSession(std::string url, std::string name, const std::string &initial_text)
    : url_(std::move(url)), name_(name.empty() ? "Anonymous" : std::move(name)), document_(), last_text_(initial_text) {
    document_.Insert(0, initial_text);
}
CollabSession::~CollabSession() { Stop(); }
// Runs Run() on a new background thread that owns the socket for the session's lifetime.
void CollabSession::Start() { worker_ = std::thread([this] { Run(); }); }
void CollabSession::Stop() {
    { std::lock_guard<std::mutex> lock(mutex_); if (stopping_) return; stopping_ = true; }
    { std::lock_guard<std::mutex> send_lock(send_mutex_); if (socket_) socket_->socket.Close(); }
    if (worker_.joinable()) worker_.join();
}
bool CollabSession::connected() const { std::lock_guard<std::mutex> lock(mutex_); return connected_; }
std::string CollabSession::error() const { std::lock_guard<std::mutex> lock(mutex_); return error_; }

void CollabSession::Send(const Json &message) {
    std::lock_guard<std::mutex> send_lock(send_mutex_);
    if (socket_) socket_->socket.SendText(message.dump());
}
void CollabSession::Run() {
    std::string ws_url = url_ + (url_.find('?') == std::string::npos ? "?name=" : "&name=") + EncodeName(name_);
    std::string conn_error; WebSocket socket = WebSocket::Connect(ws_url, &conn_error);
    if (!socket.valid()) { std::lock_guard<std::mutex> lock(mutex_); error_ = conn_error; return; }
    SocketHolder holder(std::move(socket));
    { std::lock_guard<std::mutex> send_lock(send_mutex_); socket_ = &holder; }
    { std::lock_guard<std::mutex> lock(mutex_); connected_ = true; }
    Json request = Json::Object(); request["type"] = "sync_request"; Send(request);
    std::string text;
    while (holder.socket.ReceiveText(&text, &conn_error)) ReceiveMessage(text);
    { std::lock_guard<std::mutex> send_lock(send_mutex_); socket_ = nullptr; }
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = false;
    if (!stopping_ && !conn_error.empty()) error_ = conn_error;
}
void CollabSession::ReceiveMessage(const std::string &text) {
    Json message; if (!Json::Parse(text, &message) || !message.is_object()) return;
    const std::string type = message.get("type").as_string(); const std::string peer = message.get("peer").as_string();
    std::lock_guard<std::mutex> lock(mutex_);
    if (type == "update" || type == "sync_response") {
        const Json &ops = message.get("ops"); if (!ops.is_array()) return;
        for (const Json &entry : ops.items()) { CrdtOperation op; if (CrdtOperationFromJson(entry, &op)) received_operations_.push_back(op); }
    } else if (type == "sync_request") {
        received_sync_request_ = true;
    } else if (type == "welcome") {
        const Json &peers = message.get("peers"); if (!peers.is_array()) return;
        for (const Json &item : peers.items()) { const std::string id = item.get("peer").as_string(); if (!id.empty()) collaborators_[id] = {id, item.get("name").as_string(), 0, 0, false}; }
    } else if (type == "peer_join" || type == "presence") {
        if (peer.empty()) { return; }
        Collaborator &other = collaborators_[peer]; other.id = peer; other.name = message.get("name").as_string(other.name);
        if (type == "presence") { other.row = message.get("row").as_int(); other.col = message.get("col").as_int(); other.has_location = true; }
    } else if (type == "peer_leave") {
        collaborators_.erase(peer);
    }
}
bool CollabSession::Synchronize(const std::string &local_text, std::string *merged_text) {
    std::vector<CrdtOperation> remote; bool answer_sync = false;
    { std::lock_guard<std::mutex> lock(mutex_); remote.swap(received_operations_); answer_sync = received_sync_request_; received_sync_request_ = false; }
    // Multi-hunk: local_text may carry more than one disjoint edit since the last call (e.g. two far-apart
    // keystrokes coalesced into one tick, or two separate agent-RPC calls) -- diffing line-by-line first
    // localizes each changed region so unrelated unchanged content between them isn't needlessly deleted and
    // reinserted as brand-new CRDT nodes (CRDT_PERFORMANCE_PLAN.md Phase 3).
    std::vector<CrdtOperation> local;
    if (local_text != last_text_) local = DiffIntoCrdt(document_, last_text_, local_text);
    if (!remote.empty()) document_.Apply(remote);
    // The first advert makes a newly-created room usable; later snapshot
    // replies let clients which join after us merge our complete operation
    // history instead of receiving a destructive whole-document overwrite.
    if (!announced_ && connected()) { Send(OperationsMessage("update", StateOperations(document_))); announced_ = true; }
    if (answer_sync) Send(OperationsMessage("sync_response", StateOperations(document_)));
    if (!local.empty()) Send(OperationsMessage("update", local));
    const std::string current = document_.Text(); const bool changed = current != local_text; last_text_ = current;
    if (changed && merged_text) *merged_text = current;
    return changed;
}
void CollabSession::SetPresence(int row, int col) {
    if (!connected()) return;
    if (row == last_presence_row_ && col == last_presence_col_) return;
    last_presence_row_ = row; last_presence_col_ = col;
    Json message = Json::Object(); message["type"] = "presence"; message["row"] = row; message["col"] = col; Send(message);
}
std::vector<Collaborator> CollabSession::Collaborators() const {
    std::lock_guard<std::mutex> lock(mutex_); std::vector<Collaborator> result; result.reserve(collaborators_.size()); for (const auto &[id, collaborator] : collaborators_) result.push_back(collaborator); return result;
}
}  // namespace mep::collab
