// ─────────────────────────────────────────────────────────────────────────────
//  Björklöven-lampan
//
//  Standby        långsam gul glöd
//  Vann igår      några glittrande gnistor ovanpå glöden
//  Mål            snabb gul eldgivning i 12 sekunder
//  Inget WiFi     eget nät + captive portal som frågar efter SSID/lösenord
//  Uppdatering    ArduinoOTA (push) + valfri self-update från URL
//
//  Data: SHL:s eget publika API (www.shl.se/api) + live-SSE från
//        game-broadcaster.s8y.se. Björklöven spelar i SHL från 2026/27.
// ─────────────────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>

#include "config.h"
#include "settings.h"
#include "leds.h"
#include "portal.h"
#include "shl.h"
#include "updater.h"

// ── Tillstånd ───────────────────────────────────────────────────────────────
enum class AppState { Boot, Portal, Connecting, Online };
static AppState  gState = AppState::Boot;

static NextGame  gNext;
static LiveScore gScore;            // senast kända ställning i pågående match
static bool      gInLiveWindow = false;
static bool      gTimeSynced   = false;

static uint32_t  gNextScheduleFetch = 0;
static uint32_t  gNextLivePoll      = 0;
static uint32_t  gNextOtaCheck      = 0;
static uint32_t  gPortalSince       = 0;
static uint32_t  gConnectStarted    = 0;
static uint32_t  gNextMidnightCheck = 0;

// ─────────────────────────────────────────────────────────────────────────────
static void syncTime() {
    configTzTime(TZ_STOCKHOLM, NTP_SERVER_1, NTP_SERVER_2);

    // Vänta max 10 s på att klockan blir rimlig — allt annat (matchfönster,
    // "vann igår") bygger på korrekt tid.
    const uint32_t deadline = millis() + 10000;
    while (time(nullptr) < 1700000000 && millis() < deadline) {
        Leds::render();
        delay(50);
    }
    gTimeSynced = time(nullptr) > 1700000000;

    if (gTimeSynced) {
        char buf[32];
        const time_t now = time(nullptr);
        struct tm lt;
        localtime_r(&now, &lt);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &lt);
        Serial.printf("[tid] synkad: %s\n", buf);
    } else {
        Serial.println("[tid] NTP misslyckades");
    }
}

static String formatLocal(time_t utc, const char *fmt) {
    struct tm lt;
    localtime_r(&utc, &lt);
    char buf[48];
    strftime(buf, sizeof(buf), fmt, &lt);
    return String(buf);
}

// ── Datahämtning ────────────────────────────────────────────────────────────
static void refreshSchedule() {
    gNextScheduleFetch = millis() + POLL_SCHEDULE_MS;

    bool   won = false;
    String summary;
    if (Shl::fetchLastResult(won, summary)) {
        Leds::setSparkles(won);
        status.wonYesterday = won;
        status.lastResult   = summary;
        Serial.printf("[shl] senaste: %s  (gnistor: %s)\n", summary.c_str(), won ? "på" : "av");
    } else {
        Serial.printf("[shl] resultat misslyckades: %s\n", Shl::lastError().c_str());
    }

    NextGame n;
    if (Shl::fetchNextGame(n)) {
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

    if (ourGoals > 0) {
        Serial.printf("[MÅL] Björklöven! %d–%d\n", fresh.home, fresh.away);
        Leds::triggerGoal();
    } else if (theirGoals > 0 && !settings.goalOnlyOurTeam) {
        Serial.printf("[mål] motståndaren. %d–%d\n", fresh.home, fresh.away);
        Leds::triggerGoal();
    }
}

static void serviceLive() {
    const bool inWindow = insideLiveWindow();

    if (inWindow != gInLiveWindow) {
        gInLiveWindow = inWindow;
        if (inWindow) {
            Serial.println("[live] matchfönster öppet — kopplar upp mot live-strömmen");
            Shl::sseStart(gNext.uuid);
            gNextLivePoll = millis();
        } else {
            Serial.println("[live] matchfönster stängt");
            Shl::sseStop();
            gScore = LiveScore();
            status.liveScore = "—";
            // Kolla resultatet strax efter matchen så gnistorna stämmer till imorgon.
            gNextScheduleFetch = millis() + 60000;
        }
    }

    if (!inWindow) return;

    LiveScore fresh;
    if (Shl::ssePump(fresh)) applyScore(fresh);

    // Reservpollning ifall SSE-strömmen är tyst eller formatet ändrats.
    if ((int32_t)(millis() - gNextLivePoll) >= 0) {
        gNextLivePoll = millis() + POLL_LIVE_FALLBACK_MS;
        LiveScore polled;
        if (Shl::pollLiveScore(gNext.uuid, polled)) applyScore(polled);
    }

    status.sseLive = Shl::sseConnected();
}

// ── WiFi ────────────────────────────────────────────────────────────────────
static void beginConnect() {
    Serial.printf("[wifi] ansluter till \"%s\"\n", settings.wifiSsid.c_str());
    Portal::stop();
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);            // WiFi-sleep ger hack i LED-timingen
    WiFi.setHostname(DEVICE_HOSTNAME);
    WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPass.c_str());

    gConnectStarted = millis();
    gState = AppState::Connecting;
    Leds::setMode(LED_CONNECTING);
    status.state = "Ansluter";
}

static void enterPortal() {
    Serial.println("[wifi] ingen anslutning — startar setup-portalen");
    Portal::startAccessPoint();
    gPortalSince = millis();
    gState = AppState::Portal;
    Leds::setMode(LED_PORTAL);
    status.state = "Setup-läge";
}

static void goOnline() {
    Serial.printf("[wifi] ansluten, IP %s\n", WiFi.localIP().toString().c_str());

    syncTime();

    if (MDNS.begin(DEVICE_HOSTNAME)) MDNS.addService("http", "tcp", 80);
    Portal::startStationServer();
    Updater::beginPush();

    gState = AppState::Online;
    Leds::setMode(LED_STANDBY);
    status.state = "Standby";

    gNextScheduleFetch = 0;                       // hämta direkt
    gNextOtaCheck      = millis() + 60000;        // men vänta lite med OTA-kollen
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n\n== Björklöven-lampan v" FW_VERSION " ==");

    settings.load();
    Leds::begin();
    Leds::setBrightness(settings.brightness);
    Leds::setMode(LED_BOOT);

    // Kort uppstartssvep så man ser att listen lever
    const uint32_t until = millis() + 1000;
    while (millis() < until) Leds::render();

    if (settings.hasWifi()) beginConnect();
    else                    enterPortal();
}

void loop() {
    Leds::render();
    Portal::loop();
    Updater::loop();

    switch (gState) {
        case AppState::Connecting:
            if (WiFi.status() == WL_CONNECTED) {
                goOnline();
            } else if (millis() - gConnectStarted > WIFI_CONNECT_TIMEOUT_MS) {
                WiFi.disconnect(true);
                enterPortal();
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

            if ((int32_t)(millis() - gNextScheduleFetch) >= 0) refreshSchedule();

            // Strax efter midnatt: uppdatera "vann igår" utan att vänta 6 h.
            if ((int32_t)(millis() - gNextMidnightCheck) >= 0) {
                gNextMidnightCheck = millis() + 15UL * 60 * 1000;
                if (gTimeSynced) {
                    struct tm lt;
                    const time_t now = time(nullptr);
                    localtime_r(&now, &lt);
                    if (lt.tm_hour == 0 && lt.tm_min < 20) gNextScheduleFetch = 0;
                }
            }

            serviceLive();

            {
                const bool manual = Updater::checkRequested();
                const bool due    = (int32_t)(millis() - gNextOtaCheck) >= 0;

                if (settings.otaSource.length() && (manual || due)) {
                    gNextOtaCheck = millis() + OTA_CHECK_MS;
                    Updater::clearRequest();
                    // Aldrig automatiskt mitt i en match — en omstart i tredje
                    // perioden vore synd. Manuell begäran får gå igenom ändå.
                    if (manual || !gInLiveWindow) Updater::checkAndApply(settings.otaSource);
                } else if (manual) {
                    Updater::clearRequest();
                }
            }

            // Håll LED-läget i synk med tillståndet (målfyrverkeriet får styra själv).
            if (Leds::mode() != LED_GOAL && Leds::mode() != LED_UPDATING) {
                Leds::setMode(gInLiveWindow ? LED_LIVE : LED_STANDBY);
                status.state = gInLiveWindow ? "Match pågår" : "Standby";
            }
            break;
        }

        case AppState::Boot:
        default:
            break;
    }
}
