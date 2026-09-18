# Rendera produktbilderna

Bilderna på webbplatsen är inga foton — de renderas ur samma STL-filer som
skrivs ut. Byter en del form räcker det att exportera om från nurb och köra det
här verktyget, så följer bilderna med.

```bash
cd tools/render
npm install          # hämtar three, sharp, playwright + Chromium
npm run render       # hardware/v2  →  site/assets
```

Det tar ungefär en minut och skriver `hero.webp`, `front.webp`, `exploded.webp`
och `back.webp`.

## Vyerna

| Vy | Bredd | Vad den visar | Var den används |
|---|---|---|---|
| `hero` | 1200 px | Tänd, snett framifrån | Startsidans hjältebild |
| `front` | 1000 px | Rakt framifrån | Avsnittet *Formgiven in i detalj*, och bakom filmrutan |
| `exploded` | 1400 px | Sprängskiss med alla åtta delar | *Åtta delar, en produkt* i byggguiden |
| `back` | 900 px | Bakifrån, släckt | Specifikationen |
| `side` | 900 px | Från sidan | Används inte i dag |

Alla utom `side` renderas som standard.

## Flaggor

```bash
node render.mjs --views hero,front           # bara några vyer
node render.mjs --stl ../../hardware/v2      # annan mapp med STL-filer
node render.mjs --out ~/Desktop              # annan mapp för bilderna
node render.mjs --format png                 # png i stället för webp
node render.mjs --scale 2                    # dubbel upplösning
node render.mjs --quality 92                 # webp-kvalitet, standard 88
node render.mjs --help
```

STL-mappen letas igenom rekursivt, så `hardware/v2` med sina `plate-1`,
`plate-2`, `plate-3` och `tools` fungerar som den är. Filerna hittas på namn
(`bjorkloven_sign.stl` och de andra sju); saknas någon ritas resten ändå och
namnet skrivs ut som en varning.

## Så fungerar det

`render.mjs` startar en liten webbserver över mappen, öppnar `scene.html` i ett
huvudlöst Chromium, väntar på att scenen ritat klart, fotograferar duken med
genomskinlig bakgrund och låter sharp beskära bort luften runt motivet och
skala till slutbredden.

**Allt som har med utseendet att göra bor i `scene.html`:** kameravinklarna,
ljussättningen och materialen. Några saker där är värda att veta:

- **Bilden är genomskinlig.** Bara skuggan ligger kvar under lampan, så samma
  bild fungerar mot mörkgrönt, vitt eller ett foto.
- **Det gula bandet lyser med `emissive`, inte med en lampa inuti lövet.** En
  punktljuskälla ger en het fläck mitt i bandet; emissive lyser jämnt.
  `--views back` renderas släckt, för där syns inget band ändå.
- **Tonemappingen är `Neutral`, inte ACES.** ACES drar gulet mot beige och
  grönt mot grått, och då slutar färgerna likna PLA-rullarna.
- **Monteringen står i koden.** Lövet är modellerat med framsidan i z = 0 och
  byggt bakåt, sockeln ligger på framsidan och bottenplattan monteras vänd —
  precis som i `bjorkloven_shelf.py`. Måtten (`PLATE_THICKNESS`, `BASE_HEIGHT`,
  `TIE_HEIGHT`, `SIGN_THICKNESS`, `PANEL_RECESS`, `BASE_DEPTH`) är kopior av
  konstanterna i CAD-projektets `system.py`. **Ändras de där måste de ändras
  här**, annars svävar delarna isär i sprängskissen eller lövet sjunker ner i
  sockeln.

- **USB-C-uttaget är ingen STL.** Det är en köpt panelkontakt, så den ritas i
  kod. Var den sitter mäts fram ur sockeln: `findHole` plockar ut bakväggens
  yttersta plan, letar rand-kanterna i det och tar den minsta slingan — hålet.
  Flänsen växer med hålets radie. Flyttas eller ändras hålet i CAD:en följer
  uttaget alltså med av sig självt, till skillnad från måtten ovan. Hittas
  inget hål ritas inget uttag, och `saknade delar` säger till. Sprängskissen
  visar bara de utskrivna delarna och får inget uttag.

Duken renderas ungefär dubbelt så stor som slutbilden, så motivet aldrig
behöver skalas upp. Vill du ha skarpare bilder till en retinaskärm är
`--scale 2` vägen, inte en högre `--quality`.

## Om något går fel

| Symptom | Orsak |
|---|---|
| `Hittar inte STL-mappen` | Kör från `tools/render`, eller peka ut mappen med `--stl` |
| `saknade delar: …` | Filen saknas i STL-mappen — exportera om från nurb |
| Tom eller svart bild | Chromium saknar GPU. Verktyget kör redan med SwiftShader; på en burk utan skärm kan `--scale 1` behövas för att hålla nere minnet |
| `sharp saknas` | Verktyget sparar png i full dukstorlek i stället. `npm install` igen |
| Vill peka ut en egen Chromium | `LOVGLOD_CHROMIUM=/sökväg/till/chrome node render.mjs` |
