#!/usr/bin/env python3
"""
Spela in en match. Sparar allt SHL och klubbsajten skickar under matchen, och
lampan om den är igång, så att matchen kan granskas och spelas upp i efterhand.
Det är samma inspelning som ligger i mock/recordings/.

    python3 tools/spela_in.py                      # matcher som pågår eller står på tur
    python3 tools/spela_in.py n9m6y5xe6d           # spela in, start 45 min före nedsläpp
    python3 tools/spela_in.py n9m6y5xe6d --start 14:30
    python3 tools/spela_in.py n9m6y5xe6d --nu      # börja direkt
    python3 tools/spela_in.py n9m6y5xe6d --lampa 192.168.1.58 --efter 60

Inspelningen stannar när game-overview har sagt GameEnded och --efter minuter
har gått, eller vid --senast. Allt gzippas när den stannar. Ctrl-C stannar
också, och gzippar det som hunnit sparas.

Filerna i mappen:
    sse1–3.jsonl   tre samtidiga SSE-anslutningar via tools/live.py, en ram per rad
    sse1–3.log     samma ramar i läsbar form
    sse1–3.status  anslutningar och fel per ström
    poll.log       game-overview var 15:e sekund, en rad per svar
    http.jsonl     hela svaret från game-overview, play-by-play, gameheader,
                   upcoming-games, played-games och today-games när det ändrats
    lamp_http.log  lampans status- och felsökningssida var 30:e sekund (--lampa)
    serial.log     lampans seriella utskrift om en USB-seriell port finns

Datorn hålls vaken med caffeinate under hela inspelningen, även medan den
väntar på starttiden. Stäng inte locket.

Rådatat kan städas till det lampan faktiskt läser med tools/forbehandla.py.

Inga beroenden utanför Pythons standardbibliotek.
"""

from __future__ import annotations

import argparse
import glob
import gzip
import hashlib
import html
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from datetime import datetime, timedelta
from pathlib import Path
from urllib.request import Request, urlopen

sys.path.insert(0, str(Path(__file__).resolve().parent))
import live  # noqa: E402 — samma lag, schema och SSE-klient som live.py

REPO = Path(__file__).resolve().parent.parent
LIVE_PY = REPO / "tools" / "live.py"
RECORDINGS = REPO / "mock" / "recordings"
CLUB = "https://www.bjorkloven.com"
SHL = "https://www.shl.se"

SSE_STREAMS = 3


def http_get(url: str, timeout: float = 15) -> tuple[int, bytes, int]:
    t0 = time.time()
    req = Request(url, headers={"User-Agent": "Mozilla/5.0", "Accept": "application/json"})
    with urlopen(req, timeout=timeout) as r:
        return r.status, r.read(), int((time.time() - t0) * 1000)


def stamp(ms: bool = True) -> str:
    return datetime.now().strftime("%H:%M:%S.%f")[:-3] if ms else datetime.now().strftime("%H:%M:%S")


# ─────────────────────────────────────────────────────────────────────────────
#  Välja match
# ─────────────────────────────────────────────────────────────────────────────
def schedule() -> list[dict]:
    """Björklövens matcher i säsongen, i tidsordning. Tiderna är lokal tid."""
    data = live.http_json(live.SCHEDULE_URL)
    games = [g for g in data.get("gameInfo", [])
             if live.TEAM_CODE in (g["homeTeamInfo"]["code"], g["awayTeamInfo"]["code"])]
    games.sort(key=lambda g: g.get("startDateTime", ""))
    return games


def game_start(g: dict) -> datetime:
    return datetime.strptime(g["startDateTime"][:16], "%Y-%m-%d %H:%M")


def overview(uuid: str) -> dict | None:
    """game-overview för en match, eller None om den är tom (före nedsläpp)."""
    try:
        _, body, _ = http_get(f"{CLUB}/api/gameday/game-overview/{uuid}", timeout=10)
        return json.loads(body) if body else None
    except Exception:  # noqa: BLE001 — listan klarar sig utan
        return None


def list_games(count: int) -> None:
    """Matcher som pågår eller står på tur. Spelschemat säger bara pre-game
    och post-game, så en match räknas som pågående när den har startat,
    inte är post-game och är yngre än live-fönstret i firmwaren."""
    now = datetime.now()
    window = timedelta(seconds=live.LIVE_WINDOW_S)
    rows = []
    for g in schedule():
        start = game_start(g)
        if g.get("state") == "post-game" or start + window < now:
            continue
        rows.append(g)
    if not rows:
        print("Inga kommande matcher i spelschemat.")
        return

    print(f"{'gameUuid':<11} {'datum':<16} {'match':<11} läge")
    for g in rows[:count]:
        start = game_start(g)
        home, away = g["homeTeamInfo"]["code"], g["awayTeamInfo"]["code"]
        note = g.get("state", "")
        if start <= now:
            ov = overview(g["uuid"]) or {}
            t = ov.get("time") or {}
            note = (f"PÅGÅR  {ov.get('homeGoals', '?')}–{ov.get('awayGoals', '?')}  "
                    f"P{t.get('period', '?')} {t.get('periodTime', '')}  {ov.get('state', '')}")
        elif start.date() == now.date():
            note += "  ◀ i dag"
        print(f"{g['uuid']:<11} {start:%Y-%m-%d %H:%M} {home + '–' + away:<11} {note}")
    print("\nSpela in:  python3 tools/spela_in.py <gameUuid> [--start HH:MM | --nu]")


def find_scheduled(uuid: str) -> dict | None:
    return next((g for g in schedule() if g["uuid"] == uuid), None)


def parse_clock(text: str, ref: datetime) -> datetime:
    """HH:MM samma dag som ref, eller en hel tidpunkt 'YYYY-MM-DD HH:MM'."""
    if re.fullmatch(r"\d{1,2}:\d{2}", text):
        h, m = map(int, text.split(":"))
        return ref.replace(hour=h, minute=m, second=0, microsecond=0)
    return datetime.strptime(text, "%Y-%m-%d %H:%M")


# ─────────────────────────────────────────────────────────────────────────────
#  Inspelningen
# ─────────────────────────────────────────────────────────────────────────────
class Recorder:
    def __init__(self, uuid: str, out: Path, lamp: str | None, serial: bool) -> None:
        self.uuid = uuid
        self.out = out
        self.lamp = lamp
        self.serial = serial
        self.stop = threading.Event()
        self.ended_at: float | None = None      # när game-overview sa GameEnded
        self.procs: list[subprocess.Popen] = []
        self.threads: list[threading.Thread] = []

    # ── SSE ────────────────────────────────────────────────────────────────
    def start_sse(self) -> None:
        for i in range(1, SSE_STREAMS + 1):
            self.procs.append(subprocess.Popen(
                [sys.executable, "-u", str(LIVE_PY), self.uuid, "--rå",
                 "--logg", str(self.out / f"sse{i}.jsonl")],
                stdout=open(self.out / f"sse{i}.log", "a"),
                stderr=open(self.out / f"sse{i}.status", "a"),
                cwd=REPO))

    # ── HTTP-källorna ──────────────────────────────────────────────────────
    def poll_loop(self) -> None:
        team = live.TEAM_UUID
        sources = {  # namn: (url, intervall i sekunder)
            "game-overview": (f"{CLUB}/api/gameday/game-overview/{self.uuid}", 15),
            "play-by-play": (f"{CLUB}/api/gameday/play-by-play/{self.uuid}", 30),
            "gameheader": (f"{CLUB}/api/gameday/gameheader", 60),
            "upcoming-games": (f"{SHL}/api/sports-v2/upcoming-games/{team}?gamePlace=", 60),
            "played-games": (f"{SHL}/api/sports-v2/played-games/{team}", 60),
            "today-games": (f"{SHL}/api/sports-v2/today-games", 60),
        }
        last_sha: dict[str, str] = {}
        next_at = dict.fromkeys(sources, 0.0)
        with open(self.out / "poll.log", "a", buffering=1) as plog, \
             open(self.out / "http.jsonl", "a", buffering=1) as hlog:
            while not self.stop.is_set():
                for name, (url, every) in sources.items():
                    if time.time() < next_at[name]:
                        continue
                    next_at[name] = time.time() + every
                    t = stamp()
                    try:
                        code, body, ms = http_get(url)
                    except Exception as e:  # noqa: BLE001 — loggas och prövas igen
                        err = f"{type(e).__name__}: {e}"
                        if name == "game-overview":
                            plog.write(f"{t} FEL {err}\n")
                        hlog.write(json.dumps({"rx": t, "src": name, "error": err}) + "\n")
                        continue
                    sha = hashlib.sha1(body).hexdigest()[:8]
                    changed = last_sha.get(name) != sha
                    last_sha[name] = sha
                    if name == "game-overview":
                        plog.write(f"{t} {code} {len(body):6d} b {ms:5d} ms {sha} "
                                   f"{self.describe_overview(body)}{'  ← NYTT' if changed else ''}\n")
                    if changed:
                        try:
                            data = json.loads(body) if body else None
                        except ValueError:
                            data = body.decode("utf-8", "replace")
                        hlog.write(json.dumps({"rx": t, "src": name, "status": code, "ms": ms,
                                               "len": len(body), "sha": sha, "data": data},
                                              ensure_ascii=False) + "\n")
                self.stop.wait(1)

    def describe_overview(self, body: bytes) -> str:
        try:
            d = json.loads(body)
        except ValueError:
            return ""
        if not isinstance(d, dict):
            return ""
        t = d.get("time") or {}
        if d.get("state") == "GameEnded" and self.ended_at is None:
            self.ended_at = time.time()
            print(f"{stamp(False)} slutsignal: {d.get('homeGoals')}–{d.get('awayGoals')}", flush=True)
        return (f"{d.get('homeGoals')}–{d.get('awayGoals')} {d.get('state')} "
                f"P{t.get('period')} {t.get('periodTime')}")

    # ── Lampan ─────────────────────────────────────────────────────────────
    def lamp_loop(self) -> None:
        def text(body: str) -> str:
            body = re.sub(r"(?s)<(style|script)[^>]*>.*?</\1>", "", body)
            parts = (html.unescape(p).strip() for p in re.split(r"<[^>]+>", body))
            return "|".join(p for p in parts if p)

        with open(self.out / "lamp_http.log", "a", buffering=1) as log:
            while not self.stop.is_set():
                for name, path in (("status", "/"), ("debug", "/debug")):
                    t = stamp(False)
                    t0 = time.time()
                    try:
                        with urlopen(f"http://{self.lamp}{path}", timeout=10) as r:
                            body = r.read().decode("utf-8", "replace")
                        log.write(f"{t} [{name} {int((time.time() - t0) * 1000)} ms] {text(body)}\n")
                    except Exception as e:  # noqa: BLE001
                        log.write(f"{t} [{name}] FEL {type(e).__name__}: {e}\n")
                self.stop.wait(30)

    # ── Seriella porten ────────────────────────────────────────────────────
    def serial_loop(self) -> None:
        """Väntar på en USB-seriell port och öppnar den igen om kabeln dras
        ur. Läser med stty och en vanlig fil, så pyserial behövs inte."""
        with open(self.out / "serial.log", "a", buffering=1) as log:
            while not self.stop.is_set():
                ports = sorted(glob.glob("/dev/cu.usbserial-*") + glob.glob("/dev/cu.SLAB_USBtoUART*")
                               + glob.glob("/dev/cu.wchusbserial*") + glob.glob("/dev/ttyUSB*"))
                if not ports:
                    self.stop.wait(5)
                    continue
                port = ports[0]
                flag = "-f" if sys.platform == "darwin" else "-F"
                try:
                    subprocess.run(["stty", flag, port, "115200", "raw", "-echo", "clocal", "-hupcl"],
                                   check=True)
                    fd = os.open(port, os.O_RDONLY | os.O_NONBLOCK)
                    log.write(f"{stamp()} === öppnade {port}\n")
                    buf = b""
                    try:
                        while not self.stop.is_set():
                            try:
                                chunk = os.read(fd, 4096)
                            except BlockingIOError:
                                chunk = None
                            if chunk == b"":
                                raise OSError("porten stängdes")
                            if not chunk:
                                time.sleep(0.02)
                                continue
                            buf += chunk
                            while b"\n" in buf:
                                line, buf = buf.split(b"\n", 1)
                                log.write(f"{stamp()} {line.rstrip(b'\r').decode('utf-8', 'replace')}\n")
                    finally:
                        os.close(fd)
                except Exception as e:  # noqa: BLE001
                    log.write(f"{stamp()} === {port} stängd: {type(e).__name__}: {e}\n")
                    self.stop.wait(5)

    # ── Start och stopp ────────────────────────────────────────────────────
    def run(self, after_s: float, hard_stop: datetime) -> None:
        self.out.mkdir(parents=True, exist_ok=True)
        print(f"{stamp(False)} spelar in i {self.out}", flush=True)
        self.start_sse()
        loops = [self.poll_loop]
        if self.lamp:
            loops.append(self.lamp_loop)
        if self.serial:
            loops.append(self.serial_loop)
        for fn in loops:
            th = threading.Thread(target=fn, daemon=True)
            th.start()
            self.threads.append(th)

        try:
            while True:
                if datetime.now() >= hard_stop:
                    print(f"{stamp(False)} senaste stopptid nådd", flush=True)
                    break
                if self.ended_at and time.time() - self.ended_at >= after_s:
                    print(f"{stamp(False)} {after_s / 60:.0f} min efter slutsignalen", flush=True)
                    break
                dead = [p for p in self.procs if p.poll() is not None]
                if dead:
                    print(f"{stamp(False)} VARNING: {len(dead)} SSE-anslutning(ar) har stannat",
                          flush=True)
                time.sleep(10)
        except KeyboardInterrupt:
            print(f"\n{stamp(False)} avbruten", flush=True)
        finally:
            self.shutdown()

    def shutdown(self) -> None:
        self.stop.set()
        for p in self.procs:
            if p.poll() is None:
                p.terminate()
        for p in self.procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
        for th in self.threads:
            th.join(15)
        self.compress()
        print(f"{stamp(False)} klar: {self.out}", flush=True)

    def compress(self) -> None:
        """gzippar loggarna som i de tidigare inspelningarna. Tomma filer
        (ingen lampa, ingen kabel) tas bort."""
        for f in sorted(self.out.iterdir()):
            if f.suffix not in (".jsonl", ".log"):
                continue
            if f.stat().st_size == 0:
                f.unlink()
                continue
            with open(f, "rb") as src, gzip.open(f"{f}.gz", "wb", compresslevel=9) as dst:
                shutil.copyfileobj(src, dst)
            f.unlink()


def keep_awake() -> None:
    """Håller datorn vaken så länge den här processen lever (macOS)."""
    if shutil.which("caffeinate"):
        subprocess.Popen(["caffeinate", "-dimsu", "-w", str(os.getpid())])


def on_sigterm(*_: object) -> None:
    raise KeyboardInterrupt  # samma städning som Ctrl-C


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("match", nargs="?",
                    help="gameUuid eller game-center-länk; utan argument listas matcherna")
    ap.add_argument("--antal", type=int, default=8, help="antal matcher i listan (8)")
    when = ap.add_mutually_exclusive_group()
    when.add_argument("--start", metavar="TID",
                      help="HH:MM på matchdagen eller 'ÅÅÅÅ-MM-DD HH:MM' (45 min före nedsläpp)")
    when.add_argument("--nu", action="store_true", help="börja spela in direkt")
    ap.add_argument("--före", dest="before", type=int, default=45, metavar="MIN",
                    help="minuter före nedsläpp att börja, om --start saknas (45)")
    ap.add_argument("--efter", dest="after", type=int, default=40, metavar="MIN",
                    help="minuter att fortsätta efter slutsignalen (40)")
    ap.add_argument("--senast", metavar="TID",
                    help="stanna senast HH:MM eller 'ÅÅÅÅ-MM-DD HH:MM' (5 h efter nedsläpp)")
    ap.add_argument("--lampa", metavar="IP", help="lampans adress, för status- och felsökningssidan")
    ap.add_argument("--ingen-seriell", dest="serial", action="store_false",
                    help="leta inte efter lampan på USB")
    ap.add_argument("--ut", metavar="MAPP",
                    help="mapp att spara i (mock/recordings/ÅÅÅÅ-MM-DD-HEMMA-BORTA)")
    args = ap.parse_args()

    if not args.match:
        list_games(args.antal)
        return

    uuid, _ = live.game_from_arg(args.match)
    g = find_scheduled(uuid)
    if g:
        puck = game_start(g)
        home, away = g["homeTeamInfo"]["code"], g["awayTeamInfo"]["code"]
    elif args.nu or args.start:
        puck, home, away = datetime.now(), "HEMMA", "BORTA"
        print(f"{uuid} finns inte i Björklövens spelschema — spelar in ändå.")
    else:
        raise SystemExit(f"{uuid} finns inte i Björklövens spelschema. Ange --start eller --nu.")

    start = datetime.now() if args.nu else (
        parse_clock(args.start, puck) if args.start else puck - timedelta(minutes=args.before))
    hard_stop = parse_clock(args.senast, puck) if args.senast else puck + timedelta(hours=5)
    if hard_stop <= max(start, datetime.now()):
        raise SystemExit(f"Stopptiden {hard_stop:%Y-%m-%d %H:%M} har redan passerat.")
    out = Path(args.ut) if args.ut else RECORDINGS / f"{puck:%Y-%m-%d}-{home}-{away}"
    if out.exists() and any(out.iterdir()):
        print(f"OBS: {out} finns redan — ny data läggs till i befintliga filer.")

    print(f"Match:    {home}–{away}  {uuid}")
    print(f"Nedsläpp: {puck:%Y-%m-%d %H:%M}")
    print(f"Start:    {start:%Y-%m-%d %H:%M}")
    print(f"Stopp:    {args.after} min efter slutsignalen, senast {hard_stop:%Y-%m-%d %H:%M}")
    print(f"Lampa:    {args.lampa or '–'}   USB: {'ja, om den kopplas in' if args.serial else 'nej'}")
    print(f"Mapp:     {out}", flush=True)

    keep_awake()
    signal.signal(signal.SIGTERM, on_sigterm)
    wait = (start - datetime.now()).total_seconds()
    if wait > 0:
        print(f"{stamp(False)} väntar till {start:%H:%M} …", flush=True)
        try:
            time.sleep(wait)
        except KeyboardInterrupt:
            print("\navbruten innan inspelningen började")
            return

    Recorder(uuid, out, args.lampa, args.serial).run(args.after * 60, hard_stop)


if __name__ == "__main__":
    main()
