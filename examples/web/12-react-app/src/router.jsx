import { useEffect, useState } from 'react';

// A minimal pushState router: the current view lives in ?tab=.
export function useRoute(defaultTab) {
  const read = () => new URLSearchParams(window.location.search).get('tab') || defaultTab;
  const [tab, setTab] = useState(read);
  useEffect(() => {
    const onPop = () => setTab(read());
    window.addEventListener('popstate', onPop);
    return () => window.removeEventListener('popstate', onPop);
  }, []);
  const go = next => {
    window.history.pushState({ tab: next }, '', '?tab=' + next);
    setTab(next);
  };
  return [tab, go];
}
