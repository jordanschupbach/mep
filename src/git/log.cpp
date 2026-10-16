// git log / git show: the revision walk (date order, and git's
// sort_in_topological_order for --graph/--topo-order), pretty formats,
// ref decorations, and a port of git's graph.c renderer.

#include "git/log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "git/util.h"

namespace mep::git {

std::string RelativeDate(int64_t t) { return FormatDateRelative(t, NowSeconds()); }

namespace {

// ------------------------------------------------------------ commit store

struct Node {
    Oid oid;
    int64_t date = 0;
    std::vector<int> parents;
    bool parsed = false;
    bool ok = false;
    Commit commit;
};

struct Store {
    Repo &repo;
    std::vector<Node> nodes;
    std::unordered_map<Oid, int, OidHash> index;

    int Get(const Oid &oid) {
        auto it = index.find(oid);
        if (it != index.end()) return it->second;
        int i = static_cast<int>(nodes.size());
        nodes.push_back(Node{});
        nodes.back().oid = oid;
        index[oid] = i;
        return i;
    }
    bool Parse(int i) {
        Node &n = nodes[static_cast<size_t>(i)];
        if (n.parsed) return n.ok;
        n.parsed = true;
        Commit c;
        if (!repo.ReadCommit(n.oid, c)) return false;
        n.ok = true;
        n.date = c.committer.time;
        std::vector<int> ps;
        for (const Oid &p : c.parents) ps.push_back(Get(p));
        Node &n2 = nodes[static_cast<size_t>(i)];  // Get may have reallocated
        n2.parents = std::move(ps);
        n2.commit = std::move(c);
        return true;
    }
    Node &At(int i) { return nodes[static_cast<size_t>(i)]; }
};

// ------------------------------------------------------------ walk

std::vector<int> DateWalk(Store &st, const std::vector<int> &starts, bool first_parent, long stop_after) {
    std::vector<int> pending;
    std::vector<char> seen;
    auto mark = [&](int i) {
        if (static_cast<size_t>(i) >= seen.size()) seen.resize(static_cast<size_t>(i) + 1024, 0);
        if (seen[static_cast<size_t>(i)]) return false;
        seen[static_cast<size_t>(i)] = 1;
        return true;
    };
    for (int s : starts) {
        if (!st.Parse(s)) continue;
        if (mark(s)) pending.push_back(s);
    }
    std::stable_sort(pending.begin(), pending.end(), [&](int a, int b) { return st.At(a).date > st.At(b).date; });
    struct Item {
        int64_t date;
        uint64_t seq;
        int node;
    };
    auto cmp = [](const Item &a, const Item &b) {
        if (a.date != b.date) return a.date < b.date;
        return a.seq > b.seq;
    };
    std::priority_queue<Item, std::vector<Item>, decltype(cmp)> q(cmp);
    uint64_t seq = 0;
    for (int s : pending) q.push({st.At(s).date, seq++, s});
    std::vector<int> out;
    while (!q.empty()) {
        int c = q.top().node;
        q.pop();
        out.push_back(c);
        if (stop_after >= 0 && static_cast<long>(out.size()) >= stop_after) break;
        std::vector<int> parents = st.At(c).parents;
        if (first_parent && parents.size() > 1) parents.resize(1);
        for (int p : parents) {
            if (!mark(p)) continue;
            if (!st.Parse(p)) continue;
            q.push({st.At(p).date, seq++, p});
        }
    }
    return out;
}

std::vector<int> TopoSort(Store &st, const std::vector<int> &list, bool first_parent) {
    std::unordered_map<int, int> indeg;
    for (int c : list) indeg[c] = 1;
    auto parents_of = [&](int c) {
        std::vector<int> ps = st.At(c).parents;
        if (first_parent && ps.size() > 1) ps.resize(1);
        return ps;
    };
    for (int c : list)
        for (int p : parents_of(c)) {
            auto it = indeg.find(p);
            if (it != indeg.end() && it->second) it->second++;
        }
    std::vector<int> stack;
    for (int c : list)
        if (indeg[c] == 1) stack.push_back(c);
    std::reverse(stack.begin(), stack.end());
    std::vector<int> out;
    out.reserve(list.size());
    while (!stack.empty()) {
        int c = stack.back();
        stack.pop_back();
        for (int p : parents_of(c)) {
            auto it = indeg.find(p);
            if (it == indeg.end() || !it->second) continue;
            if (--it->second == 1) stack.push_back(p);
        }
        indeg[c] = 0;
        out.push_back(c);
    }
    return out;
}

// ------------------------------------------------------------ decorations

enum class DecoType { Local, Remote, Tag, Stash, Head };
struct Deco {
    std::string name;  // full ref name ("HEAD" for HEAD)
    DecoType type;
};

std::string PrettifyRef(const std::string &name) {
    for (const char *p : {"refs/heads/", "refs/tags/", "refs/remotes/"})
        if (StartsWith(name, p)) return name.substr(std::string(p).size());
    return name;
}

std::unordered_map<Oid, std::vector<Deco>, OidHash> LoadDecorations(Repo &repo) {
    std::unordered_map<Oid, std::vector<Deco>, OidHash> m;
    auto add = [&](const Oid &o, const std::string &name, DecoType t) {
        auto &v = m[o];
        v.insert(v.begin(), Deco{name, t});  // git prepends
    };
    for (const RefEntry &r : repo.ListRefs("refs/")) {
        DecoType t;
        if (StartsWith(r.name, "refs/heads/")) t = DecoType::Local;
        else if (StartsWith(r.name, "refs/remotes/")) t = DecoType::Remote;
        else if (StartsWith(r.name, "refs/tags/")) t = DecoType::Tag;
        else if (r.name == "refs/stash") t = DecoType::Stash;
        else continue;
        add(r.oid, r.name, t);
        // Peel annotated tags: the tagged commit gets a "tag: " decoration too.
        Oid cur = r.oid;
        for (int depth = 0; depth < 10; depth++) {
            ObjType type;
            std::string data;
            if (!repo.Read(cur, type, data) || type != ObjType::Tag) break;
            Tag tag;
            if (!ParseTag(data, tag)) break;
            cur = tag.object;
            add(cur, r.name, DecoType::Tag);
        }
    }
    Oid head;
    if (repo.Resolve("HEAD", head)) add(head, "HEAD", DecoType::Head);
    return m;
}

std::string FormatDecorations(Repo &repo, const std::vector<Deco> &list, const std::string &prefix,
                              const std::string &suffix) {
    if (list.empty()) return "";
    const Deco *current = nullptr;
    bool has_head = false;
    for (const Deco &d : list)
        if (d.type == DecoType::Head) has_head = true;
    if (has_head) {
        std::string branch = repo.HeadRef();
        if (StartsWith(branch, "refs/"))
            for (const Deco &d : list)
                if (d.type == DecoType::Local && d.name == branch) current = &d;
    }
    std::string out = prefix;
    bool first = true;
    for (const Deco &d : list) {
        if (&d == current) continue;
        if (!first) out += ", ";
        first = false;
        if (d.type == DecoType::Tag) out += "tag: ";
        out += PrettifyRef(d.name);
        if (current && d.type == DecoType::Head) out += " -> " + PrettifyRef(current->name);
    }
    return out + suffix;
}

// ------------------------------------------------------------ graph

const char *kColumnColors[] = {"\033[31m",   "\033[32m",   "\033[33m",   "\033[34m",   "\033[35m",   "\033[36m",
                               "\033[1;31m", "\033[1;32m", "\033[1;33m", "\033[1;34m", "\033[1;35m", "\033[1;36m",
                               "\033[m"};
constexpr unsigned short kColorsMax = 12;

struct Graph {
    enum State { Padding, Skip, PreCommit, CommitLine, PostMerge, Collapsing };
    struct Column {
        int commit = -1;
        unsigned short color = 0;
    };
    struct Line {
        std::string buf;
        int width = 0;
        void AddCh(char c) {
            buf += c;
            width++;
        }
        void AddChars(char c, int n) {
            if (n <= 0) return;
            buf.append(static_cast<size_t>(n), c);
            width += n;
        }
        void AddStr(const std::string &s) {
            buf += s;
            width += static_cast<int>(s.size());
        }
    };

    Store &st;
    bool color;
    bool first_parent;
    int commit = -1;
    std::vector<int> parents;  // interesting parents of `commit`
    int num_parents = 0;
    int width = 0;
    int expansion_row = 0;
    State state = Padding, prev_state = Padding;
    int commit_index = 0, prev_commit_index = 0;
    int merge_layout = 0;
    int edges_added = 0, prev_edges_added = 0;
    int num_columns = 0, num_new_columns = 0;
    int mapping_size = 0;
    std::vector<Column> columns, new_columns;
    std::vector<int> mapping, old_mapping;
    unsigned short default_column_color = kColorsMax - 1;

    Graph(Store &s, bool c, bool fp) : st(s), color(c), first_parent(fp) {}

    void WriteColumn(Line &line, const Column &c, char ch) {
        if (c.color < kColorsMax) line.buf += kColumnColors[c.color];
        line.AddCh(ch);
        if (c.color < kColorsMax) line.buf += kColumnColors[kColorsMax];
    }
    unsigned short CurrentColor() const { return color ? default_column_color : kColorsMax; }
    void IncrementColor() { default_column_color = static_cast<unsigned short>((default_column_color + 1) % kColorsMax); }
    unsigned short FindCommitColor(int c) const {
        for (int i = 0; i < num_columns; i++)
            if (columns[static_cast<size_t>(i)].commit == c) return columns[static_cast<size_t>(i)].color;
        return CurrentColor();
    }
    void EnsureCapacity(int n) {
        size_t need = static_cast<size_t>(n) + 1;
        if (columns.size() < need) columns.resize(need);
        if (new_columns.size() < need) new_columns.resize(need);
        if (mapping.size() < 2 * need) mapping.resize(2 * need, -1);
        if (old_mapping.size() < 2 * need) old_mapping.resize(2 * need, -1);
    }
    int FindNewColumn(int c) const {
        for (int i = 0; i < num_new_columns; i++)
            if (new_columns[static_cast<size_t>(i)].commit == c) return i;
        return -1;
    }
    int &Map(int i) { return mapping[static_cast<size_t>(i)]; }

    void InsertIntoNewColumns(int c, int idx) {
        int i = FindNewColumn(c);
        int mapping_idx;
        if (i < 0) {
            i = num_new_columns++;
            new_columns[static_cast<size_t>(i)].commit = c;
            new_columns[static_cast<size_t>(i)].color = FindCommitColor(c);
        }
        if (num_parents > 1 && idx > -1 && merge_layout == -1) {
            int dist = idx - i;
            int shift = (dist > 1) ? 2 * dist - 3 : 1;
            merge_layout = (dist > 0) ? 0 : 1;
            edges_added = num_parents + merge_layout - 2;
            mapping_idx = width + (merge_layout - 1) * shift;
            width += 2 * merge_layout;
        } else if (edges_added > 0 && width >= 2 && i == Map(width - 2)) {
            mapping_idx = width - 2;
            edges_added = -1;
        } else {
            mapping_idx = width;
            width += 2;
        }
        Map(mapping_idx) = i;
    }

    void UpdateColumns() {
        std::swap(columns, new_columns);
        num_columns = num_new_columns;
        num_new_columns = 0;
        int max_new_columns = num_columns + num_parents;
        EnsureCapacity(max_new_columns);
        mapping_size = 2 * max_new_columns;
        for (int i = 0; i < mapping_size; i++) Map(i) = -1;
        width = 0;
        prev_edges_added = edges_added;
        edges_added = 0;
        bool seen_this = false;
        bool is_commit_in_columns = true;
        for (int i = 0; i <= num_columns; i++) {
            int col_commit;
            if (i == num_columns) {
                if (seen_this) break;
                is_commit_in_columns = false;
                col_commit = commit;
            } else {
                col_commit = columns[static_cast<size_t>(i)].commit;
            }
            if (col_commit == commit) {
                seen_this = true;
                commit_index = i;
                merge_layout = -1;
                for (int p : parents) {
                    if (num_parents > 1 || !is_commit_in_columns) IncrementColor();
                    InsertIntoNewColumns(p, i);
                }
                if (num_parents == 0) width += 2;
            } else {
                InsertIntoNewColumns(col_commit, -1);
            }
        }
        while (mapping_size > 1 && Map(mapping_size - 1) < 0) mapping_size--;
    }

    int NumExpansionRows() const { return (num_parents - 2) * 2; }
    bool NeedsPreCommitLine() const {
        return num_parents >= 3 && commit_index < (num_columns - 1) && expansion_row < NumExpansionRows();
    }
    void UpdateState(State s) {
        prev_state = state;
        state = s;
    }

    void Update(int c) {
        commit = c;
        parents = st.At(c).parents;
        if (first_parent && parents.size() > 1) parents.resize(1);
        // Duplicate parents are only interesting once.
        num_parents = static_cast<int>(parents.size());
        prev_commit_index = commit_index;
        UpdateColumns();
        expansion_row = 0;
        if (state != Padding) state = Skip;
        else if (NeedsPreCommitLine()) state = PreCommit;
        else state = CommitLine;
    }

    bool IsMappingCorrect() {
        for (int i = 0; i < mapping_size; i++) {
            int target = Map(i);
            if (target < 0) continue;
            if (target == i / 2) continue;
            return false;
        }
        return true;
    }

    void PaddingLine(Line &line) {
        for (int i = 0; i < num_new_columns; i++) {
            WriteColumn(line, new_columns[static_cast<size_t>(i)], '|');
            line.AddCh(' ');
        }
    }
    void SkipLine(Line &line) {
        line.AddStr("...");
        UpdateState(NeedsPreCommitLine() ? PreCommit : CommitLine);
    }
    void PreCommitLine(Line &line) {
        bool seen_this = false;
        for (int i = 0; i < num_columns; i++) {
            Column &col = columns[static_cast<size_t>(i)];
            if (col.commit == commit) {
                seen_this = true;
                WriteColumn(line, col, '|');
                line.AddChars(' ', expansion_row);
            } else if (seen_this && expansion_row == 0) {
                if (prev_state == PostMerge && prev_commit_index < i) WriteColumn(line, col, '\\');
                else WriteColumn(line, col, '|');
            } else if (seen_this && expansion_row > 0) {
                WriteColumn(line, col, '\\');
            } else {
                WriteColumn(line, col, '|');
            }
            line.AddCh(' ');
        }
        expansion_row++;
        if (!NeedsPreCommitLine()) UpdateState(CommitLine);
    }
    int NumDashedParents() const { return num_parents + merge_layout - 3; }
    void DrawOctopusMerge(Line &line) {
        int dashed = NumDashedParents();
        for (int i = 0; i < dashed; i++) {
            int j = Map((commit_index + i + 2) * 2);
            if (j < 0) continue;
            Column &col = new_columns[static_cast<size_t>(j)];
            WriteColumn(line, col, '-');
            WriteColumn(line, col, (i == dashed - 1) ? '.' : '-');
        }
    }
    void CommitLineOut(Line &line) {
        bool seen_this = false;
        for (int i = 0; i <= num_columns; i++) {
            Column &col = columns[static_cast<size_t>(i)];
            int col_commit;
            if (i == num_columns) {
                if (seen_this) break;
                col_commit = commit;
            } else {
                col_commit = col.commit;
            }
            if (col_commit == commit) {
                seen_this = true;
                line.AddCh('*');
                if (num_parents > 2) DrawOctopusMerge(line);
            } else if (seen_this && edges_added > 1) {
                WriteColumn(line, col, '\\');
            } else if (seen_this && edges_added == 1) {
                if (prev_state == PostMerge && prev_edges_added > 0 && prev_commit_index < i)
                    WriteColumn(line, col, '\\');
                else
                    WriteColumn(line, col, '|');
            } else if (prev_state == Collapsing && old_mapping[static_cast<size_t>(2 * i + 1)] == i &&
                       Map(2 * i) < i) {
                WriteColumn(line, col, '/');
            } else {
                WriteColumn(line, col, '|');
            }
            line.AddCh(' ');
        }
        if (num_parents > 1) UpdateState(PostMerge);
        else if (IsMappingCorrect()) UpdateState(Padding);
        else UpdateState(Collapsing);
    }
    void PostMergeLine(Line &line) {
        static const char kMergeChars[] = {'/', '|', '\\'};
        bool seen_this = false;
        for (int i = 0; i <= num_columns; i++) {
            Column &col = columns[static_cast<size_t>(i)];
            int col_commit;
            if (i == num_columns) {
                if (seen_this) break;
                col_commit = commit;
            } else {
                col_commit = col.commit;
            }
            if (col_commit == commit) {
                int idx = merge_layout;
                seen_this = true;
                for (int j = 0; j < num_parents; j++) {
                    int par_column = FindNewColumn(parents[static_cast<size_t>(j)]);
                    if (par_column < 0) par_column = 0;
                    char c = kMergeChars[idx];
                    WriteColumn(line, new_columns[static_cast<size_t>(par_column)], c);
                    if (idx == 2) {
                        if (edges_added > 0 || j < num_parents - 1) line.AddCh(' ');
                    } else {
                        idx++;
                    }
                }
                if (edges_added == 0) line.AddCh(' ');
            } else if (seen_this) {
                if (edges_added > 0) WriteColumn(line, col, '\\');
                else WriteColumn(line, col, '|');
                line.AddCh(' ');
            } else {
                WriteColumn(line, col, '|');
                if (merge_layout != 0 || i != commit_index - 1) line.AddCh(' ');
            }
        }
        if (IsMappingCorrect()) UpdateState(Padding);
        else UpdateState(Collapsing);
    }
    void CollapsingLine(Line &line) {
        bool used_horizontal = false;
        int horizontal_edge = -1;
        int horizontal_edge_target = -1;
        std::swap(mapping, old_mapping);
        for (int i = 0; i < mapping_size; i++) Map(i) = -1;
        for (int i = 0; i < mapping_size; i++) {
            int target = old_mapping[static_cast<size_t>(i)];
            if (target < 0) continue;
            if (target * 2 == i) {
                Map(i) = target;
            } else if (Map(i - 1) < 0) {
                Map(i - 1) = target;
                if (horizontal_edge == -1) {
                    horizontal_edge = i;
                    horizontal_edge_target = target;
                    for (int j = (target * 2) + 3; j < (i - 2); j += 2) Map(j) = target;
                }
            } else if (Map(i - 1) == target) {
            } else {
                if (i >= 2) Map(i - 2) = target;
                if (horizontal_edge == -1) {
                    horizontal_edge_target = target;
                    horizontal_edge = i - 1;
                    for (int j = (target * 2) + 3; j < (i - 2); j += 2) Map(j) = target;
                }
            }
        }
        for (int i = 0; i < mapping_size; i++) old_mapping[static_cast<size_t>(i)] = Map(i);
        if (Map(mapping_size - 1) < 0) mapping_size--;
        for (int i = 0; i < mapping_size; i++) {
            int target = Map(i);
            if (target < 0) {
                line.AddCh(' ');
            } else if (target * 2 == i) {
                WriteColumn(line, new_columns[static_cast<size_t>(target)], '|');
            } else if (target == horizontal_edge_target && i != horizontal_edge - 1) {
                if (i != (target * 2) + 3) Map(i) = -1;
                used_horizontal = true;
                WriteColumn(line, new_columns[static_cast<size_t>(target)], '_');
            } else {
                if (used_horizontal && i < horizontal_edge) Map(i) = -1;
                WriteColumn(line, new_columns[static_cast<size_t>(target)], '/');
            }
        }
        if (IsMappingCorrect()) UpdateState(Padding);
    }

    // Returns true when this was the commit line.
    bool NextLine(std::string &out) {
        Line line;
        bool shown = false;
        switch (state) {
            case Padding: PaddingLine(line); break;
            case Skip: SkipLine(line); break;
            case PreCommit: PreCommitLine(line); break;
            case CommitLine:
                CommitLineOut(line);
                shown = true;
                break;
            case PostMerge: PostMergeLine(line); break;
            case Collapsing: CollapsingLine(line); break;
        }
        if (line.width < width) line.AddChars(' ', width - line.width);
        out = std::move(line.buf);
        return shown;
    }
    bool Finished() const { return state == Padding; }
};

// ------------------------------------------------------------ formatting

// git log's --expand-tabs=8 (the default for the indented formats): each tab
// pads the text since the previous tab out to a multiple of 8 columns.
std::string ExpandTabs(const std::string &line) {
    if (line.find('\t') == std::string::npos) return line;
    std::string out;
    size_t pos = 0;
    for (;;) {
        size_t tab = line.find('\t', pos);
        if (tab == std::string::npos) break;
        std::string seg = line.substr(pos, tab - pos);
        // Display width: count UTF-8 lead bytes, not continuation bytes.
        size_t width = 0;
        for (char c : seg)
            if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) width++;
        out += seg;
        out.append(8 - width % 8, ' ');
        pos = tab + 1;
    }
    out += line.substr(pos);
    return out;
}

struct Formatter {
    Repo &repo;
    const LogOptions &opts;
    std::unordered_map<Oid, std::vector<Deco>, OidHash> decos;
    bool decos_loaded = false;
    int64_t now = NowSeconds();

    const std::vector<Deco> &DecosOf(const Oid &o) {
        if (!decos_loaded) {
            decos = LoadDecorations(repo);
            decos_loaded = true;
        }
        static const std::vector<Deco> kEmpty;
        auto it = decos.find(o);
        return it == decos.end() ? kEmpty : it->second;
    }

    std::string Date(const Signature &s) {
        switch (opts.date) {
            case LogOptions::Date::Short: return FormatDateShort(s.time, s.tz);
            case LogOptions::Date::Relative: return FormatDateRelative(s.time, now);
            case LogOptions::Date::Iso: return FormatDateIso(s.time, s.tz);
            case LogOptions::Date::Unix: return std::to_string(s.time);
            case LogOptions::Date::Raw: return std::to_string(s.time) + " " + FormatTz(s.tz);
            default: return FormatDateNormal(s.time, s.tz);
        }
    }

    struct ReflogCtx {
        std::string selector;  // "stash@{0}"
        std::string message;
        Signature who;
    };

    std::string User(const std::string &fmt, const Oid &oid, const Commit &c, const ReflogCtx *rl) {
        std::string out;
        for (size_t i = 0; i < fmt.size(); i++) {
            char ch = fmt[i];
            if (ch != '%') {
                out += ch;
                continue;
            }
            if (i + 1 >= fmt.size()) {
                out += '%';
                break;
            }
            char p = fmt[++i];
            auto two = [&](char x) { return i + 1 < fmt.size() && fmt[i + 1] == x; };
            switch (p) {
                case '%': out += '%'; break;
                case 'n': out += '\n'; break;
                case 'H': out += oid.Hex(); break;
                case 'h': out += repo.Abbrev(oid); break;
                case 'T': out += c.tree.Hex(); break;
                case 't': out += repo.Abbrev(c.tree); break;
                case 'P':
                case 'p':
                    for (size_t k = 0; k < c.parents.size(); k++) {
                        if (k) out += ' ';
                        out += p == 'P' ? c.parents[k].Hex() : repo.Abbrev(c.parents[k]);
                    }
                    break;
                case 's': out += CommitSubject(c.message); break;
                case 'b': out += CommitBody(c.message); break;
                case 'B': out += c.message; break;
                case 'd': out += FormatDecorations(repo, DecosOf(oid), " (", ")"); break;
                case 'D': out += FormatDecorations(repo, DecosOf(oid), "", ""); break;
                case 'x':
                    if (i + 2 < fmt.size() && std::isxdigit(static_cast<unsigned char>(fmt[i + 1])) &&
                        std::isxdigit(static_cast<unsigned char>(fmt[i + 2]))) {
                        out += static_cast<char>(std::stoi(fmt.substr(i + 1, 2), nullptr, 16));
                        i += 2;
                    }
                    break;
                case 'a':
                case 'c': {
                    const Signature &s = p == 'a' ? c.author : c.committer;
                    if (i + 1 >= fmt.size()) break;
                    char q = fmt[++i];
                    switch (q) {
                        case 'n':
                        case 'N': out += s.name; break;
                        case 'e':
                        case 'E': out += s.email; break;
                        case 'l': out += s.email.substr(0, s.email.find('@')); break;
                        case 'd': out += Date(s); break;
                        case 'r': out += FormatDateRelative(s.time, now); break;
                        case 't': out += std::to_string(s.time); break;
                        case 'i': out += FormatDateIso(s.time, s.tz); break;
                        case 's': out += FormatDateShort(s.time, s.tz); break;
                        case 'D': out += FormatDateNormal(s.time, s.tz); break;
                        default: out += '%'; out += p; out += q; break;
                    }
                    break;
                }
                case 'g': {
                    if (i + 1 >= fmt.size()) break;
                    char q = fmt[++i];
                    if (!rl) break;
                    switch (q) {
                        case 'd':
                        case 'D': out += rl->selector; break;
                        case 's': out += rl->message; break;
                        case 'n': out += rl->who.name; break;
                        case 'e': out += rl->who.email; break;
                        default: break;
                    }
                    break;
                }
                case 'C':
                    // Colors: %Cred, %Cgreen, %Cblue, %Creset, %C(...) -- not emitted.
                    if (two('(')) {
                        size_t close = fmt.find(')', i);
                        if (close != std::string::npos) i = close;
                    } else {
                        for (const char *name : {"reset", "green", "blue", "red"}) {
                            size_t l = std::char_traits<char>::length(name);
                            if (fmt.compare(i + 1, l, name) == 0) {
                                i += l;
                                break;
                            }
                        }
                    }
                    break;
                default:
                    out += '%';
                    out += p;
                    break;
            }
        }
        return out;
    }

    std::string Medium(const Oid &oid, const Commit &c, bool decorate, bool full) {
        std::string out = "commit " + oid.Hex();
        if (decorate) out += FormatDecorations(repo, DecosOf(oid), " (", ")");
        out += "\n";
        if (c.parents.size() > 1) {
            out += "Merge:";
            for (const Oid &p : c.parents) out += " " + repo.Abbrev(p);
            out += "\n";
        }
        out += "Author: " + c.author.name + " <" + c.author.email + ">\n";
        if (full) out += "Commit: " + c.committer.name + " <" + c.committer.email + ">\n";
        else out += "Date:   " + Date(c.author) + "\n";
        out += "\n";
        // Message, indented by 4; leading blank lines skipped, trailing dropped.
        std::vector<std::string> lines;
        size_t pos = 0;
        const std::string &m = c.message;
        while (pos < m.size()) {
            size_t nl = m.find('\n', pos);
            if (nl == std::string::npos) nl = m.size();
            lines.push_back(m.substr(pos, nl - pos));
            pos = nl + 1;
        }
        auto blank = [](const std::string &l) { return Trim(l).empty(); };
        size_t b = 0;
        while (b < lines.size() && blank(lines[b])) b++;
        size_t e = lines.size();
        while (e > b && blank(lines[e - 1])) e--;
        for (size_t k = b; k < e; k++) {
            if (blank(lines[k])) out += "    \n";
            else out += "    " + ExpandTabs(lines[k]) + "\n";
        }
        return out;
    }
};

}  // namespace

int RunLog(Repo &repo, const LogOptions &opts, std::string &out, std::string &err) {
    Store st{repo, {}, {}};
    Formatter fmt{repo, opts, {}, false, NowSeconds()};

    // Reflog walk (git log -g / git stash list).
    if (opts.reflog_walk) {
        std::string ref = opts.reflog_ref.empty() ? "HEAD" : opts.reflog_ref;
        std::string full = repo.DwimRef(ref);
        if (full.empty()) full = ref;
        std::vector<ReflogEntry> log = repo.ReadReflog(full);
        std::string shortname = full == "HEAD" ? "HEAD" : repo.ShortenRef(full);
        long shown = 0;
        for (size_t k = log.size(); k-- > 0;) {
            if (opts.max_count >= 0 && shown >= opts.max_count) break;
            const ReflogEntry &e = log[k];
            Commit c;
            if (!repo.ReadCommit(e.new_oid, c)) continue;
            Formatter::ReflogCtx rl{shortname + "@{" + std::to_string(log.size() - 1 - k) + "}", e.message, e.who};
            if (opts.format == LogOptions::Format::User) {
                out += fmt.User(opts.user_format, e.new_oid, c, &rl);
                out += "\n";
            } else {
                out += repo.Abbrev(e.new_oid) + " " + rl.selector + ": " + e.message + "\n";
            }
            shown++;
        }
        return 0;
    }

    // Start points.
    std::vector<int> starts;
    auto add_start = [&](const Oid &o) {
        Oid c;
        if (!repo.PeelToCommit(o, c)) return;
        starts.push_back(st.Get(c));
    };
    if (opts.all) {
        for (const RefEntry &r : repo.ListRefs("refs/")) add_start(r.oid);
        Oid h;
        if (repo.Resolve("HEAD", h)) add_start(h);
        // Other worktrees' HEADs.
        std::vector<DirEntry> wts;
        ListDir(repo.CommonDir() + "/worktrees", wts);
        std::sort(wts.begin(), wts.end(), [](const DirEntry &a, const DirEntry &b) { return a.name < b.name; });
        for (const DirEntry &w : wts) {
            std::string text;
            if (!ReadFile(repo.CommonDir() + "/worktrees/" + w.name + "/HEAD", text)) continue;
            text = Trim(text);
            Oid o;
            if (StartsWith(text, "ref: ")) {
                if (repo.Resolve(text.substr(5), o)) add_start(o);
            } else if (Oid::FromHex(text, o)) {
                add_start(o);
            }
        }
    }
    for (const std::string &r : opts.revs) {
        Oid o;
        std::string e;
        if (!repo.RevParse(r, o, &e)) {
            err = e.empty() ? "fatal: bad revision '" + r + "'" : e;
            return 128;
        }
        add_start(o);
    }
    if (!opts.all && opts.revs.empty()) {
        Oid h;
        if (!repo.Resolve("HEAD", h)) {
            std::string br = repo.HeadRef();
            if (StartsWith(br, "refs/heads/")) br = br.substr(11);
            err = "fatal: your current branch '" + br + "' does not have any commits yet";
            return 128;
        }
        add_start(h);
    }

    std::vector<int> list;
    if (opts.no_walk) {
        for (int s : starts)
            if (st.Parse(s)) list.push_back(s);
    } else {
        bool topo = opts.topo || opts.graph;
        list = DateWalk(st, starts, opts.first_parent, topo ? -1 : opts.max_count);
        if (topo) list = TopoSort(st, list, opts.first_parent);
    }
    if (opts.max_count >= 0 && static_cast<long>(list.size()) > opts.max_count)
        list.resize(static_cast<size_t>(opts.max_count));

    Graph graph(st, opts.color, opts.first_parent);
    bool decorate = opts.decorate == 1;
    bool first = true;
    for (int idx : list) {
        if (!st.Parse(idx)) continue;
        Node &n = st.At(idx);
        std::string text;
        switch (opts.format) {
            case LogOptions::Format::User: text = fmt.User(opts.user_format, n.oid, n.commit, nullptr); break;
            case LogOptions::Format::Oneline:
                text = repo.Abbrev(n.oid);
                if (decorate) text += FormatDecorations(repo, fmt.DecosOf(n.oid), " (", ")");
                text += " " + CommitSubject(n.commit.message);
                break;
            case LogOptions::Format::Medium:
            case LogOptions::Format::Short:
            case LogOptions::Format::Full:
                text = fmt.Medium(n.oid, n.commit, decorate, opts.format == LogOptions::Format::Full);
                break;
        }
        // Diff output for `show` / log -p / --stat.
        std::string diff;
        bool is_merge = false;
        if (opts.patch || opts.stat) {
            std::vector<DiffPair> pairs;
            Oid parent_tree;
            if (!n.commit.parents.empty()) {
                Commit pc;
                if (repo.ReadCommit(n.commit.parents[0], pc)) parent_tree = pc.tree;
            }
            if (n.commit.parents.size() <= 1) {
                DiffTrees(repo, parent_tree, n.commit.tree, {}, pairs);
                if (repo.Cfg().GetBool("diff.renames", true)) DetectRenames(repo, pairs);
                DiffOptions dopt;
                if (opts.stat && !pairs.empty()) diff += FormatStat(repo, pairs, dopt);
                if (opts.patch && !pairs.empty()) {
                    if (opts.stat) diff += "\n";
                    diff += FormatPatch(repo, pairs, dopt);
                }
            } else if (opts.combined_merges) {
                // A merge: the stat is against the first parent, the patch is
                // git's dense combined diff (`--cc`) against all parents.
                is_merge = true;
                DiffOptions dopt;
                if (opts.stat) {
                    DiffTrees(repo, parent_tree, n.commit.tree, {}, pairs);
                    if (repo.Cfg().GetBool("diff.renames", true)) DetectRenames(repo, pairs);
                    diff += FormatStat(repo, pairs, dopt);
                }
                if (opts.patch) {
                    int num_paths = 0;
                    std::string cc = FormatCombined(repo, n.commit, num_paths);
                    if (num_paths > 0) {
                        if (opts.stat) diff += "\n";
                        diff += cc;
                    }
                }
            }
        }
        bool multiline = opts.format != LogOptions::Format::User && opts.format != LogOptions::Format::Oneline;
        if (!first && multiline && !opts.graph) out += "\n";
        first = false;
        if (opts.graph) {
            graph.Update(idx);
            std::string line;
            for (;;) {
                bool shown = graph.NextLine(line);
                out += line;
                if (shown) break;
                out += "\n";
            }
            // Message lines after the first get graph padding prefixes.
            std::string msg = text;
            if (!diff.empty()) msg += "\n" + diff;
            size_t pos = 0;
            bool first_line = true;
            while (pos <= msg.size()) {
                size_t nl = msg.find('\n', pos);
                std::string l = msg.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                if (!first_line) {
                    std::string pad;
                    graph.NextLine(pad);
                    out += pad;
                }
                out += l;
                first_line = false;
                if (nl == std::string::npos) break;
                if (nl + 1 == msg.size()) break;
                out += "\n";
                pos = nl + 1;
            }
            if (!graph.Finished()) {
                out += "\n";
                std::string rest;
                for (;;) {
                    graph.NextLine(rest);
                    out += rest;
                    if (graph.Finished()) break;
                    out += "\n";
                }
            }
            out += "\n";
            continue;
        }
        out += text;
        if (opts.format == LogOptions::Format::User || opts.format == LogOptions::Format::Oneline) {
            if (opts.tformat || opts.format == LogOptions::Format::Oneline) out += "\n";
        }
        if (is_merge) {
            out += "\n";  // a combined diff always follows the log with a blank line
            out += diff;
        } else if (!diff.empty()) {
            if (opts.format != LogOptions::Format::Oneline) out += (opts.stat && opts.patch) ? "---\n" : "\n";
            out += diff;
        }
    }
    return 0;
}

}  // namespace mep::git
