#include "workspace_git.h"

#include "json.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>

bool ValidWorkspaceName(const std::string &name) {
    if (name.empty()) return false;
    if (name[0] == '.' || name[0] == '-') return false;
    if (name.find("..") != std::string::npos) return false;
    if (name.size() >= 5 && name.compare(name.size() - 5, 5, ".lock") == 0) return false;
    for (char c : name) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '.' || c == '_' || c == '-') continue;
        return false;
    }
    return true;
}

std::string DeriveWorktreeDir(const std::string &project_root, const std::string &ws_name,
                              const std::string &override_dir) {
    namespace fs = std::filesystem;
    fs::path root(project_root);
    fs::path base;
    if (!override_dir.empty()) {
        fs::path ov(override_dir);
        base = ov.is_absolute() ? ov : (root / ov);
    } else {
        std::string repo = root.filename().string();
        if (repo.empty()) repo = "repo";  // root == "/" or trailing slash
        fs::path parent = root.parent_path();
        if (parent.empty()) parent = root;
        base = parent / (repo + ".worktrees");
    }
    return (base / ws_name).lexically_normal().string();
}

std::vector<WorktreeEntry> ParseWorktreeList(const std::string &porcelain_text) {
    std::vector<WorktreeEntry> out;
    WorktreeEntry cur;
    bool have = false;
    auto flush = [&] {
        if (have && !cur.path.empty()) out.push_back(cur);
        cur = WorktreeEntry{};
        have = false;
    };
    size_t pos = 0;
    while (pos <= porcelain_text.size()) {
        size_t nl = porcelain_text.find('\n', pos);
        std::string line = porcelain_text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            flush();
        } else {
            size_t sp = line.find(' ');
            std::string key = line.substr(0, sp);
            std::string val = sp == std::string::npos ? "" : line.substr(sp + 1);
            if (key == "worktree") {
                if (have) flush();  // tolerate a missing blank separator
                cur.path = val;
                have = true;
            } else if (key == "HEAD") {
                cur.head = val;
            } else if (key == "branch") {
                const std::string prefix = "refs/heads/";
                cur.branch = val.compare(0, prefix.size(), prefix) == 0 ? val.substr(prefix.size()) : val;
            } else if (key == "bare") {
                cur.bare = true;
            } else if (key == "detached") {
                cur.detached = true;
            } else if (key == "prunable") {
                cur.prunable = true;
            } else if (key == "locked") {
                cur.locked = true;
            }
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    flush();
    return out;
}

std::string ProjectSlug(const std::string &root) {
    std::string base = std::filesystem::path(root).filename().string();
    if (base.empty()) base = "root";
    std::string out;
    bool pending_dash = false;
    for (char c : base) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '_' || c == '-') {
            if (pending_dash && !out.empty()) out += '-';
            pending_dash = false;
            out += static_cast<char>(std::tolower(uc));
        } else {
            pending_dash = true;
        }
    }
    if (out.empty()) out = "project";
    return out;
}

std::string ProjectHash(const std::string &root) {
    // FNV-1a 32-bit: tiny, deterministic, no dependency; collisions between
    // the handful of projects one user opens are astronomically unlikely
    // and the slug prefix disambiguates further anyway.
    uint32_t h = 2166136261u;
    for (char c : root) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", h);
    return buf;
}

std::string WorkspaceStatePath(const std::string &data_dir, const std::string &root) {
    return data_dir + "/workspaces/" + ProjectSlug(root) + "-" + ProjectHash(root) + ".json";
}

bool ValidWorkspaceStateTree(const Json &node) {
    if (!node.is_object()) return false;
    const std::string dir = node.get("dir").as_string("");
    if (dir == "leaf") {
        const Json &pane = node.get("pane");
        if (!pane.is_object()) return false;
        // `kind` is what tells a file pane from a terminal from a scratch
        // pane; an unknown one means a newer mep wrote this file, which is
        // exactly a case for declining rather than guessing.
        const std::string kind = pane.get("kind").as_string("");
        if (kind != "file" && kind != "terminal" && kind != "empty") return false;
        // A file pane with no path would restore as an empty pane -- a
        // silently different layout, so treat it as malformed instead.
        if (kind == "file" && pane.get("buffer").as_string("").empty()) return false;
        if (pane.contains("buffer_tabs") && !pane.get("buffer_tabs").is_array()) return false;
        return true;
    }
    if (dir != "horizontal" && dir != "vertical") return false;
    const Json &children = node.get("children");
    if (!children.is_array() || children.items().empty()) return false;
    // `shares` is legitimately absent or empty (SplitNode::shares means
    // "equal shares" until something resizes the split), and legitimately
    // stale-sized in the saved file for the same reason -- only a wrong
    // *non-empty* size is treated as equal shares rather than a defect, so
    // this checks the type alone.
    if (node.contains("shares") && !node.get("shares").is_array()) return false;
    for (const Json &child : children.items()) {
        if (!ValidWorkspaceStateTree(child)) return false;
    }
    return true;
}

bool ValidWorkspaceStateTabs(const Json &tabs) {
    if (!tabs.is_array() || tabs.items().empty()) return false;
    for (const Json &tab : tabs.items()) {
        if (!tab.is_object()) return false;
        if (!ValidWorkspaceStateTree(tab.get("root"))) return false;
    }
    return true;
}
