#!/usr/bin/env python3
"""
Mock-server för Björklöven-lampan.

Styr lampan i labbet mitt i sommaruppehållet genom att trycka in ett påhittat
matchläge i den:

    POST http://<lampan>/push        hela matchläget i ett anrop

Servern trycker om läget varje gång något ändras, och som hjärtslag däremellan
så att leasen i firmwaren (PUSH_LEASE_MS) aldrig hinner löpa ut. Slutar vi
trycka går lampan tillbaka till riktiga SHL av sig själv.

Riktningen är vald med flit. Hämtade lampan i stället härifrån skulle den
behöva nå datorn, och där står brandväggen i macOS/Windows i vägen på ett sätt
man inte rår över från lampan. Push är en vanlig utgående anslutning från
datorn och behöver ingenting öppnat.

Styrsidan ligger på  /  där matchläget ställs in för hand eller spelas upp
automatiskt.

Kör:
    python3 mock/server.py                 # lyssnar på 0.0.0.0:8080
    python3 mock/server.py --port 9000

Fyll sedan i lampans adress på styrsidan och slå på push.

Inga beroenden utanför Pythons standardbibliotek.
"""

from __future__ import annotations

import argparse
import json
import random
import socket
import socketserver
import threading
import time
from collections import deque
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse
from urllib.request import Request, urlopen

try:
    from zoneinfo import ZoneInfo
    TZ = ZoneInfo("Europe/Stockholm")
except Exception:                                    # pragma: no cover
    # Fallback om tzdata saknas. Firmwaren jämför bara lokala datum, så en
    # timmes fel i övergången till vintertid gör ingen praktisk skada.
    TZ = timezone(timedelta(hours=2))

# Måste stämma med SHL_TEAM_CODE i include/config.h.
TEAM_CODE = "IFB"


def iso_utc(epoch: float) -> str:
    """SHL:s format: 2026-09-19T16:00:00.000Z (firmwaren tolkar det som UTC)."""
    return datetime.fromtimestamp(epoch, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z")


def local_evening(days_ago: int, hour: int = 19) -> float:
    """Epoch för kl 19:00 svensk tid, N dagar bakåt."""
    d = datetime.now(TZ) - timedelta(days=days_ago)
    return d.replace(hour=hour, minute=0, second=0, microsecond=0).timestamp()


def lan_ip() -> str:
    """Adressen lampan ska peka på. UDP-socketen skickar aldrig något."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 53))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


# ─────────────────────────────────────────────────────────────────────────────
#  Tillstånd
# ─────────────────────────────────────────────────────────────────────────────
class Mock:
    # Firmwarens matchfönster, se LIVE_WINDOW_* i include/config.h.
    WINDOW_PRE = 15 * 60
    WINDOW_POST = 4 * 60 * 60

    def __init__(self) -> None:
        self.lock = threading.RLock()

        self.opponent = "LHF"
        self.at_home = True
        self.start = time.time() + 30 * 60        # nästa match om en halvtimme

        self.last_opponent = "SAIK"
        self.last_at_home = False
        self.last_us = 4
        self.last_them = 2
        self.last_days_ago = 1                    # 1 = igår → gnistor ska tändas

        self.us = 0                               # ställning i pågående match
        self.them = 0

        self.sim: dict | None = None
        self.sim_stop = threading.Event()

        self.log: deque = deque(maxlen=60)
        self.last_seen = 0.0                      # senaste lyckade push

        # Vart vi trycker. Utan adress står servern stilla — den har inget
        # annat sätt att nå lampan.
        self.lamp_host = ""                       # t.ex. "192.168.1.250"
        self.push_on = False
        self.push_ok = False
        self.push_msg = "av"
        self.push_at = 0.0                        # senaste lyckade push
        self.push_seq = 0                         # bumpas när något ändrats

    # ── härledda värden ────────────────────────────────────────────────────
    def codes(self) -> tuple[str, str]:
        return (TEAM_CODE, self.opponent) if self.at_home else (self.opponent, TEAM_CODE)

    def home_away_score(self) -> tuple[int, int]:
        return (self.us, self.them) if self.at_home else (self.them, self.us)

    def last_codes(self) -> tuple[str, str]:
        return ((TEAM_CODE, self.last_opponent) if self.last_at_home
                else (self.last_opponent, TEAM_CODE))

    def last_home_away_score(self) -> tuple[int, int]:
        return ((self.last_us, self.last_them) if self.last_at_home
                else (self.last_them, self.last_us))

    def window_open(self) -> bool:
        now = time.time()
        return self.start - self.WINDOW_PRE <= now <= self.start + self.WINDOW_POST

    def bump(self) -> None:
        self.push_seq += 1                        # väck pushtråden direkt

    def note(self, text: str, from_lamp: bool = False) -> None:
        stamp = datetime.now(TZ).strftime("%H:%M:%S")
        self.log.appendleft({"t": stamp, "text": text, "lamp": from_lamp})

    # ── styrning ───────────────────────────────────────────────────────────
    def public_state(self) -> dict:
        home, away = self.codes()
        now = time.time()
        return {
            "opponent": self.opponent,
            "atHome": self.at_home,
            "matchup": f"{home} – {away}",
            "startIso": iso_utc(self.start),
            "startLocal": datetime.fromtimestamp(self.start, TZ).strftime("%a %d %b %H:%M"),
            "startsInSec": int(self.start - now),
            "windowOpen": self.window_open(),
            "us": self.us,
            "them": self.them,
            "scoreLine": f"{home} {self.home_away_score()[0]} – "
                         f"{self.home_away_score()[1]} {away}",
            "lastOpponent": self.last_opponent,
            "lastAtHome": self.last_at_home,
            "lastUs": self.last_us,
            "lastThem": self.last_them,
            "lastDaysAgo": self.last_days_ago,
            "lastWon": self.last_us > self.last_them,
            "sparklesExpected": self.last_us > self.last_them and self.last_days_ago == 1,
            "simRunning": self.sim is not None,
            "sim": self.sim or {},
            "lampHost": self.lamp_host,
            "pushOn": self.push_on,
            "pushOk": self.push_ok,
            "pushMsg": self.push_msg,
            "pushAgeSec": int(now - self.push_at) if self.push_at else -1,
            "lastSeenSec": int(now - self.last_seen) if self.last_seen else -1,
            "log": list(self.log),
        }

    def apply(self, a: dict) -> None:
        action = a.get("action", "")

        if action == "next_match":
            self.opponent = (a.get("opponent") or self.opponent).strip().upper()[:6]
            self.at_home = bool(a.get("atHome", self.at_home))
            self.note(f"nästa match satt: {' – '.join(self.codes())}", False)

        elif action == "start_in":
            minutes = float(a.get("minutes", 30))
            self.start = time.time() + minutes * 60
            self.note(f"nedsläpp om {minutes:g} min", False)

        elif action == "start_now":
            # En minut bakåt: matchfönstret är då garanterat öppet.
            self.start = time.time() - 60
            self.note("matchen startad nu", False)

        elif action == "start_tomorrow":
            self.start = local_evening(-1)
            self.note("nedsläpp imorgon 19:00", False)

        elif action == "goal":
            if a.get("side") == "them":
                self.them = max(0, self.them + int(a.get("delta", 1)))
            else:
                self.us = max(0, self.us + int(a.get("delta", 1)))
            self.bump()
            self.note(f"ställning {self.us}–{self.them} ({TEAM_CODE}–{self.opponent})", False)

        elif action == "set_score":
            self.us = max(0, int(a.get("us", 0)))
            self.them = max(0, int(a.get("them", 0)))
            self.bump()
            self.note(f"ställning satt till {self.us}–{self.them}", False)

        elif action == "reset_score":
            self.us = self.them = 0
            self.bump()
            self.note("ställningen nollställd", False)

        elif action == "last_result":
            self.last_opponent = (a.get("opponent") or self.last_opponent).strip().upper()[:6]
            self.last_at_home = bool(a.get("atHome", self.last_at_home))
            self.last_us = max(0, int(a.get("us", self.last_us)))
            self.last_them = max(0, int(a.get("them", self.last_them)))
            self.last_days_ago = max(0, int(a.get("daysAgo", self.last_days_ago)))
            self.note("senaste resultat uppdaterat", False)

        elif action == "quick_last":
            self.last_us, self.last_them = (4, 2) if a.get("won") else (1, 3)
            self.last_days_ago = 1
            self.note("igår: " + ("vinst" if a.get("won") else "förlust"), False)

        elif action == "finish_match":
            # Matchen är slut: skriv in den som gårdagens resultat och lägg
            # nästa match två dagar fram. Då ska lampan tända gnistor vid vinst.
            self.last_opponent = self.opponent
            self.last_at_home = self.at_home
            self.last_us, self.last_them = self.us, self.them
            self.last_days_ago = 1
            self.us = self.them = 0
            self.bump()
            self.start = time.time() + 2 * 24 * 3600
            self.note("match avslutad → skriven som gårdagens resultat", False)

        elif action == "sim_start":
            self.start_sim(a)

        elif action == "sim_stop":
            self.stop_sim()

        elif action == "clear_log":
            self.log.clear()

        elif action == "set_lamp":
            self.lamp_host = (a.get("lamp") or "").strip()
            self.push_on = bool(a.get("on", True)) and bool(self.lamp_host)
            self.push_ok = False
            self.push_msg = "väntar…" if self.push_on else "av"
            self.note(f"push {'på' if self.push_on else 'av'}"
                      f"{' → ' + self.lamp_host if self.push_on else ''}", False)

        self.push_seq += 1        # allt ovan ändrar läget lampan ska ha

    # ── push till lampan ───────────────────────────────────────────────────
    # Lampans egen webbserver tar emot hela matchläget på POST /push. Varje
    # push förlänger PUSH_LEASE_MS i firmwaren; slutar vi trycka går lampan
    # tillbaka till att hämta från SHL själv.
    def push_payload(self) -> dict:
        home, away = self.codes()
        hs, as_ = self.home_away_score()
        lhome, laway = self.last_codes()
        lhs, las = self.last_home_away_score()
        won = self.last_us > self.last_them
        when = datetime.fromtimestamp(self.start, TZ).strftime("%a %d %b %H:%M")
        return {
            "next": {
                "home": home,
                "away": away,
                "homeIsUs": self.at_home,
                "text": f"{home} – {away}  {when}",
            },
            "live": self.window_open(),
            "score": {"home": hs, "away": as_},
            "last": {
                # Gnistorna ska bara lysa dagen efter en vinst — samma villkor
                # som firmwaren själv använder mot riktiga SHL.
                "won": won and self.last_days_ago == 1,
                "text": f"{lhome} {lhs}-{las} {laway}  ({'vinst' if won else 'förlust'})",
            },
        }

    def push_once(self) -> None:
        with self.lock:
            if not (self.push_on and self.lamp_host):
                return
            host = self.lamp_host
            body = json.dumps(self.push_payload()).encode()
            was_ok = self.push_ok

        url = host if "://" in host else "http://" + host
        url = url.rstrip("/") + "/push"
        try:
            req = Request(url, data=body, headers={"Content-Type": "application/json"})
            with urlopen(req, timeout=4) as r:
                r.read()
            ok, msg = True, "levererad"
        except Exception as exc:                  # nätverksfel, timeout, 4xx
            ok, msg = False, f"{type(exc).__name__}: {exc}"

        with self.lock:
            self.push_ok = ok
            self.push_msg = msg
            if ok:
                self.push_at = time.time()
                self.last_seen = time.time()
            # Logga bara vändningarna — annars dränker heartbeaten loggen.
            if ok != was_ok:
                self.note(f"push {'når' if ok else 'når INTE'} {host}"
                          f"{'' if ok else ' — ' + msg}", ok)

    # ── automatisk uppspelning ─────────────────────────────────────────────
    def start_sim(self, a: dict) -> None:
        self.stop_sim()
        cfg = {
            "gapMin": max(1, int(a.get("gapMin", 8))),
            "gapMax": max(1, int(a.get("gapMax", 25))),
            "ourOdds": min(100, max(0, int(a.get("ourOdds", 60)))),
            "maxGoals": max(1, int(a.get("maxGoals", 7))),
            "scored": 0,
        }
        if cfg["gapMax"] < cfg["gapMin"]:
            cfg["gapMax"] = cfg["gapMin"]

        self.us = self.them = 0
        self.bump()
        self.start = time.time() - 60          # öppna matchfönstret direkt
        self.sim = cfg
        self.sim_stop.clear()
        self.note("simulerad match startad", False)

        threading.Thread(target=self._sim_loop, daemon=True).start()

    def stop_sim(self) -> None:
        if self.sim is not None:
            self.note("simuleringen stoppad", False)
        self.sim = None
        self.sim_stop.set()

    def _sim_loop(self) -> None:
        while True:
            with self.lock:
                cfg = self.sim
                if cfg is None:
                    return
                wait = random.uniform(cfg["gapMin"], cfg["gapMax"])

            if self.sim_stop.wait(wait):
                return

            with self.lock:
                cfg = self.sim
                if cfg is None:
                    return
                ours = random.randint(1, 100) <= cfg["ourOdds"]
                if ours:
                    self.us += 1
                else:
                    self.them += 1
                cfg["scored"] += 1
                self.bump()
                self.note(f"SIM-mål: {'Björklöven' if ours else self.opponent}"
                          f"  {self.us}–{self.them}", False)
                if cfg["scored"] >= cfg["maxGoals"]:
                    self.sim = None
                    self.note("simuleringen klar", False)
                    return


MOCK = Mock()


# ─────────────────────────────────────────────────────────────────────────────
#  Styrsidan
# ─────────────────────────────────────────────────────────────────────────────
PAGE = r"""<!doctype html>
<html lang=sv><head><meta charset=utf-8>
<meta name=viewport content='width=device-width,initial-scale=1'>
<title>Björklöven — mockserver</title>
<style>
:root{--bg:#0d1210;--card:#161d1a;--edge:#26332d;--txt:#e8f0ea;--dim:#8fa398;
      --gold:#ffc21a;--red:#ff6b5e;--green:#5ed6a0}
*{box-sizing:border-box}
body{margin:0;padding:22px;background:var(--bg);color:var(--txt);
     font:15px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
.wrap{max-width:920px;margin:0 auto}
h1{font-size:24px;margin:0 0 4px;letter-spacing:-.01em}
h1 span{color:var(--gold)}
.sub{color:var(--dim);font-size:13px;margin:0 0 20px}
.sub code{color:var(--gold);background:#0b100e;padding:2px 6px;border-radius:5px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:14px}
.card{background:var(--card);border:1px solid var(--edge);border-radius:14px;padding:16px 18px}
.card h2{font-size:13px;text-transform:uppercase;letter-spacing:.08em;color:var(--dim);
         margin:0 0 14px;font-weight:600}
label{display:block;font-size:12px;color:var(--dim);margin:12px 0 5px}
.row{display:flex;gap:8px}.row>*{flex:1}
input,select{width:100%;padding:9px 10px;border-radius:8px;border:1px solid var(--edge);
     background:#0b100e;color:var(--txt);font-size:15px}
button{padding:10px 12px;border:0;border-radius:8px;background:var(--gold);color:#10160f;
       font-size:14px;font-weight:640;cursor:pointer}
button.ghost{background:transparent;color:var(--txt);border:1px solid var(--edge);font-weight:400}
button.danger{background:transparent;color:var(--red);border:1px solid var(--edge);font-weight:400}
button:hover{filter:brightness(1.12)}
.btns{display:flex;flex-wrap:wrap;gap:8px;margin-top:14px}
.btns button{flex:1;min-width:110px}
.score{display:flex;align-items:center;justify-content:center;gap:18px;margin:6px 0 4px}
.score div{text-align:center;flex:1}
.score .n{font-size:44px;font-weight:700;line-height:1;font-variant-numeric:tabular-nums}
.score .c{font-size:12px;color:var(--dim);letter-spacing:.08em;margin-top:6px}
.score .sep{flex:0;color:var(--dim);font-size:26px}
.us .n{color:var(--gold)}
table{width:100%;border-collapse:collapse;font-size:14px}
td{padding:6px 0;border-bottom:1px solid var(--edge)}
td:first-child{color:var(--dim);width:48%}
tr:last-child td{border-bottom:0}
.pill{display:inline-block;padding:2px 9px;border-radius:99px;font-size:12px;
      border:1px solid var(--edge);color:var(--dim)}
.pill.on{color:#10160f;background:var(--green);border-color:var(--green);font-weight:600}
.pill.warn{color:#10160f;background:var(--gold);border-color:var(--gold);font-weight:600}
#log{font:12px/1.6 ui-monospace,SFMono-Regular,Menlo,monospace;color:var(--dim);
     background:#0b100e;border-radius:8px;padding:10px;max-height:260px;overflow:auto;margin:0}
#log b{color:var(--gold);font-weight:600}
#log .lamp{color:var(--txt)}
.full{grid-column:1/-1}
.hint{font-size:12px;color:var(--dim);margin:0 0 12px;line-height:1.45}
</style></head><body><div class=wrap>

<h1>Björk<span>löven</span> — mockserver</h1>
<p class=sub>Fyll i lampans adress under <b>Push till lampan</b> och slå på —
då trycks matchläget härifrån in i lampan istället för att den hämtar från shl.se.</p>

<div class=grid>

  <div class=card>
    <h2>Nästa match</h2>
    <label>Motståndare</label>
    <div class=row>
      <input id=opponent maxlength=6 placeholder=LHF>
      <select id=atHome>
        <option value=1>Björklöven hemma</option>
        <option value=0>Björklöven borta</option>
      </select>
    </div>
    <div class=btns><button onclick=saveNext()>Spara match</button></div>
    <label>Nedsläpp</label>
    <div class=row>
      <input id=minutes type=number value=30 step=5>
      <button class=ghost onclick=startIn()>minuter från nu</button>
    </div>
    <div class=btns>
      <button class=ghost onclick="act('start_now')">Starta nu</button>
      <button class=ghost onclick="act('start_tomorrow')">Imorgon 19:00</button>
    </div>
  </div>

  <div class=card>
    <h2>Ställning</h2>
    <div class=score>
      <div class=us><div class=n id=nUs>0</div><div class=c id=cUs>IFB</div></div>
      <div class=sep>–</div>
      <div><div class=n id=nThem>0</div><div class=c id=cThem>LHF</div></div>
    </div>
    <div class=btns>
      <button onclick="goal('us',1)">Mål Björklöven</button>
      <button class=ghost onclick="goal('them',1)">Mål motståndaren</button>
    </div>
    <div class=btns>
      <button class=ghost onclick="goal('us',-1)">−1 IFB</button>
      <button class=ghost onclick="goal('them',-1)">−1 mots.</button>
      <button class=ghost onclick="act('reset_score')">Nollställ</button>
    </div>
    <div class=btns>
      <button class=danger onclick="act('finish_match')">Avsluta match → gårdagens resultat</button>
    </div>
  </div>

  <div class=card>
    <h2>Automatisk uppspelning</h2>
    <label>Sekunder mellan mål (min / max)</label>
    <div class=row><input id=gapMin type=number value=8><input id=gapMax type=number value=25></div>
    <label>Andel Björklövensmål (%)</label>
    <input id=ourOdds type=number value=60 min=0 max=100>
    <label>Antal mål innan matchen är slut</label>
    <input id=maxGoals type=number value=7 min=1>
    <div class=btns>
      <button onclick=simStart()>Spela upp match</button>
      <button class=danger onclick="act('sim_stop')">Stoppa</button>
    </div>
    <p class=sub style="margin:14px 0 0">Uppspelningen nollställer ställningen och
    flyttar nedsläppet till "nu", så att lampans matchfönster öppnas direkt.</p>
  </div>

  <div class=card>
    <h2>Senaste match (styr gnistorna)</h2>
    <div class=btns>
      <button onclick="act('quick_last',{won:true})">Vi vann igår</button>
      <button class=ghost onclick="act('quick_last',{won:false})">Vi förlorade igår</button>
    </div>
    <label>Motståndare / plan</label>
    <div class=row>
      <input id=lastOpponent maxlength=6>
      <select id=lastAtHome><option value=1>Hemma</option><option value=0>Borta</option></select>
    </div>
    <label>Mål IFB / motståndaren / dagar sedan</label>
    <div class=row>
      <input id=lastUs type=number min=0>
      <input id=lastThem type=number min=0>
      <input id=lastDaysAgo type=number min=0>
    </div>
    <div class=btns><button onclick=saveLast()>Spara resultat</button></div>
  </div>

  <div class=card>
    <h2>Push till lampan</h2>
    <p class=hint>Servern trycker matchläget till lampans <code>/push</code> och
    fyller på leasen i firmwaren så länge push är på. Slår du av — eller stänger
    servern — går lampan tillbaka till riktiga SHL inom några minuter.</p>
    <label>Lampans adress</label>
    <input id=lamp placeholder="192.168.1.250">
    <div class=btns>
      <button onclick="pushOn()">Slå på push</button>
      <button class=ghost onclick="act('set_lamp',{lamp:val('lamp'),on:false})">Slå av</button>
    </div>
    <table><tr><td>Status</td><td id=sPush>—</td></tr></table>
  </div>

  <div class=card>
    <h2>Vad lampan ser</h2>
    <table>
      <tr><td>Nästa match</td><td id=sMatch>—</td></tr>
      <tr><td>Nedsläpp</td><td id=sStart>—</td></tr>
      <tr><td>Matchfönster</td><td id=sWindow>—</td></tr>
      <tr><td>Senast levererad</td><td id=sSeen>—</td></tr>
      <tr><td>Gårdagens resultat</td><td id=sLast>—</td></tr>
      <tr><td>Gnistor</td><td id=sSpark>—</td></tr>
      <tr><td>Simulering</td><td id=sSim>—</td></tr>
    </table>
  </div>

  <div class="card full">
    <h2>Händelser</h2>
    <pre id=log>(inget än)</pre>
    <div class=btns><button class=ghost onclick="act('clear_log')">Töm loggen</button></div>
  </div>

</div></div>

<script>
const $ = id => document.getElementById(id);
const val = id => $(id).value;

// Skriv aldrig över ett fält användaren står i.
function fill(id, v){ const e = $(id); if (document.activeElement !== e) e.value = v; }

async function act(action, extra){
  const body = Object.assign({action}, extra || {});
  const r = await fetch('/api/mock/action', {method:'POST', body: JSON.stringify(body)});
  render(await r.json());
}
const goal      = (side, delta) => act('goal', {side, delta});
const saveNext  = () => act('next_match', {opponent: val('opponent'), atHome: val('atHome')==='1'});
const startIn   = () => act('start_in',   {minutes: parseFloat(val('minutes')) || 0});
const saveLast  = () => act('last_result',{opponent: val('lastOpponent'),
                                           atHome: val('lastAtHome')==='1',
                                           us: +val('lastUs'), them: +val('lastThem'),
                                           daysAgo: +val('lastDaysAgo')});
const simStart  = () => act('sim_start', {gapMin:+val('gapMin'), gapMax:+val('gapMax'),
                                          ourOdds:+val('ourOdds'), maxGoals:+val('maxGoals')});
const pushOn    = () => act('set_lamp', {lamp: val('lamp'), on: true});

function human(sec){
  const s = Math.abs(sec), t = s < 90 ? s + ' s'
    : s < 5400 ? Math.round(s/60) + ' min' : (s/3600).toFixed(1) + ' h';
  return sec < 0 ? t + ' sedan' : 'om ' + t;
}

function render(s){
  $('nUs').textContent = s.us; $('nThem').textContent = s.them;
  $('cUs').textContent = 'IFB'; $('cThem').textContent = s.opponent;

  fill('opponent', s.opponent);          fill('atHome', s.atHome ? '1' : '0');
  fill('lastOpponent', s.lastOpponent);  fill('lastAtHome', s.lastAtHome ? '1' : '0');
  fill('lastUs', s.lastUs);              fill('lastThem', s.lastThem);
  fill('lastDaysAgo', s.lastDaysAgo);

  $('sMatch').textContent = s.matchup;
  $('sStart').textContent = s.startLocal + '  (' + human(s.startsInSec) + ')';
  $('sWindow').innerHTML  = s.windowOpen
      ? '<span class="pill on">öppet — mål tänder fyrverkeriet</span>'
      : '<span class="pill">stängt</span>';
  $('sSeen').textContent = s.lastSeenSec < 0 ? 'aldrig' : human(-s.lastSeenSec);
  $('sLast').textContent = 'IFB ' + s.lastUs + '–' + s.lastThem + ' ' + s.lastOpponent +
      '  (' + (s.lastWon ? 'vinst' : 'förlust') + ', ' +
      (s.lastDaysAgo === 0 ? 'idag' : s.lastDaysAgo === 1 ? 'igår' : s.lastDaysAgo + ' dagar sedan') + ')';
  $('sSpark').innerHTML = s.sparklesExpected
      ? '<span class="pill warn">ska lysa</span>' : '<span class="pill">av</span>';
  fill('lamp', s.lampHost);
  $('sPush').innerHTML = !s.pushOn ? '<span class="pill">av</span>'
      : s.pushOk ? '<span class="pill on">levererar — senast ' +
                   (s.pushAgeSec < 0 ? '—' : human(-s.pushAgeSec)) + '</span>'
                 : '<span class="pill warn">når inte lampan</span> ' + s.pushMsg;

  $('sSim').textContent = s.simRunning
      ? 'pågår — ' + s.sim.scored + '/' + s.sim.maxGoals + ' mål' : 'av';

  $('log').innerHTML = s.log.length
      ? s.log.map(l => '<b>' + l.t + '</b>  ' +
          (l.lamp ? '<span class="lamp">' + l.text + '</span>' : l.text)).join('\n')
      : '(inget än)';
}

$('base').textContent = location.origin;
const poll = () => fetch('/api/mock/state').then(r => r.json()).then(render).catch(()=>{});
poll(); setInterval(poll, 1000);
</script></body></html>
"""


# ─────────────────────────────────────────────────────────────────────────────
#  HTTP
# ─────────────────────────────────────────────────────────────────────────────
class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "BjorklovenMock/1.0"

    # Tyst standardlogg — vi har en egen i webbgränssnittet.
    def log_message(self, fmt, *args):
        pass

    # ── utskrift ───────────────────────────────────────────────────────────
    def _send(self, code: int, body: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, payload, code: int = 200) -> None:
        self._send(code, json.dumps(payload, ensure_ascii=False).encode(), "application/json")

    # ── GET ────────────────────────────────────────────────────────────────
    def do_GET(self) -> None:
        path = urlparse(self.path).path.rstrip("/") or "/"

        if path == "/":
            self._send(200, PAGE.encode(), "text/html; charset=utf-8")
            return

        if path == "/api/mock/state":
            with MOCK.lock:
                self._json(MOCK.public_state())
            return

        self._send(404, b"okand endpoint\n", "text/plain; charset=utf-8")

    # ── POST ───────────────────────────────────────────────────────────────
    def do_POST(self) -> None:
        if urlparse(self.path).path.rstrip("/") != "/api/mock/action":
            self._send(404, b"okand endpoint\n", "text/plain; charset=utf-8")
            return

        length = int(self.headers.get("Content-Length") or 0)
        try:
            payload = json.loads(self.rfile.read(length) or b"{}")
        except json.JSONDecodeError:
            self._json({"error": "trasig JSON"}, 400)
            return

        with MOCK.lock:
            MOCK.apply(payload)
            self._json(MOCK.public_state())



class Server(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def server_bind(self) -> None:
        # HTTPServer.server_bind gör ett omvänt DNS-uppslag på lyssnaradressen.
        # På ett labbnät utan fungerande resolver hänger det i minuter innan
        # servern ens börjar svara. Namnet används bara i felsidor — hoppa det.
        socketserver.TCPServer.server_bind(self)
        host, port = self.server_address[:2]
        self.server_name = str(host)
        self.server_port = port


# Hjärtslag mellan ändringar. Måste vara rejält under PUSH_LEASE_MS i
# include/config.h, annars hinner lampan falla tillbaka till SHL mellan varven.
PUSH_HEARTBEAT = 20.0


def push_loop() -> None:
    seen = -1
    last = 0.0
    while True:
        time.sleep(0.4)
        with MOCK.lock:
            active, seq = MOCK.push_on, MOCK.push_seq
        if not active:
            seen, last = seq, 0.0
            continue
        now = time.time()
        if seq != seen or now - last >= PUSH_HEARTBEAT:
            seen, last = seq, now
            MOCK.push_once()


def main() -> None:
    ap = argparse.ArgumentParser(description="Mock-server för Björklöven-lampan")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--host", default="0.0.0.0")
    args = ap.parse_args()

    srv = Server((args.host, args.port), Handler)
    threading.Thread(target=push_loop, daemon=True).start()

    url = f"http://{lan_ip()}:{args.port}"
    print("── Björklöven mockserver ─────────────────────────────")
    print(f"  Styrsida:   {url}/")
    print( "  Lampan:     fyll i dess adress på styrsidan och slå på push")
    print("  Avsluta:    Ctrl-C")
    print("──────────────────────────────────────────────────────")

    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstänger av")
        MOCK.stop_sim()
        srv.shutdown()


if __name__ == "__main__":
    main()
