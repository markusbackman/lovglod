# Produktfilmen

En renderad film av LövGlöd, byggd direkt ur STL-filerna i `hardware/v2` och
med samma ljus som firmwaren ritar. Ingen kamera och ingen lampa behövs, och
ändras modellen eller ett ljusläge räcker det att köra om.

```
python3 tools/produktfilm/bygg.py                   # utkast, 960×540, ~4 s/ruta
python3 tools/produktfilm/bygg.py --kvalitet film   # 1080p, 32 sampel, ~8 s/ruta
python3 tools/produktfilm/bygg.py --kvalitet final  # 1080p, 192 sampel, ~25 s/ruta
python3 tools/produktfilm/bygg.py --stillbild 70,530,850   # bara några rutor
python3 tools/produktfilm/ljusspar.py               # ljusspåret som bild, på en sekund
```

Filmen hamnar i `tools/produktfilm/ut/lovglod-produktfilm-<kvalitet>.mp4`.
Tiderna gäller en M2 Max. Hela filmen är 1 140 rutor.

Kräver Blender 4.2 eller senare (utvecklat mot 5.2) och ffmpeg med
libfreetype: `brew install --cask blender && brew install ffmpeg`.

## Filerna

| Fil | Vad den gör |
|---|---|
| `manus.py` | Allt som bestämmer filmen: ljuslägena på tidslinjen, kamerornas rörelser, titlarna |
| `ljusspar.py` | Räknar fram vad listens dioder visar i varje ruta — lägena ur `src/leds.cpp`, portade |
| `scen.py` | Sätter ihop lampan i Blender, bygger studion och renderar med Cycles |
| `bygg.py` | Kör allt och klipper ihop med ffmpeg: titlar, toning in och ut, H.264 |
| `typsnitt/` | Barlow, samma som webbplatsen (SIL OFL) |
| `musik/` | Ljudspåret, Lone Skate Glide (Suno) |

## Hur ljuset kommer in i fönstret

Fönstret har inget animerat material, och genom det syns varje diod som en egen
ljuspunkt med ett svagare sken emellan — så ser lampan ut på film.
`scen.py` skär kärnan mitt itu och tar
fram dess yttervägg — den 488 mm långa väggen listen sitter mot. För varje
punkt i fönstret räknas sedan ut var längs väggen den ligger och hur långt från
listen den är, och det sparas i en uppslagsbild.

`ljusspar.py` ger en bild till: en kolumn per diod och en rad per filmruta.
Fönstrets shader slår upp sin plats längs listen och läser diodens färg i den
rad som hör till rutan. Lyser diod 15 guld i firmwaren, så lyser fönstret guld
just där diod 15 sitter. Filmen räknar med 30 dioder, som de byggda lamporna
har (`N` i `ljusspar.py`); firmwarens standard är 60.

Simuleringen går i firmwarens 120 bilder/s och samplas ner till filmens 30,
eftersom gnistornas och kometernas uttoning räknas per bildruta. Det som inte
är exakt är bruset (FastLEDs `inoise8` är ersatt med ett eget Perlin-brus) och
tempot i manuset: målet och segerdansen är kortade för att rymmas i en film.

## Ändra filmen

- **Ljuset**: `LJUS` i `manus.py`. Lägena heter som i firmwaren (`uppstart`,
  `glod`, `live`, `hjarta`, `mal`, `dans`, `seger`), plus `av` för mörker. Kör `ljusspar.py` och titta i
  `ut/ljusspar.png` innan du renderar.
- **Kamerorna**: `TAGNINGAR`. Varje tagning glider från `fran` till `till`, i mm
  i lampans koordinater, och bromsar in mot slutet. `skift` lägger lampan åt
  sidan i bild (−0,20 = högra tredjedelen) så att texten får fri yta.
  Klippen ligger på musikens takter, se kommentaren i manuset.
- **Studioljuset**: `FYLL` och `KANT` i manuset — hur starkt fyllet och
  motljuset lyser genom filmen. Rendera en `--stillbild` i början och slutet av
  tagningen först; ett utkast av en ruta tar några sekunder.
- **Musiken**: `MUSIK`. `start_s` är var i låten filmen börjar; nu ligger
  låtens drop (98,9 s) på målets första strobe-blixt (16,6 s). Flyttar du
  målet ska `start_s` flyttas lika mycket. `--bara-klipp` räcker.
- **Texterna**: `TEXTER`, stilarna i `bygg.py`. `--bara-klipp` lägger på dem
  igen utan att rendera något.
- **Material och ljussättning**: överst i `scen.py` (`GRON`, `VIT`, `GUL`,
  `LED_STYRKA`) och i `studio()`.

Rutor som redan finns renderas inte om, så ett avbrutet bygge fortsätter där
det slutade. Det betyder också att gamla rutor ligger kvar när manuset ändrats:
kör med `--om` för att börja om i den kvaliteten.

`scen.py` kan spara scenen för att öppna den i Blender:

```
blender -b --factory-startup -P tools/produktfilm/scen.py -- --stillbild 1 --spara scen.blend
```
