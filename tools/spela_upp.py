#!/usr/bin/env python3
"""
Spela upp en inspelad match på en lampa, med hela matchljuset: ställningen,
pauserna, stämningen mot slutet, skottrycket, målen och segerdansen.

    python3 tools/spela_upp.py 192.168.1.58 mock/recordings/2026-09-26-IFB-FBK
    python3 tools/spela_upp.py 192.168.1.58 mock/recordings/2026-09-26-IFB-FBK --från 17:05
    python3 tools/spela_upp.py 192.168.1.58 mock/recordings/2026-09-24-OHK-IFB --fart 4
    python3 tools/spela_upp.py - mock/recordings/2026-09-26-IFB-FBK --fart 0   # torrkörning

Läser lampa.jsonl från tools/forbehandla.py (mappen eller filen) och trycker
ramarna till lampans POST /push i samma takt som de kom under matchen. Ramarna
har strömmens format och går genom samma tolkning i firmwaren som de riktiga,
så omsändningar och ställningar som går bakåt testar lampans skydd på riktigt.
Tv-fördröjningen på lampan gäller som vanligt.

Källor:
    --ström sse1   den SSE-anslutning som spelas upp (sse1)
    --utan-poll    utan reservpollningen. Annars skickas game-overview som
                   lampan skulle ha sett den, var POLL_LIVE_FALLBACK_MS
                   (include/config.h) — det är den som räddade 0–5 mot FBK.

Kräver firmware med uppspelningsstöd i /push (frames). Felsökningsläget slås
på om det var av och återställs efteråt. Lampan släpper push-läget och hämtar
från SHL själv igen inom PUSH_LEASE_MS (fem minuter) efter sista pushen.

--fart ändrar bara takten ramarna skickas i. Lampans egna tider — tv-
fördröjningen, skottens halveringstid, stämningens utjämning — går i verklig
tid, så över 1 blir matchljuset mindre likt originalet. --fart 0 skickar allt
direkt, för torrkörning.

Inga beroenden utanför Pythons standardbibliotek.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import threading
import time
from datetime import datetime
from pathlib import Path
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import HTTPRedirectHandler, Request, build_opener

REPO = Path(__file__).resolve().parent.parent
HEARTBEAT_S = 60
BATCH_S = 0.25            # rader närmare än så skickas i samma push


def config_ms(name: str, default: int) -> int:
    """Läser ett '(N * 1000)'-värde ur include/config.h."""
    try:
        text = (REPO / "include" / "config.h").read_text()
        m = re.search(rf"#define {name}\s+\(([\dUL\s*]+)\)", text)
        if m:
            v = 1
            for part in m.group(1).split("*"):
                v *= int(part.strip().rstrip("UL"))
            return v
    except OSError:
        pass
    return default


POLL_MS = config_ms("POLL_LIVE_FALLBACK_MS", 45000)


# ─────────────────────────────────────────────────────────────────────────────
#  Lampan
# ─────────────────────────────────────────────────────────────────────────────
class NoRedirect(HTTPRedirectHandler):
    # /settings svarar 303 till statussidan; den behöver inte hämtas.
    def redirect_request(self, *args, **kwargs):
        return None


OPENER = build_opener(NoRedirect)


class Lamp:
    def __init__(self, host: str) -> None:
        self.dry = host == "-"
        self.base = (host if "://" in host else "http://" + host).rstrip("/")
        self.lock = threading.Lock()
        self.next: dict = {}
        self.live = False
        self.pushes = 0

    def call(self, path: str, body: bytes = b"", ctype: str = "") -> str:
        if self.dry:
            return ""
        req = Request(self.base + path, data=body if ctype else None,
                      headers={"Content-Type": ctype} if ctype else {})
        try:
            with OPENER.open(req, timeout=6) as r:
                return r.read().decode("utf-8", "replace")
        except HTTPError as e:
            if e.code == 303:
                return ""
            raise

    def read_settings(self) -> dict:
        if self.dry:
            return {"dbgpush": True, "mode": "(torrkörning)", "fw": "-"}
        page = self.call("/")
        mode = re.search(r"Läge: <b>([^<]*)</b>", page)
        fw = re.search(r"Firmware</[^>]+>\s*<[^>]+>([^<]+)", page)
        return {"dbgpush": "<option value=1 selected>" in page,
                "mode": mode.group(1) if mode else "?",
                "fw": fw.group(1).strip() if fw else "?"}

    def settings(self, **fields) -> None:
        self.call("/settings", urlencode(fields).encode(), "application/x-www-form-urlencoded")

    def push(self, frames: list[dict] | None = None, live: bool | None = None) -> None:
        with self.lock:
            if live is not None:
                self.live = live
            body: dict = {"next": self.next, "live": self.live}
            # Tom lista räcker för att lampan ska veta att det är en
            # uppspelning och köra matchljuset.
            body["frames"] = frames or []
            self.pushes += 1
        self.call("/push", json.dumps(body, ensure_ascii=False).encode(), "application/json")


# ─────────────────────────────────────────────────────────────────────────────
#  Ramarna
# ─────────────────────────────────────────────────────────────────────────────
def frame_of(row: dict) -> dict | None:
    """En rad ur lampa.jsonl tillbaka till en ram i strömmens format, med
    precis de fält firmwaren läser."""
    f: dict = {}
    if row.get("state"):
        f["liveState"] = {"liveState": row["state"]}
    if row.get("clock"):
        f["gameTime"] = {"period": row["clock"][0], "periodTime": row["clock"][1]}
    ev = row.get("event")
    score = row.get("score")
    if ev:
        e: dict = {"type": ev.get("type"), "eventId": ev.get("id")}
        if ev.get("team"):
            e["eventTeam"] = {"teamCode": ev["team"]}
        if score:
            e["homeTeam"] = {"score": score[0]}
            e["awayTeam"] = {"score": score[1]}
        f["liveEvent"] = e
    elif score:
        f["homeTeam"] = {"score": score[0]}
        f["awayTeam"] = {"score": score[1]}
    return f or None


def poll_frame(row: dict) -> dict:
    return {"homeGoals": row["score"][0], "awayGoals": row["score"][1]}


def load(path: Path) -> tuple[dict, list[dict]]:
    if path.is_dir():
        path = path / "lampa.jsonl"
    if not path.exists():
        raise SystemExit(f"{path} finns inte. Kör tools/forbehandla.py på inspelningen först.")
    with open(path, encoding="utf-8") as fh:
        head = json.loads(fh.readline())
        rows = [json.loads(line) for line in fh]
    return head, rows


def at_clock(text: str, day: str) -> str:
    """'17:05' → '2026-09-26T17:05:00.000' (inspelningsdagen)."""
    if re.fullmatch(r"\d{1,2}:\d{2}", text):
        h, m = text.split(":")
        return f"{day}T{int(h):02d}:{m}:00.000"
    return datetime.fromisoformat(text).isoformat(timespec="milliseconds")


def plan(rows: list[dict], stream: str, poll: bool, start: str | None, stop: str | None
         ) -> tuple[list[dict], list[tuple]]:
    """Startramar (läget vid --från, i en push) och tidslinjen: (t, ramar, text)."""
    sse = [r for r in rows if r["src"] == stream]
    if not sse:
        found = sorted({r["src"] for r in rows if r["src"].startswith("sse")})
        raise SystemExit(f"{stream} finns inte i filen. Finns: {', '.join(found)}")
    polls = [r for r in rows if r["src"] == "poll" and r.get("score")] if poll else []

    first_t = sse[0]["t"]
    t0 = start or first_t
    last_t = stop or default_end(sse, polls) or max(sse[-1]["t"], polls[-1]["t"] if polls else "")

    # Läget vid --från: senaste liveState och klocka före starten, och den
    # högsta ställningen hittills — den lampan skulle stå på, eftersom
    # omsändningar med lägre ställning inte får dra tillbaka den. Allt i en
    # enda ram, som kalibrerar lampan utan att tända något.
    boot: list[dict] = []
    if start:
        state = clock = score = None
        for r in sse + polls:
            if r["t"] >= t0:
                continue
            if r["src"] != "poll":
                state = r.get("state", state)
                clock = r.get("clock", clock)
            if r.get("score") and (score is None or sum(r["score"]) > sum(score)):
                score = r["score"]
        f = frame_of({"state": state, "clock": clock, "score": score})
        if f:
            boot.append(f)

    timeline: list[tuple[str, list[dict], str]] = []
    for r in sse:
        if t0 <= r["t"] <= last_t:
            f = frame_of(r)
            if f:
                timeline.append((r["t"], [f], describe(r), r))

    # Pollningen som lampan skulle ha gjort: var POLL_MS sedan starten, med
    # det game-overview svarade senast före den tidpunkten.
    if polls:
        t = datetime.fromisoformat(t0)
        end = datetime.fromisoformat(last_t)
        i, latest = 0, None
        while t <= end:
            ts = t.isoformat(timespec="milliseconds")
            while i < len(polls) and polls[i]["t"] <= ts:
                latest = polls[i]
                i += 1
            if latest:
                timeline.append((ts, [poll_frame(latest)],
                                 f"poll {latest['score'][0]}–{latest['score'][1]}", latest))
            t = datetime.fromtimestamp(t.timestamp() + POLL_MS / 1000)
    timeline.sort(key=lambda x: x[0])
    return boot, timeline


def default_end(sse: list[dict], polls: list[dict]) -> str | None:
    """20 minuter efter slutsignalen, enligt strömmen eller pollningen som
    kommer först. Inspelningen kan fortsätta i timmar efteråt."""
    marks = [r["t"] for r in sse if r.get("state") == "decided"][:1]
    marks += [r["t"] for r in polls if r.get("state") == "GameEnded"][:1]
    if not marks:
        return None
    end = datetime.fromisoformat(min(marks)).timestamp() + 20 * 60
    return datetime.fromtimestamp(end).isoformat(timespec="milliseconds")


def describe(r: dict) -> str:
    parts = []
    if "score" in r:
        parts.append(f"{r['score'][0]}–{r['score'][1]}")
    if "state" in r:
        parts.append(r["state"])
    if "clock" in r:
        parts.append(f"P{r['clock'][0]} {r['clock'][1]}")
    ev = r.get("event")
    if ev:
        parts.append(f"{ev['type']}#{ev['id']}" + (f" {ev['team']}" if ev.get("team") else "")
                     + (" (omsänd)" if ev.get("again") else ""))
    return "  ".join(parts)


# ─────────────────────────────────────────────────────────────────────────────
#  Uppspelning
# ─────────────────────────────────────────────────────────────────────────────
def run(lamp: Lamp, head: dict, boot: list[dict], timeline: list, speed: float,
        hold_s: float, verbose: bool) -> None:
    home, away = head.get("home") or "HEMMA", head.get("away") or "BORTA"
    lamp.next = {"home": home, "away": away, "homeIsUs": head.get("us") == "home",
                 "text": f"{home} – {away}  (uppspelning {head.get('recorded', '')})"}

    before = lamp.read_settings()
    print(f"Lampan: {before['mode']}, firmware {before['fw']}, "
          f"felsökningsläge {'på' if before['dbgpush'] else 'av'}")
    if before["mode"].startswith("Seger"):
        raise SystemExit("Segerläget är aktivt och går före allt som trycks in. Vänta ut det först.")

    if not before["dbgpush"]:
        lamp.settings(dbgpush="1")

    stop = threading.Event()

    def heartbeat() -> None:
        while not stop.wait(HEARTBEAT_S):
            try:
                lamp.push()
            except Exception as exc:  # noqa: BLE001
                print(f"  ! hjärtslaget gick inte fram: {exc}", file=sys.stderr)

    goals = 0
    try:
        lamp.push(boot, live=True)
        if boot:
            print(f"Startläge: {json.dumps(boot[0], ensure_ascii=False)}")
        threading.Thread(target=heartbeat, daemon=True).start()

        t_first = datetime.fromisoformat(timeline[0][0]) if timeline else None
        wall0 = time.monotonic()
        shown = {"sse": None, "poll": None, "state": None, "back": 0}
        if boot:
            b = boot[0]
            if "homeTeam" in b:
                shown["sse"] = shown["poll"] = (b["homeTeam"]["score"], b["awayTeam"]["score"])
            shown["state"] = (b.get("liveState") or {}).get("liveState")
        i = 0
        while i < len(timeline):
            t = timeline[i][0]
            rel = (datetime.fromisoformat(t) - t_first).total_seconds()
            if speed > 0:
                wait = wall0 + rel / speed - time.monotonic()
                if wait > 0:
                    time.sleep(wait)
            j = i + 1
            while j < len(timeline) and (datetime.fromisoformat(timeline[j][0])
                                         - datetime.fromisoformat(t)).total_seconds() < BATCH_S:
                j += 1
            batch = [f for entry in timeline[i:j] for f in entry[1]]
            try:
                lamp.push(batch)
            except Exception as exc:  # noqa: BLE001
                print(f"  ! push gick inte fram: {exc}", file=sys.stderr)
            for ts, _, text, row in timeline[i:j]:
                print_line(ts, text, row, shown, verbose)
            i = j

        if hold_s > 0 and speed > 0:
            print(f"Slut på inspelningen — håller matchfönstret öppet {hold_s:.0f} s till "
                  "(tv-fördröjning och segerdans)")
            time.sleep(hold_s)
        lamp.push(live=False)
        print(f"Klart: {lamp.pushes} pushar, {len(timeline)} ramar, "
              f"{shown['back']} med lägre ställning än lampan redan sett.")
    except KeyboardInterrupt:
        print("\nAvbruten.")
        try:
            lamp.push(live=False)
        except Exception:  # noqa: BLE001
            pass
    finally:
        stop.set()
        if not before["dbgpush"]:
            try:
                lamp.settings(dbgpush="0")
                print("Felsökningsläget avslaget igen — lampan hämtar från SHL själv.")
            except Exception as exc:  # noqa: BLE001
                print(f"  ! kunde inte slå av felsökningsläget: {exc}", file=sys.stderr)
        else:
            print("Felsökningsläget var på redan innan — lampan släpper push-läget själv "
                  "inom fem minuter.")


def print_line(t: str, text: str, row: dict, shown: dict, verbose: bool) -> None:
    """Skriver ut det som ändrar något: en ställning högre än lampan sett
    förut, i strömmen eller pollningen, och nytt matchläge. Ramar med lägre
    ställning — omsändningar — räknas bara. Med --alla skrivs varje ram."""
    kind = "poll" if row["src"] == "poll" else "sse"
    notes = []
    sc = tuple(row["score"]) if row.get("score") else None
    if sc:
        best = shown[kind]
        if best is None or sum(sc) > sum(best):
            if best is not None:
                notes.append("◀ NY STÄLLNING")
            shown[kind] = sc
        elif sum(sc) < sum(best):
            shown["back"] += 1
    if kind == "sse" and row.get("state") and row["state"] != shown["state"]:
        shown["state"] = row["state"]
        notes.append("◀ läge")
    if verbose or notes:
        print(f"  {t[11:19]}  {text}   {' '.join(notes)}".rstrip(), flush=True)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("lampa", help="lampans adress, eller - för torrkörning")
    ap.add_argument("inspelning", help="inspelningsmappen eller en lampa.jsonl")
    ap.add_argument("--ström", dest="stream", default="sse1", help="SSE-anslutningen att spela (sse1)")
    ap.add_argument("--utan-poll", dest="poll", action="store_false", help="utan reservpollningen")
    ap.add_argument("--från", dest="start", metavar="TID", help="börja vid HH:MM (hoppar fram)")
    ap.add_argument("--till", dest="stop", metavar="TID", help="sluta vid HH:MM")
    ap.add_argument("--fart", type=float, default=1.0, help="1 = verklig tid, 0 = allt direkt")
    ap.add_argument("--håll", dest="hold", type=float, default=120, metavar="S",
                    help="sekunder att hålla matchfönstret öppet efter sista ramen (120)")
    ap.add_argument("-v", "--alla", dest="verbose", action="store_true", help="skriv ut varje ram")
    args = ap.parse_args()

    head, rows = load(Path(args.inspelning))
    day = rows[0]["t"][:10]
    start = at_clock(args.start, day) if args.start else None
    stop = at_clock(args.stop, day) if args.stop else None
    boot, timeline = plan(rows, args.stream, args.poll, start, stop)
    if not timeline:
        raise SystemExit("Inga ramar i det valda intervallet.")

    span = (datetime.fromisoformat(timeline[-1][0]) - datetime.fromisoformat(timeline[0][0])).total_seconds()
    dur = span / args.fart if args.fart > 0 else 0
    print(f"{head.get('home')}–{head.get('away')}  {head.get('recorded', '')}  källa {args.stream}"
          f"{' + poll' if args.poll else ''}")
    print(f"{timeline[0][0][11:19]} – {timeline[-1][0][11:19]}, {len(timeline)} ramar, "
          f"tar {int(dur // 3600)} h {int(dur % 3600 // 60)} min i fart {args.fart:g}")
    run(Lamp(args.lampa), head, boot, timeline, args.fart, args.hold, args.verbose)


if __name__ == "__main__":
    main()
