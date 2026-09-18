# Bygga projektet

[← Tillbaka till README](../README.md)

För dig som vill bygga firmwaren själv, ändra i koden, testa utan match eller
släppa en ny version. Vill du bara få in en färdig binär i en lampa räcker
[Installera firmwaren](INSTALLERA.md).

- [Kom igång](#kom-igång)
- [Filer](#filer)
- [Labbtest utan match — mockservern](#labbtest-utan-match--mockservern)
- [Släppa en version (OTA)](#släppa-en-version-ota)
- [Datakällan](#datakällan)
- [Not om TLS](#not-om-tls)

---

## Kom igång

Du behöver [PlatformIO](https://platformio.org/install/cli) (CLI eller
VS Code-tillägget). Verktygskedja och bibliotek (FastLED, ArduinoJson) hämtas
automatiskt vid första bygget.

```bash
pio run                 # bygg
pio run -t upload       # flasha via USB — håll in BOOT, kortet har trasig auto-reset
pio device monitor      # seriell logg, 115200 baud
```

Lokala byggen får versionen `1.0.0-dev`, se
[Versionshantering](#versionshantering).

### Byggmiljöer

| Miljö | Vad den bygger |
|---|---|
| `esp32dev` (standard) | Hela firmwaren |
| `esp32dev_wiring` | Kopplingstest för listen — bara hårdvara, inget WiFi och ingen SHL-data |

```bash
pio run -e esp32dev_wiring -t upload && pio device monitor
```

Kör kopplingstestet först när en ny lampa är lödd: fungerar det är felet inte
i kablarna.

### Justera beteendet

Färger, andetag, målfyrverkeri och strömtak sitter i `include/config.h`.
Vad varje värde gör förklaras i [Ljusspråket](GLODEN.md#justera-beteendet).

---

## Filer

```
platformio.ini          byggkonfiguration: firmware + kopplingstest
include/config.h        all justerbar konfiguration
src/main.cpp            tillståndsmaskin, schemaläggning
src/leds.cpp            effekterna (glöd, gnistor, målfyrverkeri)
src/shl.cpp             SHL-API: HTTP(S)-poll + SSE-klient, val av datakälla
src/portal.cpp          captive portal + statussida
src/settings.cpp        NVS-lagring
src/netcheck.cpp        nätverksdiagnostik (DNS/TCP) för /debug
src/updater.cpp         signerad self-update från GitHub Releases
src/wiring_test.cpp     kopplingstest för listen, egen miljö (esp32dev_wiring)
include/ota_pubkey.h    publik nyckel för OTA-signaturen
tools/generate-ota-key.sh  skapar nyckelparet
mock/server.py          mockserver för labbtest, styrsida på /
docs/                   guiderna: montera, glöden, installera, bygga
hardware/v2/            STL-filer per utskriftsplatta, plus limfixturen
site/                   webbplatsen med utskriftsguide
.github/workflows/
  release.yml           tagg v* -> bygg -> publicera release
  ci.yml                bygg varje push/PR
```

---

## Labbtest utan match — mockservern

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

## Släppa en version (OTA)

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

### Token — bara för privata repon

Repot är publikt, så lamporna hämtar releaser utan inloggning och fältet
**GitHub-token** på statussidan ska vara tomt.

Pekar du lampan mot ett eget privat repo behövs en fine-grained token
(Contents: **Read-only**, bara det repot). Enheten hämtar då releasen via
`api.github.com/repos/OWNER/REPO/releases/assets/<id>` och följer redirecten
till den signerade nedladdningslänken för hand, så att token inte skickas
vidare. Token ligger i klartext i NVS och kan läsas ut av den som har lampan i
handen. Tomt fält vid senare sparningar betyder "rör inte", och `-` raderar.

### Sätta upp

1. Repot ligger på `markusbackman/bjorkloven-led`. Workflowsen är aktiva och
   releaser publiceras där.
2. Öppna lampans statussida och fyll i **Uppdateringskälla**:
   `markusbackman/bjorkloven-led` (standardvärdet).
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

### Betaprogrammet

Välj **Uppdateringskanal: Beta** på statussidan för att skriva in en lampa.
Valet ligger i NVS och överlever omstart och OTA.

| | Stabil | Beta |
|---|---|---|
| Källa (`owner/repo`) | `/releases/latest` | `/releases?per_page=5`, nyaste som inte är utkast |
| Källa (egen URL) | `…/firmware.json` | `…/firmware-beta.json` |
| Får | bara stabila releaser | pre-releases **och** stabila |
| Kollar | var 12:e timme | var 3:e timme |

Kanalen följer versionen. Taggar du med suffix blir releasen en pre-release och
når bara betalampor:

```bash
git tag v1.2.0-rc1 && git push origin v1.2.0-rc1   # bara beta
git tag v1.2.0     && git push origin v1.2.0       # alla
```

Betalampor tar den nyaste releasen oavsett sort, så när `v1.2.0` publiceras
efter `v1.2.0-rc1` går de över till den stabila och fortsätter därifrån.

**Dra tillbaka en trasig beta:** gör den till utkast eller ta bort den.
Betalamporna rullar då tillbaka till den näst nyaste vid nästa kontroll.

**Gå ur betan:** byt tillbaka till Stabil. Lampan kollar direkt och installerar
senaste stabila — även om det är en *äldre* version än den beta den kör. Nya
NVS-nycklar måste därför alltid tåla att äldre firmware ignorerar dem.

### Versionshantering

`FW_VERSION` sätts av CI via `-DFW_VERSION='"1.1.0"'`. `config.h` har ett
`#ifndef`-skydd, så lokala byggen får `1.0.0-dev` — vilket alltid skiljer sig
från en publicerad version och därför alltid uppdaterar vid första kollen.

Enheten uppdaterar när versionen **skiljer sig**, inte bara när den är nyare.
Det är avsiktligt och ger en gratis rollback: kryssa i *pre-release* på en
trasig release, så pekar `/releases/latest` tillbaka på den förra och lamporna
rullar tillbaka av sig själva vid nästa kontroll. Betalampor ser fortfarande
pre-releases — för att nå dem också, gör releasen till utkast i stället.

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

Webbgränssnittet har medvetet ingen inloggning — lampan är byggd för ett
hemmanät. Vem som helst på nätet kan därför peka om uppdateringskällan, ändra
inställningar eller radera WiFi-uppgifterna. Att de *inte* kan få något
installerat är signaturens förtjänst. Ska lampan stå på ett nät du inte litar
på behöver den HTTP Basic auth (`server.authenticate()`) med lösenord i NVS
och en CSRF-token i formulären.

Notera också vad borttagandet av push kostar: går en signerad uppdatering igenom
och visar sig trasig finns ingen väg tillbaka över nätet. Enda återvägen är USB
med BOOT-knappen intryckt. Det är argumentet för B5 (rollback).

---

## Datakällan

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

## Not om TLS

Enheten kör `setInsecure()` — inget certifikat valideras. shl.se ligger bakom
Cloudflare som roterar certifikatkedjan, och en hårdkodad rot-CA skulle göra
lampan tyst den dagen kedjan byts. Enheten läser bara publik matchdata och
skickar aldrig något känsligt. Vill du ändå ha validering: byt `setInsecure()`
mot `setCACert()` i `src/shl.cpp` och `src/updater.cpp`.

För firmware spelar det mindre roll numera — binären är signerad, se
[Signerad firmware](#signerad-firmware). En angripare som kan byta ut svaret kan
ändå inte producera något enheten installerar.
