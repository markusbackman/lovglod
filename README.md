# LövGlöd 🍃

ESP32 + WS2812B- eller APA102-list som lever med Björklöven:

| Läge | Ljus |
|---|---|
| **Uppstart** | Ett gult svep som drar en gång längs listen |
| **Setup** | Lugn **grön** puls — inget WiFi sparat, anslut till lampans eget nät |
| **Ansluter** | Gul punkt som jagar runt listen |
| **WiFi svarar inte** | Lugn **röd** puls — sparat WiFi finns men går inte att nå |
| **Arbetar** | Gul förloppsstapel som står stilla mellan stegen |
| **Standby** | Långsam gul glöd, ~10 s andetag, med organisk variation längs listen |
| **Vann igår** | Några glittrande gnistor ovanpå glöden |
| **Match pågår** | Piggare glöd, kortare andetag |
| **MÅL** | 12 s snabb gul eldgivning — stroboskop följt av kometer ut från mitten, fördröjt 15 s så tv:n hinner ikapp |
| **Uppdaterar** | Gul förloppsstapel som fylls med nedladdningen |
| **Ingen data** | Svagt rött andetag — SHL svarar inte |

Data hämtas från **SHL:s eget publika API**. Björklöven är uppflyttade till SHL
inför säsongen 2026/27 efter vinsten mot Karlskoga i HockeyAllsvenskans final,
så ligans officiella API är rätt källa.

---

## 1. Varje variant på listen

Lägena ligger i `LedMode` (`src/leds.h`) och ritas i `src/leds.cpp`. Exakt ett
läge är aktivt åt gången; gnistorna är det enda som ligger *ovanpå* ett annat
läge. Alla tider nedan gäller standardvärdena i `include/config.h`
(`YELLOW_R`/`YELLOW_G`, 60 dioder).

### `LED_BOOT` — uppstartsflöde

Gult ljus flödar in från listens ena ände till den andra på 700 ms och blir
stående — hela listen lyser i 300 ms innan WiFi ens försökts. Kanten är mjuk
över tre dioder, så det ser ut som ljus som rinner in och inte som en stapel
som fylls.

Syftet är rent diagnostiskt, och slutläget är poängen: det är det enda
tillfället då varje diod syns tänd samtidigt, så en död diod är omöjlig att
missa. Ett svep dolde samma fel som en lucka i efterglöden.

Formen är också vald för att inte krocka med det som kommer direkt efteråt.
`LED_CONNECTING` är en gul punkt med svans, och ett gult svep följt av ett gult
jagande ljus läses som en enda lång animation — man ser aldrig var uppstarten
slutar och anslutningen börjar.

### `LED_PORTAL` — grön puls

Hela listen i Björklövens gröna (`hue 96`, full mättnad), som andas mellan
mörkt och ljust ungefär var tredje sekund. Inget flimmer, ingen rörelse längs
listen — en lugn helyta. Betyder: *lampan har inget sparat WiFi och har startat
sitt eget nät `LövGlöd-Setup-XXXX`*. Grönt för att det inte är ett fel, det
är bara din tur att göra något — och för att grönt och gult är lagets färger,
så lampan håller sig till dem även innan den vet något om hockey.

### `LED_PORTAL_RETRY` — röd puls

Exakt samma rytm och form som den gröna pulsen, men röd (`hue 0`). Skillnaden i
betydelse: *sparat WiFi finns, men lampan kommer inte fram till det.* Routern
kan vara nere, lösenordet ändrat eller lampan för långt bort. Portalen är uppe
på samma sätt, och lampan provar dessutom de sparade uppgifterna igen var 5:e
minut — löser det sig av sig själv slocknar det röda utan att du gjort något.
Att formen är identisk och bara färgen skiljer är avsiktligt: du ska kunna
läsa av läget på håll utan att räkna pulser.

### `LED_CONNECTING` — jagande gult

En gul punkt springer varv efter varv längs listen, ungefär ett varv per
sekund, med en kort efterglödssvans. Rörelse betyder "jobbar på det" — visas
medan WiFi-anslutningen pågår.

### `LED_WORKING` — förloppsstapel vid uppstart

Listen fylls från början med dämpat gult (`WORK_BODY_VAL`), och LED:en längst
fram i stapeln lyser i full styrka som markör för steget som pågår. Används
under de blockerande momenten i uppstarten (nätverkstest, första
datahämtningen).

Stapeln har medvetet **ingen egen animation**. Anropen den täcker går inte att
avbryta, så bilden fryser ändå mitt i steget — och en stapel som står still ser
avsiktlig ut, vilket en fryst animation inte gör.

Stegen är få och långa: nätverkstestet rapporterar 9 steg, första hämtningen
bara 3. Stapeln hoppar alltså i stora språng och står stilla länge däremellan,
och den är ritad för att tåla det. Därför ligger också ett golv på
`BAR_MIN_LIT` dioder — vid noll steg klara väntar lampan som längst, och det
är precis då den inte får se släckt ut.

### `LED_STANDBY` — den långsamma glöden

Grundläget, det lampan står i nästan all sin tid. Ett gult andetag på ~6,7 s
(`GLOW_BPM 9`) mellan nästan släckt (`GLOW_MIN_VAL 22`) och tydligt uppe
(`GLOW_MAX_VAL 140`).

Spannet är brett med flit. Ögat svarar ungefär logaritmiskt på ljus: ett
andetag som bara fördubblar styrkan läses på håll knappt som en förändring —
man ser en list som lyser jämnt. Det är kvoten mellan botten och topp som gör
andetaget synligt tvärs över ett rum, så taket ligger högt medan botten står
kvar nere.

Tre detaljer gör att det inte ser ut som en dimmer:

- **Perlin-brus per LED.** Varje LED avviker ±18 % från andetaget, och
  brusmönstret vandrar långsamt längs listen. Ljuset lever istället för att
  pulsera som en enda platt yta.
- **Färgen står still, bara styrkan andas.** Gulen är en fast RGB-blandning
  (`YELLOW_R 255`, `YELLOW_G 224`) som skalas ner i sin helhet, så toppen och
  botten av andetaget är exakt samma gula — bara olika starkt. Det låter
  självklart, men går inte att få med en HSV-nyans: grönkanalen ligger lägre
  och trunkeras till noll medan den röda fortfarande lyser, så utfadningen
  landar i orange och till sist rent rött. Av samma skäl körs listen utan
  FastLEDs `TypicalLEDStrip`-korrigering, som drar ner grönt med 30 %.
- **Glöden bottnar, den slocknar aldrig — men den bottnar inte platt.**
  Golvet (`GLOW_FLOOR_VAL 15`) och andetagets botten (`GLOW_MIN_VAL 22`) är två
  skilda värden. Under en handfull räkningssteg har en WS2812 inget kvar att
  arbeta med: den röda dioden styr ensam, gulen bryts upp i rött, och varje steg
  i andetaget blir ett synligt hopp — därför golvet, som gäller varje enskild
  LED. Vid standardljusstyrkan motsvarar det ungefär nio räkningssteg.

  Andetaget vänder däremot en bit ovanför, så att bruset har plats att dra
  dioder nedåt utan att klippas. Låg de två på samma värde klipptes varje
  negativ avvikelse bort i vändningen: halva listen las sig platt på exakt samma
  nivå och strukturen blev ensidig, just i det ögonblick då listen är som
  lugnast och betraktas som mest.

  Vill du sänka botten måste därför golvet med — sänks bara `GLOW_MIN_VAL`
  äter bruset upp marginalen och klippningen är tillbaka.

Och för att tonandet ska bli mjukt hela vägen: sinusen räknas i 16 bitar med
gammakurvan lagd på fasen (`beatsin16`, avrundning först på slutet), och
`LED_DITHER` växlar mellan närliggande nivåer mellan bildrutorna. Det är
gammakurvan som gör 16 bitar nödvändiga, inte spannets bredd — kurvan är fasen
i kvadrat och rör sig knappt alls kring vändningen, så just där glöden
tillbringar mest tid mappas många bildrutor i rad till samma utnivå. Utan
dithern står listen still på samma nivå och byter sedan ett helt steg — det är
precis det man ser som ryck.

### Gnistor — "vi vann igår" (ovanpå glöden)

Inget eget läge utan ett lager som läggs ovanpå standby och live. Ungefär var
0,7:e sekund tänds en slumpad LED i kall vit (`SPARKLE_R`/`_G`/`_B`) och tonar
ut på ett par tiondelar. Intervallet slumpas runt medelvärdet så glittret
aldrig hittar en takt.

Gnistan **adderas** till glöden i stället för att blandas in i den. Blandad blev
den lika ljus som glöden råkade vara just då — i andetagets topp knappt dubbelt
så ljus som ytan under den, alltså nästan osynlig, medan samma gnista i
vändningen var tiofalt ljusare. Glittret tonade in och ut i takt med andetaget.
Adderat mättar det mot vitt oavsett var i cykeln det landar.

Tänds när `played-games` säger `WIN` på en match som spelades **i går enligt
lokalt datum i Stockholm** — inte "senaste 24 timmarna". Släcks vid nästa
midnattskontroll. Skruva med `SPARKLE_MEAN_INTERVAL_MS` (högre = färre) och
`SPARKLE_DECAY` (högre = kortare).

### `LED_LIVE` — match pågår

Samma glöd som standby, men piggare: andetaget dubbelt så snabbt (~3,3 s),
botten upplyft så listen aldrig går ner i mörkret, och nyansen dragen åt
bärnsten (`LIVE_YELLOW_G 165`). Den väntar. Gnistorna från gårdagens vinst
ligger kvar även här.

Det är **färgen** som bär skillnaden mot standby, inte styrkan. `GLOW_LIVE_LIFT`
sattes när andetaget toppade på 44 och betydde då drygt en tredjedel mer ljus;
mot dagens tak på 140 är samma sexton steg ett par procent och syns knappt. Ett
skifte från gult till orange läses däremot direkt, även i ögonvrån — och till
skillnad från ett ljusare läge kostar det ingenting i ström.

Läget slås på när matchfönstret öppnas — nedsläpp minus marginal enligt
spelschemat, eller när mockservern säger till.

### `LED_GOAL` — målfyrverkeriet

12 sekunder (`GOAL_DURATION_MS`) i två faser:

1. **Stroboskop, 0–2,5 s** (`GOAL_STROBE_MS`). Hela listen blixtrar i ~14 Hz
   mot en dämpad gul botten, växelvis vitt och mättat gult. Det är den delen
   som får folk att titta upp.
2. **Eldgivning, 2,5–12 s.** Var 110:e ms skjuts en ny komet ut från listens
   mitt åt båda hållen samtidigt: vitglödande kärna med två gula svansled
   efter sig. Ovanpå det slumpade gnistregn så salvorna inte blir mekaniska.

Sista 1,2 sekunderna tonas allt ner mot standby istället för att slockna tvärt.

**Fyrverkeriet är fördröjt 15 sekunder som standard.** SHL:s live-data är
snabbare än tv-sändningen, så utan fördröjning tänder lampan målet innan det
syns på skärmen — och alla i rummet vet att det gick in innan de får se det. Se
[Tv-fördröjning](#tv-fördröjning) nedan.

**Mål i rad staplar inte om från början.** Ett nytt mål under pågående
fyrverkeri förlänger bara till 12 s från nu — annars hade ett snabbt 2-mål
kastat tillbaka listen till stroboskopet och man hade tappat känslan av att det
var *två* mål.

Med `GOAL_ONLY_OUR_TEAM true` tänds fyrverkeriet bara på Björklövens mål; sätt
`false` om du vill ha det på alla mål i matchen. Knappen **"Testa
målfyrverkeriet"** på statussidan kör hela sekvensen när som helst.

### `LED_UPDATING` — OTA-förlopp

Listen fylls från början i takt med nedladdningen, med en snabbt pulserande LED
i fronten som visar att överföringen lever. Tar över alla andra lägen medan den
pågår, och den enda utgången är omstart in i den nya firmwaren.

Till skillnad från uppstartsstapeln **får** den här röra sig. En nedladdning är
en ström: förloppet uppdateras kontinuerligt och processorn är ledig, så ett
pulserande huvud är ärligt. Uppstartsstapeln står stilla för att den måste —
anropen den täcker fryser bilden ändå.

Kroppen ligger ljusare (`UPDATE_BODY_VAL 157` mot uppstartens 75), och samma
golv på `BAR_MIN_LIT` dioder gäller: utan det visas TLS-handskakningen mot
GitHub — flera sekunder innan första byten kommer — som en enda blinkande diod
på en släckt list, i det ögonblick då enheten skriver om sin egen firmware och
man tittar som mest.

### `LED_ERROR` — ingen kontakt

Ett mycket svagt rött andetag, långsammare än portalens puls (~5 s) och bara
knappt synligt i mörker. Betyder att lampan är på WiFi men inte får något svar
från SHL alls. Avsiktligt diskret: det är ett tillstånd som kan hålla i sig i
timmar när shl.se ligger nere, och då ska det inte lysa upp rummet.

### Ljusstyrka och strömtak

Ovanpå allt detta ligger en global ljusstyrka (`LED_DEFAULT_BRIGHTNESS 160`,
ändras på statussidan) och FastLEDs strömtak
(`LED_MAX_MILLIAMPS 3000`). Vid ett fyrverkeri på full vit skalar FastLED ner
hela bilden för att hålla sig under taket — effekten ser likadan ut, bara
svagare, så sätt taket efter ditt nätaggregat och inte tvärtom.

---

## 2. Hårdvara

| Del | Anmärkning |
|---|---|
| ESP32 DevKit v1 (ESP32-WROOM-32) | Vilken klon som helst duger |
| LED-list, 5 V | **WS2812B** (NeoPixel) eller **APA102** (DotStar), 8–150 LEDs |
| 5 V nätaggregat, ≥ 4 A | 60 LEDs på full vit ≈ 3,6 A |
| Motstånd 330–470 Ω | I serie på datalinjen — bara WS2812B |
| Kondensator 1000 µF / 6,3 V+ | Över 5 V och GND vid listens början |

Samma firmware driver båda listorna. Vilken som sitter på, och hur många
dioder den har, väljs i setup-portalen (se avsnitt 4) och sparas i NVS — det
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
- Firmware håller sig under 3000 mA via `FastLED.setMaxPowerInVoltsAndMilliamps()`.
  Justera `LED_MAX_MILLIAMPS` i `include/config.h` efter ditt nätaggregat.

---

## 3. Bygg och flasha

```bash
pio run                 # bygg
pio run -t upload       # flasha via USB (första gången)
pio device monitor      # seriell logg, 115200 baud
```

## 4. Första start — captive portal

Setup sker i två steg, av två olika personer.

**Administratören — innan lampan lämnas ut:**

1. Starta lampan utan WiFi → **grön puls** och den startar eget nät
   `LövGlöd-Setup-XXXX` (öppet). Innan en list är vald drivs båda
   utgångarna, så pulsen syns vilken list som än sitter i.
2. Anslut med mobilen. Inloggningsrutan poppar upp av sig själv (iOS, Android
   och Windows kontroll-URL:er är hanterade). Gör den inte det: gå till
   `http://192.168.4.1`.
3. Portalen visar bara listvalet: välj **WS2812B** eller **APA102** och antal
   dioder, tryck *Spara list*. Inget WiFi behövs. Pulsen flyttar direkt till
   den valda listen — syns den inte är valet fel.
4. Dra ur strömmen och lämna ut lampan.

Listvalet ligger i NVS och överlever omstart, OTA och "Glöm WiFi". Ändra det i
efterhand på `http://192.168.4.1/strip` i portalen, eller via *Felsökning* på
statussidan.

**Kunden — hemma:**

1. Kopplar in lampan → grön puls, nätet `LövGlöd-Setup-XXXX` dyker upp.
2. Ansluter med mobilen och ser bara WiFi-formuläret: välj nätverk, skriv
   lösenord, spara.
3. Lampan ansluter och glöden blir gul.

Lampor som uppgraderas över OTA från en version utan listval behåller sin
WS2812B — firmwaren känner igen dem på att WiFi redan är sparat.

Går anslutningen inte igenom öppnas portalen igen. Står portalen öppen utan att
någon gör något provar lampan de sparade uppgifterna på nytt var 5:e minut —
praktiskt om det bara var routern som startade om.

**Statussida** (när den är online): `http://bjorkloven-led.local` eller enhetens
IP. Där finns nästa match, senaste resultat, ljusstyrka, en knapp som testar
målfyrverkeriet, och "Glöm WiFi".

---

## 5. OTA-uppdatering

### Varför Releases och inte Actions-artifacts

Artifacts går **inte** att hämta från en ESP32. Verifierat:

```
GET https://api.github.com/repos/…/actions/artifacts/<id>/zip   → 401 Unauthorized
GET https://github.com/…/releases/download/v1.2.3/firmware.bin  → 200 OK
```

Artifact-nedladdning kräver en token med `actions:read` **även på publika
repon**, filen är dessutom zippad och gallras automatiskt efter 90 dagar. Att
baka in en PAT i firmwaren vore både osäkert och kortlivat.

Release assets serveras däremot anonymt, via en 302 till
`release-assets.githubusercontent.com` som `httpUpdate` följer.

`ci.yml` laddar ändå upp en artifact vid varje push — den är till för *dig* att
flasha manuellt, inte för enheterna.

### Så här fungerar kedjan

```
  git tag v1.1.0 && git push --tags
            │
            ▼
  .github/workflows/release.yml
    · injicerar FW_VERSION=1.1.0 via PLATFORMIO_BUILD_FLAGS
    · bygger, kontrollerar storlek mot partitionen
    · verifierar att versionen hamnade i binären
    · publicerar firmware.bin + firmware.json som release
            │
            ▼
  ESP32, var 12:e timme (aldrig under match)
    · GET api.github.com/repos/OWNER/REPO/releases/latest
    · tag_name "v1.1.0" ≠ installerad 1.0.0  →  ladda ner
    · progressen visas som stapel på LED-listen
    · omstart in i den nya firmwaren
```

### Privat repo — extra steg

Repot är privat, vilket betyder att **`browser_download_url` inte fungerar
anonymt** (verifierat: 404 både på API:t och nedladdningslänken). Enheten
behöver en token, och tar då en annan väg:

```
  1. GET api.github.com/repos/OWNER/REPO/releases/latest
       Authorization: Bearer <token>              → tag_name + assets[].id

  2. GET api.github.com/repos/OWNER/REPO/releases/assets/<id>
       Authorization: Bearer <token>
       Accept: application/octet-stream
       redirect EJ följd                          → 302, Location: signerad URL

  3. httpUpdate hämtar den signerade URL:en utan auth-header
```

Steg 2 följer redirecten för hand med flit. `HTTPClient` skickar annars samma
headers vidare till målet, och den signerade URL:en bär redan sina egna
engångscredentials — vår PAT har inget där att göra.

**Skapa token:** GitHub → Settings → Developer settings → Fine-grained tokens.

| Inställning | Värde |
|---|---|
| Repository access | Only select repositories → `bjorkloven-led` |
| Permissions | Contents: **Read-only** |
| Expiration | Sätt en påminnelse — enheten slutar uppdatera den dagen den går ut |

Klistra in den i fältet **GitHub-token** på statussidan. Tomt fält vid senare
sparningar betyder "rör inte" — skriv `-` för att radera. Token visas aldrig i
klartext igen, bara som antal tecken på felsökningssidan.

> **Den ligger i klartext i NVS.** Någon med fysisk åtkomst kan läsa ut den med
> `esptool read_flash`. Därför en fine-grained token med `contents:read` på
> *bara* det här repot — värsta fall är att någon kan läsa din firmwarekod.
> Ska lampan stå någon annanstans än hemma: överväg att göra repot publikt och
> hoppa över token helt, eller signera firmwaren (se Säkerhet nedan).

**Actions-minuter:** privata repon drar från gratiskvoten (2 000 min/månad).
Ett bygge tar ~1–2 min, så det är ingen praktisk gräns — men publika repon är
gratis obegränsat, om du någon gång vill byta.

### Sätta upp

1. Repot ligger på `markusbackman/bjorkloven-led` (privat). Workflowsen är
   aktiva och `v1.0.0` är redan publicerad.
2. Öppna lampans statussida och fyll i **Uppdateringskälla**:
   `markusbackman/bjorkloven-led` — plus en token, eftersom repot är privat.
   Kortformen `owner/repo` expanderas automatiskt till Releases-API:t. Vill du
   hosta själv går det lika bra att ange en full URL till ett `firmware.json`.
3. Släpp en version:

```bash
git tag v1.1.0
git push origin v1.1.0
```

Eller kör workflowen manuellt från Actions-fliken med versionen som indata —
då skapas taggen åt dig.

Knappen **"Sök efter uppdatering nu"** på statussidan tvingar fram en kontroll
direkt istället för att vänta på nästa 12-timmarsintervall.

### Versionshantering

`FW_VERSION` sätts av CI via `-DFW_VERSION='"1.1.0"'`. `config.h` har ett
`#ifndef`-skydd, så lokala byggen får `1.0.0-dev` — vilket alltid skiljer sig
från en publicerad version och därför alltid uppdaterar vid första kollen.

Enheten uppdaterar när versionen **skiljer sig**, inte bara när den är nyare.
Det är avsiktligt och ger en gratis rollback: kryssa i *pre-release* på en
trasig release, så pekar `/releases/latest` tillbaka på den förra och lamporna
rullar tillbaka av sig själva vid nästa kontroll.

Misslyckas samma version tre gånger slutar enheten försöka (`OTA_MAX_FAILURES`)
— annars skulle en trasig release ladda ner 1 MB var 12:e timme för alltid.
Räknaren nollställs så fort en ny version dyker upp.

### Ingen push-OTA

ArduinoOTA-push fanns tidigare och är borttaget med flit. Den vägen gick förbi
signaturkontrollen helt och skyddades bara av ett lösenord som låg i klartext i
det här repot — den var alltså den enda kvarvarande vägen till godtycklig
firmware på en lampa i nätet.

Kvar finns två vägar in, och båda är sådana man kan lita på: **signerad
self-update**, eller **USB**. Den som står vid USB-porten är ändå förbi varje
mjukvaruspärr, så där finns inget att skydda.

Under utveckling är det alltså USB som gäller:

```bash
pio run -t upload            # håll in BOOT — kortet har trasig auto-reset
```

### Minne

GitHub-svaret parsas strömmande med ett ArduinoJson-filter, så bara `tag_name`
och asset-namnen behålls. För ett repo med 280 assets krymper svaret från
480 kB till 38 kB; för det här repot är det 308 byte.

### Signerad firmware

Enheten kör `setInsecure()` — inget certifikat valideras, varken mot shl.se
eller GitHub. Certifikatpinning löser det dåligt här: kedjan hoppar mellan
`api.github.com` och `release-assets.githubusercontent.com` och roterar med
jämna mellanrum, så en hårdkodad rot-CA gör bara lampan tyst den dagen den byts.

Lösningen ligger i stället ett lager upp: **binären är signerad, så transporten
spelar ingen roll.** Två kontroller körs, båda innan något committas:

| Kontroll | Fångar | Källa |
|---|---|---|
| `sha256` | trasig eller avbruten nedladdning | `sha256` i `firmware.json` |
| RSA-2048-signatur | allt annat, inklusive MITM | `sig` i `firmware.json` |

Ordningen är hela poängen. `Update.end()` sätter boot-partitionen, så allt som
ska kunna säga nej måste säga det före den — därför drivs nedladdningen för hand
i `downloadAndInstall()` i stället för med `httpUpdate`, som committar och
startar om på egen hand. Faller någon kontroll anropas `Update.abort()`, den
gamla partitionen står orörd och lampan fortsätter på nuvarande firmware.

Det **failar stängt**: saknas `sha256` eller `sig` i manifestet, eller går den
publika nyckeln inte att tolka, installeras ingenting. Det finns ingen väg
tillbaka till "osignerat men okej".

#### Sätta upp nyckeln

```bash
./tools/generate-ota-key.sh
gh secret set OTA_SIGNING_KEY < ota_signing_key.pem
```

Skriptet lägger den publika nyckeln i `include/ota_pubkey.h` (committas — en
publik nyckel är publik) och den privata i `ota_signing_key.pem` (`.gitignore`:ad).
CI signerar med secreten och verifierar sedan signaturen mot just den publika
nyckel som ligger i binären, så ett nyckelpar som glidit isär fångas i bygget i
stället för av en flotta lampor som tyst slutar uppdatera sig.

Skriptet går att köra om för att laga en borttappad header utan att byta nyckel.
Byter du nyckel på riktigt måste varje lampa flashas över USB en gång — den
gamla firmwaren litar bara på den gamla nyckeln.

#### Vad detta inte täcker

Webbgränssnittet saknar autentisering, så vem som helst på nätet kan peka om
uppdateringskällan eller radera WiFi-uppgifterna — se B3 i
`PRODUKTIONSKLAR.md`. Att de *inte* kan få något installerat är just
signaturens förtjänst, men B3 bör ändå stängas.

Notera också vad borttagandet av push kostar: går en signerad uppdatering igenom
och visar sig trasig finns ingen väg tillbaka över nätet. Enda återvägen är USB
med BOOT-knappen intryckt. Det är argumentet för B5 (rollback).

## 6. Datakällan

Reverse-engineerad från shl.se:s frontend och verifierad mot skarpa svar
2026-08-24. Ingen API-nyckel behövs.

**Björklövens team-UUID: `4519-4519Rdei6`**

| Vad | Anrop |
|---|---|
| Nästa matcher | `GET https://www.shl.se/api/sports-v2/upcoming-games/4519-4519Rdei6?gamePlace=` |
| Spelade matcher | `GET https://www.shl.se/api/sports-v2/played-games/4519-4519Rdei6` |
| Dagens matcher | `GET https://www.shl.se/api/sports-v2/today-games` |
| **Live-ström (SSE)** | `GET https://game-broadcaster.s8y.se/live/game?gameUuid=<uuid>` |
| Hela seriespelet | `GET https://www.shl.se/api/sports-v2/game-schedule?seriesUuid=qQ9-bb0bzEWUk&seasonUuid=ndcf81nlb3&gameTypeUuid=qQ9-af37Ti40B` |

`played-games` ger `homeTeamInfo.status` = `WIN` / `LOSE` — det är den som styr
gnistorna. Firmware jämför matchens datum mot gårdagens *lokala* datum i
Stockholm, inte 24 timmar bakåt.

Svaren är ~5,5 kB vardera, vilket är varför enheten klarar att parsa dem direkt
utan mellanserver. Kräver `User-Agent`-header — shl.se svarar 403 utan.

### ⚠️ Live-formatet är inte verifierat

SSE-strömmen är **inte** observerad med riktig data — den här koden skrevs
2026-08-24, mitt i uppehållet, och första SHL-matchen är 19 september.
Endpointen svarar korrekt (`content-type: text/event-stream`), men *fältnamnen i
ramarna kunde inte inspekteras*.

Därför är parsningen medvetet tolerant: `findScorePair()` i `src/shl.cpp` söker
rekursivt efter alla vanliga varianter (`homeScore`/`awayScore`,
`homeGoals`/`awayGoals`, `homeTeam.score`/`awayTeam.score`, …).

**Detta bör verifieras i september.** Öppna `http://bjorkloven-led.local/debug`
under en match — där visas den senaste råa SSE-ramen. Stämmer inte fältnamnen
räcker det att lägga till dem i `findScorePair()`.

Som skyddsnät pollar enheten dessutom `today-games` var 45:e sekund under
matchfönstret, så måldetekteringen fungerar även om SSE-formatet skulle ändras.

---

## 7. Labbtest utan match — mockservern

Björklöven spelar inte varje dag, och du vill inte vänta till nedsläpp för att se
om målfyrverkeriet tänder. `mock/server.py` ger dig en styrsida där du sätter
matchläget för hand och trycker in det i lampan.

```bash
python3 mock/server.py            # lyssnar på 0.0.0.0:8080
python3 mock/server.py --port 9000
```

Servern skriver ut adressen den nås på:

```
── Björklöven mockserver ─────────────────────────────
  Styrsida:   http://192.168.1.42:8080/
  Lampan:     fyll i dess adress på styrsidan och slå på push
```

Bara standardbiblioteket — inget att installera.

### Push, inte pollning

Servern hämtar inte in lampan — den *trycker* matchläget till lampans egen
`/push`. Två steg:

1. På lampans statussida: sätt **Felsökningsläge** till *På* och spara.
2. På styrsidan: fyll i lampans adress under **Push till lampan** och slå på.

Raden *Datakälla* byter då till `push från mockservern`, och styrsidan visar om
leveransen går fram.

Felsökningsläget är porten. Är det av svarar `/push` med 404 och lampan hämtar
bara från SHL — annars hade vem som helst på nätet kunnat styra den genom att
posta ett matchläge. Läget ligger i NVS och överlever omstart och OTA, så slå av
det när labbtestet är klart. Slår du av mitt i en pågående push släpps push-läget
direkt och lampan hämtar om från SHL, utan att vänta ut leasen.

Riktningen är vald med flit. Skulle lampan hämta härifrån måste den nå datorn,
och där står brandväggen i vägen — på en jobbdator med central brandvägg
(Check Point, Defender och liknande) blockeras *inkommande* anslutningar oavsett
vad macOS egen brandvägg säger. Fällan är att `curl http://<din-ip>:8080` från
samma dator svarar direkt: den trafiken går över loopback och möter aldrig
filtret, så servern ser frisk ut medan lampan inte kommer fram.

Push går åt andra hållet, som en vanlig utgående anslutning från datorn, och
behöver därför ingenting öppnat. Når den ändå inte fram står det på styrsidan
(*når inte lampan*, med felet efter) — då är det lampans adress eller nätet som
är fel, inte brandväggen.

Servern trycker vid varje ändring plus ett hjärtslag var 20:e sekund. Varje push
förlänger en lease på fem minuter (`PUSH_LEASE_MS` i `include/config.h`). Slutar
servern höra av sig går lampan tillbaka till att hämta från SHL själv — det finns
inget läge att komma ihåg att slå av, och en glömd mockserver låser inte lampan.

Så länge leasen lever hämtar lampan ingenting på egen hand: ingen schemapollning,
ingen SSE-ström.

### Vad du kan ställa in

| På styrsidan | Vad det påverkar i lampan |
|---|---|
| **Push till lampan** | Lampans adress och på/av — allt annat kräver att den är på *och* att lampans felsökningsläge är på |
| Nästa match + nedsläpp | När matchfönstret öppnas (`LED_LIVE` istället för standby) |
| **Starta nu** | Lägger nedsläppet en minut bakåt → fönstret öppnas direkt |
| Mål Björklöven / motståndaren | Målfyrverkeriet |
| **Spela upp match** | Servern matar ut mål automatiskt tills matchen är slut |
| Vi vann igår / förlorade igår | Gnistorna ovanpå glöden |
| **Avsluta match** | Skriver ställningen som gårdagens resultat och flyttar fram nästa match |

Kolumnen *Vad lampan ser* visar om matchfönstret borde vara öppet och när läget
senast levererades. Längst ner loggas händelserna, inklusive när push slutar nå
fram.

**JSON-formatet** — allt är valfritt, det som utelämnas lämnas orört:

```json
{
  "next":  {"home": "IFB", "away": "LHF", "homeIsUs": true, "text": "IFB – LHF  lör 29 aug 19:00"},
  "live":  true,
  "score": {"home": 2, "away": 1},
  "last":  {"won": true, "text": "SAIK 2-4 IFB  (vinst)"}
}
```

`live` styr matchfönstret, `score` jämförs mot förra pushen och tänder
målfyrverkeriet vid ökning, och `last.won` tänder gnistorna. Första pushen efter
ett lägesbyte kalibrerar bara — annars hade en ny ställning sett ut som ett mål.

Endpointen har ingen autentisering, precis som resten av portalen. Den hör
hemma på ett labbnät, inte på ett öppet nät.

---

## 8. Justera beteendet

Allt sitter i `include/config.h`:

```c
#define YELLOW_G        224    // lägre = varmare gult (255 = citrongult)
#define GLOW_MAX_VAL    44     // hur ljus toppen av andetaget är
#define GLOW_BPM        6      // lägre = långsammare andetag
#define GLOW_MIN_VAL    20     // hårt golv — höj om glöden drar åt rött
#define LED_DITHER      BINARY_DITHER  // DISABLE_DITHER om du ser flimmer
#define GOAL_DURATION_MS 12000
#define GOAL_DELAY_DEFAULT_S 15        // tv-fördröjning, ändras på statussidan
#define SPARKLE_MEAN_INTERVAL_MS 700   // högre = färre gnistor
#define GOAL_ONLY_OUR_TEAM true        // false = fyra vid alla mål
```

Vill du testa effekterna utan att vänta på en match: knappen **"Testa
målfyrverkeriet"** på statussidan. Den tänder alltid direkt — en testknapp som
står tyst i 15 sekunder ser trasig ut.

### Tv-fördröjning

Live-datan från SHL kommer före tv-bilden. Hur mycket före beror på kedjan du
tittar i — marksänd tv, en strömmande tjänst och en kabelbox ligger olika långt
efter, oftast någonstans mellan 10 och 60 sekunder. Utan fördröjning tänder
lampan målet innan det syns på skärmen, och då är målet avslöjat för alla i
rummet.

Därför köas målfyrverkeriet. Fältet **Fördröjning på mål** på statussidan
sätter antalet sekunder:

| Värde | Effekt |
|---|---|
| `15` | Standard (`GOAL_DELAY_DEFAULT_S` i `include/config.h`) |
| `0` | Av — lampan tänder i samma stund som SHL rapporterar målet |
| upp till `180` | Taket, `GOAL_DELAY_MAX_S` |

**Ställ in den efter din egen skärm.** Sitt med matchen igång, notera hur många
sekunder det går mellan att ställningen tickar upp på statussidan (raden
*Ställning nu*) och att pucken går in på tv:n, och skriv in den siffran.

Detaljer värda att känna till:

- Fördröjningen gäller **alla mål från datakällan** — SSE-strömmen,
  reservpollningen och push-läget från mockservern. Det är bara testknappen som
  går förbi den.
- Kön rymmer sex mål samtidigt och hålls i tidsordning. Två mål inom
  fördröjningen tänds alltså med samma mellanrum som de gjordes — och landar de
  i samma bildruta staplar de på varandra precis som två snabba mål i realtid.
- Statussidan visar **Mål på gång** med nedräkning så länge något väntar. Står
  listen still fast *Ställning nu* redan tickat upp vet du varför.
- När matchfönstret stängs slängs kön. Ett mål som aldrig hann visas hör inte
  hemma i nästa match.
- Kön ligger i RAM. Startar lampan om under fördröjningen — eller kommer en
  OTA-uppdatering emellan — försvinner det köade målet.

---

## 9. Filer

```
platformio.ini          byggkonfiguration, två miljöer (USB + OTA)
include/config.h        all justerbar konfiguration
src/main.cpp            tillståndsmaskin, schemaläggning
src/leds.cpp            effekterna (glöd, gnistor, målfyrverkeri)
src/shl.cpp             SHL-API: HTTP(S)-poll + SSE-klient, val av datakälla
src/portal.cpp          captive portal + statussida
src/settings.cpp        NVS-lagring
src/netcheck.cpp        nätverksdiagnostik (DNS/TCP) för /debug
src/updater.cpp         signerad self-update från GitHub Releases
mock/server.py          mockserver för labbtest, styrsida på /
.github/workflows/
  release.yml           tagg v* -> bygg -> publicera release
  ci.yml                bygg varje push/PR
```

## Not om TLS

Enheten kör `setInsecure()` — inget certifikat valideras. shl.se ligger bakom
Cloudflare som roterar certifikatkedjan, och en hårdkodad rot-CA skulle göra
lampan tyst den dagen kedjan byts. Enheten läser bara publik matchdata och
skickar aldrig något känsligt. Vill du ändå ha validering: byt `setInsecure()`
mot `setCACert()` i `src/shl.cpp` och `src/updater.cpp`.

För firmware spelar det mindre roll numera — binären är signerad, se
[Signerad firmware](#signerad-firmware). En angripare som kan byta ut svaret kan
ändå inte producera något enheten installerar.
