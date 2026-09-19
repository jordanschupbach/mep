import { useEffect, useState } from 'react';

export function Inventory() {
  const [status, setStatus] = useState('loading');
  const [items, setItems] = useState([]);
  const [filter, setFilter] = useState('all');
  useEffect(() => {
    let cancelled = false;
    (async () => {
      try {
        const res = await fetch('public/inventory.json');
        if (!res.ok) throw new Error('HTTP ' + res.status);
        const data = await res.json();
        if (!cancelled) { setItems(data.items); setStatus('ready'); }
      } catch (err) {
        if (!cancelled) setStatus('error: ' + err.message);
      }
    })();
    return () => { cancelled = true; };
  }, []);
  const shown = items.filter(i => filter === 'all' || i.category === filter);
  return (
    <div className="card" id="inventory">
      <h2>Inventory (fetch in an effect)</h2>
      <p id="inventory-status" className={status.startsWith('error') ? 'error' : ''}>{status}</p>
      <select id="inventory-filter" value={filter} onChange={e => setFilter(e.target.value)}>
        <option value="all">all</option><option value="linear">linear</option><option value="tree">tree</option><option value="hash">hash</option>
      </select>
      <table className="inventory">
        <thead><tr><th>Name</th><th>Category</th><th>Year</th></tr></thead>
        <tbody id="inventory-rows">
          {shown.map(i => <tr key={i.id}><td>{i.name}</td><td>{i.category}</td><td>{i.year}</td></tr>)}
        </tbody>
      </table>
    </div>
  );
}
