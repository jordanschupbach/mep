// Tiny self-check harness for the browser-capability ladder. Deliberately
// plain ES5 (var, function, no arrows/classes/template literals -- and no
// regex literals, no reliance on var hoisting or `typeof undeclared`) so the
// harness itself runs on a very young JS engine and a level's failure is
// about what that level tests, not about the harness.
//
//   Harness.start('03-events', 4);            // name, expected check count
//   Harness.check('click fires', ok, detail); // one check (ok = boolean)
//   Harness.done();                           // optional: finish early
//
// Results are mirrored three ways so any host can read them:
//   - the #results box in the page (created if missing)
//   - document.title: "PASS 4/4 - 03-events" / "FAIL 3/4 - ..." / "RUNNING ..."
//   - window.__harness = {name, expected, passed, failed, checks: [...], done}
// An uncaught script error is recorded as a failed check named "script error".
// A top-level `var` (not only window.Harness): a page's bare `Harness`
// reference must work even where window properties are not yet globals.
var Harness = (function () {
  var state = { name: '', expected: 0, passed: 0, failed: 0, checks: [], done: false };
  var box = null;

  function ensureBox() {
    if (box) return box;
    box = document.getElementById('results');
    if (!box) {
      box = document.createElement('div');
      box.id = 'results';
      document.body.appendChild(box);
    }
    return box;
  }

  function render() {
    var b = ensureBox();
    var total = state.passed + state.failed;
    var status = 'RUNNING';
    var cls = 'pending';
    if (state.failed > 0) { status = 'FAIL'; cls = 'fail'; }
    else if (state.done || (state.expected > 0 && total >= state.expected)) { status = 'PASS'; cls = 'pass'; }
    var html = '<div class="summary ' + cls + '">' + status + ' ' + state.passed + '/' +
      (state.expected || total) + ' - ' + state.name + '</div><ul>';
    for (var i = 0; i < state.checks.length; i++) {
      var c = state.checks[i];
      html += '<li class="' + (c.ok ? 'pass' : 'fail') + '">' + (c.ok ? 'ok' : 'FAILED') + ' - ' +
        escapeHtml(c.name) + (c.detail ? ' (' + escapeHtml('' + c.detail) + ')' : '') + '</li>';
    }
    html += '</ul>';
    b.innerHTML = html;
    document.title = status + ' ' + state.passed + '/' + (state.expected || total) + ' - ' + state.name;
  }

  // split/join rather than a regex literal: the harness must not depend on
  // anything a level might be the first to test.
  function escapeHtml(s) {
    return s.split('&').join('&amp;').split('<').join('&lt;').split('>').join('&gt;');
  }

  var Harness = {
    start: function (name, expected) {
      state.name = name;
      state.expected = expected || 0;
      render();
    },
    check: function (name, ok, detail) {
      var pass = !!ok;
      state.checks.push({ name: name, ok: pass, detail: detail });
      if (pass) state.passed++; else state.failed++;
      render();
      return pass;
    },
    // Runs fn inside try/catch; a throw is a failed check carrying the message.
    attempt: function (name, fn) {
      try {
        var r = fn();
        return Harness.check(name, r !== false, r === false ? 'returned false' : '');
      } catch (e) {
        return Harness.check(name, false, e && e.message ? e.message : ('' + e));
      }
    },
    done: function () {
      state.done = true;
      if (state.expected > 0 && state.passed + state.failed < state.expected) {
        Harness.check('all checks ran', false, (state.passed + state.failed) + ' of ' + state.expected);
      }
      render();
    },
    state: state
  };

  window.__harness = state;
  window.onerror = function (msg) {
    Harness.check('script error', false, msg);
  };
  return Harness;
})();
window.Harness = Harness;
