// ─────────────────────────────────────────────────────────────────────────────
//  LövGlöd
//
//  Standby        långsam gul glöd
//  Vann senast    några glittrande gnistor ovanpå glöden, till nästa match
//  Match pågår    bärnstensglöd; slutspurt ovanpå när det är tajt mot slutet
//  Paus           timglas som rinner ut över pausens antagna längd
//  Övertid        dragkamp mellan lagen, flyttas av skottrycket
//  Mål            stroboskop och kometer, 7–22 s beroende på hur viktigt
//  Motståndarmål  listen suckar
//  Slutsignal     segerdans, sedan segerläget i tre timmar
//  Inget WiFi     eget nät + captive portal som frågar efter SSID/lösenord
//  Uppdatering    signerad self-update från GitHub Releases, annars USB
//
//  Data: SHL:s eget publika API (www.shl.se/api) + live-SSE från
//        game-broadcaster.s8y.se. Björklöven spelar i SHL från 2026/27.
// ─────────────────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#include "config.h"
#include "settings.h"
#include "leds.h"
#include "portal.h"
#include "shl.h"
#include "updater.h"
#include "netcheck.h"
#include "telemetry.h"
#include "trace.h"

// Skjuter upp OTA-kvittensen. Arduino-kärnan kvitterar annars redan i
// initArduino() (cores/esp32/esp32-hal-misc.c), alltså innan setup() ens körts,
// och då fångar bootloaderns rollback bara en binär som inte startar alls — inte
// den som startar fint och är oanvändbar. Med den här skjuts beslutet till
// serviceOtaValidation() nedan.
//
// extern "C" krävs: den svaga originalfunktionen är definierad i en C-fil.
extern "C" bool verifyRollbackLater() { return true; }

// ── Tillstånd ───────────────────────────────────────────────────────────────
enum class AppState { Boot, Portal, Connecting, Online };
static AppState  gState = AppState::Boot;

static NextGame  gNext;
static LiveScore gScore;            // senast kända ställning i pågående match
static bool      gInLiveWindow = false;
static bool      gTimeSynced   = false;

static uint32_t  gNextScheduleFetch = 0;
// "Hämta nu". Egen flagga och inte gNextScheduleFetch = 0: jämförelsen
// (int32_t)(millis() - 0) >= 0 är falsk så fort millis() passerat 2^31, så en
// nollad tidsstämpel blev en tyst no-op mellan 24,9 och 49,7 dygns upptid.
static bool      gFetchNow          = true;
static uint32_t  gScheduleRetryMs   = 0;      // backoff efter misslyckad hämtning
static uint32_t  gNextLivePoll      = 0;
static uint32_t  gNextOtaCheck      = 0;
static uint32_t  gPortalSince       = 0;
static uint32_t  gConnectStarted    = 0;
static uint32_t  gNextTimeRetry     = 0;

// Kör vi en firmware som ännu inte kvitterat sig frisk? Se serviceOtaValidation().
static bool      gOtaOnTrial        = false;
static bool      gWifiEverUp        = false;

static uint32_t  gPushUntil         = 0;      // push-läget lever tills hit
static bool      gPushReplay        = false;  // pushen bär ramar: matchljuset går som live
static bool      gReplayDanced      = false;  // segerdansen tagen i uppspelningen
static bool      gDataOk            = true;   // false när SHL inte svarar alls
static bool      gFirstFetchPending = false;  // första hämtningen visar förlopp
static uint32_t  gNextResultPoll    = 0;      // tät koll efter matchens slut

static void resetMood();                      // matchljuset, se nedan

// Ritar upp förloppet mellan blockerande steg. renderNow() krävs: nästa rad
// efter återropet blockerar ofta i sekunder, och en vanlig render() kan hoppa
// över bildrutan om FPS-grinden inte släppt än.
static void showWorkStep(uint8_t done, uint8_t total) {
    Leds::setWorkProgress(done, total);
    Leds::renderNow();
}

// ── Demoläge ────────────────────────────────────────────────────────────────
// Kortet kräver att BOOT-knappen hålls in vid varje flashning, så animationerna
// måste gå att växla mellan utan att bygga om. Siffertangenterna låser ett
// läge, x släpper det igen. Medan demon är på står app-logiken still: en
// blockerande SHL-hämtning mitt i en animation ser ut som ett fel i
// animationen, och det är animationen vi tittar på.
struct DemoItem {
    char        key;
    const char *name;
    LedMode     mode;
    bool        sparkles;
    int8_t      lead;        // matchljuset: >0 leder, <0 under
    uint8_t     intensity;   // matchljuset: 0–255
};

static const DemoItem kDemo[] = {
    {'1', "uppstartsflöde",                LED_BOOT,         false, 0, 0},
    {'2', "portal, setup (grön puls)",     LED_PORTAL,       false, 0, 0},
    {'3', "portal, WiFi nekat (röd puls)", LED_PORTAL_RETRY, false, 0, 0},
    {'4', "ansluter (gult jagande ljus)",  LED_CONNECTING,   false, 0, 0},
    {'5', "uppstartsförlopp (stapel)",     LED_WORKING,      false, 0, 0},
    {'6', "standby-glöd",                  LED_STANDBY,      false, 0, 0},
    {'7', "standby + gnistor (vann sist)", LED_STANDBY,      true,  0, 0},
    {'8', "live, match pågår",             LED_LIVE,         false, 0, 0},
    {'9', "OTA-uppdatering (stapel)",      LED_UPDATING,     false, 0, 0},
    {'0', "fel, ingen data (rött)",        LED_ERROR,        false, 0, 0},
    {'v', "seger — 3 h efter vinst",       LED_VICTORY,      false, 0, 0},
    {'h', "slutspurt, lika (hjärtslag)",   LED_LIVE,         false, 0, 230},
    {'l', "slutspurt, Löven leder (guldregn)", LED_LIVE,     false, 1, 230},
    {'u', "slutspurt, Löven under (anfallsvåg)", LED_LIVE,   false, -1, 230},
    {'p', "paus (timglas, en minut)",      LED_INTERMISSION, false, 0, 0},
    {'o', "övertid (dragkamp)",            LED_OVERTIME,     false, 0, 0},
    {'c', "slutsignal, vinst (segerdans)", LED_DANCE,        false, 0, 0},
};
static const uint8_t kDemoCount = sizeof(kDemo) / sizeof(kDemo[0]);

static int8_t   gDemoIdx   = -1;     // -1 = demoläget av
static uint32_t gDemoTick  = 0;
static uint8_t  gDemoPct   = 0;
static uint32_t gDemoSince = 0;      // senaste tangent — se DEMO_TIMEOUT_MS

static void demoList() {
    Serial.println("[demo] animationer — tangent visar, g = mål, b = stort mål, s = suck, x = normalt");
    for (uint8_t i = 0; i < kDemoCount; i++)
        Serial.printf("       %c  %s\n", kDemo[i].key, kDemo[i].name);
}

// Matchljuset ritar ur MatchMood, som main.cpp annars räknar fram ur matchen.
// I demoläget sätts den här i stället — pausen på en minut så att hela
// timglaset går att se.
static void demoMood(const DemoItem &d) {
    MatchMood m;
    m.intensity    = d.intensity;
    m.lead         = d.lead;
    m.usAtStart    = true;
    m.pauseStartMs = millis();
    m.pauseEstMs   = 60000;
    m.pressure     = (int8_t)(sinf(millis() / 4000.0f) * 80);
    Leds::setMood(m);
}

static void demoSelect(int8_t idx) {
    if (idx < 0 || idx >= (int8_t)kDemoCount) return;
    gDemoIdx  = idx;
    gDemoPct  = 0;
    gDemoTick = millis();
    Leds::setSparkles(kDemo[idx].sparkles);
    demoMood(kDemo[idx]);
    Leds::lockMode(kDemo[idx].mode);
    status.demo  = kDemo[idx].name;
    status.state = String("Demo — ") + kDemo[idx].name;
    Serial.printf("[demo] %c — %s\n", kDemo[idx].key, kDemo[idx].name);
}

static void demoOff() {
    if (gDemoIdx < 0) return;
    gDemoIdx = -1;
    status.demo = "";
    Leds::setSparkles(false);
    Leds::unlockMode();
    Serial.println("[demo] av — normal drift igen");
}

// Två av lägena har ingen egen rörelse att titta på: uppstartssvepet är en
// engångsanimation, och stapellägena ritar bara det förlopp de matas med.
static void demoLoop() {
    if (gDemoIdx < 0) return;
    const LedMode m = kDemo[gDemoIdx].mode;

    // En lampa i någons vardagsrum får inte bli stående i demoläget för att
    // ingen tryckte "Normal drift" — då missar den nästa match.
    if (millis() - gDemoSince > DEMO_TIMEOUT_MS) {
        Serial.println("[demo] tio minuter utan tangent — tillbaka till normal drift");
        demoOff();
        return;
    }

    if (m == LED_BOOT && millis() - gDemoTick > BOOT_FILL_MS + BOOT_HOLD_MS + 600) {
        gDemoTick = millis();
        Leds::lockMode(LED_BOOT);            // spela om flödet med paus emellan
    }

    // Pausen och segerdansen tar slut; spela om dem med en stund emellan.
    if ((m == LED_INTERMISSION && millis() - gDemoTick > 70000) ||
        (m == LED_DANCE && millis() - gDemoTick > DANCE_MS + 8000)) {
        demoSelect(gDemoIdx);
        return;
    }
    // Dragkampens gräns vandrar med skottrycket.
    if (m == LED_OVERTIME) {
        MatchMood mm;
        mm.pressure = (int8_t)(sinf(millis() / 4000.0f) * 80);
        Leds::setMood(mm);
    }

    if ((m == LED_WORKING || m == LED_UPDATING) && millis() - gDemoTick > 400) {
        gDemoTick = millis();
        gDemoPct  = gDemoPct >= 100 ? 0 : gDemoPct + 4;
        if (m == LED_WORKING) Leds::setWorkProgress(gDemoPct, 100);
        else                  Leds::setUpdateProgress(gDemoPct);
    }
}

// ── Segerläge ───────────────────────────────────────────────────────────────
// Fönstret ligger i NVS eftersom det räknas från när lampan fick veta om
// vinsten. SHL säger att en match är slut, inte när den tog slut, så det finns
// inget att räkna fram på nytt efter en omstart.
static bool victoryActive() {
    if (!settings.victoryUntil || !gTimeSynced) return false;
    if (time(nullptr) < (time_t)settings.victoryUntil) return true;
    settings.clearVictory();
    Serial.println("[seger] fönstret slut — tillbaka till vanlig glöd");
    return false;
}

// Tänder segerläget för en vinst vi inte firat än. Åldersgränsen finns för att
// played-games svarar med senaste matchen hur gammal den än är: utan den firar
// en lampa som startas i juli förra säsongens sista vinst.
static void maybeArmVictory(const LastResult &lr) {
    if (!lr.won || !gTimeSynced) return;
    if ((uint32_t)lr.startUtc == settings.victoryGame) return;   // redan firad

    const time_t now = time(nullptr);
    if (now - lr.startUtc > (time_t)VICTORY_MAX_GAME_AGE_S) {
        // För gammal för att fira, men märk den som avklarad ändå — annars
        // prövas samma match på nytt vid varje hämtning.
        settings.noteVictory((uint32_t)lr.startUtc, 0);
        return;
    }

    settings.noteVictory((uint32_t)lr.startUtc, (uint32_t)(now + VICTORY_DURATION_MS / 1000));
    Serial.printf("[seger] %s — firar i %lu h\n",
                  lr.summary.c_str(), VICTORY_DURATION_MS / 3600000UL);
}

// ── Gnistor ─────────────────────────────────────────────────────────────────
// Gnistorna hör ihop med matchen som kommer, inte med kalenderdygnet: de tänds
// när laget vunnit och lyser ända fram till nästa nedsläpp. Vinsten står kvar
// så länge den är det senaste som hänt — sedan tar nästa match över listen, och
// är den en förlust kommer gnistorna inte tillbaka.
static bool   gWonLast     = false;   // senast spelade matchen var en vinst
static time_t gLastGameUtc = 0;       // ...och dess nedsläpp

static bool winStillGlows() {
    if (!gWonLast || !gTimeSynced) return false;
    const time_t now = time(nullptr);

    // Nästa match har tagit vid. Vi släcker redan när matchfönstret öppnar:
    // därifrån är listen matchens, och att glittra i uppvärmningen vore att
    // fira fel match.
    if (gNext.valid && now >= gNext.startUtc - (time_t)(LIVE_WINDOW_PRE_MS / 1000))
        return false;

    // Ingen nästa match i schemat — sommaruppehåll, eller ett schema som inte
    // gick att hämta. Då är taket det enda som stänger av glittret.
    return now - gLastGameUtc <= (time_t)SPARKLE_MAX_GAME_AGE_S;
}

// Kallas både efter hämtningar och varje varv i loopen: det som släcker
// gnistorna är oftast att klockan passerat nästa nedsläpp, inte att ny data
// kommit in.
static void serviceSparkles() {
    const bool on = winStillGlows();
    if (on != Leds::sparkles())
        Serial.printf("[gnistor] %s\n",
                      on ? "på — vinsten lyser till nästa match" : "av");
    Leds::setSparkles(on);
    status.sparkles = on;
}

// Hämtar senaste resultatet och hanterar båda sakerna det styr: gnistorna
// (vinsten som står kvar) och segerläget (de närmaste timmarna efter en vinst).
static bool refreshLastResult() {
    LastResult lr;
    if (!Shl::fetchLastResult(lr)) {
        Serial.printf("[shl] resultat misslyckades: %s\n", Shl::lastError().c_str());
        return false;
    }
    gWonLast     = lr.won;
    gLastGameUtc = lr.startUtc;
    status.lastResult = lr.summary;
    serviceSparkles();
    maybeArmVictory(lr);
    return true;
}

// ── Seriella kommandon ──────────────────────────────────────────────────────
// Kortet har trasig auto-reset och kräver BOOT-knappen för att flashas. Med
// de här kommandona går det att nollställa WiFi och felsöka utan att flasha.
static void printSerialHelp() {
    Serial.println("  kommandon:  w = glöm WiFi och starta om   n = kör nätverkstest");
    Serial.println("              i = status                    r = starta om");
    Serial.println("              d = lista animationer          x = avsluta demoläget");
    Serial.println("              g = mål   b = stort mål   s = suck   (fler i listan under d)");
}

// Demoläget styrs med samma tangenter från seriemonitorn och från knapparna på
// /ljus. Falskt om tangenten inte betyder något här.
static bool demoKey(char c) {
    c = tolower(c);
    gDemoSince = millis();
    switch (c) {
        case 'x':
            demoOff();
            return true;
        case 'g':
            Serial.println("[demo] MÅL!");
            Leds::triggerGoal();
            return true;
        case 'b':
            Serial.println("[demo] AVGÖRANDE MÅL!");
            Leds::triggerGoal(0, 255);
            return true;
        case 's':
            Serial.println("[demo] motståndarmål — suck");
            Leds::sigh();
            return true;
    }
    for (uint8_t i = 0; i < kDemoCount; i++)
        if (kDemo[i].key == c) { demoSelect(i); return true; }
    return false;
}

static void handleSerialCommands() {
    if (!Serial.available()) return;
    const char c = Serial.read();
    while (Serial.available()) Serial.read();     // släng resten av raden

    switch (c) {
        case 'w': case 'W':
            Serial.println("[cmd] glömmer sparat WiFi, startar om i setup-läge");
            settings.clearWifi();
            delay(300);
            ESP.restart();
            break;

        case 'n': case 'N':
            if (WiFi.status() == WL_CONNECTED) NetCheck::run(showWorkStep);
            else Serial.println("[cmd] inte ansluten");
            break;

        case 'r': case 'R':
            Serial.println("[cmd] startar om");
            delay(200);
            ESP.restart();
            break;

        case 'i': case 'I':
            Serial.printf("[cmd] v%s  läge=%s  SSID=\"%s\"  IP=%s  heap=%u kB\n",
                          FW_VERSION, status.state.c_str(), settings.wifiSsid.c_str(),
                          WiFi.localIP().toString().c_str(), ESP.getFreeHeap() / 1024);
            Serial.printf("      LED-list:    %s, %u dioder\n",
                          Leds::stripName(settings.ledStrip), settings.ledCount);
            if (status.pushMode)
                Serial.println("      datakälla:   push från mockservern");
            else
                Serial.printf("      datakälla:   %s\n", Shl::apiBaseUrl().c_str());
            Serial.printf("      nästa match: %s\n", status.nextGame.c_str());
            Serial.printf("      senaste:     %s\n", status.lastResult.c_str());
            break;

        case 'd': case 'D':
            demoList();
            break;

        case '\n': case '\r': break;
        default:
            if (!demoKey(c)) printSerialHelp();
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
static String formatLocal(time_t utc, const char *fmt) {
    struct tm lt;
    localtime_r(&utc, &lt);
    char buf[48];
    strftime(buf, sizeof(buf), fmt, &lt);
    return String(buf);
}

// 1700000000 = november 2023. Allt under betyder att klockan aldrig blivit
// satt — ESP32:n startar på epoch noll.
static bool clockIsSane() { return time(nullptr) > 1700000000; }

static void markTimeSynced() {
    gTimeSynced        = true;
    status.timeSynced  = true;
    Serial.printf("[tid] synkad: %s\n",
                  formatLocal(time(nullptr), "%Y-%m-%d %H:%M").c_str());
}

static void syncTime() {
    configTzTime(TZ_STOCKHOLM, NTP_SERVER_1, NTP_SERVER_2);

    // Vänta max 10 s på att klockan blir rimlig — allt annat (matchfönster,
    // gnistor, segerläge) bygger på korrekt tid.
    const uint32_t deadline = millis() + 10000;
    while (!clockIsSane() && millis() < deadline) {
        Leds::render();
        delay(50);
    }

    if (clockIsSane()) {
        markTimeSynced();
    } else {
        // Inte ett slutgiltigt nej. serviceTimeSync() fortsätter pröva.
        Serial.println("[tid] NTP gick inte fram — prövar vidare i bakgrunden");
        gNextTimeRetry = millis() + TIME_RETRY_MS;
    }
}

// Klockan kan komma i efterhand, och gör den det ska lampan ta emot den.
//
// Utan det här blir tio sekunders otur permanent: syncTime() kördes bara från
// goOnline(), så en enhet som råkade starta när namnuppslaget mot pool.ntp.org
// var trögt stod utan klocka tills WiFi råkade tappa och komma tillbaka. Och
// det syns inte — lampan hämtar schema och resultat som vanligt, men
// insideLiveWindow(), victoryActive() och maybeArmVictory() är alla false för
// alltid. Matchen kommer och går utan att listen reagerar.
//
// SNTP-klienten som configTzTime() startade fortsätter fråga på egen hand, så
// oftast räcker det att titta efter igen. Vi startar ändå om den varje varv:
// gick namnuppslaget i väggen just när WiFi nyss kommit upp hjälper ingen
// väntan, bara ett nytt försök.
static void serviceTimeSync() {
    if (gTimeSynced || (int32_t)(millis() - gNextTimeRetry) < 0) return;
    gNextTimeRetry = millis() + TIME_RETRY_MS;

    if (!clockIsSane()) {
        Serial.println("[tid] klockan står stilla — ber om tiden igen");
        configTzTime(TZ_STOCKHOLM, NTP_SERVER_1, NTP_SERVER_2);
        return;
    }

    markTimeSynced();

    // Det vi hämtade utan klocka är räknat mot 1970: winStillGlows() och
    // maybeArmVictory() gav båda upp direkt. Hämta om.
    gFetchNow = true;
    Serial.println("[tid] hämtar om matchdata som räknades utan klocka");
}

// ── Datahämtning ────────────────────────────────────────────────────────────
// Ligger nedsläppet så att matchfönstret är öppet nu?
static bool insideWindowOf(time_t startUtc) {
    if (!startUtc || !gTimeSynced) return false;
    const time_t now = time(nullptr);
    return now >= startUtc - (time_t)(LIVE_WINDOW_PRE_MS / 1000) &&
           now <= startUtc + (time_t)(LIVE_WINDOW_POST_MS / 1000);
}

// Matchen vi senast öppnade fönstret för, om vi fortfarande är inne i det.
static bool rememberedLiveGame(NextGame &out) {
    if (!settings.liveUuid.length() || !settings.liveStart) return false;
    const time_t start = (time_t)settings.liveStart;
    if (!insideWindowOf(start)) return false;
    out = NextGame();
    out.valid    = true;
    out.uuid     = settings.liveUuid;
    out.startUtc = start;
    out.homeCode = settings.liveHome;
    out.awayCode = settings.liveAway;
    out.homeIsUs = out.homeCode == SHL_TEAM_CODE;
    return true;
}

static void refreshSchedule(bool showProgress) {
    gFetchNow = false;
    if (showProgress) showWorkStep(0, 2);

    const bool resultOk = refreshLastResult();
    if (resultOk) Serial.printf("[shl] senaste: %s\n", status.lastResult.c_str());
    if (showProgress) showWorkStep(1, 2);

    NextGame n;
    bool nextOk = Shl::fetchNextGame(n);

    // En pågående match har försvunnit ur upcoming-games. Har vi en sparad
    // match vars fönster fortfarande är öppet går den före.
    NextGame live;
    if (rememberedLiveGame(live) && (!nextOk || n.uuid != live.uuid)) {
        Serial.printf("[shl] återupptar pågående match %s (%s – %s)\n",
                      live.uuid.c_str(), live.homeCode.c_str(), live.awayCode.c_str());
        n = live;
        nextOk = true;
    } else if (!nextOk || !insideWindowOf(n.startUtc)) {
        // Inget sparat och inget i schemat som är i fönstret. Startade lampan
        // mitt i matchen — nyinflashad, efter en OTA eller efter strömavbrott —
        // är klubbsajtens matchlista enda stället matchen syns.
        if (Shl::fetchOngoingGame(live)) {
            Serial.printf("[shl] pågående match hittad %s (%s – %s)\n",
                          live.uuid.c_str(), live.homeCode.c_str(), live.awayCode.c_str());
            n = live;
            nextOk = true;
        }
    }
    if (nextOk) {
        // Ny match? Nollställ ställningen så att gamla mål inte trigger igen.
        if (n.uuid != gNext.uuid) { gScore = LiveScore(); resetMood(); }
        gNext = n;
        status.nextGame = n.homeCode + " – " + n.awayCode + "  " +
                          formatLocal(n.startUtc, "%a %d %b %H:%M");
        Serial.printf("[shl] nästa: %s\n", status.nextGame.c_str());
    } else {
        gNext = NextGame();
        status.nextGame = "Ingen match hittad";
        Serial.printf("[shl] schema misslyckades: %s\n", Shl::lastError().c_str());
    }
    if (showProgress) showWorkStep(2, 2);

    // Rött andetag först när ingetdera anropet gick fram. Att bara schemat är
    // tomt är normalt under sommaruppehållet och ska inte se ut som ett fel.
    gDataOk = resultOk || nextOk;

    // En enda studs hos SHL ska inte ge sex timmars rött ljus. Misslyckas
    // hämtningen provar vi igen snart, och dubblar väntan för varje miss upp
    // mot det ordinarie intervallet — så ett längre avbrott inte hamrar på API:t.
    if (gDataOk) {
        gScheduleRetryMs   = 0;
        gNextScheduleFetch = millis() + POLL_SCHEDULE_MS;
    } else {
        gScheduleRetryMs   = gScheduleRetryMs ? std::min<uint32_t>(gScheduleRetryMs * 2, POLL_SCHEDULE_MS)
                                              : SCHEDULE_RETRY_MIN_MS;
        gNextScheduleFetch = millis() + gScheduleRetryMs;
        Serial.printf("[shl] ingen kontakt — felläge, nytt försök om %lu min\n",
                      gScheduleRetryMs / 60000UL);
    }
}

// Sant när vi befinner oss i matchfönstret för nästa match.
static bool insideLiveWindow() {
    return gNext.valid && insideWindowOf(gNext.startUtc);
}

// ── Matchljus ───────────────────────────────────────────────────────────────
// Räknar fram MatchMood ur ställningen och live-strömmen, och väljer läge för
// listen under matchfönstret. Se "Matchljus" i config.h.
//
// Allt som kan avslöja något går med tv-fördröjningen. Slog stämningen om från
// hjärtslag till guldregn i samma stund som SHL rapporterade målet, vore målet
// avslöjat femton sekunder innan fyrverkeriet — och innan tv:n.

// Ställningen listen får visa, med tv-fördröjning. Går efter gScore.
struct ShownScore { uint32_t at; int ours; int theirs; };
static ShownScore gShownQueue[GOAL_QUEUE_MAX];
static uint8_t    gShownCount  = 0;
static int        gShownOurs   = 0;
static int        gShownTheirs = 0;

static float      gMoodI       = 0;      // utjämnad intensitet, 0–1
static uint32_t   gMoodAt      = 0;
static GameState  gLastShown   = GameState::Unknown;
static uint32_t   gPauseStart  = 0;

static void resetMood() {
    gShownCount = 0;
    gShownOurs = gShownTheirs = 0;
    gMoodI      = 0;
    gLastShown  = GameState::Unknown;
}

static void queueShownScore(int ours, int theirs, uint32_t delayMs) {
    if (!delayMs || gShownCount >= GOAL_QUEUE_MAX) {
        gShownOurs = ours; gShownTheirs = theirs;
        return;
    }
    gShownQueue[gShownCount++] = {millis() + delayMs, ours, theirs};
}

static void drainShownScore() {
    while (gShownCount && (int32_t)(millis() - gShownQueue[0].at) >= 0) {
        gShownOurs   = gShownQueue[0].ours;
        gShownTheirs = gShownQueue[0].theirs;
        for (uint8_t i = 1; i < gShownCount; i++) gShownQueue[i - 1] = gShownQueue[i];
        gShownCount--;
    }
}

static uint32_t tvDelayMs() { return (uint32_t)settings.goalDelayS * 1000; }

// Matchläget som listen får visa: det nya läget först när tv-fördröjningen
// gått ut sedan det kom, dessförinnan det förra.
static GameState shownState(const LiveInfo &li) {
    return millis() - li.stateRxMs >= tvDelayMs() ? li.state : li.prevState;
}

// Intensiteten, 0–1. Se formeln i config.h.
static float moodIntensity(const LiveInfo &li, int ours, int theirs) {
    const GameState st = shownState(li);
    if (st == GameState::Decided) return 0;     // avgjort — inget kvar att vara spänd på
    if (st == GameState::Overtime || st == GameState::Shootout || li.period >= 4)
        return MOOD_OVERTIME;
    if (st != GameState::Ongoing || !li.hasClock || li.period < 1) return 0;

    const int32_t regLeft = (3 - (int32_t)li.period) * 1200 + max(0, 1200 - (int)li.elapsedS);
    const float   tf = powf(constrain(1.0f - (float)regLeft / MOOD_RAMP_S, 0.0f, 1.0f), MOOD_CURVE);

    const int   d  = ours - theirs, ad = abs(d);
    const float close = ad == 0 ? 1.0f : ad == 1 ? 0.8f : ad == 2 ? 0.35f : 0.08f;
    const float bonus = d == 1 ? MOOD_LEAD_BONUS : d == -1 ? MOOD_TRAIL_BONUS : 0;

    float perMin, balance;
    Shl::shotPressure(perMin, balance);
    const float act  = constrain((perMin - MOOD_SHOTS_CALM) / (MOOD_SHOTS_HOT - MOOD_SHOTS_CALM), 0.0f, 1.0f);
    const float shot = MOOD_SHOT_WEIGHT * act * close * (0.3f + 0.7f * tf);

    return constrain(tf * (close + bonus) + shot, 0.0f, 1.0f);
}

// Hur mycket ett mål betydde, 0–255, räknat på ställningen efter målet.
// Övertidsmål avgör matchen och får alltid det största fyrverkeriet.
static uint8_t goalImportance(int ours, int theirs) {
    const LiveInfo &li = Shl::liveInfo();
    if (li.period >= 4 || li.state == GameState::Overtime) return 255;
    const int   d   = ours - theirs;
    float imp = 0.25f + 0.75f * moodIntensity(li, ours, theirs);
    if (li.period == 3 && (d == 0 || d == 1)) imp += 0.15f;     // kvittering eller ledning i tredje
    return (uint8_t)(constrain(imp, 0.0f, 1.0f) * 255);
}

// Vilket läge listen ska stå i under matchfönstret, och stämningen till det.
static LedMode serviceMood() {
    drainShownScore();
    const LiveInfo &li = Shl::liveInfo();
    const GameState st = shownState(li);

    if (st == GameState::Intermission && gLastShown != GameState::Intermission)
        gPauseStart = millis();
    gLastShown = st;

    // Glid mot målet i stället för att hoppa när en klockram kommer.
    const float    target = moodIntensity(li, gShownOurs, gShownTheirs);
    const uint32_t now    = millis();
    const float    dt     = gMoodAt ? (float)(now - gMoodAt) : 0;
    gMoodAt = now;
    gMoodI += (target - gMoodI) * (1.0f - expf(-dt / MOOD_SMOOTH_MS));

    float perMin, balance;
    Shl::shotPressure(perMin, balance);

    MatchMood m;
    m.intensity    = (uint8_t)(constrain(gMoodI, 0.0f, 1.0f) * 255);
    m.lead         = (int8_t)constrain(gShownOurs - gShownTheirs, -1, 1);
    m.pressure     = (int8_t)(constrain(balance, -1.0f, 1.0f) * 100);
    // Hemmalaget till vänster, som i tv-grafiken — listens början är vänster.
    m.usAtStart    = gNext.homeIsUs;
    m.pauseStartMs = gPauseStart;
    m.pauseEstMs   = 1000UL * (li.period >= 3 ? PAUSE_OT_EST_S : PAUSE_EST_S);
    Leds::setMood(m);

    switch (st) {
        case GameState::Intermission: return LED_INTERMISSION;
        case GameState::Overtime:
        case GameState::Shootout:     return LED_OVERTIME;
        case GameState::Decided:
            // Förlust: tillbaka till vardagsglöden. Vinsten tas om hand av
            // segerdansen och segerläget. Lika betyder straffar vi inte vet
            // utgången av — played-games avgör det.
            if (gShownOurs < gShownTheirs) return LED_STANDBY;
            return LED_LIVE;
        default:                      return LED_LIVE;
    }
}

// Slutsignal och vi leder: segerdans, och segerläget tänds direkt i stället
// för att vänta på played-games. Matchens nedsläpp är identiteten, samma som
// maybeArmVictory() använder, så vinsten firas inte två gånger.
static void maybeDance() {
    if (!gNext.valid || !gTimeSynced) return;
    if (shownState(Shl::liveInfo()) != GameState::Decided) return;
    if (gShownOurs <= gShownTheirs) return;
    if ((uint32_t)gNext.startUtc == settings.victoryGame) return;
    // Låt ett köat eller pågående övertidsmål brinna klart först.
    if (Leds::mode() == LED_GOAL || Leds::pendingGoals()) return;

    settings.noteVictory((uint32_t)gNext.startUtc,
                         (uint32_t)(time(nullptr) + VICTORY_DURATION_MS / 1000));
    Serial.printf("[seger] slutsignal %d–%d — segerdans, sedan firar i %lu h\n",
                  gShownOurs, gShownTheirs, VICTORY_DURATION_MS / 3600000UL);
    Leds::dance();
}

// Segerdansen i en uppspelad match. Samma villkor som maybeDance(), men
// ingenting sparas: en gammal vinst ska inte tända segerläget på riktigt.
static void maybeReplayDance() {
    if (gReplayDanced || shownState(Shl::liveInfo()) != GameState::Decided) return;
    if (gShownOurs <= gShownTheirs) return;
    if (Leds::mode() == LED_GOAL || Leds::pendingGoals()) return;
    gReplayDanced = true;
    Serial.printf("[uppspelning] slutsignal %d–%d — segerdans, sparas inte\n",
                  gShownOurs, gShownTheirs);
    Leds::dance();
}

// ── Lampläge ────────────────────────────────────────────────────────────────
// Ska listen lysa just nu? Allt annat — hämtningar, live-strömmen, segerläget —
// går som vanligt oavsett, så att lampan är i fas den dag läget ändras.
//
// "Bara match" tänds matchLeadMin före nedsläpp och släcks vid slutsignalen,
// med tv-fördröjning som allt annat. Ett mål eller en segerdans som redan
// brinner får brinna klart. Syns slutsignalen aldrig stänger matchfönstret.
static bool lampAwake() {
    if (settings.lampMode == LAMP_ALWAYS) return true;
    if (settings.lampMode == LAMP_OFF)    return false;

    const LedMode m = Leds::mode();
    if (m == LED_GOAL || m == LED_DANCE || Leds::pendingGoals()) return true;
    if (gInLiveWindow) return shownState(Shl::liveInfo()) != GameState::Decided;

    if (!gNext.valid || !gTimeSynced) return false;
    const time_t now = time(nullptr);
    return now >= gNext.startUtc - (time_t)settings.matchLeadMin * 60 && now < gNext.startUtc;
}

static const char *moodStateText(LedMode m) {
    switch (m) {
        case LED_INTERMISSION: return "Paus";
        case LED_OVERTIME:     return "Övertid";
        case LED_STANDBY:      return "Matchen slut";
        default:               return "Match pågår";
    }
}

// Jämför ny ställning mot den gamla och fyrar av vid mål.
static void applyScore(const LiveScore &fresh, const char *src = "push") {
    TRACE("[score] %s: %d–%d (förra %s%d–%d)\n", src, fresh.home, fresh.away,
          gScore.valid ? "" : "okalibrerad ", gScore.home, gScore.away);
    if (!fresh.valid || fresh.home < 0 || fresh.away < 0) return;

    status.liveScore = gNext.homeCode + " " + String(fresh.home) + " – " +
                       String(fresh.away) + " " + gNext.awayCode;

    if (!gScore.valid) {          // första avläsningen: bara kalibrera
        TRACE("[score] kalibrerad på %d–%d, inget mål\n", fresh.home, fresh.away);
        gScore = fresh;
        // Efter en omstart mitt i matchen: ställningen vi redan firat går före
        // en äldre från en server som släpar. Annars tänds samma mål igen.
        if (settings.liveUuid == gNext.uuid && settings.liveScoreHome >= 0) {
            gScore.home = max(gScore.home, (int)settings.liveScoreHome);
            gScore.away = max(gScore.away, (int)settings.liveScoreAway);
            TRACE("[score] sparad ställning %d–%d → kalibrerad på %d–%d\n",
                  settings.liveScoreHome, settings.liveScoreAway, gScore.home, gScore.away);
        }
        settings.noteLiveScore(gScore.home, gScore.away);
        queueShownScore(gNext.homeIsUs ? gScore.home : gScore.away,
                        gNext.homeIsUs ? gScore.away : gScore.home, 0);
        return;
    }

    const int dHome = fresh.home - gScore.home;
    const int dAway = fresh.away - gScore.away;
    // Ställningen får inte backa. SHL:s servrar ligger olika långt efter, och
    // efter ett serverbyte kan en äldre ställning komma. Sänktes gScore då,
    // tändes samma mål en gång till när servern kom ikapp. Priset: ett bortdömt
    // mål som sedan görs om igen missas.
    gScore.home = max(gScore.home, fresh.home);
    gScore.away = max(gScore.away, fresh.away);
    status.liveScore = gNext.homeCode + " " + String(gScore.home) + " – " +
                       String(gScore.away) + " " + gNext.awayCode;
    if (settings.liveUuid == gNext.uuid) settings.noteLiveScore(gScore.home, gScore.away);

    if (dHome < 0 || dAway < 0)
        TRACE("[score] STÄLLNINGEN GICK NER (dH=%d dA=%d) — bortdömt mål?\n", dHome, dAway);
    if (dHome <= 0 && dAway <= 0) return;   // rättelse eller oförändrat

    const int ourGoals   = gNext.homeIsUs ? dHome : dAway;
    const int theirGoals = gNext.homeIsUs ? dAway : dHome;

    // Tv-bilden ligger efter live-datan. Utan fördröjning tänder lampan målet
    // för alla i rummet innan det syns på skärmen.
    const uint32_t delayMs = (uint32_t)settings.goalDelayS * 1000;

    TRACE("[score] dH=%d dA=%d, vi är %s → våra %d, deras %d\n", dHome, dAway,
          gNext.homeIsUs ? "hemma" : "borta", ourGoals, theirGoals);
    const int ours   = gNext.homeIsUs ? gScore.home : gScore.away;
    const int theirs = gNext.homeIsUs ? gScore.away : gScore.home;
    queueShownScore(ours, theirs, delayMs);

    if (ourGoals > 0) {
        const uint8_t imp = goalImportance(ours, theirs);
        Serial.printf("[MÅL] Björklöven! %d–%d, vikt %u%s\n", fresh.home, fresh.away, imp,
                      delayMs ? "  (väntar på tv)" : "");
        Leds::triggerGoal(delayMs, imp);
    } else if (theirGoals > 0) {
        Serial.printf("[mål] motståndaren. %d–%d%s\n", fresh.home, fresh.away,
                      delayMs ? "  (suck väntar på tv)" : "");
        Leds::sigh(delayMs);
    }
}

static void serviceLive() {
    const bool inWindow = insideLiveWindow();

    if (inWindow != gInLiveWindow) {
        gInLiveWindow = inWindow;
        if (inWindow) {
            Serial.println("[live] matchfönster öppet — kopplar upp mot live-strömmen");
            settings.noteLiveGame(gNext.uuid, (uint32_t)gNext.startUtc,
                                  gNext.homeCode, gNext.awayCode);
            Shl::sseStart(gNext.uuid, gNext.startUtc);
            gNextLivePoll   = millis();
            // Tidsstämpeln kan vara månader gammal efter sommaruppehållet, och
            // då ser den ut att ligga i framtiden. Se gFetchNow.
            gNextResultPoll = millis();
        } else {
            Serial.println("[live] matchfönster stängt");
            Shl::sseStop();
            gScore = LiveScore();
            resetMood();
            status.liveScore = "—";
            // Ett mål som fortfarande väntar på tv-fördröjningen när fönstret
            // stänger hör inte hemma i nästa match.
            Leds::clearPendingGoals();
            // Kolla resultatet strax efter matchen: både gnistorna och nästa
            // match ska vara rätt så fort slutsignalen gått.
            gNextScheduleFetch = millis() + 60000;
        }
    }

    if (!inWindow) return;

    LiveScore fresh;
    if (Shl::ssePump(fresh)) applyScore(fresh, "sse");

    // Reservpollning ifall SSE-strömmen är tyst eller formatet ändrats. Inte
    // medan målfyrverkeriet brinner: hämtningen blockerar loop() i upp till 20 s
    // och fryser animationen mitt i. Pollningen skjuts bara upp — den går så
    // fort fyrverkeriet är slut.
    if (Leds::mode() != LED_GOAL && (int32_t)(millis() - gNextLivePoll) >= 0) {
        gNextLivePoll = millis() + POLL_LIVE_FALLBACK_MS;
        LiveScore polled;
        if (Shl::pollLiveScore(gNext.uuid, polled)) applyScore(polled, "poll");
    }

    status.sseLive = Shl::sseConnected();

    maybeDance();

    // Nära slutsignalen: fråga played-games ofta, så segerläget tänds i
    // anslutning till matchen och inte vid nästa sexttimmarshämtning. Först
    // efter VICTORY_POLL_AFTER_MS — dessförinnan spelas det fortfarande, och
    // varje hämtning är ett blockerande TLS-anrop som fryser bilden ett ögonblick.
    if (gNext.valid && gTimeSynced && !victoryActive() &&
        time(nullptr) >= gNext.startUtc + (time_t)(VICTORY_POLL_AFTER_MS / 1000) &&
        (int32_t)(millis() - gNextResultPoll) >= 0) {
        gNextResultPoll = millis() + VICTORY_POLL_MS;
        TRACE("[seger] frågar played-games\n");
        refreshLastResult();
        TRACE("[seger] senaste: %s, segerläge %s\n", status.lastResult.c_str(),
              victoryActive() ? "PÅ" : "av");
    }
}

// ── Push från mockservern ───────────────────────────────────────────────────
// Så länge leasen lever hämtar lampan ingenting själv — matchläget kommer
// utifrån. Se PUSH_LEASE_MS i config.h.
static bool pushActive() {
    // Felsökningsläget av mitt i en lease avslutar push-läget direkt — alla tre
    // ställena i loopen frågar härigenom, så ingen väg tillbaka missas.
    return settings.debugPush && gPushUntil != 0 &&
           (int32_t)(millis() - gPushUntil) < 0;
}

static void applyPush(const PushState &p) {
    if (!pushActive()) {
        Serial.println("[push] mockservern styr — egen hämtning pausad");
        Shl::sseStop();
        status.sseLive = false;
        gScore = LiveScore();          // ny källa: kalibrera om innan mål räknas
        gPushReplay   = false;
        gReplayDanced = false;
        resetMood();
    }
    gPushUntil      = millis() + PUSH_LEASE_MS;
    status.pushMode = true;
    gDataOk         = true;

    if (p.hasNext) {
        // Byte av lag räknas som ny match — annars ser en omställd ställning
        // ut som ett mål.
        if (p.homeCode != gNext.homeCode || p.awayCode != gNext.awayCode) {
            gScore = LiveScore();
            Shl::resetLive();
            resetMood();
            gReplayDanced = false;
        }
        gNext.valid     = true;
        gNext.homeCode  = p.homeCode;
        gNext.awayCode  = p.awayCode;
        gNext.homeIsUs  = p.homeIsUs;
        status.nextGame = p.nextText.length() ? p.nextText
                                              : p.homeCode + " – " + p.awayCode;
    }

    if (p.hasLast) {
        // Mockservern äger gnistorna medan pushen lever — den bestämmer själv
        // om vinsten fortfarande står kvar.
        status.sparkles   = p.wonLast;
        status.lastResult = p.lastResult;
        Leds::setSparkles(p.wonLast);
    }

    if (gInLiveWindow != p.live) {
        gInLiveWindow = p.live;
        Serial.printf("[push] matchfönster %s\n", p.live ? "öppet" : "stängt");
        if (!p.live) {
            gScore = LiveScore();
            status.liveScore = "—";
            Shl::resetLive();
            resetMood();
            gReplayDanced = false;
            Leds::clearPendingGoals();
        }
    }

    if (p.live && p.hasScore) {
        LiveScore fresh;
        fresh.valid = true;
        fresh.home  = p.home;
        fresh.away  = p.away;
        applyScore(fresh);             // härifrån triggas målfyrverkeriet
    }

    // Uppspelning: ramarna går genom samma tolkning som strömmens, och varje
    // ställning genom samma applyScore() — omsändningar och allt. Varje push
    // avgör själv: uppspelningen skickar frames även i hjärtslagen, och tar
    // mockservern över igen ska listen tillbaka till vanlig push.
    gPushReplay = p.hasFrames;
    if (p.hasFrames) {
        JsonDocument doc;
        if (const DeserializationError err = deserializeJson(doc, p.frames)) {
            Serial.printf("[uppspelning] ramarna gick inte att tolka: %s\n", err.c_str());
            return;
        }
        for (JsonVariantConst f : doc.as<JsonArrayConst>()) {
            LiveScore fresh;
            if (Shl::injectFrame(f, fresh) && p.live) applyScore(fresh, "uppspelning");
        }
    }
}

// Tystnar mockservern tar lampan över igen istället för att frysa fast i ett
// påhittat matchläge.
static void endPushMode() {
    Serial.println("[push] tyst för länge — tillbaka till egen hämtning");
    status.pushMode    = false;
    gPushUntil         = 0;
    gInLiveWindow      = false;
    gScore             = LiveScore();
    status.liveScore   = "—";
    gNext              = NextGame();
    gFetchNow          = true;
    gPushReplay        = false;
    Shl::resetLive();
    resetMood();
    Leds::clearPendingGoals();
}

// ── WiFi ────────────────────────────────────────────────────────────────────
static void beginConnect() {
    Serial.printf("[wifi] ansluter till \"%s\"\n", settings.wifiSsid.c_str());
    Portal::stop();

    // Släck innan radion går igång. Uppstartsflödet lämnar hela listen tänd, och
    // den lasten plus WiFi:s första TX-burst tog enheten under brownout-gränsen.
    // Det jagande ljuset bygger upp sig från svart igen direkt efteråt.
    Leds::setMode(LED_CONNECTING);
    Leds::blank();

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);            // WiFi-sleep ger hack i LED-timingen
    WiFi.setHostname(DEVICE_HOSTNAME);
    WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPass.c_str());

    gConnectStarted = millis();
    gState = AppState::Connecting;
    status.state = "Ansluter";
}

// credentialsFailed skiljer de två fallen åt, både i loggen och på listen:
//   false → inget WiFi sparat, användaren ska ansluta till vårt nät  (grönt)
//   true  → sparat WiFi men det svarar inte                          (rött)
static void enterPortal(bool credentialsFailed) {
    Serial.printf("[wifi] %s — startar portalen\n",
                  credentialsFailed ? "sparat WiFi svarar inte"
                                    : "inget WiFi sparat");

    // Samma skäl som i beginConnect(): softAP() startar radion, och listen står
    // kvar fulltänd från uppstartsflödet när den gör det.
    Leds::setMode(credentialsFailed ? LED_PORTAL_RETRY : LED_PORTAL);
    Leds::blank();

    Portal::startAccessPoint();
    gPortalSince = millis();
    gState = AppState::Portal;
    status.state = credentialsFailed ? "WiFi svarar inte" : "Setup-läge";
}

static void goOnline() {
    Serial.printf("[wifi] ansluten, IP %s\n", WiFi.localIP().toString().c_str());
    gWifiEverUp = true;               // halva friskkriteriet, se B5

    syncTime();

    // Kör en gång vid uppkoppling. Skiljer nätverksproblem från appfel innan
    // vi ens försöker prata med SHL.
    NetCheck::run(showWorkStep);

    if (MDNS.begin(DEVICE_HOSTNAME)) MDNS.addService("http", "tcp", 80);
    Portal::startStationServer();

    gState = AppState::Online;
    Leds::setMode(LED_STANDBY);
    status.state = "Standby";

    gFetchNow          = true;                    // hämta direkt
    gFirstFetchPending = true;                    // ...och visa förlopp medan den går
    gNextOtaCheck      = millis() + 60000;        // men vänta lite med OTA-kollen
}

// ── Omstartsorsak ───────────────────────────────────────────────────────────
// En lampa som startar om i någons vardagsrum ger annars ingenting att gå på.
// Utan seriekabel gick det inte att skilja en brownout från en krasch, och det
// är precis den skillnaden man behöver när matningen misstänks.
//
// Obs att en brownout på ESP32 syns som SW_CPU_RESET i ROM-loggen — avbrottet
// skriver ut sin varning och gör en mjuk omstart. ESP-IDF lämnar dock en hint
// efter sig, så esp_reset_reason() svarar ändå ESP_RST_BROWNOUT.
static const char *resetReasonText() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "strömpåslag";
        case ESP_RST_SW:        return "omstart från koden";
        case ESP_RST_BROWNOUT:  return "BROWNOUT — matningen sviktade";
        case ESP_RST_PANIC:     return "KRASCH — undantag i koden";
        case ESP_RST_INT_WDT:   return "WATCHDOG — avbrott blockerat";
        case ESP_RST_TASK_WDT:  return "WATCHDOG — task svarade inte";
        case ESP_RST_WDT:       return "WATCHDOG";
        case ESP_RST_EXT:       return "extern reset";
        case ESP_RST_DEEPSLEEP: return "uppvaknad ur djupsömn";
        default:                return "okänd";
    }
}

// Allt utom ett rent strömpåslag eller en omstart vi bad om själva är värt att
// lyfta fram på statussidan.
static bool resetWasAbnormal() {
    const esp_reset_reason_t r = esp_reset_reason();
    return r != ESP_RST_POWERON && r != ESP_RST_SW;
}

// ── OTA-validering ──────────────────────────────────────────────────────────
// Läser av vad bootloadern gjorde med oss vid start.
static void checkRollbackState() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t   st;

    if (esp_ota_get_state_partition(running, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        gOtaOnTrial = true;
        Serial.printf("[ota] ny firmware på prov — kvitteras när WiFi varit uppe "
                      "och %lu min gått\n", OTA_VALIDATE_AFTER_MS / 60000UL);
        return;                       // otaPendingVersion namnger den vi provar
    }

    if (!settings.otaPendingVersion.length()) return;

    if (settings.otaPendingVersion == FW_VERSION) {
        // Vi kör den och den är redan kvitterad. Städa bokföringen.
        settings.clearOtaPending();
        return;
    }

    // Vi kör något annat än det som installerades. Innan det tolkas som en
    // rollback: fanns det verkligen en partition som bootloadern dömde ut?
    // Utan den kontrollen skulle en vanlig USB-flash mitt i ett pågående prov
    // se likadan ut, och versionen svartlistas helt i onödan.
    if (esp_ota_get_last_invalid_partition() == nullptr) {
        Serial.printf("[ota] %s ersattes utan rollback (USB-flash?) — glömmer den\n",
                      settings.otaPendingVersion.c_str());
        settings.clearOtaPending();
        return;
    }

    Serial.printf("[ota] %s rullades tillbaka av bootloadern — kör %s igen\n",
                  settings.otaPendingVersion.c_str(), FW_VERSION);
    Telemetry::event("ota_rollback", settings.otaPendingVersion);
    settings.noteOtaRollback(settings.otaPendingVersion);
}

// Kvitterar, eller startar om så att bootloadern får rulla tillbaka.
static void serviceOtaValidation() {
    if (!gOtaOnTrial) return;

    const bool healthy = gWifiEverUp && millis() >= OTA_VALIDATE_AFTER_MS;
    const bool expired = millis() >= OTA_VALIDATE_DEADLINE_MS;

    // Har någon medvetet glömt WiFi står lampan och väntar på en människa. Det
    // är inte firmwarens fel, och att rulla tillbaka en frisk version för det
    // vore fel svar.
    if (healthy || (expired && !settings.hasWifi())) {
        if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) return;
        gOtaOnTrial       = false;
        status.otaOnTrial = false;
        settings.clearOtaPending();
        Telemetry::event("ota_ok", FW_VERSION);
        Serial.println(healthy ? "[ota] kvitterad som frisk — rollback avblåst"
                               : "[ota] inget WiFi konfigurerat — kvitterar ändå");
        return;
    }

    if (!expired) return;

    Serial.println("[ota] blev aldrig frisk — startar om så bootloadern kan "
                   "rulla tillbaka");
    delay(200);
    ESP.restart();
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n\n== LövGlöd v" FW_VERSION " ==");
    printSerialHelp();

    status.resetReason   = resetReasonText();
    status.resetAbnormal = resetWasAbnormal();
    Serial.printf("[boot] föregående omstart: %s\n", status.resetReason.c_str());

    settings.load();
#ifdef LIVE_SEED_UUID
    // Bara diagnostikbygget: lampan flashades mitt i en match, innan den hunnit
    // spara matchen själv.
    if (!settings.liveUuid.length())
        settings.noteLiveGame(LIVE_SEED_UUID, LIVE_SEED_START, LIVE_SEED_HOME, LIVE_SEED_AWAY);
    if (settings.liveUuid == LIVE_SEED_UUID && settings.liveScoreHome < 0)
        settings.noteLiveScore(LIVE_SEED_SCORE_HOME, LIVE_SEED_SCORE_AWAY);
#endif
    settings.noteBoot(esp_reset_reason() == ESP_RST_POWERON, status.resetAbnormal);
    Serial.printf("[boot] start nr %lu, %lu onormala omstarter sedan strömpåslag\n",
                  (unsigned long)settings.bootCount, (unsigned long)settings.abnormalBoots);
    Telemetry::event("boot");
    checkRollbackState();
    status.otaOnTrial = gOtaOnTrial;
    Leds::begin(settings.ledStrip, settings.ledCount);
    Leds::setBrightness(settings.brightness);
    // En släckt lampa startar släckt. Uppstartsflödet syns ändå när någon
    // kopplat in den — men inte när den startar om av sig själv (OTA mitt i
    // natten), då ska den förbli mörk.
    Leds::setDark(settings.lampMode != LAMP_ALWAYS, /*instant=*/true);
    Leds::setShowSetup(esp_reset_reason() != ESP_RST_SW);
    Leds::setMode(LED_BOOT);

    // Kort uppstartsflöde så man ser att listen lever
    const uint32_t until = millis() + BOOT_FILL_MS + BOOT_HOLD_MS;
    while (millis() < until) Leds::render();

    if (settings.hasWifi()) beginConnect();
    else                    enterPortal(false);
}

#ifdef LIVE_TRACE
static const char *modeName(LedMode m) {
    static const char *n[] = {"BOOT", "PORTAL", "PORTAL_RETRY", "CONNECTING", "WORKING",
                              "STANDBY", "LIVE", "GOAL", "VICTORY", "UPDATING", "ERROR",
                              "INTERMISSION", "OVERTIME", "DANCE"};
    return m < sizeof(n) / sizeof(n[0]) ? n[m] : "?";
}

// Lägesbyten på listen och en statusrad var 30:e sekund.
static void traceTick() {
    static LedMode  lastMode = LED_BOOT;
    static uint32_t nextLine = 0;
    if (Leds::mode() != lastMode) {
        Serial.printf("[led] %s → %s\n", modeName(lastMode), modeName(Leds::mode()));
        lastMode = Leds::mode();
    }
    if ((int32_t)(millis() - nextLine) < 0) return;
    nextLine = millis() + 30000;
    const time_t now = time(nullptr);
    Serial.printf("[stat] %s  läge=%s fönster=%d sse=%d ramar=%lu återansl=%lu hb=%lu tyst=%lus  "
                  "ställn=%s  kö=%u  heap=%u/%u kB  wifi=%d dBm\n",
                  gTimeSynced ? formatLocal(now, "%H:%M:%S").c_str() : "--:--:--",
                  modeName(Leds::mode()), gInLiveWindow, Shl::sseConnected(),
                  (unsigned long)Shl::sseFrames(), (unsigned long)Shl::sseReconnects(),
                  (unsigned long)Shl::sseComments(), Shl::sseSilentMs() / 1000,
                  status.liveScore.c_str(), Leds::pendingGoals(),
                  ESP.getFreeHeap() / 1024, ESP.getMinFreeHeap() / 1024, WiFi.RSSI());
}
#endif

void loop() {
#ifdef LIVE_TRACE
    traceTick();
#endif
    Leds::render();
    Portal::loop();
    serviceOtaValidation();
    handleSerialCommands();
    if (Portal::demoPending()) demoKey(Portal::takeDemo());
    demoLoop();

    // Demoläget äger listen helt — inga hämtningar, inga lägesbyten bakom ryggen.
    if (gDemoIdx >= 0) return;

    switch (gState) {
        case AppState::Connecting:
            if (WiFi.status() == WL_CONNECTED) {
                goOnline();
            } else if (millis() - gConnectStarted > WIFI_CONNECT_TIMEOUT_MS) {
                WiFi.disconnect(true);
                // Vi kom hit med sparade uppgifter som inte gick igenom.
                enterPortal(true);
            }
            break;

        case AppState::Portal:
            // Nya uppgifter sparade → prova dem direkt.
            if (Portal::credentialsSubmitted()) {
                delay(400);                    // låt kvittenssidan nå webbläsaren
                beginConnect();
            }
            // Annars: prova sparade uppgifter igen med jämna mellanrum. Routern
            // kan ha varit nere när lampan startade.
            else if (settings.hasWifi() && millis() - gPortalSince > PORTAL_RETRY_MS) {
                beginConnect();
            }
            break;

        case AppState::Online: {
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("[wifi] tappade anslutningen");
                Shl::sseStop();
                status.sseLive = false;
                beginConnect();
                break;
            }

            serviceTimeSync();

            if (Portal::pushPending()) applyPush(Portal::takePush());

            // Portalen kan ha bytt datakälla eller bett om en ny hämtning. Släpp
            // matchen vi följde — uuid:t betyder inget på den nya servern.
            if (Portal::refreshRequested()) {
                Portal::clearRefresh();
                Shl::sseStop();
                gInLiveWindow      = false;
                gNext              = NextGame();
                gScore             = LiveScore();
                status.liveScore   = "—";
                status.sseLive     = false;
                gFetchNow          = true;
                Serial.printf("[shl] hämtar om från %s\n", Shl::apiBaseUrl().c_str());
            }

            if (!pushActive() && (gFetchNow || (int32_t)(millis() - gNextScheduleFetch) >= 0)) {
                refreshSchedule(gFirstFetchPending);
                if (gFirstFetchPending) Leds::setShowSetup(false);
                gFirstFetchPending = false;
            }

            if (pushActive()) {
                // Allt kommer via POST /push — ingen SSE, ingen pollning.
                if (gPushReplay && gInLiveWindow) maybeReplayDance();
            } else {
                if (status.pushMode) endPushMode();
                serviceLive();
                // Efter serviceLive(): har matchfönstret precis öppnat ska
                // gnistorna vara borta i samma varv som listen blir matchens.
                serviceSparkles();
            }

            {
                const bool manual = Updater::checkRequested();
#ifdef LIVE_TRACE
                // Diagnostikbygget får inte ersätta sig självt med senaste release.
                const bool due    = false;
#else
                const bool due    = (int32_t)(millis() - gNextOtaCheck) >= 0;
#endif

                if (gOtaOnTrial && (manual || due)) {
                    // esp_ota_set_boot_partition() vägrar ändå så länge
                    // nuvarande image står i PENDING_VERIFY.
                    if (manual) Updater::clearRequest();
                    gNextOtaCheck = millis() + 60000;
                } else if (settings.otaSource.length() && (manual || due)) {
                    gNextOtaCheck = millis() + (settings.otaBeta ? OTA_CHECK_BETA_MS
                                                                 : OTA_CHECK_MS);
                    Updater::clearRequest();
                    // Aldrig automatiskt mitt i en match — en omstart i tredje
                    // perioden vore synd. Manuell begäran får gå igenom ändå.
                    if (manual || !gInLiveWindow) Updater::checkAndApply(settings.otaSource);
                } else if (manual) {
                    Updater::clearRequest();
                }
            }

            // Håll LED-läget i synk med tillståndet (målfyrverkeriet får styra själv).
            // Kör efter refreshSchedule() i samma varv, så förloppsstapeln ersätts
            // av rätt läge så fort hämtningen är klar och kan inte fastna.
            if (Leds::mode() != LED_GOAL && Leds::mode() != LED_UPDATING &&
                Leds::mode() != LED_DANCE) {
                if (victoryActive()) {
                    // Går före felläget med flit: segern är redan känd och
                    // sparad, så ett tillfälligt SHL-avbrott ska inte avbryta
                    // firandet.
                    Leds::setMode(LED_VICTORY);
                    status.state = "Seger — firar";
                } else if (!gDataOk && !pushActive()) {
                    Leds::setMode(LED_ERROR);
                    status.state = "Ingen kontakt med SHL";
                } else if (gInLiveWindow) {
                    // Vanlig push styr bara ställningen; en uppspelning bär
                    // matchläget också och får hela matchljuset.
                    const LedMode m = pushActive() && !gPushReplay ? LED_LIVE : serviceMood();
                    Leds::setMode(m);
                    status.state = moodStateText(m);
                    if (m == LED_LIVE && gMoodI >= MOOD_THRESHOLD)
                        status.state += " · slutspurt " + String((int)(gMoodI * 100)) + " %";
                } else {
                    Leds::setMode(LED_STANDBY);
                    status.state = "Standby";
                }
            }

            // Sist i varvet, och aldrig under en match: ett TLS-anrop tar
            // en sekund och ett tjugotal kB heap som live-strömmen behöver.
            Telemetry::loop(gInLiveWindow);

            status.dark = !lampAwake();
            Leds::setDark(status.dark);
            break;
        }

        case AppState::Boot:
        default:
            break;
    }
}
