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
    otaBadVersion   = prefs.getString("otabadv", "");
    otaBadCount     = prefs.getUChar("otabadc", 0);
    brightness      = prefs.getUChar("bright", LED_DEFAULT_BRIGHTNESS);
    goalOnlyOurTeam = prefs.getBool("ouronly", GOAL_ONLY_OUR_TEAM);
    prefs.end();
}

void Settings::save() {
    prefs.begin(NS, /*readOnly=*/false);
    prefs.putString("ssid", wifiSsid);
    prefs.putString("pass", wifiPass);
    prefs.putString("otasrc", otaSource);
    prefs.putString("otatok", otaToken);
    prefs.putString("otabadv", otaBadVersion);
    prefs.putUChar("otabadc", otaBadCount);
    prefs.putUChar("bright", brightness);
    prefs.putBool("ouronly", goalOnlyOurTeam);
    prefs.end();
}

void Settings::clearWifi() {
    wifiSsid = "";
    wifiPass = "";
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

void Settings::clearOtaFailures() {
    if (!otaBadVersion.length() && !otaBadCount) return;
    otaBadVersion = "";
    otaBadCount   = 0;
    save();
}

bool Settings::otaBlocked(const String &version) const {
    return otaBadVersion == version && otaBadCount >= OTA_MAX_FAILURES;
}
