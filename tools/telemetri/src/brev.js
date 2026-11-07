// Signaler och veckobrev till den som sköter flottan.
//
//   Ny lampa   → push via ntfy (NTFY_TOPIC), annars mejl om det går.
//   Veckobrev  → mejl via Resend (RESEND_API_KEY, MAIL_TO), måndag morgon.
//
// Inget av det här är nödvändigt: saknas hemligheterna händer ingenting, och
// lamporna märker aldrig om ett utskick misslyckas.

// Samma gränser som översiktssidan (dashboard.html), så att brevet och sidan
// är överens om vad som är "tyst" och "titta".
export const SILENT_S = 12 * 3600;
const WEAK_RSSI = -80;
const LOW_HEAP = 30 * 1024;
const BAD_RESETS = new Set(["brownout", "panic", "int_wdt", "task_wdt", "wdt"]);
const RESET_SV = {
  brownout: "brownout", panic: "krasch", int_wdt: "watchdog", task_wdt: "watchdog", wdt: "watchdog",
};
// En lampa som varit tyst längre än så räknas som nedplockad och tjatas inte om.
const GONE_S = 30 * 86400;
const WEEK_S = 7 * 86400;

const lampName = (l) => l.name || `Lampa ${l.id.slice(0, 4)}`;
const kB = (b) => `${Math.round(b / 1024)} kB`;
const esc = (s) =>
  String(s).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
const dateSv = (t) =>
  new Date(t * 1000).toLocaleDateString("sv-SE", { timeZone: "Europe/Stockholm", day: "numeric", month: "short" });
const dateTimeSv = (t) =>
  new Date(t * 1000).toLocaleString("sv-SE", { timeZone: "Europe/Stockholm", dateStyle: "short", timeStyle: "short" });
function agoSv(s) {
  if (s < 3600 * 48) return `${Math.round(s / 3600)} h`;
  return `${Math.round(s / 86400)} dagar`;
}

// "1.10.0" > "1.9.2". Pre-release-suffix ("-rc1") räknas som äldre än utan.
function cmpVersion(a, b) {
  const pa = String(a).split("-"), pb = String(b).split("-");
  const na = pa[0].split(".").map(Number), nb = pb[0].split(".").map(Number);
  for (let i = 0; i < Math.max(na.length, nb.length); i++) {
    const d = (na[i] || 0) - (nb[i] || 0);
    if (d) return d;
  }
  if (pa[1] && !pb[1]) return -1;
  if (!pa[1] && pb[1]) return 1;
  return (pa[1] || "").localeCompare(pb[1] || "", "sv", { numeric: true });
}

// ── Utskick ────────────────────────────────────────────────────────────────

async function push(env, { title, message, tags, priority }) {
  if (!env.NTFY_TOPIC) return false;
  const base = (env.NTFY_URL || "https://ntfy.sh").replace(/\/$/, "");
  const headers = { "content-type": "application/json" };
  if (env.NTFY_TOKEN) headers.authorization = `Bearer ${env.NTFY_TOKEN}`;
  const body = { topic: env.NTFY_TOPIC, title, message, tags, priority };
  if (env.DASHBOARD_URL) body.click = env.DASHBOARD_URL;
  const r = await fetch(base, { method: "POST", headers, body: JSON.stringify(body) });
  if (!r.ok) throw new Error(`ntfy ${r.status}: ${await r.text()}`);
  return true;
}

export async function mail(env, { subject, html, text }) {
  if (!env.RESEND_API_KEY || !env.MAIL_TO) return false;
  const r = await fetch("https://api.resend.com/emails", {
    method: "POST",
    headers: { authorization: `Bearer ${env.RESEND_API_KEY}`, "content-type": "application/json" },
    body: JSON.stringify({
      from: env.MAIL_FROM || "LövGlöd <onboarding@resend.dev>",
      to: env.MAIL_TO.split(",").map((s) => s.trim()).filter(Boolean),
      subject, html, text,
    }),
  });
  if (!r.ok) throw new Error(`resend ${r.status}: ${await r.text()}`);
  return true;
}

// ── Ny lampa ───────────────────────────────────────────────────────────────

export async function newLamp(env, r, total) {
  const name = typeof r.name === "string" && r.name ? r.name.slice(0, 32) : `Lampa ${r.id.slice(0, 4)}`;
  const facts = [
    `Firmware ${typeof r.fw === "string" ? r.fw.slice(0, 32) : "?"}${r.beta ? " (beta)" : ""}`,
    Number.isFinite(r.leds) ? `${r.leds} lysdioder${typeof r.strip === "string" ? ` ${r.strip.slice(0, 16)}` : ""}` : null,
    Number.isFinite(r.rssi) ? `WiFi ${r.rssi} dBm` : null,
  ].filter(Boolean);
  const title = `Ny LövGlöd: ${name}`;
  const message = `${facts.join(" · ")}\nFlottan har nu ${total} ${total === 1 ? "lampa" : "lampor"}.`;
  try {
    if (await push(env, { title, message, tags: ["bulb", "tada"], priority: 4 })) return;
    await mail(env, {
      subject: title,
      text: message,
      html: `<p><strong>${esc(name)}</strong> har anslutit flottan.</p><p>${esc(facts.join(" · "))}<br>` +
            `${esc(message.split("\n")[1])}</p>`,
    });
  } catch (e) {
    console.error("signal om ny lampa", e);
  }
}

// ── Veckobrevet ────────────────────────────────────────────────────────────

export async function weekly(env, now = Math.floor(Date.now() / 1000)) {
  const since = now - WEEK_S;
  const [lampsQ, eventsQ, reportsQ, prevQ] = await env.DB.batch([
    env.DB.prepare(
      `SELECT l.id, l.name, l.fw, l.first_seen, l.last_seen,
              r.beta, r.up, r.bad_boots, r.reset, r.min_heap, r.rssi
         FROM lamps l LEFT JOIN reports r ON r.id = l.last_report`
    ),
    env.DB.prepare(
      `SELECT e.lamp_id, e.at, e.type, e.detail FROM events e WHERE e.at >= ? ORDER BY e.at`
    ).bind(since),
    // Per lampa under veckan: antal rapporter, sämsta signal och minne.
    env.DB.prepare(
      `SELECT lamp_id, COUNT(*) AS n, MIN(rssi) AS min_rssi, MIN(min_heap) AS min_heap
         FROM reports WHERE received_at >= ? GROUP BY lamp_id`
    ).bind(since),
    env.DB.prepare(
      `SELECT COUNT(DISTINCT lamp_id) AS n FROM reports WHERE received_at >= ? AND received_at < ?`
    ).bind(since - WEEK_S, since),
  ]);

  const lamps = lampsQ.results;
  const events = eventsQ.results;
  const week = new Map(reportsQ.results.map((w) => [w.lamp_id, w]));
  const byId = new Map(lamps.map((l) => [l.id, l]));
  const nameOf = (id) => (byId.has(id) ? lampName(byId.get(id)) : `Lampa ${id.slice(0, 4)}`);

  const active = lamps.filter((l) => l.last_seen >= since);
  const newOnes = lamps.filter((l) => l.first_seen >= since).sort((a, b) => a.first_seen - b.first_seen);
  const prevActive = prevQ.results[0]?.n ?? 0;

  // Versioner bland lamporna som hörts av i veckan.
  const versions = new Map();
  for (const l of active) versions.set(l.fw || "?", (versions.get(l.fw || "?") || 0) + 1);
  const versionList = [...versions].sort((a, b) => cmpVersion(b[0], a[0]));
  const stable = active.filter((l) => l.fw && !l.fw.includes("-")).map((l) => l.fw);
  const newest = stable.sort(cmpVersion).at(-1);
  const betaCount = active.filter((l) => l.beta).length;

  const count = (t) => events.filter((e) => e.type === t).length;
  const otaOk = count("ota_ok"), otaFail = count("ota_fail"), rollback = count("ota_rollback");
  const boots = count("boot");

  // ── Saker att möjligen agera på ──
  const act = [];
  for (const l of lamps) {
    const quiet = now - l.last_seen;
    if (quiet > SILENT_S && quiet < GONE_S) {
      act.push({ lamp: l, sev: 3, text: `Tyst i ${agoSv(quiet)}, senast hörd ${dateTimeSv(l.last_seen)}` });
    }
  }
  for (const l of active) {
    if (now - l.last_seen > SILENT_S) continue;
    if (l.bad_boots > 0) act.push({ lamp: l, sev: 2, text: `${l.bad_boots} onormala omstarter i följd` });
    if (BAD_RESETS.has(l.reset)) act.push({ lamp: l, sev: 2, text: `Senaste omstart: ${RESET_SV[l.reset]}` });
    const w = week.get(l.id);
    if (w?.min_rssi != null && w.min_rssi < WEAK_RSSI) {
      act.push({ lamp: l, sev: 1, text: `Svag WiFi, ner till ${w.min_rssi} dBm (nu ${l.rssi ?? "?"} dBm)` });
    }
    if (w?.min_heap != null && w.min_heap < LOW_HEAP) {
      act.push({ lamp: l, sev: 1, text: `Lite minne, ${kB(w.min_heap)} som lägst` });
    }
    if (newest && l.fw && !l.beta && cmpVersion(l.fw, newest) < 0 && now - l.first_seen > 2 * 86400) {
      act.push({ lamp: l, sev: 0, text: `Kör ${l.fw}, nyaste i flottan är ${newest}` });
    }
  }
  const failsBy = new Map();
  for (const e of events) {
    if (e.type !== "ota_fail" && e.type !== "ota_rollback") continue;
    const k = `${e.lamp_id}|${e.type}`;
    failsBy.set(k, [...(failsBy.get(k) || []), e]);
  }
  for (const [k, es] of failsBy) {
    const [id, type] = k.split("|");
    const what = type === "ota_rollback" ? "Firmware rullades tillbaka" : "Uppdatering misslyckades";
    const detail = es.at(-1).detail ? ` (${es.at(-1).detail})` : "";
    act.push({ lamp: byId.get(id) ?? { id }, sev: type === "ota_rollback" ? 3 : 2,
               text: `${what}${es.length > 1 ? ` ${es.length} gånger` : ""}${detail}` });
  }
  // En rad per lampa, allvarligast först.
  const perLamp = new Map();
  for (const a of act) {
    const g = perLamp.get(a.lamp.id) ?? { id: a.lamp.id, sev: 0, why: [] };
    g.sev = Math.max(g.sev, a.sev);
    g.why.push(a.text);
    perLamp.set(a.lamp.id, g);
  }
  const todo = [...perLamp.values()].sort((a, b) => b.sev - a.sev || nameOf(a.id).localeCompare(nameOf(b.id), "sv"));

  // ── Text ──
  const range = `${dateSv(since)} – ${dateSv(now)}`;
  const delta = active.length - prevActive;
  const stats = [
    ["Lampor i drift", `${active.length}${prevActive ? ` (${delta >= 0 ? "+" : ""}${delta} mot förra veckan)` : ""}`],
    ["Nya i veckan", String(newOnes.length)],
    ["Kända totalt", String(lamps.length)],
    ["Tysta just nu", String(lamps.filter((l) => now - l.last_seen > SILENT_S && now - l.last_seen < GONE_S).length)],
    ["Rapporter", String(reportsQ.results.reduce((s, w) => s + w.n, 0))],
    ["Uppdateringar", `${otaOk} lyckade, ${otaFail} misslyckade, ${rollback} tillbakarullade`],
    ["Starter", String(boots)],
    ["Betakanalen", `${betaCount} av ${active.length}`],
  ];

  const subject = todo.length
    ? `LövGlöd vecka: ${active.length} lampor, ${todo.length} att titta på`
    : `LövGlöd vecka: ${active.length} lampor, allt lugnt`;

  const lines = [
    `Flottan ${range}`, "",
    ...stats.map(([k, v]) => `${k}: ${v}`), "",
    "Versioner:", ...versionList.map(([v, n]) => `  ${v}: ${n}`), "",
  ];
  if (newOnes.length) {
    lines.push("Nya lampor:", ...newOnes.map((l) => `  ${lampName(l)} — ${dateSv(l.first_seen)}, ${l.fw ?? "?"}`), "");
  }
  lines.push(todo.length ? "Att möjligen agera på:" : "Inget att agera på den här veckan.");
  for (const g of todo) lines.push(`  ${nameOf(g.id)}: ${g.why.join("; ")}`);
  if (env.DASHBOARD_URL) lines.push("", env.DASHBOARD_URL);
  const text = lines.join("\n");

  const SEV = ["#6b6b6b", "#b07d00", "#b07d00", "#d03b3b"];
  const td = 'style="padding:4px 12px 4px 0;vertical-align:top"';
  const html = `<!DOCTYPE html><html lang="sv"><body style="font:15px/1.45 -apple-system,Segoe UI,Roboto,sans-serif;color:#1a1a1a;max-width:640px">
<h2 style="margin:0 0 4px">Flottan vecka ${isoWeek(now)}</h2>
<p style="margin:0 0 16px;color:#6b6b6b">${esc(range)}</p>
<table style="border-collapse:collapse">${stats.map(([k, v]) => `<tr><td ${td}>${esc(k)}</td><td ${td}><strong>${esc(v)}</strong></td></tr>`).join("")}</table>
<h3>Versioner</h3>
<table style="border-collapse:collapse">${versionList.map(([v, n]) => `<tr><td ${td}><code>${esc(v)}</code>${v === newest ? " (nyast)" : ""}</td><td ${td}>${n}</td></tr>`).join("")}</table>
${newOnes.length ? `<h3>Nya lampor</h3><ul>${newOnes.map((l) => `<li><strong>${esc(lampName(l))}</strong> — ${esc(dateSv(l.first_seen))}, ${esc(l.fw ?? "?")}</li>`).join("")}</ul>` : ""}
<h3>Att möjligen agera på</h3>
${todo.length ? `<ul>${todo.map((g) => `<li><span style="color:${SEV[g.sev]}">●</span> <strong>${esc(nameOf(g.id))}</strong>: ${esc(g.why.join("; "))}</li>`).join("")}</ul>` : "<p>Inget den här veckan.</p>"}
${env.DASHBOARD_URL ? `<p><a href="${esc(env.DASHBOARD_URL)}">Öppna översiktssidan</a></p>` : ""}
</body></html>`;

  return { subject, text, html };
}

function isoWeek(t) {
  const d = new Date(t * 1000);
  const day = (d.getUTCDay() + 6) % 7;
  d.setUTCDate(d.getUTCDate() - day + 3);
  const jan4 = new Date(Date.UTC(d.getUTCFullYear(), 0, 4));
  return 1 + Math.round(((d - jan4) / 86400000 - 3 + ((jan4.getUTCDay() + 6) % 7)) / 7);
}
