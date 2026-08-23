#ifndef MEP_JS_ENGINE_H
#define MEP_JS_ENGINE_H

#include <functional>
#include <string>

#include "html_doc.h"

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
void RunScripts(HtmlDoc &doc, const std::function<void(const std::string &)> &on_console_log,
                 const std::function<void(const std::string &)> &on_error);

#endif
