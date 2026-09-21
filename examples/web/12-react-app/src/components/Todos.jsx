import { useMemo, useState } from 'react';

export function Todos() {
  const [todos, setTodos] = useState([{ id: 1, text: 'learn React', done: true }]);
  const [draft, setDraft] = useState('');
  const open = useMemo(() => todos.filter(t => !t.done).length, [todos]);
  const add = e => {
    e.preventDefault();
    const text = draft.trim();
    if (!text) return;
    setTodos(list => [...list, { id: Date.now() + list.length, text, done: false }]);
    setDraft('');
  };
  return (
    <div className="card" id="todos">
      <h2>Todos (controlled form)</h2>
      <form id="todo-form" onSubmit={add}>
        <input id="todo-draft" type="text" value={draft} onChange={e => setDraft(e.target.value)} placeholder="add a todo" />
        <button type="submit">Add</button>
      </form>
      <ul id="todo-list">
        {todos.map(t => (
          <li key={t.id} className={t.done ? 'done' : ''}>
            <input type="checkbox" checked={t.done}
              onChange={() => setTodos(list => list.map(x => (x.id === t.id ? { ...x, done: !x.done } : x)))} />
            {' '}{t.text}{' '}
            <button className="remove" onClick={() => setTodos(list => list.filter(x => x.id !== t.id))}>x</button>
          </li>
        ))}
      </ul>
      <p id="todo-open">{open} open</p>
    </div>
  );
}
