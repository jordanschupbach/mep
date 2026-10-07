// mep-vterm-bench: how fast a :terminal pane can swallow child output.
//
// Why this exists: feeding a terminal is main-thread work (JobManager::
// PollAll -> the session's on_stdout_raw -> VTerm::Feed), so VTerm's
// throughput *is* mep's responsiveness while a chatty child is on
// screen. A program spraying short lines (`yes`, a build log, a spinner)
// is the worst case, because every newline at the bottom of the screen
// scrolls the whole grid and pushes a line into scrollback.
//
// `scroll_flood` is the scenario the slowness was reported from; the
// `*_no_scroll` ones exist so an optimization of the scrolling path is
// not mistaken for one of the parser, and so a regression in ordinary
// output (a TUI repainting in place) shows up here too.

#include "bench_util.h"
#include "vterm.h"

#include <string>

namespace {

// A pane's worth of terminal: a touch wider and taller than a typical
// half-screen split, so the per-newline scroll cost is representative.
constexpr int kRows = 40;
constexpr int kCols = 200;

/**
 * @brief Feeds `data` to a fresh terminal in `chunk` pieces, timing the whole run.
 * @param scenario Name recorded for this measurement.
 * @param data The byte stream to feed.
 * @param chunk Bytes per Feed() call, mimicking the PTY read size the real pipeline uses.
 * @return The timing result, with `n` set to the number of bytes fed.
 */
mep::bench::Result FeedBytes(const std::string &scenario, const std::string &data, size_t chunk = 4096) {
    VTerm vt(kRows, kCols);
    mep::bench::Timer timer;
    for (size_t off = 0; off < data.size(); off += chunk) {
        vt.Feed(data.substr(off, std::min(chunk, data.size() - off)));
    }
    return {scenario, static_cast<long long>(data.size()), timer.ElapsedMs()};
}

/**
 * @brief `line` repeated until the stream is about `bytes` long.
 * @param line The unit to repeat.
 * @param bytes Approximate total size wanted.
 * @return The repeated stream.
 */
std::string Repeat(const std::string &line, size_t bytes) {
    std::string out;
    out.reserve(bytes + line.size());
    while (out.size() < bytes) out += line;
    return out;
}

}  // namespace

int main() {
    const std::string history = "bench_results/history.jsonl";
    const char *kBinary = "vterm_bench";

    // The reported case: `yes` -- two bytes per line, so almost all of the
    // work is the scroll and the scrollback push, with no text to parse.
    mep::bench::RecordResult(kBinary, history, FeedBytes("vterm_scroll_flood_yes", Repeat("y\n", 4u << 20)));

    // Full-width lines: the same scrolling, with a screenful of cells
    // actually written between each one.
    mep::bench::RecordResult(kBinary, history,
                             FeedBytes("vterm_scroll_flood_full_lines",
                                       Repeat(std::string(static_cast<size_t>(kCols), 'x') + "\n", 4u << 20)));

    // A build log's shape: moderate lines, every one of them scrolling.
    mep::bench::RecordResult(
        kBinary, history,
        FeedBytes("vterm_scroll_log_lines", Repeat("[12:34:56] compiling src/some/module/file.cpp ... ok\n", 4u << 20)));

    // No scrolling at all: a TUI repainting in place (home, write a row,
    // move on) -- the common interactive case, and the one an
    // optimization of the scrolling path must not slow down.
    {
        std::string repaint;
        for (int r = 1; r <= kRows; r++) {
            repaint += "\x1b[" + std::to_string(r) + ";1H";
            repaint += "\x1b[1;32m";
            repaint += std::string(static_cast<size_t>(kCols) - 10, 'o');
            repaint += "\x1b[0m";
        }
        mep::bench::RecordResult(kBinary, history, FeedBytes("vterm_tui_repaint_no_scroll", Repeat(repaint, 4u << 20)));
    }

    // Plain printable bytes with wrapping but no escape parsing, to keep
    // an eye on the per-byte path itself.
    mep::bench::RecordResult(kBinary, history, FeedBytes("vterm_plain_wrapped_text", Repeat("the quick brown fox ", 4u << 20)));
    return 0;
}
