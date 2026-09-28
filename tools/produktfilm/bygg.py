#!/usr/bin/env python3
"""
Produktfilmen för LövGlöd — från STL-filerna i hardware/v2 till en färdig mp4.

    python3 tools/produktfilm/bygg.py                  # utkast, 960×540
    python3 tools/produktfilm/bygg.py --kvalitet film  # 1080p, ~8 s/ruta
    python3 tools/produktfilm/bygg.py --kvalitet final # 1080p, ~25 s/ruta
    python3 tools/produktfilm/bygg.py --stillbild 70,213,850
    python3 tools/produktfilm/bygg.py --bara-klipp     # bara titlar och kodning
    python3 tools/produktfilm/bygg.py --om             # släng gamla rutor först

Tre steg:

  1. ljusspar.py räknar fram vad listens dioder visar i varje ruta — samma
     lägen och konstanter som firmwaren.
  2. scen.py sätter ihop lampan i Blender, lägger ljusspåret i det gula
     fönstret och renderar varje tagning i manus.py med Cycles.
  3. ffmpeg lägger på titlarna och tonar in och ut.

Rutor som redan finns renderas inte om, så ett avbrutet bygge fortsätter där
det slutade. Har manus.py eller scen.py ändrats sedan rutorna gjordes: kör med
--om, annars blandas gamla och nya rutor.

Kräver Blender (4.2 eller senare; utvecklat mot 5.2) och ffmpeg med
libfreetype. På macOS: brew install --cask blender && brew install ffmpeg.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

HAR = Path(__file__).resolve().parent
sys.path.insert(0, str(HAR))
import manus  # noqa: E402

UT = HAR / "ut"
TYPSNITT = HAR / "typsnitt"
LOGGA = HAR / "logga.png"

# Titlarnas stil. Storlek och läge i andelar av bildens höjd, så att samma
# manus fungerar i alla kvaliteter.
# Texten står i det tomma svarta till vänster om lampan, i samma storlek och
# på samma höjd oavsett om det är namnet eller en rad — manuset lägger lampan
# i högra tredjedelen när det finns text.
STILAR = {
    "under": dict(typsnitt="Barlow-Regular.ttf", storlek=0.030, x="w*0.08+h*0.004", y=0.53, tona=0.6),
    "rad":   dict(typsnitt="Barlow-SemiBold.ttf", storlek=0.075, x="w*0.08", y=0.41, tona=0.45),
    # Målet: större och i guld, och snabbt in så att det slår till med strobet.
    "mal":   dict(typsnitt="Barlow-SemiBold.ttf", storlek=0.11, x="w*0.08", y=0.39, tona=0.15,
                  farg="0xf8d12b"),
}

# Logotypen, stilen "logga": logga.png är site/assets/logo-dark.svg — vitt Löv,
# gult Glöd och gult streck under — renderad med 8 px per SVG-enhet, beskuren
# till viewBox 0 −104 314,45 125. Versalerna går från y = −86 till 0, alltså
# rad 144 till 832 i bilden. Här sätts versalhöjden och var versalerna börjar,
# i andelar av bildens höjd, så att strecket hamnar ovanför "under"-raden.
LOGGA_VERSAL = 0.07
LOGGA_TOPP = 0.415
LOGGA_TONA = 0.6


def blender() -> str:
    for kandidat in (shutil.which("blender"), "/Applications/Blender.app/Contents/MacOS/Blender"):
        if kandidat and Path(kandidat).exists():
            return kandidat
    sys.exit("Hittar inte Blender. brew install --cask blender")


def rendera(kvalitet: str, rutor_dir: Path, stillbild: str | None):
    cmd = [blender(), "-b", "--factory-startup", "-P", str(HAR / "scen.py"), "--",
           "--kvalitet", kvalitet, "--ut", str(rutor_dir)]
    if stillbild:
        cmd += ["--stillbild", stillbild]
    print("[bygg] renderar", kvalitet, "→", rutor_dir)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for rad in proc.stdout:
        # Blender pratar mycket; visa bara färdiga rutor och fel.
        if "Saved:" in rad or "Error" in rad or "Traceback" in rad or rad.startswith("[scen]"):
            print("  ", rad.rstrip(), flush=True)
    if proc.wait():
        sys.exit(f"Blender avslutade med {proc.returncode}")


def alfa(a: float, b: float, f: float) -> str:
    """Toning in och ut för drawtext, som uttryck i t."""
    return (f"if(lt(t,{a}),0,if(lt(t,{a + f}),(t-{a})/{f},"
            f"if(lt(t,{b - f}),1,if(lt(t,{b}),({b}-t)/{f},0))))")


def klipp(rutor_dir: Path, ut: Path):
    antal = len(list(rutor_dir.glob("[0-9][0-9][0-9][0-9].png")))
    if antal < manus.antal_rutor():
        print(f"[bygg] obs: {antal} av {manus.antal_rutor()} rutor finns — klipper det som finns")

    textdir = UT / "texter"
    textdir.mkdir(parents=True, exist_ok=True)
    langd = manus.LANGD_S
    with open(rutor_dir / "0001.png", "rb") as f:
        huvud = f.read(24)
    bild_h = int.from_bytes(huvud[20:24], "big")

    # Logotypen läggs på som bild, en gång per gång den syns, före texterna.
    loggor = [(a, b) for a, b, _, stil in manus.TEXTER if stil == "logga"]
    graf = []
    forra = "[0:v]"
    if loggor:
        logga_h = round(bild_h * LOGGA_VERSAL * 1000 / (832 - 144))
        logga_y = round(bild_h * LOGGA_TOPP - logga_h * 144 / 1000)
        graf.append(f"[2:v]scale=-1:{logga_h},format=rgba,split={len(loggor)}"
                    + "".join(f"[lg{i}]" for i in range(len(loggor))))
        for i, (a, b) in enumerate(loggor):
            graf.append(
                f"[lg{i}]fade=t=in:st={a}:d={LOGGA_TONA}:alpha=1,"
                f"fade=t=out:st={b - LOGGA_TONA}:d={LOGGA_TONA}:alpha=1[lgt{i}]")
            graf.append(f"{forra}[lgt{i}]overlay=x=W*0.08:y={logga_y}"
                        f":enable='between(t,{a},{b})'[v{i}]")
            forra = f"[v{i}]"

    filter_ = []
    for i, (a, b, text, stil) in enumerate(manus.TEXTER):
        if stil == "logga":
            continue
        s = STILAR[stil]
        # Texten går via fil, så att å, ä, ö och skiljetecken slipper escapas.
        tf = textdir / f"{i}.txt"
        tf.write_text(text, encoding="utf-8")
        filter_.append(
            f"drawtext=fontfile='{TYPSNITT / s['typsnitt']}':textfile='{tf}'"
            f":fontsize=h*{s['storlek']}:fontcolor={s.get('farg', 'white')}"
            # Skuggan håller texten läsbar där den hamnar över lövets vita
            # bokstäver eller ett tänt fönster.
            f":shadowcolor=black@0.55:shadowx=0:shadowy=2"
            f":x={s['x']}:y=h*{s['y']}"
            f":alpha='{alfa(a, b, s['tona'])}':enable='between(t,{a},{b})'")
    filter_ += [
        f"fade=t=in:st=0:d={manus.TONA_IN_S}",
        f"fade=t=out:st={langd - manus.TONA_UT_S}:d={manus.TONA_UT_S}",
        "format=yuv420p",
    ]
    cmd = ["ffmpeg", "-loglevel", "error", "-stats", "-y",
           "-framerate", str(manus.FPS), "-start_number", "1",
           "-i", str(rutor_dir / "%04d.png")]
    ljud = []
    m = getattr(manus, "MUSIK", None)
    if m:
        # Ett utsnitt av låten, lika långt som filmen. Den tonar in med bilden
        # och ut lite längre än bilden, så att den inte klipps av mitt i ett slag.
        cmd += ["-ss", str(m["start_s"]), "-t", str(langd), "-i", str(HAR / m["fil"])]
        ljud = ["-af", f"afade=t=in:st=0:d={manus.TONA_IN_S},"
                       f"afade=t=out:st={langd - m['tona_ut_s']}:d={m['tona_ut_s']}",
                "-map", "1:a", "-c:a", "aac", "-b:a", "256k", "-shortest"]
    else:
        # Logotypen ska vara ingång 2 även utan musik.
        cmd += ["-f", "lavfi", "-t", str(langd), "-i", "anullsrc"]
    cmd += ["-framerate", str(manus.FPS), "-loop", "1", "-t", str(langd), "-i", str(LOGGA)]
    graf.append(f"{forra}{','.join(filter_)}[ut]")
    cmd += ["-filter_complex", ";".join(graf), "-map", "[ut]", *ljud,
           "-c:v", "libx264", "-preset", "slow", "-crf", "16",
           "-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace", "bt709",
           "-movflags", "+faststart", str(ut)]
    print("[bygg] klipper →", ut)
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--kvalitet", default="utkast", choices=["utkast", "film", "final"])
    ap.add_argument("--stillbild", help="bara de här rutorna, t.ex. 70,213,850")
    ap.add_argument("--bara-klipp", action="store_true", help="rendera inget, klipp det som finns")
    ap.add_argument("--om", action="store_true", help="släng tidigare rutor i den här kvaliteten")
    a = ap.parse_args()

    rutor_dir = UT / ("stillbilder" if a.stillbild else f"rutor-{a.kvalitet}")
    if a.om and rutor_dir.exists():
        shutil.rmtree(rutor_dir)
    rutor_dir.mkdir(parents=True, exist_ok=True)

    if not a.bara_klipp:
        rendera(a.kvalitet, rutor_dir, a.stillbild)
    if not a.stillbild:
        klipp(rutor_dir, UT / f"lovglod-produktfilm-{a.kvalitet}.mp4")


if __name__ == "__main__":
    main()
