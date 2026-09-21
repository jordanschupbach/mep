# Learn: org-file-driven learning games

An org file is the question database ("deck"); each game is a projection of
that same deck into a randomized session. `kBuiltinLearn` (src/main.cpp)
holds the deck parser, the shared session shell, the reusable question
engines and every game. `examples/learn_basic_datastructures.org` is the
reference deck and documents the format in its header comment.

This plan lists every game the deck format can drive, what data each one
needs, how the org format grows to feed it, and which are implemented.

## 1. Deck format

### 1.1 Base (implemented)

```org
#+LEARN_TITLE: Basic Data Structures
#+LEARN_TAG: card                 ; headline tag marking a card (default)
#+LEARN_CHOICES: 4                ; options per choice question
#+LEARN_DIRECTION: both           ; term | definition | both (flashcards)
#+LEARN_ROUNDS: 0                 ; questions/rounds per session, 0 = all
#+LEARN_MATCH_SIZE: 5             ; pairs per matching round

* Category headline                ; :CATEGORY: default for its children
** Term                                            :card:
:PROPERTIES:
:CATEGORY:    override the parent-headline category
:QUESTION:    custom prompt instead of the definition
:HINT:        shown after a miss
:DISTRACTORS: extra wrong answers, |-separated
:ALIASES:     accepted alternative names, |-separated
:HIDE:        identifiers to blank out in the card's code, |-separated
:END:
Definition paragraphs (the body, minus blocks/keywords/planning lines).

#+begin_src python                 ; the card's implementation (card.code)
class Term: ...
#+end_src
```

A deck with no tagged headline treats every headline with a body as a card.
`deck.cards[i]` = `{term, definition, paragraphs, category, hint, question,
distractors, aliases, hide, code, tags, props, line, level}`.

### 1.2 Amendments (this plan)

| Addition | Org form | Feeds |
|---|---|---|
| Cloze markup | `{{answer}}` or `{{answer\|hint}}` anywhere in a card's prose | Fill in the blank |
| Tagged src blocks | `#+begin_src python :learn cloze` (code with `{{...}}`), `:learn bug :line N` (a buggy variant), `:learn output` followed by a `#+RESULTS:` drawer (`: line` fixed-width) | Complete the code, Spot the bug, Predict the output |
| Fact properties | any drawer property listed in `#+LEARN_FACTS: ACCESS INSERT SEARCH` (or, unlisted, any key ≥ 4 cards share that isn't reserved) | Fact quiz, Mixed |
| Fact tables | a 2-column org table in the body, `\| key \| value \|` | Fact quiz (same as properties) |
| Steps | an ordered list (`1. `, `2. `) in the body | Put in order |
| Points | `:POINTS: 300` (default: 100, 200, ... by position within the category) | Jeopardy |
| Time limit | `#+LEARN_TIME_LIMIT: 10` seconds, or `opts.time_limit` | Timed mode on any choice game |
| Rank keys | `#+LEARN_RANK: YEAR` (else any numeric fact on ≥ 3 cards) | Rank by fact |
| Images | `[[file:path.png]]` on its own line in the body (relative to the deck file) | Picture quiz, Mixed |
| SRS | `#+LEARN_SRS: yes`; per-card `:DRILL_DUE:` etc. written by grading | due-first dealing |
| Mixed weights | `#+LEARN_MIXED: mc code tf cloze facts oddone category` | Mixed practice |

All additions are optional and backwards compatible: a deck that uses none
of them still plays every game whose data it has, and a game whose data is
missing refuses with a one-line notice naming what the deck needs.

Everything the parser produces is reachable from Lua (`mep.learn_parse_deck`)
so further games -- or an external tool -- can build on the same files.

## 2. Engines (shared code)

| Engine | What it gives a game | Games on it |
|---|---|---|
| Session shell | one `Learn` pane, `on_key`/click routing, restart/quit/open-card, end-of-session summary with a review list | all |
| Choice engine | `st.questions` of `{prompt..., choices, answer}`, digit/click answering, streaks, feedback, per-question prompt/reveal hooks, optional per-question timer | flashcards, identify the code, true/false, cloze, complete the code, fact quiz, odd one out, which category, predict the output, mixed, jeopardy questions |
| Typed-answer engine | `mep.ui_input` prompt, alias-aware normalized comparison, near-miss detection | type the term, cloze (typed mode) |
| Selection engine | pick rows by key/click, pair or sequence them, submit as a whole | matching, put in order, spot the bug |
| Board | category x value grid with used-cell tracking | jeopardy |

## 3. Games

Status: **done** = shipped before this plan, **now** = implemented by this
plan (rows 4-17 on 2026-09-17, rows 18-26 on 2026-09-18). Every row is
built.

| # | Game | Data needed | Interaction | Status |
|---|---|---|---|---|
| 1 | Multiple-choice flashcards | cards | 1-9 / click | done |
| 2 | Matching | cards | digit+letter pairs, submit | done |
| 3 | Identify the code | cards with a src block | 1-9 / click | done |
| 4 | True or false | cards (auto: term paired with its own or a same-category definition) | 1/2, t/f | now |
| 5 | Fill in the blank (cloze) | `{{answer}}` in prose | 1-9 / click; `t` types the answer instead | now |
| 6 | Type the term | cards, `:ALIASES:` | `t`/Enter opens a prompt, typed answer graded | now |
| 7 | Odd one out | ≥ 2 categories (one with ≥ 3 cards) | 1-9 / click | now |
| 8 | Which category? | ≥ 2 categories | 1-9 / click | now |
| 9 | Fact quiz | `#+LEARN_FACTS:` properties or fact tables | 1-9 / click | now |
| 10 | Put in order | ordered lists in card bodies | letters in sequence, `u` undo, auto-grades on the last pick | now |
| 11 | Jeopardy | cards, categories, `:POINTS:` | letter+digit picks a cell (or click), then a choice question; wrong answers cost points | now |
| 12 | Hangman | cards | a-z guesses; `Q`/`R`/`O` are the shell keys while letters are taken | now |
| 13 | Complete the code | `:learn cloze` blocks | 1-9 / click | now |
| 14 | Spot the bug | `:learn bug :line N` blocks | j/k + Enter or click on the line, digits then `s` | now |
| 15 | Predict the output | `:learn output` blocks + `#+RESULTS:` | 1-9 / click | now |
| 16 | Mixed practice | whatever the deck has | per-question keys | now |
| 17 | Timed mode | `#+LEARN_TIME_LIMIT:` | countdown in the header; expiry counts as a miss | now (choice games) |
| 18 | Spaced repetition | org-drill's `:DRILL_*` properties (`mep.org_drill_grade`) | `#+LEARN_SRS: yes` deals due cards first; `g` on any summary grades the session (Good/Again) | now |
| 19 | Which code is it (term -> code) | src blocks | four anonymized snippets, 1-9 / click | now |
| 20 | Survival | cards | any choice game (default mixed) until the first miss | now (`opts.survival`) |
| 21 | Sort into buckets | ≥ 2 categories | digit+letter places a term in a category, submit | now |
| 22 | Rank by fact | numeric properties (`#+LEARN_RANK: YEAR`, or any numeric fact on ≥ 3 cards) | ordering engine across cards, ties accepted either way | now |
| 23 | Picture quiz | `[[file:...]]` on its own line in the body | the image (sidebar image rows) with four terms to choose from; `t` types | now |
| 24 | Crossword | terms (3+ letters) + definitions | a text grid with spans; pick a clue (number + a/d, click, `n`), type its answer (`t`), `s` checks | now |
| 25 | Hot-seat two player | any | the shell alternates turns per question/round/cell and keeps two scores | now (`opts.players = 2`) |
| 26 | Relation quiz | `:BUILT_ON:` properties naming other terms | the fact quiz asks "Term -- built on?" | now (deck content) |

### 3.1 Per-game notes

**True or false.** Each card yields one statement: "Term -- definition". Half
the statements swap in a same-category definition (else any other card's).
Reveal names the real term for a false statement.

**Cloze.** `{{answer|hint}}` in any paragraph. One question per blank;
the sentence is shown with `____`, the choices are the answer plus other
cloze answers (same category first), then terms. `t` opens a typed prompt
instead; the typed answer is graded by the typed-answer engine.

**Type the term.** Prompt is the definition (or `:QUESTION:`). The typed text
is normalized (case, whitespace, punctuation) and compared with the term and
`:ALIASES:`; an edit distance ≤ 1 on a word of ≥ 5 letters counts as a
near miss (accepted, flagged). Skipping (`n`) counts as a miss.

**Odd one out.** Three cards from one category plus one from another; the
reveal shows every card's category.

**Fact quiz.** `#+LEARN_FACTS:` names the properties (else auto-detected:
keys ≥ 4 cards share, minus the reserved ones). A 2-column table in a body
contributes rows the same way. Question "Term -- KEY?"; choices are distinct
values other cards have for that key.

**Put in order.** A card's first ordered list is one sequence ("Put the
steps of Term in order"). Items are shown shuffled with letters; pressing
letters builds the order, `u` removes the last pick, the sequence grades
itself when complete (score = items in the right position).

**Jeopardy.** Board rows per category, cells by value; `a3` (letter row,
digit value) or a click opens the cell's question -- the card's
`:QUESTION:` or definition with four terms to choose from. Right adds the
value, wrong subtracts it, both mark the cell used. The session ends when
the board is empty or on `n` past the last cell.

**Hangman.** The definition is the clue, the term is the word (letters
blank, other characters shown), 6 misses lose the word. Letters are consumed
by the game, so the shell keys are uppercase `Q`/`R`/`O` here.

**Complete the code.** A `:learn cloze` block with `{{...}}` on one or more
lines; each blank is one question showing the block (Treesitter-highlighted,
blank as `____`) with the answer among the deck's other cloze code answers.

**Spot the bug.** A `:learn bug :line N` block; lines are numbered rows,
pick the wrong one (j/k + Enter, click, or digits then `s`). The reveal
shows the card's real code for comparison.

**Predict the output.** A `:learn output` block followed by `#+RESULTS:`;
the card's main block is shown above it for context. Choices are the other
cards' results.

**Mixed practice.** Draws from every choice-engine generator the deck can
feed (`#+LEARN_MIXED:` picks the subset), shuffles, caps by `#+LEARN_ROUNDS:`.
Each question keeps its own prompt/reveal style.

**Timed mode.** `#+LEARN_TIME_LIMIT: N` (or `opts.time_limit`) on any
choice game: the header shows the seconds left, re-rendered once a second
from `mep.on_frame`; hitting zero answers the question wrong.

**Spaced repetition.** Every game records the cards it asked about; the
summary offers `g`, which grades each (SM-2 quality 4 for a right answer,
1 for a miss) through `mep.org_drill_grade` into the card's `:DRILL_*:`
properties -- bottom-up, since a grade can grow a drawer, then the deck is
re-read so the session's line numbers stay right. `#+LEARN_SRS: yes` makes
every deal put due (or never-graded) cards first, so a `#+LEARN_ROUNDS:`
cap reviews what's due.

**Which code is it.** The mirror of identify-the-code: the term (and its
definition) is the prompt, four anonymized snippets are listed with their
headers as the choices; answering restores every snippet's names and owner.

**Sort into buckets.** Rounds of `#+LEARN_MATCH_SIZE:` terms and the deck's
categories as lettered buckets; many terms may share one. Graded on submit.

**Rank by fact.** `#+LEARN_RANK:` keys read straight from the drawer (a
`:YEAR:` used only for ranking needn't be a quiz fact); without the
keyword, any numeric fact on three or more cards. Rounds of
`#+LEARN_MATCH_SIZE:` cards on the same ordering engine as put-in-order;
equal values grade right in either order and the reveal shows them.

**Picture quiz.** `[[file:...]]` links on their own line are the card's
pictures (resolved against the deck's directory) and never prose. The
sidebar widget model gained image rows for this: a widget with `image =
path, image_rows = n` occupies n rows and every renderer (pane, docked,
popout) draws the texture from the org inline-image cache scaled into that
box. `examples/learn_images/` holds ten generated diagrams with no term
names in them.

**Crossword.** `mep.learn_crossword_build(cards, max)` lays terms
(letters only, 3-16 of them) out greedily: longest first, then each word
on the crossing letter that gives the most crossings, perpendicular to the
word it crosses, never side-on to another; what doesn't fit is skipped.
The grid is plain text rows with spans (`_` open, letters typed in cyan,
the selected clue's cells yellow; after checking green/red), clues are
listed across then down and are clickable; the answer for a selected clue
is typed through `mep.ui_input`. `s` grades every word; the reveal fills
the grid and names each clue's term.

**Survival / hot-seat.** Modifiers, not games: `opts.survival` ends a
choice game at the first miss (score = questions survived);
`opts.players = 2` makes the shell attribute score changes to whoever's
turn it is and pass the turn whenever the game's position (question,
round, or used jeopardy cell) advances, with a two-score banner and a
verdict on the summary. `:LearnSurvival [game]` / `:LearnHotSeat [game]`
default to mixed practice.

## 4. Implementation notes

Everything marked **now** shipped together in `kBuiltinLearn` (split into
`kBuiltinLearnParts[]`, joined at load, because the chunk passed the 64 KB
string-literal limit the compiler enforces under `-Werror`). Entry points:
`:Learn` / `<leader>ol` picks a game; each game is a `:Learn*` command, a
`<leader>og?` key (`mep.learn_catalog` is the table of record) and a bare
global (`LearnJeopardy()`, ...) for a deck's own `mep-lua` launcher blocks.
`mep.learn_games` holds the game tables; `mep.learn_state()` exposes the
live session.

Verification lives outside the C++ test binaries: the chunk is developed
as a plain Lua file, exercised by a stub-`mep` script that drives every
game through its `on_key` handler (parsing, generators, flows, validation
notices, the timer via a fake clock), then embedded and checked live over
the agent socket. The **later** rows are the natural next steps; the SRS
one only needs the deck to be the current buffer when grading
(`mep.org_drill_grade` works on the live buffer).
