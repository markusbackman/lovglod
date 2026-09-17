// ─────────────────────────────────────────────────────────────────────────────
//  LövGlöd
//
//  Standby        långsam gul glöd
//  Vann igår      några glittrande gnistor ovanpå glöden
//  Mål            snabb gul eldgivning i 12 sekunder
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
static uint32_t  gNextMidnightCheck = 0;
static uint32_t  gNextTimeRetry     = 0;

// Kör vi en firmware som ännu inte kvitterat sig frisk? Se serviceOtaValidation().
static bool      gOtaOnTrial        = false;
static bool      gWifiEverUp        = false;

static uint32_t  gPushUntil         = 0;      // push-läget lever tills hit
static bool      gDataOk            = true;   // false när SHL inte svarar alls
static bool      gFirstFetchPending = false;  // första hämtningen visar förlopp
static uint32_t  gNextResultPoll    = 0;      // tät koll efter matchens slut

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
    const char *name;
    LedMode     mode;
    bool        sparkles;
};

static const DemoItem kDemo[] = {
    {"uppstartsflöde",                LED_BOOT,         false},
    {"portal, setup (grön puls)",     LED_PORTAL,       false},
    {"portal, WiFi nekat (röd puls)", LED_PORTAL_RETRY, false},
    {"ansluter (gult jagande ljus)",  LED_CONNECTING,   false},
    {"uppstartsförlopp (stapel)",     LED_WORKING,      false},
    {"standby-glöd",                  LED_STANDBY,      false},
    {"standby + gnistor (vann igår)", LED_STANDBY,      true },
    {"live, match pågår",             LED_LIVE,         false},
    {"OTA-uppdatering (stapel)",      LED_UPDATING,     false},
    {"fel, ingen data (rött)",        LED_ERROR,        false},
    {"seger — 3 h efter vinst",       LED_VICTORY,      false},
};
static const uint8_t kDemoCount = sizeof(kDemo) / sizeof(kDemo[0]);

static int8_t   gDemoIdx  = -1;      // -1 = demoläget av
static uint32_t gDemoTick = 0;
static uint8_t  gDemoPct  = 0;

static void demoList() {
    Serial.println("[demo] animationer — siffra visar, v = seger, g = mål, x = normalt");
    for (uint8_t i = 0; i < kDemoCount; i++)
        Serial.printf("       %c  %s\n", i == 9 ? '0' : char('1' + i), kDemo[i].name);
}

static void demoSelect(int8_t idx) {
    if (idx < 0 || idx >= (int8_t)kDemoCount) return;
    gDemoIdx  = idx;
    gDemoPct  = 0;
    gDemoTick = 0;
    Leds::setSparkles(kDemo[idx].sparkles);
    Leds::lockMode(kDemo[idx].mode);
    Serial.printf("[demo] %d — %s\n", idx + 1, kDemo[idx].name);
}

static void demoOff() {
    if (gDemoIdx < 0) return;
    gDemoIdx = -1;
    Leds::setSparkles(false);
    Leds::unlockMode();
    Serial.println("[demo] av — normal drift igen");
}

// Två av lägena har ingen egen rörelse att titta på: uppstartssvepet är en
// engångsanimation, och stapellägena ritar bara det förlopp de matas med.
static void demoLoop() {
    if (gDemoIdx < 0) return;
    const LedMode m = kDemo[gDemoIdx].mode;

    if (m == LED_BOOT && millis() - gDemoTick > BOOT_FILL_MS + BOOT_HOLD_MS + 600) {
        gDemoTick = millis();
        Leds::lockMode(LED_BOOT);            // spela om flödet med paus emellan
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

// Hämtar senaste resultatet och hanterar båda sakerna det styr: gnistorna
// (gårdagens vinst) och segerläget (dagens).
static bool refreshLastResult() {
    LastResult lr;
    if (!Shl::fetchLastResult(lr)) {
        Serial.printf("[shl] resultat misslyckades: %s\n", Shl::lastError().c_str());
        return false;
    }
    Leds::setSparkles(lr.wonYesterday);
    status.wonYesterday = lr.wonYesterday;
    status.lastResult   = lr.summary;
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

        case 'x': case 'X':
            demoOff();
            break;

        case 'g': case 'G':
            Serial.println("[demo] MÅL!");
            Leds::triggerGoal();
            break;

        case 'v': case 'V':
            demoSelect(kDemoCount - 1);          // segerläget, sist i listan
            break;

        case '1': case '2': case '3': case '4': case '5':
        case '6': case '7': case '8': case '9':
            demoSelect(c - '1');
            break;
        case '0':
            demoSelect(9);
            break;

        case '\n': case '\r': break;
        default: printSerialHelp(); break;
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
    // "vann igår") bygger på korrekt tid.
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

    // Det vi hämtade utan klocka är räknat på fel datum: "vann igår" jämförde
    // mot 1970, och maybeArmVictory() hoppade över allt. Hämta om.
    gFetchNow = true;
    Serial.println("[tid] hämtar om matchdata som räknades utan klocka");
}

// ── Datahämtning ────────────────────────────────────────────────────────────
static void refreshSchedule(bool showProgress) {
    gFetchNow = false;
    if (showProgress) showWorkStep(0, 2);

    const bool resultOk = refreshLastResult();
    if (resultOk) Serial.printf("[shl] senaste: %s\n", status.lastResult.c_str());
    if (showProgress) showWorkStep(1, 2);

    NextGame n;
    const bool nextOk = Shl::fetchNextGame(n);
    if (nextOk) {
        // Ny match? Nollställ ställningen så att gamla mål inte trigger igen.
        if (n.uuid != gNext.uuid) gScore = LiveScore();
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
    if (!gNext.valid || !gTimeSynced) return false;
    const time_t now = time(nullptr);
    return now >= gNext.startUtc - (time_t)(LIVE_WINDOW_PRE_MS / 1000) &&
           now <= gNext.startUtc + (time_t)(LIVE_WINDOW_POST_MS / 1000);
}

// Jämför ny ställning mot den gamla och fyrar av vid mål.
static void applyScore(const LiveScore &fresh) {
    if (!fresh.valid || fresh.home < 0 || fresh.away < 0) return;

    status.liveScore = gNext.homeCode + " " + String(fresh.home) + " – " +
                       String(fresh.away) + " " + gNext.awayCode;

    if (!gScore.valid) {          // första avläsningen: bara kalibrera
        gScore = fresh;
        return;
    }

    const int dHome = fresh.home - gScore.home;
    const int dAway = fresh.away - gScore.away;
    gScore = fresh;

    if (dHome <= 0 && dAway <= 0) return;   // rättelse eller oförändrat

    const int ourGoals   = gNext.homeIsUs ? dHome : dAway;
    const int theirGoals = gNext.homeIsUs ? dAway : dHome;

    // Tv-bilden ligger efter live-datan. Utan fördröjning tänder lampan målet
    // för alla i rummet innan det syns på skärmen.
    const uint32_t delayMs = (uint32_t)settings.goalDelayS * 1000;

    if (ourGoals > 0) {
        Serial.printf("[MÅL] Björklöven! %d–%d%s\n", fresh.home, fresh.away,
                      delayMs ? "  (väntar på tv)" : "");
        Leds::triggerGoal(delayMs);
    } else if (theirGoals > 0 && !settings.goalOnlyOurTeam) {
        Serial.printf("[mål] motståndaren. %d–%d\n", fresh.home, fresh.away);
        Leds::triggerGoal(delayMs);
    }
}

static void serviceLive() {
    const bool inWindow = insideLiveWindow();

    if (inWindow != gInLiveWindow) {
        gInLiveWindow = inWindow;
        if (inWindow) {
            Serial.println("[live] matchfönster öppet — kopplar upp mot live-strömmen");
            Shl::sseStart(gNext.uuid);
            gNextLivePoll   = millis();
            // Tidsstämpeln kan vara månader gammal efter sommaruppehållet, och
            // då ser den ut att ligga i framtiden. Se gFetchNow.
            gNextResultPoll = millis();
        } else {
            Serial.println("[live] matchfönster stängt");
            Shl::sseStop();
            gScore = LiveScore();
            status.liveScore = "—";
            // Ett mål som fortfarande väntar på tv-fördröjningen när fönstret
            // stänger hör inte hemma i nästa match.
            Leds::clearPendingGoals();
            // Kolla resultatet strax efter matchen så gnistorna stämmer till imorgon.
            gNextScheduleFetch = millis() + 60000;
        }
    }

    if (!inWindow) return;

    LiveScore fresh;
    if (Shl::ssePump(fresh)) applyScore(fresh);

    // Reservpollning ifall SSE-strömmen är tyst eller formatet ändrats. Inte
    // medan målfyrverkeriet brinner: hämtningen blockerar loop() i upp till 20 s
    // och fryser animationen mitt i. Pollningen skjuts bara upp — den går så
    // fort fyrverkeriet är slut.
    if (Leds::mode() != LED_GOAL && (int32_t)(millis() - gNextLivePoll) >= 0) {
        gNextLivePoll = millis() + POLL_LIVE_FALLBACK_MS;
        LiveScore polled;
        if (Shl::pollLiveScore(gNext.uuid, polled)) applyScore(polled);
    }

    status.sseLive = Shl::sseConnected();

    // Nära slutsignalen: fråga played-games ofta, så segerläget tänds i
    // anslutning till matchen och inte vid nästa sexttimmarshämtning. Först
    // efter VICTORY_POLL_AFTER_MS — dessförinnan spelas det fortfarande, och
    // varje hämtning är ett blockerande TLS-anrop som fryser bilden ett ögonblick.
    if (gNext.valid && gTimeSynced && !victoryActive() &&
        time(nullptr) >= gNext.startUtc + (time_t)(VICTORY_POLL_AFTER_MS / 1000) &&
        (int32_t)(millis() - gNextResultPoll) >= 0) {
        gNextResultPoll = millis() + VICTORY_POLL_MS;
        refreshLastResult();
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
    }
    gPushUntil      = millis() + PUSH_LEASE_MS;
    status.pushMode = true;
    gDataOk         = true;

    if (p.hasNext) {
        // Byte av lag räknas som ny match — annars ser en omställd ställning
        // ut som ett mål.
        if (p.homeCode != gNext.homeCode || p.awayCode != gNext.awayCode)
            gScore = LiveScore();
        gNext.valid     = true;
        gNext.homeCode  = p.homeCode;
        gNext.awayCode  = p.awayCode;
        gNext.homeIsUs  = p.homeIsUs;
        status.nextGame = p.nextText.length() ? p.nextText
                                              : p.homeCode + " – " + p.awayCode;
    }

    if (p.hasLast) {
        status.wonYesterday = p.wonYesterday;
        status.lastResult   = p.lastResult;
        Leds::setSparkles(p.wonYesterday);
    }

    if (gInLiveWindow != p.live) {
        gInLiveWindow = p.live;
        Serial.printf("[push] matchfönster %s\n", p.live ? "öppet" : "stängt");
        if (!p.live) {
            gScore = LiveScore();
            status.liveScore = "—";
        }
    }

    if (p.live && p.hasScore) {
        LiveScore fresh;
        fresh.valid = true;
        fresh.home  = p.home;
        fresh.away  = p.away;
        applyScore(fresh);             // härifrån triggas målfyrverkeriet
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
    settings.noteBoot(esp_reset_reason() == ESP_RST_POWERON, status.resetAbnormal);
    Serial.printf("[boot] start nr %lu, %lu onormala omstarter sedan strömpåslag\n",
                  (unsigned long)settings.bootCount, (unsigned long)settings.abnormalBoots);
    checkRollbackState();
    status.otaOnTrial = gOtaOnTrial;
    Leds::begin(settings.ledStrip, settings.ledCount);
    Leds::setBrightness(settings.brightness);
    Leds::setMode(LED_BOOT);

    // Kort uppstartsflöde så man ser att listen lever
    const uint32_t until = millis() + BOOT_FILL_MS + BOOT_HOLD_MS;
    while (millis() < until) Leds::render();

    if (settings.hasWifi()) beginConnect();
    else                    enterPortal(false);
}

void loop() {
    Leds::render();
    Portal::loop();
    serviceOtaValidation();
    handleSerialCommands();
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
                gFirstFetchPending = false;
            }

            // Strax efter midnatt: uppdatera "vann igår" utan att vänta 6 h.
            if ((int32_t)(millis() - gNextMidnightCheck) >= 0) {
                gNextMidnightCheck = millis() + 15UL * 60 * 1000;
                if (gTimeSynced) {
                    struct tm lt;
                    const time_t now = time(nullptr);
                    localtime_r(&now, &lt);
                    if (lt.tm_hour == 0 && lt.tm_min < 20) gFetchNow = true;
                }
            }

            if (pushActive()) {
                // Allt kommer via POST /push — ingen SSE, ingen pollning.
            } else {
                if (status.pushMode) endPushMode();
                serviceLive();
            }

            {
                const bool manual = Updater::checkRequested();
                const bool due    = (int32_t)(millis() - gNextOtaCheck) >= 0;

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
            if (Leds::mode() != LED_GOAL && Leds::mode() != LED_UPDATING) {
                if (victoryActive()) {
                    // Går före felläget med flit: segern är redan känd och
                    // sparad, så ett tillfälligt SHL-avbrott ska inte avbryta
                    // firandet.
                    Leds::setMode(LED_VICTORY);
                    status.state = "Seger — firar";
                } else if (!gDataOk && !pushActive()) {
                    Leds::setMode(LED_ERROR);
                    status.state = "Ingen kontakt med SHL";
                } else {
                    Leds::setMode(gInLiveWindow ? LED_LIVE : LED_STANDBY);
                    status.state = gInLiveWindow ? "Match pågår" : "Standby";
                }
            }
            break;
        }

        case AppState::Boot:
        default:
            break;
    }
}
