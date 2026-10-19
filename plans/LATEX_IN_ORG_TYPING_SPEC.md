# Prompt: implement fast hand-typed LaTeX math in Org-mode buffers

You are working in a text-editor codebase. Your job is to reproduce, as exactly as
possible, a LaTeX-in-Org typing system that currently exists as a Neovim config
(LuaSnip + nvim-cmp + a few custom Lua modules). The goal is **speed**: the writer
types math by hand during lectures, so every keystroke saved matters, the caret
must always land in the slot they'll type into next, and `<Tab>` must walk them
through the slots and then out of the construct, with no reaching for arrow keys.

Below is the full behavioral spec: the concepts, the math-zone detector, trigger
matching rules, every trigger and its exact expansion with tab-stop positions,
the Tab/completion key model, and the surrounding ergonomics. Where the
original has bugs, they're called out. Fix them rather than copying them.

First, survey your own editor's existing snippet, completion, and keybinding
infrastructure and map each concept below onto it. Build missing pieces
(e.g. autosnippets, conditional triggers, regex triggers, nested snippet
sessions) as general mechanisms, not one-offs.

---

## 1. Core concepts

### 1.1 Two kinds of snippets

| Kind | How it fires | Used for |
|---|---|---|
| **Autosnippet** | Expands **the instant the last character of the trigger is typed**. No key press, no menu. | All math shorthand (`//`, `sum`, `;a`, `->`, …) and the math-zone openers (`mk`, `dk`, `ck`, `flalign`). |
| **Regular snippet** | Shows up in the completion menu as soon as the trigger is typed (min. 1 char); the user confirms it (`<C-y>`, or `<CR>` on a selected entry) to expand. | Org structure (`src`, `title`, `eqn`, …) and TikZ graphs. Kept manual so ordinary English words ("quote", "date", "graph") never expand by accident. |

### 1.2 Tab stops, placeholders, exit point

Notation used throughout this document:

- `⟨1:num⟩`: tab stop 1, pre-filled with the placeholder text `num`.
- `⟨1⟩`: tab stop 1, empty.
- `⟨0⟩`: the **final exit position**. When a snippet has no explicit `⟨0⟩`, there is
  an implicit one **immediately after the last character of the expansion**.
- `⟨=1⟩`: a **mirror** of tab stop 1. It updates live, on every keystroke, as stop 1 is typed.
- All backslashes shown are **literal output characters**, not escapes.
  A newline inside an expansion is shown as an actual line break; indentation is literal (2 spaces).

Required behavior:

1. On expansion the caret jumps to stop 1. If stop 1 has placeholder text, that
   text is **selected** so the first typed character **replaces** it (Vim
   "select mode" / VS Code placeholder selection).
2. `<Tab>` → next stop (its placeholder selected). `<S-Tab>` → previous stop.
3. Pressing `<Tab>` on a stop **without typing** keeps the placeholder text
   as real content. That's intentional: defaults like `\infty` in `lim`, or
   `\text{otherwise}` in `cases`, are usually what you want.
4. `<Tab>` from the last stop jumps to `⟨0⟩`, which is usually just past the closing
   `}`. This is the "jump out of the braces" move and it matters most for speed:
   `//` `a` Tab `b` Tab leaves the caret after `\frac{a}{b}`, ready for the next term.
5. Once at `⟨0⟩` the snippet session ends. From then on `<Tab>` is a literal Tab again.

### 1.3 Nested snippets

Snippets **nest**: expanding a snippet while sitting in another snippet's tab
stop starts a child session. `<Tab>` walks the child's stops, the child's
`⟨0⟩` leads back into the parent, and the next `<Tab>` continues to the parent's
next stop. Example, typing in a math zone:

```
//  →  \frac{|num}{den}            (caret on stop 1 of the outer frac)
//  →  \frac{\frac{|num}{den}}{den}   (inner frac nested inside outer stop 1)
a Tab b Tab      → inner frac done, caret back in the outer numerator after the inner frac
Tab c Tab        → outer denominator = c, caret exits past the final }
Result: \frac{\frac{a}{b}}{c}|
```

The original config sets `keep_roots`, `link_roots`, `link_children`. Together they
mean you can also `<S-Tab>` back into a finished sibling or parent snippet that's
still in the buffer. Implement nesting at minimum; cross-snippet back-jumping is
nice to have.

### 1.4 Choice stops

Some regular snippets have a **choice stop**: one slot whose content cycles through
a fixed list (e.g. the source-block language). The active choice stop should show
a virtual-text hint (the original shows `choiceNode` in Comment highlight). **Gap
in the original:** it has no key bound to cycle choices. The docs say `<C-n>`, but
`<C-n>` is owned by the completion menu. Bind a dedicated cycle key that works only
while a choice stop is active (e.g. `<C-j>`/`<C-k>`, or `<C-n>`/`<C-p>` *only when no
completion menu is visible*).

---

## 2. Math-zone detection (gates every math autosnippet)

Math autosnippets must fire **only inside math**. Writing "the sum is large" in prose
must not turn into `\sum`. This is the most important correctness piece.

Detection works by **text scan, not syntax highlighting**. Org buffers aren't
tex-highlighted, so a syntax-group check doesn't work there. The algorithm runs on
every candidate expansion:

```
input: buffer lines from max(0, cursorRow-500) up to cursorRow,
       last line truncated at the cursor column (text before caret only)
state: inline=false, display=false, envDepth=0

for each line:
  if line is blank (only whitespace) OR line is an org heading (^\*+\s):
      inline=false; display=false          # paragraph boundary heals a stray $
  text = line with every "\\" removed, then every "\$" removed
  scan text left→right:
    "$$"           → display = !display     (consume 2)
    "\(" or "\["   → display = true         (consume 2)
    "\)" or "\]"   → display = false        (consume 2)
    "$"            → inline = !inline       (consume 1)
    "\begin{NAME}" or "\begin{NAME*}" with NAME in MATH_ENVS → envDepth++   (skip to after })
    "\end{NAME}"   or "\end{NAME*}"   with NAME in MATH_ENVS → envDepth = max(0, envDepth-1)
    else consume 1
in_math = inline OR display OR envDepth > 0

MATH_ENVS = equation align alignat gather multline flalign math displaymath array
            cases matrix pmatrix bmatrix Bmatrix vmatrix Vmatrix smallmatrix
            split aligned gathered
```

Notes:

- `envDepth` is **not** reset at blank lines, but `inline`/`display` are.
- The 500-line bound keeps the per-keystroke cost small. Cache or incrementalize if your editor makes this expensive.
- **Known limitation, keep it or improve it:** text inside `\text{…}` counts as math,
  so triggers fire there too. Typing `\text{independent}` would mangle `in`. The
  original works around this by capitalizing risky triggers (`Ind`, `Pr`, `Var`). An
  improvement would be to treat the inside of `\text{…}`/`\mathrm{…}` as non-math, but
  if you do, make it explicit and documented.
- The same `in_math` function is reused, negated, as a "prose only" gate for one org snippet (`cbl`).

---

## 3. Trigger-matching rules

Each autosnippet has these attributes:

| Attribute | Meaning |
|---|---|
| `condition = in_math` | Expansion is suppressed outside math (and the snippet is also hidden from the completion menu outside math). |
| **word-trigger** (default ON) | Fires only if the character immediately **before** the trigger is not a word character (`[A-Za-z0-9_]`), or the trigger is at line start. This is why `nsum` doesn't fire `sum`, `argmin` doesn't fire `min`, `notin` doesn't fire `in`, `ounder` doesn't fire `under`, and `vnorm` doesn't fire `norm`. |
| word-trigger OFF | Used for **symbolic** triggers (`//`, `^^`, `__`, `->`, `!=`, `<=`, `>=`, `~=`, `+-`, `=>`, `:=`, `xx`, `...`, `AA`, `EE`, `lr(`, `lr[`, all `;x` Greek). These fire right after any character, e.g. `a//` or `x->`. |
| case-sensitive | `Pr` ≠ `pr`, `Var` ≠ `var`, `RR` ≠ `rr`. Capitalized triggers are deliberate: they avoid colliding with English words in `\text{}`. |

On expansion the typed trigger text is **removed** and replaced by the expansion.

### 3.1 Prefix collisions and the "absorb backslash" rule (important)

Because autosnippets fire immediately, a trigger that starts with another trigger
gets pre-empted. Example: `in` → `\in` fires after `i`,`n`. Then typing `t` gives
`\int`, the `int` trigger sees a non-word char (`\`) before it, fires, and you get
`\\int…`, with a doubled backslash.

The fix in the original, `ms_absorb(word)`: the trigger is the regex `\\?word`
(optional leading backslash). Word-trigger is replaced by a manual check that the
character before the **whole match** (including the optional `\`) isn't
`[A-Za-z0-9\\]`. On expansion the absorbed backslash is deleted too, so `\in`+`t`
collapses cleanly into the `int` expansion. Applied to `int` and `indep`.

Implement this as a **general rule**: for every autosnippet trigger T that has a
proper prefix P which is itself an autosnippet trigger expanding to `\P`-ish text,
use the absorb form. **Bugs in the original you should fix:**

- `inf` (→ `\infty`) is **not** absorbed, so `in`+`f` produces `\\infty`. Apply the absorb rule.
- `;p`→`\pi` fires before `;ph`/`;ps` can be completed, so `\phi`/`\psi` are
  **unreachable** (you get `\pih`, `\pis`). Same for `;P` vs `;Ph`/`;Ps`. Fix it either by
  absorbing (regex `\\pi` + `h` → `\phi`, i.e. trigger on the *expanded* prefix), or
  by choosing non-colliding keys (e.g. `;f`→`\phi`, `;y`→`\psi`, `;F`/`;Y` upper). Check
  the whole Greek table for other prefix clashes after you choose.
- Audit the full trigger list programmatically for any other "T has an autosnippet prefix" cases.

### 3.2 Regex autosnippet: auto-subscript

- Trigger regex `([A-Za-z])(\d)`, word-trigger OFF, math only.
- Expansion: `<letter>_{<digit>}`, so `x1` → `x_{1}`, `b0` → `b_{0}`. No tab stops; caret after `}`.
- Single digit only: `x12` → `x_{1}2`. (Keep it or extend it to `x_{12}` via
  a second rule on `_{d}d`; document which you pick.)

---

## 4. Math-zone openers (autosnippets, NOT math-gated, word-trigger ON)

These **open** math, so they fire from prose. Word-trigger keeps them out of
the middle of words (no English word starts with "ck"; "mk"/"dk" are rare).

| Trigger | Expansion |
|---|---|
| `mk` | `$⟨1⟩$` (inline; caret between the dollars; Tab exits past the closing `$`) |
| `dk` | `\(`⏎`  ⟨1⟩`⏎`\)` (opener on its own line, body indented 2 spaces) |
| `ck` | `\[`⏎`  ⟨1⟩`⏎`\]` |
| `flalign` | `\begin{flalign*}`⏎`  ⟨1:a⟩ &= ⟨2:b⟩ & \\`⏎`  ⟨3:c⟩ &= ⟨4:d⟩ &`⏎`\end{flalign*}` |

Why `dk` vs `ck`: in Org, an opener **on its own line** forms a `latex_env`
element that the editor renders as an inline image. On export, `\( \)`
stays in the text flow (left, uncentered) and `\[ \]` is centered display.
`mk` stays inline and isn't image-rendered.

Org-only **regular** (menu) twins of these, same expansions:
`mm` → `$⟨1⟩$`, `dm` → `\(`⏎`  ⟨1⟩`⏎`\)`, `cm` → `\[`⏎`  ⟨1⟩`⏎`\]`.

---

## 5. Math autosnippets (gated by `in_math`)

Word-trigger is ON unless the row says **sym** (word-trigger OFF).

### 5.1 Fractions, scripts, roots

| Trigger | Expansion |
|---|---|
| `//` sym | `\frac{⟨1:num⟩}{⟨2:den⟩}` |
| `frac` | `\frac{⟨1:num⟩}{⟨2:den⟩}` |
| `^^` sym | `^{⟨1:exp⟩}` |
| `__` sym | `_{⟨1:idx⟩}` |
| `sq` | `\sqrt{⟨1:x⟩}` |
| `nrt` | `\sqrt[⟨1:n⟩]{⟨2:x⟩}` |
| `binom` | `\binom{⟨1:n⟩}{⟨2:k⟩}` |

### 5.2 Big operators / calculus

| Trigger | Expansion |
|---|---|
| `sum` | `\sum_{⟨1:i=1⟩}^{⟨2:n⟩} ⟨3:a_i⟩` |
| `prod` | `\prod_{⟨1:i=1⟩}^{⟨2:n⟩} ⟨3:a_i⟩` |
| `int` (absorb) | `\int_{⟨1:a⟩}^{⟨2:b⟩} ⟨3:f(x)⟩ \, d⟨4:x⟩` |
| `lim` | `\lim_{⟨1:n⟩ \to ⟨2:\infty⟩} ⟨3:a_n⟩` |
| `dd` | `\frac{d⟨1:y⟩}{d⟨2:x⟩}` |
| `part` | `\frac{\partial ⟨1:f⟩}{\partial ⟨2:x⟩}` |
| `nsum` | `\sum_{i=1}^{n}` |
| `nprod` | `\prod_{i=1}^{n}` |
| `bigcup` | `\bigcup_{⟨1:i=1⟩}^{⟨2:n⟩}` |
| `bigcap` | `\bigcap_{⟨1:i=1⟩}^{⟨2:n⟩}` |
| `argmax` | `\underset{⟨1:\theta⟩}{\arg\max}\;` |
| `argmin` | `\underset{⟨1:\theta⟩}{\arg\min}\;` |
| `log` `exp` `min` `max` | `\log` `\exp` `\min` `\max` (bare, so `\min_{…}` composes via `__`) |
| `pd` | `\partial` |
| `grad` | `\nabla` |

### 5.3 Accents, fonts, text

| Trigger | Expansion |
|---|---|
| `bar` `hat` `vec` | `\bar{⟨1:x⟩}` `\hat{⟨1:x⟩}` `\vec{⟨1:v⟩}` |
| `tt`, `txt` | `\text{⟨1:text⟩}` |
| `over` | `\overset{⟨1:above⟩}{⟨2:x⟩}` |
| `under` | `\underset{⟨1:below⟩}{⟨2:x⟩}` |
| `ounder` | `\overset{⟨1:above⟩}{\underset{⟨2:below⟩}{⟨3:x⟩}}` |
| `cal` | `\mathcal{⟨1:L⟩}` |
| `bf` | `\mathbf{⟨1:x⟩}` |
| `rm` | `\mathrm{⟨1:op⟩}` |
| `bb` | `\mathbb{⟨1:R⟩}` |
| `xbar` `bhat` `thhat` `phat` | `\bar{x}` `\hat{\beta}` `\hat{\theta}` `\hat{p}` |
| `RR` `ZZ` `NN` `QQ` `CC` | `\mathbb{R}` … `\mathbb{C}` |
| `xn` `xi` `xj` | `x_{n}` `x_{i}` `x_{j}` |

### 5.4 Delimiters

| Trigger | Expansion |
|---|---|
| `abs` | `\lvert ⟨1:x⟩ \rvert` |
| `vnorm` | `\lVert ⟨1:v⟩ \rVert` |
| `nrm` | `\left\lVert ⟨1:x⟩ \right\rVert` |
| `floor` | `\lfloor ⟨1:x⟩ \rfloor` |
| `ceil` | `\lceil ⟨1:x⟩ \rceil` |
| `set` | `\{ ⟨1:x⟩ \}` |
| `lr(` sym | `\left( ⟨1:x⟩ \right)` |
| `lr[` sym | `\left[ ⟨1:x⟩ \right]` |
| `lbrace` / `rbrace` | `\left\{` / `\right\}` (placed separately) |

### 5.5 Relations & symbols (no tab stops; caret lands right after the output)

| Trigger | Out | Trigger | Out | Trigger | Out |
|---|---|---|---|---|---|
| `->` sym | `\to` | `!=` sym | `\neq` | `>=` sym | `\geq` |
| `<=` sym | `\leq` | `~=` sym | `\approx` | `+-` sym | `\pm` |
| `=>` sym | `\implies` | `:=` sym | `\coloneqq` | `xx` sym | `\times` |
| `...` sym | `\ldots` | `AA` sym | `\forall` | `EE` sym | `\exists` |
| `equiv` | `\equiv` | `iff` | `\iff` | `prop` | `\propto` |
| `deq` | `\triangleq` | `inf` (**absorb**, see 3.1) | `\infty` | `cdot` | `\cdot` |
| `nn` | `\cap` | `uu` | `\cup` | `smin` | `\setminus` |
| `subs` | `\subseteq` | `sups` | `\supseteq` | `empty` | `\emptyset` |
| `oplus` | `\oplus` | `otimes` | `\otimes` | `odot` | `\odot` |
| `land` | `\land` | `lor` | `\lor` | `and` | `\wedge` |
| `or` | `\vee` | `neg` | `\neg` | `in` | `\in` |
| `notin` | `\notin` | `given`, `mid` | `\mid` | `sim` | `\sim` |
| `iid` | `\overset{\text{iid}}{\sim}` | `perp` | `\perp` | `iperp` | `\perp\!\!\!\perp` |
| `indep` (absorb) | `\perp\!\!\!\perp` | `convp` | `\xrightarrow{p}` | `convd` | `\xrightarrow{d}` |

### 5.6 Statistics

| Trigger | Expansion |
|---|---|
| `Ev` | `\mathbb{E}\left[ ⟨1:X⟩ \right]` |
| `Var` | `\mathrm{Var}\left( ⟨1:X⟩ \right)` |
| `Cov` | `\mathrm{Cov}\left( ⟨1:X⟩, ⟨2:Y⟩ \right)` |
| `Cor` | `\mathrm{Corr}\left( ⟨1:X⟩, ⟨2:Y⟩ \right)` |
| `Pr` | `P\left( ⟨1:A⟩ \right)` |
| `cond` | `P\left( ⟨1:A⟩ \mid ⟨2:B⟩ \right)` |
| `Ind` | `\begin{cases} 1 & ⟨1:condition⟩ \\ 0 & \text{otherwise} \end{cases}` |
| `norm` | `\mathcal{N}\left( ⟨1:\mu⟩, ⟨2:\sigma^2⟩ \right)` |
| `Pois` | `\mathrm{Poisson}(⟨1:\lambda⟩)` |
| `Bin` | `\mathrm{Binomial}(⟨1:n⟩, ⟨2:p⟩)` |
| `Unif` | `\mathrm{Uniform}(⟨1:a⟩, ⟨2:b⟩)` |
| `Bern` | `\mathrm{Bernoulli}(⟨1:p⟩)` |
| `Gam` | `\mathrm{Gamma}(⟨1:\alpha⟩, ⟨2:\beta⟩)` |
| `Expo` | `\mathrm{Exponential}(⟨1:\lambda⟩)` |
| `Beta` | `\mathrm{Beta}(⟨1:\alpha⟩, ⟨2:\beta⟩)` |
| `Geom` | `\mathrm{Geometric}(⟨1:p⟩)` |

### 5.7 Environments (multi-line; 2-space body indent)

| Trigger | Expansion |
|---|---|
| `beg` | `\begin{⟨1:env⟩}`⏎`  ⟨2:body⟩`⏎`\end{⟨=1⟩}` (the end name **mirrors** stop 1 live) |
| `ali` | `\begin{align*}`⏎`  ⟨1:a &= b⟩`⏎`\end{align*}` |
| `aeq` | `\begin{align*}`⏎`  ⟨1:a⟩ &= ⟨2:b⟩ \\`⏎`  &= ⟨3:c⟩`⏎`\end{align*}` |
| `cases` | `\begin{cases}`⏎`  ⟨1:a⟩ & ⟨2:condition⟩ \\`⏎`  ⟨3:b⟩ & ⟨4:\text{otherwise}⟩`⏎`\end{cases}` |
| `bmat` | `\begin{bmatrix}`⏎`  ⟨1:a & b⟩`⏎`\end{bmatrix}` |
| `pmat` | `\begin{pmatrix}`⏎`  ⟨1:a & b⟩`⏎`\end{pmatrix}` |
| `cvec` | `\begin{bmatrix}`⏎`  ⟨1:a⟩ \\`⏎`  ⟨2:b⟩`⏎`\end{bmatrix}` |

Multi-line expansions should be inserted relative to the current line's
indentation (each continuation line gets the trigger line's leading
whitespace prepended), like LuaSnip does.

### 5.8 Greek (`;` prefix, sym, math only)

Lower: `;a` α `;b` β `;g` γ `;d` δ `;e` ε (`\epsilon`) `;z` ζ `;h` η `;th` θ `;k` κ
`;l` λ `;m` μ `;n` ν `;x` ξ `;p` π `;r` ρ `;s` σ `;ta` τ `;ph` φ `;ch` χ `;ps` ψ `;o` ω.

Upper: `;G` `;D` `;Th` `;L` `;X` `;P` `;S` `;Ph` `;Ps` `;O` → `\Gamma` `\Delta`
`\Theta` `\Lambda` `\Xi` `\Pi` `\Sigma` `\Phi` `\Psi` `\Omega`.

Output is `\name` with no tab stops. **Fix the `;p`/`;ph`/`;ps` and `;P`/`;Ph`/`;Ps`
collision (see 3.1).**

---

## 6. Regular (menu) snippets for Org

The completion menu offers these; `<C-y>` or `<CR>` expands. `[a|b|c]` = choice stop.

| Trigger | Expansion |
|---|---|
| `src` | `#+begin_src ⟨1:[python\|lua\|bash\|R\|cpp\|(empty)]⟩`⏎`⟨2⟩`⏎`#+end_src` |
| `nsrc` | `#+NAME: ⟨1:name⟩`⏎`#+begin_src ⟨2:[…same list…]⟩ :results output`⏎`⟨3⟩`⏎`#+end_src` |
| `pysrc` `rsrc` `javasrc` `cppsrc` | `#+begin_src python\|R\|java\|cpp`⏎`⟨1⟩`⏎`#+end_src` |
| `ex` / `quote` / `pcode` | `#+begin_example` / `_quote` / `_pseudocode` ⏎`⟨1⟩`⏎ matching `#+end_…` |
| `callout` | `#+begin_quote`⏎`*⟨1:[Note\|Tip\|Warning\|Definition\|Example]⟩:* ⟨2⟩`⏎`#+end_quote` |
| `eqn` | `\begin{equation}`⏎`  \label{eq:⟨1:name⟩}`⏎`  ⟨2:E = mc^2⟩`⏎`\end{equation}` |
| `eqref` | `\(\eqref{eq:⟨1:name⟩}\)` (wrapped in `\( \)` so MathJax processes it in prose) |
| `cbl` (prose only: `!in_math`) | `{{{hl(orange,COME BACK LATER)}}}` |
| `title` | `#+TITLE:`/`#+AUTHOR:`/`#+DATE:` header with ⟨1:Title⟩, ⟨2:author⟩, today's date (`YYYY-MM-DD`, computed), then boilerplate `#+MACRO`/`#+LATEX_HEADER` lines, then an explicit `⟨0⟩` at the end |
| `h1` `h2` `h3` | `* ⟨1⟩` `** ⟨1⟩` `*** ⟨1⟩` |
| `link` | `[[⟨1:target⟩][⟨2:description⟩]]` |
| `img` | `[[file:⟨1:path⟩][⟨2:caption⟩]]` |
| `oref` | `[[file:⟨1:../page/index.org⟩][⟨2:description⟩]]` |
| `sref` | `[[file:⟨1:../page/index.org⟩::#⟨2:anchor-id⟩][⟨3:description⟩]]` |
| `aref` | `[[#⟨1:anchor-id⟩][⟨2:description⟩]]` |
| `cid` | `:PROPERTIES:`⏎`:CUSTOM_ID: ⟨1:anchor-id⟩`⏎`:END:` |
| `tbl` | `\| ⟨1:h1⟩ \| ⟨2:h2⟩ \|`⏎`\|----+----\|`⏎`\| ⟨3⟩ \| ⟨4⟩ \|` |
| `cite` | `[cite:@⟨1:key⟩]` |
| `refs` | `* References`⏎⏎`#+print_bibliography:` |
| `date` | today as `YYYY-MM-DD` |
| `pdf` | `#+begin_export html`⏎`<embed src="⟨1:file.pdf⟩" width="100%" height="⟨2:800⟩px" type="application/pdf">`⏎`#+end_export` |

TikZ (regular snippets, available in Org too):

| Trigger | Expansion |
|---|---|
| `graph` | `\begin{tikzpicture}[`⏎`  >={Stealth[]},`⏎`  every node/.style={draw, circle, align=center, inner sep=4pt},`⏎`]`⏎`  \graph[⟨1:[spring layout\|layered layout\|tree layout]⟩, node distance=20mm]{`⏎`    ⟨2:a⟩ [as={⟨3:A⟩ \\ ⟨4:1⟩}] -- ⟨5:b⟩ [as={⟨6:B⟩ \\ ⟨7:2⟩}];`⏎`  };`⏎`\end{tikzpicture}` |
| `gex` | the same scaffold, fully filled in as a 3-node weighted directed example (no stops) |
| `gnode` | `⟨1:id⟩ [as={⟨2:name⟩ \\ ⟨3:value⟩}]` |
| `gedge` | `⟨1:a⟩ ⟨2:[--\|->\|<->]⟩ ⟨3:b⟩;` |
| `gwedge` | `⟨1:a⟩ ⟨2:[--\|->\|<->]⟩["⟨3:1⟩"] ⟨4:b⟩;` |

Snippet scoping: Org buffers see **Org snippets + all the tex/math snippets**
(the original uses `filetype_extend('org', {'tex'})`). `.tex` buffers get the math
and TikZ sets but not the Org ones.

---

## 7. Key model (this exact separation is what makes it fast)

| Key (insert/select mode) | Behavior |
|---|---|
| `<Tab>` | **Only** jumps to the next tab stop, and only while the caret is inside an active snippet's region. **Never** touches the completion menu. Otherwise inserts a literal Tab. |
| `<S-Tab>` | Jump to the previous tab stop if there is one; otherwise literal `<S-Tab>`. |
| `<C-n>` / `<C-p>` | Next / previous completion-menu item. If the menu is closed, `<C-n>` opens it. |
| `<C-y>` | Confirm the current menu item (selects the first if none is selected). |
| `<CR>` | Confirm **only if** a menu entry is selected; otherwise a normal newline. |
| `<C-e>` | Toggle the completion menu open/closed. |
| `<C-f>` / `<C-b>` | Menu open: scroll docs. Menu closed: open a path / buffer-words-only completion. |
| `<C-l>` | Accept an AI inline suggestion (moved off `<Tab>` so it can't steal snippet jumps). |

Completion menu config: opens automatically while typing; first item preselected
but **not inserted** (`menu,menuone,noinsert`); ghost-text preview of the
selected item; source labels like `[SNIP]`, `[TeX]`, `[BUF]`.

Org/tex/markdown completion sources, highest priority first:
1. **Snippets** (min. 1 char typed). Autosnippets are listed only when their condition holds.
2. **LaTeX command fuzzy finder**: starts when you type `\` + 2 chars, fuzzy-matches the
   full LaTeX symbol list (`\al` → `\alpha`, `\aleph`, …), and inserts the `\command` form.
   **Suppress it right after `\\`** (a row break in align/matrix/cases); otherwise the
   second backslash opens a menu of every symbol. A single `\` still completes.
3. LSP (min. 3 chars), buffer words, file paths.

## 8. No auto-pairing in note buffers

In org/tex/markdown buffers, **auto-pairing of `(`, `[`, `{` is disabled and `$` is
never auto-paired.** The snippets already produce balanced `{…}` with correct
tab stops, and auto-inserted closers fight with them (you'd get `\frac{}{}}` or have
to skip over stray closers). Code buffers keep normal auto-pairing.
(The original's cheatsheet says the opposite, but it's stale. Follow this spec.)

## 9. Supporting ergonomics (implement if your editor has the equivalents)

- **Visual-mode wrap** (Org, charwise): `<Space>m` wraps the selection in `$…$`,
  `<Space>b` in `*…*`, `<Space>i` in `/…/`.
- **Delimiter-balance linter** (org + tex): scans the whole buffer and reports
  unmatched or mismatched `$`, `$$`, `\(`/`\)`, `\[`/`\]`, and `\begin{env}`/`\end{env}` for MATH_ENVS
  as warnings at the exact delimiter. It uses a stack, not toggles. A blank line or org heading
  flushes the stack (reporting what's still open). In org it skips `#+begin_src/example/export`
  bodies, `#+…` directive lines, `# ` comments and `:` drawer/fixed-width lines. In tex it
  ignores `%` comments. It does **not** check parentheses (too noisy with `\left(`, `(0,1]`).
  It reruns on save, on leaving insert mode, and 200 ms after normal-mode edits.
  It's needed because the in-editor zone detector self-heals at paragraphs, so a stray
  `$` otherwise only shows up as a broken PDF.
- **Searchable cheat-sheet popup** (`<Space>?`): tabbed floating list of every trigger
  and symbol, with search and fuzzy search. `<CR>` on a snippet row **expands the
  real snippet** (with tab stops) at the original cursor. `<CR>` on a symbol row
  inserts the LaTeX and puts the caret in the most useful slot, using this
  heuristic (first match wins):
  1. just inside the first empty `{}` or `()`;
  2. just past the first `( ` or `[ ` (opener + space), e.g. inside `P\left(  \right)`;
  3. between the first double space (`\lVert  \rVert`, `\{  \}`);
  4. otherwise at the end.
- Org buffer options: conceal on (level 2, not concealed on the cursor line in insert), spell
  on, soft wrap with linebreak and breakindent, 2-space soft tabs, and **no smart
  list re-indentation** (Enter just copies the current line's indent, so flat lists stay flat).

## 10. Acceptance checks (write these as automated tests)

All typed in an Org buffer; `|` = caret; `[x]` = selected placeholder.

1. Prose `the sum is in a set` → unchanged (no expansion outside math).
2. `mk` → `$|$`. Then `//` → `$\frac{[num]}{den}$`. Then `a`⇥`b`⇥ → `$\frac{a}{b}|$`. Then ⇥ → `$\frac{a}{b}$|`.
3. In math: `x1+y2` → `x_{1}+y_{2}`.
4. In math: `;a->;b` → `\alpha\to\beta`.
5. In math: `int` → `\int_{[a]}^{b} f(x) \, dx` (one backslash). `inf` → `\infty` (one backslash, after the fix).
6. In math: `;ph` → `\phi`, `;ps` → `\psi` (after the fix).
7. In math: `nsum` → `\sum_{i=1}^{n}` and **not** `n\sum…`. `argmin` doesn't fire `min`.
8. `ck` from prose → three lines `\[`, `  |`, `\]`. Inside it `ali` → nested align.
9. `beg` then type `matrix` → the `\end{matrix}` line updates live.
10. A stray `$` in one paragraph doesn't make the next paragraph count as math.
11. In `\begin{cases} … \\` typing `\\` opens **no** symbol menu.
12. `<Tab>` with no active snippet inserts a Tab. `<Tab>` while the menu is open still jumps the snippet, not the menu.
13. `lim`⇥⇥⇥ (no typing) → `\lim_{n \to \infty} a_n|` (defaults kept, caret after).
14. Typing `(` or `{` in an Org buffer inserts only that character.
