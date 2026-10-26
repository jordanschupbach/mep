// mep-editor-reload-test: the regression harness for the reported bug where
// `git pull` inside mep silently lost the pulled commits.
//
// What was reported: work pushed from one machine, pulled on another from
// inside mep. The pull succeeded -- but the pane kept showing the pre-pull
// text, and once mep was closed and reopened the pulled changes were gone,
// while `git pull` insisted the repository was "Already up to date". It read
// as a forgotten push and went unrecognised as a bug for a long time.
//
// What was actually happening: mep watches no files (there is no inotify /
// kqueue / FSEvents anywhere in this codebase), and `struct Buffer` recorded
// nothing about the file it was read from, so nothing could detect that a
// file had been rewritten underneath an open buffer. `git pull` updated the
// working tree; the buffer went on holding pre-pull text; and the next write
// -- very often the "Save main.py?" prompt on the way out -- truncated the
// file and put that stale text back. HEAD had genuinely moved, so git was
// telling the truth; only the working tree had been reverted.
//
// Every check below drives a real Editor against real files on disk, and
// rewrites those files behind its back the way git does, so a failure here
// is a failure a person can hit. No window is opened.

#include "editor.h"

#include "gfx/backend.h"
#include "gfx/backend_native.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Defined in main.cpp for the real binary; a windowless test has to supply
// them (lua_env.cpp's bindings pull them in from mep_core). Nothing on the
// save/reload path calls any of them.
float GetFontSizePx();
float GetFontSizePx() { return 16.0f; }
float GetCharWidthPx();
float GetCharWidthPx() { return 8.0f; }
void InvalidateOrgInlineImageTexture(const std::string &path);
void InvalidateOrgInlineImageTexture(const std::string &) {}
void SetFontSizePx(float px);
void SetFontSizePx(float) {}
std::string MathRenderFastPng(const std::string &tex, float size_px, const std::string &out_dir);
std::string MathRenderFastPng(const std::string &, float, const std::string &) { return std::string(); }

namespace {

int g_failures = 0;
int g_checks = 0;

void Report(bool ok, const std::string &what, const std::string &detail) {
    g_checks++;
    if (ok) {
        std::printf("  ok   %s\n", what.c_str());
        return;
    }
    g_failures++;
    std::printf("  FAIL %s\n       %s\n", what.c_str(), detail.c_str());
}

#define EXPECT(cond, what, detail) Report((cond), (what), (detail))

// The buffer's text as one string, the way it would be written back out.
std::string BufferText(const Editor &ed, int buffer_id) {
    std::string s;
    for (const std::string &line : ed.GetBuffer(buffer_id).lines) {
        s += line;
        s += "\n";
    }
    return s;
}

std::string ReadWholeFile(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "<unreadable>";
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteWholeFile(const std::string &path, const std::string &content) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    out.close();
}

// The pre-pull file: what the laptop had checked out.
const char *kBeforePull =
    "def gain_ratio(df, feature):\n"
    "    gain_f = gain(df, feature)\n"
    "    iv_f = shannon_sum(df[feature])\n"
    "    if iv_f > 0:\n"
    "        return gain_f/iv_f\n"
    "    else:\n"
    "        return 0\n";

// What `git pull` brought in: deliberately a different length as well as
// different bytes, so the stamp differs in size and not only in mtime --
// a filesystem with a coarse clock must not be able to turn this test
// green by accident.
const char *kAfterPull =
    "def gain_ratio(df, feature):\n"
    "    \"\"\"Information gain normalised by the feature's split info.\"\"\"\n"
    "    gain_f = gain(df, feature)\n"
    "    iv_f = shannon_sum(df[feature])\n"
    "    if iv_f > 0:\n"
    "        return gain_f / iv_f\n"
    "    return 0.0\n"
    "\n"
    "\n"
    "def evaluate(folds, model):\n"
    "    scores = [model.score(f) for f in folds]\n"
    "    return sum(scores) / len(scores)\n";

// A minimal classic-xref PDF with `pages` empty US-letter pages (the same
// hand-built shape pdf_document_test.cpp uses); offsets are computed, not
// hardcoded. Each page count gives a different byte length, so the stamp
// differs in size as well as mtime, as for kBeforePull/kAfterPull.
std::string MinimalPdf(int pages) {
    std::vector<std::pair<int, std::string>> objects;
    std::string kids;
    for (int i = 0; i < pages; ++i) kids += std::to_string(3 + i) + " 0 R ";
    objects.emplace_back(1, "<< /Type /Catalog /Pages 2 0 R >>");
    objects.emplace_back(2, "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(pages) +
                                " /MediaBox [0 0 612 792] >>");
    for (int i = 0; i < pages; ++i) objects.emplace_back(3 + i, "<< /Type /Page /Parent 2 0 R >>");
    std::string doc = "%PDF-1.4\n";
    std::vector<size_t> offsets;
    for (const auto &obj : objects) {
        offsets.push_back(doc.size());
        doc += std::to_string(obj.first) + " 0 obj\n" + obj.second + "\nendobj\n";
    }
    const size_t xref_off = doc.size();
    doc += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t off : offsets) {
        char line[32];
        std::snprintf(line, sizeof(line), "%010lld 00000 n \n", static_cast<long long>(off));
        doc += line;
    }
    doc += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R >>\n";
    doc += "startxref\n" + std::to_string(xref_off) + "\n%%EOF\n";
    return doc;
}

std::string ScratchDir() {
    const char *env = std::getenv("MEP_RELOAD_TEST_DIR");
    if (env && *env) return env;
    return (std::filesystem::temp_directory_path() / "mep-reload-test").string();
}

}  // namespace

int main() {
    // Installing the native backend set only registers function pointers; it
    // touches neither X11 nor GL until something asks for a window, and
    // nothing here does. Editor's constructor does reach the gfx:: facade
    // (theme/font bookkeeping), which aborts with no backend installed.
    gfx::SetBackends(gfx::ToBackends(gfx::CreateNativeBackendSet()));

    const std::string dir = ScratchDir();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    // ------------------------------------------------------------------ 1
    // THE REPORTED BUG, END TO END. A buffer open on a file, the file
    // replaced underneath it (what `git pull` does), then a save. Before the
    // fix the save succeeded and the pulled content was gone for good.
    std::printf("\n== 1. a save cannot overwrite a file that changed on disk\n");
    {
        const std::string path = dir + "/pull/main.py";
        WriteWholeFile(path, kBeforePull);

        Editor ed;
        ed.LoadFile(path);
        const int id = ed.CurrentBufferId();
        EXPECT(BufferText(ed, id) == kBeforePull, "the buffer opened on the pre-pull file",
               BufferText(ed, id));
        EXPECT(!ed.BufferChangedOnDisk(id), "a freshly opened file is not reported as changed",
               "it was reported changed");

        // `git pull` lands. mep is told nothing -- that is the whole premise.
        WriteWholeFile(path, kAfterPull);

        EXPECT(ed.BufferChangedOnDisk(id), "the pulled file is detected as changed on disk",
               "the change went unnoticed");

        // The exact call the quit prompt's "Save" made.
        const bool saved = ed.SaveBufferById(id);
        EXPECT(!saved, "the save is refused rather than performed", "it reported success");
        EXPECT(ReadWholeFile(path) == kAfterPull,
               "THE REGRESSION: the pulled file on disk is untouched by the refused save",
               "the file now holds: " + ReadWholeFile(path));
        EXPECT(ed.StatusMessage().find("changed on disk") != std::string::npos,
               "the refusal says why, and names both ways out",
               "status was: " + ed.StatusMessage());
    }

    // ------------------------------------------------------------------ 2
    // The override has to exist, or the guard above is just an obstacle:
    // `:w!` means "I know, mine wins".
    std::printf("\n== 2. :w! deliberately overwrites it\n");
    {
        const std::string path = dir + "/force/main.py";
        WriteWholeFile(path, kBeforePull);
        Editor ed;
        ed.LoadFile(path);
        const int id = ed.CurrentBufferId();
        WriteWholeFile(path, kAfterPull);

        EXPECT(!ed.SaveBufferById(id), "a plain save is still refused", "it succeeded");
        ed.RunCommand("w!");
        EXPECT(ReadWholeFile(path) == kBeforePull, "w! wrote the buffer over the newer file",
               "the file holds: " + ReadWholeFile(path));
        EXPECT(!ed.BufferChangedOnDisk(id), "and the buffer is in step with the file again",
               "it still reports a disk change");
        EXPECT(ed.SaveBufferById(id), "so an ordinary save works again", ed.StatusMessage());
    }

    // ------------------------------------------------------------------ 3
    // The other half of the report: "sometimes they never appeared". The
    // pull worked, the file on disk was right, and the pane went on showing
    // the old text because nothing ever re-read it.
    std::printf("\n== 3. an unmodified buffer is re-read after the working tree moves\n");
    {
        const std::string path = dir + "/sync/main.py";
        WriteWholeFile(path, kBeforePull);
        Editor ed;
        ed.LoadFile(path);
        const int id = ed.CurrentBufferId();
        WriteWholeFile(path, kAfterPull);

        std::vector<std::string> conflicts;
        const int reloaded = ed.ReloadChangedBuffersUnder(dir + "/sync", &conflicts);
        EXPECT(reloaded == 1, "one buffer was reloaded", std::to_string(reloaded) + " were");
        EXPECT(conflicts.empty(), "an unmodified buffer is not a conflict",
               std::to_string(conflicts.size()) + " conflicts reported");
        EXPECT(BufferText(ed, id) == kAfterPull, "the pane now shows the pulled text",
               BufferText(ed, id));
        EXPECT(!ed.GetBuffer(id).modified, "and is not left looking locally modified",
               "it is marked modified");
        EXPECT(!ed.BufferChangedOnDisk(id), "and is in step with the file", "it still reports a change");
    }

    // ------------------------------------------------------------------ 4
    // A buffer with unsaved edits must NOT be silently reloaded: those edits
    // exist nowhere else, and discarding them to take the file's version is
    // the same class of silent loss pointed the other way. It is reported so
    // the user can decide, and left exactly as it was.
    std::printf("\n== 4. a buffer with unsaved edits is reported, never silently reloaded\n");
    {
        const std::string path = dir + "/dirty/main.py";
        WriteWholeFile(path, kBeforePull);
        Editor ed;
        ed.LoadFile(path);
        const int id = ed.CurrentBufferId();
        ed.RunCommand("normal ggOimport numpy as np");  // a local edit, unsaved
        const std::string edited = BufferText(ed, id);
        EXPECT(ed.GetBuffer(id).modified, "the buffer has unsaved edits", "it is not marked modified");

        WriteWholeFile(path, kAfterPull);

        std::vector<std::string> conflicts;
        const int reloaded = ed.ReloadChangedBuffersUnder(dir + "/dirty", &conflicts);
        EXPECT(reloaded == 0, "nothing was reloaded", std::to_string(reloaded) + " were");
        EXPECT(conflicts.size() == 1, "the clash is reported to the caller",
               std::to_string(conflicts.size()) + " conflicts reported");
        EXPECT(BufferText(ed, id) == edited, "the unsaved edits are still there untouched",
               BufferText(ed, id));
        EXPECT(ReadWholeFile(path) == kAfterPull, "and the pulled file is still there untouched",
               ReadWholeFile(path));

        // Both ways out of the clash, which is what the quit prompt offers.
        EXPECT(!ed.SaveBufferById(id), "a plain save of the clashing buffer is refused", "it succeeded");
        EXPECT(ed.ReloadBufferFromDisk(id, /*force=*/true), "a forced reload takes the file's version",
               "the reload failed");
        EXPECT(BufferText(ed, id) == kAfterPull, "the buffer holds the file's text", BufferText(ed, id));
        ed.RunCommand("normal u");
        EXPECT(BufferText(ed, id) == edited,
               "and undo brings the discarded edits back -- a reload never loses text outright",
               BufferText(ed, id));
    }

    // ------------------------------------------------------------------ 5
    // Scope. The guard must not fire where there is no conflict to find, or
    // it becomes a prompt people learn to dismiss.
    std::printf("\n== 5. the guard stays out of the way where nothing changed\n");
    {
        // An ordinary edit-then-save cycle, repeated: every write re-stamps,
        // so the second save must not think the first one was someone else.
        const std::string path = dir + "/quiet/main.py";
        WriteWholeFile(path, kBeforePull);
        Editor ed;
        ed.LoadFile(path);
        const int id = ed.CurrentBufferId();
        ed.RunCommand("normal ggOimport os");
        EXPECT(ed.SaveBufferById(id), "the first save goes through", ed.StatusMessage());
        ed.RunCommand("normal ggOimport sys");
        EXPECT(ed.SaveBufferById(id), "and so does the next one", ed.StatusMessage());
        EXPECT(!ed.BufferChangedOnDisk(id), "repeated saves never report a false disk change",
               "a false change was reported");

        // A brand-new file has nothing to be stale against.
        const std::string fresh = dir + "/quiet/brand-new.py";
        ed.LoadFile(fresh);
        const int fresh_id = ed.CurrentBufferId();
        ed.RunCommand("normal iprint('hello')");
        EXPECT(!ed.BufferChangedOnDisk(fresh_id), "a file that does not exist yet is not 'changed'",
               "it was reported changed");
        EXPECT(ed.SaveBufferById(fresh_id), "and saving it creates it", ed.StatusMessage());

        // A file deleted under the buffer is not a conflict either: there are
        // no newer bytes to protect, and a save that recreates it is the
        // point.
        std::filesystem::remove(path, ec);
        ed.RunCommand("normal ggOimport json");
        EXPECT(ed.SaveBufferById(id), "a save recreates a file deleted under the buffer",
               ed.StatusMessage());
    }

    // ------------------------------------------------------------------ 6
    // A save-as is not this guard's business: Buffer::disk describes the
    // file the buffer was read from, and comparing it against a *different*
    // target would refuse a perfectly ordinary `:w other.py`.
    std::printf("\n== 6. a save-as to another existing file is not refused\n");
    {
        const std::string src = dir + "/saveas/from.py";
        const std::string dst = dir + "/saveas/to.py";
        WriteWholeFile(src, kBeforePull);
        WriteWholeFile(dst, kAfterPull);
        Editor ed;
        ed.LoadFile(src);
        const int id = ed.CurrentBufferId();
        EXPECT(ed.SaveFile(dst), "the save-as went through", ed.StatusMessage());
        EXPECT(ReadWholeFile(dst) == kBeforePull, "it wrote the buffer to the new path",
               ReadWholeFile(dst));
        // ...and the buffer now tracks its new file, so the guard protects
        // *that* one from here on.
        WriteWholeFile(dst, kAfterPull);
        EXPECT(ed.BufferChangedOnDisk(id), "and the buffer now tracks the file it was saved as",
               "a change to the new file went unnoticed");
    }

    // ------------------------------------------------------------------ 7
    // Buffers outside the directory a git action ran in are nobody's
    // business: a pull in one project must not re-read files in another.
    std::printf("\n== 7. only buffers under the given root are considered\n");
    {
        const std::string inside = dir + "/roots/repo/main.py";
        const std::string outside = dir + "/roots/elsewhere/notes.py";
        WriteWholeFile(inside, kBeforePull);
        WriteWholeFile(outside, kBeforePull);
        Editor ed;
        ed.LoadFile(inside);
        const int in_id = ed.CurrentBufferId();
        ed.LoadFile(outside);
        const int out_id = ed.CurrentBufferId();
        WriteWholeFile(inside, kAfterPull);
        WriteWholeFile(outside, kAfterPull);

        const int reloaded = ed.ReloadChangedBuffersUnder(dir + "/roots/repo", nullptr);
        EXPECT(reloaded == 1, "only the buffer under the root was reloaded",
               std::to_string(reloaded) + " were");
        EXPECT(BufferText(ed, in_id) == kAfterPull, "the in-root buffer was re-read",
               BufferText(ed, in_id));
        EXPECT(BufferText(ed, out_id) == kBeforePull, "the out-of-root buffer was left alone",
               BufferText(ed, out_id));
    }

    // ------------------------------------------------------------------ 8
    // The same file open in two split panes: a reload that shrinks the file
    // must not leave the other pane's cursor past the new end of it.
    std::printf("\n== 8. every pane's cursor survives a reload that shrinks the file\n");
    {
        const std::string path = dir + "/splits/main.py";
        WriteWholeFile(path, kAfterPull);  // the long version first
        Editor ed;
        ed.LoadFile(path);
        const int id = ed.CurrentBufferId();
        ed.RunCommand("split");
        ed.RunCommand("normal G");  // last line, in this pane
        WriteWholeFile(path, kBeforePull);  // now a much shorter file

        EXPECT(ed.ReloadBufferFromDisk(id), "the reload went through", "it failed");
        const int lines = static_cast<int>(ed.GetBuffer(id).lines.size());
        EXPECT(BufferText(ed, id) == kBeforePull, "the buffer holds the shorter file",
               BufferText(ed, id));
        // The real assertion: no pane is left pointing past the end. An
        // out-of-range cursor is a crash or a garbage row at draw time, not
        // something that shows up as wrong text.
        EXPECT(ed.Cursor().row >= 0 && ed.Cursor().row < lines,
               "the focused pane's cursor is inside the shortened file",
               "row " + std::to_string(ed.Cursor().row) + " of " + std::to_string(lines));
    }

    // ------------------------------------------------------------------ 9
    // The same no-watcher gap for a PDF viewer: a document recompiled
    // beside mep (pdflatex writing main.pdf) has to show up on its own,
    // without a half-written file being decoded mid-build and without the
    // reader being thrown back to page one. Editor::TickPdfAutoReload is
    // what HandleInput runs every frame; here it is driven with its own
    // clock, so the poll interval and settle wait are exercised exactly.
    std::printf("\n== 9. an open PDF follows its file on disk\n");
    {
        const std::string path = dir + "/latex/main.pdf";
        WriteWholeFile(path, MinimalPdf(3));
        Editor ed;
        ed.LoadFile(path);
        const int id = ed.CurrentBufferId();
        EXPECT(ed.IsPdfBuffer(id), "the file opened as a PDF buffer", "it did not");
        PdfSession *sess = ed.AnyPdfSessionForBuffer(id);
        EXPECT(sess != nullptr && sess->doc && sess->doc->PageCount() == 3,
               "the viewer holds the three-page build",
               sess && sess->doc ? std::to_string(sess->doc->PageCount()) + " pages" : "no session");
        if (!sess) return 1;
        sess->page = 2;  // proofreading the last page
        sess->scroll_y = 120.0f;

        // Nothing changed: however often the tick runs, nothing happens.
        ed.TickPdfAutoReload(1.0);
        ed.TickPdfAutoReload(2.0);
        EXPECT(sess->page == 2 && sess->doc->PageCount() == 3, "an unchanged file is left alone",
               "page " + std::to_string(sess->page));

        // The build rewrites the file: a longer document this time. The
        // first poll after the write sees a new stamp and starts the wait;
        // a poll inside the settle window must NOT decode yet (the real
        // file would still be half-written), the one after it must.
        WriteWholeFile(path, MinimalPdf(5));
        ed.TickPdfAutoReload(10.0);
        EXPECT(sess->doc->PageCount() == 3, "the first sight of a changed file only starts the wait",
               std::to_string(sess->doc->PageCount()) + " pages already");
        ed.TickPdfAutoReload(10.3);
        EXPECT(sess->doc->PageCount() == 3, "a file seen 0.3 s ago is still waited on",
               std::to_string(sess->doc->PageCount()) + " pages already");
        ed.TickPdfAutoReload(10.7);
        EXPECT(sess->doc->PageCount() == 5, "THE FEATURE: once the file holds still it is re-read",
               std::to_string(sess->doc->PageCount()) + " pages");
        EXPECT(sess->page == 2 && sess->scroll_y == 120.0f,
               "the reader is still on the page they were on, at the same scroll",
               "page " + std::to_string(sess->page) + " scroll " + std::to_string(sess->scroll_y));

        // Settled: later polls see the stamp the reload recorded and do
        // nothing (in particular, no reload loop).
        ed.TickPdfAutoReload(11.0);
        ed.TickPdfAutoReload(11.5);
        EXPECT(sess->doc->PageCount() == 5 && sess->page == 2, "a re-read file is not re-read again",
               "page " + std::to_string(sess->page));

        // A file that keeps changing (the compiler still writing) keeps
        // restarting the wait: three different stamps 0.3 s apart never
        // trigger a decode, the still one after them does.
        sess->page = 4;
        WriteWholeFile(path, MinimalPdf(1));
        ed.TickPdfAutoReload(20.0);
        WriteWholeFile(path, MinimalPdf(2));
        ed.TickPdfAutoReload(20.3);
        WriteWholeFile(path, MinimalPdf(4));
        ed.TickPdfAutoReload(20.6);
        EXPECT(sess->doc->PageCount() == 5, "a file still being written is not decoded",
               std::to_string(sess->doc->PageCount()) + " pages");
        ed.TickPdfAutoReload(20.9);
        ed.TickPdfAutoReload(21.2);
        EXPECT(sess->doc->PageCount() == 4, "the final write is picked up once it settles",
               std::to_string(sess->doc->PageCount()) + " pages");
        EXPECT(sess->page == 3, "a page past the new end is clamped to the last page",
               "page " + std::to_string(sess->page));

        // The ticks are throttled: two polls within the interval are one.
        WriteWholeFile(path, MinimalPdf(6));
        ed.TickPdfAutoReload(30.0);
        ed.TickPdfAutoReload(30.1);  // same poll window: not a second sighting
        ed.TickPdfAutoReload(30.2);
        EXPECT(sess->doc->PageCount() == 4, "polls inside the interval do not advance the wait",
               std::to_string(sess->doc->PageCount()) + " pages");
        ed.TickPdfAutoReload(30.5);
        EXPECT(sess->doc->PageCount() == 6, "and the settled file is read after the interval",
               std::to_string(sess->doc->PageCount()) + " pages");

        // `:set nopdfautoreload` switches the whole thing off.
        ed.RunCommand("set nopdfautoreload");
        EXPECT(!ed.PdfAutoReload(), "the option turned off", "it is still on");
        WriteWholeFile(path, MinimalPdf(2));
        ed.TickPdfAutoReload(40.0);
        ed.TickPdfAutoReload(41.0);
        EXPECT(sess->doc->PageCount() == 6, "with the option off the file is not followed",
               std::to_string(sess->doc->PageCount()) + " pages");
        ed.RunCommand("set pdfautoreload");
        ed.TickPdfAutoReload(42.0);
        ed.TickPdfAutoReload(43.0);
        EXPECT(sess->doc->PageCount() == 2, "turning it back on picks the change up",
               std::to_string(sess->doc->PageCount()) + " pages");

        // Deleted out from under the viewer (a clean before a rebuild):
        // the stale document stays up, and the rebuilt file is followed.
        std::filesystem::remove(path, ec);
        ed.TickPdfAutoReload(50.0);
        ed.TickPdfAutoReload(51.0);
        EXPECT(sess->doc->PageCount() == 2, "a missing file leaves the document up",
               std::to_string(sess->doc->PageCount()) + " pages");
        WriteWholeFile(path, MinimalPdf(3));
        ed.TickPdfAutoReload(52.0);
        ed.TickPdfAutoReload(53.0);
        EXPECT(sess->doc->PageCount() == 3, "the rebuilt file is followed",
               std::to_string(sess->doc->PageCount()) + " pages");
    }

    std::printf("\n---- %d checks, %d failures ----\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
