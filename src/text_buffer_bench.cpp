// CRDT_PERFORMANCE_PLAN.md Phase 1: baseline benchmarks for the current
// Buffer::lines representation (std::vector<std::string>, editor.h:607)
// and its whole-snapshot undo/redo (editor.h:637-638) -- run for
// context alongside text_crdt_bench, NOT to justify replacing
// Buffer::lines this round (see the plan doc's own Non-goals: that
// swap is explicitly out of scope). Reimplements the same
// vector<string> editing shape editor.cpp's real primitives use
// (InsertChar/InsertNewline/PasteAfter/PushUndo) rather than linking
// against Editor itself, which pulls in the whole application.

#include "bench_util.h"

#include <cstdio>
#include <random>
#include <string>
#include <vector>

using mep::bench::RecordResult;
using mep::bench::Result;
using mep::bench::Timer;

namespace {

constexpr const char *kHistoryPath = "bench_results/history.jsonl";
constexpr const char *kBinary = "text_buffer_bench";

// Mirrors Editor::InsertChar's shape (editor.cpp:14996): in-place
// std::string::insert at a column.
void InsertChar(std::vector<std::string> &lines, size_t row, size_t col, char c) { lines[row].insert(col, 1, c); }

// Mirrors Editor::InsertNewline's shape (editor.cpp:15005): split the
// current line at col, vector<string>::insert the remainder as a new
// line right after.
void InsertNewline(std::vector<std::string> &lines, size_t row, size_t col) {
    std::string remainder = lines[row].substr(col);
    lines[row].erase(col);
    lines.insert(lines.begin() + static_cast<long>(row) + 1, remainder);
}

/**
 * @brief Sequential typing simulation: insert characters one at a time at the end of the last line, occasionally
 * splitting into a new line (~every 60 chars, like natural line wrapping), staged at doubling cumulative line
 * counts so the effect of a growing vector<string> (line-insert shifting) is visible per stage.
 */
void BenchSequentialTyping() {
    std::vector<std::string> lines{""};
    size_t chars_done = 0;
    for (size_t target_chars : {2000UL, 4000UL, 8000UL, 16000UL, 32000UL}) {
        size_t stage_n = target_chars - chars_done;
        Timer t;
        for (size_t i = 0; i < stage_n; i++) {
            size_t row = lines.size() - 1;
            InsertChar(lines, row, lines[row].size(), 'x');
            if (lines[row].size() % 60 == 0) InsertNewline(lines, row, lines[row].size());
        }
        double ms = t.ElapsedMs();
        RecordResult(kBinary, kHistoryPath, {"sequential_typing_at_" + std::to_string(target_chars), static_cast<long long>(stage_n), ms});
        chars_done = target_chars;
    }
}

/**
 * @brief A large multi-line paste (vector<string>::insert of many lines at once) into documents of increasing
 * pre-existing line count -- shows the O(lines after insertion point) cost of vector<string>::insert.
 */
void BenchLargePaste() {
    std::vector<std::string> paste_lines;
    for (int i = 0; i < 2000; i++) paste_lines.push_back("pasted line " + std::to_string(i));
    for (size_t preexisting : {0UL, 20000UL, 100000UL}) {
        std::vector<std::string> lines(std::max<size_t>(preexisting, 1), "line of existing text");
        // Insert at the very front -- the worst case for vector<string>::insert (every existing line shifts).
        Timer t;
        lines.insert(lines.begin(), paste_lines.begin(), paste_lines.end());
        double ms = t.ElapsedMs();
        RecordResult(kBinary, kHistoryPath, {"large_paste_at_front_of_" + std::to_string(preexisting), static_cast<long long>(paste_lines.size()), ms});
    }
}

/**
 * @brief Whole-buffer-snapshot undo push cost (Editor::PushUndo's own shape: `undo_stack.push_back(lines)`, a
 * full deep copy of every line string) at increasing document sizes -- the cost every single undoable edit pays
 * today, regardless of how small the edit itself is.
 */
void BenchUndoPush() {
    for (size_t line_count : {1000UL, 10000UL, 100000UL}) {
        std::vector<std::string> lines(line_count, "a moderately sized line of representative source code text");
        std::vector<std::vector<std::string>> undo_stack;
        constexpr int kPushes = 20;
        Timer t;
        for (int i = 0; i < kPushes; i++) {
            undo_stack.push_back(lines);  // Buffer::undo_stack.push_back(Buf().lines), editor.cpp:19744-ish
            if (undo_stack.size() > 200) undo_stack.erase(undo_stack.begin());  // kMaxUndo cap, editor.h:9027
        }
        double ms = t.ElapsedMs();
        RecordResult(kBinary, kHistoryPath, {"undo_push_x20_doc_of_" + std::to_string(line_count) + "_lines", kPushes, ms});
    }
}

/**
 * @brief Random-position single-character edits (matching text_crdt_bench's BenchRandomEdits shape) for a direct
 * apples-to-apples comparison against the CRDT under the same workload shape.
 */
void BenchRandomEdits() {
    std::mt19937 rng(42);
    for (size_t preexisting : {5000UL, 20000UL}) {
        std::vector<std::string> lines{std::string(preexisting, 'a')};
        constexpr size_t kOps = 2000;
        Timer t;
        for (size_t i = 0; i < kOps; i++) {
            if (lines[0].empty()) break;
            size_t pos = rng() % lines[0].size();
            if (rng() % 2 == 0) {
                lines[0].insert(pos, 1, 'q');
            } else {
                lines[0].erase(pos, 1);
            }
        }
        double ms = t.ElapsedMs();
        RecordResult(kBinary, kHistoryPath, {"random_edits_in_doc_of_" + std::to_string(preexisting), static_cast<long long>(kOps), ms});
    }
}

}  // namespace

int main() {
    BenchSequentialTyping();
    BenchLargePaste();
    BenchUndoPush();
    BenchRandomEdits();
    std::printf("text_buffer_bench done\n");
    return 0;
}
