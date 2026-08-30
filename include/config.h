#pragma once
#include <Arduino.h>

// ─────────────────────────────────────────────────────────────
//  Firmware
// ─────────────────────────────────────────────────────────────
// Sätts av CI vid release (-DFW_VERSION='"1.2.3"'). Lokala byggen får -dev,
// vilket alltid skiljer sig från en publicerad version och därför alltid
// uppdaterar vid första OTA-kollen.
#ifndef FW_VERSION
#define FW_VERSION      "1.0.0-dev"
#endif
#define DEVICE_HOSTNAME "bjorkloven-led"

// ─────────────────────────────────────────────────────────────
//  LED-hårdvara
// ─────────────────────────────────────────────────────────────
// #ifndef så att kopplingstestet kan prova andra pinnar/antal via build-flagga:
//   PLATFORMIO_BUILD_FLAGS="-DLED_PIN=5 -DLED_COUNT=30" pio run -e esp32dev_wiring -t upload
#ifndef LED_PIN
#define LED_PIN         13          // Datapinne till WS2812B (via 330–470 Ω)
#endif
#ifndef LED_COUNT
#define LED_COUNT       60
#endif
#define LED_TYPE        WS2812B
#define LED_COLOR_ORDER GRB

// Ingen färgkorrigering. FastLEDs TypicalLEDStrip (255,176,240) drar ner grönt
// med 30 % och rubbar därmed blandningen i YELLOW_R/YELLOW_G nedan — gulen blir
// orange. Vill man ha varmare vitt görs det i färgerna, inte här.
#define LED_COLOR_CORRECTION UncorrectedColor

// Temporal dithering. Glöden rör sig i ett smalt band längst ner i skalan —
// ett tjugotal räkningssteg — och utan dither syns varje steg som ett hopp i
// andetaget. Dithern växlar mellan två närliggande nivåer mellan bildrutorna
// och ger i praktiken några bitar till, vilket är det som gör tonandet mjukt.
// Den flimrade synligt förr, men bara för att glöden gick ner till 1–2 steg;
// med GLOW_MIN_VAL som golv finns inte det läget längre. Byt till
// DISABLE_DITHER om du ändå ser flimmer.
#define LED_DITHER      BINARY_DITHER

// Strömbudget. FastLED dimmar automatiskt så att detta tak hålls.
// 60 LEDs på full vit ≈ 3.6 A. 3000 mA ger marginal på ett 5 V/4 A-nät.
#define LED_MAX_MILLIAMPS 3000
#define LED_PSU_VOLTS     5

// Global ljusstyrka (0–255). Kan ändras i webbportalen.
#define LED_DEFAULT_BRIGHTNESS 160

// Uppstart. Ljuset flödar in på BOOT_FILL_MS och står sedan kvar tills
// setup() går vidare — pausen på slutet är det som gör floden till en egen
// gest och inte bara början på nästa animation.
#define BOOT_FILL_MS        700
#define BOOT_HOLD_MS        300
#define BOOT_EDGE_SOFTNESS  768   // 8.8-fixpunkt: 3 dioder mjuk kant

// De två förloppsstaplarna: uppstartsarbetet (LED_WORKING) och OTA
// (LED_UPDATING).
//
// Kroppens ljusstyrka. WORK_BODY_VAL låg tidigare på GLOW_MIN_VAL (20), alltså
// den svagaste nivå som fortfarande är gul — mot ett huvud på 255 gav det 12:1
// och lästes inte som en stapel med ljust huvud, utan som en ensam ljus punkt
// med en suddig skugga bakom sig. OTA-stapeln ligger ljusare än
// uppstartsstapeln med flit: den ena är en väntan, den andra är enhetens mest
// ingripande ögonblick.
#define WORK_BODY_VAL   75
#define UPDATE_BODY_VAL 157

// Golv i antal dioder, gemensamt för båda staplarna. Utan det står stapeln på
// noll under just den längsta väntan i respektive förlopp — första
// HTTPS-hämtningen (done=0 av 2) respektive TLS-handskakningen mot GitHub innan
// första byten kommit — och en nästan släckt list med en ensam tänd diod ser ut
// som ett fel, inte som arbete som pågår. Det är dessutom precis då man tittar
// som mest: lampan håller på att skriva om sin egen firmware.
#define BAR_MIN_LIT     4

// ─────────────────────────────────────────────────────────────
//  Färger
// ─────────────────────────────────────────────────────────────
// Björklövens gult, angivet som en fast RGB-blandning i stället för en
// HSV-nyans. Det är hela poängen: gold() skalar bara ner de här två kanalerna
// tillsammans, så färgen är exakt densamma vid 5 % som vid 100 % — det enda
// som andas är styrkan. Med en HSV-nyans trunkeras grönkanalen till noll före
// den röda på vägen ner, och glöden slår om till orange och sedan rött.
//
// Lägre YELLOW_G = varmare gult. Under ~180 börjar det dra åt bärnsten igen,
// och då kommer rödskiftet i utfadningen tillbaka.
#define YELLOW_R        255
#define YELLOW_G        224         // 255 = citrongult, 224 = varmt gult
#define YELLOW_B        0

// Standby-glöd: andningen pendlar mellan dessa nivåer, och nivån är direkt
// ljusstyrka på gulen (ingen kurva emellan — gold() är linjär).
//
// GLOW_FLOOR_VAL och GLOW_MIN_VAL är två olika saker, och det är själva
// poängen med uppdelningen:
//
//   GLOW_FLOOR_VAL  hårdvarugolvet. Under en handfull räkningssteg har en
//                   WS2812 ingenting kvar att arbeta med: den röda dioden styr
//                   ensam, gulen slår om till rött och varje steg i andetaget
//                   blir ett synligt hopp. Vid standardljusstyrkan motsvarar 20
//                   omkring 12 räkningssteg — den lägsta nivå som fortfarande
//                   är otvetydigt gul. Sänker du LED_DEFAULT_BRIGHTNESS mycket
//                   bör golvet upp. Det här är en ren skyddsklämma.
//
//   GLOW_MIN_VAL    andetagets nedre vändpunkt. Ligger med flit ovanför golvet,
//                   så att Perlin-bruset har plats att dra enskilda dioder
//                   nedåt utan att klippas. Låg de två på samma värde — som de
//                   gjorde förut — klipptes varje negativ avvikelse bort i
//                   vändningen: halva listen las sig platt på exakt samma nivå,
//                   strukturen blev ensidig och medelljuset lyftes tyst över
//                   det golv man trodde man hade ställt in. Just i vändningen
//                   är listen som lugnast och tittas på som mest, så det var
//                   där glöden slutade leva.
//
// Marginalen behöver vara minst 18 % av GLOW_MIN_VAL, som är brusets amplitud.
//
// Spannet mellan botten och toppen är brett med flit. Ögat svarar ungefär
// logaritmiskt på ljus, så ett andetag mellan 30 och 68 — drygt en fördubbling
// — läses på håll knappt som en förändring alls; man ser en list som lyser
// jämnt. Det som gör andetaget synligt tvärs över ett rum är kvoten, inte
// antalet steg, och därför ligger taket högt medan botten står kvar nere.
// De två följer varandra nedåt: sänks bara botten äter bruset upp marginalen
// och klippningen i vändningen är tillbaka. Golvet ligger numera under den
// nivå som en gång mättes upp som "lägsta otvetydigt gula" (20), och det är
// medvetet — då satt hela listen kvar på golvet i varje vändning, nu är det
// bara enskilda dioder som dyker ner och studsar upp igen. En kort dipp på en
// diod syns inte som färgskifte, det gör en hel list som ligger still.
#define GLOW_FLOOR_VAL  15          // hårt golv — ingen enskild diod går under
#define GLOW_MIN_VAL    22          // andetagets botten, med plats för brus
#define GLOW_MAX_VAL    140         // toppen — drygt 6x botten, syns på håll
#define GLOW_BPM        9           // ~6,7 s per andetag

// Under match ligger både golv och tak högre — listen "väntar".
#define GLOW_LIVE_FLOOR_LIFT 8
#define GLOW_LIVE_LIFT       16

// Under match dras gulen dessutom åt bärnsten. Det är färgen och inte styrkan
// som skiljer live från standby: lyften ovanför är kvar från ett smalare
// andetag och betyder numera bara ett par procent på toppen, medan ett skifte
// från gult till orange syns direkt även i ögonvrån.
//
// Samma trick som YELLOW_G: bara grönkanalen sänks, röd står kvar på 255, och
// gold() skalar båda med samma faktor så nyansen är exakt densamma i hela
// andetaget. 165 är rakt av CSS-orange (255,165,0).
//
// Obs att det ligger under de ~180 där YELLOW_G-kommentaren varnar för att
// rödskiftet i utfadningen kommer tillbaka. Live-glöden bottnar på
// GLOW_MIN_VAL + GLOW_LIVE_FLOOR_LIFT och kommer aldrig lika långt ner som
// standby, så marginalen finns — men sänker du LED_DEFAULT_BRIGHTNESS mycket
// är det här värdet det första som börjar slå om mot rött.
#define LIVE_YELLOW_G   165

// Gnistor (när laget vann igår)
#define SPARKLE_MEAN_INTERVAL_MS 700  // ungefär en gnista var 0,7 s
#define SPARKLE_DECAY            14   // högre = kortare gnista

// Kall vit topp — kallare än gulen, och det är temperaturskillnaden som gör
// att det läses som ett glitter och inte bara som en ljus fläck.
//
// Gnistan adderas till glöden, den blandas inte in i den. Med blend() blev
// gnistan lika ljus som glöden råkade vara just då: mot ett andetag som toppar
// på GLOW_MAX_VAL var en gnista knappt dubbelt så ljus som ytan den låg på och
// försvann i den, medan samma gnista i vändningen var tiofalt ljusare. Glittret
// tonade alltså in och ut i takt med andetaget. Adderat mättar gnistan mot vitt
// oavsett var i andetaget den hamnar, och syns lika mycket hela cykeln igenom.
#define SPARKLE_R       255
#define SPARKLE_G       250
#define SPARKLE_B       225

// ─────────────────────────────────────────────────────────────
//  Segerläge — tre timmar efter en vinst
// ─────────────────────────────────────────────────────────────
// Släkt med målfyrverkeriet men byggt för att stå på i timmar, inte sekunder:
// inget stroboskop, långsamma kometer, och en glöd under som listen vilar på
// så att den aldrig ser släckt ut mellan salvorna.
//
// Fönstret räknas från det ögonblick lampan *får veta* om vinsten, inte från
// nedsläpp — SHL:s played-games säger att matchen är slut men inte när den tog
// slut. Sluttiden sparas därför i NVS och överlever omstart och OTA.
#define VICTORY_DURATION_MS (3UL * 60 * 60 * 1000)

// En komet tar så här lång tid från mitten ut till kanten. Målfyrverkeriets
// kometer gör samma resa på 110 ms — här är den drygt femton gånger lugnare.
#define VICTORY_TRAVEL_MS   1800
// Samtidiga salvor, jämnt utspridda i tiden. Fler = tätare ström av kometer.
#define VICTORY_VOLLEYS     3
#define VICTORY_HEAD_VAL    170     // kometens kärna
#define VICTORY_TAIL_VAL    60      // dioden bakom kärnan

// Grundglöden under kometerna. Ljusare än standby, dämpad mot live — det ska
// gå att sitta i rummet i tre timmar.
#define VICTORY_GLOW_MIN    26
#define VICTORY_GLOW_MAX    90
#define VICTORY_GLOW_BPM    7

// Hur ofta lampan frågar played-games när matchen kan ha tagit slut. Startar
// först VICTORY_POLL_AFTER_MS efter nedsläpp: dessförinnan spelas det, och
// varje hämtning är ett blockerande TLS-anrop som fryser bilden ett ögonblick.
#define VICTORY_POLL_AFTER_MS (2UL * 60 * 60 * 1000)
#define VICTORY_POLL_MS       (5UL * 60 * 1000)

// Hur gammal en vinst får vara för att fortfarande tändas. Utan taket firar en
// lampa som startas efter ett strömavbrott i juli förra säsongens sista vinst:
// played-games svarar med den matchen oavsett hur länge sedan den spelades.
// Fem timmar från nedsläpp är drygt två timmar efter slutsignalen.
#define VICTORY_MAX_GAME_AGE_S (5UL * 60 * 60)

// Målfyrverkeri
#define GOAL_DURATION_MS 12000
#define GOAL_STROBE_MS   2500       // inledande stroboskop innan "skotten"

// TV-fördröjning. Live-datan från SHL kommer före tv-bilden — utan fördröjning
// skulle lampan avslöja målet för alla i rummet innan det syns på skärmen.
// Sekunder, 0 = av. Ställs om på statussidan; det här är startvärdet.
#define GOAL_DELAY_DEFAULT_S 15
#define GOAL_DELAY_MAX_S     180    // taket i formuläret och vid inläsning
#define GOAL_QUEUE_MAX       6      // köade mål som väntar på att tändas

// ─────────────────────────────────────────────────────────────
//  Björklöven / SHL-API
// ─────────────────────────────────────────────────────────────
// Team-UUID för IF Björklöven i SHL:s eget API (verifierat 2026-08-24).
#define SHL_TEAM_UUID   "4519-4519Rdei6"
#define SHL_TEAM_CODE   "IFB"

#define SHL_API_HOST    "www.shl.se"
#define SHL_LIVE_HOST   "game-broadcaster.s8y.se"

// true  = fyra bara när Björklöven gör mål
// false = fyra vid alla mål i matchen
#define GOAL_ONLY_OUR_TEAM true

// ─────────────────────────────────────────────────────────────
//  Pollningsintervall
// ─────────────────────────────────────────────────────────────
#define POLL_SCHEDULE_MS   (6UL * 60 * 60 * 1000)  // spelschema + gårdagens resultat
#define POLL_LIVE_FALLBACK_MS (45UL * 1000)        // reserv-poll under pågående match

#define OTA_CHECK_MS       (12UL * 60 * 60 * 1000) // kolla efter ny firmware

// ─────────────────────────────────────────────────────────────
//  OTA-källa
// ─────────────────────────────────────────────────────────────
// Skrivs in i webbportalen. Två format stöds:
//   "markusbackman/bjorkloven-led"     → GitHub Releases (rekommenderat)
//   "https://.../firmware.json"        → eget manifest, valfri webbserver
// Privat repo kräver dessutom en token, se README.
#define OTA_DEFAULT_SOURCE "markusbackman/bjorkloven-led"

// Filnamnet på .bin-filen i releasen som ska installeras.
#define OTA_ASSET_NAME  "firmware.bin"

// Ge upp efter så här många misslyckade försök på samma version, så att en
// trasig release inte får lampan att ladda ner 1 MB var 12:e timme för alltid.
#define OTA_MAX_FAILURES 3

// Matchfönster: när enheten anses vara "på matchdag/live".
#define LIVE_WINDOW_PRE_MS  (15UL * 60 * 1000)     // öppna 15 min före nedsläpp
#define LIVE_WINDOW_POST_MS (4UL * 60 * 60 * 1000) // stäng 4 h efter start

// ─────────────────────────────────────────────────────────────
//  Testserver (mock)
// ─────────────────────────────────────────────────────────────
// Lampan hämtar alltid från riktiga SHL. Testservern styr den i stället genom
// att POSTa hela matchläget till lampans egen /push — se mock/server.py.
//
// Riktningen är vald med flit. Hämtar lampan själv måste den nå datorn, och
// där står brandväggen i macOS/Windows i vägen på ett sätt man inte rår över
// från lampan. Push går åt andra hållet, som en vanlig utgående anslutning
// från datorn, och fungerar därför utan att något behöver öppnas.
//
// Varje push förlänger leasen nedan. Slutar mockservern trycka går lampan
// tillbaka till SHL av sig själv — ingen inställning att komma ihåg att slå av.
#define PUSH_LEASE_MS (5UL * 60 * 1000)

// ─────────────────────────────────────────────────────────────
//  Nätverk
// ─────────────────────────────────────────────────────────────
#define AP_SSID_PREFIX  "Bjorkloven-Setup"
#define AP_PASSWORD     ""          // tomt = öppet nät (enklast för captive portal)
#define OTA_PASSWORD    "***"

// Hur länge vi försöker ansluta till sparat WiFi innan portalen startar.
#define WIFI_CONNECT_TIMEOUT_MS 25000
// Hur länge portalen står öppen innan enheten försöker igen med sparade uppgifter.
#define PORTAL_RETRY_MS (5UL * 60 * 1000)

// Tidszon Stockholm med automatisk sommartid.
#define TZ_STOCKHOLM "CET-1CEST,M3.5.0,M10.5.0/3"
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.google.com"
