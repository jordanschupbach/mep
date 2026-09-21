import greet, { VERSION, add } from './lib/math.js';
import * as shapes from './lib/shapes.js';
import { counter, increment } from './lib/counter.js';

window.__moduleRan = true;
Harness.check('module script executed', true);
Harness.check('named + default imports', add(2, 3) === 5 && greet('mep') === 'hello, mep' && VERSION === '1.0');
Harness.check('namespace import + re-export', shapes.area({ kind: 'square', side: 3 }) === 9 && typeof shapes.PI === 'number');
increment(); increment();
Harness.check('live binding across modules', counter === 2, 'counter=' + counter);
Harness.check('modules are strict + have their own scope', (function () { return this === undefined; })() && typeof window.add === 'undefined');
Harness.check('import.meta.url', typeof import.meta.url === 'string' && import.meta.url.indexOf('main.js') >= 0);
import('./lib/lazy.js')
  .then(mod => {
    document.getElementById('out').textContent = mod.message;
    Harness.check('dynamic import()', mod.message === 'loaded lazily' && mod.default() === 42);
  })
  .catch(e => Harness.check('dynamic import()', false, String(e)))
  .then(() => Harness.done());
