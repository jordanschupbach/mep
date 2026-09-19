import { createRoot } from 'react-dom/client';
import { App } from './App.jsx';
import { runSelfTest } from './selftest.js';

const root = createRoot(document.getElementById('root'));
root.render(<App />);
runSelfTest();
