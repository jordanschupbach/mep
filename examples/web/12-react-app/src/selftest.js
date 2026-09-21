// Drives the mounted application through the DOM, step by step, waiting for
// React's asynchronous commits between steps.
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
const $ = sel => document.querySelector(sel);

// React tracks an input's value through its own property descriptor, so a
// test has to set it the way the browser would before firing `input`.
function typeInto(el, text) {
  const proto = el.tagName === 'SELECT' ? HTMLSelectElement.prototype : HTMLInputElement.prototype;
  const setter = Object.getOwnPropertyDescriptor(proto, 'value').set;
  setter.call(el, text);
  el.dispatchEvent(new Event(el.tagName === 'SELECT' ? 'change' : 'input', { bubbles: true }));
}

export async function runSelfTest() {
  const H = window.Harness;
  const step = async (name, fn) => {
    try { H.check(name, await fn()); } catch (err) { H.check(name, false, err && err.message ? err.message : String(err)); }
  };
  await wait(100);
  await step('app mounted', () => !!$('#shell') && $('#count-value').textContent === 'Count: 0');
  await step('useReducer dispatch on click', async () => {
    $('#count-inc').click(); $('#count-inc').click();
    await wait(60);
    return $('#count-value').textContent === 'Count: 2';
  });
  await step('controlled input drives state', async () => {
    typeInto($('#count-step'), '5');
    await wait(60);
    $('#count-inc').click();
    await wait(60);
    return $('#count-value').textContent === 'Count: 7' && $('#count-inc').textContent === '+5';
  });
  await step('useEffect with deps', () => window.__counterEffects >= 3);
  await step('context update re-renders consumers', async () => {
    $('#theme-toggle').click();
    await wait(60);
    return $('#shell').className === 'theme-dark' && $('#theme-toggle').textContent === 'theme: dark';
  });
  await step('router: pushState switches the view', async () => {
    $('#tab-todos').click();
    await wait(60);
    return !!$('#todos') && !$('#counter') && window.location.search === '?tab=todos';
  });
  await step('form submit adds a keyed row', async () => {
    typeInto($('#todo-draft'), 'ship the browser');
    await wait(60);
    $('#todo-form').dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }));
    await wait(60);
    return document.querySelectorAll('#todo-list li').length === 2 && $('#todo-draft').value === '' && $('#todo-open').textContent === '1 open';
  });
  await step('checkbox toggles through onChange', async () => {
    document.querySelectorAll('#todo-list input[type=checkbox]')[1].click();
    await wait(60);
    return $('#todo-open').textContent === '0 open';
  });
  await step('row removal', async () => {
    document.querySelector('#todo-list .remove').click();
    await wait(60);
    return document.querySelectorAll('#todo-list li').length === 1;
  });
  await step('fetch inside useEffect renders a table', async () => {
    $('#tab-inventory').click();
    for (let i = 0; i < 40 && (!$('#inventory-status') || $('#inventory-status').textContent === 'loading'); i++) await wait(50);
    return $('#inventory-status').textContent === 'ready' && document.querySelectorAll('#inventory-rows tr').length === 5;
  });
  await step('select filters the list', async () => {
    typeInto($('#inventory-filter'), 'tree');
    await wait(60);
    return document.querySelectorAll('#inventory-rows tr').length === 2;
  });
  await step('history.back() returns to the previous view', async () => {
    window.history.back();
    await wait(150);
    return !!$('#todos') && window.location.search === '?tab=todos';
  });
  H.done();
}
