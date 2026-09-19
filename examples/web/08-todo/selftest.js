// Drives the todo app through the DOM the way a user would.
(function () {
  Harness.start('08-todo', 8);
  Harness.check('app.js loaded', typeof window.TodoApp === 'object');
  if (!window.TodoApp) { Harness.done(); return; }
  TodoApp.reset();
  var input = document.getElementById('new-todo');
  var form = document.getElementById('add-form');
  function submit(text) {
    input.value = text;
    form.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }));
  }
  Harness.attempt('form submit adds rows', function () {
    submit('write tests'); submit('ship it');
    var cleared = input.value === '';
    submit('   ');  // blank: rejected, no new row
    return document.querySelectorAll('#todos li').length === 2 && cleared;
  });
  Harness.attempt('delegated click toggles a row', function () {
    document.querySelector('#todos li .toggle').click();
    return document.querySelector('#todos li').className === 'done' && document.getElementById('count').textContent.indexOf('1 open of 2') === 0;
  });
  Harness.attempt('filter buttons', function () {
    document.getElementById('show-open').click();
    var open = document.querySelectorAll('#todos li').length;
    document.getElementById('show-done').click();
    var done = document.querySelectorAll('#todos li').length;
    document.getElementById('show-all').click();
    return open === 1 && done === 1 && document.querySelectorAll('#todos li').length === 2;
  });
  Harness.attempt('element.closest + dataset drive delete', function () {
    var rows = document.querySelectorAll('#todos li');
    rows[1].querySelector('.delete').click();
    return document.querySelectorAll('#todos li').length === 1;
  });
  Harness.attempt('state persisted to localStorage', function () {
    var saved = JSON.parse(localStorage.getItem(TodoApp.STORAGE_KEY));
    return saved.todos.length === 1 && saved.todos[0].text === 'write tests' && saved.todos[0].done === true;
  });
  Harness.attempt('state reloads from localStorage', function () {
    TodoApp.state.todos = [];
    TodoApp.load();
    TodoApp.render();
    return document.querySelectorAll('#todos li').length === 1;
  });
  Harness.attempt('clear done', function () {
    document.getElementById('clear-done').click();
    return document.querySelectorAll('#todos li').length === 0;
  });
  Harness.done();
})();
