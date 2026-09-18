# Ljusspråket — förstå glöden

[← Tillbaka till README](../README.md)

LövGlöd pratar bara med ljus. Den här guiden går igenom varje läge listen kan
stå i — vad det betyder, hur det ser ut och varför det är ritat just så — och
avslutas med hur du skruvar på beteendet och ställer in tv-fördröjningen.

| Läge | Ljus | Betyder |
|---|---|---|
| [Uppstart](#led_boot--uppstartsflöde) | Gult flödar in och står tänt | Lampan startar — nu syns en död diod |
| [Setup](#led_portal--grön-puls) | Lugn **grön** puls | Inget WiFi sparat, anslut till lampans eget nät |
| [WiFi svarar inte](#led_portal_retry--röd-puls) | Lugn **röd** puls | Sparat WiFi finns men går inte att nå |
| [Ansluter](#led_connecting--jagande-gult) | Gul punkt som jagar runt | Kopplar upp mot WiFi |
| [Arbetar](#led_working--förloppsstapel-vid-uppstart) | Gul stapel som står still | Nätverkstest och första hämtningen |
| [Standby](#led_standby--den-långsamma-glöden) | Långsamt gult andetag, ~7 s | Vardag — här står lampan nästan jämt |
| [Vann igår](#gnistor--vi-vann-igår-ovanpå-glöden) | Vita gnistor ovanpå glöden | Björklöven vann i går |
| [Match pågår](#led_live--match-pågår) | Bärnsten, dubbelt så snabbt andetag | Matchfönstret är öppet |
| [MÅL](#led_goal--målfyrverkeriet) | 12 s stroboskop och kometer | Mål — 15 s fördröjt så tv:n hinner ikapp |
| [Uppdaterar](#led_updating--ota-förlopp) | Gul stapel som fylls | Ny firmware laddas ner |
| [Ingen data](#led_error--ingen-kontakt) | Svagt rött andetag | På WiFi, men SHL svarar inte |

---

## Lägena

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

Fyrverkeriet tänds bara på Björklövens mål. Motståndarens mål syns i
ställningen men rör aldrig listen — lampan hejar inte på fel lag. Knappen
**"Testa målfyrverkeriet"** på statussidan kör hela sekvensen när som helst.

### `LED_UPDATING` — OTA-förlopp

Listen fylls från början i takt med nedladdningen, med en snabbt pulserande LED
i fronten som visar att överföringen lever. Tar över alla andra lägen medan den
pågår, och den enda utgången är omstart in i den nya firmwaren.

Till skillnad från uppstartsstapeln **får** den här röra sig. En nedladdning är
en ström: förloppet uppdateras kontinuerligt och processorn är ledig, så ett
pulserande huvud är ärligt. Uppstartsstapeln står stilla för att den måste —
anropen den täcker fryser bilden ändå.

Kroppen ligger däremot dämpad (`UPDATE_BODY_VAL 40`, huvudet `UPDATE_HEAD_VAL
120`). En ljusare stapel drog tillräckligt med ström för att fälla enheten mitt i
nedladdningen. Samma golv på `BAR_MIN_LIT` dioder
gäller: utan det visas TLS-handskakningen mot
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

## Justera beteendet

Allt sitter i `include/config.h`:

```c
#define YELLOW_G        224    // lägre = varmare gult (255 = citrongult)
#define GLOW_MAX_VAL    140    // hur ljus toppen av andetaget är
#define GLOW_BPM        9      // lägre = långsammare andetag
#define GLOW_MIN_VAL    22     // andetagets botten
#define GLOW_FLOOR_VAL  15     // hårt golv per diod — höj om glöden drar åt rött
#define LED_DITHER      BINARY_DITHER  // DISABLE_DITHER om du ser flimmer
#define GOAL_DURATION_MS 12000
#define GOAL_DELAY_DEFAULT_S 15        // tv-fördröjning, ändras på statussidan
#define SPARKLE_MEAN_INTERVAL_MS 700   // högre = färre gnistor
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
