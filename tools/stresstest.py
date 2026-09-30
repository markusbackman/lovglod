#!/usr/bin/env python3
"""
Stresstest av lampan över USB: inspelade matcher i hög fart, med mål, suckar,
demoläget, nättestet och omstarter kastade in mitt i, medan varje rad från
lampan läses efter krascher, häng, vakthundar och en heap som krymper.

    python3 tools/stresstest.py rök                      # tre minuter, allt en gång
    python3 tools/stresstest.py full --minuter 25        # matcher + missbruk + långkörning
    python3 tools/stresstest.py full --minuter 20 --fart 40 --inspelning mock/recordings/2026-09-24-OHK-IFB
    python3 tools/stresstest.py kapp --varv 20           # köade mål mot loopens setMode()
    python3 tools/stresstest.py lyssna --sekunder 120    # bara övervakning

Kräver diagnostikbygget med seriekroken (pio run -e esp32dev_diag -t upload):
en rad som börjar med { är en push i samma format som POST /push, och lampan
svarar "[push] ok" när loopen tillämpat den. Värden skickar nästa rad först
då, så ingen ram skrivs över medan loopen står i ett TLS-handslag. "@kö N MS"
köar mål direkt i listen, fler än GOAL_QUEUE_MAX om man vill.

Lampan nås bara över seriekabeln. Porten öppnas med en puls på RTS, som
startar om lampan: då börjar fångsten rent på ROM-bannern, i stället för mitt
i det som Macens CP2102-drivrutin upprepar de första sekunderna efter att
porten öppnats. Rader som upprepas i följd skrivs en gång med antal.

Det som räknas som fel:
    Guru Meditation, Backtrace, abort(), assert, CORRUPT HEAP, stack canary,
    task-vakthunden, brownout, en omstart vi inte bad om (rst:0x eller
    "[boot] föregående omstart:" utan föregående r eller RTS-puls), en rapport
    om krasch i telemetrin, och tystnad: ingen [stat]-rad på STILLA_S sekunder
    fast lampan ska vara uppe. Då pulsas RTS och omstartsorsaken noteras.
Backtrace-adresserna avkodas med addr2line mot .pio/build/esp32dev_diag.

Rå logg och sammanfattning hamnar i --logg (en mapp per körning). Varje
[stat]-rad blir en punkt i heap- och stacktabellerna; heapens lutning räknas
per uppstart, eftersom minsta heap nollställs vid varje omstart.

Seriekommandona läser ett tecken och slänger resten av det som ligger i
bufferten. Därför skickas en push aldrig direkt efter ett kommando: värden
väntar först in kommandots svar, annars kunde pushen slängas som "resten av
raden".

Beroenden: pyserial (finns i PlatformIO:s python, ~/.platformio/penv/bin/python).
"""

from __future__ import annotations

import argparse
import json
import random
import re
import subprocess
import sys
import threading
import time
from collections import Counter
from datetime import datetime
from pathlib import Path

try:
    import serial
except ImportError:
    raise SystemExit("pyserial saknas. Kör med ~/.platformio/penv/bin/python.")

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import spela_upp as su  # noqa: E402  planen och ramformatet delas med uppspelningen

PORT = "/dev/cu.usbserial-0001"
ELF = REPO / ".pio" / "build" / "esp32dev_diag" / "firmware.elf"
ADDR2LINE = Path.home() / ".platformio/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-addr2line"
STILLA_S = 70            # ingen [stat] så här länge = troligt häng ([stat] kommer var 30:e s)
KVITTO_S = 25            # väntan på "[push] ok" — nättestet kan blockera loopen upp mot 30 s
RAD_MAX = 4000           # värdens tak för en push-rad; lampans är 8192
BATCH_S = su.BATCH_S
SETTLE_S = 3.0           # CP2102-bruset efter öppning, se ovan

# Rader som betyder att något gått sönder, oavsett när de kommer.
FEL = [
    ("krasch", re.compile(r"Guru Meditation")),
    ("krasch", re.compile(r"^Backtrace:")),
    ("krasch", re.compile(r"abort\(\) was called")),
    ("assert", re.compile(r"assert(ion)? failed|assert failed", re.I)),
    ("heap", re.compile(r"CORRUPT HEAP|heap_caps.*(fail|corrupt)", re.I)),
    ("stack", re.compile(r"Stack canary|stack overflow|Stack smashing", re.I)),
    ("vakthund", re.compile(r"Task watchdog|task_wdt|TWDT", re.I)),
    ("brownout", re.compile(r"Brownout", re.I)),
    ("telemetri", re.compile(r"^\[tele\] förra rapporten")),
    ("vakthund", re.compile(r"rendertasken står utan vakthund")),
]
RST = re.compile(r"^rst:0x([0-9a-f]+) \(([A-Z_0-9]+)\)")
BOOT = re.compile(r"^\[boot\] föregående omstart: (.*)")
STAT = re.compile(r"^\[stat\] (\S+)\s+läge=(\w+).*?kö=(\d+)\s+heap=(\d+)/(\d+) kB.*?ledstack=(\d+)")
LED = re.compile(r"^\[led\] (\w+) → (\w+)")
ACK = re.compile(r"^\[push\] (ok|fel)(.*)")
BT_ADDR = re.compile(r"0x(4[0-9a-f]{7}):0x[0-9a-f]{8}")

# Omstartsorsaker som hör till en omstart vi själva bad om.
VÄNTADE_ORSAKER = {"omstart från koden", "strömpåslag", "extern reset"}


def now_s() -> float:
    return time.monotonic()


# ─────────────────────────────────────────────────────────────────────────────
#  Övervakningen
# ─────────────────────────────────────────────────────────────────────────────
class Monitor:
    """Läser varje rad och håller reda på det sammanfattningen behöver."""

    def __init__(self) -> None:
        self.t0 = now_s()
        self.phase = "start"
        self.lines: list[tuple[float, str]] = []
        self.stats: list[dict] = []
        self.transitions: list[tuple[float, str, str, str]] = []
        self.modes = Counter()
        self.stat_modes = Counter()
        self.modes_by_phase: dict[str, set] = {}
        self.anomalies: list[dict] = []
        self.observations: list[dict] = []
        self.boots: list[dict] = []
        self.acks_ok = 0
        self.acks_fel: list[str] = []
        self.expected_fel = 0
        self.ack_heap: list[tuple[float, int]] = []
        self.last_stat = now_s()
        self.expect_reset_until = 0.0
        self.expected_resets = 0
        self.unexpected_resets = 0
        self.boot_seq = 0
        self.decoded: dict[int, str] = {}

    def rel(self) -> float:
        return now_s() - self.t0

    def set_phase(self, name: str) -> None:
        self.phase = name
        self.modes_by_phase.setdefault(name, set())

    def expect_reset(self, secs: float = 25) -> None:
        self.expect_reset_until = now_s() + secs
        self.last_stat = now_s() + secs      # tyst under omstarten är väntat

    def anomaly(self, kind: str, text: str, idx: int | None = None) -> None:
        self.anomalies.append({"t": self.rel(), "kind": kind, "text": text, "phase": self.phase,
                               "idx": len(self.lines) - 1 if idx is None else idx})
        print(f"  !!! {kind}: {text}", flush=True)

    def observe(self, kind: str, text: str) -> None:
        self.observations.append({"t": self.rel(), "kind": kind, "text": text,
                                  "phase": self.phase, "idx": len(self.lines) - 1})

    def feed(self, line: str) -> None:
        t = self.rel()
        self.lines.append((t, line))

        for kind, rx in FEL:
            if rx.search(line):
                self.anomaly(kind, line)
                if kind == "krasch" and line.startswith("Backtrace"):
                    self.decode(len(self.lines) - 1, line)
                break

        m = RST.search(line)
        if m:
            self.boot_seq += 1
            expected = now_s() < self.expect_reset_until
            self.boots.append({"t": t, "rst": m.group(2), "expected": expected, "reason": None,
                               "phase": self.phase})
            if expected:
                self.expected_resets += 1
            else:
                self.unexpected_resets += 1
                self.anomaly("omstart", f"oväntad omstart: {line}")
            self.last_stat = now_s() + 20
            return

        m = BOOT.search(line)
        if m:
            reason = m.group(1).strip()
            if self.boots and self.boots[-1]["reason"] is None:
                self.boots[-1]["reason"] = reason
            else:
                # rst-raden kan ha gått förlorad i bruset — räkna omstarten här.
                expected = now_s() < self.expect_reset_until
                self.boots.append({"t": t, "rst": "?", "expected": expected, "reason": reason,
                                   "phase": self.phase})
                if expected:
                    self.expected_resets += 1
                else:
                    self.unexpected_resets += 1
                    self.anomaly("omstart", f"oväntad omstart: {line}")
            if reason not in VÄNTADE_ORSAKER:
                self.anomaly("omstartsorsak", line)
            return

        m = STAT.search(line)
        if m:
            self.last_stat = now_s()
            self.stats.append({"t": t, "clock": m.group(1), "mode": m.group(2),
                               "q": int(m.group(3)), "heap": int(m.group(4)),
                               "min": int(m.group(5)), "ledstack": int(m.group(6)),
                               "phase": self.phase, "boot": self.boot_seq})
            self.stat_modes[m.group(2)] += 1
            return

        m = LED.search(line)
        if m:
            a, b = m.group(1), m.group(2)
            self.transitions.append((t, a, b, self.phase))
            self.modes[b] += 1
            self.modes_by_phase.setdefault(self.phase, set()).add(b)
            return

        m = ACK.search(line)
        if m:
            if m.group(1) == "ok":
                self.acks_ok += 1
                h = re.search(r"heap=(\d+)", m.group(2))
                if h:
                    self.ack_heap.append((t, int(h.group(1))))
            else:
                self.acks_fel.append(line)

    def decode(self, idx: int, line: str) -> None:
        addrs = ["0x" + a for a in BT_ADDR.findall(line)]
        if not addrs or not ELF.exists() or not ADDR2LINE.exists():
            return
        try:
            out = subprocess.run([str(ADDR2LINE), "-pfiaC", "-e", str(ELF), *addrs],
                                 capture_output=True, text=True, timeout=20).stdout
        except Exception as exc:  # noqa: BLE001
            out = f"(addr2line misslyckades: {exc})"
        self.decoded[idx] = out.strip()
        print(out, flush=True)

    def context(self, idx: int, before: int = 15, after: int = 15) -> list[str]:
        lo, hi = max(0, idx - before), min(len(self.lines), idx + after + 1)
        return [f"{self.lines[i][0]:8.1f}  {self.lines[i][1]}" for i in range(lo, hi)]


# ─────────────────────────────────────────────────────────────────────────────
#  Seriekabeln
# ─────────────────────────────────────────────────────────────────────────────
class Link:
    """Porten, en läsartråd, och väntan på rader som matchar ett mönster."""

    def __init__(self, port: str, logdir: Path, mon: Monitor) -> None:
        self.port = port
        self.mon = mon
        self.raw = open(logdir / "rå.log", "w", encoding="utf-8")
        self.cond = threading.Condition()
        self.wlock = threading.Lock()
        self.s: serial.Serial | None = None
        self.stop = threading.Event()
        self.opened = 0.0
        self.noise = 0
        self.thread: threading.Thread | None = None

    def open(self) -> None:
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = self.port, 115200, 0.1
        s.dtr = False
        s.rts = False
        s.open()
        self.s = s
        self.opened = now_s()
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def pulse_reset(self, why: str) -> None:
        self.mon.expect_reset(30)
        self._log(f"### RTS-puls: {why}")
        with self.wlock:
            self.s.dtr = False
            self.s.rts = True
            time.sleep(0.15)
            self.s.rts = False

    def close(self) -> None:
        self.stop.set()
        if self.thread:
            self.thread.join(2)
        if self.s:
            self.s.close()
        self.raw.close()

    def _log(self, text: str) -> None:
        stamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
        self.raw.write(f"{stamp} {self.mon.rel():8.1f}  {text}\n")
        self.raw.flush()

    def write(self, data: str, show: bool = True) -> None:
        if show:
            shown = data if len(data) < 120 else data[:100] + f"… ({len(data)} byte)"
            self._log(f">>> {shown.rstrip()!r}")
        with self.wlock:
            self.s.write(data.encode("utf-8"))

    def mark(self) -> int:
        with self.cond:
            return len(self.mon.lines)

    def wait_for(self, pattern: str, timeout: float, since: int | None = None) -> str | None:
        rx = re.compile(pattern)
        deadline = now_s() + timeout
        with self.cond:
            i = len(self.mon.lines) if since is None else since
            while True:
                while i < len(self.mon.lines):
                    if rx.search(self.mon.lines[i][1]):
                        return self.mon.lines[i][1]
                    i += 1
                left = deadline - now_s()
                if left <= 0:
                    return None
                self.cond.wait(min(left, 0.5))

    def _reader(self) -> None:
        buf = b""
        last, rep = None, 0
        while not self.stop.is_set():
            try:
                chunk = self.s.read(4096)
            except (serial.SerialException, OSError) as exc:
                self._log(f"### läsfel: {exc}")
                time.sleep(0.5)
                continue
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").rstrip("\r").rstrip()
                settling = now_s() - self.opened < SETTLE_S
                if line == last:
                    # Loggen skriver upprepningen en gång med antal. Övervakningen
                    # får den ändå — två likadana kvittenser i rad är två pushar —
                    # utom under bruset efter öppningen, där den är drivrutinens.
                    rep += 1
                    if not settling:
                        with self.cond:
                            self.mon.feed(line)
                            self.cond.notify_all()
                    continue
                # Avhuggna upprepningar av förra raden under bruset är inte lampans.
                if settling and last and line and (last.endswith(line) or last.startswith(line)):
                    self.noise += 1
                    continue
                if rep:
                    self._log(f"          (… ×{rep + 1})")
                rep, last = 0, line
                if not line:
                    continue
                self._log(line)
                with self.cond:
                    self.mon.feed(line)
                    self.cond.notify_all()


# ─────────────────────────────────────────────────────────────────────────────
#  Drivningen
# ─────────────────────────────────────────────────────────────────────────────
class Driver:
    def __init__(self, link: Link, mon: Monitor, args) -> None:
        self.link = link
        self.mon = mon
        self.args = args
        self.rng = random.Random(args.frö)
        self.pushes = 0
        self.ack_missing = 0
        self.cmd_missing = Counter()
        self.phases: list[tuple[str, float, float]] = []
        self.next: dict = {}
        self.watch_stop = threading.Event()
        self.restarts_requested = 0

    # ── grunder ──────────────────────────────────────────────────────────────
    def log(self, text: str) -> None:
        print(f"[{self.mon.rel():7.1f}] {text}", flush=True)
        self.link._log(f"### {text}")

    def phase(self, name: str):
        driver = self

        class _P:
            def __enter__(self_):
                driver.mon.set_phase(name)
                self_.t = driver.mon.rel()
                driver.log(f"── fas: {name}")

            def __exit__(self_, *exc):
                driver.phases.append((name, self_.t, driver.mon.rel()))
                return False
        return _P()

    def sleep(self, s: float) -> None:
        time.sleep(max(0.0, s))

    def cmd(self, c: str, expect: str | None, timeout: float = 3.0) -> bool:
        """Ett seriekommando. Väntar in svaret, så att en push efteråt inte
        slängs som resten av raden."""
        idx = self.link.mark()
        self.link.write(c)
        if expect is None:
            self.sleep(0.3)
            return True
        if self.link.wait_for(expect, timeout, idx):
            return True
        self.cmd_missing[c] += 1
        return False

    def push(self, body: dict) -> bool:
        line = json.dumps(body, ensure_ascii=False, separators=(",", ":")) + "\n"
        idx = self.link.mark()
        self.link.write(line)
        self.pushes += 1
        r = self.link.wait_for(r"^\[push\] (ok|fel)", KVITTO_S, idx)
        if r is None:
            self.ack_missing += 1
            self.mon.observe("kvitto", f"ingen kvittens på push nr {self.pushes} inom {KVITTO_S} s")
            self.log(f"ingen kvittens på push nr {self.pushes}")
            return False
        if r.startswith("[push] fel"):
            self.mon.anomaly("push", f"lampan avvisade en giltig push: {r}")
            return False
        return True

    def wait_online(self, since: int, timeout: float = 90) -> bool:
        r = self.link.wait_for(r"^\[portal\] Statussida|^\[wifi\] tappade|^\[wifi\] .*portalen",
                               timeout, since)
        if r and r.startswith("[portal]"):
            # Första hämtningen efter start går i samma varv — låt den gå klart.
            self.link.wait_for(r"^\[shl\] nästa|^\[shl\] schema|^\[shl\] ingen kontakt", 25)
            return True
        self.mon.anomaly("uppkoppling", f"lampan kom inte online inom {timeout:.0f} s: {r}")
        return False

    def restart(self, why: str) -> None:
        self.log(f"omstart: {why}")
        self.restarts_requested += 1
        self.mon.expect_reset(40)
        idx = self.link.mark()
        self.link.write("r")
        if not self.link.wait_for(r"^\[boot\] föregående omstart", 20, idx):
            self.mon.anomaly("omstart", "r gav ingen ny uppstart inom 20 s")
        self.wait_online(idx)

    # ── vakten ───────────────────────────────────────────────────────────────
    def watchdog(self) -> None:
        """Tystnad = häng. Frågar först loopen, pulsar sedan RTS."""
        while not self.watch_stop.wait(2):
            quiet = now_s() - self.mon.last_stat
            if quiet < STILLA_S:
                continue
            self.mon.anomaly("tystnad", f"ingen [stat] på {quiet:.0f} s — troligt häng")
            idx = self.link.mark()
            self.link.write("i")
            alive = self.link.wait_for(r"^\[cmd\] v", 5, idx)
            self.mon.anomaly("tystnad", "loopen svarar på i" if alive else "loopen svarar inte på i")
            self.link.pulse_reset("tystnad")
            self.link.wait_for(r"^\[boot\] föregående omstart", 15)

    # ── missbruk ─────────────────────────────────────────────────────────────
    def goal_spam(self, n: int = 12) -> None:
        self.log(f"målspam ×{n}")
        for _ in range(n):
            c = self.rng.choice("gggb")
            self.link.write(c)
            self.sleep(self.rng.uniform(0.05, 0.15))
        self.link.wait_for(r"^\[demo\] (MÅL|AVGÖRANDE)", 3)
        self.sleep(0.5)

    def sigh_spam(self, n: int = 10) -> None:
        self.log(f"suckspam ×{n}")
        for _ in range(n):
            self.link.write("s")
            self.sleep(self.rng.uniform(0.05, 0.2))
        self.sleep(0.5)

    def queue_flood(self) -> None:
        n = self.rng.choice([6, 9, 14])
        ms = self.rng.choice([1500, 3000, 6000])
        self.log(f"köflod: {n} mål bakom {ms} ms")
        idx = self.link.mark()
        self.link.write(f"@kö {n} {ms}\n")
        if not self.link.wait_for(r"^\[diag\] (ok|fel)", 5, idx):
            self.cmd_missing["@kö"] += 1

    def net_test(self, during_goal: bool = False) -> None:
        if during_goal:
            self.cmd("b", r"^\[demo\] AVGÖRANDE")
            self.sleep(1.0)
        self.log("nättest" + (" mitt i fyrverkeri" if during_goal else ""))
        idx = self.link.mark()
        self.link.write("n")
        if not self.link.wait_for(r"^\[net\] ─{6,}$", 45, idx + 1):
            self.cmd_missing["n"] += 1
            self.log("nättestet blev inte klart inom 45 s")

    def bad_lines(self) -> None:
        """Trasig JSON och en för lång rad ska ge fel, inte krasch."""
        self.log("trasiga rader")
        for data, what in (("{trasig json\n", "json"), ("{" + "x" * 9000 + "}\n", "för lång")):
            idx = self.link.mark()
            self.link.write(data)
            r = self.link.wait_for(r"^\[push\] (ok|fel)", 10, idx)
            if r and r.startswith("[push] fel"):
                self.mon.expected_fel += 1
            else:
                self.mon.anomaly("krok", f"{what} rad gav {r!r}, väntade [push] fel")

    def demo_cycle(self, secs: float) -> None:
        """Demolägets alla tangenter i 100–300 ms takt, med mål emellan —
        lockMode/unlockMode under låset medan fyrverkeriet brinner."""
        keys = "1234567890vhlupoc"
        end = now_s() + secs
        sent = 0
        while now_s() < end:
            r = self.rng.random()
            if r < 0.12:
                self.link.write(self.rng.choice("gbs"))
            elif r < 0.17:
                # x mitt i fyrverkeriet: demolåset släpps medan GOAL ritas.
                self.link.write("b")
                self.sleep(self.rng.uniform(0.3, 1.5))
                self.link.write("x")
            else:
                self.link.write(self.rng.choice(keys))
            sent += 1
            self.sleep(self.rng.uniform(0.1, 0.3))
        self.cmd("x", None)
        self.log(f"demo: {sent} tangenter")

    # ── uppspelning ──────────────────────────────────────────────────────────
    def batches(self, timeline: list) -> list[tuple[str, list]]:
        out = []
        i = 0
        while i < len(timeline):
            t = timeline[i][0]
            frames, size = [], 0
            j = i
            while j < len(timeline):
                dt = (datetime.fromisoformat(timeline[j][0]) - datetime.fromisoformat(t)).total_seconds()
                fsz = len(json.dumps(timeline[j][1], ensure_ascii=False))
                if j > i and (dt >= BATCH_S or size + fsz > RAD_MAX):
                    break
                frames += timeline[j][1]
                size += fsz
                j += 1
            out.append((t, frames))
            i = j
        return out

    def replay(self, rec: Path, speed: float, abuse: list[str], abuse_every: float,
               restart_frac: float | None = None, hold_s: float = 55) -> None:
        head, rows = su.load(rec)
        poll = any(r["src"] == "poll" for r in rows)
        _, timeline = su.plan(rows, "sse1", poll, None, None)
        home, away = head.get("home") or "HEMMA", head.get("away") or "BORTA"
        self.next = {"home": home, "away": away, "homeIsUs": head.get("us") == "home",
                     "text": f"{home} – {away}  (stresstest {head.get('recorded', '')})"}
        bs = self.batches(timeline)
        span = (datetime.fromisoformat(bs[-1][0]) - datetime.fromisoformat(bs[0][0])).total_seconds()
        self.log(f"uppspelning {head.get('recorded')}: {len(timeline)} ramar i {len(bs)} pushar, "
                 f"{span / 60:.0f} min matchtid → {span / speed / 60:.1f} min i fart {speed:g}")
        self.push({"next": self.next, "live": True, "frames": []})

        t_first = datetime.fromisoformat(bs[0][0])
        wall0 = now_s()
        lag = 0.0
        next_abuse = now_s() + abuse_every
        ai = 0
        restarted = restart_frac is None
        for k, (t, frames) in enumerate(bs):
            rel = (datetime.fromisoformat(t) - t_first).total_seconds() / speed
            wait = wall0 + lag + rel - now_s()
            if wait > 0:
                time.sleep(wait)
            self.push({"next": self.next, "live": True, "frames": frames})

            if abuse and now_s() >= next_abuse:
                a0 = now_s()
                self.abuse(abuse[ai % len(abuse)])
                ai += 1
                lag += now_s() - a0        # missbruket äter inte upp matchtid
                next_abuse = now_s() + abuse_every
            if not restarted and k >= len(bs) * restart_frac:
                restarted = True
                a0 = now_s()
                self.restart("mitt i uppspelningen")
                self.push({"next": self.next, "live": True, "frames": []})
                lag += now_s() - a0

        # Tv-fördröjningen och segerdansen (45 s) ska hinna brinna klart.
        end = now_s() + hold_s
        while now_s() < end:
            self.sleep(min(15, end - now_s()))
            self.push({"next": self.next, "live": True, "frames": []})
        self.push({"next": self.next, "live": False, "frames": []})
        self.log(f"uppspelning klar, {self.pushes} pushar hittills")

    def abuse(self, what: str) -> None:
        if what == "mål":
            self.goal_spam()
        elif what == "suck":
            self.sigh_spam()
        elif what == "kö":
            self.queue_flood()
        elif what == "nät":
            self.net_test()
        elif what == "nät-mål":
            self.net_test(during_goal=True)
        elif what == "trasigt":
            self.bad_lines()
        elif what == "omstart-mål":
            self.cmd("b", r"^\[demo\] AVGÖRANDE")
            self.sleep(self.rng.uniform(0.5, 4))
            self.restart("mitt i fyrverkeriet")
            self.push({"next": self.next, "live": True, "frames": []})

    # ── körningar ────────────────────────────────────────────────────────────
    def start(self) -> None:
        with self.phase("uppstart"):
            self.mon.expect_reset(30)
            idx = self.link.mark()
            self.link.pulse_reset("start av körningen")
            self.wait_online(idx)
            self.link.wait_for(r"^\[stat\]", 35)
            # Demoläget kan stå kvar efter en tidigare körning om inget r kom
            # emellan — det gör det inte efter RTS, men säkert är säkert.
            self.cmd("x", None)

    def smoke(self) -> None:
        self.start()
        rec = self.args.inspelning[0]
        with self.phase("rök: uppspelning"):
            # Bara en bit av matchen: första 45 min matchtid räcker för en
            # period, mål och missbruk en gång var.
            head, rows = su.load(rec)
            self.replay_part(rec, rows, head, minutes=45)
        with self.phase("rök: demo"):
            self.demo_cycle(25)
            self.cmd("b", r"^\[demo\] AVGÖRANDE")
            self.sleep(1)
            self.cmd("x", None)
        with self.phase("rök: nät"):
            self.net_test(during_goal=True)
        with self.phase("rök: omstart"):
            self.cmd("7", r"^\[demo\] 7")
            self.cmd("b", r"^\[demo\] AVGÖRANDE")
            self.sleep(1.5)
            self.restart("mitt i fyrverkeriet i demoläget")
        with self.phase("rök: efter"):
            self.link.wait_for(r"^\[stat\]", 35)

    def replay_part(self, rec: Path, rows: list, head: dict, minutes: float) -> None:
        """Spelar början av en match — rökens korta variant av replay()."""
        # Från nedsläppet, första raden med matchklocka, och `minutes` framåt.
        start = next((r["t"] for r in rows if r.get("clock")), rows[0]["t"])
        stop = datetime.fromtimestamp(datetime.fromisoformat(start).timestamp() + minutes * 60
                                      ).isoformat(timespec="milliseconds")
        tmp = self.args.logdir / "rök.jsonl"
        with open(tmp, "w", encoding="utf-8") as fh:
            fh.write(json.dumps(head, ensure_ascii=False) + "\n")
            for r in rows:
                if start <= r["t"] <= stop:
                    fh.write(json.dumps(r, ensure_ascii=False) + "\n")
        self.replay(tmp, self.args.fart, ["mål", "kö", "suck", "trasigt"], 12, hold_s=20)

    def race(self, rounds: int) -> None:
        """Köade mål som tänds medan loopen står mellan sin Leds::mode()-koll
        och sitt Leds::setMode(). Rendertasken tänder målet ur kön; hinner
        loopen sedan skriva sitt läge över det är fyrverkeriet borta innan det
        syns. Varje köat mål ska följas av en övergång till GOAL."""
        self.start()
        for live in (False, True):
            with self.phase("kapplöpning " + ("live-push" if live else "standby")):
                if live:
                    self.next = {"home": "IFB", "away": "HV71", "homeIsUs": True, "text": "kapp"}
                    self.push({"next": self.next, "live": True, "frames": []})
                lost = 0
                for k in range(rounds):
                    ms = self.rng.randint(1500, 2500)
                    idx = self.link.mark()
                    self.link.write(f"@kö 1 {ms}\n")
                    fire = self.link.wait_for(r"^\[led\] köat mål tänds nu", ms / 1000 + 5, idx)
                    if not fire:
                        self.mon.anomaly("kö", "det köade målet tändes aldrig")
                        continue
                    i2 = self.link.mark() - 1
                    if not self.link.wait_for(r"^\[led\] \w+ → GOAL", 2.0, i2):
                        lost += 1
                        self.mon.anomaly("tappat mål",
                                         "köat mål tändes men listen gick aldrig till GOAL — "
                                         "loopens setMode() skrev över det")
                    # Vänta ut fyrverkeriet (vikt 0 = GOAL_MIN_MS) innan nästa.
                    self.link.wait_for(r"^\[led\] GOAL → ", 12, i2)
                    self.sleep(0.5)
                    if live and k % 5 == 4:
                        self.push({"next": self.next, "live": True, "frames": []})
                self.log(f"{lost} av {rounds} köade mål tappade")
                if live:
                    self.push({"next": self.next, "live": False, "frames": []})

    def full(self) -> None:
        self.start()
        budget = self.args.minuter * 60
        t_start = now_s()
        recs = list(self.args.inspelning)

        with self.phase("vila före"):
            self.link.wait_for(r"^\[stat\]", 35)

        with self.phase(f"match 1: {recs[0].name}"):
            self.replay(recs[0], self.args.fart,
                        ["mål", "kö", "nät", "suck", "nät-mål", "trasigt", "omstart-mål"],
                        20, restart_frac=0.6)

        with self.phase("demo 1"):
            self.demo_cycle(90)
            self.net_test()

        n = 1
        while now_s() - t_start < budget - 60:
            n += 1
            rec = recs[(n - 1) % len(recs)]
            left = budget - (now_s() - t_start)
            # Farten väljs så att matchen ryms i det som är kvar, med marginal
            # för missbruk och hållning — men aldrig långsammare än --fart.
            head, rows = su.load(rec)
            poll = any(r["src"] == "poll" for r in rows)
            _, tl = su.plan(rows, "sse1", poll, None, None)
            span = (datetime.fromisoformat(tl[-1][0]) - datetime.fromisoformat(tl[0][0])).total_seconds()
            speed = max(self.args.fart, span / max(60.0, left * 0.6))
            with self.phase(f"match {n}: {rec.name}"):
                self.replay(rec, speed, ["kö", "mål", "nät", "suck", "nät-mål", "omstart-mål"], 25,
                            restart_frac=0.5 if n % 2 == 0 else None)
            if now_s() - t_start >= budget - 60:
                break
            with self.phase(f"demo {n}"):
                self.demo_cycle(min(60, max(10, budget - (now_s() - t_start) - 60)))

        with self.phase("vila efter"):
            self.cmd("x", None)
            self.link.wait_for(r"^\[stat\]", 35)
            self.link.wait_for(r"^\[stat\]", 35, self.link.mark())


# ─────────────────────────────────────────────────────────────────────────────
#  Sammanfattningen
# ─────────────────────────────────────────────────────────────────────────────
def slope_per_min(pts: list[tuple[float, int]]) -> float | None:
    if len(pts) < 4:
        return None
    n = len(pts)
    mx = sum(p[0] for p in pts) / n
    my = sum(p[1] for p in pts) / n
    den = sum((p[0] - mx) ** 2 for p in pts)
    if den == 0:
        return None
    return sum((p[0] - mx) * (p[1] - my) for p in pts) / den * 60


def summarize(mon: Monitor, drv: Driver, link: Link, args, wall: float) -> tuple[str, bool]:
    out: list[str] = []
    w = out.append
    fatal = [a for a in mon.anomalies]
    ok = not fatal and mon.unexpected_resets == 0

    w(f"# Stresstest {args.läge} — {'GODKÄNT' if ok else 'UNDERKÄNT'}")
    w("")
    w(f"Tid: {wall / 60:.1f} min. Pushar: {drv.pushes} ({mon.acks_ok} kvitterade, "
      f"{drv.ack_missing} utan kvittens, {len(mon.acks_fel)} [push] fel varav "
      f"{mon.expected_fel} avsiktliga). Rader från lampan: {len(mon.lines)}, "
      f"CP2102-brus bortfiltrerat: {link.noise}.")
    w(f"Omstarter: {mon.expected_resets} väntade ({drv.restarts_requested} r + RTS-pulser), "
      f"{mon.unexpected_resets} oväntade.")
    if drv.cmd_missing:
        w("Kommandon utan svar (inget fel i sig — tecken som slängs som 'resten av raden' vid "
          "spam): " + ", ".join(f"{k}×{v}" for k, v in drv.cmd_missing.items()))
    w("")

    w("## Faser")
    for name, a, b in drv.phases:
        modes = ", ".join(sorted(mon.modes_by_phase.get(name, set()))) or "—"
        w(f"- {name}: {a / 60:.1f}–{b / 60:.1f} min ({(b - a) / 60:.1f} min) · lägen: {modes}")
    w("")
    w("## Lägen som setts ([led] A → B)")
    w(", ".join(f"{k} ×{v}" for k, v in mon.modes.most_common()))
    all_modes = {"BOOT", "PORTAL", "PORTAL_RETRY", "CONNECTING", "WORKING", "STANDBY", "LIVE",
                 "GOAL", "VICTORY", "UPDATING", "ERROR", "INTERMISSION", "OVERTIME", "DANCE"}
    w("Lägen på [stat]-raderna: " + ", ".join(f"{k} ×{v}" for k, v in mon.stat_modes.most_common()))
    missing = sorted(all_modes - set(mon.modes) - set(mon.stat_modes))
    if missing:
        w(f"Aldrig sedda som övergång: {', '.join(missing)}")
    w("")

    w("## Omstarter")
    for b in mon.boots:
        w(f"- {b['t'] / 60:6.1f} min  {b['rst']:<18} {b['reason'] or '?':<32} "
          f"{'väntad' if b['expected'] else 'OVÄNTAD'}  ({b['phase']})")
    w("")

    w("## Heap och LED-stack")
    if mon.stats:
        st = mon.stats
        w(f"Första [stat]: heap {st[0]['heap']}/{st[0]['min']} kB, ledstack {st[0]['ledstack']} B")
        w(f"Sista  [stat]: heap {st[-1]['heap']}/{st[-1]['min']} kB, ledstack {st[-1]['ledstack']} B")
        w(f"Lägsta fria heap: {min(s['heap'] for s in st)} kB · lägsta min-heap: "
          f"{min(s['min'] for s in st)} kB · lägsta ledstack: {min(s['ledstack'] for s in st)} B")
        w("")
        w("Per uppstart (bortser från första minuten, då TLS och NTP tar sitt):")
        w("")
        w("| uppstart | från–till min | [stat] | heap första/sista | min-heap första/sista | lutning B/min | ledstack min |")
        w("|---|---|---|---|---|---|---|")
        for boot in sorted({s["boot"] for s in st}):
            seg = [s for s in st if s["boot"] == boot]
            t0 = seg[0]["t"]
            core = [s for s in seg if s["t"] - t0 >= 60] or seg
            sl = slope_per_min([(s["t"], s["heap"] * 1024) for s in core])
            w(f"| {boot} | {seg[0]['t'] / 60:.1f}–{seg[-1]['t'] / 60:.1f} | {len(seg)} | "
              f"{seg[0]['heap']}/{seg[-1]['heap']} | {seg[0]['min']}/{seg[-1]['min']} | "
              f"{'—' if sl is None else f'{sl:+.0f}'} | {min(s['ledstack'] for s in seg)} |")
        w("")
        w("| min | klocka | fas | läge | kö | heap kB | min kB | ledstack B |")
        w("|---|---|---|---|---|---|---|---|")
        for s in st:
            w(f"| {s['t'] / 60:.1f} | {s['clock']} | {s['phase']} | {s['mode']} | {s['q']} | "
              f"{s['heap']} | {s['min']} | {s['ledstack']} |")
    if mon.ack_heap:
        pts = mon.ack_heap
        w("")
        w(f"Heap vid kvittenserna (byte, efter varje tillämpad push): första {pts[0][1]}, "
          f"sista {pts[-1][1]}, lägsta {min(p[1] for p in pts)}, högsta {max(p[1] for p in pts)}, "
          f"{len(pts)} mätpunkter.")
    w("")

    w("## Avvikelser")
    if not mon.anomalies:
        w("Inga.")
    for a in mon.anomalies:
        w(f"### {a['t'] / 60:.1f} min — {a['kind']} ({a['phase']})")
        w(a["text"])
        w("```")
        out.extend(mon.context(a["idx"]))
        w("```")
        if a["idx"] in mon.decoded:
            w("Avkodad backtrace:")
            w("```")
            w(mon.decoded[a["idx"]])
            w("```")
    w("")
    if mon.observations:
        w("## Iakttagelser (inte fel)")
        for o in mon.observations:
            w(f"- {o['t'] / 60:.1f} min ({o['phase']}) {o['kind']}: {o['text']}")
        w("")
    return "\n".join(out), ok


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("läge", choices=["rök", "full", "kapp", "lyssna"])
    ap.add_argument("--port", default=PORT)
    ap.add_argument("--inspelning", type=Path, action="append",
                    help="inspelningsmapp (flera går); standard: alla i mock/recordings, "
                         "IFB–HV71 först")
    ap.add_argument("--fart", type=float, default=40, help="uppspelningsfart (40)")
    ap.add_argument("--minuter", type=float, default=25, help="full: total längd (25)")
    ap.add_argument("--sekunder", type=float, default=120, help="lyssna: hur länge (120)")
    ap.add_argument("--varv", type=int, default=20, help="kapp: köade mål per läge (20)")
    ap.add_argument("--frö", type=int, default=1, help="slumpfrö för missbruket")
    ap.add_argument("--logg", type=Path,
                    default=Path("/private/tmp/claude-501/-Users-p950mbc-Development-playground-"
                                 "bj-rkl-ven-esp32/5c261510-5da0-47c7-a208-4d8c9c65cd5f/"
                                 "scratchpad/stress"))
    args = ap.parse_args()
    if not args.inspelning:
        recs = sorted((REPO / "mock" / "recordings").iterdir())
        recs = [r for r in recs if (r / "lampa.jsonl").exists()]
        recs.sort(key=lambda r: (0 if "HV71" in r.name else 1 if "OHK" in r.name else 2, r.name))
        args.inspelning = recs
    args.logdir = args.logg / f"{datetime.now():%Y%m%d-%H%M%S}-{args.läge}"
    args.logdir.mkdir(parents=True, exist_ok=True)
    print(f"Logg: {args.logdir}")

    mon = Monitor()
    link = Link(args.port, args.logdir, mon)
    drv = Driver(link, mon, args)
    link.open()
    watch = threading.Thread(target=drv.watchdog, daemon=True)
    t0 = now_s()
    try:
        if args.läge == "lyssna":
            mon.set_phase("lyssna")
            mon.last_stat = now_s()
            watch.start()
            time.sleep(args.sekunder)
        else:
            watch.start()
            if args.läge == "rök":
                drv.smoke()
            elif args.läge == "kapp":
                drv.race(args.varv)
            else:
                drv.full()
    except KeyboardInterrupt:
        print("\nAvbruten.")
    finally:
        drv.watch_stop.set()
        wall = now_s() - t0
        time.sleep(0.5)
        link.close()
    text, ok = summarize(mon, drv, link, args, wall)
    (args.logdir / "sammanfattning.md").write_text(text, encoding="utf-8")
    print()
    print(text)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
