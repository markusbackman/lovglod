#include "updater.h"
#include "config.h"
#include "settings.h"
#include "leds.h"
#include "ota_pubkey.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <mbedtls/sha256.h>
#include <mbedtls/pk.h>
#include <mbedtls/base64.h>

namespace {

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
// hårdkodad rot-CA gör bara enheten tyst en dag den roterar.
//
// Det är därför signaturkontrollen nedan finns. Med den spelar transporten
// ingen roll: en angripare som kan byta ut svaret kan ändå inte producera en
// binär som går igenom mbedtls_pk_verify() utan den privata nyckeln.
void configureTls(WiFiClientSecure &c) {
    c.setInsecure();
    c.setTimeout(20);
}

// ── Integritet och äkthet ───────────────────────────────────────────────────
// Två oberoende kontroller, båda *före* Update.end():
//
//   sha256    fångar en trasig eller avbruten nedladdning och ger den ett eget
//             felmeddelande, så att "nätet strulade" inte ser likadant ut som
//             "någon försöker lura in firmware".
//   signatur  fångar allt annat, och är den som gör transporten irrelevant.
//
// Ordningen är hela poängen. Update.end() sätter boot-partitionen, så allt som
// ska kunna säga nej måste säga det innan dess. Update.abort() lämnar den
// gamla partitionen orörd och enheten fortsätter på nuvarande firmware.
//
// Det finns ingen andra väg in. ArduinoOTA-push togs bort med flit: den gick
// förbi hela den här kontrollen och skyddades bara av ett hårdkodat lösenord.
// Uppdateringar sker antingen härifrån — signerade — eller över USB, och den
// som står vid USB-porten är ändå förbi varje mjukvaruspärr.
bool hexEquals(const uint8_t digest[32], const String &hex) {
    if (hex.length() != 64) return false;
    for (uint8_t i = 0; i < 32; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", digest[i]);
        if (tolower(hex[i * 2]) != buf[0] || tolower(hex[i * 2 + 1]) != buf[1])
            return false;
    }
    return true;
}

bool verifySignature(const uint8_t digest[32], const String &sigB64) {
    // RSA-2048 ger 256 byte. Taket rymmer RSA-4096 om nyckeln byts upp.
    uint8_t sig[512];
    size_t  sigLen = 0;
    if (mbedtls_base64_decode(sig, sizeof(sig), &sigLen,
                              (const uint8_t *)sigB64.c_str(), sigB64.length()) != 0) {
        gStatus = "Signaturen gick inte att avkoda";
        return false;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    // Nyckeln måste vara nullterminerad — därav +1 på längden.
    const char *pem = OTA_PUBLIC_KEY_PEM;
    if (mbedtls_pk_parse_public_key(&pk, (const uint8_t *)pem, strlen(pem) + 1) != 0) {
        // Fail closed. Utan giltig nyckel installerar vi ingenting alls; att
        // falla tillbaka på osignerat vore att göra hela kontrollen valfri för
        // den som kan ta bort headern.
        gStatus = "Ingen giltig publik nyckel i firmwaren — self-update avstängt";
        mbedtls_pk_free(&pk);
        return false;
    }

    const int rc = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, digest, 32, sig, sigLen);
    mbedtls_pk_free(&pk);

    if (rc != 0) {
        gStatus = "Signaturen stämmer inte — installationen avbruten";
        return false;
    }
    return true;
}

}  // namespace

namespace Updater {

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

// På ett privat repo är browser_download_url oanvändbar — den kräver en
// inloggad webbsession. Rätt väg är assets-API:t med Accept: octet-stream,
// som svarar 302 till en signerad engångs-URL.
//
// Redirecten följs medvetet för hand. HTTPClient skickar med samma headers
// vidare till målet, och den signerade URL:en bär redan sina egna
// engångscredentials — att dessutom skicka vår PAT dit är onödig exponering.
// Vi plockar ut Location och lämnar en ren URL till nedladdningen.
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

// Strömmande GET som tolkar JSON genom ett filter. Filtret är inte kosmetiskt:
// en GitHub-release med många assets är flera hundra kB och heapen tar slut
// utan det.
static bool getJson(const String &url, JsonDocument &doc, const JsonDocument &filter,
                    bool withToken, const char *accept) {
    if (WiFi.status() != WL_CONNECTED) { gStatus = "WiFi nere"; return false; }

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
    http.addHeader("Accept", accept);
    // Privata repon kräver token. Publika fungerar utan.
    if (withToken && settings.otaToken.length())
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

    const DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();

    if (err) { gStatus = String("Kunde inte tolka svaret: ") + err.c_str(); return false; }
    return true;
}

// Manifestet bär det som binären inte kan bära själv: summan och signaturen.
static bool fetchManifest(const String &url, ReleaseInfo &out) {
    JsonDocument filter;
    filter["version"] = true;
    filter["sha256"]  = true;
    filter["sig"]     = true;
    filter["size"]    = true;
    filter["url"]     = true;

    JsonDocument doc;
    if (!getJson(url, doc, filter, true, "application/json")) return false;

    out.sha256Hex = doc["sha256"] | "";
    out.sigB64    = doc["sig"]    | "";
    out.size      = doc["size"]   | 0UL;
    return true;
}

bool queryLatest(const String &source, ReleaseInfo &out) {
    out = ReleaseInfo();

    const String url = resolveSourceUrl(source);
    if (url.length() < 8) return false;

    JsonDocument filter;
    filter["tag_name"] = true;                       // GitHub Releases
    JsonObject asset = filter["assets"].add<JsonObject>();
    asset["name"] = true;
    asset["browser_download_url"] = true;
    asset["id"] = true;                              // behövs för privata repon
    filter["version"] = true;                        // eget manifest
    filter["url"]     = true;
    filter["sha256"]  = true;
    filter["sig"]     = true;
    filter["size"]    = true;
    // Obs: GitHub-svaret har också ett "url" på toppnivå (länken till releasen
    // i API:t). Därför avgörs formatet av "tag_name" nedan, inte av "url".

    JsonDocument doc;
    if (!getJson(url, doc, filter, true, "application/vnd.github+json")) return false;

    if (!doc["tag_name"].is<const char *>()) {
        // ── Eget manifest: allt står redan här ──
        out.version   = normalizeVersion(doc["version"] | "");
        out.binUrl    = doc["url"]    | "";
        out.sha256Hex = doc["sha256"] | "";
        out.sigB64    = doc["sig"]    | "";
        out.size      = doc["size"]   | 0UL;
        if (!out.version.length() || !out.binUrl.length()) {
            gStatus = "Manifestet saknar \"version\" eller \"url\"";
            return false;
        }
        return true;
    }

    // ── GitHub Releases: binär och manifest ligger som var sin asset ──
    out.version = normalizeVersion(doc["tag_name"].as<const char *>());

    String   manifestUrl;
    uint32_t manifestId = 0;
    for (JsonVariantConst a : doc["assets"].as<JsonArrayConst>()) {
        const String name = a["name"] | "";
        if (name == OTA_ASSET_NAME) {
            out.binUrl     = a["browser_download_url"] | "";
            out.binAssetId = a["id"] | 0UL;
        } else if (name == OTA_MANIFEST_NAME) {
            manifestUrl = a["browser_download_url"] | "";
            manifestId  = a["id"] | 0UL;
        }
    }

    if (!out.binUrl.length()) {
        gStatus = "Releasen " + out.version + " saknar " OTA_ASSET_NAME;
        return false;
    }
    if (!manifestUrl.length()) {
        gStatus = "Releasen " + out.version + " saknar " OTA_MANIFEST_NAME
                  " — utan manifest finns ingen signatur att kontrollera";
        return false;
    }

    // Privat repo: manifestet måste hämtas via den signerade asset-URL:en av
    // samma skäl som binären.
    if (settings.otaToken.length() && manifestId) {
        String signedUrl;
        if (!resolvePrivateAssetUrl(source, manifestId, signedUrl)) return false;
        manifestUrl = signedUrl;
    }

    return fetchManifest(manifestUrl, out);
}

// ─────────────────────────────────────────────────────────────────────────────
// Nedladdningen drivs för hand i stället för med httpUpdate. Skälet är
// kontrollordningen: httpUpdate anropar Update.end() själv och startar om, och
// då är boot-partitionen redan satt. Vi behöver kunna säga nej mellan sista
// byten och commit.
static bool downloadAndInstall(const String &url, const ReleaseInfo &rel) {
    WiFiClientSecure client;
    configureTls(client);

    HTTPClient http;
    http.setTimeout(20000);
    http.setConnectTimeout(10000);
    http.setReuse(false);
    // github.com svarar 302 vidare till release-assets.githubusercontent.com.
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!http.begin(client, url)) { gStatus = "Kunde inte nå firmware-filen"; return false; }
    http.addHeader("User-Agent", "BjorklovenLED/" FW_VERSION);

    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        gStatus = "HTTP " + String(code) + " vid nedladdning";
        http.end();
        return false;
    }

    const int len = http.getSize();
    if (len <= 0) {
        gStatus = "Servern uppgav ingen storlek på firmware-filen";
        http.end();
        return false;
    }
    if (rel.size && (uint32_t)len != rel.size) {
        gStatus = "Storleken avviker från manifestet (" + String(len) + " mot " +
                  String(rel.size) + ")";
        http.end();
        return false;
    }

    if (!Update.begin((size_t)len, U_FLASH)) {
        gStatus = String("Update.begin misslyckades: ") + Update.errorString();
        http.end();
        return false;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, /*is224=*/0);

    Stream  *stream   = http.getStreamPtr();
    uint8_t  buf[1024];
    size_t   done     = 0;
    uint32_t lastData = millis();
    bool     ok       = true;

    while (done < (size_t)len) {
        const size_t avail = stream->available();
        if (!avail) {
            if (!client.connected() || millis() - lastData > 20000) {
                gStatus = "Nedladdningen bröts efter " + String(done) + " av " +
                          String(len) + " byte";
                ok = false;
                break;
            }
            Leds::render();
            delay(1);
            continue;
        }

        const size_t want = avail < sizeof(buf) ? avail : sizeof(buf);
        const int    n    = stream->readBytes(buf, want);
        if (n <= 0) continue;
        lastData = millis();

        // Hashen räknas på det som kommer över nätet, inte på det som ligger i
        // flashen. Update:s egen bufferthantering är därmed ute ur bilden.
        mbedtls_sha256_update_ret(&sha, buf, (size_t)n);

        if (Update.write(buf, (size_t)n) != (size_t)n) {
            gStatus = String("Skrivfel mot flashen: ") + Update.errorString();
            ok = false;
            break;
        }
        done += (size_t)n;
        showProgress(done, (unsigned int)len);
    }
    http.end();

    uint8_t digest[32];
    mbedtls_sha256_finish_ret(&sha, digest);
    mbedtls_sha256_free(&sha);

    if (!ok)                              { Update.abort(); return false; }

    if (!hexEquals(digest, rel.sha256Hex)) {
        gStatus = "sha256 stämmer inte — nedladdningen är skadad";
        Update.abort();
        return false;
    }
    Serial.println("[ota] sha256 OK");

    if (!verifySignature(digest, rel.sigB64)) { Update.abort(); return false; }
    Serial.println("[ota] signatur OK");

    if (!Update.end(true)) {
        gStatus = String("Kunde inte slutföra installationen: ") + Update.errorString();
        return false;
    }
    return true;
}

bool checkAndApply(const String &source) {
    ReleaseInfo rel;
    if (!queryLatest(source, rel)) {
        Serial.printf("[ota] kontroll misslyckades: %s\n", gStatus.c_str());
        return false;
    }

    const String current = normalizeVersion(FW_VERSION);

    if (rel.version == current) {
        gStatus = "Senaste versionen (" + current + ")";
        Serial.printf("[ota] %s\n", gStatus.c_str());
        settings.clearOtaFailures();
        return false;
    }

    if (settings.otaBlocked(rel.version)) {
        gStatus = "Version " + rel.version + " misslyckades " + String(settings.otaBadCount) +
                  " gånger — hoppas över";
        Serial.printf("[ota] %s\n", gStatus.c_str());
        return false;
    }

    // Versionen *skiljer sig* — inte nödvändigtvis nyare. Det är avsiktligt:
    // markerar du en trasig release som pre-release på GitHub pekar
    // /releases/latest tillbaka på den förra, och lampan rullar tillbaka.
    Serial.printf("[ota] %s → %s\n", current.c_str(), rel.version.c_str());

    // Fail closed innan en enda byte laddas ner. En release utan summa eller
    // signatur är inte "osignerad men okej", den är en release vi inte kan
    // avgöra något om.
    if (!rel.sha256Hex.length() || !rel.sigB64.length()) {
        gStatus = "Releasen saknar sha256 eller signatur — installerar inte";
        Serial.printf("[ota] %s\n", gStatus.c_str());
        settings.noteOtaFailure(rel.version);
        return false;
    }

    String binUrl = rel.binUrl;
    if (settings.otaToken.length() && rel.binAssetId) {
        String signedUrl;
        if (!resolvePrivateAssetUrl(source, rel.binAssetId, signedUrl)) {
            Serial.printf("[ota] %s\n", gStatus.c_str());
            settings.noteOtaFailure(rel.version);
            return false;
        }
        binUrl = signedUrl;
        Serial.println("[ota] hämtar via signerad asset-URL (privat repo)");
    } else {
        Serial.printf("[ota] hämtar %s\n", binUrl.c_str());
    }

    gStatus = "Installerar " + rel.version + "…";
    Leds::setUpdateProgress(0);

    if (!downloadAndInstall(binUrl, rel)) {
        Serial.printf("[ota] misslyckades: %s\n", gStatus.c_str());
        settings.noteOtaFailure(rel.version);
        Leds::setMode(LED_STANDBY);
        return false;
    }

    // Skriv ner vad vi startar om i, innan vi gör det. Rullar bootloadern
    // tillbaka den är det här den gamla firmwaren läser för att veta vilken
    // version som ska svartlistas. Se B5 i PRODUKTIONSKLAR.md.
    settings.noteOtaPending(rel.version);

    gStatus = "Installerade " + rel.version;
    Serial.println("[ota] klar — startar om i den nya firmwaren, på prov");
    Leds::setUpdateProgress(100);
    Leds::renderNow();
    delay(200);
    ESP.restart();
    return true;                          // nås aldrig
}

void requestCheck()  { gRequested = true; }
bool checkRequested(){ return gRequested; }
void clearRequest()  { gRequested = false; }

const String &statusText() { return gStatus; }

}  // namespace Updater
