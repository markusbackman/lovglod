# Montera — skriva ut och bygga lampan

[← Tillbaka till README](../README.md)

Den fysiska lampan: Björklövens löv, 220 mm högt, stående i en sockel. Lövet är
en 2 mm grön framsida där klubbmärkets gula band är ingjutet som ett
genomskinligt fönster och "BJÖRKLÖVEN / UMEÅ" ligger i vitt. Bakom bandet står
LED-listen på högkant, så bandet lyser. Lövets spets går ner i en sockel med
"SM Guld 1987" i vitt, ett fack för ESP32:n under golvet och ett USB-C-uttag i
ryggen.

Färdigt mått **182,6 × 84 × 250,5 mm**, cirka **307 g** filament och **7 h 59 min**
utskrift före purge (Bambu X2D, 0,2 mm, PLA Basic).

```
hardware/v2/
  plate-1/   bjorkloven_sign, _core, _window, _letters   lövet, flerfärg
  plate-2/   bjorkloven_back                             bakstycket
  plate-3/   bjorkloven_base, _base_letters, _plate      sockeln, flerfärg + bottenplattan
  tools/     bjorkloven_glue_jig                         limfixtur, valfri
```

---

## 1. Delarna

| Fil | Färg | Vad det är |
|---|---|---|
| `bjorkloven_sign` | grön | Lövets skal: framsida, kant, öppen baksida |
| `bjorkloven_core` | grön | Blocket inne i lövet som LED-listen sitter mot |
| `bjorkloven_window` | gul, genomskinlig | Linsen listen lyser igenom |
| `bjorkloven_letters` | vit | BJÖRKLÖVEN / UMEÅ, i nivå med framsidan |
| `bjorkloven_back` | grön | Löstagbart bakstycke, fyra skruvar |
| `bjorkloven_base` | grön | Sockeln, med ESP32-fack och hål för USB-C |
| `bjorkloven_base_letters` | vit | "SM Guld 1987", i nivå med sockelns front |
| `bjorkloven_plate` | grön | Sockelns botten, det lampan står på |
| `bjorkloven_glue_jig` | valfri | Verktyg, inte en del av lampan: håller listen mot kärnan medan limmet härdar |

## 2. Skriva ut

Tre plattor på en skrivare med AMS laddad med **grön**, **genomskinlig gul**
och **vit** PLA. Två av plattorna är *ett objekt i flera färger*, inte lösa
delar: filerna är exporterade i samma koordinatsystem och hamnar rätt
av sig själva.

> **Rotera ingenting i slicern.** Lövet och sockeln är ritade med framsidan
> nedåt, så att den färgade inläggningen blir första lagret mot plattan.
> Utskriftsriktningen är redan inbyggd i filerna.

### Platta 1 — lövet (flerfärg)

Dra in `bjorkloven_sign.stl`, `bjorkloven_core.stl`, `bjorkloven_window.stl` och
`bjorkloven_letters.stl` i Bambu Studio **samtidigt** och svara ja på *"load
these files as a single object with multiple parts"*. Flytta inget.

| Del | Filament |
|---|---|
| sign | grön |
| core | grön |
| window | gul, genomskinlig |
| letters | vit |

Kontrollera att `bjorkloven_core` verkligen kom med. Utan den blir lövet ihåligt,
utan säte för listen och utan kant för bakstycket att vila på.

Kärnan skrivs ut i samma gröna filament som skalet, så lagren över kärnans
höjd är enfärgade och kostar ingenting i purge. Färgbytena som blir kvar
ligger i det gula bandet och i texten. Lita på slicerns egen uppskattning.

### Platta 2 — bakstycket

`bjorkloven_back.stl` i grönt, **med brim**. Stor första lageryta och nio vassa
hörn — den vill släppa i hörnen annars.

### Platta 3 — sockeln (flerfärg) och bottenplattan

Dra in `bjorkloven_base.stl` och `bjorkloven_base_letters.stl` **samtidigt**, som
ett objekt med flera delar. Grönt för sockeln, vitt för texten. Vitt finns bara
i första millimetern, så färgbytena är få.

`bjorkloven_plate.stl` i grönt på samma platta, bredvid. Rotera inte den heller:
"LövGlöd / V2" och skruvhuvudenas försänkningar ska vara uppåt vid utskrift.

### Valfritt — limfixturen

`tools/bjorkloven_glue_jig.stl`, valfri färg, 1 h 04 min och 41 g.

### Tider och vikt

| Del | Tid | Vikt |
|---|---|---|
| sign | 2 h 14 min | 88 g |
| core | 36 min | 21 g |
| window | 22 min | 8 g |
| letters | 26 min | 8 g |
| back | 1 h 09 min | 49 g |
| base | 2 h 22 min | 106 g |
| base_letters | 8 min | 1 g |
| plate | 42 min | 26 g |
| **Totalt** | **7 h 59 min** | **307 g** + purge |

---

## 3. Det du behöver utöver utskrifterna

| Sak | Spec | Var den sitter |
|---|---|---|
| Insexskruv M3 × 8 (ISO 4762) | **tio st**, självgängande i utskrivna 2,5 mm-hål | 4 bakstycke, 4 bottenplatta, 2 lövets spets |
| USB-C-uttag för panelmontering | gänga M11 × 1,0, 16,5 mm bakom flänsen, 20 V/3 A, fyra 24 AWG-ledare, med mutter och dammskydd | sockelns bakvägg |
| Adresserbar LED-list, 5 V | **8 mm bred**, ~55 cm, kapad vid en lödpunkt. WS2812B eller APA102 (se [Koppling i detalj](#6-koppling-i-detalj)) | spåret bakom det gula bandet |
| ESP32 DevKit | högst 57,5 × 30,5 × 15 mm | facket under sockelns golv |
| 330–470 Ω-motstånd | bara för WS2812B | på datatråden |
| 1000 µF / 6,3 V+ kondensator | | över 5 V och GND vid listens början |
| Insexnyckel 2,5 mm | lång nog att nå 40 mm upp i facket | alla tio skruvar |
| Lödkolv | | uttagets ledare och listens trådar |
| Limfixtur (valfri) | `bjorkloven_glue_jig` | hängs i spåret medan listens lim härdar |

Alla skruvhål är blinda på 8 mm, så **M3 × 8 är längden som drar åt överallt**.
M3 × 10 passar lite bättre till de två skruvarna i lövets spets om du har sådana,
men **inget längre än M3 × 9,5 får plats i bottenplattan**.

Listen måste vara *adresserbar* — firmwaren driver WS2812B eller APA102. En
vanlig enfärgad COB-list går inte att styra. Ta en 8 mm bred list: spåret är
dimensionerat efter det, och bredden är ännu inte uppmätt med skjutmått, så prova
listen i spåret innan du limmar.

---

## 4. Montera

Räkna med en halvtimme. Gör stegen i den här ordningen — flera av dem blir
mycket svårare om de byter plats.

### 1. Sätt LED-listen

Spåret är glappet mellan lövets gröna kant och kärnan, 6,78 mm på sitt
smalaste och öppet bakåt. **Ställ listen på högkant**, stående på lövets
framsida, med tejpsidan mot kärnans yttervägg — då lyser den åt sidan, tvärs
över spåret, in i det gula bandet.

Väggen är 488 mm lång, så 55 cm list räcker med lite att kapa. Den böjer aldrig
snävare än 20 mm radie, vilket är hela anledningen till att kärnan
finns. Väggen saknar avsiktligt fas — den är listens säte.

Har du skrivit ut limfixturen: häng den i spåret i sin kant så att dess vägg
trycker listen mot kärnan, låt limmet härda, och lyft ur den. Den täcker kanten
bakstycket ska ligga på, så den måste ut innan steg 3.

Tänk på att listen har en ingångsände (pilarna pekar *bort* från den). Det är
den änden som ska ha trådarna och sitta nederst mot spetsen.

### 2. Dra ner trådarna

Trådarna går ut genom de nedersta 22 mm av spåret, som förblir öppna bakåt, och
följer det 7 mm breda kabelspåret nedför baksidan av lövets spets. Lämna dem
långa nog att nå ner genom sockeln till facket.

### 3. Skruva på bakstycket

Det vilar på toppen av kärnan, som slutar 5 mm under lövets baksida för
just det, och kantens insida styr det i sidled. Fyra M3 × 8; huvudena hamnar i
nivå med lövet.

### 4. Sätt USB-C-uttaget — innan kortet åker i

Tryck uttaget genom 11,6 mm-hålet i sockelns bakvägg, 9,5 mm upp, med flänsen
utåt. Skruva muttern inifrån facket, i sin 16,6 mm urfräsning. Gör du det efter
att kortet är på plats har du fingrarna där kortet sitter.

### 5. Sänk ner lövet i sockeln

De nedersta 22 mm går ner i sockelns urtag och kragen greppar ytterligare 8,6 mm
ovanför toppen. Trådarna följer kabelschaktet längs urtagets bakvägg ner i
facket. Lövet vilar på det plana blocket på spetsen, mot urtagets botten — **tar
det stopp för tidigt är det trådarna som är i vägen**, inte passningen.

### 6. Skruva fast lövet inifrån facket

Stick upp 2,5 mm-nyckeln genom facket och skruva två M3 genom fackets tak in i
blocket på lövets spets. Inget syns utifrån.

### 7. Koppla

Allt ström kommer genom USB-C-uttaget. Uttagets **röda** ledare är +5 V och
**svarta** är GND (de två andra ledarna används inte).

```
   USB-C röd   ──┬─────────────────► list 5V
                 │
               1000µF
                 │
   USB-C svart ──┴──┬──────────────► list GND
                    │
                    └──────────────► ESP32 GND

   USB-C röd   ────────────────────► ESP32 5V (VIN)
```

Datatrådarna, samma pinnar som i [Koppling i detalj](#koppling):

```
   WS2812B:  ESP32 GPIO13 ──[390Ω]──► DIN
   APA102:   ESP32 GPIO23 ──────────► DI  (data)
             ESP32 GPIO18 ──────────► CI  (klocka)
```

- Mata listen **direkt från uttaget**, inte via ESP32:ns 5V-pinne.
- ESP32 och listen måste dela GND.
- Kontrollera märkningen på listens kopparbleck innan du slår på strömmen — byts
  5 V och GND kan listen gå sönder direkt.
- Uttaget och dess ledare är märkta för 3 A, och firmware begränsar listen till
  `LED_MAX_MILLIAMPS` (3000 mA) i `include/config.h`. ESP32:n drar sitt ovanpå
  det, så sänk gärna taket något i den här lampan. Laddaren måste klara 5 V/3 A.
- **Använd en USB-A→USB-C-kabel.** Uttaget ger ingen ström från en
  USB-C→USB-C-kabel eller en ren USB-C-laddare.

Motstånd och kondensator får plats i facket bredvid kortet; krympslang om dem.

### 8. Kortet i, bottenplattan på

Kortet åker ner i facket med **USB-änden bakåt** och landar med trådarna bredvid
sig, inte under. Flasha det gärna över USB innan ([Installera firmwaren](INSTALLERA.md)) — efter det här
steget kommer du bara åt det genom att skruva loss plattan.

Skruva sedan på bottenplattan underifrån, **försänkningarna nedåt mot hyllan**,
så att de fyra M3-huvudena hamnar i nivå och sockeln står plant. "LövGlöd / V2"
är graverat i den sidan.

Klart. Koppla in laddaren i uttaget på baksidan och fortsätt med
[första start](INSTALLERA.md#första-start--setup-portalen).

---

## 5. Felsökning vid montering

| Symptom | Trolig orsak |
|---|---|
| Lövet går inte ner hela vägen i sockeln | Trådarna ligger i vägen i urtaget, inte fel passning |
| Bakstycket ligger inte plant | Limfixturen sitter kvar, eller listen står inte nere i spåret |
| Lövet kom ut ihåligt från skrivaren | `bjorkloven_core.stl` laddades inte på platta 1 |
| Texten på sockeln/lövet är grön | Delarna laddades som separata objekt, eller fel filament tilldelat |
| Listen mörk men ESP32 lever | Kopplad till listens utgångsände, eller data/klocka skiftade (APA102) |
| Bakstycket blev skevt i hörnen | Skrevs ut utan brim |

---

## 6. Koppling i detalj

Referens för kopplingen — gäller både lövlampan ovan och en egen konstruktion
med annan list eller längd.

| Del | Anmärkning |
|---|---|
| ESP32 DevKit v1 (ESP32-WROOM-32) | Vilken klon som helst duger |
| LED-list, 5 V | **WS2812B** (NeoPixel) eller **APA102** (DotStar), 8–150 LEDs |
| 5 V-matning, minst 3 A | Lövlampan matas via USB-C. Startar ESP32:n om vid målfyrverkeriet räcker matningen inte — sänk `LED_MAX_MILLIAMPS` |
| Motstånd 330–470 Ω | I serie på datalinjen — bara WS2812B |
| Kondensator 1000 µF / 6,3 V+ | Över 5 V och GND vid listens början |

Samma firmware driver båda listorna. Vilken som sitter på, och hur många
dioder den har, väljs i setup-portalen (se [Installera firmwaren](INSTALLERA.md#första-start--setup-portalen)) och sparas i NVS — det
överlever omstart och OTA, och går att ändra i efterhand på statussidan.

### Koppling

Matningen är densamma för båda listorna:

```
   5V PSU ──┬──────────────► LED 5V
            │
          1000µF
            │
   PSU GND ─┴──┬───────────► LED GND
               │
        ESP32 GND
```

**WS2812B** — en datatråd:

```
   ESP32 GPIO13 ──[390Ω]────► LED DIN
```

**APA102 / DotStar** — data och klocka:

```
   ESP32 GPIO23 ────────────► LED DI   (data)
   ESP32 GPIO18 ────────────► LED CI   (klocka)
```

APA102-listen har fyra färgade trådar. Den vanligaste färgkoden för rött,
vitt, grönt och blått är:

| Tråd | Listens märkning | Kopplas till |
|---|---|---|
| **Röd** | 5V (VCC) | Nätaggregatets +5 V |
| **Vit** | GND | Nätaggregatets GND **och** en GND-pinne på ESP32 |
| **Grön** | DI (data in) | ESP32 **GPIO23** |
| **Blå** | CI (klocka in) | ESP32 **GPIO18** |

```
   Röd   ──── PSU +5V
   Vit   ──┬─ PSU GND
           └─ ESP32 GND
   Grön  ──── ESP32 GPIO23
   Blå   ──── ESP32 GPIO18
```

> **Kontrollera mot listen innan du slår på strömmen.** Färgerna är ingen
> standard och varierar mellan tillverkare. Det som gäller är texten på
> kopparblecken där trådarna är lödda — `5V`, `CI`, `DI`, `GND` — och att
> pilarna på listen pekar *bort* från trådarna (det är ingångsänden). Byts 5V
> och GND kan listen gå sönder direkt. Byts bara data och klocka tar inget
> skada, men listen förblir mörk eller visar skräp: byt då grön och blå.

**Viktigt:**
- ESP32 och listen måste dela GND, annars blir datasignalen skräp.
- Koppla till listens *ingång* — DIN, respektive DI/CI. Utgångsänden heter DO/CO.
- Mata *inte* 60 LEDs genom ESP32:ns 5V-pinne — dra 5 V direkt från nätaggregatet.
- ESP32:ns 3,3 V-signal räcker oftast till båda listorna. Vid glitch: sätt in en
  nivåomvandlare (74AHCT125) — för APA102 på både DI och CI — eller mata listen
  med 4,5 V istället för 5 V.
- Innan en list är vald drivs båda utgångarna samtidigt, så portalens gröna
  puls syns vilken list som än är inkopplad.
- Firmware håller listen under `LED_MAX_MILLIAMPS` (3000 mA) via
  `FastLED.setMaxPowerInVoltsAndMilliamps()`. Justera det i `include/config.h`
  efter din matning. ESP32:n drar sitt ovanpå, och en svag matning ger
  brownout-omstarter vid målfyrverkeriet långt under taket.
