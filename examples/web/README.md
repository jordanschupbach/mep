# Browser capability ladder

Twelve small JavaScript sites, simplest first, used to measure how much of
a browser mep's in-house engine (`src/html_doc.*`, `src/js_engine.*`, the
html pane) really is. Every level checks itself and reports its verdict
three ways — a results box in the page, `document.title`
(`PASS 7/7 - 04-timers`, `FAIL 3/9 - ...`, or `RUNNING ...` when the page
needed an event loop that never ran), and `window.__harness` — so any host
can read the score.

**The rule:** every level passes in a real browser (see *Reference run*).
A level that fails inside mep therefore names a missing browser feature.

| # | Level | What it needs from the browser |
|---|---|---|
| 01 | Static page | HTML parsing, an external stylesheet, inline styles, table, lists, image |
| 02 | DOM scripting | create/insert/remove nodes, `classList`, attributes, `dataset`, `style`, `innerHTML`, `querySelector(All)`, tree navigation |
| 03 | Events | `addEventListener`/`onclick`, `click()`, `dispatchEvent`, bubbling, `stopPropagation`, `preventDefault`, `CustomEvent` |
| 04 | Timers | `setTimeout`/`setInterval`/`requestAnimationFrame` running *after* load, microtask ordering, `Date` |
| 05 | External resources | `<link>`, several `<script src>` in document order, `getComputedStyle`, `<img>` load + `naturalWidth` |
| 06 | Modern JavaScript | 28 ES2015+ features, one per `<script>` element so a syntax error costs one check, plus `await`/`Promise.all` |
| 07 | fetch and XMLHttpRequest | `fetch` JSON/text, status + headers, a 404, `XMLHttpRequest` — needs `http://` |
| 08 | Todo app | a real vanilla app: state, render, form submit, event delegation (`closest`), `localStorage` |
| 09 | Client-side router | `history.pushState`/`replaceState`/`back`, `popstate`, `location`, `URL`, `URLSearchParams` |
| 10 | ES modules | `<script type="module">`, named/default/namespace imports, re-exports, live bindings, `import.meta`, `import()` |
| 11 | React (UMD) | the unmodified react + react-dom 18 production builds: hooks, a keyed list, click → re-render |
| 12 | React application | an esbuild-bundled JSX app: context, `useReducer`, effects, refs, a controlled form, routing, a `fetch` in an effect |

`shared/harness.js` is deliberately primitive ES5 — no regex literals, no
`String()`, no reliance on `var` hoisting, `typeof undeclared` or
`window.x` becoming a global — so that the harness itself runs on a very
young engine and a level's verdict is about that level.

## Running it in mep

```
:WebLadder          serve this directory on a free localhost port and open it in the browser pane
:WebLadderRun       load every level in turn and show each page's verdict
:Serve [dir] [port] serve any directory; :ServeStop [port]; :Servers
```

In the browser pane the omnibar at the top shows the URL: `o`, `Ctrl-L` or
a click edits it (`localhost:8000`, `:8000/app`, `example.com` and paths
all work), `Enter` goes, `Esc` cancels; `<`, `>` and `R` are back, forward
and reload (`H`/`L`/`r` on the keyboard).

Headless, through the same engine (no window needed):

```
cmake --build build/native --target mep-web-ladder-test
./build/native/mep-web-ladder-test              # unit checks + the ladder report
./build/native/mep-web-ladder-test --strict     # exit non-zero unless every level passes
./build/native/mep-web-ladder-test --eval x.js  # run one script, print what the engine says
./build/native/mep-web-ladder-test --level 12   # one level, with its console output and every error
```

`--eval` is the quick way to find which construct of a failing page the
engine rejects: cut the script down, re-run. Both modes run the page's
event loop (timers, promises, animation frames) until it goes idle, the
way the pane does frame by frame.

mep currently passes all twelve, so `--strict` is a regression gate.

## Reference run (a real browser)

```
cd tools && npm install
nix shell nixpkgs#chromium --command node baseline.mjs     # or CHROME=/path/to/chrome node baseline.mjs
```

This must print `ok` for all twelve levels. Run it after changing any
level: a level that fails here is a bug in the level, not in mep.

## Level 12's bundle

`12-react-app/dist/bundle.js` is committed so the ladder works offline. To
rebuild it after editing `12-react-app/src/`:

```
cd 12-react-app && npm install && npm run build
```

`11-react-umd/vendor/` holds the unmodified React 18.3.1 UMD production
builds (MIT, see `LICENSE-react.txt` there).
