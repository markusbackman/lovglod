#include "shl.h"
#include "config.h"
#include "trace.h"
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
constexpr const char *CLUB_BASE = "https://" SHL_CLUB_HOST;
constexpr const char *LIVE_BASE = "https://" SHL_LIVE_HOST;

// ── GET mot SHL ─────────────────────────────────────────────────────────────
bool httpGet(const char *base, const String &path, String &body, size_t maxBytes = 24000) {
    if (WiFi.status() != WL_CONNECTED) { gLastError = "WiFi nere"; return false; }

    WiFiClientSecure secure;
    configureTls(secure);

    HTTPClient http;
    http.setTimeout(12000);
    http.setConnectTimeout(8000);
    http.setReuse(false);

    const String url = String(base) + path;
    if (!http.begin(secure, url)) { gLastError = "http.begin misslyckades"; return false; }

    // shl.se svarar med 403 på tomma/okända user agents.
    http.addHeader("User-Agent", "Mozilla/5.0 (compatible; LovGlod/" FW_VERSION ")");
    http.addHeader("Accept", "application/json");

    const uint32_t t0 = millis();
    const int code = http.GET();
    TRACE("[api] GET %s → %d på %lu ms\n", path.c_str(), code, millis() - t0);
    if (code != HTTP_CODE_OK) {
        gLastError = "HTTP " + String(code) + " " + path;
        http.end();
        return false;
    }

    body = http.getString();
    http.end();
    TRACE("[api]   %u byte, totalt %lu ms\n", body.length(), millis() - t0);

    if (body.length() > maxBytes) body.remove(maxBytes);
    return true;
}

bool apiGet(const String &path, String &body, size_t maxBytes = 24000) {
    return httpGet(API_BASE, path, body, maxBytes);
}

// Klubbsajten skriver ut lagkoden med sajtens eget påhäng: "IFB Herr".
String plainCode(const char *code) {
    String c = code ? code : "";
    const int space = c.indexOf(' ');
    return space > 0 ? c.substring(0, space) : c;
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
//
// Servern svarar HTTP/1.1 med Transfer-Encoding: chunked. Chunk-ramarna måste
// packas upp innan SSE-raderna tolkas: skickar servern en händelse i flera
// skrivningar hamnar storleksraden annars mitt i JSON:en och ramen går förlorad.
WiFiClientSecure gSse;
bool     gSseActive   = false;
bool     gSseHeaders  = false;    // true när HTTP-headers är avklarade
bool     gSseStatusOk = false;    // statusraden var 200
bool     gSseChunked  = false;
uint16_t gSseHeaderLines = 0;
String   gSseGameUuid;
time_t   gSseGameStart = 0;
uint32_t gSseConnectedAt = 0;
bool     gSseGood      = false;   // servern har skickat något annat än "unknown"
uint8_t  gSseLagStrikes = 0;      // händelser i följd som kommit för sent
time_t   gSseNewestUpd = 0;       // nyaste updatedTime på den här anslutningen
String   gSseLine;
String   gSseData;
String   gSseEvent;
uint32_t gSseLastRx   = 0;
uint32_t gSseRetryAt  = 0;

enum class ChunkState : uint8_t { Size, Data, Trailer };
ChunkState gChunkState = ChunkState::Size;
String     gChunkHdr;
uint32_t   gChunkLeft  = 0;

uint32_t gSseFrames     = 0;
uint32_t gSseReconnects = 0;
uint32_t gSseComments   = 0;

constexpr size_t SSE_MAX = 8192;

bool sseConnect() {
    gSse.stop();
    configureTls(gSse);

    const uint32_t t0 = millis();
    if (!gSse.connect(SHL_LIVE_HOST, 443)) {
        gLastError = "SSE: kunde inte ansluta till " SHL_LIVE_HOST ":443";
        TRACE("[sse] anslutning MISSLYCKADES efter %lu ms\n", millis() - t0);
        gSseRetryAt = millis() + 15000;
        return false;
    }
    TRACE("[sse] ansluten på %lu ms, uuid=%s\n", millis() - t0, gSseGameUuid.c_str());

    String req = "GET /live/game";
    if (gSseGameUuid.length()) req += "?gameUuid=" + gSseGameUuid;
    req += " HTTP/1.1\r\n";
    req += "Host: " SHL_LIVE_HOST "\r\n";
    req += "Accept: text/event-stream\r\n"
           "Cache-Control: no-cache\r\n"
           "User-Agent: LovGlod/" FW_VERSION "\r\n"
           "Connection: keep-alive\r\n\r\n";
    gSse.print(req);

    gSseHeaders  = false;
    gSseStatusOk = false;
    gSseChunked  = false;
    gSseHeaderLines = 0;
    gChunkState  = ChunkState::Size;
    gChunkHdr    = "";
    gChunkLeft   = 0;
    gSseLine     = "";
    gSseData     = "";
    gSseEvent    = "";
    gSseLastRx   = millis();
    gSseConnectedAt = millis();
    gSseGood     = false;
    gSseLagStrikes = 0;
    gSseNewestUpd  = 0;
    return true;
}

// En header-rad. Första raden är statusraden.
void sseHeaderLine(const String &line, bool first) {
    TRACE("[sse] < %s\n", line.c_str());
    if (first) {
        gSseStatusOk = line.startsWith("HTTP/1.1 200") || line.startsWith("HTTP/1.0 200");
        if (!gSseStatusOk) gLastError = "SSE: " + line;
        return;
    }
    String l = line;
    l.toLowerCase();
    if (l.startsWith("transfer-encoding:") && l.indexOf("chunked") >= 0) gSseChunked = true;
}

// Tolkar en komplett SSE-händelse. Sant om den bar en ställning.
bool sseDispatch(LiveScore &out) {
    gSseFrames++;
    gLastRaw = gSseData.substring(0, 900);
    if (gSseData.indexOf("\"liveState\":\"unknown\"") < 0) gSseGood = true;

    TRACE("[sse] ram #%lu event=%s len=%u: %.200s%s\n", (unsigned long)gSseFrames,
          gSseEvent.length() ? gSseEvent.c_str() : "-", gSseData.length(), gSseData.c_str(),
          gSseData.length() > 200 ? " …" : "");

    bool got = false;
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, gSseData);
    if (err) {
        TRACE("[sse] JSON-FEL: %s\n", err.c_str());
    } else {
        // Eftersläpning: updatedTime (UTC) på nya händelser mot klockan. En
        // frisk server ligger på ~20 s; de som hängt sig glider iväg i minuter
        // och skickar samma händelser om och om igen.
        //
        // Bara händelser som är nyare än allt servern skickat förut räknas.
        // Även friska servrar skickar om gamla händelser — målvaktsbytet från
        // före nedsläpp kom tillbaka 52 minuter senare — och de säger inget om
        // hur långt efter servern ligger. Den första händelsen på anslutningen
        // sätter bara referensen, av samma skäl.
        const char *upd = doc["liveEvent"]["updatedTime"] | (const char *)nullptr;
        const time_t now = time(nullptr);
        const time_t ut  = upd ? parseIso8601Utc(upd) : 0;
        if (ut && now > 1700000000 && ut > gSseNewestUpd) {
            const bool first = gSseNewestUpd == 0;
            gSseNewestUpd = ut;
            const long lag = (long)(now - ut);
            if (!first) {
                if (lag > 90) gSseLagStrikes++; else gSseLagStrikes = 0;
            }
            TRACE("[sse] ny händelse, eftersläpning %ld s%s\n", lag,
                  first ? " (referens)" : lag > 90 ? " — FÖR SENT" : "");
        }

        int h = -1, a = -1;
        if (findScorePair(doc.as<JsonVariantConst>(), h, a)) {
            out.valid = true; out.home = h; out.away = a;
            got = true;
            TRACE("[sse] ställning i ramen: %d–%d\n", h, a);
        } else {
            TRACE("[sse] ingen ställning i ramen\n");
        }
    }
    gSseData  = "";
    gSseEvent = "";
    return got;
}

// Ett uppackat byte ur kroppen. Sant när en ställning lästs in.
bool sseBodyByte(char c, LiveScore &out) {
    if (c == '\r') return false;
    if (c != '\n') {
        if (gSseLine.length() < SSE_MAX) gSseLine += c;
        else if (gSseLine.length() == SSE_MAX) { TRACE("[sse] rad KAPAD vid %u byte\n", SSE_MAX); gSseLine += c; }
        return false;
    }

    bool got = false;
    if (gSseLine.length() == 0) {
        // Tom rad = händelsen är komplett
        if (gSseData.length()) got = sseDispatch(out);
    } else if (gSseLine.startsWith("data:")) {
        String chunk = gSseLine.substring(5);
        if (chunk.startsWith(" ")) chunk.remove(0, 1);
        if (gSseData.length() + chunk.length() <= SSE_MAX) gSseData += chunk;
        else TRACE("[sse] data KAPAD, ram över %u byte\n", SSE_MAX);
    } else if (gSseLine.startsWith("event:")) {
        gSseEvent = gSseLine.substring(6);
        gSseEvent.trim();
    } else if (gSseLine[0] == ':') {
        gSseComments++;
        TRACE("[sse] kommentar/heartbeat: %.60s\n", gSseLine.c_str());
    } else if (!gSseLine.startsWith("id:") && !gSseLine.startsWith("retry:")) {
        TRACE("[sse] okänd rad: %.120s\n", gSseLine.c_str());
    }
    gSseLine = "";
    return got;
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

// Matchen som pågår just nu, hämtad från klubbsajten. Används när lampan
// startar mitt i en match: då har den ingenting sparat, och SHL:s egna listor
// har tappat matchen — upcoming-games släpper den vid nedsläpp och
// played-games tar upp den först när den är slut.
bool fetchOngoingGame(NextGame &out) {
    const time_t now = time(nullptr);
    if (now < 1700000000) { gLastError = "Klockan inte synkad"; return false; }

    String body;
    if (!httpGet(CLUB_BASE, "/api/gameday/gameheader", body, 16000)) return false;

    // Svaret är ett objekt med ett datum per nyckel och en matchlista i varje.
    JsonDocument filter;
    JsonObject g = filter["*"][0].to<JsonObject>();
    g["uuid"] = true;
    g["startDateTime"] = true;
    g["seriesCode"] = true;
    g["played"] = true;
    g["homeTeam"]["code"] = true;
    g["awayTeam"]["code"] = true;

    JsonDocument doc;
    const DeserializationError err =
        deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err) { gLastError = String("JSON gameheader: ") + err.c_str(); return false; }

    for (JsonPairConst day : doc.as<JsonObjectConst>()) {
        for (JsonVariantConst gv : day.value().as<JsonArrayConst>()) {
            // Bara A-laget. Listan har U20 och damlaget också.
            if (strcmp(gv["seriesCode"] | "", "SHL") != 0) continue;
            if (gv["played"] | false) continue;

            const time_t start = parseIso8601Utc(gv["startDateTime"]);
            if (!start) continue;
            if (now < start - (time_t)(LIVE_WINDOW_PRE_MS / 1000)) continue;
            if (now > start + (time_t)(LIVE_WINDOW_POST_MS / 1000)) continue;

            out = NextGame();
            out.valid    = true;
            out.uuid     = gv["uuid"].as<const char *>();
            out.homeCode = plainCode(gv["homeTeam"]["code"]);
            out.awayCode = plainCode(gv["awayTeam"]["code"]);
            out.startUtc = start;
            out.homeIsUs = out.homeCode == SHL_TEAM_CODE;
            TRACE("[shl] pågående match %s (%s – %s)\n", out.uuid.c_str(),
                  out.homeCode.c_str(), out.awayCode.c_str());
            return true;
        }
    }

    gLastError = "Ingen match i matchfönstret";
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

    // Bara till sammanfattningen: "igår" enligt lokal kalender i Stockholm,
    // inte 24 h bakåt. Gnistorna räknas inte på datum längre — de lyser från
    // vinsten fram till nästa match, se winStillGlows() i main.cpp.
    const time_t now = time(nullptr);
    const long   yesterdayKey = localDateKey(now - 86400L);
    const bool   wasYesterday = localDateKey(start) == yesterdayKey;

    out.valid        = true;
    out.won          = won;
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
    if (body.length() < 5) {                              // tom kropp utanför matchdag
        TRACE("[poll] today-games tom (%u byte)\n", body.length());
        return false;
    }

    JsonDocument doc;
    if (const DeserializationError err = deserializeJson(doc, body)) {
        gLastError = "JSON today-games";
        TRACE("[poll] JSON-FEL: %s (%u byte, kapad?)\n", err.c_str(), body.length());
        return false;
    }

    // Hitta vår match i trädet och läs ut ställningen därifrån.
    JsonVariantConst root = doc.as<JsonVariantConst>();
    JsonArrayConst   arr  = root.is<JsonArrayConst>() ? root.as<JsonArrayConst>()
                                                      : root["games"].as<JsonArrayConst>();
    if (arr.isNull()) {
        TRACE("[poll] hittar ingen matchlista i today-games: %.200s\n", body.c_str());
        return false;
    }

    for (JsonVariantConst g : arr) {
        const char *u = g["uuid"] | "";
        if (gameUuid != u) continue;
        int h = -1, a = -1;
        if (findScorePair(g, h, a)) {
            out.valid = true; out.home = h; out.away = a;
            TRACE("[poll] ställning %d–%d\n", h, a);
            return true;
        }
        TRACE("[poll] matchen finns men ingen ställning hittad\n");
        return false;
    }
    TRACE("[poll] %s saknas bland %u matcher\n", gameUuid.c_str(), arr.size());
    return false;
}

void sseStart(const String &gameUuid, time_t startUtc) {
    if (gSseActive && gSseGameUuid == gameUuid) return;
    gSseGameUuid = gameUuid;
    gSseGameStart = startUtc;
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

    // Byt server. Bakom game-broadcaster.s8y.se står flera, och alla är inte
    // friska: ungefär var tredje anslutning har hamnat på en som bara svarar
    // "unknown" hela matchen, och andra har legat minuter efter. Lampan ser
    // ansluten ut i båda fallen men missar målen.
    if (gSse.connected() && gSseHeaders) {
        const time_t now = time(nullptr);
        const bool   started = gSseGameStart && now > 1700000000 && now > gSseGameStart;
        const char  *why = nullptr;
        if (started && !gSseGood && millis() - gSseConnectedAt > 45000)
            why = "bara unknown efter nedsläpp";
        else if (gSseLagStrikes >= 2)
            why = "servern släpar";
        if (why) {
            TRACE("[sse] BYTER SERVER: %s\n", why);
            gSseReconnects++;
            gSseRetryAt = millis() + 15000;
            sseConnect();
            return false;
        }
    }

    // Återanslut vid tappad ström eller 180 s tystnad. SHL:s flöde har stått
    // helt still i 100 s mitt i en period, och varje onödig återanslutning är
    // ett nytt lotteri om vilken server man hamnar på.
    if (!gSse.connected() || (millis() - gSseLastRx > 180000)) {
        if ((int32_t)(millis() - gSseRetryAt) < 0) return false;
        TRACE("[sse] ÅTERANSLUTER (%s, tyst i %lu s, %lu ramar hittills)\n",
              gSse.connected() ? "tystnad" : "tappad", (millis() - gSseLastRx) / 1000,
              (unsigned long)gSseFrames);
        gSseReconnects++;
        gSseRetryAt = millis() + 15000;
        if (!sseConnect()) return false;
    }

    bool got = false;

    // Läs allt som ligger i bufferten, men släpp loopen efter en rimlig mängd
    // så att LED-renderingen inte hackar.
    for (uint16_t guard = 0; guard < 4096 && gSse.available(); guard++) {
        const char c = (char)gSse.read();
        gSseLastRx = millis();

        if (!gSseHeaders) {
            if (c == '\r') continue;
            if (c != '\n') { if (gSseLine.length() < 512) gSseLine += c; continue; }
            if (gSseLine.length() == 0) {           // tom rad = slut på headers
                gSseHeaders = true;
                TRACE("[sse] headers klara: status %s, %s\n", gSseStatusOk ? "200" : "FEL",
                      gSseChunked ? "chunked" : "ej chunked");
                if (!gSseStatusOk) { gSse.stop(); gSseRetryAt = millis() + 15000; return false; }
            } else {
                sseHeaderLine(gSseLine, gSseHeaderLines++ == 0);
            }
            gSseLine = "";
            continue;
        }

        if (!gSseChunked) {
            got = sseBodyByte(c, out);
        } else switch (gChunkState) {
            case ChunkState::Size:
                if (c == '\n') {
                    gChunkLeft = strtoul(gChunkHdr.c_str(), nullptr, 16);   // stannar vid \r eller ;
                    gChunkHdr  = "";
                    if (gChunkLeft == 0) {
                        TRACE("[sse] servern avslutade strömmen (0-chunk)\n");
                        gSse.stop();
                        return got;
                    }
                    gChunkState = ChunkState::Data;
                } else if (gChunkHdr.length() < 16) {
                    gChunkHdr += c;
                }
                break;
            case ChunkState::Data:
                got = sseBodyByte(c, out);
                if (--gChunkLeft == 0) gChunkState = ChunkState::Trailer;
                break;
            case ChunkState::Trailer:                  // \r\n efter varje chunk
                if (c == '\n') gChunkState = ChunkState::Size;
                break;
        }
        if (got) break;
    }

    return got;
}

uint32_t sseFrames()     { return gSseFrames; }
uint32_t sseReconnects() { return gSseReconnects; }
uint32_t sseComments()   { return gSseComments; }
uint32_t sseSilentMs()   { return gSseActive ? millis() - gSseLastRx : 0; }

String apiBaseUrl()  { return API_BASE; }
String liveBaseUrl() { return LIVE_BASE; }

const String &lastRawFrame() { return gLastRaw; }
const String &lastError()    { return gLastError; }

}  // namespace Shl
