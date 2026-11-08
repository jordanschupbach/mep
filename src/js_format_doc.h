#pragma once

// The document IR and line printer behind mep's JavaScript formatter: a
// port of Prettier 3.6.2's src/document (builders.js, utils.js, printer.js;
// MIT, James Long and contributors). Same doc kinds, same propagateBreaks,
// same fits()/printDocToString decisions, so a doc built the way Prettier's
// estree printer builds it prints to the same bytes.
//
// Representation. A Doc is an arena node; the JavaScript "" doc is nullptr
// (so `!doc` means what it means in Prettier), and the shared singletons
// (line, softline, hardline, ...) are compared by address the way Prettier
// compares them with ===. Arrays are DocKind::Array nodes; code that needs
// to edit a parts list before it becomes a doc uses a std::vector<Doc*>.

#include <cstdint>
#include <deque>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace jsfmt {

enum class DocKind : uint8_t {
    String,
    Array,
    Indent,
    Align,
    Group,
    Fill,
    IfBreak,
    IndentIfBreak,
    LineSuffix,
    LineSuffixBoundary,
    Line,
    BreakParent,
    Trim,
    Label,
};

enum class AlignKind : uint8_t { Width, Text, DedentToRoot, Root, Dedent };

// Label bits (Prettier's label objects: {memberChain: true}, {embed, hug}, ...).
enum : uint32_t { kLabelMemberChain = 1u << 0, kLabelTagged = 1u << 1 };

using GroupId = uint32_t;  // 0 = none

struct Doc {
    DocKind kind = DocKind::String;
    bool hard = false, soft = false, literal = false;  // Line
    uint8_t brk = 0;                                     // Group: 0 no, 1 yes, 2 propagated
    bool negate = false;                                 // IndentIfBreak
    bool expanded = false;                               // Group: parts are expandedStates
    AlignKind align_kind = AlignKind::Width;
    int align_n = 0;
    uint32_t label = 0;
    GroupId id = 0;          // Group id, or IfBreak/IndentIfBreak groupId
    std::string_view str;    // String text, Align text
    Doc* contents = nullptr; // Indent Align Group IndentIfBreak LineSuffix Label; IfBreak: breakContents
    Doc* flat = nullptr;     // IfBreak flatContents
    Doc** parts = nullptr;   // Array, Fill; Group expandedStates
    uint32_t n = 0;
};

using Parts = std::vector<Doc*>;

// Thrown when a formatting budget runs out (the caller refuses the input).
struct BudgetExceeded : std::runtime_error {
    BudgetExceeded() : std::runtime_error("formatting budget exceeded") {}
};

class DocArena {
public:
    DocArena();
    DocArena(const DocArena&) = delete;
    DocArena& operator=(const DocArena&) = delete;

    // Builders (document/builders.js).
    Doc* Str(std::string_view s);    // s must outlive the arena; "" -> nullptr
    Doc* Own(std::string s);         // copied into the arena
    Doc* Cat(std::initializer_list<Doc*> parts);
    Doc* Cat(const Parts& parts);
    Doc* Indent(Doc* contents);
    Doc* Align(int n, Doc* contents);
    Doc* AlignText(std::string_view s, Doc* contents);
    Doc* DedentToRoot(Doc* contents);
    Doc* MarkAsRoot(Doc* contents);
    Doc* Dedent(Doc* contents);
    Doc* Group(Doc* contents, bool should_break = false, GroupId id = 0);
    Doc* ConditionalGroup(const Parts& states, bool should_break = false);
    Doc* Fill(const Parts& parts);
    Doc* IfBreak(Doc* break_contents, Doc* flat_contents = nullptr, GroupId group_id = 0);
    Doc* IndentIfBreak(Doc* contents, GroupId group_id, bool negate = false);
    Doc* LineSuffix(Doc* contents);
    Doc* Label(uint32_t label, Doc* contents);  // label 0 -> contents
    Doc* Join(Doc* sep, const Parts& docs);
    Parts JoinParts(Doc* sep, const Parts& docs);
    Doc* AddAlignmentToDoc(Doc* doc, int size, int tab_width);
    GroupId NewId() { return ++next_id_; }

    // Shared singletons.
    Doc* line;
    Doc* softline;
    Doc* hardline;             // [hardlineWithoutBreakParent, breakParent]
    Doc* literalline;          // [literallineWithoutBreakParent, breakParent]
    Doc* hardline_nobreak;
    Doc* literalline_nobreak;
    Doc* break_parent;
    Doc* line_suffix_boundary;
    Doc* trim;

    // Utilities (document/utils.js).
    bool WillBreak(Doc* doc);
    bool CanBreak(Doc* doc);
    bool IsEmptyDoc(Doc* doc);
    void PropagateBreaks(Doc* doc);
    Doc* RemoveLines(Doc* doc);
    Doc* CleanDoc(Doc* doc);
    Doc* StripTrailingHardline(Doc* doc);
    Doc* ReplaceEndOfLine(std::string_view text);  // join(literalline, text.split("\n"))
    Doc* ReplaceEndOfLineDoc(Doc* doc);            // mapDoc over strings

    // Budget: every allocation counts; Throws BudgetExceeded past the limit.
    void SetBudget(size_t max_docs) { max_docs_ = max_docs; }
    size_t Count() const { return docs_.size(); }

private:
    Doc* New(DocKind k);
    Doc** AllocParts(size_t n);
    template <class Fn>
    Doc* MapDoc(Doc* doc, Fn&& fn);
    Doc* StripTrailingHardlineFromDoc(Doc* doc);
    Parts StripTrailingHardlineFromParts(Doc** parts, uint32_t n);

    std::deque<Doc> docs_;
    std::deque<std::string> strings_;
    std::vector<std::unique_ptr<Doc*[]>> chunks_;
    size_t chunk_left_ = 0;
    Doc** chunk_pos_ = nullptr;
    GroupId next_id_ = 0;
    size_t max_docs_ = SIZE_MAX;
};

struct PrintOptions {
    int width = 80;           // < 0: infinite (Number.POSITIVE_INFINITY)
    int tab_width = 2;
    bool use_tabs = false;
    uint64_t* work = nullptr; // optional step counter (budget)
    uint64_t work_limit = UINT64_MAX;
};

// printDocToString: lf line endings, no cursor.
std::string PrintDocToString(DocArena& arena, Doc* doc, const PrintOptions& opts);

// utils/get-string-width.js over UTF-8 text.
int StringWidth(std::string_view s);
// JavaScript's String.prototype.length over UTF-8 text.
size_t Utf16Length(std::string_view s);

}  // namespace jsfmt
