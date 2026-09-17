#include "settings.h"
#include <Preferences.h>
#include "config.h"

Settings settings;
static Preferences prefs;
static const char *NS = "lovenled";

void Settings::load() {
    prefs.begin(NS, /*readOnly=*/true);
    wifiSsid        = prefs.getString("ssid", "");
    wifiPass        = prefs.getString("pass", "");
    otaSource       = prefs.getString("otasrc", OTA_DEFAULT_SOURCE);
    otaToken        = prefs.getString("otatok", "");
    otaBeta         = prefs.getBool("otabeta", false);
    otaBadVersion   = prefs.getString("otabadv", "");
    otaPendingVersion = prefs.getString("otapend", "");
    otaBadCount     = prefs.getUChar("otabadc", 0);
    brightness      = prefs.getUChar("bright", LED_DEFAULT_BRIGHTNESS);
    goalOnlyOurTeam = prefs.getBool("ouronly", GOAL_ONLY_OUR_TEAM);
    debugPush       = prefs.getBool("dbgpush", false);
    goalDelayS      = prefs.getUChar("goaldly", GOAL_DELAY_DEFAULT_S);
    victoryUntil    = prefs.getULong("victuntil", 0);
    victoryGame     = prefs.getULong("victgame", 0);
    ledStrip        = (LedStrip)prefs.getUChar("ledstrip", LED_STRIP_UNSET);
    ledCount        = prefs.getUShort("ledcount", LED_COUNT_DEFAULT);

    // Lampor från före listvalet har WiFi men ingen listnyckel. De sitter alla
    // med WS2812B — det var den enda list firmwaren kunde driva — och utan det
    // här skulle varje lampa ute i fält börja driva båda utgångarna efter OTA.
    const bool legacy = !prefs.isKey("ledstrip") && wifiSsid.length();
    prefs.end();

    if (goalDelayS > GOAL_DELAY_MAX_S) goalDelayS = GOAL_DELAY_MAX_S;
    if (ledStrip > LED_STRIP_APA102)   ledStrip = LED_STRIP_UNSET;
    ledCount = constrain(ledCount, LED_COUNT_MIN, LED_COUNT_MAX);

    if (legacy) {
        ledStrip = LED_STRIP_WS2812;
        save();
    }
}

void Settings::save() {
    prefs.begin(NS, /*readOnly=*/false);
    prefs.putString("ssid", wifiSsid);
    prefs.putString("pass", wifiPass);
    prefs.putString("otasrc", otaSource);
    prefs.putString("otatok", otaToken);
    prefs.putBool("otabeta", otaBeta);
    prefs.putString("otabadv", otaBadVersion);
    prefs.putString("otapend", otaPendingVersion);
    prefs.putUChar("otabadc", otaBadCount);
    prefs.putUChar("bright", brightness);
    prefs.putBool("ouronly", goalOnlyOurTeam);
    prefs.putBool("dbgpush", debugPush);
    prefs.putUChar("goaldly", goalDelayS);
    prefs.putULong("victuntil", victoryUntil);
    prefs.putULong("victgame", victoryGame);
    prefs.putUChar("ledstrip", ledStrip);
    prefs.putUShort("ledcount", ledCount);
    prefs.end();
}

void Settings::clearWifi() {
    wifiSsid = "";
    wifiPass = "";
    save();
}

void Settings::noteVictory(uint32_t gameStartUtc, uint32_t untilUtc) {
    victoryGame  = gameStartUtc;
    victoryUntil = untilUtc;
    save();
}

// Nollar bara sluttiden. victoryGame ligger kvar — den är kvittot på att
// matchen redan är firad, och utan den skulle nästa hämtning tända om läget.
void Settings::clearVictory() {
    if (!victoryUntil) return;
    victoryUntil = 0;
    save();
}

// Räkna upp misslyckanden för en specifik version. Byter versionen börjar
// räknaren om — en ny release ska alltid få ett ärligt försök.
void Settings::noteOtaFailure(const String &version) {
    if (otaBadVersion != version) {
        otaBadVersion = version;
        otaBadCount   = 0;
    }
    if (otaBadCount < 255) otaBadCount++;
    save();
}

// En version som installerades, startade och sedan rullades tillbaka har haft
// sin chans. Till skillnad från en misslyckad nedladdning — som gärna får
// försökas om — blockeras den direkt: den är bevisligen oanvändbar på just den
// här enheten, och varje nytt försök kostar 1 MB och två omstarter.
void Settings::noteOtaRollback(const String &version) {
    otaBadVersion     = version;
    otaBadCount       = OTA_MAX_FAILURES;
    otaPendingVersion = "";
    save();
}

void Settings::noteOtaPending(const String &version) {
    otaPendingVersion = version;
    save();
}

void Settings::clearOtaPending() {
    if (!otaPendingVersion.length()) return;
    otaPendingVersion = "";
    save();
}

void Settings::clearOtaFailures() {
    if (!otaBadVersion.length() && !otaBadCount) return;
    otaBadVersion = "";
    otaBadCount   = 0;
    save();
}

bool Settings::otaBlocked(const String &version) const {
    return otaBadVersion == version && otaBadCount >= OTA_MAX_FAILURES;
}
