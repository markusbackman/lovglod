#!/usr/bin/env node
// Renderar produktbilderna till webbplatsen ur STL-filerna.
//
//   node render.mjs                         # alla vyer, hardware/v2 → site/assets
//   node render.mjs --views hero,front      # bara några vyer
//   node render.mjs --stl ../../hardware/v2 --out ../../site/assets
//   node render.mjs --format png --scale 2  # png i dubbel upplösning
//
// Scenen ligger i scene.html; kamera, ljus och material bor där.

import { createServer } from 'node:http';
import { readFile, mkdir, writeFile } from 'node:fs/promises';
import { existsSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));

// Vyerna, och hur breda bilderna blir. Måtten är de som sidan använder;
// bilden beskärs mot innehållet innan den skalas, så höjden följer motivet.
// w/h är scenens duk, out är bildens bredd efter beskärning. Duken är tilltagen
// så att motivet aldrig behöver skalas upp: modellen fyller ungefär halva duken.
const VIEWS = {
  hero:     { w: 2600, h: 2600, out: 1200, note: 'tänd, snett framifrån — startsidans hjältebild' },
  front:    { w: 2200, h: 2600, out: 1000, fov: 20, note: 'rakt framifrån' },
  exploded: { w: 3000, h: 3000, out: 1400, dist: 1250, note: 'sprängskiss, alla åtta delar' },
  back:     { w: 2000, h: 2000, out: 900,  glow: 0, note: 'bakifrån: bakstycke och USB-C' },
  side:     { w: 2000, h: 2400, out: 900,  note: 'från sidan' },
};

const args = process.argv.slice(2);
const flag = (name, fallback) => {
  const i = args.indexOf('--' + name);
  return i >= 0 && args[i + 1] ? args[i + 1] : fallback;
};
if (args.includes('--help') || args.includes('-h')) {
  console.log('Vyer: ' + Object.entries(VIEWS).map(([k, v]) => `\n  ${k.padEnd(9)} ${v.note}`).join(''));
  console.log('\nFlaggor: --stl <mapp> --out <mapp> --views a,b --format webp|png --scale 1 --quality 88');
  process.exit(0);
}

const stlDir  = path.resolve(HERE, flag('stl', '../../hardware/v2'));
const outDir  = path.resolve(HERE, flag('out', '../../site/assets'));
const format  = flag('format', 'webp');
const scale   = Number(flag('scale', 1));
const quality = Number(flag('quality', 88));
const wanted  = flag('views', Object.keys(VIEWS).filter((v) => v !== 'side').join(',')).split(',');

for (const v of wanted) if (!VIEWS[v]) { console.error(`Okänd vy: ${v}`); process.exit(1); }
if (!existsSync(stlDir)) { console.error(`Hittar inte STL-mappen: ${stlDir}`); process.exit(1); }

// STL-filerna ligger i en mapp per platta. Servern plattar ut det till /stl/<namn>.stl.
const { readdir } = await import('node:fs/promises');
async function findStls(dir, found = new Map()) {
  for (const e of await readdir(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) await findStls(p, found);
    else if (e.name.toLowerCase().endsWith('.stl')) found.set(e.name, p);
  }
  return found;
}
const stls = await findStls(stlDir);
if (!stls.size) { console.error(`Inga STL-filer under ${stlDir}`); process.exit(1); }

const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.stl': 'model/stl' };
const server = createServer(async (req, res) => {
  try {
    const url = decodeURIComponent(req.url.split('?')[0]);
    const file = url.startsWith('/stl/') ? stls.get(path.basename(url)) : path.join(HERE, url);
    if (!file || !existsSync(file)) { res.writeHead(404).end('404'); return; }
    res.writeHead(200, { 'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream' });
    res.end(await readFile(file));
  } catch (err) { res.writeHead(500).end(String(err)); }
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const base = `http://127.0.0.1:${server.address().port}`;

// playwright finns som fullpaket eller som -core (då måste webbläsaren pekas ut).
const { chromium } = await import('playwright').catch(() => import('playwright-core'));
const browser = await chromium.launch({
  executablePath: process.env.LOVGLOD_CHROMIUM || undefined,
  args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'],
});
const page = await browser.newPage();
page.on('pageerror', (e) => console.error('  scenfel:', e.message));

let sharp = null;
try { ({ default: sharp } = await import('sharp')); }
catch { console.warn('sharp saknas — bilderna sparas som png i full storlek'); }

await mkdir(outDir, { recursive: true });
for (const view of wanted) {
  const v = VIEWS[view];
  const w = Math.round(v.w * scale), h = Math.round(v.h * scale);
  const q = new URLSearchParams({ view, w, h });
  if (v.fov)  q.set('fov', v.fov);
  if (v.dist) q.set('dist', v.dist);
  if (v.glow === 0) q.set('glow', '0');

  await page.setViewportSize({ width: w, height: h });
  await page.goto(`${base}/scene.html?${q}`);
  await page.waitForFunction(() => window.__done, null, { timeout: 180000 });
  const missing = await page.evaluate(() => window.__missing);
  if (missing.length) console.warn(`  saknade delar: ${missing.join(', ')}`);

  const png = await page.locator('canvas').screenshot({ omitBackground: true });
  const name = `${view}.${sharp ? format : 'png'}`;
  const target = path.join(outDir, name);
  if (sharp) {
    // Trimma bort den tomma luften runt motivet först, så bilderna får samma
    // vikt i sidan oavsett hur stor kameran råkade rama in.
    const img = sharp(png).trim({ threshold: 0 });
    const out = Math.round(v.out * scale);
    await img.resize({ width: out, withoutEnlargement: true })
             .toFormat(format, { quality })
             .toFile(target);
  } else {
    await writeFile(target, png);
  }
  console.log(`${name.padEnd(16)} ${v.note}`);
}

await browser.close();
server.close();
console.log(`\nKlart — ${wanted.length} bilder i ${outDir}`);
