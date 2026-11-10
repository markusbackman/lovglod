// Mobilbilderna av portalen på Kom igång-sidan.
//
// CSS:en och logotypen läses direkt ur firmwaren (src/portal.cpp och
// include/logo_svg.h), så de följer med när portalen ändras. Sidornas markup
// speglar head(), handleSetup() och handleStatus() — ändras de måste
// mallarna nedan ändras med.
//
// Bilderna renderas i WebKit med iPhone-emulering och utan Antonio och
// Barlow, som i en riktig mobil: lampan skickar inga typsnitt och har ingen
// internetförbindelse i setup-läget, så rubrikerna faller tillbaka på
// Avenir Next Condensed och texten på systemtypsnittet.
//
//   cd tools/render
//   npx playwright install webkit   # första gången
//   node portal.mjs                 # → site/assets/portal-*.webp

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { webkit, devices } from 'playwright';
import sharp from 'sharp';

const root = fileURLToPath(new URL('../../', import.meta.url));
const read = (p) => readFileSync(root + p, 'utf8');

const raw = (src, name, tag) => {
  const m = src.match(new RegExp(`${name}\\[\\] PROGMEM = R"${tag}\\(([\\s\\S]*?)\\)${tag}"`));
  if (!m) throw new Error(`hittar inte ${name}`);
  return m[1];
};
const css = raw(read('src/portal.cpp'), 'PAGE_CSS', 'CSS');
const logoSrc = read('include/logo_svg.h');
const logo = raw(logoSrc, 'LOGO_SVG', 'SVG');
const icon = raw(logoSrc, 'LOGO_ICON_SVG', 'SVG');

// Ingen webbfont får smita in: lampan skickar inga.
if (css.includes('@font-face') || css.includes('fonts.g'))
  throw new Error('portalen laddar typsnitt nu — se över skriptet');

const head = (title, mode, eyebrow, heading, lead = '') =>
  `<!doctype html><html lang=sv><head><meta charset=utf-8>` +
  `<meta name=viewport content='width=device-width,initial-scale=1'>` +
  `<meta name=theme-color content='#0C2A1F'><link rel=icon href=/icon.svg><title>${title}</title>` +
  `<style>${css}</style></head><body><header class=bar><div class=in>` +
  `<img src=/logo.svg alt='LövGlöd'><small>${mode}</small></div></header>` +
  `<section class=hero><div class=in><p class=eye>${eyebrow}</p><h1>${heading}</h1>` +
  (lead ? `<p class=lead>${lead}</p>` : '') + `</div></section><main class=in>`;
const END = '</main></body></html>';

const setup =
  head('LövGlöd — WiFi', 'Setup', 'Välkommen hem', `Anslut din<img class=mark src=/logo.svg alt='LövGlöd'>`,
       'Välj ditt WiFi så börjar lampan följa Björklöven.') +
  `<div class=card><form method=POST action=/save><label>Nätverk</label><select name=ssid_pick>` +
  `<option value=''>— välj nätverk —</option>` +
  `<option value='Hemma' selected>Hemma  (-52 dBm)</option>` +
  `<option value='Grannen'>Grannen  (-78 dBm)</option></select>` +
  `<label>Nätverksnamn (SSID)</label><input id=ssid name=ssid required value='Hemma'>` +
  `<label>Lösenord</label><input name=pass type=password value='hemligtpass'>` +
  `<input type=hidden name=telef value=1><label class=check><input type=checkbox name=tele value=1 checked>` +
  `<span>Skicka driftstatistik till den som byggt lampan — version, omstarter och WiFi-signal. ` +
  `Inget om ditt nät eller hem.</span></label>` +
  `<button type=submit>Spara och anslut</button></form>` +
  `<p class=note>Lampan startar om och ansluter. Lyckas det inte dyker det här nätverket upp igen.</p></div>` + END;

const row = (k, v, hi) => `<tr><td>${k}</td><td${hi ? ' class=ok' : ''}>${v}</td></tr>`;
const status =
  head('LövGlöd — Admin', 'Admin', 'v1.8.0 · 192.168.1.42',
       `<img class=mark src=/logo.svg alt='LövGlöd'>`, 'Läge: <b>Standby</b>') +
  `<h2>Status</h2><div class=card><table>` +
  row('Läge', 'Standby', true) +
  row('Lampa', 'Alltid på', true) +
  row('Mellan matcher', 'Växlar var 15 min — visar nu Gröna kometer') +
  row('Nästa match', 'IFB – MODO lör 14 nov 15:15') +
  row('Senaste resultat', 'IFB 4 – 2 LIF') +
  row('Ställning nu', '—') +
  `</table></div>` + END;

// 390 punkter brett, retina. Höjden slutar en bit under första kortet, så att
// hela setup-formuläret och hela statustabellen kommer med.
const W = 390;
const browser = await webkit.launch();
const ctx = await browser.newContext({
  ...devices['iPhone 13'], viewport: { width: W, height: 1000 }, deviceScaleFactor: 2,
});
const page = await ctx.newPage();
await page.route('http://lampa.local/**', (r) => {
  const p = new URL(r.request().url()).pathname;
  if (p === '/logo.svg') return r.fulfill({ contentType: 'image/svg+xml', body: logo });
  if (p === '/icon.svg') return r.fulfill({ contentType: 'image/svg+xml', body: icon });
  return r.fulfill({ contentType: 'text/html; charset=utf-8', body: p === '/setup' ? setup : status });
});

for (const [name, path] of [['portal-setup', '/setup'], ['portal-status', '/']]) {
  await page.goto('http://lampa.local' + path);
  await page.evaluate(() => document.fonts.ready);
  const H = Math.ceil(await page.evaluate(() =>
    document.querySelector('.card').getBoundingClientRect().bottom + 24));
  const png = await page.screenshot({ clip: { x: 0, y: 0, width: W, height: H } });
  const out = `${root}site/assets/${name}.webp`;
  await sharp(png).webp({ quality: 86 }).toFile(out);
  console.log('skrev', out, `${W}×${H} → visas som 300×${Math.round(H * 300 / W)}`);
}
await browser.close();
