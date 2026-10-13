#pragma once

// The script a :Browser pane's web view runs in every page and frame
// (WebView::SetTheme): it draws the page in mep's colours, the way a PDF
// or :Browse pane is drawn with theme colours on (theme_recolor.h).
//
// It runs in a block that first declares `const MEP_THEME = {...};` -- the theme as JSON:
// recolor, dark, and "#rrggbb" strings bg, fg, dim, selection, border and
// pal (red, green, yellow, blue, purple, cyan, orange) -- and run in a
// script world of its own, so the page can neither see nor undo it. Run a
// second time in the same page (a theme change), it re-themes that page.
//
// The mapping, per colour the page uses:
//   - How much ink it is: its lightness (CIE L*), read against
//     the page's own polarity -- on a light page white is paper and black
//     ink, on a dark page the other way round. So a site's own dark mode
//     (prefers-color-scheme follows mep's theme) maps just as cleanly.
//   - Paper-to-ink becomes mep's bg-to-fg. (Not in linear light, as
//     theme_recolor.h does for glyph edges: that lifts the near-white
//     panels pages are built of to a visibly grey surface.)
//   - A coloured one (a link, a button, a highlight) is pulled towards
//     the palette hue nearest its own -- all the way for text, by how
//     saturated it is for a background.
// Every element's background, text, border, outline, decoration and SVG
// fill/stroke colour is pinned to its mapped value as an !important
// inline style, its own value kept to put back. Images, video and canvas
// are left alone, and so is text over a background image.
//
// The one subtlety is inheritance: a child whose colour is inherited would
// read the parent's already-mapped colour and map it again. Elements are
// done parents first, and a colour equal to the parent's current one is
// taken to be inherited and left to follow it.
//
// Elements are re-done when the page adds them or changes their class
// (a whole subtree, since a class can restyle its descendants), and the
// whole page once more when it has loaded (late style sheets).

inline constexpr const char *kWebThemeScript = R"JS(
(function () {
  'use strict';
  if (window.__mepTheme) { window.__mepTheme.set(MEP_THEME); return; }

  let theme = null;        // MEP_THEME, its colours parsed
  let pageLight = true;    // the page's own polarity
  let paperL = 1;          // ... and its paper's lightness
  let style = null;        // our <style>
  const saved = new WeakMap();  // element -> {prop: [value, priority]}
  let touched = [];             // WeakRefs of the elements in `saved`
  const surface = new WeakMap(); // element -> 'image' (text over a picture)

    const SKIP = new Set(['img', 'video', 'canvas', 'picture', 'iframe', 'embed', 'object', 'script', 'style',
                        'link', 'meta', 'head', 'title', 'noscript', 'template', 'br', 'image']);

  // --- colours -------------------------------------------------------------

  const toLin = (c) => { c /= 255; return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4); };
  const hex = (s) => [parseInt(s.slice(1, 3), 16), parseInt(s.slice(3, 5), 16), parseInt(s.slice(5, 7), 16), 1];
  const lum = (c) => 0.2126 * toLin(c[0]) + 0.7152 * toLin(c[1]) + 0.0722 * toLin(c[2]);
  // CIE L*, 0..1: lightness as seen.
  const light = (c) => { const y = lum(c); return (y > 0.008856 ? 116 * Math.cbrt(y) - 16 : 903.3 * y) / 100; };
  const css = (c) => c[3] >= 1 ? `rgb(${c[0]}, ${c[1]}, ${c[2]})` : `rgba(${c[0]}, ${c[1]}, ${c[2]}, ${c[3]})`;
  // t = 0 is a, 1 is b.
  const mix = (a, b, t) => [0, 1, 2].map((i) => Math.round(a[i] + (b[i] - a[i]) * t));
  const hue = (c) => {
    const r = c[0] / 255, g = c[1] / 255, b = c[2] / 255;
    const max = Math.max(r, g, b), d = max - Math.min(r, g, b);
    if (d === 0) return 0;
    const h = max === r ? ((g - b) / d) % 6 : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
    return (h * 60 + 360) % 360;
  };
  const chroma = (c) => (Math.max(c[0], c[1], c[2]) - Math.min(c[0], c[1], c[2])) / 255;

  // A computed colour: [r, g, b, a], or null (transparent, or a colour
  // space this does not know).
  function parse(s) {
    if (!s || s === 'none' || s === 'transparent' || s === 'currentcolor') return null;
    let m = /^rgba?\(\s*([\d.]+)[,\s]+([\d.]+)[,\s]+([\d.]+)(?:\s*[,/]\s*([\d.]+%?))?\s*\)$/.exec(s);
    let scale = 1;
    if (!m) {
      m = /^color\((?:srgb|display-p3)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)(?:\s*\/\s*([\d.]+%?))?\s*\)$/.exec(s);
      scale = 255;
    }
    if (!m) return null;
    let a = m[4] === undefined ? 1 : m[4].endsWith('%') ? parseFloat(m[4]) / 100 : parseFloat(m[4]);
    if (a <= 0) return null;
    return [Math.round(m[1] * scale), Math.round(m[2] * scale), Math.round(m[3] * scale), a];
  }

  // `c` as mep's: a background (`bg`) or ink.
  function map(c, bg) {
    // 0 at the page's paper (or beyond it), 1 at the far end from it.
    const l = light(c);
    let ink = pageLight ? (paperL - l) / Math.max(paperL, 0.01) : (l - paperL) / Math.max(1 - paperL, 0.01);
    // (Body text is rarely pure black: #222 and #333 are mep's own fg.)
    ink = Math.min(1, Math.max(0, ink / 0.85));
    let out = mix(theme.bg, theme.fg, ink);
    const k = chroma(c);
    if (k > 0.12) {
      const h = hue(c);
      let best = null, bestD = 999;
      for (const p of theme.pal) {
        const d = Math.min(Math.abs(p.h - h), 360 - Math.abs(p.h - h));
        if (d < bestD) { bestD = d; best = p.c; }
      }
      out = mix(out, best, Math.min(1, bg ? k : k * 2));
    }
    out.push(c[3]);
    return out;
  }

  // --- elements ----------------------------------------------------------------

  function pin(el, prop, value) {
    let s = saved.get(el);
    if (!s) {
      s = {};
      saved.set(el, s);
      touched.push(new WeakRef(el));
    }
    if (!(prop in s)) s[prop] = [el.style.getPropertyValue(prop), el.style.getPropertyPriority(prop)];
    el.style.setProperty(prop, value, 'important');
  }

  function unpin(el) {
    const s = saved.get(el);
    if (!s) return;
    saved.delete(el);
    for (const prop in s) {
      if (s[prop][0]) el.style.setProperty(prop, s[prop][0], s[prop][1]);
      else el.style.removeProperty(prop);
    }
  }

  // Over a picture: its own, or the nearest ancestor that has a background.
  function overImage(el, cs) {
    if (/url\(/.test(cs.backgroundImage)) { surface.set(el, 'image'); return true; }
    if (parse(cs.backgroundColor)) return false;
    for (let p = el.parentElement; p; p = p.parentElement) {
      if (surface.get(p) === 'image') return true;
      if (saved.has(p) && 'background-color' in saved.get(p)) return false;
    }
    return false;
  }

  function themeOne(el) {
    if (SKIP.has(el.localName)) return;
    const cs = getComputedStyle(el);
    const parent = el.parentElement ? getComputedStyle(el.parentElement) : null;
    const picture = overImage(el, cs);

    const bgi = cs.backgroundImage;
    let bg = parse(cs.backgroundColor);
    if (bgi && bgi !== 'none' && !/url\(/.test(bgi) && /gradient/.test(bgi)) {
      // A gradient: flattened to its first colour.
      const first = /(rgba?\([^)]*\)|color\([^)]*\))/.exec(bgi);
      if (first) {
        pin(el, 'background-image', 'none');
        if (!bg) bg = parse(first[1]);
      }
    }
    if (bg && el !== document.documentElement) pin(el, 'background-color', css(map(bg, true)));

    if (!picture) {
      const fg = cs.color;
      if (!parent || fg !== parent.color) {
        const c = parse(fg);
        if (c) pin(el, 'color', css(map(c, false)));
      }
    }
    for (const side of ['top', 'right', 'bottom', 'left']) {
      if (cs[`border-${side}-style`] === 'none' || parseFloat(cs[`border-${side}-width`]) === 0) continue;
      const v = cs.getPropertyValue(`border-${side}-color`);
      if (v === cs.color) continue;  // currentColor: follows `color`
      const c = parse(v);
      if (c) pin(el, `border-${side}-color`, css(map(c, false)));
    }
    if (cs.outlineStyle !== 'none' && cs.outlineColor !== cs.color) {
      const c = parse(cs.outlineColor);
      if (c) pin(el, 'outline-color', css(map(c, false)));
    }
    if (cs.textDecorationLine !== 'none' && cs.textDecorationColor !== cs.color) {
      const c = parse(cs.textDecorationColor);
      if (c) pin(el, 'text-decoration-color', css(map(c, false)));
    }
    if (el instanceof SVGElement) {
      for (const prop of ['fill', 'stroke']) {
        const v = cs.getPropertyValue(prop);
        if (v === cs.color || (parent && v === parent.getPropertyValue(prop))) continue;
        const c = parse(v);
        if (c) pin(el, prop, css(map(c, false)));
      }
    }
  }

  // `root` and everything under it (open shadow roots too), parents first.
  function* walk(root) {
    const stack = [root];
    while (stack.length) {
      const el = stack.pop();
      yield el;
      if (el.shadowRoot) watchShadow(el.shadowRoot);
      const kids = el.shadowRoot ? [...el.children, ...el.shadowRoot.children] : el.children;
      for (let i = kids.length - 1; i >= 0; i--) stack.push(kids[i]);
    }
  }

  // Big pages a slice at a time, so the page stays responsive.
  let queue = [];
  let scheduled = false;
  function enqueue(root, redo) {
    queue.push([root, redo]);
    if (!scheduled) { scheduled = true; setTimeout(drain, 0); }
  }
  function drain() {
    scheduled = false;
    if (!theme || !theme.recolor) { queue = []; return; }
    const start = performance.now();
    while (queue.length) {
      const [root, redo] = queue.shift();
      if (!root.isConnected) continue;
      const els = [...walk(root)];
      if (redo) for (const el of els) unpin(el);
      for (const el of els) themeOne(el);
      if (performance.now() - start > 30 && queue.length) { scheduled = true; setTimeout(drain, 0); return; }
    }
  }

  // --- the page --------------------------------------------------------------

  function polarity() {
    // Read with our pins off and our sheet out of the way.
    const root = document.documentElement, body = document.body;
    if (style) style.disabled = true;
    const was = [root, body].filter(Boolean).map((el) => [el, saved.get(el)]);
    for (const [el] of was) unpin(el);
    let paper = null;
    for (const [el] of was) paper = paper || parse(getComputedStyle(el).backgroundColor);
    const ink = body ? parse(getComputedStyle(body).color) : null;
    if (style) style.disabled = false;
    if (paper && paper[3] > 0.5) {
      paperL = light(paper);
      return paperL > 0.5;
    }
    // No paper of its own: light text means the page means to be dark.
    const isLight = ink ? light(ink) < 0.6 : true;
    paperL = isLight ? 1 : 0;
    return isLight;
  }

  function sheet() {
    const t = theme;
    return `
      :root { color-scheme: ${t.dark ? 'dark' : 'light'} !important; background-color: ${t.bgCss} !important; }
      ::selection { background-color: ${t.selection} !important; color: ${t.fgCss} !important; }
      ::placeholder { color: ${t.dim} !important; opacity: 1 !important; }
      input, textarea, [contenteditable] { caret-color: ${t.fgCss} !important; }
      img, video, picture, canvas, iframe { color-scheme: normal; }
      ::-webkit-scrollbar-thumb { background-color: ${t.border}; }
    `;
  }

  function putSheet() {
    if (!style) {
      style = document.createElement('style');
      style.id = 'mep-theme';
    }
    style.textContent = theme.recolor ? sheet() : '';
    const host = document.head || document.documentElement;
    if (host && style.parentNode !== host) host.appendChild(style);
  }

  function unpinAll() {
    for (const ref of touched) {
      const el = ref.deref();
      if (el) unpin(el);
    }
    touched = [];
    queue = [];
  }

  function rethemeAll() {
    if (!document.documentElement) return;
    putSheet();
    unpinAll();
    if (!theme.recolor) return;
    pageLight = polarity();
    enqueue(document.documentElement, false);
    drain();
  }

  const observer = new MutationObserver((records) => {
    if (!theme || !theme.recolor) return;
    for (const r of records) {
      if (r.type === 'childList') {
        for (const n of r.addedNodes) if (n.nodeType === 1 && n !== style) enqueue(n, false);
        // Our sheet, thrown out by a page that rewrote its <head>.
        if (style && !style.isConnected) putSheet();
      } else if (r.target === document.documentElement || r.target === document.body) {
        // <html class="dark">: the page may have changed its polarity.
        rethemeSoon();
      } else {
        enqueue(r.target, true);
      }
    }
  });
  function watch(root) { observer.observe(root, {childList: true, subtree: true, attributes: true, attributeFilter: ['class']}); }
  const shadows = new WeakSet();
  function watchShadow(sr) {
    if (shadows.has(sr)) return;
    shadows.add(sr);
    watch(sr);
    if (theme && theme.recolor && !sr.querySelector('style.mep-theme')) {
      const s = document.createElement('style');
      s.className = 'mep-theme';
      s.textContent = `::selection { background-color: ${theme.selection} !important; }`;
      sr.appendChild(s);
    }
  }

  let soon = 0;
  function rethemeSoon() {
    clearTimeout(soon);
    soon = setTimeout(rethemeAll, 50);
  }

  function set(t) {
    const pal = ['red', 'green', 'yellow', 'blue', 'purple', 'cyan', 'orange']
      .map((k) => hex(t.pal[k])).map((c) => ({c, h: hue(c)}));
    theme = {recolor: t.recolor, dark: t.dark, bg: hex(t.bg), fg: hex(t.fg), bgCss: t.bg, fgCss: t.fg,
             dim: t.dim, selection: t.selection, border: t.border, pal};
    rethemeAll();
  }

  window.__mepTheme = {set};
  theme = null;
  if (document.documentElement) {
    set(MEP_THEME);
    watch(document);
  } else {
    // At document start there may be no <html> yet.
    const early = new MutationObserver(() => {
      if (!document.documentElement) return;
      early.disconnect();
      set(MEP_THEME);
      watch(document);
    });
    early.observe(document, {childList: true});
  }
  document.addEventListener('DOMContentLoaded', rethemeAll);
  window.addEventListener('load', rethemeSoon);
  matchMedia('(prefers-color-scheme: dark)').addEventListener('change', rethemeSoon);
})();
)JS";
