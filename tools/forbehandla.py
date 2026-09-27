#!/usr/bin/env python3
"""
Städar en inspelning från tools/spela_in.py till det lampan faktiskt läser.

    python3 tools/forbehandla.py mock/recordings/2026-09-26-IFB-FBK
    python3 tools/forbehandla.py mock/recordings/2026-09-26-IFB-FBK --ström 1
    python3 tools/forbehandla.py mock/recordings/2026-09-26-IFB-FBK --ut match.jsonl

Resultatet är en JSON-rad per händelse i tidsordning, i lampa.jsonl i samma
mapp om inget annat anges. Första raden beskriver matchen:

    {"match": "n9m6y5xe6d", "home": "IFB", "away": "FBK", "us": "home", ...}

Därefter en rad per ram som bär något firmwaren läser (src/shl.cpp):

    {"t": "2026-09-26T15:20:56.828", "src": "sse1", "id": "1234",
     "score": [0, 1],                          findScorePair() på hela ramen
     "state": "ongoing",                       liveState.liveState
     "clock": [1, "02:16"],                    gameTime: period, periodTime
     "event": {"type": "goal", "id": 7,        liveEvent: typ och eventId,
               "team": "FBK", "us": false,     lag för skott och mål,
               "updated": "…", "lag": 11.2,    updatedTime (UTC) och
               "again": true}}                 eftersläpning; again = omsänd

    {"t": "…", "src": "poll", "score": [0, 1], "state": "Ongoing", "clock": [1, "02:16"]}

Ramar utan något av det — playerStatistics, teamStatistics utan ställning —
tas bort. Omsändningar behålls, eftersom lampan ser dem och ska klara dem;
"again" markerar dem. Pollraderna är game-overview, bara när svaret ändrats.

Tiderna "t" är lokal tid i Stockholm när ramen togs emot, utan zon. Det räcker
för uppspelning: skillnaden mellan två rader är den tid som gick.

Inga beroenden utanför Pythons standardbibliotek.
"""

from __future__ import annotations

import argparse
import gzip
import json
import re
import sys
from collections import Counter
from datetime import datetime, timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from live import TEAM_CODE, firmware_score  # noqa: E402 — samma sökning som findScorePair()

UTC_OFFSET = timedelta(hours=2)   # sommartid; updatedTime i strömmen är UTC utan zon


def open_text(path: Path):
    return gzip.open(path, "rt", encoding="utf-8") if path.suffix == ".gz" else open(path, encoding="utf-8")


def find(folder: Path, name: str) -> Path | None:
    for p in (folder / name, folder / f"{name}.gz"):
        if p.exists():
            return p
    return None


def local_iso(rx: str) -> str:
    """'2026-09-26T15:20:56.828+02:00' → '2026-09-26T15:20:56.828'."""
    return datetime.fromisoformat(rx).replace(tzinfo=None).isoformat(timespec="milliseconds")


def parse_updated(s: str | None) -> datetime | None:
    if not s or s.startswith("0001"):
        return None
    try:
        return datetime.fromisoformat(s[:26].replace("Z", "")) + UTC_OFFSET
    except ValueError:
        return None


# ─────────────────────────────────────────────────────────────────────────────
#  SSE
# ─────────────────────────────────────────────────────────────────────────────
def jsonl_frames(path: Path, day: datetime):
    """sse*.jsonl från live.py --logg: en ram per rad med mottagningstid."""
    with open_text(path) as fh:
        for line in fh:
            try:
                f = json.loads(line)
            except ValueError:
                continue
            yield local_iso(f["rx"]), f.get("id"), f.get("data")


def raw_frames(path: Path, day: datetime):
    """sse_raw.log från 2026-09-19: SSE-texten rad för rad med tidsstämpel,
    chunk-storlekarna kvar. En ram kan vara delad över flera chunkar, så
    data-raden fylls på tills den går att tolka."""
    prev: datetime | None = None
    sse_id, buf, t = None, "", ""
    with open_text(path) as fh:
        for line in fh:
            m = re.match(r"^(\d\d:\d\d:\d\d\.\d{3}) ?(.*)$", line.rstrip("\n"))
            if not m:
                continue
            prev = with_date(m[1], day, prev)
            body = m[2]
            if body.startswith("id:"):
                sse_id = body[3:].strip()
            elif body.startswith("data:"):
                buf, t = body[5:].lstrip(), prev.isoformat(timespec="milliseconds")
            elif buf and body and not re.fullmatch(r"[0-9a-f]+", body):
                buf += body
                t = prev.isoformat(timespec="milliseconds")
            if buf:
                try:
                    data = json.loads(buf)
                except ValueError:
                    continue
                yield t, sse_id, data
                buf = ""


def sse_rows(frames, src: str, meta: dict) -> list[dict]:
    rows = []
    seen: set[tuple[str, int]] = set()
    for t, sse_id, data in frames:
        if not isinstance(data, dict):
            continue
        for v in data.values():
            if isinstance(v, dict) and v.get("gameUuid"):
                meta.setdefault("match", v["gameUuid"])
        row: dict = {"t": t, "src": src, "id": sse_id}

        sc = firmware_score(data)
        if sc:
            row["score"] = list(sc)

        ls = data.get("liveState")
        if isinstance(ls, dict) and isinstance(ls.get("liveState"), str):
            row["state"] = ls["liveState"]
            meta.setdefault("gameSourceId", ls.get("gameSourceId"))

        gt = data.get("gameTime")
        if isinstance(gt, dict) and gt.get("period"):
            row["clock"] = [gt.get("period"), gt.get("periodTime")]

        ev = data.get("liveEvent")
        if isinstance(ev, dict):
            meta.setdefault("gameSourceId", ev.get("gameSourceId"))
            e: dict = {"type": ev.get("type"), "id": ev.get("eventId")}
            team = (ev.get("eventTeam") or {}).get("teamCode")
            if team and e["type"] in ("goal", "shot"):
                e["team"] = team
                e["us"] = team == TEAM_CODE
            if e["type"] == "goal":
                e["period"], e["time"] = ev.get("period"), ev.get("time")
                e["made"] = ev.get("realWorldTime")
            e["updated"] = ev.get("updatedTime")
            upd = parse_updated(ev.get("updatedTime"))
            if upd:
                e["lag"] = round((datetime.fromisoformat(t) - upd).total_seconds(), 1)
            key = (str(e["type"]), e["id"])
            if key in seen:
                e["again"] = True
            seen.add(key)
            row["event"] = e

        if len(row) > 3:
            rows.append(row)
    return rows


# ─────────────────────────────────────────────────────────────────────────────
#  Pollning
# ─────────────────────────────────────────────────────────────────────────────
def overview_row(t: str, d: object) -> dict | None:
    if not isinstance(d, dict):
        return None
    g = d.get("gameOverview", d)
    sc = firmware_score(g)
    tm = g.get("time") or {}
    row: dict = {"t": t, "src": "poll"}
    if sc:
        row["score"] = list(sc)
    if g.get("state"):
        row["state"] = g["state"]
    if tm.get("period"):
        row["clock"] = [tm.get("period"), tm.get("periodTime")]
    return row if len(row) > 2 else None


def with_date(clock: str, day: datetime, prev: datetime | None) -> datetime:
    """HH:MM:SS.mmm på inspelningsdagen, med övergång till nästa dygn."""
    t = datetime.combine(day.date(), datetime.strptime(clock, "%H:%M:%S.%f").time())
    while prev and t < prev - timedelta(hours=1):
        t += timedelta(days=1)
    return t


def poll_rows(folder: Path, day: datetime) -> list[dict]:
    rows: list[dict] = []
    prev: datetime | None = None
    http = find(folder, "http.jsonl")
    if http:
        with open_text(http) as fh:
            for line in fh:
                d = json.loads(line)
                if d.get("src") != "game-overview" or "error" in d:
                    continue
                prev = with_date(d["rx"], day, prev)
                row = overview_row(prev.isoformat(timespec="milliseconds"), d.get("data"))
                if row:
                    rows.append(row)
        return rows

    # Äldre inspelningar har bara poll.log: "HH:MM:SS.mmm  454 b sha 0–3 Ongoing P2 20:00".
    log = find(folder, "poll.log")
    if not log:
        return rows
    pat = re.compile(r"^(\d\d:\d\d:\d\d\.\d{3}).*? (\d+)–(\d+) (\w+) P(\w+) (\S+)")
    last = None
    with open_text(log) as fh:
        for line in fh:
            m = pat.match(line)
            if not m:
                continue
            prev = with_date(m[1], day, prev)
            row: dict = {"t": prev.isoformat(timespec="milliseconds"), "src": "poll",
                         "score": [int(m[2]), int(m[3])], "state": m[4]}
            if m[5].isdigit():
                row["clock"] = [int(m[5]), m[6]]
            key = json.dumps({k: v for k, v in row.items() if k != "t"})
            if key != last:
                rows.append(row)
                last = key
    return rows


# ─────────────────────────────────────────────────────────────────────────────
#  Sammanfattning
# ─────────────────────────────────────────────────────────────────────────────
def summary(rows: list[dict], meta: dict) -> None:
    print(f"{meta.get('home')}–{meta.get('away')}  {meta.get('match')}  "
          f"(Björklöven {'hemma' if meta.get('us') == 'home' else 'borta'})")
    by_src = Counter(r["src"] for r in rows)
    print("Rader per källa: " + ", ".join(f"{k} {v}" for k, v in sorted(by_src.items())))

    # Första gången varje ställning syns, per källa. Det är det lampan tänder på.
    first: dict[tuple, dict[str, str]] = {}
    for r in rows:
        if "score" in r:
            first.setdefault(tuple(r["score"]), {}).setdefault(r["src"], r["t"][11:19])
    srcs = sorted(by_src)
    print("\nFörsta gången varje ställning syns:")
    print(f"  {'ställn':<7}" + "".join(f"{s:>10}" for s in srcs))
    for sc in sorted(first, key=lambda s: (sum(s), min(first[s].values()))):
        print(f"  {sc[0]}–{sc[1]:<5}" + "".join(f"{first[sc].get(s, '–'):>10}" for s in srcs))

    # Ställningar som går bakåt — omsändningar och polling som hoppar tillbaka.
    for s in srcs:
        best, back = (0, 0), 0
        for r in rows:
            if r["src"] == s and "score" in r:
                sc = tuple(r["score"])
                if sum(sc) < sum(best):
                    back += 1
                best = max(best, sc, key=sum)
        if back:
            print(f"  {s}: {back} rader med lägre ställning än redan visad")

    again = sum(1 for r in rows if r.get("event", {}).get("again"))
    print(f"\nOmsända händelser: {again}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mapp", help="inspelningsmappen, t.ex. mock/recordings/2026-09-26-IFB-FBK")
    ap.add_argument("--ström", dest="stream", type=int, action="append", metavar="N",
                    help="ta bara med sse N (kan anges flera gånger; standard alla)")
    ap.add_argument("--utan-poll", dest="poll", action="store_false", help="hoppa över game-overview")
    ap.add_argument("--ut", metavar="FIL", help="utfil (lampa.jsonl i mappen)")
    args = ap.parse_args()

    folder = Path(args.mapp)
    m = re.match(r"(\d{4}-\d\d-\d\d)", folder.name)
    if not m:
        raise SystemExit("Mappnamnet ska börja med datum, ÅÅÅÅ-MM-DD.")
    day = datetime.strptime(m[1], "%Y-%m-%d")

    meta: dict = {}
    rows: list[dict] = []
    streams = sorted(set(args.stream or range(1, 10)))
    for n in streams:
        p = find(folder, f"sse{n}.jsonl")
        if p:
            rows += sse_rows(jsonl_frames(p, day), f"sse{n}", meta)
        elif args.stream and n != 1:
            raise SystemExit(f"Hittar inte sse{n}.jsonl i {folder}")
    raw = find(folder, "sse_raw.log")
    if not rows and raw and 1 in streams:
        rows += sse_rows(raw_frames(raw, day), "sse1", meta)
    if not rows:
        raise SystemExit(f"Hittar ingen SSE-inspelning i {folder}.")
    if args.poll:
        rows += poll_rows(folder, day)
    rows.sort(key=lambda r: (r["t"], r["src"]))

    sid = meta.pop("gameSourceId", "") or ""
    parts = sid.split("-")
    head = {"match": meta.get("match"), "home": parts[1] if len(parts) == 3 else None,
            "away": parts[2] if len(parts) == 3 else None}
    head["us"] = "home" if head["home"] == TEAM_CODE else "away"
    head["recorded"] = folder.name
    head["from"] = rows[0]["t"]
    head["to"] = rows[-1]["t"]
    head["sources"] = sorted({r["src"] for r in rows})

    out = Path(args.ut) if args.ut else folder / "lampa.jsonl"
    with open(out, "w", encoding="utf-8") as fh:
        fh.write(json.dumps(head, ensure_ascii=False) + "\n")
        for r in rows:
            fh.write(json.dumps(r, ensure_ascii=False, separators=(",", ":")) + "\n")

    summary(rows, head)
    print(f"\n{len(rows)} rader → {out} ({out.stat().st_size / 1024:.0f} kB)")


if __name__ == "__main__":
    main()
