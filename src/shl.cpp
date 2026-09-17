#include "shl.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

namespace {

String gLastRaw;
String gLastError;

// Nätet bakom shl.se ligger på Cloudflare och roterar certifikatkedja.
// Vi kör därför utan certifikatvalidering — enheten läser bara publik
// matchdata och skickar aldrig något känsligt. Vill man ha validering:
// hämta rot-CA:t och byt setInsecure() mot setCACert().
void configureTls(WiFiClientSecure &c) {
    c.setInsecure();
    c.setTimeout(12);
}

// ── Datakälla ───────────────────────────────────────────────────────────────
// Alltid riktiga SHL över HTTPS. Under labbtest matas matchläget in utifrån
// med POST /push mot lampans egen webbserver — se PUSH_LEASE_MS i config.h —
// så det finns ingen andra datakälla att peka om hämtningen mot.
constexpr const char *API_BASE  = "https://" SHL_API_HOST;
constexpr const char *LIVE_BASE = "https://" SHL_LIVE_HOST;

// ── GET mot SHL ─────────────────────────────────────────────────────────────
bool apiGet(const String &path, String &body, size_t maxBytes = 24000) {
    if (WiFi.status() != WL_CONNECTED) { gLastError = "WiFi nere"; return false; }

    WiFiClientSecure secure;
    configureTls(secure);

    HTTPClient http;
    http.setTimeout(12000);
    http.setConnectTimeout(8000);
    http.setReuse(false);

    const String url = String(API_BASE) + path;
    if (!http.begin(secure, url)) { gLastError = "http.begin misslyckades"; return false; }

    // shl.se svarar med 403 på tomma/okända user agents.
    http.addHeader("User-Agent", "Mozilla/5.0 (compatible; BjorklovenLED/" FW_VERSION ")");
    http.addHeader("Accept", "application/json");

    const int code = http.GET();
    if (code != HTTP_CODE_OK) {
        gLastError = "HTTP " + String(code) + " " + path;
        http.end();
        return false;
    }

    body = http.getString();
    http.end();

    if (body.length() > maxBytes) body.remove(maxBytes);
    return true;
}

// ── Datum/tid ───────────────────────────────────────────────────────────────
// Howard Hinnants days_from_civil — undviker att vi behöver timegm/mktime i UTC.
long daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int      era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (long)era * 146097 + (long)doe - 719468;
}

// "2026-09-19T16:00:00.000Z" → UTC epoch. 0 vid parsfel.
time_t parseIso8601Utc(const char *s) {
    if (!s) return 0;
    int Y, M, D, h, mi, se = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &mi, &se) < 5) return 0;
    return (time_t)(daysFromCivil(Y, M, D) * 86400L + h * 3600L + mi * 60L + se);
}

// Lokalt (Stockholm) datum för en UTC-tidpunkt, som YYYYMMDD.
long localDateKey(time_t utc) {
    struct tm lt;
    localtime_r(&utc, &lt);
    return (lt.tm_year + 1900) * 10000L + (lt.tm_mon + 1) * 100L + lt.tm_mday;
}

// ── Tolerant poängutvinning ─────────────────────────────────────────────────
// Live-ramens exakta fältnamn är inte dokumenterade och kunde inte observeras
// utanför säsong. Vi letar därför brett efter kända varianter i hela trädet.
bool readScoreField(JsonVariantConst v, int &out) {
    if (v.is<int>())   { out = v.as<int>(); return true; }
    if (v.is<const char *>()) {
        const char *s = v.as<const char *>();
        if (s && isdigit((unsigned char)s[0])) { out = atoi(s); return true; }
    }
    return false;
}

bool findScorePair(JsonVariantConst v, int &home, int &away, uint8_t depth = 0) {
    if (depth > 8) return false;

    if (v.is<JsonObjectConst>()) {
        JsonObjectConst o = v.as<JsonObjectConst>();

        // Variant A: platta fält på samma objekt
        static const char *hKeys[] = {"homeScore", "homeGoals", "homeTeamScore", "homeResult"};
        static const char *aKeys[] = {"awayScore", "awayGoals", "awayTeamScore", "awayResult"};
        for (uint8_t i = 0; i < 4; i++) {
            int h, a;
            if (o[hKeys[i]].is<JsonVariantConst>() && o[aKeys[i]].is<JsonVariantConst>() &&
                readScoreField(o[hKeys[i]], h) && readScoreField(o[aKeys[i]], a)) {
                home = h; away = a; return true;
            }
        }

        // Variant B: nästlade lagobjekt
        static const char *hObj[] = {"homeTeam", "homeTeamInfo", "home"};
        static const char *aObj[] = {"awayTeam", "awayTeamInfo", "away"};
        static const char *sKeys[] = {"score", "result", "goals"};
        for (uint8_t i = 0; i < 3; i++) {
            JsonVariantConst ho = o[hObj[i]], ao = o[aObj[i]];
            if (ho.isNull() || ao.isNull()) continue;
            for (uint8_t k = 0; k < 3; k++) {
                int h, a;
                if (readScoreField(ho[sKeys[k]], h) && readScoreField(ao[sKeys[k]], a)) {
                    home = h; away = a; return true;
                }
            }
        }

        for (JsonPairConst kv : o)
            if (findScorePair(kv.value(), home, away, depth + 1)) return true;

    } else if (v.is<JsonArrayConst>()) {
        for (JsonVariantConst e : v.as<JsonArrayConst>())
            if (findScorePair(e, home, away, depth + 1)) return true;
    }
    return false;
}

// ── SSE-klient ──────────────────────────────────────────────────────────────
// Live-strömmen går över TLS mot game-broadcaster.s8y.se.
WiFiClientSecure gSse;
bool     gSseActive   = false;
bool     gSseHeaders  = false;    // true när HTTP-headers är avklarade
String   gSseGameUuid;
String   gSseLine;
String   gSseData;
uint32_t gSseLastRx   = 0;
uint32_t gSseRetryAt  = 0;

bool sseConnect() {
    gSse.stop();
    configureTls(gSse);

    if (!gSse.connect(SHL_LIVE_HOST, 443)) {
        gLastError = "SSE: kunde inte ansluta till " SHL_LIVE_HOST ":443";
        gSseRetryAt = millis() + 15000;
        return false;
    }

    String req = "GET /live/game";
    if (gSseGameUuid.length()) req += "?gameUuid=" + gSseGameUuid;
    req += " HTTP/1.1\r\n";
    req += "Host: " SHL_LIVE_HOST "\r\n";
    req += "Accept: text/event-stream\r\n"
           "Cache-Control: no-cache\r\n"
           "User-Agent: BjorklovenLED/" FW_VERSION "\r\n"
           "Connection: keep-alive\r\n\r\n";
    gSse.print(req);

    gSseHeaders = false;
    gSseLine    = "";
    gSseData    = "";
    gSseLastRx  = millis();
    return true;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
namespace Shl {

bool fetchNextGame(NextGame &out) {
    String body;
    const String path = "/api/sports-v2/upcoming-games/" SHL_TEAM_UUID "?gamePlace=";
    if (!apiGet(path, body)) return false;

    // Filtret håller nere minnesåtgången — vi behöver bara ett fåtal fält.
    JsonDocument filter;
    JsonObject g = filter["upcomingGames"].add<JsonObject>();
    g["uuid"] = true;
    g["startDateTime"] = true;
    g["state"] = true;
    g["homeTeamInfo"]["code"] = true;
    g["awayTeamInfo"]["code"] = true;

    JsonDocument doc;
    const DeserializationError err =
        deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err) { gLastError = String("JSON upcoming: ") + err.c_str(); return false; }

    JsonArrayConst games = doc["upcomingGames"].as<JsonArrayConst>();
    if (games.isNull() || games.size() == 0) { gLastError = "Inga kommande matcher"; return false; }

    // Listan kommer sorterad, men vi tar första matchen som inte redan är över.
    const time_t now = time(nullptr);
    for (JsonVariantConst gv : games) {
        const time_t start = parseIso8601Utc(gv["startDateTime"]);
        if (!start) continue;
        if (now > start + (time_t)(LIVE_WINDOW_POST_MS / 1000)) continue;

        out.valid    = true;
        out.uuid     = gv["uuid"].as<const char *>();
        out.homeCode = gv["homeTeamInfo"]["code"].as<const char *>();
        out.awayCode = gv["awayTeamInfo"]["code"].as<const char *>();
        out.startUtc = start;
        out.homeIsUs = out.homeCode == SHL_TEAM_CODE;
        return true;
    }

    gLastError = "Alla kommande matcher passerade";
    return false;
}

bool fetchLastResult(LastResult &out) {
    out = LastResult();

    String body;
    if (!apiGet("/api/sports-v2/played-games/" SHL_TEAM_UUID, body)) return false;

    JsonDocument filter;
    JsonObject g = filter["playedGames"].add<JsonObject>();
    g["startDateTime"] = true;
    g["state"] = true;
    for (const char *side : {"homeTeamInfo", "awayTeamInfo"}) {
        g[side]["code"]   = true;
        g[side]["score"]  = true;
        g[side]["status"] = true;
    }

    JsonDocument doc;
    const DeserializationError err =
        deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err) { gLastError = String("JSON played: ") + err.c_str(); return false; }

    JsonArrayConst games = doc["playedGames"].as<JsonArrayConst>();
    if (games.isNull() || games.size() == 0) {
        out.valid   = true;
        out.summary = "Inga spelade matcher";
        return true;
    }

    JsonVariantConst last = games[0];
    const time_t start = parseIso8601Utc(last["startDateTime"]);
    if (!start) { gLastError = "Kunde inte tolka matchdatum"; return false; }

    JsonVariantConst home = last["homeTeamInfo"];
    JsonVariantConst away = last["awayTeamInfo"];
    const bool weAreHome  = String(home["code"].as<const char *>()) == SHL_TEAM_CODE;
    JsonVariantConst us   = weAreHome ? home : away;

    const char *status = us["status"] | "";
    const bool  won    = strcasecmp(status, "WIN") == 0;

    // "Igår" enligt lokal kalender i Stockholm, inte 24 h bakåt.
    const time_t now = time(nullptr);
    const long   yesterdayKey = localDateKey(now - 86400L);
    const bool   wasYesterday = localDateKey(start) == yesterdayKey;

    out.valid        = true;
    out.won          = won;
    out.wonYesterday = won && wasYesterday;
    out.startUtc     = start;

    out.summary = String(home["code"].as<const char *>()) + " " +
                  String(home["score"].as<int>()) + "-" + String(away["score"].as<int>()) + " " +
                  String(away["code"].as<const char *>()) + "  (" + (won ? "vinst" : "förlust") +
                  (wasYesterday ? ", igår" : "") + ")";
    return true;
}

bool pollLiveScore(const String &gameUuid, LiveScore &out) {
    String body;
    if (!apiGet("/api/sports-v2/today-games", body)) return false;
    if (body.length() < 5) return false;                  // tom kropp utanför matchdag

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
        gLastError = "JSON today-games";
        return false;
    }

    // Hitta vår match i trädet och läs ut ställningen därifrån.
    JsonVariantConst root = doc.as<JsonVariantConst>();
    JsonArrayConst   arr  = root.is<JsonArrayConst>() ? root.as<JsonArrayConst>()
                                                      : root["games"].as<JsonArrayConst>();
    if (arr.isNull()) return false;

    for (JsonVariantConst g : arr) {
        const char *u = g["uuid"] | "";
        if (gameUuid != u) continue;
        int h = -1, a = -1;
        if (findScorePair(g, h, a)) {
            out.valid = true; out.home = h; out.away = a;
            return true;
        }
    }
    return false;
}

void sseStart(const String &gameUuid) {
    if (gSseActive && gSseGameUuid == gameUuid) return;
    gSseGameUuid = gameUuid;
    gSseActive   = true;
    // Inte 0: en nollad tidsstämpel ser ut att ligga i framtiden efter 24,9
    // dygns upptid, och då återansluter strömmen aldrig.
    gSseRetryAt  = millis();
    sseConnect();
}

void sseStop() {
    gSseActive = false;
    gSse.stop();
    gSseGameUuid = "";
}

bool sseConnected() { return gSseActive && gSse.connected(); }

bool ssePump(LiveScore &out) {
    if (!gSseActive) return false;

    // Återanslut vid tappad ström eller 90 s tystnad (servern skickar heartbeats).
    if (!gSse.connected() || (millis() - gSseLastRx > 90000)) {
        if ((int32_t)(millis() - gSseRetryAt) < 0) return false;
        gSseRetryAt = millis() + 15000;
        if (!sseConnect()) return false;
    }

    bool got = false;

    // Läs allt som ligger i bufferten, men släpp loopen efter en rimlig mängd
    // så att LED-renderingen inte hackar.
    for (uint16_t guard = 0; guard < 4096 && gSse.available(); guard++) {
        const char c = (char)gSse.read();
        gSseLastRx = millis();

        if (c == '\r') continue;
        if (c != '\n') {
            if (gSseLine.length() < 4096) gSseLine += c;
            continue;
        }

        // Komplett rad
        if (!gSseHeaders) {
            if (gSseLine.length() == 0) gSseHeaders = true;   // tom rad = slut på headers
            gSseLine = "";
            continue;
        }

        if (gSseLine.length() == 0) {
            // Tom rad = händelsen är komplett
            if (gSseData.length()) {
                gLastRaw = gSseData.substring(0, 900);

                JsonDocument doc;
                if (!deserializeJson(doc, gSseData)) {
                    int h = -1, a = -1;
                    if (findScorePair(doc.as<JsonVariantConst>(), h, a)) {
                        out.valid = true; out.home = h; out.away = a;
                        got = true;
                    }
                }
                gSseData = "";
            }
        } else if (gSseLine.startsWith("data:")) {
            String chunk = gSseLine.substring(5);
            if (chunk.startsWith(" ")) chunk.remove(0, 1);
            if (gSseData.length() < 8192) gSseData += chunk;
        }
        // ":"-rader är kommentarer/heartbeats, "event:"/"id:" ignoreras.

        gSseLine = "";
        if (got) break;
    }

    return got;
}

String apiBaseUrl()  { return API_BASE; }
String liveBaseUrl() { return LIVE_BASE; }

const String &lastRawFrame() { return gLastRaw; }
const String &lastError()    { return gLastError; }

}  // namespace Shl
