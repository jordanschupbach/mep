import { useEffect, useReducer, useRef } from 'react';

function reducer(state, action) {
  switch (action.type) {
    case 'inc': return { ...state, count: state.count + state.step };
    case 'dec': return { ...state, count: state.count - state.step };
    case 'step': return { ...state, step: action.value };
    case 'reset': return { count: 0, step: 1 };
    default: return state;
  }
}

export function Counter() {
  const [state, dispatch] = useReducer(reducer, { count: 0, step: 1 });
  const renders = useRef(0);
  renders.current += 1;
  useEffect(() => { window.__counterEffects = (window.__counterEffects || 0) + 1; }, [state.count]);
  return (
    <div className="card" id="counter">
      <h2>Counter (useReducer)</h2>
      <p id="count-value">Count: {state.count}</p>
      <button id="count-inc" onClick={() => dispatch({ type: 'inc' })}>+{state.step}</button>
      <button id="count-dec" onClick={() => dispatch({ type: 'dec' })}>-{state.step}</button>
      <button id="count-reset" onClick={() => dispatch({ type: 'reset' })}>reset</button>
      <label> step <input id="count-step" type="text" value={state.step}
        onChange={e => dispatch({ type: 'step', value: Number(e.target.value) || 1 })} /></label>
      <p className="row">renders: <span id="count-renders">{renders.current}</span></p>
    </div>
  );
}
