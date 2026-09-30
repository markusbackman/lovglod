#include "telemetry.h"
#include "config.h"
#include "settings.h"
#include "leds.h"
#include "shl.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_attr.h>
#include <time.h>

namespace {

struct Event {
    uint32_t t;          // UTC epoch, 0 = klockan inte synkad när det hände
    uint32_t up;         // sekunder sedan start — går alltid att lita på
    String   type;
    String   detail;
};

Event    gQueue[TELEMETRY_QUEUE_MAX];
uint8_t  gCount     = 0;
uint32_t gDropped   = 0;              // händelser som föll ur en full kö
bool     gStarted   = false;
uint32_t gNextSend  = 0;
uint32_t gInterval  = TELEMETRY_INTERVAL_MS;
bool     gServerOff = false;          // Worker:n bad oss vila till nästa omstart
String   gStatus    = "Ingen rapport skickad än";
bool     gForce     = false;          // "Skicka nu" på /debug
bool     gCrashOff  = false;          // förra försöket kraschade lampan
String   gLastCrash;                  // sparad i NVS, visas på /debug
String   gLastTrace;                  // stegen i senaste försöket, med tider

// ── Brödsmulor genom en krasch ──────────────────────────────────────────────
// En krasch mitt i rapporten syns annars bara som "panic", och en lampa hos
// någon annan har ingen seriell kabel att läsa backtracen från. RTC-minnet överlever en panic-omstart men inte ett
// strömavbrott, så där skriver send() vilket steg den är i innan varje steg
// som kan krascha. Hittar nästa start en smula vet vi var den dog.
enum Stage : uint8_t { ST_NONE, ST_DNS, ST_TCP, ST_TLS, ST_POST, ST_REPLY };

const char *stageName(uint8_t s) {
    switch (s) {
        case ST_DNS:   return "dns";
        case ST_TCP:   return "tcp";
        case ST_TLS:   return "tls";
        case ST_POST:  return "post";
        case ST_REPLY: return "svar";
        default:       return "okänt";
    }
}

struct Crumb {
    uint32_t magic;
    uint8_t  stage;
    uint32_t up;          // sekunder sedan start
    uint32_t heap;        // fritt heap när steget började
    uint32_t stack;       // loop-taskens minsta lediga stack hittills, byte (inte LED-taskens)
    uint32_t crashes;     // kraschar i send() sedan strömpåslag
};

constexpr uint32_t CRUMB_MAGIC = 0x7E1E5E4D;
RTC_NOINIT_ATTR Crumb rtcCrumb;

void crumb(Stage s) {
    rtcCrumb.stage = s;
    rtcCrumb.up    = millis() / 1000;
    rtcCrumb.heap  = ESP.getFreeHeap();
    rtcCrumb.stack = uxTaskGetStackHighWaterMark(nullptr);
    rtcCrumb.magic = s == ST_NONE ? 0 : CRUMB_MAGIC;
}

uint32_t nowUtc() {
    const time_t t = time(nullptr);
    return t > 1600000000 ? (uint32_t)t : 0;   // osynkad klocka står nära 1970
}

// Korta koder i stället för statussidans svenska text, så att servern kan
// gruppera på dem utan att tolka fritext.
const char *resetCode() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_SW:        return "sw";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_EXT:       return "ext";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        default:                return "unknown";
    }
}

void build(JsonDocument &doc) {
    doc["v"]        = 1;                           // schemaversion
    doc["id"]       = settings.lampId;
    if (settings.lampName.length()) doc["name"] = settings.lampName;
    doc["fw"]       = FW_VERSION;
    doc["beta"]     = settings.otaBeta;
    doc["t"]        = nowUtc();
    doc["up"]       = millis() / 1000;
    doc["boots"]    = settings.bootCount;
    doc["badBoots"] = settings.abnormalBoots;
    doc["reset"]    = resetCode();
    doc["heap"]     = ESP.getFreeHeap();
    doc["minHeap"]  = ESP.getMinFreeHeap();
    doc["ledStack"] = Leds::stackFree();
    doc["rssi"]     = WiFi.RSSI();
    doc["strip"]    = Leds::stripName(settings.ledStrip);
    doc["leds"]     = settings.ledCount;
    doc["bright"]   = settings.brightness;
    doc["mode"]     = (uint8_t)settings.lampMode;
    doc["goalDelay"]= settings.goalDelayS;
    // Räknare sedan start, inte sedan förra rapporten: då tål de att en
    // rapport tappas, och servern räknar ut skillnaden själv.
    doc["shlErr"]   = Shl::httpErrors();
    doc["sseRe"]    = Shl::sseReconnects();
    if (gDropped) doc["dropped"] = gDropped;

    JsonArray ev = doc["events"].to<JsonArray>();
    for (uint8_t i = 0; i < gCount; i++) {
        JsonObject e = ev.add<JsonObject>();
        e["type"] = gQueue[i].type;
        e["t"]    = gQueue[i].t;
        e["up"]   = gQueue[i].up;
        if (gQueue[i].detail.length()) e["detail"] = gQueue[i].detail;
    }
}

// Worker:n kan svara {"interval": sekunder, "enabled": false}. Båda gäller bara
// till nästa omstart — det är en ventil, inte en inställning.
void applyReply(const String &body) {
    JsonDocument doc;
    if (deserializeJson(doc, body)) return;
    if (doc["enabled"].is<bool>() && !doc["enabled"].as<bool>()) gServerOff = true;
    if (doc["interval"].is<uint32_t>()) {
        const uint32_t s = constrain(doc["interval"].as<uint32_t>(),
                                     TELEMETRY_MIN_S, TELEMETRY_MAX_S);
        gInterval = s * 1000UL;
    }
}

// Stegvis förkontroll innan HTTPClient tar över: namnuppslag, ren TCP och
// ett TLS-handslag var för sig. Kraschar lampan säger smulorna vilket av
// stegen som tog den med sig.
bool preflight(uint32_t &t0) {
    auto lap = [&](const char *name) {
        gLastTrace += String(name) + " " + String(millis() - t0) + " ms, ";
        t0 = millis();
    };

    crumb(ST_DNS);
    IPAddress ip;
    if (!WiFi.hostByName(TELEMETRY_HOST, ip)) {
        gLastTrace += "dns misslyckades";
        gStatus = "Misslyckades (namnuppslag)";
        return false;
    }
    lap("dns");

    crumb(ST_TCP);
    {
        WiFiClient tcp;
        const bool ok = tcp.connect(ip, 443, 6000) == 1;
        tcp.stop();
        if (!ok) {
            gLastTrace += "tcp misslyckades";
            gStatus = "Misslyckades (TCP till " + ip.toString() + ")";
            return false;
        }
    }
    lap("tcp");

    crumb(ST_TLS);
    {
        WiFiClientSecure tls;
        tls.setInsecure();
        tls.setTimeout(10);
        const bool ok = tls.connect(TELEMETRY_HOST, 443) == 1;
        char err[80] = "";
        if (!ok) tls.lastError(err, sizeof(err));
        tls.stop();
        if (!ok) {
            gLastTrace += String("tls misslyckades: ") + err;
            gStatus = String("Misslyckades (TLS: ") + err + ")";
            return false;
        }
    }
    lap("tls");
    return true;
}

bool send() {
    JsonDocument doc;
    build(doc);
    String body;
    serializeJson(doc, body);

    gLastTrace = "";
    uint32_t t0 = millis();
    if (!preflight(t0)) {
        crumb(ST_NONE);
        return false;
    }

    // Samma avvägning som mot SHL: innehållet är inte känsligt, och en
    // hårdkodad rot-CA gör bara lampan tyst den dag Cloudflare byter kedja.
    WiFiClientSecure secure;
    secure.setInsecure();
    secure.setTimeout(10);

    HTTPClient http;
    http.setTimeout(8000);
    http.setConnectTimeout(6000);
    http.setReuse(false);
    if (!http.begin(secure, TELEMETRY_URL)) {
        crumb(ST_NONE);
        gStatus = "Kunde inte starta anropet";
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-LovGlod-Key", TELEMETRY_KEY);
    http.addHeader("User-Agent", "LovGlod/" FW_VERSION);

    crumb(ST_POST);
    const int code = http.POST(body);
    gLastTrace += "post " + String(millis() - t0) + " ms, ";
    t0 = millis();
    crumb(ST_REPLY);
    const String reply = code > 0 ? http.getString() : String();
    http.end();
    crumb(ST_NONE);
    gLastTrace += "svar " + String(millis() - t0) + " ms (" + String(code) + ")";

    if (code != 200 && code != 204) {
        gStatus = "Misslyckades (" + (code > 0 ? "HTTP " + String(code)
                                                 : String(HTTPClient::errorToString(code))) + ")";
        return false;
    }

    applyReply(reply);
    if (gLastCrash.length()) {
        // Kraschen följde med som händelse i den här rapporten.
        Preferences p;
        p.begin("tele", false);
        p.remove("crash");
        p.end();
    }
    gCount   = 0;
    gDropped = 0;
    gStatus  = "Senast skickad efter " + String(millis() / 60000) + " min upptid, " +
               String(body.length()) + " byte";
    return true;
}

}  // namespace

namespace Telemetry {

bool available() { return strlen(TELEMETRY_URL) > 0; }

void checkCrashedInSend() {
    const bool powerOn = esp_reset_reason() == ESP_RST_POWERON;
    if (powerOn || rtcCrumb.magic != CRUMB_MAGIC) {
        // Efter strömpåslag är RTC-minnet skräp. Räknaren börjar om, men den
        // senaste kraschen ligger kvar i NVS tills en rapport gått fram.
        if (powerOn) rtcCrumb.crashes = 0;
    } else {
        rtcCrumb.crashes++;
        gCrashOff  = true;             // tyst till nästa strömpåslag
        gLastCrash = String("kraschade i steget ") + stageName(rtcCrumb.stage) +
                     " efter " + String(rtcCrumb.up) + " s upptid, heap " +
                     String(rtcCrumb.heap) + " B, minsta lediga stack " +
                     String(rtcCrumb.stack) + " B, omstart: " + resetCode() +
                     ", " + String(rtcCrumb.crashes) + " gånger sedan strömpåslag";
        Serial.printf("[tele] förra rapporten %s\n", gLastCrash.c_str());
        event("tele_crash", String(stageName(rtcCrumb.stage)) + " heap=" +
                            String(rtcCrumb.heap) + " stack=" + String(rtcCrumb.stack));
        Preferences p;
        p.begin("tele", false);
        p.putString("crash", gLastCrash);
        p.end();
    }
    rtcCrumb.magic = 0;

    if (gLastCrash.isEmpty()) {
        Preferences p;
        p.begin("tele", true);
        gLastCrash = p.getString("crash", "");
        p.end();
        if (gLastCrash.length()) event("tele_crash_tidigare", gLastCrash);
    }
}

void requestSend() { gForce = true; }

void event(const char *type, const String &detail) {
    if (gCount == TELEMETRY_QUEUE_MAX) {
        for (uint8_t i = 1; i < gCount; i++) gQueue[i - 1] = gQueue[i];
        gCount--;
        gDropped++;
    }
    gQueue[gCount++] = Event{nowUtc(), millis() / 1000, type, detail.substring(0, 80)};
}

void loop(bool inLiveWindow) {
    if (!available() || !settings.telemetry) return;

    // Knappen på /debug går före allt annat: matchfönster, serverns paus och
    // avstängningen efter en krasch. Den som trycker vill se vad som händer.
    if (gForce) {
        gForce     = false;
        gStarted   = true;
        gCrashOff  = false;
        const bool ok = send();
        gNextSend  = millis() + (ok ? gInterval : TELEMETRY_RETRY_MS);
        Serial.printf("[tele] %s — %s\n", gStatus.c_str(), gLastTrace.c_str());
        return;
    }

    if (gServerOff || gCrashOff) return;

    if (!gStarted) {
        gStarted  = true;
        gNextSend = millis() + TELEMETRY_FIRST_MS;
        return;
    }
    if ((int32_t)(millis() - gNextSend) < 0) return;
    if (inLiveWindow) return;         // väntar in slutsignalen, och tar då kön med sig

    const bool ok = send();
    gNextSend = millis() + (ok ? gInterval : TELEMETRY_RETRY_MS);
    Serial.printf("[tele] %s — %s\n", gStatus.c_str(), gLastTrace.c_str());
}

String preview() {
    JsonDocument doc;
    build(doc);
    String out;
    serializeJsonPretty(doc, out);
    return out;
}

const String &statusText() {
    static const String off     = "Avstängd i inställningarna";
    static const String none    = "Ingen mottagare inbyggd i den här firmwaren";
    static const String paused  = "Pausad av servern till nästa omstart";
    if (!available())       return none;
    if (!settings.telemetry) return off;
    if (gServerOff)          return paused;
    if (gCrashOff) {
        static String crashed;
        crashed = "Avstängd till nästa strömpåslag: förra försöket " + gLastCrash;
        return crashed;
    }
    return gStatus;
}

const String &lastTrace() { return gLastTrace; }
const String &lastCrash() { return gLastCrash; }

}  // namespace Telemetry
