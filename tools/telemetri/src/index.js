// LövGlöd driftstatistik — tar emot lampornas hälsorapporter och sparar dem i D1.
//
//   POST /v1/rapport               från lampan, se src/telemetry.cpp
//   GET  /v1/lampor                senaste läget per lampa        (ADMIN_TOKEN)
//   GET  /v1/lampor/:id            rapporter och händelser        (ADMIN_TOKEN)
//
// Avsändarens IP-adress läses aldrig och sparas inte. Det enda som
// identifierar en lampa är det slumpade ID:t och smeknamnet ägaren satt.

const MAX_BODY = 8 * 1024;
const MIN_GAP_S = 5 * 60;          // en lampa rapporterar var 6:e timme; tätare än så är fel
const MAX_EVENTS = 32;
const ID_RE = /^[0-9a-f]{16}$/;

const json = (data, status = 200) =>
  new Response(JSON.stringify(data), {
    status,
    headers: { "content-type": "application/json; charset=utf-8" },
  });

const str = (v, max) => (typeof v === "string" ? v.slice(0, max) : null);
const int = (v) => (Number.isFinite(v) ? Math.trunc(v) : null);

async function report(request, env) {
  if (request.headers.get("x-lovglod-key") !== env.LAMP_KEY) return json({ error: "nyckel" }, 401);

  const raw = await request.text();
  if (raw.length > MAX_BODY) return json({ error: "för stor" }, 413);

  let r;
  try { r = JSON.parse(raw); } catch { return json({ error: "inte json" }, 400); }
  if (r?.v !== 1 || !ID_RE.test(r.id ?? "")) return json({ error: "okänt format" }, 400);

  const now = Math.floor(Date.now() / 1000);
  const lamp = await env.DB.prepare("SELECT last_seen FROM lamps WHERE id = ?").bind(r.id).first();
  if (lamp && now - lamp.last_seen < MIN_GAP_S) return json({ error: "för tätt" }, 429);

  const ins = await env.DB.prepare(
    `INSERT INTO reports (lamp_id, received_at, fw, beta, up, boots, bad_boots, reset,
                          heap, min_heap, rssi, shl_err, sse_re, body)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`
  ).bind(
    r.id, now, str(r.fw, 32), r.beta ? 1 : 0, int(r.up), int(r.boots), int(r.badBoots),
    str(r.reset, 16), int(r.heap), int(r.minHeap), int(r.rssi), int(r.shlErr), int(r.sseRe), raw
  ).run();
  const reportId = ins.meta.last_row_id;

  // Lampans klocka kan ha varit osynkad när händelsen inträffade (t = 0). Då
  // räknas tiden fram från upptiden: rapporten skickades vid r.up, händelsen
  // skedde vid e.up, och vi tog emot rapporten nu.
  const events = Array.isArray(r.events) ? r.events.slice(0, MAX_EVENTS) : [];
  const stmts = events
    .filter((e) => typeof e?.type === "string")
    .map((e) => {
      const at = e.t > 0 ? int(e.t) : now - Math.max(0, (int(r.up) ?? 0) - (int(e.up) ?? 0));
      return env.DB.prepare(
        "INSERT INTO events (lamp_id, report_id, at, type, detail) VALUES (?, ?, ?, ?, ?)"
      ).bind(r.id, reportId, at, str(e.type, 24), str(e.detail, 120));
    });

  stmts.push(
    env.DB.prepare(
      `INSERT INTO lamps (id, name, fw, first_seen, last_seen, last_report)
       VALUES (?1, ?2, ?3, ?4, ?4, ?5)
       ON CONFLICT (id) DO UPDATE SET name = ?2, fw = ?3, last_seen = ?4, last_report = ?5`
    ).bind(r.id, str(r.name, 32), str(r.fw, 32), now, reportId)
  );
  await env.DB.batch(stmts);

  const reply = { enabled: true };
  const interval = parseInt(env.INTERVAL_S, 10);
  if (interval > 0) reply.interval = interval;
  return json(reply);
}

function authorized(request, env) {
  return env.ADMIN_TOKEN && request.headers.get("authorization") === `Bearer ${env.ADMIN_TOKEN}`;
}

async function lamps(env) {
  const { results } = await env.DB.prepare(
    `SELECT l.id, l.name, l.fw, l.first_seen, l.last_seen,
            r.up, r.boots, r.bad_boots, r.reset, r.heap, r.min_heap, r.rssi, r.shl_err, r.sse_re
       FROM lamps l LEFT JOIN reports r ON r.id = l.last_report
      ORDER BY l.last_seen DESC`
  ).all();
  return json(results);
}

async function lampDetail(env, id) {
  if (!ID_RE.test(id)) return json({ error: "okänt id" }, 404);
  const [reports, events] = await env.DB.batch([
    env.DB.prepare(
      `SELECT received_at, fw, up, boots, bad_boots, reset, heap, min_heap, rssi, shl_err, sse_re
         FROM reports WHERE lamp_id = ? ORDER BY received_at DESC LIMIT 200`
    ).bind(id),
    env.DB.prepare(
      "SELECT at, type, detail FROM events WHERE lamp_id = ? ORDER BY at DESC LIMIT 200"
    ).bind(id),
  ]);
  return json({ reports: reports.results, events: events.results });
}

export default {
  async fetch(request, env) {
    const { pathname } = new URL(request.url);

    if (pathname === "/v1/rapport") {
      if (request.method !== "POST") return json({ error: "POST" }, 405);
      return report(request, env);
    }

    if (pathname.startsWith("/v1/lampor")) {
      if (!authorized(request, env)) return json({ error: "behörighet" }, 401);
      const id = pathname.split("/")[3];
      return id ? lampDetail(env, id) : lamps(env);
    }

    return json({ error: "finns inte" }, 404);
  },

  async scheduled(_event, env) {
    const days = parseInt(env.RETENTION_DAYS, 10) || 365;
    const cutoff = Math.floor(Date.now() / 1000) - days * 86400;
    await env.DB.batch([
      env.DB.prepare("DELETE FROM reports WHERE received_at < ?").bind(cutoff),
      env.DB.prepare("DELETE FROM events WHERE at < ?").bind(cutoff),
    ]);
  },
};
