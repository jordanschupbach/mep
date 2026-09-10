# CRDT-backed collaborative text storage + persistent benchmarking

Follow-on to the collaboration feature (`collab_crdt.*`/`collab_session.*`/
`collab_relay.cpp`, landed in `cfde61e "Add collab, gantt, and kanban"`).
The user asked to investigate mep's core text-editing data structures,
build persistent benchmarks first, then implement combinations of
standard structures (CRDT, rope, piece table, gap buffer) chosen by
those benchmarks. Given research findings (a CRDT already exists but is
O(N) per op and only a bolt-on, not authoritative), the user chose to
prioritize making the CRDT a real, efficient, authoritative
representation for the actively-collaborative buffer over
`Buffer::lines`-wide work this round.

**How to resume:** check the boxes below, continue with the first
unchecked phase. Same rigor as every other plan here: implement ->
`nix develop --command cmake --build build/native` -> `just test` ->
live-verify -> tick the box with a "Verified via:" note -> next phase.

---

## Research findings (done)

- `Buffer::lines` (`editor.h:607`) is `std::vector<std::string>`, sole
  source of truth for all editing, ~276 call sites concentrated almost
  entirely in `editor.cpp`. Undo/redo is whole-vector snapshotting, a
  pattern duplicated identically across 4 other document types.
- `mep::collab::TextCrdt` (`collab_crdt.h`/`.cpp`) is a correct,
  hand-rolled per-byte RGA, but backed by `std::map<CrdtId,Node>` + a
  `children_` DFS walk (`VisibleIds()`) that's O(visible size) per
  single `Insert`/`Erase` call -- an M-byte edit costs ~O(M x N).
- The CRDT is a bolt-on, not the buffer's real storage: fed once per
  input tick by joining the whole buffer to a string and diffing
  against last-seen text via a single common-prefix/suffix scan
  (`CollabSession::Synchronize`, `collab_session.cpp:114`) -- can only
  faithfully represent one contiguous edit region per tick.
- Agent-RPC edits (`buffer.insertText` -> `InsertTextAt`,
  `editor.cpp:22097`) are a separate path with no CRDT-specific
  handling; only swept in incidentally via the same per-tick diff.
- No performance/stress test exists for the CRDT anywhere (2-site tiny
  fixed examples only).
- User's explicit scope choice: focus on CRDT-as-collaborative-
  source-of-truth; defer `Buffer::lines`-wide rope/gap-buffer/
  piece-table work.

## Phase 1: Benchmark harness (persistent, house-style)

- [x] `src/text_crdt_bench.cpp` (primary): single-site sequential
  insert throughput at increasing doc sizes, large multi-KB insert,
  random insert/delete mix, many-site convergence, snapshot/restore
  cost at scale.
- [x] `src/text_buffer_bench.cpp` (baseline context): sequential
  typing, large paste, undo push cost at increasing file size, random
  edits, using the same vector<string> primitives `Buffer` uses.
- [x] Persistence: `bench_results/history.jsonl` (git-tracked,
  append-only, one JSON record per scenario per run: timestamp, git
  commit, scenario, N, wall time, ops/sec). `tools/bench_report.py`
  prints latest-vs-previous per scenario. `just bench` recipe.
- [x] Baseline run recorded against the *current*, unmodified
  `TextCrdt` before any data-structure changes.
- [x] **Verified via:** `just bench` builds and runs both binaries
  cleanly; `tools/bench_report.py` prints a readable per-scenario
  table. **A real, severe bug was found in the process, not just slow
  numbers**: `TextCrdt::Visit` was a recursive DFS, and a sequential
  run of N inserts (typing, or any single multi-byte `Insert()` call)
  forms a `children_` chain N deep -- the recursion blew the C++ call
  stack (a hard crash, `mep-crdt-bench` segfaulted) on document sizes
  well under 100k characters, confirmed live via gdb backtrace (63,943
  stacked `Visit` frames). Fixed immediately as a minimal, behavior-
  and-complexity-preserving change (iterative explicit-stack DFS,
  still O(N) -- Phase 2 below is the actual complexity fix) so Phase 1
  could record honest baseline numbers instead of a crash: `undo_push`
  cost scales from 122,762 ops/sec (1000-line doc) down to 328 ops/sec
  (100,000-line doc, a ~375x slowdown) confirming the whole-snapshot
  undo cost; `TextCrdt` sequential typing scales from 36,152 ops/sec
  (N=1000) down to 1,065 ops/sec (N=10000, a ~34x slowdown over a mere
  10x size increase) confirming the O(N)-per-op problem headed into
  Phase 2.

## Phase 2: Rope-backed `TextCrdt` internals (same public API)

- [x] Size-augmented treap (balanced BST, random priorities, O(log N)
  expected) keyed by visible sequence order, replacing
  `std::map<CrdtId,Node>` + `children_` DFS. Side `unordered_map<CrdtId,
  Node*>` for O(1) average id->node lookup. Subtree-size augmentation
  (`total_size`/`visible_size`) for O(log N) position<->id conversion.
  Tombstones stay in-tree as today (never physically removed).
  `children_` (semantic sibling order, sorted by id) kept exactly as
  before for deciding concurrent-sibling order; only position lookup/
  maintenance changed. Fast path (no existing concurrent siblings at an
  anchor -- true for essentially all sequential typing) is a single
  O(log N) rank lookup; the rarer concurrent-sibling path
  (`EndOfSubtree`) is bounded by the size of that one sibling's own
  edit cluster, not by document size -- a deliberate, documented
  tradeoff over a from-scratch universal-worst-case algorithm (see
  Non-goals).
- [x] Public API (`Insert`/`Erase`/`Apply`/`Text`/`Snapshot`/`Restore`)
  unchanged -- zero changes needed to `collab_session.cpp`/
  `collab_web_session.cpp`/`editor.cpp` glue for this phase.
  `collab_crdt_test.cpp`'s existing assertions keep passing unmodified.
  (`TextCrdt` gained explicit move ctor/assignment and a destructor,
  and copy is now explicitly deleted -- it owns a heap-allocated treap
  now, unlike the old all-value-type members.)
- [x] New stress/fuzz tests (`collab_crdt_test.cpp`): 6-site randomized
  interleaved insert/delete convergence (150 rounds), out-of-order
  delivery (shuffled op arrival vs. in-order, on a fresh replica each),
  large sequential convergence (40,000 chars combined, well past the
  Phase-1-fixed crash threshold). **Found and fixed a second real bug**
  this way: a node integrated already-tombstoned (its delete arrived
  before its insert -- normal under out-of-order delivery) kept
  `Node`'s default-initialized `visible_size = 1` instead of `0`,
  silently counting deleted bytes as visible whenever delete-before-
  insert ordering occurred. Reproduced reliably by making
  `priority_rng_`'s seed deterministic (from the actor id's own hash,
  not `std::random_device` -- a permanent change, documented in
  `TextCrdt`'s constructor, that doesn't reduce real-world randomness
  since actor ids are already randomly generated) rather than chasing
  a flaky failure; confirmed the fix against a 15,000-trial sweep of
  actor-id/seed combinations (0 failures) before trusting it.
- [x] Re-ran Phase 1 benchmarks against the new internals; recorded in
  `bench_results/history.jsonl` (`tools/bench_report.py` shows the
  delta directly). Sequential typing at N=10000: 1,079 -> 2,892,899
  ops/sec (~2680x). Random edits in a 20,000-char doc: 247 -> 56,629
  ops/sec (~229x). Full benchmark suite wall time: 15.2s -> 0.24s
  (~63x). `VisibleSize()` is now O(1) (was O(N)).
- [x] **Verified via:** full clean build under `MEP_STRICT_FLAGS`,
  `just test` passes (`collab_crdt_test`'s expanded stress cases
  included, same pre-existing unrelated `mep-collab-session-test`
  failure). `just bench` re-run, comparison recorded. Both real bugs
  found live (not just numbers looking good) -- the Visit stack
  overflow via gdb backtrace, the tombstone visible_size bug via a
  1000+ trial randomized sweep isolating a deterministic repro -- and
  both fixed and re-verified before moving on.

## Phase 3: Make the CRDT robustly authoritative for the live buffer

- [x] Extracted `DiffHunk`/`MyersDiffHunks` out of `editor.h`/`.cpp`
  into standalone, dependency-free `src/text_diff.h`/`.cpp`
  (`namespace mep::diff`) so `collab_session.cpp` can reuse it without
  linking `editor.cpp` -- `mep-collab-session-test`'s own CMake target
  never has, and must not start now. `editor.h` keeps every existing
  call site (`Editor::GitGutterRefresh`/`GitStageHunk`, `mep.diff_lines`)
  working unchanged via `using DiffHunk = mep::diff::DiffHunk; using
  mep::diff::MyersDiffHunks;`.
- [x] Replaced `CollabSession::Synchronize`'s single common-prefix/
  suffix diff with a multi-hunk diff: new `mep::collab::DiffIntoCrdt`
  (`collab_session.h`/`.cpp`) splits old/new text into lines, runs
  `MyersDiffHunks` to localize changed line regions, translates each
  hunk's line range back to a byte range, narrows it to the exact
  changed characters (prefix/suffix scan *within* the hunk), and
  applies each as its own Erase/Insert splice -- tracking a running
  byte delta so each hunk's offset stays correct in the document's own
  shifting coordinate space as earlier hunks' edits land. Declared in
  the header (not file-local) specifically so it's testable directly
  against a bare `TextCrdt`, no live relay needed. By design this only
  localizes to line granularity: two disjoint edits on the *same* line
  still collapse into one hunk, same as before -- the plan's own
  "collapsing into one wrong span" failure mode is specifically about
  edits landing on *different* lines with unchanged content between
  them, which this fixes. No special-casing by edit origin, so
  agent-RPC edits converge with the same fidelity as human edits.
- [x] No change to `Buffer::lines`'s type or the ~276 general call
  sites.
- [x] New test: `TestDisjointEditsPreserveUnchangedNodes`
  (`collab_session_test.cpp`, runs unconditionally at the top of
  `main()` so it always executes under `just test` without a live
  relay) -- two edits on different lines with an unchanged line
  between them; asserts the unchanged line's CRDT node ids are
  unchanged after the diff (not tombstoned-and-recreated), which a
  single-hunk diff would have failed (no common prefix/suffix at all
  across the whole 3-line string, so it would replace everything).
- [x] **A second, more severe pre-existing bug was found and fixed by
  this test**, in `TextCrdt::IntegrateInsert` itself (`collab_crdt.cpp`,
  predates this session -- present since before the Phase 2 treap
  rewrite too, per its own "unchanged from the original design"
  comment on `children_`'s sibling ordering): when a new insert's
  anchor already has an existing sibling (e.g. two *separate*
  `Insert()` calls both targeting position 0, an extremely common
  real-editing shape -- editing the first line twice, or, as this
  test does, an edit landing right after an already-tombstoned first
  line), the tie-break for "which sibling goes where" was ordering by
  *ascending* id, sequencing the newer insert after the older
  sibling's *entire* subtree instead of placing it where it was
  actually asked to go. For the very first character ever inserted,
  that subtree can be most of the document, so the practical effect
  was: any second edit at a previously-used anchor silently landed at
  (or near) the *end* of the document instead of its requested
  position. Invisible to every prior convergence test (`collab_crdt_
  test.cpp`'s `TestManySitesConvergence`/`TestOutOfOrderDelivery` both
  call `Insert(pos, ...)` at arbitrary positions on the same replica
  repeatedly, which should have hit this constantly) because those
  only assert that replicas converge with *each other* -- a
  deterministic-but-wrong tie-break still converges, it just converges
  to the wrong place, which no existing assertion checked. Fixed by
  flipping the tie-break to descending-id order (standard RGA
  semantics: the most-recently-created sibling becomes the anchor's
  new immediate successor, matching normal "insert right here" intent
  for the non-concurrent case while still giving concurrent inserts a
  deterministic, agreed-upon order). New regression test
  `TestRepeatedAnchorInsertHonorsPosition` added directly to
  `collab_crdt_test.cpp` (the bug's actual home), checking the
  resulting *text*, not just cross-replica equality.
- [x] **Verified via:** full clean build under `MEP_STRICT_FLAGS`,
  `just test` passes (`collab_crdt_test`'s new position-fidelity case
  and `collab_session_test`'s new disjoint-edit case both included;
  same pre-existing unrelated `mep-collab-session-test` no-relay-arg
  failure as every prior phase). Manually isolated and confirmed both
  bugs with small standalone repros before and after the fix (`Erase`
  then `Insert(0, ...)` landing at the end pre-fix, at the front
  post-fix). `just bench` re-run: `text_crdt_bench`'s
  `random_edits_in_doc_of_5000`/`_20000` (which repeatedly insert/erase
  at random positions on one replica -- exactly the shape that hits
  repeated-anchor inserts) jumped ~7-25x (335k -> 2.51M ops/sec, 55.6k
  -> 1.48M ops/sec) -- the old tie-break wasn't just positionally
  wrong, it was also pathologically walking large `EndOfSubtree`
  chains on every such insert, so the fix is a real perf win too, not
  only a correctness one.

## Phase 4: Docs + tracking

- [x] This doc fully checked off with verification notes.
- [x] Non-goals section below reflects what was deliberately deferred.
- [x] Live collaboration smoke test (plan's own Verification section
  below): `mep-collabd` run locally, a POST `/v1/sessions` minted a
  real capability link, and two independent `CollabSession` clients
  (Alice pre-loaded with a 3-line document, Bob starting empty)
  connected over it. After confirming initial convergence, both
  clients made *concurrent* disjoint edits -- Alice changing line 1,
  Bob changing line 3, in the same tick before either had seen the
  other's change -- and both converged to the fully-merged 3-line
  result with the unchanged middle line intact. This is the real
  cross-peer version of what `TestDisjointEditsPreserveUnchangedNodes`
  checks locally.

### Non-goals

- Not changing `Buffer::lines`'s type or touching its ~276 call sites.
- Not implementing a standalone gap buffer or piece table this round --
  rope only, and only as the CRDT's internal backing structure. Gap
  buffer/piece table's natural fit is single-mutable-buffer local
  editing and large-file load, exactly the scope deprioritized this
  round.
- Not run-length-encoding the RGA (coalescing contiguous same-origin
  inserts into runs) -- a real further optimization, not required for
  the O(N) -> O(log N) win the treap alone delivers.
- Not supporting more than one simultaneously-collaborative buffer --
  matches the existing architecture, unchanged by this plan.
- `DiffIntoCrdt`'s multi-hunk diff localizes to line granularity only
  (via `MyersDiffHunks`): two disjoint edits on the *same* line still
  collapse into one prefix/suffix-narrowed splice, same as before this
  plan. Only edits on *different* lines get split into independent
  hunks. A character-level (not line-level) multi-region diff would
  handle same-line disjoint edits too, but line hunks are what the
  actual failure mode (whole-buffer diffing collapsing unrelated
  distant edits into one wrong span) needed, and matches the
  granularity `MyersDiffHunks` already provides for git-gutter.
