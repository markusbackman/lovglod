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
| [Vann senast](#gnistor--vi-vann-senast-ovanpå-glöden) | Vita gnistor ovanpå glöden | Björklöven vann sin senaste match — lyser till nästa nedsläpp |
| [Match pågår](#led_live--match-pågår) | Bärnsten, dubbelt så snabbt andetag | Matchfönstret är öppet |
| [Slutspurt](#slutspurten--ovanpå-matchglöden) | Hjärtslag, guldregn eller anfallsvågor ovanpå glöden | Tajt match mot slutet av tredje |
| [Paus](#led_intermission--timglaset) | Bärnstensstapel som krymper mot mitten | Paus mellan perioderna |
| [Övertid](#led_overtime--dragkampen) | Guld mot blåvitt, gränsen slits fram och tillbaka | Övertid och straffar |
| [MÅL](#led_goal--målfyrverkeriet) | 7–22 s stroboskop och kometer | Mål — större ju viktigare, 15 s fördröjt så tv:n hinner ikapp |
| [Motståndarmål](#suck--motståndarmål) | Listen faller ihop och hämtar sig | Motståndaren gjorde mål |
| [Segerdans](#led_dance--segerdansen) | Guld och grönt jagar utåt från mitten | Slutsignal, Björklöven vann |
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

### Gnistor — "vi vann senast" (ovanpå glöden)

Inget eget läge utan ett lager som läggs ovanpå standby och live. Ungefär var
0,7:e sekund tänds en slumpad LED i kall vit (`SPARKLE_R`/`_G`/`_B`) och tonar
ut på ett par tiondelar. Intervallet slumpas runt medelvärdet så glittret
aldrig hittar en takt.

Gnistan **adderas** till glöden i stället för att blandas in i den. Blandad blev
den lika ljus som glöden råkade vara just då — i andetagets topp knappt dubbelt
så ljus som ytan under den, alltså nästan osynlig, medan samma gnista i
vändningen var tiofalt ljusare. Glittret tonade in och ut i takt med andetaget.
Adderat mättar det mot vitt oavsett var i cykeln det landar.

Tänds när `played-games` säger `WIN` på den senast spelade matchen, och lyser
**ända fram till nästa match** — inte bara dagen efter. Vinsten hör ihop med
matchen som kommer, inte med kalenderdygnet: spelar laget på lördag och nästa
gång på onsdag glittrar listen hela veckan emellan. Släcks när matchfönstret
för nästa match öppnar (nedsläpp minus `LIVE_WINDOW_PRE_MS`) — från den stunden
är listen den matchens, och att glittra i uppvärmningen vore att fira fel match.
Efter den matchen tänds gnistorna igen bara om den också blev en vinst.

Finns ingen nästa match i schemat — sommaruppehåll, eller ett schema som inte
gick att hämta — tar `SPARKLE_MAX_GAME_AGE_S` (14 dygn) vid, så att säsongens
sista vinst inte ligger och glittrar i juli. Fjorton dygn täcker
landslagsuppehållen, det längsta glappet mitt i en säsong.

Skruva med `SPARKLE_MEAN_INTERVAL_MS` (högre = färre) och `SPARKLE_DECAY`
(högre = kortare).

### `LED_LIVE` — match pågår

Samma glöd som standby, men piggare: andetaget dubbelt så snabbt (~3,3 s),
botten upplyft så listen aldrig går ner i mörkret, och nyansen dragen åt
bärnsten (`LIVE_YELLOW_G 165`). Den väntar. Gnistorna är redan släckta när
läget slår om — de slutar vid nästa nedsläpp — men lagret ritas även här, så
att en push som säger att vinsten står kvar syns också under match.

Det är **färgen** som bär skillnaden mot standby, inte styrkan. `GLOW_LIVE_LIFT`
sattes när andetaget toppade på 44 och betydde då drygt en tredjedel mer ljus;
mot dagens tak på 140 är samma sexton steg ett par procent och syns knappt. Ett
skifte från gult till orange läses däremot direkt, även i ögonvrån — och till
skillnad från ett ljusare läge kostar det ingenting i ström.

Läget slås på när matchfönstret öppnas — nedsläpp minus marginal enligt
spelschemat, eller när mockservern säger till.

### Vad lampan vet om matchen

Allt matchljus nedan bygger på det SHL:s live-ström faktiskt skickar. Det är
mindre än man tror, och inspelningen av ÖRE–IFB
(`mock/recordings/2026-09-24-OHK-IFB`) visar exakt hur lite:

| Signal | Hur ofta och hur sent |
|---|---|
| `liveState` — pågår, paus, övertid, avgjord | En gång per byte, 10–31 s efter att det hänt i hallen |
| `gameTime` — speltid i perioden | Var 20–60:e sekund, bara när klockan rört sig. Säger aldrig om klockan står. De sista sekunderna av en period kommer oftast inte alls. |
| Skott | Varje skott med lag, ~36 s efter skottet |
| Ställning | 54–77 s efter målet |

Därför räknar **ingenting sekunder mot en slutsignal**. Klockan duger till
"ungefär åtta minuter kvar", inte till "tio sekunder kvar" — de sista 26
sekunderna av tredje perioden på ÖRE–IFB rapporterades aldrig. Lampan gissar
heller aldrig framåt: den använder senaste klockramen som den är.

Allt som kan avslöja något visas med [tv-fördröjningen](#tv-fördröjning):
ställningen, paus och övertid. Slog listen om från hjärtslag till guldregn i
samma stund som SHL rapporterade målet, vore målet avslöjat innan fyrverkeriet —
och innan tv:n.

### Slutspurten — ovanpå matchglöden

Lampan räknar hela matchen fram en **intensitet** mellan 0 och 1, hur
spännande den tycker att det är:

```
intensitet = tid × (tajthet + läge) + skottryck
```

- **Tid** är noll fram till åtta minuter före slutet av tredje
  (`MOOD_RAMP_S`) och stiger sedan mot 1 längs en kurva där det mesta händer på
  slutet (`MOOD_CURVE 1.6`).
- **Tajthet** är 1 vid lika, 0,8 vid ett måls skillnad, 0,35 vid två och
  nästan inget därutöver. 5–1 med fem minuter kvar ger ingen slutspurt.
- **Läge** lägger till 0,2 när Löven leder med ett mål och 0,1 när de ligger
  under med ett.
- **Skottryck** är en avklingande räkning av skotten, halveringstid tre
  minuter. Många skott i en tajt match höjer intensiteten en bit redan innan
  slutminuterna (`MOOD_SHOT_WEIGHT`).

Intensiteten glider mot sitt nya värde över ungefär tio sekunder
(`MOOD_SMOOTH_MS`) i stället för att hoppa när en klockram kommer — mellan två
ramar kan det ha gått en minut i matchen.

När intensiteten passerar 20 % (`MOOD_THRESHOLD`) tonas slutspurten in över
matchglöden, och den är helt framme vid 45 %. Vilken slutspurt beror på
ställningen:

- **Lika — hjärtslag.** Dubbelslag som sprider sig från mitten. Pulsen går
  från vilopuls (`HEART_REST_BPM 55`) mot `HEART_MAX_BPM 140`, och färgen värms
  från guld mot rött (`MOOD_HEAT`).
- **Löven leder — guldregn.** Guldglöd där gnistorna tätnar med intensiteten,
  från en per sekund till ett fyrtiotal. Ingen slutnedräkning: den skulle gissa
  på en klocka lampan inte har.
- **Löven under — anfallsvåg.** Vågor rullar från Lövens ände mot
  motståndarens, allt snabbare — och ännu snabbare när Löven faktiskt skjuter
  mest.

På ÖRE–IFB, 3–3 in i tredje, hade hjärtslaget tonats in ungefär fem minuter
före slutet och slagit på drygt 80 % när perioden tog slut.

**Vilken ände är Lövens?** Hemmalaget har listens början, som i tv-grafiken
där hemmalaget står till vänster. Spelar Löven borta kommer anfallsvågorna och
dragkampens guld alltså från andra änden.

### `LED_INTERMISSION` — timglaset

En bärnstensstapel står över hela listen när pausen börjar och krymper mot
mitten medan tiden går, och korn faller ut mot kanterna. Sista minuten
pulserar den — dags att hämta chipsen.

Pausens längd står **inte** i datan. ÖRE–IFB hade 17:39 och 18:26, och 81 s
före övertiden. Timglaset rinner därför ut på en antagen längd, 17 minuter
(`PAUSE_EST_S`) eller 90 s före övertid (`PAUSE_OT_EST_S`), och när det är tomt
andas listen lugnt tills nästa period faktiskt rapporteras. Hellre ett glas som
väntar än ett som påstår att nedsläppet är nu.

Startar lampan mitt i en paus börjar glaset fullt — den vet inte när pausen
började.

### `LED_OVERTIME` — dragkampen

Guld från Lövens ände, motståndarens färg — kall blåvit, `OPP_R/G/B` — från
den andra. Gränsen mellan dem slits fram och tillbaka och gnistrar, och
skottrycket flyttar den: skjuter Löven mest tar guldet mark. Gäller både
övertid och straffar.

### `LED_GOAL` — målfyrverkeriet

Två faser, och **hur stort fyrverkeriet blir beror på hur mycket målet
betydde**:

1. **Stroboskop.** Hela listen blixtrar i ~14 Hz mot en dämpad gul botten,
   växelvis vitt och mättat gult. Det är den delen som får folk att titta upp.
2. **Eldgivning.** Kometer skjuts ut från listens mitt åt båda hållen
   samtidigt: vitglödande kärna med två gula svansled efter sig. Ovanpå det
   slumpade gnistregn så salvorna inte blir mekaniska.

Målets vikt räknas på ställningen efter målet, med samma intensitet som
slutspurten, plus ett tillägg för kvittering eller ledningsmål i tredje.
Övertidsmål avgör matchen och får alltid full vikt.

| | Tidigt i matchen | Testknappen | Avgörande mål |
|---|---|---|---|
| Vikt | 25 % (golvet) | 33 % | 100 % |
| Längd | ~11 s | 12 s | 22 s |
| Stroboskop | 1,9 s | 2,1 s | 4 s |
| Ny salva | var 165:e ms | var 150:e ms | var 60:e ms, varannan i grönt |

En kvittering till 2–2 med fem minuter kvar hamnar kring 55 %, samma mål med
två minuter kvar kring 85 %, och ett övertidsmål alltid på 100 %.

Gränserna sitter i `GOAL_MIN_MS`/`GOAL_MAX_MS` och grannarna i
`include/config.h`.

Sista 1,2 sekunderna tonas allt ner mot standby istället för att slockna tvärt.

**Fyrverkeriet är fördröjt 15 sekunder som standard.** SHL:s live-data är
snabbare än tv-sändningen, så utan fördröjning tänder lampan målet innan det
syns på skärmen — och alla i rummet vet att det gick in innan de får se det. Se
[Tv-fördröjning](#tv-fördröjning) nedan.

**Mål i rad staplar inte om från början.** Ett nytt mål under pågående
fyrverkeri förlänger bara, och vikten får bara växa — annars hade ett snabbt 2-mål
kastat tillbaka listen till stroboskopet och man hade tappat känslan av att det
var *två* mål.

Fyrverkeriet tänds bara på Björklövens mål — lampan hejar inte på fel lag.
Knappen **"Testa målfyrverkeriet"** på statussidan kör hela sekvensen när som
helst.

### Suck — motståndarmål

Motståndarens mål får inget fyrverkeri, men listen suckar: den faller ihop mot
nästan mörker på en halv sekund och hämtar sig långsamt under fyra
(`SIGH_MS`). Sucken köas med samma tv-fördröjning som våra mål och läggs bara
över de lugna lägena, aldrig över ett fyrverkeri.

### `LED_DANCE` — segerdansen

När strömmen säger att matchen är avgjord och Löven leder: guld- och gröna
block jagar utåt från mitten med gnistor ovanpå, i 45 sekunder (`DANCE_MS`)
medan laget tackar publiken. De sista fem sekunderna tonar den över i
segerläget, som då redan är tänt — lampan väntar inte på played-games när den
själv sett slutsignalen.

Ett övertidsmål som fortfarande väntar på tv-fördröjningen får brinna klart
först. Går matchen till straffar står ställningen lika när den avgörs, och då
vet lampan inte vem som vann: den dansar inte, och segerläget tänds som förut
när played-games bekräftar vinsten.

Förlust går direkt tillbaka till standby.

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

### Lampläge — alltid, bara match eller av

Fältet **Lampa** på statussidan bestämmer när listen får lysa:

| Val | Effekt |
|---|---|
| Alltid på | Standard. Allt ovan, dygnet runt |
| Bara match | Släckt mellan matcherna. Tänds 15 eller 30 min före nedsläpp och släcks vid slutsignalen |
| Av | Helt släckt |

Listen tonar ut och in över två sekunder (`LAMP_FADE_MS`). Bakom mörkret går
lampan som vanligt — den hämtar schema, följer live-strömmen och håller reda på
segrar — så att den är i fas i samma stund läget ändras.

- **Bara match** släcker när slutsignalen syns, med tv-fördröjningen som allt
  annat. Ett mål eller en segerdans som redan brinner får brinna klart, men
  segerläget efteråt och gnistorna fram till nästa match syns inte. Kommer
  slutsignalen aldrig fram släcks listen när matchfönstret stänger, 4 h efter
  nedsläpp.
- Med 30 minuter lyser vardagsglöden den första kvarten; matchljuset tar över
  när matchfönstret öppnar, 15 min före nedsläpp.
- Portalen syns alltid — utan den går lampan inte att ställa in. Demoläget på
  `/ljus` syns också, men inte testknappen för målet.
- Uppstartsflödet syns när lampan kopplas in eller startat om efter ett fel,
  fram till första hämtningen. Startar den om av sig själv — efter en
  OTA-uppdatering — eller tappar WiFi, förblir den mörk.

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
#define GOAL_MIN_MS      7000          // minsta målet …
#define GOAL_MAX_MS      22000         // … och det avgörande
#define MOOD_RAMP_S      (8 * 60)      // när slutspurten börjar byggas upp
#define MOOD_THRESHOLD   0.20f         // när den börjar synas
#define HEART_MAX_BPM    140           // hjärtslagets toppuls
#define PAUSE_EST_S      (17 * 60)     // timglasets antagna paus
#define GOAL_DELAY_DEFAULT_S 15        // tv-fördröjning, ändras på statussidan
#define SPARKLE_MEAN_INTERVAL_MS 700   // högre = färre gnistor
#define SPARKLE_MAX_GAME_AGE_S (14UL*24*60*60)  // tak när nästa match saknas
```

Vill du se matchljuset utan att vänta på en match: **Testa matchljuset** på
statussidan (`http://lovglod.local/ljus`) har en knapp per läge — slutspurterna,
paus, övertid, segerdans, mål och suck. Knappen låser listen i läget tills du
väljer Normal drift, eller tills tio minuter gått (`DEMO_TIMEOUT_MS`); under
tiden hämtar lampan ingen matchdata. Samma lägen finns på tangenterna i
seriemonitorn, `d` listar dem.

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
