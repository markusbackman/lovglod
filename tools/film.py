#!/usr/bin/env python3
"""
Filmmanus för LövGlöd — spelar upp exakt det varje film på webbplatsen ska
visa, på sekunden, medan kameran rullar.

    python3 tools/film.py 192.168.1.250 glod
    python3 tools/film.py 192.168.1.250 match
    python3 tools/film.py 192.168.1.250 mal
    python3 tools/film.py 192.168.1.250 huvud
    python3 tools/film.py - huvud --torr          # visa tidslinjen, rör ingen lampa

Lampan styrs som mockservern gör det: hela matchläget trycks in på POST /push,
med hjärtslag emellan så att PUSH_LEASE_MS aldrig löper ut. Målet tänds däremot
med POST /test. Det är samma fyrverkeri, men utan tv-fördröjningen — ett mål
via ställningen skulle vänta settings.goalDelayS (15 s som standard) och då
stämmer inte tidslinjen.

Före start läses lampans inställningar av från statussidan. Felsökningsläget
slås på om det var av, och allt återställs när manuset är slut eller avbryts
med Ctrl-C. Var felsökningsläget av från början hämtar lampan från SHL igen
direkt efteråt.

Flödet: lampan ställs i startläget, du startar inspelningen och trycker Enter,
tre pip räknar ner och på det fjärde (T0) börjar tidslinjen. Pipet är
synkmarkören i ljudspåret; klipp bort allt före.

Inga beroenden utanför Pythons standardbibliotek.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import HTTPRedirectHandler, Request, build_opener

# Måste stämma med SHL_TEAM_CODE i include/config.h.
TEAM_CODE = "IFB"
OPPONENT = "LHF"

HEARTBEAT_S = 20          # långt under PUSH_LEASE_MS (5 min)
GOAL_S = 12               # GOAL_DURATION_MS
GLOW_BREATH_S = 60 / 9    # GLOW_BPM → ~6,7 s
LIVE_BREATH_S = 60 / 18   # GLOW_BPM * 2 under match → ~3,3 s


# ─────────────────────────────────────────────────────────────────────────────
#  Manus
# ─────────────────────────────────────────────────────────────────────────────
# Varje steg: (sekunder från T0, åtgärd, text i loggen). Åtgärderna är
#   "glod"    standby, gul glöd
#   "gnistor" standby med gnistor (vann senaste matchen)
#   "match"   matchfönster öppet, bärnsten
#   "mal"     målfyrverkeriet, tolv sekunder
#   "slut"    tidslinjen är klar — sista bilden står kvar
# Första steget ligger alltid på 0 och är startläget som ställs in före Enter.
#
# Looparna spelas in längre än de ska bli, så att det finns flera andetag att
# välja en sömlös skarv bland.
@dataclass
class Script:
    title: str
    note: str
    steps: list[tuple[float, str, str]]


SCRIPTS: dict[str, Script] = {
    "glod": Script(
        "Glöden (kort 1, tyst loop)",
        f"Klipp ut två andetag, {2 * GLOW_BREATH_S:.1f} s, med skarven på "
        "andetagets botten.",
        [
            (0, "glod", "gul glöd"),
            (40, "slut", "sex andetag inspelade"),
        ],
    ),
    "match": Script(
        "Match pågår (kort 2, tyst loop)",
        f"Klipp ut tre andetag, {3 * LIVE_BREATH_S:.1f} s, med skarven på "
        "andetagets botten.",
        [
            (0, "match", "bärnsten, matchen pågår"),
            (30, "slut", "nio andetag inspelade"),
        ],
    ),
    "mal": Script(
        "MÅL (kort 3, med ljud)",
        "Klipp från T0+2 till T0+17: en sekund bärnsten, tolv sekunder mål, "
        "två sekunder efter.",
        [
            (0, "match", "bärnsten, matchen pågår"),
            (3, "mal", "MÅL — stroboskop 2,5 s, sedan kometer"),
            (3 + GOAL_S, "match", "fyrverkeriet slut, tillbaka i bärnsten"),
            (3 + GOAL_S + 5, "slut", ""),
        ],
    ),
    "huvud": Script(
        "Huvudfilm (hela ljusspråket)",
        "Ca 55 s rått. Varje byte ligger på en jämn sekund i loggen nedan.",
        [
            (0, "glod", "vardag: gul glöd"),
            (14, "match", "nedsläpp: bärnsten, snabbare andetag"),
            (24, "mal", "MÅL"),
            (24 + GOAL_S, "match", "fyrverkeriet slut, matchen fortsätter"),
            (42, "gnistor", "efter en vinst: glöd med gnistor fram till nästa match"),
            (56, "slut", ""),
        ],
    ),
}


# ─────────────────────────────────────────────────────────────────────────────
#  Lampan
# ─────────────────────────────────────────────────────────────────────────────
class NoRedirect(HTTPRedirectHandler):
    # /settings och /test svarar 303 till statussidan. Att följa den kostar en
    # hel sidrendering på lampan, och det är tid som hamnar i tidslinjen.
    def redirect_request(self, *args, **kwargs):
        return None


OPENER = build_opener(NoRedirect)


class Lamp:
    def __init__(self, host: str, dry: bool) -> None:
        self.base = (host if "://" in host else "http://" + host).rstrip("/")
        self.dry = dry
        self.lock = threading.Lock()
        self.state = "glod"
        self.stop = threading.Event()

    def _call(self, path: str, body: bytes = b"", ctype: str = "") -> str:
        if self.dry:
            return ""
        req = Request(self.base + path, data=body if ctype else None,
                      headers={"Content-Type": ctype} if ctype else {})
        try:
            with OPENER.open(req, timeout=4) as r:
                return r.read().decode("utf-8", "replace")
        except HTTPError as e:
            if e.code == 303:                      # väntat svar från formulären
                return ""
            raise

    # ── inställningar ──────────────────────────────────────────────────────
    def read_settings(self) -> dict:
        if self.dry:
            return {"bright": 0, "dbgpush": True, "mode": "(torrkörning)"}
        page = self._call("/")
        bright = re.search(r"name=bright[^>]*value='(\d+)'", page)
        mode = re.search(r"Läge: <b>([^<]*)</b>", page)
        return {
            "bright": int(bright.group(1)) if bright else 0,
            "dbgpush": "<option value=1 selected>" in page,
            "mode": mode.group(1) if mode else "?",
        }

    def settings(self, **fields) -> None:
        self._call("/settings", urlencode(fields).encode(),
                   "application/x-www-form-urlencoded")

    # ── matchläget ─────────────────────────────────────────────────────────
    def payload(self) -> dict:
        live = self.state == "match"
        return {
            "next": {"home": TEAM_CODE, "away": OPPONENT, "homeIsUs": True,
                     "text": f"{TEAM_CODE} – {OPPONENT}  (film)"},
            "live": live,
            # Ställningen står still: målet tänds via /test, och en ändrad
            # ställning skulle tända ett till, fördröjt.
            "score": {"home": 0, "away": 0},
            "last": {"won": self.state == "gnistor",
                     "text": f"{TEAM_CODE} 4-2 {OPPONENT}  (film)"},
        }

    def push(self) -> None:
        with self.lock:
            body = json.dumps(self.payload()).encode()
        self._call("/push", body, "application/json")

    def set(self, action: str) -> None:
        if action == "mal":
            self._call("/test", b"", "application/x-www-form-urlencoded")
            return
        with self.lock:
            self.state = action
        self.push()

    def heartbeat(self) -> None:
        while not self.stop.wait(HEARTBEAT_S):
            try:
                self.push()
            except Exception as exc:
                print(f"\n  ! hjärtslaget gick inte fram: {exc}", file=sys.stderr)


# ─────────────────────────────────────────────────────────────────────────────
#  Uppspelning
# ─────────────────────────────────────────────────────────────────────────────
def beep(final: bool = False) -> None:
    sound = "/System/Library/Sounds/" + ("Glass" if final else "Tink") + ".aiff"
    if shutil.which("afplay"):
        subprocess.Popen(["afplay", sound], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    else:
        print("\a", end="", flush=True)


def stamp(t: float) -> str:
    return f"{int(t // 60):02d}:{t % 60:04.1f}"


def run(lamp: Lamp, script: Script, brightness: int | None) -> None:
    print(f"\n  {script.title}")
    print(f"  {script.note}\n")

    before = lamp.read_settings()
    print(f"  lampan: {before['mode']}, ljusstyrka {before['bright']}, "
          f"felsökningsläge {'på' if before['dbgpush'] else 'av'}")
    if before["mode"].startswith("Seger"):
        print("  ! segerläget är aktivt och går före allt som trycks in.\n"
              "    Vänta ut det eller starta om lampan efter att ha nollställt det.")
        return

    changed: dict[str, str] = {}
    if not before["dbgpush"]:
        changed["dbgpush"] = "0"
    if brightness is not None and brightness != before["bright"]:
        if not before["bright"]:
            print("  ! hittade inte ljusstyrkan på statussidan — lämnar den orörd")
            brightness = None
        else:
            changed["bright"] = str(before["bright"])

    try:
        fields = {}
        if "dbgpush" in changed:
            fields["dbgpush"] = "1"
        if "bright" in changed:
            fields["bright"] = str(brightness)
        if fields:
            lamp.settings(**fields)

        first = script.steps[0][1]
        lamp.set(first)
        threading.Thread(target=lamp.heartbeat, daemon=True).start()
        print(f"  startläge: {script.steps[0][2]}\n")

        input("  Starta inspelningen och tryck Enter … ")
        for n in (3, 2, 1):
            print(f"  {n}", flush=True)
            beep()
            time.sleep(1)
        beep(final=True)
        t0 = time.monotonic()
        print(f"  [{stamp(0)}]  T0  {script.steps[0][2]}")

        for at, action, text in script.steps[1:]:
            wait = t0 + at - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            if action != "slut":
                lamp.set(action)
            print(f"  [{stamp(time.monotonic() - t0)}]  {text or 'klart'}")

        print("\n  Stoppa inspelningen.")
    finally:
        lamp.stop.set()
        if changed:
            try:
                lamp.settings(**changed)
                print("  inställningarna återställda: " +
                      ", ".join(f"{k}={v}" for k, v in changed.items()))
            except Exception as exc:
                print(f"  ! kunde inte återställa {changed}: {exc}", file=sys.stderr)
        elif before["dbgpush"]:
            print("  felsökningsläget var på redan innan — lampan släpper "
                  "push-läget själv inom fem minuter")


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Spela upp en film på LövGlöd, på sekunden.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="\n".join(f"  {k:6} {s.title}" for k, s in SCRIPTS.items()))
    ap.add_argument("lampa", help="lampans adress, t.ex. 192.168.1.250 (- med --torr)")
    ap.add_argument("film", choices=SCRIPTS)
    ap.add_argument("--ljusstyrka", type=int, metavar="5-255",
                    help="samma värde i alla tagningar; återställs efteråt")
    ap.add_argument("--torr", action="store_true",
                    help="skriv bara ut tidslinjen i realtid, rör ingen lampa")
    args = ap.parse_args()

    if args.ljusstyrka is not None and not 5 <= args.ljusstyrka <= 255:
        ap.error("--ljusstyrka ska ligga mellan 5 och 255")

    try:
        run(Lamp(args.lampa, args.torr), SCRIPTS[args.film], args.ljusstyrka)
    except KeyboardInterrupt:
        print("\n  avbrutet")
    except OSError as exc:
        sys.exit(f"  ! når inte lampan på {args.lampa}: {exc}")


if __name__ == "__main__":
    main()
