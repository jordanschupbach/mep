# Vim Parity Plan

Living roadmap for closing the gap between mep and Vim. Written so a new
session can pick this up cold: read the **Status** section first for
where things stand, then the relevant phase for what's left in it.

**How to resume:** check the checkboxes below, `git log --oneline` for
what's landed, then continue with the first unchecked item in the
lowest-numbered incomplete phase. Phases are ordered so each one only
depends on earlier ones (e.g. text objects need the count infrastructure
from Phase 0 first).

## Status

- [x] Phase 0 — Count infrastructure
- [x] Phase 1 — Core motions
- [x] Phase 2 — Operators, text objects, line operators
- [x] Phase 3 — Registers
- [x] Phase 4 — Search
- [x] Phase 5 — Marks & jumps
- [x] Phase 6 — Substitute & global commands
- [x] Phase 7 — Repeat (`.`) & macros
- [x] Phase 8 — Insert mode improvements
- [x] Phase 9 — Visual mode improvements (incl. Visual Block)
- [x] Phase 10 — Scrolling & misc normal-mode commands
- [x] Phase 11 — Ex commands & polish

All phases complete.

Last updated: 2026-08-19 (session that added Phase 11 — all phases now complete).

---

## Audit: what mep has today (pre-plan baseline)

Modes: Normal, Insert, Visual (charwise), Visual-Line, Command-line. No
Visual-Block.

Motions: `h j k l 0 $ w b gg G`. No counts on anything.

Operators: `d y c`, composable with the motions above, doubled (`dd`
`yy` `cc`) for linewise-current-line. No text objects, no `D`/`C`/`Y`,
no counts.

Registers: exactly one unnamed yank slot (`yank_text_` /
`yank_linewise_`). No named/numbered/special registers.

Undo: whole-buffer-snapshot stack per operation (not per keystroke),
200 deep, `u` / `Ctrl-r`. Coarse but correct; not part of this plan.

Search: none at all — no `/`, `?`, `n`, `N`, `*`, `#`.

Marks/jumps: none — no `m`, `` ` ``, `'`, jumplist, `gv`.

Ex commands: `:w[a] :q[a][!] :wq[a] :x[a] :e :split :vsplit :close
:tabnew :tabdelete :tabnext :tabprevious :lua :source :<N>`, plus
Lua-defined commands via `mep.command()`. No `:s`, `:g`, `:v`, `:normal`,
`:d`, `:y`, `:m`, `:t`/`:co`, ranges, or marks in ranges.

Repeat/macros: none — no `.`, no `q`/`@`.

Insert mode: char typing, Enter, Backspace, Delete, arrow keys. No
`Ctrl-W`/`Ctrl-U`, no `r`/`R`.

Other Normal-mode commands present: `x p P u Ctrl-r v V :`. Missing:
`r R J gJ ~ gu gU >> << Ctrl-A Ctrl-X Ctrl-D Ctrl-U Ctrl-F Ctrl-B zz zt
zb`.

Windows/tabs/panes, Lua scripting API, and the GUI menu bar are already
solid and out of scope for this plan (it's specifically about Vim
*editing* parity).

---

## Design decisions

- **Counts**: a single `int pending_count_` accumulator, reset on any
  non-digit key and on mode exit. `0` is only a count digit when
  `pending_count_ != 0` already (leading `0` is the "start of line"
  motion, matching Vim). An operator can have a count on both the
  operator and the motion (`2d3w` deletes 6 words) — multiply them.
- **Text objects**: implemented as a second pending-state machine
  (`pending_textobj_scope_` = `i`/`a`) entered when `d`/`y`/`c`/`v` is
  followed by `i` or `a`, resolved on the next key (`w`, `"`, `(`, ...).
  Returns a `[start, end)` range + linewise flag, fed into the existing
  `ApplyOperator`.
- **Registers**: `std::unordered_map<char, Register>` where `Register =
  {text, linewise}`. Unnamed register (`""`) always mirrors the last
  yank/delete, exactly like Vim. Named registers via `"{a-z}` prefix
  before an operator/paste. Numbered registers (`"1`.."9" shifting
  delete history, `"0` last yank) are a stretch goal within Phase 3, not
  required for the phase to be considered done.
- **Search**: plain substring search first (matches the current
  "no regex" reality and is a small, self-contained piece of code).
  Upgrading to `std::regex` (ECMAScript grammar, reasonably close to
  Vim's for simple patterns) is called out as an explicit stretch item
  at the end of Phase 4, not a blocker for the rest of the plan.
- **Repeat (`.`)**: record a small closure/struct describing the last
  *change* (not motion) — either "re-run this operator+range-spec" or
  "re-run these N inserted characters" — rather than literally replaying
  raw keystrokes, so it composes correctly with counts given at replay
  time (`3.`).
- **Macros**: record raw key events (codepoints + special keys) into a
  buffer while `q{reg}` is active, replay by re-injecting them through
  the same `DispatchNormalKey`/insert-mode path used for real input.
- Everything here targets **Normal/Visual/Command mode key handling in
  `src/editor.cpp`**; `src/main.cpp` (rendering) and `src/lua_env.cpp`
  (scripting surface) are touched only where a new feature needs a
  status-line indicator (e.g. showing a pending count/register) or a
  new Lua hook, never as the primary target.

---

## Phase 0 — Count infrastructure ✅

Foundation for almost everything else.

- [x] `pending_count_` accumulator: digits `1-9` start it, `0` only
      continues it (never starts it — bare `0` stays the start-of-line
      motion).
- [x] Applies to simple motions: `5j`, `3k`, `10G`, `4w`, `6b`, `99$`
      (clamped to buffer bounds, same as Vim).
- [x] Applies to `dd`/`yy`/`cc`-style doubled linewise operators: `3dd`
      deletes 3 lines.
- [x] Applies to operator+motion: `d3w`, `2d3w` (multiplied).
- [x] Applies to `x`/`p`/`P`: `5x`, `3p`.
- [x] Count reset on Escape, on completing a command, and on mode
      switch.
- [x] Status line shows the pending count (and, once Phase 3 lands,
      pending register) so it's not invisible while typing.

## Phase 1 — Core motions ✅

- [x] `e` / `E` — end of word / WORD forward.
- [x] `ge` / `gE` — end of word / WORD backward.
- [x] `W` / `B` — WORD forward/backward (whitespace-delimited only,
      vs. `w`/`b`'s punctuation-aware word boundaries).
- [x] `^` — first non-blank character of line.
- [x] `f{char}` / `F{char}` — find char forward/backward on the line.
- [x] `t{char}` / `T{char}` — till char forward/backward on the line.
- [x] `;` / `,` — repeat last f/F/t/T, forward/backward.
- [x] `{` / `}` — paragraph backward/forward (blank-line-delimited).
- [x] `%` — jump to matching `()`/`{}`/`[]`.
- [x] All of the above work as operator motions too (`df.`, `dt,`,
      `d}`, `d%`, `dW`, `de`) and take counts (`3fx`, `2}`).
- [x] `(` / `)` sentence motions — **skipped**: even a rough
      "sentence = up to `.`/`!`/`?` + whitespace" heuristic added
      meaningful edge-case complexity for a motion pair mep's own usage
      patterns rarely need; revisit only if it turns out to matter.
- [x] `H` / `M` / `L` — top/middle/bottom of *visible* screen. Needs
      the pane's visible-line count, which today only `main.cpp`
      computes per-frame; added a small `Editor::SetVisibleLines(pane_id,
      n)` call from `UpdateScrollForPane` so Normal-mode handling has it
      without threading rendering geometry into `editor.cpp` proper.

---

## Phase 2 — Operators, text objects, line operators ✅

- [x] `D` / `C` — operate from cursor to end of line (shorthand for
      `d$`/`c$`). **`Y` corrected during implementation**: real Vim
      makes `Y` a synonym for `yy` (whole line), *not* `y$` despite
      superficially matching `D`/`C`'s pattern — this plan had it wrong
      above; implemented as `yy`, matching Vim.
- [x] Text objects, inner and around: `iw`/`aw` (word), `iW`/`aW`
      (WORD), `i"`/`a"`, `i'`/`a'`, `` i` ``/`` a` `` (quoted strings),
      `i(`/`a(`/`ib`/`ab`, `i{`/`a{`/`iB`/`aB`, `i[`/`a[`, `i<`/`a<`
      (bracket pairs — inner excludes the brackets, around includes
      them and one side's trailing whitespace for word/quote objects).
      Bracket objects find the *enclosing* pair (proper nesting-depth
      scan), not just the nearest same-glyph pair, and correctly span
      multiple lines (`di{` on a multi-line block) — this required
      fixing DeleteRange/YankRange, which only handled single-line
      charwise ranges before now (a latent bug already reachable via
      Phase 1's `d}`, not just text objects).
- [x] `ip`/`ap` — inner/around paragraph.
- [x] Text objects work after `d`/`y`/`c`/`>`/`<`/`gu`/`gU` and in
      Visual mode (`viw`, `di(`, `ya"`, `>ip`).
- [x] `~` — toggle case of char under cursor, advance (takes a count:
      `3~`). Also acts on the whole selection in Visual mode.
- [x] `gu{motion}` / `gU{motion}` / `guu`/`gUU` — lowercase/uppercase
      over a motion, text object, or current line; also acts on the
      Visual selection. **Simplification**: `gugu`/`gUgU` (Vim's other
      valid spelling of the doubled-line form) aren't recognized, only
      `guu`/`gUU` — low value for the extra dispatch-state complexity.
- [x] `>>` / `<<` — indent/dedent current line (or count lines) by a
      fixed 4-space shiftwidth (no `shiftwidth`/`expandtab`/tabstop
      config); `>{motion}` / `<{motion}` generally, and `>`/`<` in
      Visual mode. Always a whole-line operation even for a charwise
      motion, matching Vim.
- [x] `J` — join current line with next (single space between, unless
      next starts with `)`, or either side is empty); strips leading
      whitespace from the joined-in line. `gJ` — join without inserting
      a space. Both take a count (join that many lines, minimum 2).
- [x] All operators above take counts and combine with Phase 1 motions
      and Phase 2 text objects, via the same `pending_op_`
      infrastructure Phase 0 built for `d`/`y`/`c` — `gu`/`gU`/`>`/`<`
      are internally just more operator letters fed through it.

Verified via the actual native build under Xvfb: `di(`/`di"` (cursor
inside), a no-op `di(` correctly refusing when the cursor is *before*
any bracket on the line (matching Vim, not a bug), multi-line `di{`
(the DeleteRange fix), `dap` (paragraph + trailing blank line), `J`,
`>>`, and `~`/`3~` (case toggle + correct cursor advancement over
non-letters).

## Phase 3 — Registers ✅

- [x] `Register` struct (`text`, `linewise`) and a
      `std::unordered_map<char, Register>` on `Editor` (global, not
      per-buffer, matching Vim — decided as planned).
- [x] `"{a-z}` prefix before an operator or `p`/`P` targets a named
      register instead of unnamed; can appear before or after a count
      (`"a2dd` and `2"add` both work, since the register prefix doesn't
      touch `pending_count_`). `x` now yanks into the register too
      (routed through the same `ApplyOperator('d', ...)` path as other
      deletes), which it silently didn't before this phase.
- [x] Unnamed register (`"`) mirrors the most recent yank/delete
      regardless of whether a named register was also targeted —
      verified concretely: yank into `"a`, then a *plain* `yy` elsewhere
      does not clobber `"a`'s contents, and `"ap` still pastes the
      original text while plain `p` pastes the newer one.
- [x] Uppercase register name (`"A`) *appends* to register `a` instead
      of replacing it — verified concretely (`"Ayy` after `"ayy` leaves
      `"a` holding both lines, pasted together via `"ap`).
- [x] Status line shows a pending register prefix (`"a`) the same way
      it shows a pending count, and the two compose in the display.
- [ ] Stretch, not implemented: numbered registers `"1`-`"9` (shifting
      delete history) and `"0` (last yank, survives intervening
      deletes).
- [ ] Stretch, not implemented: `"%` (current filename), read-only.

Register consumption is centralized in `ApplyOperator` (one
`TakeRegisterSpec` call covers `d`/`y`/`c`/`x`) and in the `p`/`P`
dispatch cases, so it was a small, contained change despite touching
several call sites — no changes needed to Phase 1/2's motion or text
object code, which just feed ranges into `ApplyOperator` as before.

## Phase 4 — Search ✅

- [x] `/{text}` and `?{text}` enter a search-input mode: two new `Mode`
      values (`SearchForward`/`SearchBackward`, not a flag on
      `Mode::Command`) with their own `HandleSearchInput`, which mirrors
      `HandleCommandInput`'s text-editing but runs a search on Enter
      instead of `ExecuteCommandLine`. The command bar draws whichever of
      `:`/`/`/`?` matches the current mode.
- [x] `n` / `N` — repeat last search, forward/backward respectively,
      honoring the direction the original search was in (`N` after a
      backward `?` search goes forward, etc.) — verified concretely,
      including that `N` after `*` (which is always a forward search)
      correctly goes backward.
- [x] `*` / `#` — search for word under cursor, forward/backward (reuses
      Phase 2's `WordObjectRange` to extract the word).
- [x] Wraps around the buffer with a "search hit BOTTOM, continuing at
      TOP" / "search hit TOP, continuing at BOTTOM" status message,
      matching Vim's wording — verified concretely in both directions.
- [x] An empty query (bare `/<Enter>` or `?<Enter>`) repeats the last
      search pattern in the newly-given direction, matching Vim.
- [ ] Not implemented (stretch, as planned): search motions as operator
      targets (`d/foo<Enter>`), and incremental highlight-while-typing.
- [ ] Not implemented (explicit stretch, as planned): `std::regex`
      matching — plain substring only.

Verified via the actual native build: typed `/fox<Enter>` landing on the
first match past the cursor, `n`/`n` advancing through subsequent
matches, wrap-around in both directions with the exact status message
text, `*` on a word jumping to its next occurrence, `N` reversing
correctly, and `?dog<Enter>` wrapping backward with the matching message.

## Phase 5 — Marks & jumps ✅

- [x] `m{a-z}` — set mark at cursor position. Standalone only (unlike
      `` ` ``/`'`, never an operator target — there's no such thing as
      "deleting to a mark-setting").
- [x] `` `{a-z} `` — jump to exact mark position. Works standalone and
      as an operator target (`` d`a ``), exclusive charwise like Vim.
- [x] `'{a-z}` — jump to first non-blank of mark's line, linewise when
      used as an operator target (`d'a`).
- [x] `` `` `` / `''` — jump to the position before the last "big jump"
      (implemented as a one-deep `last_jump_from`/`has_last_jump` slot
      per buffer, not a full jumplist). Recorded by `G`, `gg`, a mark
      jump, and `/`/`?`/`n`/`N`/`*`/`#` searches — i.e. exactly the set
      of motions Vim itself treats as jumps, not plain cursor movement
      like `hjkl`/`w`/`f`. Pressing `` `` `` twice toggles back and
      forth between two positions, matching Vim.
- [x] `gv` — reselect the last Visual selection. `Editor::EnterNormal()`
      captures mode + anchor + cursor whenever it's leaving `Visual`/
      `VisualLine` (before overwriting `mode_`), so it fires on every
      path out of Visual mode (Escape, an operator, `:`) without each
      of those call sites needing its own capture logic.
- [ ] Stretch, not implemented: full jumplist (`Ctrl-O` / `Ctrl-I`) — a
      bounded deque of positions, pushed on any "big" jump.
- [ ] Stretch, not implemented: marks surviving edits above them
      shifting line numbers (mep's marks are fixed `{row, col}` and go
      stale, like a plain snapshot, if lines are inserted/deleted above
      them).

Verified via the actual native build under Xvfb: `ma` at a specific
position followed by `` `a `` landing on that exact spot; `'a` landing
on the mark's line's first non-blank instead; `` `` `` after `G`, after
a mark jump, and after a `/hotel<Enter>` search each correctly
returning to the pre-jump position (and toggling back on a second
`` `` ``); `` d`a `` as an operator target producing the same
multi-line exclusive-delete result `dG`-style ranges do; and `gv` after
`v` + a charwise selection + Escape + `gg` restoring the exact original
selection (mode, anchor, and cursor).

## Phase 6 — Substitute & global commands ✅

- [x] `:s/pattern/replacement/` — current line only, first match.
- [x] `:s/pattern/replacement/g` — all matches on current line.
- [x] `:%s/pattern/replacement/[g]` — whole buffer.
- [x] `:{range}s/...` — numeric ranges (`:5,10s/...`), `.`/`$` (current
      line / last line), and `'a,'b` (mark ranges, since Phase 5 has
      landed) — plus a `+N`/`-N` offset on any of those (`.+1,.+3s/...`),
      a small extra since the address parser needed to handle it anyway.
- [x] Backreferences — **not implemented, as anticipated**: still no
      regex (Phase 4's plain-substring decision carries through), so
      `replacement` is always a literal string with no `\1`/`\0`/`&`.
- [x] `:g/pattern/{cmd}` — run `{cmd}` on every matching line. Also
      accepts the `global` spelling and an explicit range (default: the
      whole buffer, matching Vim).
- [x] `:v/pattern/{cmd}` (a.k.a. `:g!`) — run on every *non*-matching
      line. Implemented as one function (`ExGlobal`) with an `invert`
      flag shared by both spellings.
- [x] `:d` / `:y` / `:m{addr}` / `:t{addr}` (a.k.a. `:co`) — delete/
      yank/move/copy line ranges via ex command, not just Normal mode.
      `:d`/`:y` reuse the existing `ApplyOperator('d'/'y', ...)` linewise
      path from Phase 2/3 directly. `:m`/`:t` needed their own
      `ExMoveOrCopy` (no equivalent existed); `:m` refuses with Vim's own
      `E134: Move lines into themselves` when the destination falls
      inside the source range.

**Design note on parsing**: range-taking commands (`:s :g :v :d :y :m
:t`) have their own inline syntax (no space before args, arbitrary
delimiter for `:s`/`:g`/`:v`) rather than the generic
`name<space>args` shape the rest of `ExecuteCommandLine` uses, so
they're matched *before* the space-based split -- but only once their
own syntax actually checks out (a delimiter right after `s`, a valid
address right after `m`, etc.). Anything that doesn't fully match falls
through untouched to the normal dispatch, which is what keeps `:sp`/
`:split`, `:vsplit`/`:vs`, and `:tabnew` (which all start with a letter
one of the new commands also uses) working exactly as before --
verified concretely, not just by inspection.

**`:g`/`:v` implementation note**: since mep's buffers are plain
`std::vector<std::string>` with no stable per-line identity, `ExGlobal`
snapshots matching row indices up front (Vim's own documented
algorithm), then replays `{cmd}` on each via a recursive
`ExecuteCommandLine` call with the cursor positioned there, tracking a
running line-count delta so later snapshotted rows still land correctly
after earlier ones insert or delete lines. Each replayed sub-command
manages its own undo push (same as it would run standalone), so
undoing an entire `:g` takes one `u` per affected line rather than one
`u` for the whole command -- a known, accepted gap given mep's existing
whole-buffer-snapshot undo model (out of scope for this plan, same as
noted in the original audit).

Verified via the actual native build under Xvfb: `:s/foo/XXX/` (first
match, current line only) vs. `:%s/foo/YYY/g` (whole buffer, all
matches, correct "N substitution(s) on M line(s)" status message);
`:g/bar/d` deleting every matching line; `:v/YYY/d` deleting every
*non*-matching line; `:1t3` (copy) and `:5m0` (move, including landing
correctly at the very top); `:2,3m2` correctly refusing with
`E134: Move lines into themselves`; `:'a,'bs/YYY/ZZZ/` substituting
across a mark range; `:s/nomatch/x/` giving `E486: Pattern not found`
and leaving the buffer untouched; and, as a regression check on the new
parse-before-the-space-split ordering, that `:sp`, `:tabnew`, and
`:close` still behave exactly as before.

## Phase 7 — Repeat (`.`) & macros ✅

- [x] Last-change record: populated by every change-making command
      (operators, `x`, `p`/`P`, insert-mode sessions closed by Escape;
      `r` lands in Phase 8) with enough info to redo it verbatim.
      **Implemented differently than originally planned**: rather than a
      structured "operator + range-spec" description, it's the literal
      sequence of keys that produced the change (printable codepoints,
      plus a handful of sentinel values for Escape/Enter/Backspace/
      Delete inside a recorded Insert session), replayed through the
      same per-key dispatch real input goes through
      (`ProcessNormalKey`/`ProcessInsertKey`). This generalizes for free
      to every operator/motion/text-object/register combination without
      needing a bespoke case for each one, at the cost of the dispatch
      loop needing to instrument itself (see design note below).
      Whether a completed command counts as a "change" worth keeping is
      decided by watching a counter (`change_epoch_`) that `PushUndo()`
      bumps -- if it moved during the command, the command edited the
      buffer and gets kept; if not (a bare motion, a search, a yank, a
      cancelled operator), the recording is discarded.
- [x] `.` — replays the last change. A count before `.` overrides the
      original count (`3.`), matching Vim, by re-splicing the digits at
      the front of the recorded key sequence before replay. **Known
      simplification**: an override given to `.` does *not* become
      "sticky" for a later bare `.` (real Vim's does) -- replaying `.`
      doesn't re-record itself, so the original count is always what's
      remembered afterward. `u` and `.` itself are never recorded as a
      change (there's nothing to redo, and Vim doesn't repeat undo via
      `.` either).
- [x] `q{a-z}` — start recording a macro into register `{a-z}` (`q{A-Z}`
      appends, matching Vim); `q` again stops.
- [x] `@{a-z}` — replay macro from register `{a-z}`, `{count}@{a-z}`
      repeats it that many times.
- [x] `@@` — replay the last-played macro.
- [x] Nested/recursive macros (`@b` inside macro `a`, or a macro that
      calls itself -- a common intentional Vim idiom, usually terminated
      by a motion eventually failing rather than an explicit count) are
      allowed, guarded only by a generous depth backstop (1000) against
      genuinely runaway recursion.
- [ ] **Not implemented as originally planned**: macro registers do
      *not* share storage with Phase 3's yank/delete `registers_`.
      Those hold yanked *text*; a macro is a keystroke sequence in this
      implementation's own encoding, not text -- sharing one
      `unordered_map` between the two would need a variant/union type
      for limited real benefit (mep has no `:put`-from-macro-register or
      similar feature that would actually need them unified). Macros
      live in their own `std::unordered_map<char, std::vector<int>>
      macros_`, keyed the same a-z way.

**Design note**: both `.` and macro replay need every keystroke --
real or replayed -- to flow through one place so replaying a change can
itself be watched for macro-recording purposes (a macro containing `.`
records the literal `.`, not its expansion, matching Vim: what `.`
does is re-resolved live at *playback* time, not baked in at recording
time). `DispatchNormalKey`/`InsertChar`/`InsertNewline`/`Backspace`/
`DeleteForward` stayed as the low-level per-key actions; two new
wrappers, `ProcessNormalKey`/`ProcessInsertKey`, sit in front of them
and are the only thing that ever calls them now -- real input
(`HandleNormalInput`/`HandleInsertInput`), `.` (`RepeatLastChange`), and
macro playback (`PlayMacro`) all go through these wrappers rather than
the raw dispatch functions directly, which is what lets the three
interact correctly (e.g. a change made *during* macro playback still
becomes the next `.`-repeatable change, exactly as if it had been typed
by hand).

Verified via the actual native build under Xvfb: `dw` then `.` deleting
the next word too (re-resolving the motion at the new cursor position,
not replaying stale coordinates); `A<text><Esc>` then `.` on a
different line correctly appending the same text there; `x` then `.`
then `3.` producing the expected count-override deletion (`3.` deleting
3 characters where the original `x`/`.` each deleted 1); recording
macro `a` as `A X<Esc>j`, then `@a` and `@@` each correctly appending
"` X`" to the next line and advancing; and `u` after all of that
undoing exactly one step (confirming undo itself never gets swept into
the repeat/macro recording machinery).

## Phase 8 — Insert mode improvements ✅

- [x] `r{char}` — replace `count` chars under/after the cursor, stay in
      Normal mode (`3rx` replaces 3 chars with `x`); refuses (no-op)
      rather than running past the end of the line if the count doesn't
      fit, matching Vim. Feeds through the same repeat/macro recording
      as everything else in Phase 7, for free.
- [x] `R` — Replace mode (typing overwrites instead of inserting;
      Backspace restores the overwritten character, or removes it
      without "restoring" anything if that keystroke had extended the
      line). **Implemented as a flag on `Mode::Insert`** (`replace_mode_`),
      not a distinct `Mode::Replace` — every other mode check in the
      codebase only cares about "is this Insert-like", so a flag needed
      touching far less than a new Mode value would have. Status line
      shows `-- REPLACE --` instead of `-- INSERT --` while active.
- [x] `Ctrl-W` in Insert mode — delete word before cursor (no line-join
      if already at column 0, matching Vim).
- [x] `Ctrl-U` in Insert mode — delete from cursor to start of line.
- [ ] `Ctrl-O` in Insert mode — **not implemented, as anticipated**:
      still a stretch item (low usage, needs a real "one-shot mode"
      concept mep has no other use for yet).

**Implementation note — a real bug caught by testing, not a design
choice**: the first pass at Ctrl-W/Ctrl-U used `IsKeyPressed(KEY_W/
KEY_U)` gated on a `ctrl` flag, mirroring how `HandleNormalInput`
already checks Ctrl-R/Ctrl-W-for-panes. Under Xvfb this was visibly
flaky -- exactly the same same-frame-keydown+keyup race documented
earlier in this plan's own history (the original `:qa` hang). Fixed by
routing Ctrl-W/Ctrl-U through the same `GetKeyPressed()` press-queue
loop `HandleInsertInput` already uses for Escape/Enter/Backspace/
Delete (which, notably, was *not* flaky, since it predates this
session's Phase 8 work and was already on the queue-based pattern) --
paired with an ordinary level-state `IsKeyDown()` ctrl check, which
isn't subject to the same edge-triggered race. `HandleNormalInput`'s
own Ctrl-R/Ctrl-W checks still use the older `IsKeyPressed` pattern and
were left alone (out of scope for this phase) -- worth keeping in mind
if either is ever reported flaky.

Verified via the actual native build under Xvfb: `3rx` replacing
exactly 3 characters and landing the cursor on the last one; `R` typed
over existing text showing `-- REPLACE --`, then two Backspaces
restoring the original two characters one at a time (confirmed via a
clean `u` afterward reverting the *entire* Replace session in one
step, same granularity as a plain Insert session); Ctrl-W deleting the
last word of a line without touching the trailing whitespace before
it; and Ctrl-U clearing from the cursor back to column 0.

## Phase 9 — Visual mode improvements ✅

- [x] **Visual Block mode** (`Ctrl-V`) — third Visual submode (`Mode::
      VisualBlock`) selecting a rectangular region by (row range × col
      range) rather than a linear char/line range, via its own
      `VisualBlockRange()` (top/bottom/left/right) alongside the
      existing linear `VisualRange()`.
  - [x] Block yank/delete (`d`/`y` operate per-row over the column
        range, via a dedicated `ApplyVisualBlockOperator` rather than
        the existing linear `ApplyOperator` -- a rectangle isn't a
        `[start, end)` range). Yanks into the register as a new
        `blockwise` kind (`Register` gained a `bool blockwise` flag
        alongside its existing `linewise`); `p`/`P` on a blockwise
        register paste it back as a block (one row below the next,
        padding short rows with spaces if the paste column is past
        their actual end) via a new `PasteBlockAt`, reusing
        `SplitYankLines`'s existing trailing-`\n`-per-row convention so
        blockwise storage needed no new parsing.
  - [x] Block insert (`I` at the left edge, `A` at the right edge)
        inserts the same typed text on every row of the block once
        Insert mode exits -- confirmed the trickiest part, as
        anticipated. Implemented by recording what's actually typed
        during the first row's Insert session (`block_insert_typed_`,
        updated live from `ProcessInsertKey`) and replicating it onto
        the other rows when Escape closes that session
        (`FinishVisualBlockInsert`), padding a short row with spaces
        first if the insert column falls past its current end
        (matching Vim's own block-`A` padding behavior). Same
        limitation Vim itself has: only plain typed characters and
        Backspace are replicated, not Enter/Delete/motions.
  - [x] `$` in Visual Block extends to each row's actual end (ragged
        right edge), matching Vim's "block to end of line" behavior --
        tracked as a "sticky" flag (`block_to_eol_`) set by `$` and
        cleared by any other column-changing motion, read by the yank/
        delete/insert/render code as "this row's real length" instead
        of a fixed column wherever it's set.
- [x] `o` — swap cursor to the other end of the current
      selection (works in charwise/linewise/block Visual — implemented
      once, in the shared motion-dispatch path, rather than per
      submodule).
- [x] Text objects usable directly in Visual mode to *set* the
      selection (`viw`, `vi(`) — **already present since Phase 2**,
      confirmed still working; no new work needed here.
- [x] `>` / `<` in Visual mode — indent/dedent the selected lines,
      reselecting afterward (Vim keeps you in Visual mode after `>`/`<`
      so you can repeat it) — **fixed a real gap**: this existed since
      before this plan but used to call `EnterNormal()` immediately
      after indenting, exiting the selection; now it leaves
      anchor/cursor untouched and stays in Visual mode, matching Vim.
- [x] `gv` — see Phase 5 (implemented there since it's really a
      mark/jump feature). Extended here so leaving a Visual Block
      selection also populates it -- with a known simplification: mep's
      gv memory only has room for a linewise/charwise bool, so `gv`
      after a block selection restores it as an ordinary charwise
      selection covering the same anchor/cursor, not the actual block
      shape.

**Known gap, not fixed in this phase**: Visual-mode operations
(charwise/linewise/block alike) don't participate in Phase 7's `.`
repeat or macro recording -- `HandleVisualInput` is a separate dispatch
loop from `HandleNormalInput`/`HandleInsertInput` and was never routed
through `ProcessNormalKey`/`ProcessInsertKey`. Retrofitting it would
also need real design work, not just plumbing: Vim's own dot-repeat of
a Visual change reapplies the operator over a same-*shaped* selection
at the new cursor position, not a literal keystroke replay. Left as an
explicit gap rather than attempted partially.

**Implementation note -- another real Xvfb-testing-caught bug, not a
design choice**: `Ctrl-V`'s entry check started out as
`IsKeyPressed(KEY_V) && ctrl`, mirroring `HandleNormalInput`'s existing
Ctrl-R/Ctrl-W checks, and was just as flaky under Xvfb as Phase 8's
Ctrl-W/Ctrl-U were before their fix. Fixed the same way: read `KEY_V`
off the `GetKeyPressed()` press queue instead, paired with a plain
`IsKeyDown()` ctrl check.

Verified via the actual native build under Xvfb: `Ctrl-V` entering
`-- V-BLOCK --` with a correct rectangular highlight; a ragged (`$`)
block selection rendering each row's highlight to its own actual end;
block `y` then `p` reproducing the yanked rectangle at the paste
point, including a case where the block's fixed right edge ran past a
shorter row's actual length -- confirmed that's *correct* clamping
(fewer characters from that row, not padding), not a bug, by working
through Vim's own documented behavior for exactly this case; block `I`
replicating a typed character onto every row of a left-aligned block;
block `A` with `$` correctly appending at each row's own end despite
the rows having different lengths; `o` swapping the visible cursor to
the opposite corner while leaving the highlighted range unchanged; and
`>` twice in `V-LINE` mode indenting by two levels total while staying
in Visual mode the whole time (`-- V-LINE --` still shown, selection
still highlighted, after each press).

## Phase 10 — Scrolling & misc normal-mode commands ✅

- [x] `Ctrl-D` / `Ctrl-U` — scroll half a screen down/up, moving the
      cursor with it (by the same number of lines, so it stays at
      roughly the same screen row).
- [x] `Ctrl-F` / `Ctrl-B` — scroll a full screen down/up. Both this and
      Ctrl-D/Ctrl-U reuse the same `p.visible_lines` field Phase 1's
      `H`/`M`/`L` already reads (set once per frame by the renderer),
      so no new plumbing was needed to know the screen size.
- [x] `zz` / `zt` / `zb` — reposition the view so the cursor's line is
      centered / at the top / at the bottom, without moving the cursor.
- [x] `Ctrl-A` / `Ctrl-X` — increment/decrement the first number at or
      after the cursor on the current line, with count (`5<C-a>` adds
      5). Preserves leading-zero padding the way Vim's default
      `nrformats` does (`030` → `031`, not `31`) and leaves the cursor
      on the number's last digit, matching Vim.

**Implementation note — same Xvfb-testing-caught bug as Phases 8/9,
same fix**: every new Ctrl-combo here (`Ctrl-D/U/F/B/A/X`, alongside
Phase 9's `Ctrl-V`) reads off the `GetKeyPressed()` press queue rather
than `IsKeyPressed()`, for the same same-frame-race reason documented
in Phase 8 and Phase 9's notes -- by this phase it's the established
pattern for *any* new Ctrl-combo, not something rediscovered each time.

Verified via the actual native build under Xvfb, on a 60-line file:
`Ctrl-D`/`Ctrl-F` scrolling down (and `Ctrl-U`/`Ctrl-B` back up) by
exactly half/a full screen, cursor and view moving together by the
same amount each time; `:30<Enter>` then `zz`/`zt`/`zb` each
repositioning line 30 to the vertical center/top/bottom of the view
without moving the cursor off it; `Ctrl-A` on `line 030 value 30`
producing `line 031 value 30` (leading zero preserved, cursor landing
on the new last digit); and `5<C-x>` on the trailing `30` producing
`25`.

## Phase 11 — Ex commands & polish ✅

- [x] `:normal {keys}` (a.k.a. `:norm`) — run `{keys}` as literal
      Normal-mode input; useful on its own and as the mechanism `:g`
      (Phase 6) commonly pairs with in real Vim usage. Implemented by
      feeding each character of `{keys}` through the same
      `ProcessNormalKey`/`ProcessInsertKey` entry points real keystrokes
      use (Phase 7's repeat/macro machinery), so it composes with
      counts, operators, registers, text objects, etc. for free, and
      auto-closes an unterminated Insert session at the end (matching
      Vim). Doesn't support driving Visual mode (see Phase 9's Visual/
      repeat gap note — same underlying reason: `HandleVisualInput`
      isn't wired into `Process*Key`); `:normal v...` enters Visual via
      the `v` but the rest of the keys are dropped and Visual mode is
      closed back out rather than acted on. No range-prefixed
      `:{range}normal` either — scoped out since `:g`'s own per-line
      iteration already covers that combination's real use case.
- [x] `:set` for a small, real set of options mep can actually honor:
      `number`/`nonumber` (line numbers in the gutter), `ignorecase`/
      `noignorecase`, `wrapscan`/`nowrapscan` (on by default, matching
      Vim). Also accepts `nu`/`ic`/`ws` short forms and multiple options
      space-separated on one `:set` line. `ignorecase` is centralized
      in two small `CiFind`/`CiRfind` wrappers around `find`/`rfind`
      that `SearchOnce` (Phase 4), `ExSubstitute`, and `ExGlobal`
      (Phase 6) all now call instead of the raw `std::string` methods,
      so all three honor it automatically rather than needing their own
      copies of the option check.
- [x] Line numbers in the gutter (relates to `:set number` above; the
      one rendering change in this plan that lives in `main.cpp` rather
      than `editor.cpp` — a right-aligned gutter sized to the buffer's
      largest line number, shifting the existing selection-highlight/
      cursor/text x-coordinates over rather than duplicating them).
- [x] Command-line history (Up/Down while in Command mode recalls
      previous `:` commands) and search history similarly for `/`/`?`.
      **Implemented with Up/Down, not Ctrl-P/Ctrl-N** as the plan
      originally suggested — Up/Down is the more discoverable, more
      conventional binding for this (shell/browser address bar history
      both use it), and doesn't collide with anything else already
      bound in Command/Search mode the way Ctrl-P might in the future
      (e.g. fuzzy-file-open). Each history is its own small
      vector-plus-index pair (`command_history_`/`cmd_history_index_`,
      `search_history_`/`search_history_index_`), with a "what was
      being typed before Up" slot so Down past the newest entry returns
      to it instead of an empty line, the same shape shell history uses.
- [x] Update `README.org`'s "Not a Vim clone in the full sense" section
      now that the phases it specifically called out (registers, marks,
      text objects, macros — everything except regex search, which
      remains a deliberate, documented non-goal per Phase 4/6's own
      design decisions) are addressed, to keep the README honest about
      current scope. Rewrote the intro paragraph and the `*** Keys`
      section in full to cover Phases 0–11 rather than only the
      pre-plan baseline.

**Bug fixed along the way, not part of the original scope**: `:s`
(`ExSubstitute`) was calling `PushUndo()` unconditionally before
scanning for matches, so a substitute that matched nothing (which
correctly reports `E486: Pattern not found` and touches no text) was
still pushing a wasted, do-nothing snapshot onto the undo stack. Fixed
by deferring the `PushUndo()` call until the first real match is found,
noticed while touching this function for `ignorecase`.

Verified via the actual native build under Xvfb: `:set number` showing
a correct line-number gutter; `:normal dwA!!!` deleting a word then
appending text and correctly returning to Normal mode afterward (not
left stuck in Insert); `:set ignorecase` then `/ALPHA<Enter>` finding a
lowercase `alpha`; and, in Command mode, Up recalling the most recent
`:` command, a second Up recalling the one before that, and Down
returning to the first — confirmed via the actual command text shown
in the command bar at each step, not just that *something* changed.

---

## Explicitly out of scope (noted so it isn't re-litigated)

- **Folding** (`zf`, `za`, etc.) — significant rendering + editor-state
  complexity for a feature mep's own likely usage (a lightweight
  personal editor) rarely needs. Revisit only on explicit request.
- **Tag jumping / `Ctrl-]`**, **spell check**, **diff mode**, **netrw
  file explorer** — all substantial standalone subsystems, well beyond
  "editing parity."
- **Full Vimscript compatibility** — mep has real Lua instead by design
  (see README); not a gap to close.
