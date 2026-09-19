#ifndef MEP_JS_ENGINE_H
#define MEP_JS_ENGINE_H

#include <functional>
#include <memory>
#include <string>

struct HtmlDoc;


// A tiny, intentionally non-spec-compliant tree-walking JS interpreter --
// see js_engine.cpp's own header comment for the full scope/exclusion
// list. Not a JIT, not V8/JSC-class; built to run small hand-written
// scripts against a page's DOM (a getElementById + textContent mutation is
// the typical use case), not real-world JS-heavy sites or frameworks.

// Executes every script in doc.scripts (in document order, one shared
// global scope across all of them -- a later block sees an earlier one's
// globals, matching real multi-<script>-tag pages) against doc's own DOM
// tree. document.getElementById/.title, a DOM element's .textContent, and
// a bare inert `window` object (property assignment only -- no BOM
// methods on it) are the entire binding surface (js_engine.cpp's header
// has the full list). on_console_log fires once per console.log(...) call with
// its arguments already stringified and space-joined; on_error fires once
// per script that fails to parse or throws while running, with a message
// that includes enough of the script's own text to locate the problem --
// a failing script doesn't stop the rest of doc.scripts from running, and
// never leaves doc's tree in a half-mutated state beyond whatever it
// legitimately changed before the failure. Safe to call on a doc with an
// empty scripts list (a no-op). Callers only need to re-run
// ComputeStyles(doc) afterward if they care about a script having added/
// removed elements -- this engine can't do that (see js_engine.cpp), so a
// textContent-only mutation never needs it.
/**
 * @brief Runs every script in doc.scripts in document order against doc's DOM, sharing one global scope across all of them.
 * @param doc The document whose scripts are executed and whose DOM tree they may read/mutate.
 * @param on_console_log Invoked once per console.log(...) call with its arguments already stringified and space-joined.
 * @param on_error Invoked once per script that fails to parse or throws while running, with a message locating the problem.
 */
void RunScripts(HtmlDoc &doc, const std::function<void(const std::string &)> &on_console_log,
                 const std::function<void(const std::string &)> &on_error);

// ---- a page whose scripts keep running -------------------------------------
// RunScripts above is one-shot: when it returns, pending timers, promise
// reactions and event listeners are gone. A browser pane instead keeps the
// runtime and pumps it every frame, so setTimeout/setInterval/
// requestAnimationFrame callbacks, async functions and event handlers all
// run for as long as the page is open.
struct JsRuntime;
struct DomNode;

/**
 * @brief Runs the document's scripts like RunScripts, fires DOMContentLoaded and load, and returns the live runtime.
 * @param doc The document; must outlive the returned runtime and must not be moved or re-parsed while it is alive.
 * @param on_console_log Receives each console.log line (held for the runtime's lifetime).
 * @param on_error Receives parse errors and uncaught exceptions from scripts, timers, events and promise jobs.
 * @return The runtime to pump; destroy it before replacing `doc`'s tree.
 */
std::shared_ptr<JsRuntime> StartScripts(HtmlDoc &doc, std::function<void(const std::string &)> on_console_log,
                                        std::function<void(const std::string &)> on_error);

/** @brief Runs every due timer/animation-frame callback and promise job; true when script ran (styles are recomputed, relayout needed). */
bool PumpScripts(JsRuntime &runtime);

/** @brief Milliseconds until the runtime next has work (0 = now), or a negative value when nothing is scheduled. */
double ScriptsNextWakeMs(const JsRuntime &runtime);

/** @brief A user click on `node`: the click event with capture/bubble, then the default action (checkbox toggle, form submit). False when a listener cancelled it (so the host must not follow the link either). */
bool ScriptsClick(JsRuntime &runtime, DomNode *node);

/** @brief Dispatches a synthetic event (`input`, `change`, `submit`, ...) at `node`; false when a listener called preventDefault(). */
bool ScriptsDispatchEvent(JsRuntime &runtime, DomNode *node, const std::string &type, bool bubbles);

/** @brief Whether the page registered any event listener at all (lets a host skip dispatch work for static pages). */
bool ScriptsHaveListeners(JsRuntime &runtime);

#endif
