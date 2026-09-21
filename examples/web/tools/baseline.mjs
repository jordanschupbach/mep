// Reference run: every ladder level in a real browser. A level that fails
// here is a bug in the level; one that passes here and fails inside mep is
// a missing browser feature.
//
//   cd examples/web/tools && npm install
//   CHROME=$(which chromium) node baseline.mjs          # or:
//   nix shell nixpkgs#chromium --command node baseline.mjs
import { createServer } from 'node:http';
import { readFile, readdir } from 'node:fs/promises';
import { execSync } from 'node:child_process';
import { extname, join, normalize, resolve } from 'node:path';
import puppeteer from 'puppeteer-core';

const ROOT = resolve(import.meta.dirname, '..');
const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8', '.json': 'application/json', '.png': 'image/png', '.txt': 'text/plain; charset=utf-8' };

const server = createServer(async (req, res) => {
  const path = normalize(decodeURIComponent(new URL(req.url, 'http://x').pathname));
  const file = join(ROOT, path.endsWith('/') ? path + 'index.html' : path);
  if (!file.startsWith(ROOT)) { res.writeHead(403).end(); return; }
  try {
    const body = await readFile(file);
    res.writeHead(200, { 'Content-Type': TYPES[extname(file)] || 'application/octet-stream' }).end(body);
  } catch { res.writeHead(404, { 'Content-Type': 'text/plain' }).end('not found'); }
});
await new Promise(r => server.listen(0, '127.0.0.1', r));
const port = server.address().port;

const chrome = process.env.CHROME || execSync('which chromium || which google-chrome || which chrome', { encoding: 'utf8' }).trim();
const browser = await puppeteer.launch({ executablePath: chrome, headless: true, args: ['--no-sandbox', '--disable-gpu'] });
const levels = (await readdir(ROOT)).filter(d => /^\d\d-/.test(d)).sort();
let failed = 0;
for (const level of levels) {
  const page = await browser.newPage();
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  await page.goto(`http://127.0.0.1:${port}/${level}/index.html`, { waitUntil: 'load' });
  // 01-static has no script; every other level finishes by calling Harness.done().
  const hasHarness = await page.evaluate(() => typeof window.__harness === 'object');
  if (hasHarness) await page.waitForFunction(() => window.__harness.done, { timeout: 15000 }).catch(() => {});
  const title = await page.title();
  const fails = hasHarness ? await page.evaluate(() => window.__harness.checks.filter(c => !c.ok).map(c => c.name + (c.detail ? ' (' + c.detail + ')' : ''))) : [];
  const ok = hasHarness ? title.startsWith('PASS') : true;
  if (!ok) failed++;
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${level.padEnd(14)} ${title}`);
  for (const f of fails) console.log('       - ' + f);
  for (const e of errors) console.log('       ! ' + e);
  await page.close();
}
await browser.close();
server.close();
process.exit(failed ? 1 : 0);
