#include "updater.h"
#include "config.h"
#include "settings.h"
#include "leds.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>

namespace {

bool   gPushReady = false;
bool   gRequested = false;
String gStatus    = "Ingen kontroll gjord än";

void showProgress(unsigned int done, unsigned int total) {
    if (!total) return;
    Leds::setUpdateProgress((uint8_t)((uint64_t)done * 100 / total));
    Leds::render();
}

// GitHub-taggar heter oftast "v1.2.3" men FW_VERSION är "1.2.3".
String normalizeVersion(String v) {
    v.trim();
    if (v.length() && (v[0] == 'v' || v[0] == 'V')) v.remove(0, 1);
    return v;
}

// Certifikatkedjan för release-nedladdningar hoppar mellan github.com och
// release-assets.githubusercontent.com och byts ut med jämna mellanrum, så en
// hårdkodad rot-CA gör bara enheten tyst en dag den roterar. Se README för
// hur du hårdar detta med signerad firmware istället.
void configureTls(WiFiClientSecure &c) {
    c.setInsecure();
    c.setTimeout(20);
}

}  // namespace

namespace Updater {

// ─────────────────────────────────────────────────────────────────────────────
void beginPush() {
    ArduinoOTA.setHostname(DEVICE_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]() {
        Serial.println("[ota] push-uppdatering startar");
        gStatus = "Push-uppdatering pågår";
        Leds::setUpdateProgress(0);
    });
    ArduinoOTA.onProgress(showProgress);
    ArduinoOTA.onEnd([]() {
        Serial.println("[ota] klar, startar om");
        Leds::setUpdateProgress(100);
        Leds::render();
    });
    ArduinoOTA.onError([](ota_error_t e) {
        Serial.printf("[ota] fel %u\n", e);
        gStatus = "Push-uppdatering misslyckades (" + String(e) + ")";
        Leds::setMode(LED_ERROR);
    });

    ArduinoOTA.begin();
    gPushReady = true;
    Serial.printf("[ota] lyssnar som %s.local\n", DEVICE_HOSTNAME);
}

void loop() {
    if (gPushReady) ArduinoOTA.handle();
}

// ─────────────────────────────────────────────────────────────────────────────
String resolveSourceUrl(const String &source) {
    String s = source;
    s.trim();
    if (!s.length()) return "";
    if (s.startsWith("http://") || s.startsWith("https://")) return s;

    // Kortform "owner/repo"
    const int slash = s.indexOf('/');
    if (slash > 0 && slash < (int)s.length() - 1 && s.indexOf('/', slash + 1) < 0)
        return "https://api.github.com/repos/" + s + "/releases/latest";

    return s;
}

bool queryLatest(const String &source, String &version, String &binUrl, uint32_t *assetId) {
    version = "";
    binUrl  = "";
    if (assetId) *assetId = 0;

    const String url = resolveSourceUrl(source);
    if (url.length() < 8 || WiFi.status() != WL_CONNECTED) return false;

    WiFiClientSecure client;
    configureTls(client);

    HTTPClient http;
    http.setTimeout(15000);
    http.setConnectTimeout(10000);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!http.begin(client, url)) { gStatus = "Kunde inte nå uppdateringskällan"; return false; }

    // GitHub svarar 403 utan User-Agent.
    http.addHeader("User-Agent", "BjorklovenLED/" FW_VERSION);
    http.addHeader("Accept", "application/vnd.github+json");
    // Privata repon kräver token. Publika fungerar utan.
    if (settings.otaToken.length())
        http.addHeader("Authorization", "Bearer " + settings.otaToken);

    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        gStatus = (code == 404) ? "Ingen release hittad (privat repo utan token?)"
                : (code == 401) ? "GitHub: token avvisad (401)"
                : (code == 403) ? "GitHub API: nekad eller för många anrop (403)"
                                : "HTTP " + String(code) + " från uppdateringskällan";
        http.end();
        return false;
    }

    // Filtret gör att vi kan strömma svaret. En GitHub-release med många
    // assets kan vara flera hundra kB — utan filter tar heapen slut.
    JsonDocument filter;
    filter["tag_name"] = true;                       // GitHub Releases
    JsonObject asset = filter["assets"].add<JsonObject>();
    asset["name"] = true;
    asset["browser_download_url"] = true;
    asset["id"] = true;                              // behövs för privata repon
    filter["version"] = true;                        // eget manifest
    filter["url"]     = true;
    // Obs: GitHub-svaret har också ett "url" på toppnivå (länken till releasen
    // i API:t). Därför avgörs formatet av "tag_name" nedan, inte av "url".

    JsonDocument doc;
    const DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();

    if (err) { gStatus = String("Kunde inte tolka svaret: ") + err.c_str(); return false; }

    if (doc["tag_name"].is<const char *>()) {
        // ── GitHub Releases ──
        version = normalizeVersion(doc["tag_name"].as<const char *>());
        for (JsonVariantConst a : doc["assets"].as<JsonArrayConst>()) {
            if (String(a["name"] | "") == OTA_ASSET_NAME) {
                binUrl = a["browser_download_url"] | "";
                if (assetId) *assetId = a["id"] | 0UL;
                break;
            }
        }
        if (!binUrl.length()) {
            gStatus = "Releasen " + version + " saknar " OTA_ASSET_NAME;
            return false;
        }
    } else {
        // ── Eget manifest ──
        version = normalizeVersion(doc["version"] | "");
        binUrl  = doc["url"] | "";
        if (!version.length() || !binUrl.length()) {
            gStatus = "Manifestet saknar \"version\" eller \"url\"";
            return false;
        }
    }

    return true;
}

// På ett privat repo är browser_download_url oanvändbar — den kräver en
// inloggad webbsession. Rätt väg är assets-API:t med Accept: octet-stream,
// som svarar 302 till en signerad engångs-URL.
//
// Redirecten följs medvetet för hand. HTTPClient skickar med samma headers
// vidare till målet, och den signerade URL:en bär redan sina egna
// engångscredentials — att dessutom skicka vår PAT dit är onödig exponering.
// Vi plockar ut Location och lämnar en ren URL till httpUpdate.
static bool resolvePrivateAssetUrl(const String &source, uint32_t assetId, String &signedUrl) {
    const String api = resolveSourceUrl(source);
    const int    idx = api.indexOf("/releases");
    if (idx < 0) return false;

    const String url = api.substring(0, idx) + "/releases/assets/" + String(assetId);

    WiFiClientSecure client;
    configureTls(client);

    HTTPClient http;
    http.setTimeout(15000);
    http.setConnectTimeout(10000);
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

    if (!http.begin(client, url)) return false;
    http.addHeader("User-Agent", "BjorklovenLED/" FW_VERSION);
    http.addHeader("Accept", "application/octet-stream");
    http.addHeader("Authorization", "Bearer " + settings.otaToken);

    const char *want[] = {"Location"};
    http.collectHeaders(want, 1);

    const int code = http.GET();
    if (code == HTTP_CODE_FOUND || code == HTTP_CODE_TEMPORARY_REDIRECT ||
        code == HTTP_CODE_MOVED_PERMANENTLY) {
        signedUrl = http.header("Location");
    } else {
        gStatus = "Kunde inte hämta assetens nedladdningslänk (HTTP " + String(code) + ")";
    }
    http.end();
    return signedUrl.length() > 0;
}

bool checkAndApply(const String &source) {
    String   latest, binUrl;
    uint32_t assetId = 0;
    if (!queryLatest(source, latest, binUrl, &assetId)) {
        Serial.printf("[ota] kontroll misslyckades: %s\n", gStatus.c_str());
        return false;
    }

    const String current = normalizeVersion(FW_VERSION);

    if (latest == current) {
        gStatus = "Senaste versionen (" + current + ")";
        Serial.printf("[ota] %s\n", gStatus.c_str());
        settings.clearOtaFailures();
        return false;
    }

    if (settings.otaBlocked(latest)) {
        gStatus = "Version " + latest + " misslyckades " + String(settings.otaBadCount) +
                  " gånger — hoppas över";
        Serial.printf("[ota] %s\n", gStatus.c_str());
        return false;
    }

    // Versionen *skiljer sig* — inte nödvändigtvis nyare. Det är avsiktligt:
    // markerar du en trasig release som pre-release på GitHub pekar
    // /releases/latest tillbaka på den förra, och lampan rullar tillbaka.
    Serial.printf("[ota] %s → %s\n", current.c_str(), latest.c_str());

    if (settings.otaToken.length() && assetId) {
        String signedUrl;
        if (!resolvePrivateAssetUrl(source, assetId, signedUrl)) {
            Serial.printf("[ota] %s\n", gStatus.c_str());
            settings.noteOtaFailure(latest);
            return false;
        }
        binUrl = signedUrl;
        Serial.println("[ota] hämtar via signerad asset-URL (privat repo)");
    } else {
        Serial.printf("[ota] hämtar %s\n", binUrl.c_str());
    }

    gStatus = "Installerar " + latest + "…";

    Leds::setUpdateProgress(0);
    httpUpdate.setLedPin(-1);
    httpUpdate.rebootOnUpdate(true);
    httpUpdate.onProgress(showProgress);
    // github.com svarar 302 vidare till release-assets.githubusercontent.com.
    httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    WiFiClientSecure binClient;
    configureTls(binClient);

    const t_httpUpdate_return ret = httpUpdate.update(binClient, binUrl, FW_VERSION);
    switch (ret) {
        case HTTP_UPDATE_OK:
            return true;                     // enheten startar om

        case HTTP_UPDATE_NO_UPDATES:
            gStatus = "Servern hade inget nytt";
            Leds::setMode(LED_STANDBY);
            return false;

        case HTTP_UPDATE_FAILED:
        default:
            gStatus = "Installation av " + latest + " misslyckades: " +
                      httpUpdate.getLastErrorString();
            Serial.printf("[ota] misslyckades (%d): %s\n", httpUpdate.getLastError(),
                          httpUpdate.getLastErrorString().c_str());
            settings.noteOtaFailure(latest);
            Leds::setMode(LED_STANDBY);
            return false;
    }
}

void requestCheck()  { gRequested = true; }
bool checkRequested(){ return gRequested; }
void clearRequest()  { gRequested = false; }

const String &statusText() { return gStatus; }

}  // namespace Updater
