// The layout half of mep's Nix formatter: a port of nixfmt's document
// model and renderer (Nixfmt/Predoc.hs). See nix_format_internal.h.
//
// A document is a list of texts, spacings and groups. A group is rendered
// on one line when it fits, otherwise its spacings become line breaks and
// its children are tried in turn. Two refinements make nixfmt's layout
// what it is, and both are reproduced exactly here: "priority" groups
// (expand only the last argument of a call, keeping the rest on one line)
// and nesting that only indents once per line however many levels open on
// it.
//
// Haskell's lazy lists let the renderer look ahead at "everything after
// this element" for free. Here that lookahead is a chain of (document,
// index) segments on the stack, read through a Stream that flattens groups
// the way `ys ++ xs` does.

#include "nix_format_internal.h"

#include <algorithm>
#include <cstddef>
#include <tuple>
#include <utility>

namespace nixfmt::detail {

int TextWidth(const std::string &s) {
    int n = 0;
    for (const char ch : s) {
        if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) n++;
    }
    return n;
}

namespace {

struct SpV {
    Sp sp;
    int n;
};

bool Less(SpV a, SpV b) {
    if (a.sp != b.sp) return a.sp < b.sp;
    return a.n < b.n;
}

SpV Merge(SpV x, SpV y) {
    if (Less(y, x)) std::swap(x, y);
    if (x.sp == Sp::Break && y.sp == Sp::Softspace) return {Sp::Space, 0};
    if (x.sp == Sp::Break && y.sp == Sp::Hardspace) return {Sp::Space, 0};
    if (x.sp == Sp::Softbreak && y.sp == Sp::Hardspace) return {Sp::Softspace, 0};
    if (x.sp == Sp::Newlines && y.sp == Sp::Newlines) return {Sp::Newlines, x.n + y.n};
    if (x.sp == Sp::Emptyline && y.sp == Sp::Newlines) return {Sp::Newlines, y.n + 2};
    if (x.sp == Sp::Hardspace && y.sp == Sp::Newlines) return {Sp::Newlines, y.n};
    if (y.sp == Sp::Newlines) return {Sp::Newlines, y.n + 1};
    return y;
}

DocE MakeSpacing(SpV v) {
    DocE e;
    e.kind = DocE::Spacing;
    e.sp = v.sp;
    e.n = v.n;
    return e;
}

bool IsSoft(const DocE &e) {
    return e.kind == DocE::Spacing &&
           (e.sp == Sp::Softbreak || e.sp == Sp::Break || e.sp == Sp::Softspace || e.sp == Sp::Space);
}
bool IsHard(const DocE &e) { return e.kind == DocE::Spacing && !IsSoft(e); }
bool IsComment(const DocE &e) {
    if (e.kind == DocE::Text) return e.tann == TAnn::Comment || e.tann == TAnn::TrailingComment;
    if (e.kind == DocE::Group) {
        return std::all_of(e.kids.begin(), e.kids.end(), [](const DocE &x) { return IsComment(x) || IsHard(x); });
    }
    return false;
}

// ---- fixup ----

void FixupInto(Doc input, Doc &out);

struct GroupParts {
    Doc pre, body, post;
};
GroupParts SplitGroup(GAnn ann, Doc kids) {
    Doc fixed;
    FixupInto(std::move(kids), fixed);
    GroupParts g;
    size_t i = 0;
    while (i < fixed.size() && (IsHard(fixed[i]) || IsComment(fixed[i]))) i++;
    size_t j = fixed.size();
    while (j > i && IsHard(fixed[j - 1])) j--;
    g.pre.assign(std::make_move_iterator(fixed.begin()), std::make_move_iterator(fixed.begin() + static_cast<std::ptrdiff_t>(i)));
    g.body.assign(std::make_move_iterator(fixed.begin() + static_cast<std::ptrdiff_t>(i)),
                  std::make_move_iterator(fixed.begin() + static_cast<std::ptrdiff_t>(j)));
    g.post.assign(std::make_move_iterator(fixed.begin() + static_cast<std::ptrdiff_t>(j)), std::make_move_iterator(fixed.end()));
    // simplifyGroup
    if (g.body.size() == 1 && g.body[0].kind == DocE::Group && g.body[0].g == ann) {
        Doc inner = std::move(g.body[0].kids);
        g.body = std::move(inner);
    }
    return g;
}

// `input` is consumed from the back: it holds the remaining list reversed,
// so putting something back in front is a push_back.
void FixupInto(Doc input, Doc &out) {
    std::reverse(input.begin(), input.end());
    auto push_front_all = [&](Doc d) {
        for (size_t k = d.size(); k-- > 0;) input.push_back(std::move(d[k]));
    };
    while (!input.empty()) {
        DocE x = std::move(input.back());
        input.pop_back();
        DocE *next = input.empty() ? nullptr : &input.back();
        if (x.kind == DocE::Spacing && next && next->kind == DocE::Spacing) {
            const SpV m = Merge({x.sp, x.n}, {next->sp, next->n});
            input.pop_back();
            input.push_back(MakeSpacing(m));
            continue;
        }
        if (x.kind == DocE::Text && next && next->kind == DocE::Text && next->tann == x.tann) {
            x.text += next->text;
            input.pop_back();
            input.push_back(std::move(x));
            continue;
        }
        if (x.kind == DocE::Spacing && x.sp == Sp::Hardspace && next && next->kind == DocE::Group) {
            next->kids.insert(next->kids.begin(), std::move(x));
            continue;
        }
        if (x.kind == DocE::Spacing && next && next->kind == DocE::Group) {
            DocE g = std::move(input.back());
            input.pop_back();
            GroupParts parts = SplitGroup(g.g, std::move(g.kids));
            if (parts.body.empty()) {
                // fixup ((a : pre) ++ post ++ ys)
                push_front_all(std::move(parts.post));
                push_front_all(std::move(parts.pre));
                input.push_back(std::move(x));
                continue;
            }
            Doc head;
            head.push_back(std::move(x));
            for (auto &p : parts.pre) head.push_back(std::move(p));
            FixupInto(std::move(head), out);
            DocE ng;
            ng.kind = DocE::Group;
            ng.g = g.g;
            ng.kids = std::move(parts.body);
            out.push_back(std::move(ng));
            push_front_all(std::move(parts.post));
            continue;
        }
        if (x.kind == DocE::Group) {
            GroupParts parts = SplitGroup(x.g, std::move(x.kids));
            if (parts.body.empty()) {
                push_front_all(std::move(parts.post));
                push_front_all(std::move(parts.pre));
                continue;
            }
            FixupInto(std::move(parts.pre), out);
            DocE ng;
            ng.kind = DocE::Group;
            ng.g = x.g;
            ng.kids = std::move(parts.body);
            out.push_back(std::move(ng));
            push_front_all(std::move(parts.post));
            continue;
        }
        out.push_back(std::move(x));
    }
}

// ---- streams over "this doc ++ the rest" ----

struct Seg {
    const Doc *d;
    size_t i;
    const Seg *next;
};

class Stream {
public:
    explicit Stream(const Seg *s) : tail_(s) {}
    Stream(const Doc &d, size_t i) : tail_(nullptr) { st_.emplace_back(&d, i); }

    const DocE *Peek() {
        for (;;) {
            if (!st_.empty()) {
                auto &top = st_.back();
                if (top.second < top.first->size()) return &(*top.first)[top.second];
                st_.pop_back();
                continue;
            }
            if (!tail_) return nullptr;
            st_.emplace_back(tail_->d, tail_->i);
            tail_ = tail_->next;
        }
    }
    void Next() { st_.back().second++; }
    void Enter() {
        const DocE *g = Peek();
        Next();
        st_.emplace_back(&g->kids, 0);
    }
    // A run of adjacent spacings, merged.
    SpV ReadSpacing() {
        const DocE *e = Peek();
        SpV v{e->sp, e->n};
        Next();
        for (;;) {
            const DocE *f = Peek();
            if (!f || f->kind != DocE::Spacing) break;
            v = Merge(v, {f->sp, f->n});
            Next();
        }
        return v;
    }

private:
    std::vector<std::pair<const Doc *, size_t>> st_;
    const Seg *tail_;
};

std::optional<std::string> Fits(int ni, int c, Stream st) {
    std::string out;
    for (;;) {
        if (c < 0) return std::nullopt;
        const DocE *e = st.Peek();
        if (!e) return out;
        switch (e->kind) {
        case DocE::Text: {
            st.Next();
            switch (e->tann) {
            case TAnn::Regular: {
                const int w = TextWidth(e->text);
                out += e->text;
                ni -= w;
                c -= w;
                break;
            }
            case TAnn::Comment: out += e->text; break;
            case TAnn::TrailingComment:
                if (ni == 0) out += ' ';
                out += e->text;
                break;
            case TAnn::Trailing: break;
            }
            break;
        }
        case DocE::Spacing: {
            const SpV v = st.ReadSpacing();
            switch (v.sp) {
            case Sp::Softbreak:
            case Sp::Break: break;
            case Sp::Softspace:
            case Sp::Space:
            case Sp::Hardspace:
                out += ' ';
                ni--;
                c--;
                break;
            case Sp::Hardline:
            case Sp::Emptyline:
            case Sp::Newlines: return std::nullopt;
            }
            break;
        }
        case DocE::Group: st.Enter(); break;
        }
    }
}

int FirstLineWidth(Stream st) {
    int w = 0;
    for (;;) {
        const DocE *e = st.Peek();
        if (!e) return w;
        switch (e->kind) {
        case DocE::Text:
            st.Next();
            if (e->tann != TAnn::Comment && e->tann != TAnn::TrailingComment) w += TextWidth(e->text);
            break;
        case DocE::Spacing: {
            const SpV v = st.ReadSpacing();
            if (v.sp != Sp::Hardspace) return w;
            w++;
            break;
        }
        case DocE::Group: st.Enter(); break;
        }
    }
}

bool FirstLineFits(int target, int max_width, Stream st) {
    int c = max_width;
    for (;;) {
        if (c < 0) return false;
        const DocE *e = st.Peek();
        if (!e) return max_width - c <= target;
        switch (e->kind) {
        case DocE::Text:
            st.Next();
            if (e->tann == TAnn::Regular) c -= TextWidth(e->text);
            break;
        case DocE::Spacing: {
            const SpV v = st.ReadSpacing();
            if (v.sp != Sp::Hardspace) return max_width - c <= target;
            c--;
            break;
        }
        case DocE::Group: {
            Stream after = st;
            after.Next();
            const int flw = FirstLineWidth(after);
            const std::optional<std::string> t = Fits(0, c - flw, Stream(e->kids, 0));
            if (!t) {
                st.Enter();
            } else {
                c -= TextWidth(*t);
                st.Next();
            }
            break;
        }
        }
    }
}

std::pair<int, int> NextIndent(Stream st) {
    for (;;) {
        const DocE *e = st.Peek();
        if (!e) return {0, 0};
        if (e->kind == DocE::Text) return {e->nest, e->off};
        if (e->kind == DocE::Group) return NextIndent(Stream(e->kids, 0));
        st.Next();
    }
}

bool HasPriority(const Doc &d) {
    for (const auto &e : d) {
        if (e.kind != DocE::Group) continue;
        if (e.g == GAnn::Priority) return true;
        if (e.g == GAnn::Transparent && HasPriority(e.kids)) return true;
    }
    return false;
}

void Segments(const Doc &d, std::vector<std::pair<bool, Doc>> &segs) {
    for (const auto &e : d) {
        if (e.kind == DocE::Group && e.g == GAnn::Priority) {
            segs.emplace_back(true, e.kids);
        } else if (e.kind == DocE::Group && e.g == GAnn::Transparent) {
            Segments(e.kids, segs);
        } else if (!segs.empty() && !segs.back().first) {
            segs.back().second.push_back(e);
        } else {
            segs.emplace_back(false, Doc{e});
        }
    }
}

// Each priority group with everything before and after it.
std::vector<std::tuple<Doc, Doc, Doc>> PriorityGroups(const Doc &d) {
    std::vector<std::tuple<Doc, Doc, Doc>> out;
    if (!HasPriority(d)) return out;
    std::vector<std::pair<bool, Doc>> segs;
    Segments(d, segs);
    for (size_t j = 0; j < segs.size(); j++) {
        if (!segs[j].first) continue;
        Doc pre, post;
        for (size_t k = 0; k < j; k++) pre.insert(pre.end(), segs[k].second.begin(), segs[k].second.end());
        for (size_t k = j + 1; k < segs.size(); k++) post.insert(post.end(), segs[k].second.begin(), segs[k].second.end());
        out.emplace_back(std::move(pre), segs[j].second, std::move(post));
    }
    return out;
}

// unexpandSpacing: the soft spacings directly in `d` made compact.
Doc UnexpandShallow(const Doc &d) {
    Doc out;
    for (const auto &e : d) {
        if (e.kind == DocE::Spacing && (e.sp == Sp::Space || e.sp == Sp::Softspace)) {
            out.push_back(MakeSpacing({Sp::Hardspace, 0}));
        } else if (e.kind == DocE::Spacing && (e.sp == Sp::Break || e.sp == Sp::Softbreak)) {
            continue;
        } else {
            out.push_back(e);
        }
    }
    return out;
}

class Layout {
public:
    Layout(int tw, int iw) : tw_(tw), iw_(iw) { ind_.emplace_back(0, 0); }

    std::string Run(const Doc &d) {
        Doc top;
        DocE g;
        g.kind = DocE::Group;
        g.kids = d;
        top.push_back(std::move(g));
        Go(top, 0, nullptr);
        return std::move(out_);
    }

private:
    int tw_, iw_;
    int cc_ = 0;
    std::vector<std::pair<int, int>> ind_;  // (indent, nesting level); back() is the head
    std::string out_;

    struct State {
        int cc;
        std::vector<std::pair<int, int>> ind;
        size_t len;
    };
    State Save() const { return {cc_, ind_, out_.size()}; }
    void Restore(const State &s) {
        cc_ = s.cc;
        ind_ = s.ind;
        out_.resize(s.len);
    }

    void PutText(int nl, int off, const std::string &t) {
        for (;;) {
            const auto [ci, lnl] = ind_.back();
            if (nl > lnl) {
                ind_.emplace_back(cc_ == 0 ? ci + iw_ : ci, nl);
                break;
            }
            if (nl < lnl) {
                ind_.pop_back();
                continue;
            }
            break;
        }
        if (cc_ == 0) out_.append(static_cast<size_t>(std::max(0, ind_.back().first + off)), ' ');
        out_ += t;
        cc_ += TextWidth(t);
    }
    void PutNL(int n) {
        out_.append(static_cast<size_t>(n), '\n');
        cc_ = 0;
    }

    void Go(const Doc &d, size_t from, const Seg *rest) {
        for (size_t i = from; i < d.size(); i++) {
            const Seg s{&d, i + 1, rest};
            GoOne(d[i], &s);
        }
    }

    void GoOne(const DocE &x, const Seg *xs) {
        switch (x.kind) {
        case DocE::Text:
            if (x.tann == TAnn::TrailingComment && cc_ == 2 && NextIndent(Stream(xs)).first > ind_.back().second) {
                out_ += ' ';
                out_ += x.text;
                cc_ += 1 + TextWidth(x.text);
                return;
            }
            PutText(x.nest, x.off, x.text);
            return;
        case DocE::Spacing:
            if (cc_ == 0) return;
            switch (x.sp) {
            case Sp::Break:
            case Sp::Space:
            case Sp::Hardline: PutNL(1); return;
            case Sp::Hardspace:
                out_ += ' ';
                cc_++;
                return;
            case Sp::Emptyline: PutNL(2); return;
            case Sp::Newlines: PutNL(x.n); return;
            case Sp::Softbreak:
                if (!FirstLineFits(tw_ - cc_, tw_, Stream(xs))) PutNL(1);
                return;
            case Sp::Softspace:
                if (FirstLineFits(tw_ - cc_ - 1, tw_, Stream(xs))) {
                    out_ += ' ';
                    cc_++;
                } else {
                    PutNL(1);
                }
                return;
            }
            return;
        case DocE::Group: {
            if (GoGroup(x.kids, 0, xs)) return;
            if (x.g != GAnn::Transparent) {
                auto groups = PriorityGroups(x.kids);
                for (size_t k = groups.size(); k-- > 0;) {
                    if (GoPriority(std::get<0>(groups[k]), std::get<1>(groups[k]), std::get<2>(groups[k]), xs)) return;
                }
            }
            Go(x.kids, 0, xs);
            return;
        }
        }
    }

    bool GoPriority(const Doc &pre, const Doc &prio, const Doc &post, const Seg *rest) {
        const State saved = Save();
        const Seg post_seg{&post, 0, rest};
        const Seg prio_seg{&prio, 0, &post_seg};
        if (!GoGroup(pre, 0, &prio_seg)) {
            Restore(saved);
            return false;
        }
        const Doc compact = UnexpandShallow(post);
        const Seg compact_seg{&compact, 0, rest};
        Go(prio, 0, &compact_seg);
        if (!GoGroup(post, 0, rest)) {
            Restore(saved);
            return false;
        }
        return true;
    }

    // The group on the rest of this line, or false (and nothing changed).
    bool GoGroup(const Doc &grp, size_t from, const Seg *rest) {
        if (from >= grp.size()) return true;
        const Stream rest_stream(rest);
        if (cc_ == 0) {
            // Drop the group's leading spacing: we are at the start of a line.
            std::optional<std::string> t;
            std::pair<int, int> ni;
            const int flw = FirstLineWidth(rest_stream);
            const int rest_nl = NextIndent(rest_stream).first;
            auto iwi = [&](int nl) {
                const int last = ind_.back().second;
                const int line_nl = last + (nl > last ? iw_ : 0);
                return rest_nl > line_nl ? iw_ : 0;
            };
            const DocE &first = grp[from];
            if (first.kind == DocE::Spacing) {
                ni = NextIndent(Stream(grp, from + 1));
                t = Fits(iwi(ni.first), tw_ - flw, Stream(grp, from + 1));
            } else if (first.kind == DocE::Group && !first.kids.empty() && first.kids[0].kind == DocE::Spacing) {
                ni = NextIndent(Stream(first.kids, 1));
                // the group's children without that spacing, then the rest of the group
                const Seg tail{&grp, from + 1, nullptr};
                const Seg inner{&first.kids, 1, &tail};
                t = Fits(iwi(ni.first), tw_ - flw, Stream(&inner));
            } else {
                ni = NextIndent(Stream(grp, from));
                t = Fits(iwi(ni.first), tw_ - flw, Stream(grp, from));
            }
            if (!t) return false;
            PutText(ni.first, ni.second, *t);
            return true;
        }
        const int iwi = NextIndent(rest_stream).first > ind_.back().second ? iw_ : 0;
        const std::optional<std::string> t = Fits(iwi - cc_, tw_ - cc_ - FirstLineWidth(rest_stream), Stream(grp, from));
        if (!t) return false;
        out_ += *t;
        cc_ += TextWidth(*t);
        return true;
    }
};

std::optional<Doc> UnexpandDeep(std::optional<int> n, Stream st) {
    Doc out;
    for (;;) {
        if (n && *n < 0) return std::nullopt;
        const DocE *e = st.Peek();
        if (!e) return out;
        switch (e->kind) {
        case DocE::Text:
            out.push_back(*e);
            if (n) *n -= TextWidth(e->text);
            st.Next();
            break;
        case DocE::Spacing:
            switch (e->sp) {
            case Sp::Hardspace:
            case Sp::Space:
            case Sp::Softspace:
                out.push_back(MakeSpacing({Sp::Hardspace, 0}));
                if (n) *n -= 1;
                break;
            case Sp::Break:
            case Sp::Softbreak: break;
            default: return std::nullopt;
            }
            st.Next();
            break;
        case DocE::Group: st.Enter(); break;
        }
    }
}

}  // namespace

Doc Fixup(const Doc &d) {
    Doc out;
    FixupInto(d, out);
    return out;
}

std::optional<Doc> UnexpandSpacing(std::optional<int> limit, const Doc &d) { return UnexpandDeep(limit, Stream(d, 0)); }

std::string LayoutGreedy(int width, int indent, const Doc &d) {
    Layout l(width, indent);
    return l.Run(d);
}

}  // namespace nixfmt::detail
