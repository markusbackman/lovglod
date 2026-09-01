# Produktionsklar — vad som saknas

Genomgång 2026-08-30 mot commit `64b1a71`. Bygget är rent
(`esp32dev`: flash 56,9 %, RAM 16,5 %) och animationslagret är den mest färdiga
delen av kodbasen. Det som står här är resten.

Radnumren gäller commit `64b1a71` och glider när koden ändras — sök på
funktionsnamnet om de inte stämmer.

**Minsta uppsättning för att våga flasha lamporna:** B1, B3, B5, R6, R7.
B2 krävs först om lampan ska stå på ett nät du inte äger; på ett hemmanät är det
B3 och B4 som stänger den realistiska vägen in.

---

## Blockerare

### B0. Enheten bootloopar på brownout innan den ens når WiFi

- [x] **Firmware, uppstart** — listen släcks före radiostart. Verifierat
      2026-08-30: 7 starter av 7 utan brownout, mot 4 av 4 med brownout före.
- [x] **Firmware, OTA** — förloppsstapeln dämpad, se nedan.
- [ ] **Hårdvara** — kvarstår. Marginalen är fortfarande tunn nog att LED-lasten
      ensam avgör om en OTA går igenom.

#### Nedladdningen var ett andra, hårdare strömfall (2026-09-01)

Uppstarten var en kort topp. Nedladdningen är ihållande WiFi-mottagning och
flashskrivning i tiotals sekunder, med en förloppsstapel som växer mot full list
just som nedladdningen hunnit längst — `UPDATE_BODY_VAL 157` gav ~0,87 A.

Uppmätt, samma enhet, samma release, enda skillnaden global ljusstyrka:

| Ljusstyrka | Utfall |
|---|---|
| 160 | brownout mitt i nedladdningen, 2 försök av 2, omstartsloop var ~64 s |
| 25 | nedladdningen gick igenom, installerad och omstartad |

Därmed är LED-lasten bevisad som orsak, inte en hypotes. `UPDATE_BODY_VAL`
sänkt till 40 och huvudet till 120, vilket ger ~0,22 A vid full stapel.

**Fixen provad under exakt de förhållanden som fällde den förut:** enheten körde
`v1.0.3` med den dämpade stapeln, global ljusstyrka tillbaka på 160, och hämtade
`v1.0.4` på under 24 sekunder utan brownout. Kvitterad som frisk vid tre
minuter. Två hela OTA-cykler i följd, båda rena.

#### Målfyrverkeriet fäller enheten vid standardljusstyrka (2026-09-01)

Provat, och det är värre än OTA-fallet. `drawGoal()` fyller hela listen med
nästan vitt (255,244,210) i stroboskopfasen — vid ljusstyrka 160 är det **~2,1 A**,
mot uppstartsflödets 1,4 A och OTA-stapelns 0,87 A. Dessutom växlar det mot
nästan släckt i 14 Hz, alltså en stor strömsvängning och inte en jämn last.

| Ljusstyrka | WiFi-last | Utfall |
|---|---|---|
| 160 | fyra parallella HTTP-strömmar | startade om |
| 160 | inaktiv | startade om |
| 25 (~0,33 A) | fyra parallella HTTP-strömmar | överlevde |

**WiFi är inte den avgörande faktorn** — stroboskopet ensamt räcker. Lampans
huvudfunktion kraschar alltså enheten vid standardinställningen.

Tillsammans med OTA-mätningarna ligger brytpunkten för LED-ström någonstans
mellan **0,33 A (fungerar) och 0,87 A (fäller)**. Det är långt under de 3000 mA
som `LED_MAX_MILLIAMPS` utlovar, och kommentaren där antar "ett 5 V/4 A-nät".
En total budget kring en halv ampere ser mer ut som USB-matning än som ett eget
nät — värt att mäta upp.

**Två vägar:**

1. **`LED_MAX_MILLIAMPS` till något matningen faktiskt klarar.** Det är precis
   vad FastLEDs strömtak är till för: den dimmar automatiskt ner för att hålla
   taket, så glöden — som ligger långt under — blir orörd medan bara topparna
   kapas. En konstant, och den täcker hela klassen av fall: fyrverkeri,
   uppstartsflöde, OTA-stapel, segerläge.
2. **Matning som klarar designens 3 A.** Då behöver ingenting i ljusspråket
   kompromissas.

Väg 1 gör lampan säker på vilken matning som helst men kostar topparnas
ljusstyrka. De två utesluter inte varandra.

Obs att en enhet som startar om under ett fyrverkeri inte bara tappar
animationen: sker det under valideringsfönstret efter en OTA (B5) tolkar
bootloadern det som ett misslyckat första försök och rullar tillbaka en frisk
firmware.

Uppmätt på skarp enhet 2026-08-30 (`/dev/cu.usbserial-0001`, v1.0.0-dev).
Fyra starter av fyra, alltid på samma millisekund:

```
[      0 ms] == Björklöven-lampan v1.0.0-dev ==
[   1018 ms] [wifi] ansluter till "Home"
[   1071 ms] Brownout detector was triggered
[   1079 ms] rst:0xc (SW_CPU_RESET)
```

1 018 ms är precis när `setup()` lämnar uppstartsflödet
(`BOOT_FILL_MS` + `BOOT_HOLD_MS`) och kallar `beginConnect()`. 54 ms senare slår
brownout-detektorn till. Enheten kommer alltså aldrig till `Online` — inget av
det som står nedan i den här filen går att verifiera på hårdvara förrän det här
är löst.

**Grundorsaken är strömförsörjningen**, men firmwaren väljer sämsta tänkbara
ögonblick att dra maximalt:

`drawBoot()` har vid 700 ms fört kanten förbi listens slut, så alla 60 dioder
står på `gold(255)` — och där står de kvar under `BOOT_HOLD_MS`. Vid
`LED_DEFAULT_BRIGHTNESS 160` blir det ~23,5 mA per diod, alltså **~1,4 A**, och
det är exakt den lasten som ligger på när WiFi-radion drar sin första
TX-burst. `LED_MAX_MILLIAMPS 3000` griper inte in, för 1,4 A ligger under taket.

`setMode(LED_CONNECTING)` sker dessutom *efter* `WiFi.begin()` i
`beginConnect()` (`src/main.cpp:477`), så listen hinner aldrig rita om till det
betydligt snålare jagande ljuset innan smällen.

**Fix, hårdvara (nödvändig):** 5 V-nät som orkar med list + kort, och en
elektrolyt på minst 1000 µF över listens matning.

**Fix, firmware — applicerad 2026-08-30.** Ny `Leds::blank()` som fyller svart
och skriver ut direkt, anropad i `beginConnect()` före `WiFi.mode()`/`begin()`
och i `enterPortal()` före `startAccessPoint()`.

Att bara byta läge räcker *inte*, vilket den ursprungliga formuleringen här
missade: `drawConnecting()` inleder med `fadeToBlackBy(28)` och sänker en fulltänd
list med ~11 % per bildruta. Den måste släckas, inte tonas.

Mätning efter fix: 6 omstarter via seriekommandot `r` plus en power-on, alla
rena, anslutning ~198 ms efter uppstartsflödet — och det på **-74 dBm**, alltså
svagare signal och högre sändareffekt än de -69 dBm som kraschade före.

### B1. Statussidan går inte att nå efter första uppsättningen

- [x] Fixad 2026-08-30 — rutterna registreras en gång, läget avgörs i handlern

Verifierat på enhet i stationsläge: `/` → statussidan, `/debug` → felsökning,
captive-probe-URL:erna → 404 (de betyder bara något i portalläge), okänd URL →
404. Själva övergången portal → station är *inte* provad på hårdvara — det
kräver att WiFi-uppgifterna raderas och skrivs in på nytt.

`registerRoutes()` (`src/portal.cpp:382`) körs en gång till från
`startStationServer()`, men `WebServer` tar aldrig bort handlers:
`_addRequestHandler` lägger sist i listan och `Parsing.cpp:126` väljer den
**första** som matchar. Har enheten passerat portalen är `GET /` alltså fortfarande
bunden till `handleSetup` — WiFi-formuläret — för all framtid.

Träffar exakt den vanligaste vägen in: första uppsättningen, och varje start där
routern var nere. Dessutom läcker varje WiFi-återanslutning ytterligare åtta
handler-objekt som aldrig frigörs.

**Fix:** registrera rutterna en enda gång och låt handlers avgöra läget själva
via `gApMode`, alternativt lägg till en egen route-lista som går att nollställa
i `stop()`.

### B2. OTA har ingen integritetskontroll alls

- [x] Kod klar 2026-08-30 — sha256 + RSA-2048-signatur, båda före commit
- [x] **Verifierad skarpt 2026-08-31**: enheten hämtade, kontrollerade och
      installerade `v1.0.2` från det privata repot, via signerade asset-URL:er
      för både manifest och binär. Att den kör versionen *är* beviset — koden
      installerar inget som inte passerat både sha256 och signatur.

Var: `setInsecure()`, ingen signaturkontroll, och den sha256 som CI redan
räknade fram lästes aldrig av enheten.

**Så här löstes det.** Två kontroller, båda före `Update.end()`:

| Kontroll | Fångar | Källa |
|---|---|---|
| `sha256` | trasig eller avbruten nedladdning | `sha256` i `firmware.json` |
| RSA-2048 PKCS#1 v1.5 över SHA-256 | allt annat, inklusive MITM | `sig` i `firmware.json` |

`httpUpdate` gick inte att använda: den anropar `Update.end()` själv och startar
om, och då är boot-partitionen redan satt. Nedladdningen drivs därför för hand i
`downloadAndInstall()` — hashen räknas på strömmen från nätet, och faller någon
kontroll anropas `Update.abort()`, som lämnar den gamla partitionen orörd.

Failar stängt: saknas `sha256` eller `sig`, eller går den publika nyckeln inte
att tolka, installeras ingenting. Ingen väg tillbaka till osignerat.

Nyckelhantering i `tools/generate-ota-key.sh`. CI signerar med secreten
`OTA_SIGNING_KEY` och verifierar sedan mot den publika nyckel som ligger i
binären, så ett nyckelpar som glidit isär fångas i bygget.

**Verifierat lokalt** (`openssl`, mot den skarpa binären): signaturen är 256
byte / 344 tecken base64; CI-kontrollen mot `include/ota_pubkey.h` går igenom;
en signatur skapad över *filen* verifierar mot den *32-byte digesten* — vilket
är exakt vad `mbedtls_pk_verify(..., MBEDTLS_MD_SHA256, digest, 32, ...)` gör på
enheten; och en enda ändrad byte avvisas.

Dessutom kompilerat och kört mot mbedTLS lokalt med `include/ota_pubkey.h`
oförändrad: PEM:en tolkas till RSA 2048, `pk_verify` godkänner den skarpa
signaturen och avvisar en manipulerad digest. Ett förbehåll: lokalt fanns
mbedTLS 3.6.2, enheten kör 2.28.7. Anropen som används är identiska mellan
versionerna, men det är inte samma binär.

Push-vägen som tidigare gick förbi hela den här kontrollen finns inte längre,
se B4.

### B3. Webbgränssnittet saknar autentisering och CSRF-skydd

- [ ] Fixad

Rutterna i `src/portal.cpp:396-407` är helt öppna. Vem som helst på nätet kan:

- `POST /settings` — peka om `otasrc` till egen server
- `POST /update` — och därmed installera godtycklig firmware
- `POST /forget` — radera WiFi-uppgifterna

Det är dessutom vanliga formulär-POST:ar, så CORS stoppar inte en webbsida som
någon på nätet råkar besöka från att nå `http://bjorkloven-led.local/settings`.

**Fix:** HTTP Basic auth på de muterande rutterna (`server.authenticate()`) med
ett lösenord i NVS, plus en CSRF-token i formulären. Statussidan kan gärna vara
öppen.

### B4. OTA-lösenordet är hårdkodat i källkoden

- [x] Löst 2026-08-30 genom att **ta bort hela push-vägen**

`OTA_PASSWORD "***"` låg i `include/config.h` och `--auth=***`
i `platformio.ini`.

> **Rättelse 2026-08-31.** Posten sa "i ett publikt repo". Repot är privat.
> Exponeringen var alltså mindre än jag skrev — men lösenordet låg också i
> klartext i varje binär, och det räcker: den som fått tag i en lampa eller en
> release-fil har det. Slutsatsen står kvar.

Efter B2 var ArduinoOTA-push dessutom den enda kvarvarande vägen till godtycklig
firmware på en lampa i nätet — signaturkontrollen gäller bara pull.

Valet blev att ta bort vägen i stället för att lösenordsskydda den. Ett
lösenord hade gjort den svårare att attackera; borttagandet gör att den inte
finns att attackera. Kvar är signerad self-update och USB, och den som står vid
USB-porten är ändå förbi varje mjukvaruspärr.

Borttaget: `ArduinoOTA`-anropen och `beginPush()`/`loop()` i `src/updater.cpp`,
anropen i `src/main.cpp`, `OTA_PASSWORD` i `include/config.h` och hela
`[env:esp32dev_ota]` i `platformio.ini`. Binären krympte 7,8 kB flash och
1,7 kB RAM.

**Konsekvens att vara medveten om:** går en signerad uppdatering igenom och visar
sig trasig finns ingen väg tillbaka över nätet. Enda återvägen är USB med
BOOT-knappen. Det gör B5 (rollback) mer angeläget, inte mindre.

### B5. Rollback finns, men släpper igenom det fall vi bryr oss om

- [x] Kod klar 2026-08-31 — kvittensen uppskjuten, friskkriterium WiFi + upptid
- [x] **Verifierad skarpt 2026-09-01**: efter OTA till `v1.0.3` visade
      statussidan `1.0.3 — på prov, inte kvitterad` i tre minuter, och flaggan
      försvann vid upptid 3 min — exakt `OTA_VALIDATE_AFTER_MS`.

> **Rättelse 2026-08-31.** Posten påstod tidigare att rollback inte var påslaget
> i bootloadern. Det var fel — det är påslaget. Verifierat i den sdkconfig.h som
> faktiskt kompileras mot: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1` och
> `CONFIG_APP_ROLLBACK_ENABLE`. Problemet är ett annat och mer subtilt.

**Så fungerar det idag.** Bootloadern kör hela tillståndsmaskinen:
`esp_ota_set_boot_partition()` (som `Update.end()` anropar) sätter `IMG_NEW`,
bootloadern gör om det till `IMG_PENDING_VERIFY` vid första start, och står det
kvar i `PENDING_VERIFY` vid *nästa* start blir det `IMG_ABORTED` — den
partitionen väljs aldrig mer.

Men Arduino-kärnan kvitterar direkt. I `initArduino()`
(`cores/esp32/esp32-hal-misc.c:223`), alltså **innan `setup()` ens körts**:

```c
if (!verifyRollbackLater()) {
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (verifyOta()) esp_ota_mark_app_valid_cancel_rollback();
        else             esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}
```

`verifyOta()` och `verifyRollbackLater()` är **weak** och går att överskugga;
default är `true` respektive `false`. Vi överskuggar ingendera, så varje
uppdatering godkänns i samma ögonblick den nått fram till Arduino-uppstarten.

**Vad som därmed skyddas — och inte.** Rollback fångar idag bara en binär som
inte startar alls: trasig magic, felaktig checksumma, panik före
`initArduino()`. Det fall vi faktiskt är rädda för — *startar fint men är
oanvändbar*: får inte upp WiFi, kraschar i `loop()`, når aldrig SHL — går rakt
igenom och kvitteras som frisk.

**Fix.** Överskugga `verifyRollbackLater()` till `true` så att kvittensen skjuts
upp, och anropa `esp_ota_mark_app_valid_cancel_rollback()` själva först när
enheten bevisat sig. Plus en tidsgräns som startar om om den aldrig gör det —
utan omstart kommer bootloadern aldrig åt att rulla tillbaka.

**Tre saker som måste vägas in:**

1. **Vad räknas som frisk?** Frestande att kräva en lyckad SHL-hämtning, men då
   rullar en fungerande firmware tillbaka bara för att routern var nere — och
   den gamla klarar sig inte bättre, så det blir ett pingpong. Kriteriet bör
   handla om firmwaren, inte om omvärlden.

2. **B0 slår in här.** Enheten är brownout-benägen. En oväntad reset under
   valideringsfönstret ser för bootloadern ut som ett misslyckat första försök
   och rullar tillbaka en helt frisk uppdatering. Fönstret bör vara kort, eller
   hårdvarudelen av B0 löst först.

3. **Ingen ny OTA under fönstret.** `esp_ota_set_boot_partition()` returnerar
   `ESP_ERR_OTA_ROLLBACK_INVALID_STATE` så länge nuvarande image står i
   `PENDING_VERIFY`. Ett långt fönster blockerar alltså nästa uppdatering.

**Extra angeläget nu:** sedan push-vägen togs bort (B4) är rollback den enda
automatiska återvägen. Utan den är det USB med BOOT-knappen som gäller.

---

**Så här löstes det.**

`extern "C" bool verifyRollbackLater() { return true; }` i `src/main.cpp`
skjuter upp kärnans automatiska kvittens. Verifierat i den länkade binären att
den starka symbolen vinner över kärnans svaga:

```
40187550 T verifyRollbackLater      <- vår
401881fc W verifyOta                <- kärnans, orörd

40187550 <verifyRollbackLater>:
  entry a1, 32 ; movi.n a2, 1 ; retw.n     <- returnerar true
```

`serviceOtaValidation()` kvitterar med
`esp_ota_mark_app_valid_cancel_rollback()` när **WiFi varit uppe och
`OTA_VALIDATE_AFTER_MS` (3 min) gått utan omstart**. Nätverksberoende kriterier
valdes bort: en firmware som fungerar ska inte rullas tillbaka för att routern
låg nere, för den gamla klarar sig inte bättre och det blir pingpong.

Blir den aldrig frisk startar `OTA_VALIDATE_DEADLINE_MS` (10 min) om enheten med
flit — utan omstart kommer bootloadern aldrig åt att rulla tillbaka. Undantag:
saknas sparat WiFi väntar lampan på en människa, och då kvitteras den i stället
för att straffa en frisk version.

**Bokföring.** `otaPendingVersion` i NVS namnger versionen som är på prov.
Rullar bootloadern tillbaka den läser den gamla firmwaren strängen vid start och
kallar `noteOtaRollback()`, som blockerar versionen **direkt** i stället för
efter tre försök — en binär som installerades, startade och ändå inte dög har
haft sin chans, och varje nytt försök kostar 1 MB och två omstarter.

En fälla som täpptes på vägen: en vanlig USB-flash mitt i ett pågående prov ser
ut precis som en rollback. Därför kontrolleras `esp_ota_get_last_invalid_partition()`
— finns ingen utdömd partition var det ingen rollback, och versionen glöms i
stället för att svartlistas.

**Medan provet pågår hoppas OTA-kontrollen över**, eftersom
`esp_ota_set_boot_partition()` ändå vägrar i `PENDING_VERIFY`. Statussidan visar
`på prov, inte kvitterad` intill firmwareversionen.

---

## Robusthet

### R6. En enda studs hos SHL ger sex timmars rött ljus

- [ ] Fixad

`refreshSchedule()` sätter `gNextScheduleFetch = millis() + POLL_SCHEDULE_MS`
högst upp (`src/main.cpp:285`), innan den vet hur det gick. Misslyckas båda
anropen går listen till `LED_ERROR` och står där i sex timmar.

**Fix:** vid misslyckande, sätt om till kort återförsök (5–15 min) med
exponentiell backoff upp mot det ordinarie intervallet.

### R7. NTP-fel är permanent

- [x] Fixad och verifierad på enhet 2026-08-30

`syncTime()` anropas bara från `goOnline()` (`src/main.cpp:251`). Misslyckas den
står `gTimeSynced` kvar på false, och matchfönstret, segerläget och "vann igår"
är alla döda tills en WiFi-återanslutning råkar köra `goOnline()` på nytt.

**Fix:** försök om i `loop()` så länge `gTimeSynced` är false, t.ex. var femte
minut.

**Inträffade skarpt 2026-08-30**, direkt efter en USB-flashning:

```
[   1206 ms] [wifi] ansluten, IP 192.168.1.250
[  11240 ms] [tid] NTP misslyckades
```

Enheten fortsatte som vanligt — hämtade schema och resultat, statussidan såg
frisk ut — men utan klocka är `insideLiveWindow()`, `victoryActive()` och
`maybeArmVictory()` alla permanent false. Lampan hade alltså aldrig gått in i
matchläge eller tänt segerläget, och ingenting hade synts utåt förrän matchen
kom och gick utan att listen reagerade.

Det gjorde R7 till den otäckaste posten i listan: tyst, läker inte, och träffar
exakt de lägen lampan finns till för.

**Så här löstes det.** `serviceTimeSync()` i `src/main.cpp` prövar var
`TIME_RETRY_MS` (5 min) så länge klockan inte är satt. SNTP-klienten som
`configTzTime()` startade fortsätter fråga på egen hand, så oftast räcker det
att titta efter igen — men vi startar ändå om den, för gick namnuppslaget i
väggen just när WiFi kom upp hjälper ingen väntan.

Kommer klockan i efterhand hämtas matchdatan om, eftersom det som hämtades utan
klocka är räknat mot 1970: "vann igår" jämförde mot fel datum och
`maybeArmVictory()` hoppade över allt.

Statussidan har dessutom en **Klocka**-rad. Hela problemet var att felet inte
syntes utåt; nu står det "INTE synkad — matchläge, mål och seger är avstängda"
i klartext.

**Verifierat med ett konstruerat prov:** första NTP-servern satt till den
orutbara `192.0.2.1`, andra kvar på `time.google.com`. Då hinner den blockerande
tiosekundersväntan inte lyckas, men bakgrundsklienten gör det strax efter —
exakt scenariot ovan:

```
[  11.2 s] [tid] NTP gick inte fram — prövar vidare i bakgrunden
[  12.9 s] [shl] senaste: BIK 3-7 IFB  (vinst)      <- hämtat utan klocka
[  26.2 s] [tid] synkad: 2026-08-30 18:13
[  26.2 s] [tid] hämtar om matchdata som räknades utan klocka
[  29.8 s] [shl] nästa: TIK – IFB  Fri 11 Sep 18:00 <- omräknat med klocka
```

Skarp konfiguration därefter: synkad efter 0,7 s, `Klocka`-raden visar tiden.

### R8. `gNextScheduleFetch = 0` slutar fungera mellan 24,9 och 49,7 dygns upptid

- [ ] Fixad

`(int32_t)(millis() - 0) >= 0` är falskt så fort `millis()` passerat 2^31. De
fyra ställen som tvingar fram en hämtning genom att nolla variabeln
(`src/main.cpp:473`, `:523`, `:603`, `:619`) blir därmed tysta no-ops i ett
25-dygnsfönster var femtionde dygn.

Rollover hanteras korrekt överallt annars — det är sentinelvärdet som är fel,
inte jämförelsen.

**Fix:** en egen `bool gFetchNow` i stället för att nolla tidsstämpeln.

### R9. All nätverkstrafik blockerar `loop()`

- [ ] Fixad

`apiGet` kan hålla i ~20 s (8 s anslutning + 12 s läsning) utan ett enda
`Leds::render()` däremellan. Fyra gånger per dygn är det en fryst glöd. Under
match kör `pollLiveScore` var 45:e sekund **utan någon spärr mot att `LED_GOAL`
pågår** — reservpollningen kan alltså frysa målfyrverkeriet mitt i.

Det här är den enda strukturella punkten i listan. Snabb lindring: hoppa över
reservpollningen medan `Leds::mode() == LED_GOAL`. Riktig fix: flytta all
hämtning till en egen FreeRTOS-task på core 0 och låt `loop()` bara rendera.

### R10. `apiGet` kapar kroppen i efterhand

- [ ] Fixad

`http.getString()` buffrar hela svaret och `body.remove(maxBytes)`
(`src/shl.cpp:58`) klipper sedan mitt i JSON:en — resultatet blir korrupt indata
i stället för ett rent fel.

Uppmätt 2026-08-30: `played-games` = 5 616 byte och **begränsas till 5 matcher**
av servern, `upcoming-games` = 5 372 byte. Alltså ofarligt i dag, men det är ett
antagande som servern äger och vars felläge är tyst korruption.

**Fix:** strömma svaret genom det befintliga ArduinoJson-filtret, som
`src/updater.cpp:141` redan gör.

### R11. `today-games` parsas utan filter

- [ ] Fixad

`pollLiveScore` (`src/shl.cpp:290`) är den enda hämtningen utan
`DeserializationOption::Filter` — och den kör medan både en SSE-TLS-session och
en HTTPClient-TLS-session är uppe.

**Fix:** samma filtermönster som `fetchNextGame`.

---

## Drift och det som inte är verifierat

### D12. SSE-formatet har aldrig setts med riktig data

- [ ] Verifierat

README säger det själv. Skyddsnätet är 45-sekunderspollningen — som är precis det
som fryser målanimationen (R9). Först riktiga match: 11 sep 2026.

**Att göra:** öppna `http://bjorkloven-led.local/debug` under matchen, läs av den
råa ramen och komplettera `findScorePair()` i `src/shl.cpp` med de faktiska
fältnamnen.

### D13. Hemligheter ligger i klartext

- [ ] Åtgärdad

GitHub-PAT och WiFi-lösenord ligger okrypterat i NVS, ingen flash-kryptering.
`/debug` skriver dessutom ut tokenens längd. Fysisk åtkomst = tokenen är röjd.

**Fix:** flash-kryptering, eller en PAT med så snäv scope att den inte är värd
något utanför lampan.

### D14. Setup-portalen är ett öppet nät över okrypterad HTTP

- [ ] Åtgärdad

`AP_PASSWORD ""` (`include/config.h:285`). Användarens WiFi-lösenord skickas
alltså i klartext över ett öppet nät.

**Fix:** WPA2 på AP:n med ett lösenord tryckt på lampan, eller åtminstone en
notis i README om att uppsättningen bör göras när ingen lyssnar.

### D15. Inga tester

- [ ] Åtgärdad

CI kompilerar och inget mer. `parseIso8601Utc`, `daysFromCivil`,
`findScorePair`, `normalizeVersion` och segerfönstrets logik är rena funktioner
som går att köra på värddatorn under `pio test -e native` på några minuter — och
`findScorePair` är den mest spekulativa koden i hela repot.

### D16. CI täcker inte allt

- [ ] Åtgärdad

`.github/workflows/ci.yml:33` bygger bara `esp32dev`. `esp32dev_wiring` byggs
aldrig och kan ruttna tyst. Ingen `pio check` heller.

### D17. Ingen felsökningsdata efter en krasch

- [x] Delvis åtgärdad 2026-09-01 — omstartsorsak visas på statussidan

Ingenting sparade `esp_reset_reason()`. Det slog till på riktigt under
strömfelsökningen: utan seriekabel gick det inte att skilja en brownout från en
krasch, vilket är precis den skillnad man behöver när matningen misstänks.

Statussidan visar nu **Senaste omstart**, markerad när den är onormal. En
brownout på ESP32 syns som `SW_CPU_RESET` i ROM-loggen — avbrottet skriver ut
sin varning och gör en mjuk omstart — men ESP-IDF lämnar en hint efter sig, så
`esp_reset_reason()` svarar ändå `ESP_RST_BROWNOUT`. Verifierat i
`libesp_system.a` att brownout-hanteraren anropar `esp_reset_reason_set_hint`.

**Kvar:** ingen starträknare i NVS, så en lampa som startar om var tionde minut
ser likadan ut som en som gjort det en gång. Orsaken syns, frekvensen inte.

---

## Det som redan håller

Inte en att göra-lista — noterat så att det inte rivs upp av misstag.

- Versionssträngen injiceras av CI och verifieras faktiskt inne i binären
  (`release.yml:82`), och partitionstaket kontrolleras före release.
- OTA-backoffen räknas per version, så en ny release får alltid ett ärligt försök.
- Segerfönstret överlever omstart och OTA via NVS.
- `millis()`-rollover hanteras korrekt överallt utom R8.
- Captive-portalen svarar på operativsystemens probe-URL:er, så inloggningsrutan
  öppnas av sig själv.
- `netcheck.cpp` är riktig fältdiagnostik — skiljer DNS, routing och TLS åt.


---

## Väntar på hårdvara

Enheten är urkopplad. Det här är vad som ska provas när den är tillbaka,
i ordning.

### 0. Släppkedjan — VERIFIERAD 2026-08-31

Committat, pushat, `OTA_SIGNING_KEY` satt, `v1.0.2` taggad och byggd av CI.
Release-workflowen signerade och verifierade mot `include/ota_pubkey.h` (den
kontrollen hade fällt bygget om nyckelparet glidit isär). Den publicerade
releasen kontrollerad lokalt:

| Kontroll | Resultat |
|---|---|
| Assets | `firmware.bin`, `firmware.json`, `firmware.sig` |
| `sha256` i manifestet mot binären | matchar |
| Signatur, 256 byte, mot nyckeln i enhetens firmware | `Verified OK` |

Kvar är enhetens egen halva: att hämta, kontrollera och installera.

**Obs att repot är privat.** Enheten går därför via `resolvePrivateAssetUrl()`
för *både* binären och manifestet — den mest komplicerade grenen i `updater.cpp`
och helt oprövad. Det är den vägen provet nedan faktiskt testar.

**Gratis negativt prov:** de gamla releaserna `v1.0.0` och `v1.0.1` från
2026-08-24 byggdes med den förra workflowen och har `sha256` men **ingen `sig`**.
Pekas enheten mot en av dem ska den vägra med "Releasen saknar sha256 eller
signatur — installerar inte", utan att ladda ner en enda byte.

### 1. OTA end-to-end — har aldrig körts skarpt

Viktigast av allt. Self-update har aldrig gått igenom en enda gång; statussidan
har hela tiden sagt "Ingen kontroll gjord än". Nu ligger dessutom
signaturkontroll ovanpå, så två oprövade saker testas samtidigt om man inte är
noggrann.

Förberedelse: `gh secret set OTA_SIGNING_KEY < ota_signing_key.pem`, committa
`include/ota_pubkey.h`, flasha enheten över USB med den nya firmwaren (den måste
bära den publika nyckeln *innan* den kan ta emot en signerad release), tagga
`v1.0.1`.

Sedan, på `/` → "Sök efter uppdatering nu". Förväntat i seriekonsolen:

```
[ota] 1.0.1-dev → 1.0.1
[ota] hämtar …
[ota] sha256 OK
[ota] signatur OK
[ota] klar — startar om i den nya firmwaren
```

Kolla också att förloppsstapeln rör sig under nedladdningen och att
statussidan visar den nya versionen efter omstart.

### 2. OTA negativa prov

Görs enklast genom att lägga en trasig `firmware.json` över den riktiga med
`gh release upload v1.0.1 firmware.json --clobber`:

- **Fel signatur** (ändra ett tecken i `sig`) → `Signaturen stämmer inte`,
  enheten står kvar på nuvarande firmware och bootar normalt efteråt. Det här är
  provet som visar att `Update.abort()` verkligen lämnar partitionen orörd.
- **Fel sha256** → `sha256 stämmer inte`, eget felmeddelande skilt från ovan.
- **`sig` borttagen** → vägrar innan en enda byte laddats ner.

### 3. B1 — övergången portal → station

Den enda delen av B1 som inte gick att prova. Kräver att WiFi-uppgifterna
raderas, så ha lösenordet till nätet framme: seriekommandot `w`, anslut till
`Bjorkloven-Setup-XXXX`, skriv in uppgifterna, och kontrollera sedan att `/`
visar **statussidan** och inte setup-formuläret. Det var det ursprungliga
symptomet.

### 4. B5 — rollback

Kan inte provas förrän OTA fungerar (punkt 1), och delar testflöde med det.

- **Lyckad väg:** efter en OTA ska statussidan visa `på prov, inte kvitterad`
  och seriekonsolen `[ota] ny firmware på prov`. Efter tre minuter med WiFi:
  `[ota] kvitterad som frisk`. Starta om — den nya versionen ska vara kvar.
- **Rollback-vägen:** bygg en testrelease med `OTA_VALIDATE_AFTER_MS` orimligt
  högt och `OTA_VALIDATE_DEADLINE_MS` kort (t.ex. 2 min). Då kan den aldrig bli
  frisk i tid, och maskineriet ska sluta i en avsiktlig omstart följd av att
  bootloadern rullar tillbaka. Ingen trasig binär behöver konstrueras.
- **Efter rollbacken:** den gamla firmwaren ska logga
  `[ota] … rullades tillbaka av bootloadern`, och `/debug` ska visa versionen
  som misslyckad så att den inte hämtas igen var tolfte timme.

### 5. ~~otadata vid USB-flash~~ — avskriven, var ett feltolkat fynd

Jag påstod att PlatformIO bara skriver appen vid USB-flash och att en
USB-flash därför kunde se ut att inte ta efter en OTA. **Det var fel**, och
byggde på en grep i plattformens byggskript som missade steget.

Den faktiska uppladdningsloggen 2026-08-31 visar att otadata skrivs:

```
Wrote 8192 bytes (47 compressed) at 0x0000e000    <- otadata / boot_app0.bin
Wrote 1115648 bytes at 0x00010000                 <- appen, app0
```

USB-flash nollställer alltså otadata till app0 och slår alltid igenom, även
efter en OTA som flyttat enheten till app1. Ingen `erase` behövs, och den
manuella återvägen är intakt. Inget att testa.

### 6. ~~B0 — det värsta strömfallet~~ — provat 2026-09-01, se B0 ovan

Firmwarefixen täcker uppstarten. Målfyrverkeriet är en tyngre last än så: hela
listen på full styrka i stroboskopfasen, samtidigt som WiFi sänder. Det har
aldrig körts på den här matningen.

Prov: `/` → "Testa målfyrverkeriet", och håll seriekonsolen öppen. Ingen
`Brownout detector was triggered` under de 12 sekunderna. Faller den där är det
hårdvarudelen av B0 som måste lösas, inte firmwaren.
