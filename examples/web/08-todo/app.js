// A small todo application: one state object, one render(), event
// delegation on the list, persistence in localStorage.
(function () {
  'use strict';
  const STORAGE_KEY = 'ladder-todos';
  let state = { todos: [], filter: 'all', nextId: 1 };

  function load() {
    try {
      const raw = localStorage.getItem(STORAGE_KEY);
      if (raw) state = Object.assign(state, JSON.parse(raw));
    } catch (e) { /* first run or storage unavailable */ }
  }
  function save() {
    try { localStorage.setItem(STORAGE_KEY, JSON.stringify(state)); } catch (e) { /* ignore */ }
  }
  function visible() {
    return state.todos.filter(t => state.filter === 'all' || (state.filter === 'done') === t.done);
  }
  function render() {
    const ul = document.getElementById('todos');
    ul.innerHTML = '';
    for (const todo of visible()) {
      const li = document.createElement('li');
      li.dataset.id = String(todo.id);
      if (todo.done) li.className = 'done';
      const box = document.createElement('input');
      box.type = 'checkbox';
      box.checked = todo.done;
      box.className = 'toggle';
      const label = document.createElement('span');
      label.textContent = ' ' + todo.text + ' ';
      const del = document.createElement('button');
      del.textContent = 'x';
      del.className = 'delete';
      li.append(box, label, del);
      ul.appendChild(li);
    }
    const open = state.todos.filter(t => !t.done).length;
    document.getElementById('count').textContent = `${open} open of ${state.todos.length} `;
    save();
  }
  function add(text) {
    text = text.trim();
    if (!text) return false;
    state.todos.push({ id: state.nextId++, text, done: false });
    render();
    return true;
  }
  function toggle(id) {
    const todo = state.todos.find(t => t.id === id);
    if (todo) todo.done = !todo.done;
    render();
  }
  function remove(id) {
    state.todos = state.todos.filter(t => t.id !== id);
    render();
  }

  document.getElementById('add-form').addEventListener('submit', e => {
    e.preventDefault();
    const input = document.getElementById('new-todo');
    if (add(input.value)) input.value = '';
  });
  // Event delegation: one listener on the list handles every row.
  document.getElementById('todos').addEventListener('click', e => {
    const li = e.target.closest('li');
    if (!li) return;
    const id = Number(li.dataset.id);
    if (e.target.classList.contains('delete')) remove(id);
    else if (e.target.classList.contains('toggle')) toggle(id);
  });
  for (const f of ['all', 'open', 'done']) {
    document.getElementById('show-' + f).addEventListener('click', () => { state.filter = f; render(); });
  }
  document.getElementById('clear-done').addEventListener('click', () => {
    state.todos = state.todos.filter(t => !t.done);
    render();
  });

  window.TodoApp = { add, toggle, remove, reset() { state = { todos: [], filter: 'all', nextId: 1 }; render(); }, get state() { return state; }, load, render, STORAGE_KEY };
  load();
  render();
})();
