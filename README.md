# Björklöven-lampan 🍃

ESP32 + WS2812B-list som lever med Björklöven:

| Läge | Ljus |
|---|---|
| **Standby** | Långsam gul glöd, ~10 s andetag, med organisk variation längs listen |
| **Vann igår** | Några glittrande gnistor ovanpå glöden |
| **Match pågår** | Piggare glöd, kortare andetag |
| **MÅL** | 12 s snabb gul eldgivning — stroboskop följt av kometer ut från mitten |
| **Setup** | Lugn blå puls (eget WiFi-nät uppe) |

Data hämtas från **SHL:s eget publika API**. Björklöven är uppflyttade till SHL
inför säsongen 2026/27 efter vinsten mot Karlskoga i HockeyAllsvenskans final,
så ligans officiella API är rätt källa.

---

## 1. Hårdvara

| Del | Anmärkning |
|---|---|
| ESP32 DevKit v1 (ESP32-WROOM-32) | Vilken klon som helst duger |
| WS2812B-list, 60 LEDs | 5 V, adresserbar (NeoPixel) |
| 5 V nätaggregat, ≥ 4 A | 60 LEDs på full vit ≈ 3,6 A |
| Motstånd 330–470 Ω | I serie på datalinjen |
| Kondensator 1000 µF / 6,3 V+ | Över 5 V och GND vid listens början |

### Koppling

```
   5V PSU ──┬──────────────► LED 5V
            │
          1000µF
            │
   PSU GND ─┴──┬───────────► LED GND
               │
        ESP32 GND

   ESP32 GPIO13 ──[390Ω]────► LED DIN
```

**Viktigt:**
- ESP32 och listen måste dela GND, annars blir datasignalen skräp.
- Mata *inte* 60 LEDs genom ESP32:ns 5V-pinne — dra 5 V direkt från nätaggregatet.
- ESP32:ns 3,3 V-signal räcker oftast till WS2812B. Vid glitch: sätt in en
  nivåomvandlare (74AHCT125) eller mata listen med 4,5 V istället för 5 V.
- Firmware håller sig under 3000 mA via `FastLED.setMaxPowerInVoltsAndMilliamps()`.
  Justera `LED_MAX_MILLIAMPS` i `include/config.h` efter ditt nätaggregat.

---

## 2. Bygg och flasha

```bash
pio run                 # bygg
pio run -t upload       # flasha via USB (första gången)
pio device monitor      # seriell logg, 115200 baud
```

## 3. Första start — captive portal

1. Lampan hittar inget sparat WiFi → **blå puls** och den startar eget nät
   `Bjorkloven-Setup-XXXX` (öppet).
2. Anslut med mobilen. Inloggningsrutan poppar upp av sig själv (iOS, Android
   och Windows kontroll-URL:er är hanterade). Gör den inte det: gå till
   `http://192.168.4.1`.
3. Välj nätverk i listan, skriv lösenord, spara.
4. Lampan ansluter och glöden blir gul. Uppgifterna ligger kvar i NVS och
   överlever både omstart och OTA.

Går anslutningen inte igenom öppnas portalen igen. Står portalen öppen utan att
någon gör något provar lampan de sparade uppgifterna på nytt var 5:e minut —
praktiskt om det bara var routern som startade om.

**Statussida** (när den är online): `http://bjorkloven-led.local` eller enhetens
IP. Där finns nästa match, senaste resultat, ljusstyrka, en knapp som testar
målfyrverkeriet, och "Glöm WiFi".

---

## 4. OTA-uppdatering

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

### Push-OTA under utveckling

```bash
pio run -e esp32dev_ota -t upload
```

Lösenord `***` (ändra `OTA_PASSWORD` i `config.h`). Snabbast när du
itererar på effekterna och inte vill tagga en release för varje ändring.

### Minne

GitHub-svaret parsas strömmande med ett ArduinoJson-filter, så bara `tag_name`
och asset-namnen behålls. För ett repo med 280 assets krymper svaret från
480 kB till 38 kB; för det här repot är det 308 byte.

### ⚠️ Säkerhet

Enheten kör `setInsecure()` — inget certifikat valideras, varken mot shl.se
eller GitHub. För matchdata spelar det ingen roll, men **för OTA betyder det
att någon som kan göra en MITM på ditt nät kan flasha godtycklig firmware.**

Certifikatpinning löser det dåligt här: kedjan hoppar mellan `api.github.com`
och `release-assets.githubusercontent.com` och roterar med jämna mellanrum — en
hårdkodad rot-CA gör bara lampan tyst den dagen den byts.

Rätt lösning är **signerad firmware**: generera ett RSA-nyckelpar, låt CI
signera `firmware.bin` och lägg in den publika nyckeln i binären via
`Update.installSignature()`. Då spelar transporten ingen roll — enheten
vägrar installera något som inte är signerat av dig. Det är inte implementerat
här; för en lampa på ett hemmanät är risken liten, men det är det du ska göra
om enheten ska stå någon annanstans.

## 5. Datakällan

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

## 6. Justera beteendet

Allt sitter i `include/config.h`:

```c
#define YELLOW_HUE      52     // 64 = rent gult, 42 = bärnsten
#define GLOW_BPM        6      // lägre = långsammare andetag
#define GLOW_MIN_VAL    18     // hur mörk botten i glöden är
#define GOAL_DURATION_MS 12000
#define SPARKLE_MEAN_INTERVAL_MS 700   // högre = färre gnistor
#define GOAL_ONLY_OUR_TEAM true        // false = fyra vid alla mål
```

Vill du testa effekterna utan att vänta på en match: knappen **"Testa
målfyrverkeriet"** på statussidan.

---

## 7. Filer

```
platformio.ini          byggkonfiguration, två miljöer (USB + OTA)
include/config.h        all justerbar konfiguration
src/main.cpp            tillståndsmaskin, schemaläggning
src/leds.cpp            effekterna (glöd, gnistor, målfyrverkeri)
src/shl.cpp             SHL-API: HTTPS-poll + SSE-klient
src/portal.cpp          captive portal + statussida
src/settings.cpp        NVS-lagring
src/updater.cpp         ArduinoOTA + self-update från GitHub Releases
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
