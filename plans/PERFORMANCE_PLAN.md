# Runtime performance: benchmark suite, analysis, plan

The user asked for a benchmarking suite for mep and all of its
supporting code that can run one benchmark or all of them, to be used as
the test harness for performance work, and, before any optimization, an
analysis of where the time goes and a plan for fixing it. This document
is that analysis and plan. Its sections:

1. the suite: what it measures and how to use it;
2. the baseline: what it measured on 2026-10-16;
3. the analysis: why;
4. the plan;
5. a progress log at the end.

(Earlier, narrower plans this builds on: CRDT_PERFORMANCE_PLAN.md's
first bench binaries and `bench_results/history.jsonl`, and
MEPML_PERFORMANCE_PLAN.md's mepml benches. Neither file survives in
`plans/`, but both are referenced from the code they produced.
BUILD_PERFORMANCE_PLAN.md is about compile time, not runtime.)

---

## 1. The benchmark suite

### Shape

Three tiers, one runner (`tools/bench.py`), one history file:

| tier | what | where |
|---|---|---|
| headless libraries | the editor core (a real windowless `Editor` driven by `:normal`/ex commands), tree-sitter highlight/folds, the regex engine, inflate/deflate, PNG/JPEG, JSON, spell, project search, in-house git, Org/HTML/SVG | `mep-bench` (`src/bench/*.cpp`) |
| headless, older | buffer shape, CRDT, Lua frame-hook dispatch, terminal emulator, PDF, the mepml toolchain | `mep-buffer-bench`, `mep-crdt-bench`, `mep-lua-frame-hook-bench`, `mep-vterm-bench`, `mep-pdf-bench`, `mep-mepml-bench` |
| live editor | a throwaway real mep per fixture: startup, `:e`, the per-edit hooks called directly, per-frame CPU work while idle, scrolling, holding `j` and typing, and memory | `tools/bench_live.py` + `tools/bench_live.lua` |

All the binaries share one harness, `src/bench_util.h`, with one command line:

- `--list` prints the benchmark ids.
- `--filter PAT` selects benchmarks.
- `--quick` runs a faster, less precise pass.
- `--reps N` and `--min-time MS` control repetition.
- `--no-record` skips appending to the history file.
- `--json PATH` also writes the results to PATH.

A measurement does one untimed warm-up and then repeats until it has at least `--reps` runs and `--min-time` of total time. It records the median, with the min, p95 and repetition count alongside.

New benchmarks are one function each:

```cpp
MEP_BENCH("editor/cpp_60k") {
    mep::bench::Measure("substitute_global", n, [&] { ... });
}
```

### Ids and patterns

Every benchmark has an **entry** id, `group/name` (`editor/cpp_60k`), and every
measurement in it a full id, `editor/cpp_60k/substitute_global`. A pattern is
a glob (`*`, `?`); one without glob characters is a prefix. So `editor`
is the whole editor group, `editor/cpp_60k/sub` one measurement, and
`'syntax/*/highlight_rows*'` one measurement across every language.

### Using it

```sh
just bench                               # everything (~10 min; opens mep windows briefly on macOS)
just bench editor/cpp_60k                # one entry
just bench 'syntax/*/highlight_rows*'    # one measurement across entries
just bench core --quick --no-record      # a fast look, nothing recorded
just bench --no-live                     # skip the windowed tier
just bench live --rounds 3               # live tier, median of 3 fresh mep instances
just bench-list [PAT]                    # what exists

# the loop for a performance change:
just bench-save before editor syntax     # before the change: save a named baseline
# ...change code...
just bench-compare before editor syntax  # after: compare against it (faster/SLOWER flagged at >5%)
just bench-report [PAT]                  # latest vs previous record of everything in history

# on a busy machine: A/B two builds, interleaved round by round
just bench-ab HEAD editor syntax         # working tree vs HEAD (built in build/bench-ab/<sha>)
python3 tools/bench.py ab --before-dir OTHER/build --rounds 3 live
```

`ab` is the trustworthy comparison when anything else is using the CPU.
With `--no-build` it measures whatever binaries are on disk. `just test`
does not rebuild `mep-bench`, so after a test run use the default (which
builds) or build the bench targets first. One such stale run looked like a
±20% swing between two builds of nearly the same code.
Both sides run under the same load, alternating which goes first, and the
medians over rounds are compared. While this plan was written, an
unrelated mep instance and an LSP server were each using a full core,
which moved sequential runs by 10-20%. `ab` cancelled that out.

Results append to `bench_results/history.jsonl` (git-tracked, never
rotated: the historical record across commits; each record carries its
commit, with `+dirty` for uncommitted trees). Named baselines are in
`bench_results/baselines/`. Re-saving a name with a subset replaces only
those results.

Profiling a live scenario:

```sh
python3 tools/bench_live.py --profile cpp_60k:typing --seconds 8
#   scenarios: idle, scroll, cursor_down, typing, highlight
```

This opens the fixture and repeats the scenario every frame while macOS
`sample` (Linux: `perf`) records. The heaviest self-time functions are
printed, and the full call tree is written to `bench_results/profiles/`
(gitignored). Every finding below marked *(profiled)* came from this.

### Measuring a frame

Frame intervals measure the display's vsync and mep's idle throttle, not
mep. So the live tier reads a new `mep.frame_stats()`: `Editor::FrameStats`, filled by `UpdateDrawFrame` in main.cpp. It reports:

- **work**: the whole frame minus the buffer swap;
- **hooks**: `RunFrameHooks`;
- **draw**: `DrawEditor` minus the swap.

Frame scenarios report the **mean** work per frame. Hooks fire on throttles, so a median either catches them or doesn't depending on the frame rate. The median, p95 and max are kept alongside.

### Noise, and what to trust

- **Headless tier:** repeatable to a few percent. Trust 5% differences.
- **Live tier:** frame scenarios are within ~3% run-to-run when nothing in the editor is racing. The directly-called hooks wander ±20%, and so does `frame_typing`: an LSP server analysing the fixture on another core competes with mep for CPU and for its stdin pipe (finding E1). For live changes under ~25%, use `--rounds 3`.
- **macOS window throttling:** a benchmark window behind the terminal would be App-Napped onto efficiency cores, with timers coalesced. That produced 20-500% swings between identical runs. The driver sets `MEP_BENCH_NO_THROTTLE=1`, which makes `backend_native_macos.mm` hold an `NSActivityLatencyCritical` activity. The variable is not set in normal use.
- **Memory:** `memory_mb` is the macOS physical footprint (GPU memory included) and Linux VmRSS. It is not `ps` RSS, which counts ~0.9 GB of shared system libraries.

---

## 2. Baseline (2026-10-16, Apple M5 10-core, 16 GB, commit 1ad53f5+dirty, `-O3`)

Saved as `bench_results/baselines/baseline.jsonl`. These are the numbers the plan's targets refer to.

### The editor as a person feels it (live tier)

| scenario | cpp 2k lines | cpp 60k | py 20k | md 20k | org 20k |
|---|---|---|---|---|---|
| idle frame, CPU ms | 2.3 | **13.0** | 6.5 | 2.0 | **35-44** |
| holding `j`, ms/frame | 2.5 | **14-21** | 6.5 | **37-56** | **72-85** |
| scrolling, ms/frame | 2.5 | 14-21 | 6.9 | 29-36 | 72-85 |
| typing, ms/frame (mean) | 0.6 | **17-176** (bimodal, E1) | 3.1 | 12-67 | **92-117** |
| `mep.syntax_highlight()` on unchanged text | 0.4 | 15-20 | 6.2 | **275-346** | 12.6 |
| `mep.syntax_highlight()` after a one-line edit | 18 | **55-76** | 9.6 | **279-348** | 15 |
| one `mep.set_line` | 0.03 | **0.84-1.07** | 0.24 | 0.23 | 0.17 |
| memory (footprint) | **559 MB** | **1.43 GB** | 872 MB | 791 MB | 706 MB |

Startup to agent socket answering: **580-650 ms**.

A frame budget at 120 Hz is 8.3 ms. With a 60k-line file open, mep cannot
hold it even when idle. In a 20k-line Org file, mep spends 35 ms of CPU per
frame while doing nothing at all.

### Headless (selected; full set in the baseline file)

| benchmark | baseline | note |
|---|---|---|
| `editor/cpp_60k/search_miss` (`/` or `:s` over 60k lines, no match) | 81 ms | ~37 MB/s; `memmem` would be <1 ms |
| `editor/cpp_60k/substitute_global` | 88 ms | |
| `editor/cpp_60k/lua_set_line_x100` | 133 ms | 1.3 ms per `set_line`: a full undo snapshot each |
| `editor/cpp_60k/normal_x_then_undo` | 3.0 ms | 2 full-buffer copies |
| `core/regex/whole_text_literal` (1.8 MB) | 54 ms | literal search at 34 MB/s |
| `core/regex/whole_text_alternation` | 264 ms | |
| `project/search/regex_no_literal` (src+tools+docs+plans) | 751 ms | vs ~23 ms when a literal prefilter applies |
| `syntax/md/highlight_rows_unchanged` (20k lines) | **259 ms** | should be ~0 (C++ unchanged: 0.3 ms) |
| `syntax/cpp/highlight_rows_keystroke` (40k lines) | 20 ms | |
| `syntax/cpp/fold_ranges` (40k) | 47 ms | |
| `core/image/png_encode_1080p` | 257 ms | |
| `core/image/png_decode_1080p` | 34 ms | |
| `core/spell/suggest_x20` | 107 ms | 5 ms per suggestion |
| `docs/svg/raster_2000_shapes` | 291 ms | |
| `vterm/scroll_flood_yes` (4 MB of `y\n`) | 389 ms | other vterm shapes ~40 ms |
| `project/git/status` (this repo) | 27 ms | |

---

## 3. Analysis

Sources:
- the live and headless measurements above;
- `sample` profiles from `bench_live.py --profile`;
- three read-only code audits (editor core, render loop, supporting
  libraries), every claim checked against the code. Line numbers are as
  of commit 1ad53f5.

Findings are grouped by where the cost lands; each has an id used in the plan.

### E. Editing: what a keystroke costs

- **E1. The LSP `didChange` blocks the UI thread** *(profiled, the largest typing cost)*.
  - **The write:** `mep.lsp_notify` → `JobManager::WriteStdin` → `Job::WriteStdin` (job.cpp:361-370) loops on a blocking `write()`. The pipe is never set to `O_NONBLOCK`.
  - **The payload:** the full document text (main.cpp:7911-7922), built in Lua line by line.
  - **What happens:** once the server is busy analysing and the 16-64 KB pipe is full, mep waits on it.
  - **Profile:** typing in the 60k-line file, ~3,800 of ~4,400 hook samples sat in `WriteStdin`.
  - **Why it's bimodal:** whether a run hits this depends on whether the server is keeping up. That is the 17 ms vs 176 ms per frame split.
  - **Related costs:**
    - semantic tokens and signature help ride on every change (main.cpp:7931, 7941);
    - completion sends its own `didChange`, bypassing the 0.3 s throttle (main.cpp:11415, 11597);
    - the servers never drop superseded versions (rpc_framing.h:55-73);
    - they rebuild their analyses per request (c_lsp_server.cpp:200-208, python_lsp_server.cpp:540-549).
- **E2. Every edit hook rebuilds the buffer as Lua strings.**
  - **The pattern:** 42 Lua loops in main.cpp do `for i=1,mep.line_count() do lines[i]=mep.get_line(i) end`, usually followed by `table.concat`. Each `get_line` copies twice (`GetLineForLua` by value at editor.cpp:34429, then `lua_pushlstring`). Examples: syntax at main.cpp:14080, LSP at 7917, spell at 14321, folds at 14170.
  - **Measured:** `lua_get_all_lines` costs 6-8 ms on 60k lines, per hook, per edit, and the garbage is a GC cost later.
- **E3. The syntax hook re-does the whole document per edit, around an incremental core.**
  - **The core is cheap:** `TreesitterHighlightRows` is incremental (20 ms per keystroke at 40k lines of C++, 2.5 ms for Python).
  - **The wrapping is not:** `mep.syntax_highlight` (main.cpp:14074-14150) builds the full text in Lua and `ns_clear`s the namespace, then `l_ts_highlight` re-adds *every* decoration in the file (lua_env.cpp:5438-5460). That is why the after-edit hook is 55-76 ms against the core's 20 ms.
  - **Hidden cost:** even unchanged text costs 15-20 ms, all of it decoration churn.
  - **The edit is guessed:** it is found by diffing two full strings (`ComputeEdit`, treesitter.cpp:809-827), several full-text copies and compares per pass. The primitives that know what changed never report it.
  - **No per-buffer cache key:** the main path passes none (main.cpp:14140), so two buffers of one language evict each other's tree.
  - **Parser churn:** `ts_parser_new`/`_delete` run per call (treesitter.cpp:865, 924).
- **E4. Markdown highlighting is never incremental and never cached** *(measured: 259 ms on unchanged text)*.
  - **No cache check:** `TreesitterHighlightRows`' no-query fallback (treesitter.cpp:1232-1238) is taken for md. It does not check `st.text == text`, so every call is a full block-grammar pass.
  - **Inline re-parse:** that pass also does a from-scratch inline parse of every `(inline)` node (`CollectMarkdownInlineSpans`, treesitter.cpp:975-1000).
- **E5. Undo snapshots the whole buffer.**
  - **On every edit:** `PushUndo` (editor.cpp:33386) copies all lines plus folds on every normal-mode edit and every Lua `set_line` (editor.cpp:34436). It keeps up to `kMaxUndo` = 200 snapshots.
  - **Undo/redo:** both copy where they could move (editor.cpp:33399, 33424).
  - **Measured:** a `set_line` on 60k lines costs ~1 ms, and 100 of them cost 133 ms.
  - **Memory:** a 3 MB file can hold ~1 GB of snapshots, which is part of the 1.43 GB.
- **E6. Batch edits are quadratic.** `:g/pat/d` runs one `:d` and one `PushUndo` per match (editor.cpp:33598-33605). A Lua `set_line` loop pushes once per call.
- **E7. Search and substitute have no literal fast path** *(measured: 81 ms per miss over 60k lines; regex literal search at 34 MB/s)*.
  - **The engine:** a recursive backtracker with `std::function` continuations (regex.cpp:557, 715, 726). It copies the groups vector per quantifier step (regex.cpp:650, 728) and builds a new `Matcher` per start position (regex.cpp:805-807).
  - **No prefilter in the editor:** `SearchOnce`/`ExSubstitute` (editor.cpp:31596, 33525) try every byte. `project_search.cpp` has a literal prefilter (`RequiredLiterals`, :432-436); the editor does not use it.
  - **Stack overflow:** recursion depth follows subject length, so `/.*x` on a 1 MB minified line can overflow the stack (project_search caps lines at 2,000 bytes for this reason, :371-383).
- **E8. Newline and paste cost O(lines × (N + D)).**
  - **Per line:** `InsertNewline` does a vector insert, then `ShiftDecorationsForLineEdit` walks every decoration (editor.cpp:32484-32509).
  - **Pastes:** typed pastes replay one codepoint at a time (`InsertTextAsTyped`, editor.cpp:24123+).
- **E9. The mepml parse cache is validated by comparing the whole buffer** (`plain.lines == buf.lines`, editor_mepml.cpp:759, 775, 1051-1062). It then copies the whole buffer into the cache and into the async worker.

### R. Rendering: what an idle or scrolling frame costs

- **R1. Scope guides re-lex the whole buffer every frame, for every pane** *(profiled: the top self-time function idle, scrolling and typing in C++/Python)*.
  - **The re-lex:** `FindScopeGuides(buf.lines, …)` runs unconditionally in DrawPane (main.cpp:56356).
  - **The draw loop:** `main.cpp:57832` then walks all guides for every visible row.
  - **Scaling:** this is why the idle frame grows with file size (2.3 ms at 2k lines, 13 ms at 60k).
- **R2. Every decoration is walked every frame, for every pane** (main.cpp:57137-57151).
  - **Why it costs:** a `Decoration` holds 8 `std::string`s, about 300 B (editor.h:521-580).
  - **Scale:** highlighting a 60k-line file makes hundreds of thousands of them, so this is ~100 MB of mostly cache-missing reads per frame.
  - **Memory:** they are a large part of the footprint.
- **R3. The Org/Markdown style lookup re-reads sheet files from disk on every call** *(profiled: the top self-time function in Org and Markdown, and the cause of their idle/cursor-move cost)*.
  - **The chain:** `OrgHighlightEmphasis` → `DocSheetStyle` → `MepmlSheets` → `MepmlSheetFile` (editor_mepml.cpp:345, 839-880).
  - **Per call:** a `stat`, an `fopen` (`std::ifstream`), `getrlimit` and two `getenv`s, once per styled span.
  - **The usual case is the worst:** a *missing* sheet is never cached (`e.sheet` stays null, so every call retries the read), and the per-user `~/.config/mep/mepml.mepss` is usually missing.
  - **Who calls it:** the cursor-row hooks, on every cursor move. `MepmlStylesStale` already throttles the same check to every 300 ms; `DocSheetStyle` does not.
- **R4. Syntax colour is drawn as overdraw, per glyph, on the slow text path.**
  - **The overdraw:** each row is drawn in `Normal`, then every highlighted span again, one glyph at a time (main.cpp:59251, 59380-59452).
  - **Per glyph:** a `std::string` allocation, then `DrawTextEx`, which runs `strlen` and a *linear* `GetGlyphIndex` scan twice (backend_native_text.cpp:266, 333-347).
  - **Profile:** `GetGlyphIndex` shows in every profile.
- **R5. One heap allocation and one `sin`/`cos` per quad.** `DrawTexturePro` builds a `std::vector<Vtx>` per call (backend_native_renderer2d.cpp:672; the same pattern in the rectangle primitives at :340-352). That is ~5-15k mallocs per frame.
- **R6. Org/Markdown/notebook structure is re-parsed per frame.**
  - **Org/Markdown:** `OrgTables` (editor.cpp:36779) and `OrgBlockCards` (editor.cpp:2110) re-parse per frame. `OrgBlockCards` is called twice per frame and copies its result on the mepml path (main.cpp:56822, 56827).
  - **Notebooks:** `NotebookRefresh` rescans per frame (editor.cpp:39665).
- **R7. Idle is a full redraw.**
  - **The loop:** with no dirty tracking, every iteration draws everything (main.cpp:65986). Idle throttling only lowers the rate (10 fps, then 2 fps).
  - **Lua hooks:** ~35 permanent `mep.on_frame` hooks run each frame, one doing `os.getenv('PATH')` (main.cpp:33899).
  - **macOS wake-ups:** `WaitEvents` wakes every 10 ms to `select()` the wake fd (backend_native_macos.mm:711-728).
- **R8. Wrapped long lines draw every slice, even off-screen** (main.cpp:59253-59270). A 1 MB minified line becomes ~1M quads.

### M. Memory

- **M1. Idle footprint.** An empty editor holds 559 MB, which is mostly not text:
  - ~300 MB of GPU memory (`IOAccelerator (graphics)`, mostly font atlases at several sizes and fonts: emoji, icon, office, maths);
  - ~110 MB of malloc heap.
- **M2. Per-file cost.** A 3 MB file adds ~0.9 GB:
  - undo snapshots (E5);
  - ~300 B decorations by the hundred thousand (R2);
  - tree-sitter trees and row-span caches, which keep a `std::string` capture name per span (treesitter.h:20-25);
  - garbage from E2's Lua string arrays.

### S. Supporting libraries (throughput; interactive where noted)

- **S1. Regex engine** (E7): literal search at 34 MB/s, alternation at 7 MB/s. In project search, a pattern without a literal is 33× slower (751 ms vs 23 ms).
- **S2. PNG encode** costs 257 ms for 1080p (fixed-Huffman deflate, per-byte appends). Decode is 34 ms.
- **S3. Inflate.**
  - **Bit reader:** `PeekBits` rebuilds 3 bytes per symbol (deflate.cpp:171-176).
  - **Copies:** match copies `push_back` per byte (deflate.cpp:281).
  - **Fixed tables:** rebuilt per fixed block (deflate.cpp:348-353).
  - **No size hints:** callers never pass the output size.
  - **Measured:** ~2× slower than zlib, in a scratch harness.
  - **Who pays:** PNG, PDF, git and zip.
- **S4. JPEG decode.**
  - **Huffman:** decoded bit at a time (jpeg_codec.cpp:101-145).
  - **IDCT:** a naive float 8×8 matrix multiply (jpeg_codec.cpp:196-225), whose static init flag races across threads.
  - **Per pixel:** `lround` per sample, and colour conversion with integer divides (jpeg_codec.cpp:873-905).
- **S5. SHA-1** is scalar at 400 MB/s, against 2.6 GB/s for CommonCrypto (sha1.cpp:14-48).
  - **Who pays:** git status, add and commit, plus the index checksum on every `Index::Load`.
- **S6. Git status.**
  - **Cold repo:** a fresh `Repo`/`Odb` per command (cli.cpp:994).
  - **No cache-tree:** `DiffTreeIndex` flattens the whole HEAD tree per refresh (status.cpp:415-430). The index TREE extension is neither read nor written, and `Index::Save` drops all extensions, which also costs the real git its caches.
  - **Ignore checks:** `IsIgnored` re-matches every ancestor of every path (status.cpp:105-129).
- **S7. Spell suggestions** take 5 ms each (spell.cpp:249-262): every candidate is lowercased and Soundex'd per call.
- **S8. The HTML pane re-lays out every frame** (main.cpp:54002) and restyles in O(elements × rules) (html_doc.cpp:2286-2291).
  - **Browser fetches:** `:Browser` fetches resources one after another by spawning `curl`, with no cache (http_client.cpp:200-245, html_doc.cpp:682-692).
- **S9. PDF re-decodes images and Form XObjects on every render** (pdf_content.cpp:1856, 1954-1980).
  - **Text extraction:** it gets no `RenderCache` (pdf_text.cpp:104, 177).
  - **Object resolution:** objects are re-parsed per lookup (pdf_xref.cpp:372-397).
- **S10. The JS engine copies strings.**
  - **Strings:** each string member access copies the whole string (js_engine.cpp:4928).
  - **Arrays:** stored as `to_string(i)`-keyed hash maps (js_engine.cpp:734, 5404).
  - **Map/Set:** linear search (js_engine.cpp:945).
- **S11. Smaller items:**
  - **Job output:** the reader is quadratic on long lines (job.cpp:451-461).
  - **Terminal scrolling:** `vterm` scroll-flood is 389 ms for 4 MB of `y\n` (10× the other shapes).
  - **JSON:** objects use linear key lookup, and `%g` numbers lose precision. **That is a correctness bug, not only speed:** `%.17g` is needed (json.h:247).
  - **Zip:** `zip::Extract` re-parses the central directory per entry (PPTX does it ~6× per slide).
  - **FEM:** factorization redoes the symbolic phase every Newton and time step (num_sparse.cpp:640-673).
  - **TeX:** builds its `Resolver` twice per compile (tex_compile.cpp:257, 358). **Possible correctness bug:** `ExeIdentity` reads `/proc/self/exe`, which macOS lacks, so a rebuilt engine may reuse a stale cached format.

### Checked and fine

These looked suspicious and are not:
- no `std::regex` in any render path;
- glyphs are baked once, not re-rasterized;
- GL draws are batched per texture;
- inflate already has a fast table;
- the packfile index uses mmap and binary search;
- zstd, AAC and CAVLC are table-driven;
- Myers diff is capped;
- the sparse solver's assembly and CG are sound;
- tree-sitter `#match?` caches its compiled regex;
- the mepml toolchain (parse 5.6 ms for 16k lines, all exporters ≤ 40 ms) is in good shape after the MEPML_PERFORMANCE work.

---

## 4. Plan

### Principles

- **Measure first, every change:** `just bench-save before <groups>`, then change, then `just bench-compare before <groups>`, with `just test` in between. A change ships with its numbers in the progress log below.
- **Benchmark first:** if no benchmark sees the problem, add one before the fix.
- **One finding per commit,** so a regression bisects to one idea.
- **No behaviour changes hidden in performance work.** Where a fix changes what the user sees (e.g. R4's single-pass colouring), check it visually with the agent-socket screenshot workflow.

### Phase 1: quick, low-risk wins (each a small local change)

Ordered by measured user impact over effort.

Status: Phase 1 done (2026-10-16, see the progress log). Its exit
criteria are met for idle frames in all four fixtures and for C++ typing.
Org cursor moves (48 ms) and Markdown typing (65 ms) are not; they move
to Phase 2 (2.2, 2.6) and Phase 3 (R2, 3.3), where the remaining costs are.

| # | change | finding | benchmark that must move | target |
|---|---|---|---|---|
| 1.1 | Cache sheet files, misses included: keep a "checked at" time per path and re-`stat` at most every 300 ms; hoist `getenv` | R3 | `live/org_20k/frame_*`, `live/md_20k/frame_cursor_down` | org idle < 10 ms, md cursor move < 5 ms |
| 1.2 | `TreesitterHighlightRows`' fallback path: return the cached rows when `st.text == text` | E4 | `syntax/md/highlight_rows_unchanged`, `live/md_20k/hook_*` | unchanged: < 1 ms (was 259) |
| 1.3 | Cache `FindScopeGuides` per buffer on (change epoch, filetype); visit only guides overlapping the visible rows | R1 | `live/cpp_60k/frame_idle`, `frame_scroll` | idle at 60k lines within 2× of 2k lines |
| 1.4 | Non-blocking LSP stdin: set `O_NONBLOCK`, queue unwritten bytes, flush from `PollAll` | E1 | `live/cpp_60k/frame_typing` (and its p95) | no frame blocked on a pipe; typing mean < 20 ms, steady across runs |
| 1.5 | Literal fast path in `SearchOnce`/`ExSubstitute`/`ExGlobal`: a regex without metacharacters (or a required literal) goes through `memmem`/`CiFind` first | E7 | `editor/*/search_miss`, `editor/*/substitute_global` | 60k-line miss < 5 ms (was 81) |
| 1.6 | `std::move` instead of copies in `Undo`/`Redo` | E5 | `editor/*/normal_x_then_undo` | -30% |
| 1.7 | Per-buffer cache key for `mep.syntax_highlight` (`'buf:'..id`) | E3 | switching buffers re-highlights incrementally | no full reparse on buffer switch |
| 1.8 | O(1) `GetGlyphIndex` (ASCII table plus a hash map), as `g_glyph_index` already does for `g_font` | R4 | `live/*/frame_*` draw split | -10% draw |
| 1.9 | `DrawTexturePro`/rectangles append straight into the batch; skip `sin`/`cos` when rotation is 0 | R5 | draw split; malloc count | -10% draw |
| 1.10 | `OrgBlockCards` called once per frame, returning a const ref; `OrgTables` cached on the change epoch | R6 | `live/org_20k/frame_idle` | together with 1.1, org idle < 5 ms |
| 1.11 | Spell: precompute lowercase/Soundex per dictionary word at load | S7 | `core/spell/suggest_x20` | < 20 ms |
| 1.12 | `json.h`: print with `%.17g` (correctness) | S11 | tests | - |

**Exit:**
- org/md/cpp idle frames under the 8.3 ms budget at the fixture sizes;
- typing in the 60k-line C++ fixture never blocks;
- `just test` green.

### Phase 2: the edit pipeline (structural; medium risk)

A buffer that knows what changed, so nothing downstream has to guess.

Status (2026-10-16): done, with three changes from the plan below. All are
in the progress log.
- **2.1 is replaced by diffing.** `Buffer::lines` is mutated directly at
  96 sites, so an edit log fed by the primitives would be wrong the first
  time one site forgot. A common-prefix/suffix diff of two line vectors is
  cheap (60k lines: ~0.3 ms) and sees every mutation, so undo (2.5) uses
  that. Tree-sitter already diffs text (`ComputeEdit`): measured at ~6% of
  a pass, it is not worth replacing.
- **2.4 is deferred.** Once the stdin write stopped blocking (1.4) and the
  text is built in C++ (2.3), the whole `didChange` path profiled at ~3%
  of typing in the 60k-line file. Incremental sync would need range
  application in all six in-house servers for that 3%; revisit if an LSP
  replay benchmark says otherwise.
- **New 2.7: off-thread highlighting.** After 2.2 the remaining typing
  cost was tree-sitter's own reparse. On code the grammar cannot parse
  (this repo's `editor.cpp` has 133 error nodes, from JavaScript inside
  `EM_JS` macros), error recovery makes an *incremental* reparse linear in
  the file: 28 ms at 3.7 MB, against ~2 ms for error-free C++ of the same
  size. Any slow key now parses on a background thread.

- **2.1 Edit log on `Buffer`.**
  - **What:** a monotonically increasing `version`, plus a list of `{start_row, old_rows, new_rows}` (and byte/point deltas for tree-sitter) appended by every primitive (`InsertChar`, `InsertNewline`, `Backspace`, `ReplaceLines`, `ReplaceLinesForLua`, paste, `:s`, undo/redo).
  - **What it replaces:** the 40 `plain.lines == buf.lines` validations (E9), with a version check.
- **2.2 Tree-sitter driven by the log** (E3).
  - **Edits:** `ts_tree_edit` from the recorded edits, not from `ComputeEdit`'s string diff.
  - **Text:** a `TSInput` callback reading `Buf().lines` directly (no joined copy), and one `TSParser` per cache entry.
  - **Lua API:** a `mep.ts_highlight_buffer(ns, ft)` that reads the buffer in C++. It replaces only the decorations of rows the tree reports as changed (`ts_tree_get_changed_ranges`), instead of `ns_clear` + re-add all.
  - **Target:** `live/cpp_60k/hook_syntax_highlight_after_edit` under 5 ms (was 55-76).
- **2.3 Bulk buffer access for Lua** (E2). Add `mep.buffer_text()` (one `luaL_Buffer`), `mep.get_lines(a, b)`, and `mep.buffer_version()`. Port the 42 whole-buffer loops. The ones that only need to know *whether* something changed become version checks.
  - **Target:** no hook spends more than 1 ms per edit on a 60k-line buffer just reading it.
- **2.4 Incremental LSP sync** (E1). Advertise and send `TextDocumentContentChangeEvent` ranges from the edit log.
  - **Servers:** the in-house servers accept them, and drain their input queue, keeping only the latest `didChange` per URI before analysing.
  - **Requests:** semantic tokens and signature help move off the per-keystroke path, onto the existing debounce.
- **2.5 Delta undo** (E5, E6, M2). Undo entries become the 2.1 edit records (removed and inserted lines), not snapshots.
  - **Folds:** they keep their exact restore through a fold snapshot taken only when a fold boundary is touched (the reason `UndoState` carries folds).
  - **Batches:** `:g` and Lua batches group under one undo step.
  - **Target:** `lua_set_line_x100` at 60k lines under 2 ms (was 133); `live/cpp_60k/memory_mb` down by the snapshot share.
- **2.6 Markdown incremental highlighting** (E4). Keep per-`(inline)`-node results keyed by byte range and content hash. Re-parse only the inline nodes in changed ranges.
  - **Target:** `syntax/md/highlight_rows_keystroke` under 10 ms (was 262).

### Phase 3: the frame (structural; medium risk)

Status (2026-10-16): done as far as measurement justified. The rest is
recorded, with the numbers behind each decision, in the progress log.
- **3.1 and 3.2 are not done.** They were planned against a 13 ms idle
  frame at 60k lines, which turned out to be mostly scope guides (1.3) and
  sheet I/O (1.1). After Phases 1-2 that frame is ~1.5 ms in total. A
  decoration index and single-pass text would rewrite every draw path for
  a fraction of a millisecond, and 3.2 changes glyph rendering.
  Revisit if a fixture shows draw time over budget.
- **3.3 is partly done.** Idle CPU was mostly wake-ups, not frames, so the
  10 ms wake slices became event-driven on macOS (idle CPU roughly
  halved). The per-cursor-move Org hooks became row-scoped. Skipping
  unchanged frames is not done: at 2 fps idle the frames are ~2-3 ms/s of
  the remaining ~7-11 ms/s, and a stale screen is a visible bug.
- **3.4 is done.** A 1 MB line went from never drawing to ~15 ms frames.
- **3.5 is partly done.** Startup lost a 260 ms window animation, and font
  atlases went single-channel. Lazy atlas baking is not done.

- **3.1 Row-indexed decorations** (R2, E8, M2).
  - **Storage:** per namespace, a vector sorted by row, or a row-bucketed vector, with the hot fields (row, columns, interned highlight id, flags) split from the cold strings.
  - **Lookups:** DrawPane does a `lower_bound` to the visible rows.
  - **Edits:** `ShiftDecorationsForLineEdit` becomes one offset adjustment per bucket boundary.
  - **Highlight names:** interned to ids at `AddDecoration` (R4's `ResolveHlGroup` string hashing per span goes too).
- **3.2 Single-pass coloured text** (R4). Build a per-column colour run list for each visible row from the row's decorations (priority order), then emit each glyph once through `DrawLineFast` with per-run tint. No overdraw, no per-glyph allocation.
  - **Visual check required:** antialiased edges get *thinner* (no double blending), so compare screenshots.
- **3.3 Skip the frame when nothing changed** (R7).
  - **Draw:** no events, no activity-epoch change, no buffer/theme/cursor/scroll change, and no caret-blink edge means no draw and no swap.
  - **Hooks:** the filename- and cursor-keyed `on_frame` hooks become `on_buf_enter`/`on_cursor_moved` events.
  - **macOS:** the 10 ms wake slices become a `dispatch_source` on the wake fd.
  - **Measure:** idle CPU% and wake-ups/s. Add a `live/*/idle_cpu_pct` scenario: process CPU time over 10 s idle.
- **3.4 Cull off-screen wrap slices and off-pane glyphs** (R8). Add a `live/longline_1mb` fixture.
- **3.5 Memory: font atlases** (M1).
  - **Lazy:** bake the emoji, icon, office and maths atlases on first use, not at startup.
  - **Smaller:** use a single-channel (R8) texture format for glyph alpha where it is RGBA now.
  - **Target:** idle footprint under 250 MB.
  - **Also:** move startup work (font bakes, theme, builtin Lua chunks) off the critical path toward a first frame under 300 ms. Measure it with a new `live/startup/to_first_frame` (`frame_stats().frames > 0`).

### Phase 4: libraries (independent; each low-medium risk)

Each is self-contained, so these can interleave with Phases 2-3:

- **4.1 Regex** (S1, E7).
  - **Search entry:** a required-literal prefilter in `Regex::Search` itself, so every caller gets it.
  - **Engine:** save/restore groups only when the pattern has groups, and template the continuations instead of `std::function`.
  - **Longer term:** a Pike VM / lazy DFA for patterns without backreferences (also removes the stack-depth hazard).
  - **Targets:** `core/regex/whole_text_*` ≥ 5×; `project/search/regex_no_literal` < 150 ms.
- **4.2 Inflate** (S3). A 64-bit bit buffer, `memcpy` match copies, static fixed tables, an output-size hint from PNG/git/zip callers. Target 2×.
- **4.3 PNG encode** (S2). Dynamic-Huffman or stored+fast deflate, and filter selection without per-byte appends. Target < 60 ms at 1080p.
- **4.4 JPEG** (S4). A table-driven Huffman decoder, AAN or integer IDCT, fixed-point colour conversion (fixes the IDCT init race). Target 3×.
- **4.5 SHA-1** (S5). CommonCrypto on Apple, ARMv8/SHA-NI intrinsics elsewhere. Add a `core/sha1` bench and a `project/git/status_after_touch` bench (1,000 stat-dirty files).
- **4.6 Git** (S6). Read and write the index TREE extension, skip unchanged subtrees in `DiffTreeIndex`, and keep a process-wide `Repo`/`Odb` cache keyed by git dir.
- **4.7 PDF** (S9). An image/form cache in `RenderCache` with a byte budget, the render cache passed to text extraction, object memoization. Add an image-heavy fixture to `pdf-fixtures`.
- **4.8 HTML/Browser** (S8). Cache the layout per restyle generation and viewport, index rules by rightmost compound, fetch resources in parallel off-thread with a URL cache.
- **4.9 JS engine** (S10). Shared string payloads, dense arrays, hashed Map/Set. Add `core/js` benches (fib, a `charCodeAt` loop, `Map.set` ×1e5).
- **4.10 Smaller items** (S11): the job reader, `vterm` scroll flood, JSON object lookup, the zip directory, FEM symbolic reuse, the TeX resolver cache plus the macOS `ExeIdentity` fix.

### Benchmarks still to add (as their phases start)

- `live/longline_1mb` (3.4)
- `live/*/idle_cpu_pct` (3.3)
- `live/startup/to_first_frame` (3.5)
- `core/sha1` and `project/git/status_after_touch` (4.5)
- `core/js/*` (4.9)
- an LSP replay benchmark: `didChange` + semantic tokens + completion at 10 Hz on a 5k-line file, measuring time to the last reply (2.4)
- a PDF image-heavy fixture (4.7)
- a split-pane variant of the live frame scenarios (R1 and R2 scale per pane)

---

## 5. Progress log

- **2026-10-16:** Suite built and baselined.
  - `src/bench_util.h` became a shared harness (registry, `--list`/`--filter`/`--reps`/`--json`, median/min/p95).
  - The six existing bench binaries were moved onto it, keeping their history scenario names.
  - New: `mep-bench` (`src/bench/`: core, syntax, editor, project, docs), the live tier (`tools/bench_live.py` and `.lua`, with `--rounds` and `--profile`), `mep.frame_stats()` / `Editor::FrameStats`, the `MEP_BENCH_NO_THROTTLE` App Nap opt-out on macOS, the runner `tools/bench.py`, and the `just bench*` recipes.
  - Baseline saved as `bench_results/baselines/baseline.jsonl`.
- **2026-10-16: Phase 1, items 1.1-1.6.**
  - **What changed:**
    - **1.1:** missing style sheets are cached by mtime, and sheet resolution in `DocSheetStyle` happens once per drawn frame instead of per styled span (`editor_mepml.cpp`).
    - **1.2:** `TreesitterHighlightRows`' whole-text fallback returns its rows for unchanged text (`treesitter.cpp`).
    - **1.3:** `ScopeGuidesCached` (per buffer; edit epoch, line count, byte total, filetype) plus a cull to the drawn rows (`main.cpp`).
    - **1.4:** a non-blocking pipe stdin with a queue flushed from `PollAll` (`job.cpp`). `CloseStdin` delivers the queue, blocking, before EOF, so `git apply`-style jobs keep their semantics. PTYs are unchanged. On the Lua side, `mep.lsp_write_pending` lets `mep.lsp_did_change` hold full-text changes back while the server is still reading, then send only the latest. Completion forces its `didChange` through.
    - **1.5:** regex prefilters read off the AST: a required literal, a leading literal, and a pure-literal fast path, all ASCII, case-folding exact (`regex.cpp`).
      - **Verified** by a differential test against `std::regex`: 2.36M comparisons over editor.cpp and main.cpp lines, with and without ignorecase, 0 mismatches.
    - **1.6:** `Undo`/`Redo` move snapshots instead of copying.
  - **Tests:** `just test` is green up to the pre-existing `mep-collab-session-test` usage exit.
  - **Measured** with `bench.py ab` against a build of the same tree without these changes, 2 interleaved rounds (baselines `phase1-before` and `phase1-after`):

    | benchmark | before | after |
    |---|---|---|
    | `live/cpp_60k/frame_idle` | 13.1 ms | **1.8 ms** |
    | `live/cpp_60k/frame_scroll` | 21.3 ms | **2.5 ms** |
    | `live/cpp_60k/frame_typing` (mean) | 131 ms | **27 ms** |
    | `live/org_20k/frame_idle` | 34.2 ms | **10.3 ms** |
    | `live/org_20k/frame_cursor_down` | 83.0 ms | **40.4 ms** |
    | `live/md_20k/frame_cursor_down` | 35.9 ms | **13.7 ms** |
    | `live/md_20k/hook_syntax_highlight_unchanged` | 302 ms | **2.5 ms** |
    | `live/py_20k/frame_idle` | 5.5 ms | **2.0 ms** |
    | `editor/cpp_60k/search_miss` | 95.9 ms | **0.85 ms** |
    | `editor/cpp_60k/substitute_global` | 107.7 ms | **6.1 ms** |
    | `core/regex/whole_text_literal` | 83.2 ms | **0.19 ms** |
    | `syntax/md/highlight_rows_unchanged` | 300 ms | **0.21 ms** |

  - **Unchanged, as expected:**
    - project search: it already had its own prefilter, and `regex_no_literal` is Phase 4.1;
    - Markdown keystroke highlighting (Phase 2.6);
    - `set_line` and the syntax hook after an edit (Phase 2).

    Their ±15-20% moves in either direction are the hook-timing noise noted in section 1. `core/regex/compile_x1000` +12% is the prefilter analysis, about 0.1 µs per compile.
  - **Still over budget, and next:**
    - Org: idle 10 ms, cursor move 40 ms (1.10: `OrgTables`/`OrgBlockCards`; then R6/R2).
    - Markdown: typing 70 ms (2.6).
    - The C++ typing mean (27 ms) is now dominated by the syntax hook's decoration churn (2.2).
- **2026-10-16: Phase 1, items 1.7-1.12.**
  - **What changed:**
    - **1.7:** `mep.syntax_highlight` passes a per-buffer cache key, `buf:<id>:<ft>`. `treesitter.cpp` keeps the 8 most recently highlighted `buf:` keys (LRU, `TreesitterForget`); mep has no buffer-closed hook, so a closed buffer ages out. New benchmark: `syntax/*/switch_files_{shared_key,own_keys}`.
    - **1.8:** `GetGlyphIndex` uses a per-glyph-array table (ASCII direct, hash map for the rest), thread-local. Every hit is verified against the glyph it names and the table rebuilds on a glyph-count change, so a font changed in place cannot be answered stale. `DrawTextEx` looks each glyph up once instead of twice.
    - **1.9:** `DrawRectangle`, `DrawRectangleRec` and `DrawTexturePro` build their quads on the stack, and skip `sin`/`cos` at rotation 0.
    - **1.10:** `OrgBlockCards`/`OrgTables` are cached per buffer across frames.
      - **Keys:** `BufferContentKey` (edit epoch, line count, byte total, filetype) plus the sheet signature, theme epoch and Rmd-notebook flag for cards, and a wrapped-row layout signature for tables.
      - **mepml:** their mepml paths return the scan's vectors instead of copying them per call.
      - **Sheets:** `DocSheetStyle`'s per-frame sheet resolution is now `RefreshDocSheets`, shared by both.
    - **1.11:** spell keeps each word's lowercase form and a-z counts from load.
      - **Exact pre-filter:** candidates whose letter-count L1 distance exceeds 2 × max_dist are skipped. An edit changes it by at most 2, so this is exact, not a heuristic.
      - **No allocation:** `EditDistance` runs on stack rows.
    - **1.12:** JSON numbers are written with `std::to_chars`, the shortest text that reads back as exactly the same double. NaN and infinity are written as `null` (they used to be written as invalid `nan`/`inf`). Checked by a round-trip of 11 values, including 0.1, 1/3, 1e-300 and 6.02214076e23.
  - **Tests:** `just test` is green except `mep-git-test`'s "this repo worktree list", and only while a scratch worktree is registered in this repo.
    - **The difference:** real git lists worktrees in directory order, while mep's in-house git sorts them, so an extra worktree exposes the ordering difference.
    - **Status:** an existing parity gap in `src/git`, not a regression; it passes again with the worktree removed.
    - **The rest:** every test after it in the recipe was run directly and passes.
  - **Measured** with `bench.py ab` against a build of the tree after items 1.1-1.6, 2-3 interleaved rounds (baselines `phase1b-before` and `phase1b-after`):

    | benchmark | before | after |
    |---|---|---|
    | `core/spell/suggest_x20` | 127.6 ms | **4.0 ms** |
    | `syntax/cpp/switch_files_*` (shared key → own keys) | 298 ms | **0.36 ms** |
    | `live/org_20k/frame_idle` | 9.7 ms | **3.75 ms** |
    | `live/org_20k/frame_after_typing` | 5.1 ms | **2.3 ms** |
    | `live/md_20k/frame_idle` | 6.3 ms | **1.8 ms** |
    | `live/cpp_60k/frame_idle` | 2.1 ms | **1.6 ms** |
    | `live/cpp_2k/frame_scroll` | 1.67 ms | **1.02 ms** |
    | `live/py_20k/frame_scroll` | 2.25 ms | **1.67 ms** |

  - **Draw-side gains (1.8, 1.9):** 20-40% off every idle, scroll and cursor-move frame.
  - **Cost:** `core/spell/load_dictionary` is ~1 ms slower (+8%) from building the per-word tables.
  - **Noise, not code:** the directly-timed hooks moved ±10% together per fixture, including ones no change touched (`set_line`, `lua_get_all_lines`), which is per-run machine state.
  - **Phase 1 overall,** baseline to now:

    | fixture | idle | scroll |
    |---|---|---|
    | cpp 60k | 13.1 → 1.6 ms | 21.3 → 2.0 ms |
    | Org 20k | 34 → 3.75 ms | 81 → 40 ms |
    | Markdown 20k | 7.4 → 1.8 ms | 36 → 16.5 ms |
    | Python 20k | 5.5 → 1.4 ms | 6.8 → 1.7 ms |

    | other | baseline | now |
    |---|---|---|
    | C++ typing frame, mean | 131 ms | 28 ms |
    | 60k-line search miss | 96 ms | 0.85 ms |

  - **Not met yet:**
    - Org cursor move (48 ms): the cursor-row hooks, `OrgHighlightEmphasis` plus Lua `string.match` and `ScanOrgLinkSpans` per move (2.2/3.3).
    - Org and Markdown scroll frames (the same hooks).
    - Markdown typing (65 ms; 2.6).
    - C++ typing (28 ms): the syntax hook re-adding every decoration (2.2) and the whole-buffer Lua reads (2.3).
- **2026-10-16: Phase 2.**
  - **2.3:** `mep.buffer_text()` and `mep.get_lines(a, b)` read the buffer in one C++ call.
    - **Ported** the per-edit hooks that rebuilt the buffer in Lua: syntax highlight, LSP `didOpen`/`didChange`, `syntax_fold`.
    - **Not ported:** the other ~35 loops in main.cpp run on demand (commands, exports), not per edit or frame.
  - **2.2:** `mep.ts_highlight_buffer` (plain filetypes) reads the buffer in C++ and calls `Editor::ReconcileNamespaceRows`.
    - **How it works:** one pass compares each row's decorations (in draw order) with the wanted spans, and only differing rows are removed and re-added. If the row answer's generation and the namespace's fingerprint (buffer, count, storage) are unchanged since the last pass, nothing is done.
    - **Still on the old path:** layered filetypes (Org, mepml, Rmd, notebooks), whose namespace also holds code-block highlights.
    - **Verified** live by comparing against a fresh `mep.ts_highlight` after edits: 108 checks over C++, Python and Markdown files (unterminated strings, unclosed comments, deletions, duplications, emptied buffer), 0 mismatches.
  - **2.5:** reverse-delta undo.
    - **Storage:** one `undo_base` copy per buffer, and entries that hold only the replaced rows, plus folds (still restored exactly).
    - **Behaviour:** Undo/Redo build the target on the base and sync the buffer to it, so an edit made since a checkpoint without its own undo step is undone along with it, as with snapshots. Marks shift by the diff. Undo resets go through `Buffer::ResetUndo`; the org-table "was there an edit" check reads `UndoDepth()`.
    - **Verified** differentially against the old snapshot implementation: 290 random edits, undo/redo interleaved mid-sequence, 500 undos past the 200-step cap and 500 redos produced the same 1,035 states (text checksum and cursor) in both, passing through 275 distinct states.
  - **2.6:** Markdown is on the incremental row path.
    - **How it works:** the block grammar is the kept tree, and the inline grammar is re-parsed only for the `(inline)` nodes the changed rows touch. Changed ranges are widened to whole paragraphs, since emphasis can span lines without the block tree changing.
    - **New test:** `mep-treesitter-test` gained `TestIncrementalRowsMatchWholeText`, checking incremental rows against a whole-text pass after each of 13 edits across Markdown, C++ and Python. With the paragraph widening removed, it fails.
  - **2.7:** `TreesitterHighlightRowsMaybeAsync`.
    - **When:** a key whose last *incremental, working* pass took over 8 ms is parsed on one background thread (newest request wins). Cost from first parses and no-op passes doesn't count.
    - **What's applied:** results for exactly the current text, or for an older text with the same line count; never rows that could land on the wrong lines.
    - **Locking:** `treesitter.cpp`'s caches are guarded by a short-lived lock, with parsing outside it; the worker's key is never evicted, and only the UI thread evicts. A frame hook applies finished results (`mep.ts_highlight_async_ready`).
    - **Verified** live on a 60k-line file with parse errors: after each of 8 edits the applied rows equal a synchronous whole-text pass (115k decorations), and hook calls drop from ~45 ms to ~1.2 ms.
    - **Two wrong turns, kept for the record:**
      - A single lock held across the parse serialized the UI thread behind the worker.
      - Counting no-op passes as cost flipped keys back to synchronous.
  - **Measured** with `bench.py ab` against a build of the tree before Phase 2, 2 interleaved rounds. Baselines `phase2a-*` cover 2.2/2.3/2.5/2.6; the rows below include 2.7.

    | benchmark | before | after |
    |---|---|---|
    | `live/cpp_60k/frame_typing` (mean) | 35.0 ms | **13.6 ms** |
    | `live/md_20k/frame_typing` | 64.7 ms | **4.0 ms** |
    | `live/py_20k/frame_typing` | 15.2 ms | **5.1 ms** |
    | `live/cpp_60k/hook_syntax_highlight_after_edit` | 62.6 ms | **2.9 ms** |
    | `live/md_20k/hook_syntax_highlight_after_edit` | 298.7 ms | **0.45 ms** |
    | `live/cpp_60k/hook_syntax_highlight_unchanged` | 16.0 ms | **1.2-2.4 ms** |
    | `live/cpp_60k/memory_mb` | 1536 MB | **725-768 MB** |
    | `live/md_20k/memory_mb` / `py_20k` / `org_20k` | 931 / 913 / 898 MB | **644-659 / 677-694 / 633 MB** |
    | `editor/cpp_60k/lua_set_line_x100` | 144.6 ms | **52.0 ms** |
    | `editor/cpp_10k/lua_set_line_x100` | 22.4 ms | **6.6 ms** |

  - **Costs, recorded honestly:**
    - **`editor/*/substitute_global` (`:%s` plus undo): 2.8 → 5.0 ms** at 60k lines. A substitute touching the whole file makes a delta of most of the file, copied twice, where a snapshot was moved.
    - **Background-thread contention:** with the worker parsing, UI-thread timings taken at the same moment (`set_line`, `lua_get_all_lines` in the cpp_60k run) read 20-75% higher. That is the two threads sharing CPU and memory bandwidth, not extra UI-thread work.
    - **Open-file A/B noise:** the A/B showed opening the 60k-line file 2× slower. Re-measured three times per build in isolation, it was equal (10.8 ms both), so it was external load during that run.
  - **Tests:** `just test` is green (the recipe still ends at `mep-collab-session-test`'s usage exit).
  - **Remaining for Phase 3:**
    - Org: cursor move 38-48 ms and typing 71 ms, in its layered highlight path and the cursor-row hooks (`OrgHighlightEmphasis`, Lua `string.match`, `ScanOrgLinkSpans`).
    - Markdown cursor moves: 12-17 ms.
    - The first highlight of a large file is still a synchronous full parse (≈340 ms at 60k lines). 2.7 could take that too, at the cost of a moment of uncoloured text after opening.
- **2026-10-16: Phase 3.**
  - **New tools:**
    - `mep.hook_profile(true)` / `mep.hook_stats()` (lua_env.cpp) time every frame hook by where it was defined, attributing `mep.on_buffer_changed`'s per-hook poller to the function it wraps. `tools/bench_live.py --profile` prints the table beside the native profile.
    - New live benchmarks: `idle_cpu_ms_per_s` (10 s of mep's own CPU after 12 s untouched), `startup/to_first_frame`, and the `longline_1mb` fixture.
    - `MEP_GFX_TEX_LOG=1` logs every texture upload and the running total.
  - **Org cursor moves.** The hook profile named three whole-buffer rescans per move.
    - **Links/emphasis/tables hook (8 ms per move):** `OrgHighlightEmphasis`/`OrgLinkScan` gained a rows-only form (`mep.org_rescan_rows`). It redoes only the row the cursor left and the row it entered, from per-row block state the full scans record (`Editor::OrgRowScanState`, valid while the text is unchanged), and rebuilds concealment runs for those rows only.
    - **`{{{hl}}}` macro renderer (4.6 ms):** caches its parse per (file, edit epoch, line count) and exits at once when the document has no macro.
    - **Polyglot LSP status (a 55 ms stall every 2 s):** caches its block list per text, and buckets diagnostics by line instead of walking every diagnostic for every block.
    - **Verified** differentially against the build without these changes: an Org document with links, emphasis, src/example blocks, a box, a rule, a table, and single- and multi-line macros, through 13 cursor moves, an edit and a line delete. The decorations of all three namespaces were identical in all 16 recorded states.
  - **3.3, macOS idle wake-ups.** `WaitEvents` used to wake every 10 ms to poll the wake pipe. Now:
    - **Waiting:** a GCD read source on the pipe posts an application-defined NSEvent into the Cocoa queue, and the wait is one blocking call.
    - **Re-arming:** the source suspends after firing and is re-armed by the next wait.
    - **Input accounting:** the event pump drops wake events uncounted, so they don't keep mep at full frame rate.
    - **Exercised by** the live benchmark itself, whose every agent-socket command wakes the loop this way.
  - **3.4, long lines.**
    - **Column mapping:** `SpanCol` (a decoration's byte offset to its column) called `ByteOffsetToColumn`, a walk from the line start, for both ends of every decoration on the row. With ~100k syntax spans on one 1 MB line, a frame was ~10^11 steps and mep never drew one. It is now a per-row table: identity for ASCII rows, else exactly ByteOffsetToColumn's rule.
    - **Culling:** wrapped slices and `ForEachWrapPiece` pieces outside the pane are skipped.
  - **3.5, startup.**
    - **Window animation:** restoring a maximized window called `[NSWindow zoom:]`, which animated for ~260 ms before the first frame. `MepWindow` now reports zero resize time during that programmatic zoom; the title bar's zoom button still animates.
    - **Tried and reverted:** registering defaults to drop AppKit's automatic text-input menu items, which measured no gain.
  - **3.5, GPU memory.**
    - **Single-channel atlases:** font atlases are now `kPixelFormatAlphaWhite`, one coverage byte per pixel. They upload as `GL_R8` with texture swizzle (1, 1, 1, R), so shaders see what they saw before. WebGL2, which has no swizzle, widens at upload, and `ImageFromImage` crops still come back RGBA.
    - **Result:** startup texture uploads went from 174 MB to 43.5 MB (the emoji atlas alone from 79 to 20 MB).
    - **Visual change, small, flagged for the user:** glyph edges lose a faint dark fringe the RGBA atlas's black transparent texels gave them under linear filtering. 10-15% of glyph pixels differ, by at most ~75/255, and at 3x zoom the two are hard to tell apart.
  - **Tests:** green. `mep-git-test` again failed only its worktree-order check while a scratch worktree existed, and passes without it.
  - **Measured** with `bench.py ab` against a build of the tree before Phase 3, 2 interleaved rounds (baselines `phase3-*`):

    | benchmark | before | after |
    |---|---|---|
    | `live/org_20k/frame_cursor_down` | 47.6 ms | **4.1 ms** |
    | `live/org_20k/frame_scroll` | 40.6 ms | **4.7 ms** |
    | `live/org_20k/frame_typing` | 65.8 ms | **28.7 ms** |
    | `live/md_20k/frame_scroll` | 17.5 ms | **11.1 ms** |
    | `live/*/idle_cpu_ms_per_s` (cpp 2k/60k, py, md, org) | 17 / 19 / 18 / 17 / 51 | **8 / 9 / 8 / 7 / 11** |
    | `live/*/memory_mb` (cpp 2k/60k, py, md, org) | 566 / 764 / 696 / 656 / 644 MB | **336 / 567 / 476 / 408 / 398 MB** |
    | `live/startup/to_first_frame` | 634 ms | **390 ms** |
    | `live/longline_1mb/frame_idle` (measured separately: the old build never drew a frame) | never | **15 ms** |

  - **Small rises, within noise:** `org/frame_after_typing` 2.4 → 2.7 ms and `py/frame_after_typing` 1.5 → 1.9 ms. Neither path has a Phase 3 change; the typing mean on the same fixtures fell or held.
  - **Next, where the measurements point:**
    - Org typing (29 ms per frame, ~27 ms of it edit hooks): the full emphasis/link/table scans and the layered Org syntax path on every edit, which 2.2's reconciliation and a rows-only form for edits could take.
    - Markdown cursor moves (12 ms, its conceal hooks).
    - The long-line row's own per-frame O(length) work (15-34 ms).
    - A synchronous first highlight of large files.
    - Lazy emoji/icon atlases (most of the remaining 43 MB, plus their bake time).

