import { Counter } from './components/Counter.jsx';
import { Todos } from './components/Todos.jsx';
import { Inventory } from './components/Inventory.jsx';
import { ThemeProvider, useTheme } from './theme.jsx';
import { useRoute } from './router.jsx';

const TABS = { counter: Counter, todos: Todos, inventory: Inventory };

function Shell() {
  const { theme, toggle } = useTheme();
  const [tab, go] = useRoute('counter');
  const View = TABS[tab] || Counter;
  return (
    <div id="shell" className={'theme-' + theme}>
      <div className="tabs row">
        {Object.keys(TABS).map(name => (
          <button key={name} id={'tab-' + name} className={name === tab ? 'active' : ''} onClick={() => go(name)}>{name}</button>
        ))}
        <button id="theme-toggle" onClick={toggle}>theme: {theme}</button>
      </div>
      <View />
    </div>
  );
}

export function App() {
  return <ThemeProvider><Shell /></ThemeProvider>;
}
