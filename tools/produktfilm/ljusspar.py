#!/usr/bin/env python3
"""
Ljusspåret — vad var och en av listens dioder visar i varje bildruta.

En port av lägena i src/leds.cpp till Python, så att filmen visar samma ljus
som lampan och inte en tolkning av det. Samma konstanter som include/config.h,
samma formler, och simuleringen går i firmwarens 120 bilder/s och samplas ner
till filmens takt — annars skulle gnistornas och kometernas uttoning, som räknas
per bildruta, gå fyra gånger för långsamt.

Det som inte är exakt: FastLEDs inoise8 är ersatt med ett eget Perlin-brus av
samma karaktär, och slumpen är seedad så att filmen blir likadan varje gång.

    python3 tools/produktfilm/ljusspar.py            # skriv ljusspar.png
    python3 tools/produktfilm/ljusspar.py --ascii    # grovt i terminalen

ljusspar.png har en rad per filmruta och en kolumn per diod — en snabb koll
av hela tidslinjen utan att rendera något.

Inga beroenden utanför Pythons standardbibliotek; scen.py importerar samma
modul inifrån Blender.
"""

from __future__ import annotations

import math
import random
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import manus  # noqa: E402

# ── include/config.h ─────────────────────────────────────────────────────────
# Antalet dioder är en inställning i lampan (ledCount på statussidan).
# Firmwarens standard är 60 (LED_COUNT_DEFAULT), men de byggda lamporna har 30
# — och filmen ska se ut som dem. Alla lägen räknar mot N, precis som
# firmwaren räknar mot gCount.
N = 30
HZ = 120                    # FPS i leds.cpp
BRIGHTNESS = 160            # LED_DEFAULT_BRIGHTNESS, läggs på i scen.py
YELLOW_R, YELLOW_G = 255, 224
LIVE_YELLOW_G = 165
GLOW_FLOOR_VAL, GLOW_MIN_VAL, GLOW_MAX_VAL, GLOW_BPM = 15, 22, 140, 9
GLOW_LIVE_FLOOR_LIFT, GLOW_LIVE_LIFT = 8, 16
SPARKLE = (255, 250, 225)
SPARKLE_MEAN_INTERVAL_MS, SPARKLE_DECAY = 700, 14
VICTORY_TRAVEL_MS, VICTORY_VOLLEYS = 1800, 3
VICTORY_HEAD_VAL, VICTORY_TAIL_VAL = 170, 60
VICTORY_GLOW_MIN, VICTORY_GLOW_MAX, VICTORY_GLOW_BPM = 26, 90, 7
GOAL_STROBE_MIN_MS, GOAL_STROBE_MAX_MS = 1200, 4000
GOAL_VOLLEY_SLOW_MS, GOAL_VOLLEY_FAST_MS = 200, 60
GOAL_TEAM_COLORS_AT = 204
HEART_REST_BPM, HEART_MAX_BPM, MOOD_HEAT = 55, 140, 0.75
MOOD_THRESHOLD, MOOD_FADE = 0.20, 0.25
BOOT_FILL_MS, BOOT_EDGE_SOFTNESS = 700, 768
COMET_FADE_IN = 0.25
WIN_TRAVEL_MS, WIN_VOLLEYS, WIN_GLOW_MAX, WIN_GLOW_BPM = 5000, 3, 110, 5
WALKON_BLOCK, WALKON_SWAP_MS = 3, 2000
WALKON_LINE_MS, WALKON_LINE_HOLD_MS, WALKON_LINE_FADE_MS, WALKON_LINE_GAP_MS = 1500, 600, 500, 400


# ── FastLED-hjälp ────────────────────────────────────────────────────────────
def clamp(x, lo=0.0, hi=1.0):
    return lo if x < lo else hi if x > hi else x


def lerp(a, b, t):
    return a + (b - a) * t


def smooth(x):
    x = clamp(x)
    return x * x * (3 - 2 * x)


def gold(level, green=YELLOW_G):
    s = level / 255
    return [YELLOW_R * s, green * s, 0.0]


def level(c, v):
    s = clamp(v, 0, 255) / 255
    return [c[0] * s, c[1] * s, c[2] * s]


def add(a, b):
    return [min(255.0, a[0] + b[0]), min(255.0, a[1] + b[1]), min(255.0, a[2] + b[2])]


def blend(a, b, amount):
    t = amount / 255
    return [lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t)]


# lovGreen: CHSV(104, 255, v) genom FastLEDs rainbow-omvandling. Nyans 104
# ligger i avsnittet grönt → aqua: (0, 255 − 21, 21) i full styrka.
def lov_green(v):
    s = clamp(v, 0, 255) / 255
    return [0.0, 234.0 * s, 21.0 * s]


# cometLife: tonar in under första COMET_FADE_IN av resan, ut resten av vägen.
def comet_life(p):
    i = smooth(p / COMET_FADE_IN) if p < COMET_FADE_IN else 1.0
    return 255 * i * (1 - p)


def heat_color(h):
    h = clamp(h)
    g = lerp(200, 110, h * 2) if h < 0.5 else lerp(110, 22, (h - 0.5) * 2)
    return [255.0, g, 0.0]


class Brus:
    """Tvådimensionellt Perlin-brus, 0–255 som inoise8."""

    def __init__(self, seed=7):
        r = random.Random(seed)
        p = list(range(256))
        r.shuffle(p)
        self.p = p + p

    @staticmethod
    def _fade(t):
        return t * t * t * (t * (t * 6 - 15) + 10)

    @staticmethod
    def _grad(h, x, y):
        h &= 7
        u, v = (x, y) if h < 4 else (y, x)
        return (u if h & 1 == 0 else -u) + (2 * v if h & 2 == 0 else -2 * v)

    def __call__(self, x, y):
        # inoise8 tar koordinater i 1/256-delar av en cell.
        x /= 256
        y /= 256
        xi, yi = int(math.floor(x)) & 255, int(math.floor(y)) & 255
        xf, yf = x - math.floor(x), y - math.floor(y)
        u, v = self._fade(xf), self._fade(yf)
        p = self.p
        aa, ab = p[p[xi] + yi], p[p[xi] + yi + 1]
        ba, bb = p[p[xi + 1] + yi], p[p[xi + 1] + yi + 1]
        x1 = lerp(self._grad(aa, xf, yf), self._grad(ba, xf - 1, yf), u)
        x2 = lerp(self._grad(ab, xf, yf - 1), self._grad(bb, xf - 1, yf - 1), u)
        return clamp((lerp(x1, x2, v) * 0.5 + 0.5) * 255, 0, 255)


# ── Lampan ───────────────────────────────────────────────────────────────────
class Lampa:
    def __init__(self):
        self.leds = [[0.0, 0.0, 0.0] for _ in range(N)]
        self.sparkle = [0.0] * N
        self.rnd = random.Random(1987)
        self.brus = Brus()
        self.ms = 0
        self.mode_since = 0
        self.next_sparkle = 0
        self.heart_phase = 0.0

    # drawGlow
    def glow(self, min_val, max_val, bpm, green=YELLOW_G):
        # beatsin16: sinus med `bpm` slag per minut, sedan fasen i kvadrat.
        phase = (math.sin(2 * math.pi * self.ms / 60000 * bpm) + 1) / 2
        breath = min_val + phase * phase * (max_val - min_val)
        t = self.ms / 24
        out = []
        for i in range(N):
            n = self.brus(i * 26, t)
            val = breath + (n - 128) * breath / 700
            out.append(gold(clamp(val, GLOW_FLOOR_VAL, 255), green))
        return out

    # updateSparkles
    def sparkles(self, spawn=True):
        if spawn and self.ms >= self.next_sparkle:
            self.sparkle[self.rnd.randrange(N)] = 255
            self.next_sparkle = self.ms + self.rnd.randint(
                SPARKLE_MEAN_INTERVAL_MS // 2, SPARKLE_MEAN_INTERVAL_MS * 3 // 2)
        for i in range(N):
            if self.sparkle[i] <= 0:
                continue
            self.leds[i] = add(self.leds[i], level(SPARKLE, self.sparkle[i]))
            self.sparkle[i] = max(0, self.sparkle[i] - SPARKLE_DECAY)

    def spawn_sparkles(self, per_second, dt):
        if self.rnd.random() < clamp(per_second * dt, 0, 1):
            self.sparkle[self.rnd.randrange(N)] = 255

    # Släckt — mörkläggningen före målet.
    def draw_av(self, p, dt, t_in, t_len):
        self.leds = [[0.0, 0.0, 0.0] for _ in range(N)]

    # LED_BOOT: ljuset flödar in från listens början och blir stående. I
    # firmwaren tar anslutningen vid; i filmen tonar det i stället över i
    # standbyglöden under resten av läget.
    def draw_uppstart(self, p, dt, t_in, t_len):
        edge = clamp(t_in * 1000 / BOOT_FILL_MS) * (N * 256 + BOOT_EDGE_SOFTNESS)
        boot = []
        for i in range(N):
            d = edge - i * 256
            boot.append(gold(0 if d <= 0 else 255 if d >= BOOT_EDGE_SOFTNESS
                             else d * 255 / BOOT_EDGE_SOFTNESS))
        hall = p.get("hall_s", 0.3) + BOOT_FILL_MS / 1000
        if t_in < hall:
            self.leds = boot
            return
        glod = self.glow(GLOW_MIN_VAL, GLOW_MAX_VAL, GLOW_BPM)
        k = smooth((t_in - hall) / max(0.001, t_len - hall)) * 255
        self.leds = [blend(boot[i], glod[i], k) for i in range(N)]

    # LED_STANDBY
    def draw_glod(self, p, dt, t_in=0, t_len=0):
        self.leds = self.glow(GLOW_MIN_VAL, GLOW_MAX_VAL, GLOW_BPM)
        self.sparkles(spawn=False)

    # LED_LIVE utan slutspurt
    def draw_live(self, p, dt, t_in=0, t_len=0):
        self.leds = self.glow(GLOW_MIN_VAL + GLOW_LIVE_FLOOR_LIFT,
                              GLOW_MAX_VAL + GLOW_LIVE_LIFT, GLOW_BPM * 2, LIVE_YELLOW_G)
        self.sparkles(spawn=False)

    # LED_LIVE med drawHeart ovanpå, intensiteten stiger genom läget
    def draw_hjarta(self, p, dt, t_in, t_len):
        I = lerp(p.get("fran", 0.6), p.get("till", 1.0), clamp(t_in / t_len))
        base_glow = self.glow(GLOW_MIN_VAL + GLOW_LIVE_FLOOR_LIFT,
                              GLOW_MAX_VAL + GLOW_LIVE_LIFT, GLOW_BPM * 2, LIVE_YELLOW_G)
        self.heart_phase += lerp(HEART_REST_BPM, HEART_MAX_BPM, I) / 60 * dt
        self.heart_phase -= math.floor(self.heart_phase)
        col = heat_color(lerp(0.1, MOOD_HEAT, I))
        c = (N - 1) / 2
        base, peak = lerp(22, 34, I), lerp(110, 240, I)
        w = smooth((I - MOOD_THRESHOLD) / MOOD_FADE)
        for i in range(N):
            d = abs(i - c) / (N / 2)
            f = self.heart_phase - d * 0.15
            f -= math.floor(f)
            pulse = (math.exp(-((f - 0.03) / 0.035) ** 2)
                     + 0.55 * math.exp(-((f - 0.2) / 0.045) ** 2))
            fx = level(col, base + pulse * peak * (1 - 0.35 * d))
            self.leds[i] = blend(base_glow[i], fx, w * 255)
        self.sparkles(spawn=False)

    # LED_GOAL
    def draw_mal(self, p, dt, t_in, t_len):
        imp = p.get("vikt", 255)
        elapsed = t_in * 1000
        strobe = p.get("strobe_s", None)
        strobe = strobe * 1000 if strobe else (
            GOAL_STROBE_MIN_MS + (GOAL_STROBE_MAX_MS - GOAL_STROBE_MIN_MS) * imp / 255)
        volley = GOAL_VOLLEY_SLOW_MS - (GOAL_VOLLEY_SLOW_MS - GOAL_VOLLEY_FAST_MS) * imp / 255
        if elapsed < strobe:
            on = int(self.ms / 36) % 2
            white = int(self.ms / 72) % 2 == 0
            col = [255.0, 244.0, 210.0] if white else gold(255)
            self.leds = [list(col) if on else gold(7) for _ in range(N)]
            return
        self.leds = [[v * (1 - 48 / 256) for v in c] for c in self.leds]
        center = N // 2
        travel = int((elapsed % volley) * center / volley)
        green = imp >= GOAL_TEAM_COLORS_AT and int(elapsed / volley) % 2
        head = [170, 255, 170] if green else [255, 248, 220]
        tail1 = [0, 190, 20] if green else gold(190)
        tail2 = [0, 32, 4] if green else gold(32)
        for d in (-1, 1):
            pos = center + d * travel
            if not 0 <= pos < N:
                continue
            self.leds[pos] = list(map(float, head))
            if 0 <= pos - d < N:
                self.leds[pos - d] = add(self.leds[pos - d], tail1)
            if 0 <= pos - 2 * d < N:
                self.leds[pos - 2 * d] = add(self.leds[pos - 2 * d], tail2)
        for _ in range(3):
            if self.rnd.random() < 90 / 256:
                j = self.rnd.randrange(N)
                self.leds[j] = add(self.leds[j], gold(self.rnd.randint(101, 254)))

    # LED_VICTORY
    def victory(self):
        self.leds = self.glow(VICTORY_GLOW_MIN, VICTORY_GLOW_MAX, VICTORY_GLOW_BPM, LIVE_YELLOW_G)
        center = N // 2
        for v in range(VICTORY_VOLLEYS):
            phase = (self.ms + v * VICTORY_TRAVEL_MS / VICTORY_VOLLEYS) % VICTORY_TRAVEL_MS
            travel = int(phase * center / VICTORY_TRAVEL_MS)
            life = comet_life(phase / VICTORY_TRAVEL_MS)
            for d in (-1, 1):
                pos = center + d * travel
                if not 0 <= pos < N:
                    continue
                self.leds[pos] = add(self.leds[pos], gold(VICTORY_HEAD_VAL * life / 255, LIVE_YELLOW_G))
                if 0 <= pos - d < N:
                    self.leds[pos - d] = add(self.leds[pos - d],
                                             gold(VICTORY_TAIL_VAL * life / 255, LIVE_YELLOW_G))
        self.sparkles(spawn=True)

    def draw_seger(self, p, dt, t_in, t_len):
        self.victory()

    # LED_STANDBY med vann-senast-flaggan: drawWinComets. Gröna kometer över en
    # grön glöd, utan gnistor — läget står fram till nästa match.
    def draw_vann(self, p, dt, t_in, t_len):
        bad = self.glow(VICTORY_GLOW_MIN, WIN_GLOW_MAX, WIN_GLOW_BPM, LIVE_YELLOW_G)
        self.leds = [lov_green(c[0]) for c in bad]
        c = (N - 1) / 2
        for v in range(WIN_VOLLEYS):
            ph = ((self.ms + v * WIN_TRAVEL_MS // WIN_VOLLEYS) % WIN_TRAVEL_MS) / WIN_TRAVEL_MS
            life = comet_life(ph)
            for d in (-1, 1):
                pos = c + d * ph * (c + 1)
                self.splat(pos, level([110, 255, 90], life), 0.7)
                self.splat(pos - d * 1.5, lov_green(230 * life / 255), 1.0)
                self.splat(pos - d * 3.2, lov_green(90 * life / 255), 1.2)

    # addSplat: mjuk ljusfläck kring en position mellan dioderna, adderad.
    def splat(self, pos, col, radius):
        for i in range(N):
            g = math.exp(-((i - pos) / radius) ** 2)
            if g > 0.01:
                self.leds[i] = add(self.leds[i], level(col, g * 255))

    # LED_WALKON_BLOCKS: grönt och gult i block om tre dioder, som alla byter
    # färg samtidigt. Hårda byten, som sargen i arenan.
    def draw_intag_block(self, p, dt, t_in, t_len):
        flip = int(t_in * 1000 / WALKON_SWAP_MS) & 1
        self.leds = [lov_green(200) if ((i // WALKON_BLOCK) & 1) ^ flip else gold(200, 190)
                     for i in range(N)]

    # LED_WALKON_LINE: grönt växer ut från mitten med en ljus front, står,
    # tonar ut och börjar om efter en kort mörk paus.
    def draw_intag_linje(self, p, dt, t_in, t_len):
        cykel = WALKON_LINE_MS + WALKON_LINE_HOLD_MS + WALKON_LINE_FADE_MS + WALKON_LINE_GAP_MS
        t = (t_in * 1000) % cykel
        c = (N - 1) / 2
        reach = smooth(t / WALKON_LINE_MS) * (c + 1.5) if t < WALKON_LINE_MS else c + 1.5
        fade = 1.0
        if t >= WALKON_LINE_MS + WALKON_LINE_HOLD_MS:
            fade = 1 - clamp((t - WALKON_LINE_MS - WALKON_LINE_HOLD_MS) / WALKON_LINE_FADE_MS)
        for i in range(N):
            d = reach - abs(i - c)
            v = clamp(d)
            self.leds[i] = lov_green(200 * v * fade)
            if t < WALKON_LINE_MS and -1 < d < 1.5:
                self.leds[i] = add(self.leds[i], level(
                    [150, 255, 130], 255 * (1 - abs(d - 0.25) / 1.25) * v))

    # LED_DANCE — i filmen tonar den över i segerläget under sista 2 s i
    # stället för firmwarens 5, eftersom dansen är kortad.
    def draw_dans(self, p, dt, t_in, t_len):
        age = t_in * 1000
        c = (N - 1) / 2
        green = hsv_green()
        fx = []
        for i in range(N):
            ph = math.floor((abs(i - c) - age / 1000 * 12) / 4)
            fx.append(gold(170, 190) if ph & 1 else list(green))
        self.spawn_sparkles(20, dt)
        blend_from = (t_len - 2) * 1000
        if age < blend_from:
            self.leds = fx
            self.sparkles(spawn=True)
            return
        self.victory()
        keep = 255 - clamp((age - blend_from) / 2000 * 255, 0, 255)
        self.leds = [blend(self.leds[i], fx[i], keep) for i in range(N)]


def hsv_green():
    # CHSV(104, 255, 170), segerdansens grönt.
    return lov_green(170)


def rakna_ut():
    """Alla filmrutor: lista[ruta][diod] = [r, g, b] i 0–255, före ljusstyrka."""
    lampa = Lampa()
    ljus = manus.LJUS
    rutor = []
    steg_per_ruta = HZ // manus.FPS
    dt = 1 / HZ
    total_steg = manus.antal_rutor() * steg_per_ruta
    for steg in range(total_steg):
        t = steg / HZ
        lampa.ms = steg * 1000 // HZ
        k = max(i for i, (s, _, _) in enumerate(ljus) if s <= t)
        start, lage, p = ljus[k]
        slut = ljus[k + 1][0] if k + 1 < len(ljus) else manus.LANGD_S
        getattr(lampa, "draw_" + lage)(p, dt, t - start, slut - start)
        if steg % steg_per_ruta == 0:
            rutor.append([list(c) for c in lampa.leds])
    return rutor


def skriv_png(rutor, sokvag, skala=6):
    """En rad per filmruta, en kolumn per diod (förstorad `skala` gånger)."""
    b = BRIGHTNESS / 255
    rader = []
    for r in rutor:
        rad = bytearray([0])
        for c in r:
            px = bytes(int(clamp(v * b / 255) ** (1 / 2.2) * 255) for v in c)
            rad += px * skala
        rader.append(bytes(rad))
    w, h = N * skala, len(rutor)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(rader), 9)) + chunk(b"IEND", b"")
    Path(sokvag).write_bytes(png)


if __name__ == "__main__":
    rutor = rakna_ut()
    if "--ascii" in sys.argv:
        tecken = " .:-=+*#%@"
        for i, r in enumerate(rutor[:: manus.FPS // 3]):
            rad = "".join(tecken[min(9, int(sum(c) / 765 * 10))] for c in r)
            print(f"{i / 3:5.1f}s |{rad}|")
    else:
        ut = Path(__file__).resolve().parent / "ut" / "ljusspar.png"
        ut.parent.mkdir(exist_ok=True)
        skriv_png(rutor, ut)
        print(f"{len(rutor)} rutor → {ut}")
