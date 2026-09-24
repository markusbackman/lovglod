#!/usr/bin/env python3
"""
Följ SHL:s live-ström i terminalen. Visar samma SSE-ström som lampan läser,
game-broadcaster.s8y.se/live/game, ram för ram, så att fördröjning och
fältformat kan granskas medan matchen pågår.

    python3 tools/live.py                     # Björklövens match i dag (eller nästa)
    python3 tools/live.py wza53fczpy          # en viss match
    python3 tools/live.py https://www.bjorkloven.com/game-center/wza53fczpy/...
    python3 tools/live.py alla                # hela strömmen, alla sporter
    python3 tools/live.py alla --filter DIF   # bara ramar där DIF finns med
    python3 tools/live.py lista               # Björklövens matcher i säsongen med id
    python3 tools/live.py --logg match.jsonl  # spara varje ram med mottagningstid
    python3 tools/live.py --rå                # utan TUI, en rad per ram

Tangenter i TUI:t:
    ↑ ↓ j k      välj ram (följer inte längre senaste)
    PgUp PgDn    bläddra en sida
    End G f      följ senaste ramen igen
    Home g       första ramen
    /            filtrera på innehåll (tom rad tar bort filtret, Esc avbryter)
    r            anslut på nytt
    c            töm ramlistan
    q            avsluta

Matchens UUID är det första id:t i game-center-länken. Det andra id:t är inte
en match. Utan UUID skickar strömmen allt som pågår på plattformen, även
basket, och då är ställningen i rutan inte att lita på.

Filtret jämför mot ramens råa JSON utan hänsyn till versaler. Ord med
mellanslag emellan måste alla finnas med, | ger alternativ:
    DIF           ramar där DIF förekommer
    DIF goal      DIF och goal i samma ram
    DIF|IFB       antingen DIF eller IFB
Alla ramar tas emot och sparas i loggen ändå; filtret styr bara vad som visas.

Kolumnen "fw" visar vilken ställning lampans findScorePair() skulle läsa ut ur
ramen (src/shl.cpp), så att firmware kan jämföras mot vad strömmen faktiskt
bär.

Inga beroenden utanför Pythons standardbibliotek.
"""

from __future__ import annotations

import argparse
import curses
import http.client
import json
import locale
import re
import socket
import ssl
import sys
import threading
import time
from collections import Counter, deque
from dataclasses import dataclass, field
from datetime import datetime, timezone
from urllib.request import Request, urlopen

# Måste stämma med include/config.h.
LIVE_HOST = "game-broadcaster.s8y.se"
TEAM_CODE = "IFB"
TEAM_UUID = "4519-4519Rdei6"

USER_AGENT = "LovGlod/live.py"
SILENCE_S = 180            # samma gräns som ssePump() i src/shl.cpp
RETRY_S = 5
MAX_FRAMES = 5000
LIVE_WINDOW_S = 4 * 3600   # LIVE_WINDOW_POST_MS

# Grundserien 2026/27, samma som "Hela seriespelet" i docs/BYGGA.md.
SCHEDULE_URL = ("https://www.shl.se/api/sports-v2/game-schedule?seriesUuid=qQ9-bb0bzEWUk"
                "&seasonUuid=ndcf81nlb3&gameTypeUuid=qQ9-af37Ti40B")


# ─────────────────────────────────────────────────────────────────────────────
#  Välja match
# ─────────────────────────────────────────────────────────────────────────────
def http_json(url: str) -> object:
    req = Request(url, headers={"User-Agent": "Mozilla/5.0", "Accept": "application/json"})
    with urlopen(req, timeout=15) as r:
        return json.loads(r.read())


def parse_iso(s: str) -> datetime | None:
    try:
        return datetime.fromisoformat(s.replace("Z", "+00:00"))
    except (ValueError, AttributeError):
        return None


def find_game() -> tuple[str, str]:
    """Björklövens SHL-match som pågår eller står näst på tur.

    shl.se:s upcoming-games tappar matchen i samma stund som den börjar, och
    played-games tar upp den först när den är slut. Klubbsajtens gameheader
    har den hela tiden, så den frågas först.
    """
    now = datetime.now(timezone.utc)
    try:
        days = http_json("https://www.bjorkloven.com/api/gameday/gameheader")
        games = [g for gs in days.values() for g in gs if g.get("seriesCode") == "SHL"]
        games.sort(key=lambda g: g.get("startDateTime", ""))
        for g in games:
            start = parse_iso(g.get("startDateTime", ""))
            if start and (now - start).total_seconds() < LIVE_WINDOW_S:
                return g["uuid"], describe(g["homeTeam"]["code"], g["awayTeam"]["code"], start)
    except Exception as e:  # noqa: BLE001 — reserven nedan tar över
        print(f"gameheader: {e}", file=sys.stderr)

    data = http_json(f"https://www.shl.se/api/sports-v2/upcoming-games/{TEAM_UUID}?gamePlace=")
    for g in data.get("upcomingGames", []):
        start = parse_iso(g.get("startDateTime", ""))
        if start and (now - start).total_seconds() < LIVE_WINDOW_S:
            return g["uuid"], describe(g["homeTeamInfo"]["code"], g["awayTeamInfo"]["code"], start)
    raise SystemExit("Hittar ingen kommande match — ange UUID som argument.")


def describe(home: str, away: str, start: datetime) -> str:
    home = home.replace(" Herr", "")
    away = away.replace(" Herr", "")
    return f"{home}–{away} {start.astimezone():%-d/%-m %H:%M}"


def list_games() -> None:
    """Skriver ut säsongens Björklövenmatcher med gameUuid."""
    data = http_json(SCHEDULE_URL)
    games = [g for g in data.get("gameInfo", [])
             if TEAM_CODE in (g["homeTeamInfo"]["code"], g["awayTeamInfo"]["code"])]
    games.sort(key=lambda g: g.get("startDateTime", ""))
    # Tiderna i spelschemat är redan lokal tid i Stockholm, utan zon.
    now = datetime.now().strftime("%Y-%m-%d %H:%M")
    today = now[:10]
    marked = False
    print(f"{'gameUuid':<11} {'datum':<16} {'match':<11} {'ställn':<7} läge")
    for g in games:
        start = g.get("startDateTime", "")[:16]
        home, away = g["homeTeamInfo"], g["awayTeamInfo"]
        score = f"{home.get('score')}–{away.get('score')}" if g.get("state") != "pre-game" else ""
        note = ""
        if start[:10] == today:
            note, marked = "  ◀ i dag", True
        elif not marked and start > now:
            note, marked = "  ◀ nästa", True
        print(f"{g['uuid']:<11} {start:<16} {home['code'] + '–' + away['code']:<11} "
              f"{score:<7} {g.get('state', '')}{note}")


def game_from_arg(arg: str | None) -> tuple[str | None, str]:
    if arg is None:
        return find_game()
    if arg.lower() in ("alla", "all", "-"):
        return None, "hela strömmen"
    m = re.search(r"/game-center/([a-z0-9]+)", arg)
    return (m.group(1) if m else arg), ""


# ─────────────────────────────────────────────────────────────────────────────
#  Tolka ramar
# ─────────────────────────────────────────────────────────────────────────────
# Samma sökning som findScorePair() i src/shl.cpp: djupet först, platta fält
# före nästlade lagobjekt.
H_KEYS = ("homeScore", "homeGoals", "homeTeamScore", "homeResult")
A_KEYS = ("awayScore", "awayGoals", "awayTeamScore", "awayResult")
H_OBJ = ("homeTeam", "homeTeamInfo", "home")
A_OBJ = ("awayTeam", "awayTeamInfo", "away")
S_KEYS = ("score", "result", "goals")


def score_field(v: object) -> int | None:
    if isinstance(v, bool):
        return None
    if isinstance(v, int):
        return v
    if isinstance(v, str) and v.strip().isdigit():
        return int(v)
    return None


def firmware_score(v: object, depth: int = 0) -> tuple[int, int] | None:
    if depth > 8:
        return None
    if isinstance(v, dict):
        for hk, ak in zip(H_KEYS, A_KEYS):
            if hk in v and ak in v:
                h, a = score_field(v[hk]), score_field(v[ak])
                if h is not None and a is not None:
                    return h, a
        for ho, ao in zip(H_OBJ, A_OBJ):
            hv, av = v.get(ho), v.get(ao)
            if not isinstance(hv, dict) or not isinstance(av, dict):
                continue
            for sk in S_KEYS:
                h, a = score_field(hv.get(sk)), score_field(av.get(sk))
                if h is not None and a is not None:
                    return h, a
        for child in v.values():
            r = firmware_score(child, depth + 1)
            if r:
                return r
    elif isinstance(v, list):
        for child in v:
            r = firmware_score(child, depth + 1)
            if r:
                return r
    return None


@dataclass
class Frame:
    rx: float                 # mottagen, time.time()
    sse_id: str
    event: str
    raw: str
    data: object = None
    kind: str = "?"           # första nyckeln i JSON:en, t.ex. gameOverview
    summary: str = ""
    fw: tuple[int, int] | None = None
    game_uuid: str = ""


def interpret(f: Frame) -> None:
    try:
        f.data = json.loads(f.raw)
    except json.JSONDecodeError as e:
        f.kind = "JSON-FEL"
        f.summary = f"{e.msg}: {f.raw[:80]}"
        return

    f.fw = firmware_score(f.data)
    d = f.data
    if isinstance(d, dict) and len(d) == 1:
        f.kind, body = next(iter(d.items()))
    else:
        f.kind, body = type(d).__name__, d
    if not isinstance(body, dict):
        f.summary = compact(body)
        return
    f.game_uuid = str(body.get("gameUuid", ""))

    if f.kind == "gameOverview":
        ht, at = body.get("homeTeam") or {}, body.get("awayTeam") or {}
        t = body.get("time") or {}
        f.summary = (f"{ht.get('teamCode') or ht.get('teamName', '?')} "
                     f"{body.get('homeGoals', ht.get('score', '?'))}–"
                     f"{body.get('awayGoals', at.get('score', '?'))} "
                     f"{at.get('teamCode') or at.get('teamName', '?')}  "
                     f"{body.get('state', '')}  P{t.get('period', '?')} {t.get('periodTime', '')}")
    elif f.kind == "gameScorer":
        team = (body.get("teamDetails") or {}).get("teamCode", "?")
        f.summary = (f"{body.get('type', '?').upper()} {team} {body.get('scorer', '')} "
                     f"#{body.get('playerNumber', '')} {body.get('goalStatus', '')}  "
                     f"P{body.get('period', '?')} {body.get('time', '')}  → "
                     f"{body.get('homeTeamValue', '?')}–{body.get('awayTeamValue', '?')}")
    else:
        f.summary = compact(body)


class Filter:
    def __init__(self, text: str = ""):
        self.text = text.strip()
        self.alts = [alt.lower().split() for alt in self.text.split("|") if alt.strip()]

    def __bool__(self) -> bool:
        return bool(self.alts)

    def match(self, f: Frame) -> bool:
        if not self.alts:
            return True
        raw = f.raw.lower()
        return any(all(word in raw for word in alt) for alt in self.alts)


def compact(v: object) -> str:
    return json.dumps(v, ensure_ascii=False, separators=(",", ":"))


# ─────────────────────────────────────────────────────────────────────────────
#  SSE-klient
# ─────────────────────────────────────────────────────────────────────────────
@dataclass
class State:
    game_uuid: str | None
    label: str
    status: str = "startar"
    error: str = ""
    http: str = ""
    connected_at: float = 0.0
    last_rx: float = 0.0
    connects: int = 0
    frames: deque = field(default_factory=lambda: deque(maxlen=MAX_FRAMES))
    total_frames: int = 0
    comments: int = 0
    last_comment: str = ""
    bytes: int = 0
    last_id: str = ""
    kinds: Counter = field(default_factory=Counter)
    # Matchläget, bara från ramar som hör till vald match
    home: str = ""
    away: str = ""
    score: tuple[int, int] | None = None
    game_state: str = ""
    clock: str = ""
    goals: deque = field(default_factory=lambda: deque(maxlen=50))
    version: int = 0


class Stream(threading.Thread):
    def __init__(self, st: State, lock: threading.Lock, log_path: str | None):
        super().__init__(daemon=True)
        self.st, self.lock = st, lock
        self.conn: http.client.HTTPSConnection | None = None
        self.stop = threading.Event()
        self.manual = threading.Event()   # r i TUI:t: ingen felrad, ingen väntan
        self.log = open(log_path, "a", encoding="utf-8") if log_path else None
        self.on_frame = None   # för --rå

    def path(self) -> str:
        return "/live/game" + (f"?gameUuid={self.st.game_uuid}" if self.st.game_uuid else "")

    def set(self, **kw) -> None:
        with self.lock:
            for k, v in kw.items():
                setattr(self.st, k, v)
            self.st.version += 1

    def reconnect(self) -> None:
        self.manual.set()
        conn = self.conn
        if conn and conn.sock:
            try:
                conn.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass

    def run(self) -> None:
        while not self.stop.is_set():
            try:
                self.session()
            except Exception as e:  # noqa: BLE001 — allt ska leda till ny anslutning
                if self.stop.is_set():
                    return
                if not self.manual.is_set():
                    self.set(status="fel", error=f"{type(e).__name__}: {e}")
            finally:
                if self.conn:
                    self.conn.close()
            for _ in range(RETRY_S * 10):
                if self.stop.is_set():
                    return
                if self.manual.is_set():
                    break
                time.sleep(0.1)
            self.manual.clear()

    def session(self) -> None:
        self.set(status="ansluter", http="", error="")
        ctx = ssl.create_default_context()
        self.conn = http.client.HTTPSConnection(LIVE_HOST, 443, timeout=SILENCE_S, context=ctx)
        self.conn.request("GET", self.path(), headers={
            "Accept": "text/event-stream",
            "Cache-Control": "no-cache",
            "User-Agent": USER_AGENT,
        })
        resp = self.conn.getresponse()
        now = time.time()
        with self.lock:
            self.st.connects += 1
            self.st.http = f"{resp.status} {resp.reason}"
            self.st.connected_at = now
            self.st.last_rx = now
            self.st.status = "ansluten" if resp.status == 200 else "fel"
            self.st.error = "" if resp.status == 200 else resp.read(300).decode(errors="replace")
            self.st.version += 1
        if resp.status != 200:
            return

        data: list[str] = []
        event = ""
        sse_id = ""
        while not self.stop.is_set():
            try:
                raw = resp.readline()
            except socket.timeout:
                self.set(status="fel", error=f"tyst i {SILENCE_S} s, ansluter på nytt")
                return
            if not raw:
                self.set(status="fel", error="servern stängde strömmen")
                return
            with self.lock:
                self.st.bytes += len(raw)
                self.st.last_rx = time.time()
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")

            if line == "":
                if data:
                    self.dispatch(sse_id, event, "\n".join(data))
                data, event = [], ""
            elif line.startswith(":"):
                with self.lock:
                    self.st.comments += 1
                    self.st.last_comment = line[1:].strip()[:60]
                    self.st.version += 1
            else:
                name, _, value = line.partition(":")
                value = value[1:] if value.startswith(" ") else value
                if name == "data":
                    data.append(value)
                elif name == "event":
                    event = value
                elif name == "id":
                    sse_id = value

    def dispatch(self, sse_id: str, event: str, raw: str) -> None:
        f = Frame(rx=time.time(), sse_id=sse_id, event=event, raw=raw)
        interpret(f)
        if self.log:
            self.log.write(compact({
                "rx": datetime.fromtimestamp(f.rx).astimezone().isoformat(timespec="milliseconds"),
                "id": sse_id, "event": event or None, "data": f.data if f.data is not None else raw,
            }) + "\n")
            self.log.flush()

        st = self.st
        with self.lock:
            st.frames.append(f)
            st.total_frames += 1
            st.last_id = sse_id or st.last_id
            st.kinds[f.kind] += 1
            mine = st.game_uuid is None or f.game_uuid in ("", st.game_uuid)
            body = f.data.get(f.kind) if isinstance(f.data, dict) else None
            if mine and st.game_uuid and isinstance(body, dict):
                self.track(f, body)
            st.version += 1
        if self.on_frame:
            self.on_frame(f)

    def track(self, f: Frame, body: dict) -> None:
        st = self.st
        stamp = datetime.fromtimestamp(f.rx).strftime("%H:%M:%S")
        if f.kind == "gameOverview":
            ht, at = body.get("homeTeam") or {}, body.get("awayTeam") or {}
            st.home = ht.get("teamCode") or ht.get("teamName") or st.home
            st.away = at.get("teamCode") or at.get("teamName") or st.away
            st.game_state = str(body.get("state", st.game_state))
            t = body.get("time") or {}
            st.clock = f"P{t.get('period', '?')} {t.get('periodTime', '')}"
            h = score_field(body.get("homeGoals", ht.get("score")))
            a = score_field(body.get("awayGoals", at.get("score")))
            if h is not None and a is not None:
                if st.score is not None and (h, a) != st.score:
                    st.goals.append((stamp, "ställning", f"{st.score[0]}–{st.score[1]} → {h}–{a}"))
                st.score = (h, a)
        elif f.kind == "gameScorer" and body.get("type") == "goal":
            team = (body.get("teamDetails") or {}).get("teamCode", "?")
            st.goals.append((stamp, "mål", f"{team} {body.get('scorer', '')} "
                             f"P{body.get('period', '?')} {body.get('time', '')} → "
                             f"{body.get('homeTeamValue', '?')}–{body.get('awayTeamValue', '?')}"))


# ─────────────────────────────────────────────────────────────────────────────
#  Rått läge
# ─────────────────────────────────────────────────────────────────────────────
def run_raw(st: State, lock: threading.Lock, stream: Stream, flt: Filter) -> None:
    def show(f: Frame) -> None:
        if not flt.match(f):
            return
        fw = f"{f.fw[0]}–{f.fw[1]}" if f.fw else "–"
        print(f"{datetime.fromtimestamp(f.rx):%H:%M:%S.%f}"[:-3] +
              f"  #{f.sse_id or '-':>6}  {f.kind:<14} fw={fw:<6} {f.summary}", flush=True)

    stream.on_frame = show
    last = ""
    try:
        while True:
            with lock:
                line = f"[{st.status}] {st.http} {st.error}".strip()
            if line != last:
                print(f"{datetime.now():%H:%M:%S}  {line}", file=sys.stderr, flush=True)
                last = line
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass


# ─────────────────────────────────────────────────────────────────────────────
#  TUI
# ─────────────────────────────────────────────────────────────────────────────
class Tui:
    def __init__(self, scr, st: State, lock: threading.Lock, stream: Stream,
                 log_path: str | None, flt: Filter):
        self.scr, self.st, self.lock, self.stream = scr, st, lock, stream
        self.log_path = log_path
        self.filter = flt
        self.n_visible = 0
        self.follow = True
        self.sel = 0
        curses.curs_set(0)
        curses.set_escdelay(25)       # Esc i filterrutan ska inte ta en sekund
        scr.nodelay(True)
        scr.timeout(250)
        curses.start_color()
        curses.use_default_colors()
        for i, c in enumerate((curses.COLOR_GREEN, curses.COLOR_YELLOW, curses.COLOR_RED,
                               curses.COLOR_CYAN, curses.COLOR_MAGENTA), start=1):
            curses.init_pair(i, c, -1)
        self.GREEN, self.YELLOW, self.RED, self.CYAN, self.MAG = (curses.color_pair(i) for i in range(1, 6))

    def put(self, y: int, x: int, text: str, attr: int = 0) -> None:
        h, w = self.scr.getmaxyx()
        if y < 0 or y >= h or x >= w:
            return
        try:
            self.scr.addnstr(y, x, text, max(0, w - x - (1 if y == h - 1 else 0)), attr)
        except curses.error:
            pass

    def loop(self) -> None:
        while True:
            self.draw()
            k = self.scr.getch()
            if k == -1:
                continue
            n = self.n_visible
            page = max(1, self.list_h - 1)
            if k in (ord("q"), ord("Q"), 27):
                return
            elif k in (curses.KEY_UP, ord("k")):
                self.move(-1, n)
            elif k in (curses.KEY_DOWN, ord("j")):
                self.move(1, n)
            elif k == curses.KEY_PPAGE:
                self.move(-page, n)
            elif k == curses.KEY_NPAGE:
                self.move(page, n)
            elif k in (curses.KEY_HOME, ord("g")):
                self.follow, self.sel = False, 0
            elif k in (curses.KEY_END, ord("G"), ord("f")):
                self.follow = True
            elif k == ord("/"):
                text = self.prompt("filter: ", self.filter.text)
                if text is not None:
                    self.filter = Filter(text)
                    self.sel, self.follow = 0, True
            elif k == ord("r"):
                self.stream.reconnect()
            elif k == ord("c"):
                with self.lock:
                    self.st.frames.clear()
                self.sel, self.follow = 0, True
            elif k == curses.KEY_RESIZE:
                self.scr.erase()

    def prompt(self, label: str, initial: str) -> str | None:
        """Enradsinmatning i sidfoten. None om den avbryts med Esc."""
        buf = list(initial)
        self.scr.timeout(-1)
        curses.curs_set(1)
        try:
            while True:
                h, w = self.scr.getmaxyx()
                text = label + "".join(buf)
                self.scr.move(h - 1, 0)
                self.scr.clrtoeol()
                self.put(h - 1, 0, text + "   (Enter använd, Esc avbryt, tom rad = inget filter)")
                self.scr.move(h - 1, min(len(text), w - 2))
                self.scr.refresh()
                ch = self.scr.get_wch()
                if ch in ("\n", "\r", curses.KEY_ENTER):
                    return "".join(buf)
                if ch == "\x1b":
                    return None
                if ch in (curses.KEY_BACKSPACE, "\x7f", "\b"):
                    if buf:
                        buf.pop()
                elif ch == "\x15":           # Ctrl-U
                    buf.clear()
                elif isinstance(ch, str) and ch.isprintable():
                    buf.append(ch)
        finally:
            self.scr.timeout(250)
            curses.curs_set(0)

    def move(self, d: int, n: int) -> None:
        if n == 0:
            return
        cur = n - 1 if self.follow else self.sel
        self.sel = max(0, min(n - 1, cur + d))
        self.follow = self.sel == n - 1 and d > 0

    list_h = 10

    def draw(self) -> None:
        with self.lock:
            st = self.st
            frames = list(st.frames)
            snap = dict(status=st.status, error=st.error, http=st.http, connected_at=st.connected_at,
                        last_rx=st.last_rx, connects=st.connects, total=st.total_frames,
                        comments=st.comments, last_comment=st.last_comment, bytes=st.bytes,
                        last_id=st.last_id, kinds=st.kinds.most_common(), home=st.home,
                        away=st.away, score=st.score, game_state=st.game_state, clock=st.clock,
                        goals=list(st.goals))
        s = snap
        scr = self.scr
        scr.erase()
        h, w = scr.getmaxyx()
        now = time.time()

        # Rubrik
        target = f"{LIVE_HOST}{self.stream.path()}"
        self.put(0, 0, " LövGlöd live ", curses.A_REVERSE | curses.A_BOLD)
        self.put(0, 15, f"{target}  {self.st.label}")

        # Anslutning
        color = {"ansluten": self.GREEN, "ansluter": self.YELLOW}.get(s["status"], self.RED)
        silent = now - s["last_rx"] if s["last_rx"] else 0
        up = now - s["connected_at"] if s["connected_at"] and s["status"] == "ansluten" else 0
        self.put(2, 0, "● ", color)
        self.put(2, 2, s["status"].upper(), color | curses.A_BOLD)
        self.put(2, 14, f"HTTP {s['http'] or '–'}   uppe {fmt_dur(up)}   "
                        f"anslutningar {s['connects']}   senaste id {s['last_id'] or '–'}")
        silent_attr = self.RED if silent > 60 else self.YELLOW if silent > 20 else 0
        self.put(3, 2, f"tyst {silent:5.0f} s", silent_attr | curses.A_BOLD)
        self.put(3, 16, f"ramar {s['total']}   hjärtslag {s['comments']}"
                        f"{' (' + s['last_comment'] + ')' if s['last_comment'] else ''}   "
                        f"{s['bytes'] / 1024:.1f} kB")
        kinds = "  ".join(f"{k} {n}" for k, n in s["kinds"]) or "inga ramar än"
        self.put(4, 2, kinds, curses.A_DIM)
        if s["error"]:
            self.put(5, 2, s["error"], self.RED)

        # Matchen
        y = 6
        if self.st.game_uuid:
            score = f"{s['score'][0]} – {s['score'][1]}" if s["score"] else "– – –"
            self.put(y, 0, " MATCH ", curses.A_REVERSE)
            self.put(y, 8, f"{s['home'] or '?'}  {score}  {s['away'] or '?'}",
                     curses.A_BOLD | self.CYAN)
            self.put(y, 34, f"{s['game_state'] or 'ingen gameOverview än'}  {s['clock']}")
            goals = s["goals"][-3:]
            for i, (t, kind, text) in enumerate(goals):
                attr = self.MAG | curses.A_BOLD if kind == "mål" else self.YELLOW
                self.put(y + 1 + i, 2, f"{t}  {kind:<9} {text}", attr)
            y += 1 + max(1, len(goals))
            if not goals:
                self.put(y - 1, 2, "inga mål eller ställningsändringar än", curses.A_DIM)
        else:
            self.put(y, 0, " HELA STRÖMMEN ", curses.A_REVERSE)
            self.put(y, 16, "ingen gameUuid — ramar från alla matcher och sporter", curses.A_DIM)
            y += 1
        y += 1

        # Filter
        total_kept = len(frames)
        if self.filter:
            frames = [f for f in frames if self.filter.match(f)]
            self.put(y, 0, " FILTER ", curses.A_REVERSE | self.YELLOW)
            self.put(y, 9, f"{self.filter.text}", curses.A_BOLD | self.YELLOW)
            self.put(y, 11 + len(self.filter.text),
                     f"{len(frames)} av {total_kept} ramar   (/ ändra, / + Enter på tom rad tar bort)",
                     curses.A_DIM)
            y += 2
        self.n_visible = len(frames)

        # Ramlista + detalj
        footer_y = h - 1
        body_h = footer_y - y
        side = w >= 140
        list_w = w // 2 if side else w
        self.list_h = body_h if side else max(3, body_h // 2)
        n = len(frames)
        if self.follow:
            self.sel = max(0, n - 1)
        self.sel = min(self.sel, max(0, n - 1))

        self.put(y, 0, f"{'mottagen':<12} {'id':>6} {'typ':<14} {'fw':<6} innehåll",
                 curses.A_UNDERLINE)
        rows = self.list_h - 1
        top = max(0, min(self.sel - rows + 1, n - rows)) if n > rows else 0
        top = min(top, self.sel)
        for i, f in enumerate(frames[top:top + rows]):
            idx = top + i
            fw = f"{f.fw[0]}–{f.fw[1]}" if f.fw else "–"
            line = (f"{datetime.fromtimestamp(f.rx):%H:%M:%S.%f}"[:-3] +
                    f" {f.sse_id[-6:] or '-':>6} {f.kind[:14]:<14} {fw:<6} {f.summary}")
            attr = curses.A_REVERSE if idx == self.sel else 0
            if f.kind == "gameScorer":
                attr |= self.MAG
            elif f.kind == "JSON-FEL":
                attr |= self.RED
            elif self.st.game_uuid and f.game_uuid and f.game_uuid != self.st.game_uuid:
                attr |= curses.A_DIM
            self.put(y + 1 + i, 0, line.ljust(list_w - 1)[:list_w - 1], attr)

        if side:
            dx, dy, dh, dw = list_w + 1, y, body_h, w - list_w - 1
        else:
            dx, dy, dh, dw = 0, y + self.list_h, body_h - self.list_h, w
        if frames and dh > 1:
            f = frames[self.sel]
            self.put(dy, dx, f" ram {f.sse_id or '-'} · {f.event or 'message'} · "
                             f"{len(f.raw)} byte ".ljust(dw - 1, "─"), curses.A_DIM)
            pretty = json.dumps(f.data, ensure_ascii=False, indent=2) if f.data is not None else f.raw
            for i, line in enumerate(pretty.splitlines()[:dh - 1]):
                self.put(dy + 1 + i, dx, line[:dw - 1])
        elif dh > 1:
            wait = "ingen ram matchar filtret än" if total_kept else "väntar på första ramen"
            self.put(dy + 1, dx, wait + " …", curses.A_DIM)

        keys = " q avsluta  ↑↓ välj  End följ  / filter  r anslut igen  c töm "
        mode = "FÖLJER" if self.follow else f"ram {self.sel + 1}/{n}"
        log = f"  logg → {self.log_path}" if self.log_path else ""
        self.put(footer_y, 0, (keys + f"│ {mode}{log}").ljust(w - 1), curses.A_REVERSE)
        scr.refresh()


def fmt_dur(s: float) -> str:
    s = int(s)
    return f"{s // 3600:d}:{s % 3600 // 60:02d}:{s % 60:02d}"


# ─────────────────────────────────────────────────────────────────────────────
def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("match", nargs="?", help="gameUuid, game-center-länk, 'alla' eller 'lista'")
    ap.add_argument("--logg", metavar="FIL", help="spara varje ram som JSON-rad med mottagningstid")
    ap.add_argument("--filter", metavar="TEXT", default="",
                    help="visa bara ramar som innehåller TEXT (ord = och, | = eller)")
    ap.add_argument("--rå", dest="raw", action="store_true", help="ingen TUI, en rad per ram")
    args = ap.parse_args()
    if args.match and args.match.lower() in ("lista", "list"):
        list_games()
        return

    game_uuid, label = game_from_arg(args.match)
    st = State(game_uuid=game_uuid, label=label)
    lock = threading.Lock()
    stream = Stream(st, lock, args.logg)

    if args.raw:
        print(f"{LIVE_HOST}{stream.path()}  {label}", file=sys.stderr)
        stream.start()
        run_raw(st, lock, stream, Filter(args.filter))
    else:
        locale.setlocale(locale.LC_ALL, "")
        stream.start()
        try:
            curses.wrapper(lambda scr: Tui(scr, st, lock, stream, args.logg, Filter(args.filter)).loop())
        except KeyboardInterrupt:
            pass
    stream.stop.set()
    stream.reconnect()


if __name__ == "__main__":
    main()
