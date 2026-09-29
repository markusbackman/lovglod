#include "telemetry.h"
#include "config.h"
#include "settings.h"
#include "leds.h"
#include "shl.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
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

bool send() {
    JsonDocument doc;
    build(doc);
    String body;
    serializeJson(doc, body);

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
        gStatus = "Kunde inte starta anropet";
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-LovGlod-Key", TELEMETRY_KEY);
    http.addHeader("User-Agent", "LovGlod/" FW_VERSION);

    const int code = http.POST(body);
    const String reply = code > 0 ? http.getString() : String();
    http.end();

    if (code != 200 && code != 204) {
        gStatus = "Misslyckades (" + (code > 0 ? "HTTP " + String(code)
                                                 : String(HTTPClient::errorToString(code))) + ")";
        return false;
    }

    applyReply(reply);
    gCount   = 0;
    gDropped = 0;
    gStatus  = "Senast skickad efter " + String(millis() / 60000) + " min upptid, " +
               String(body.length()) + " byte";
    return true;
}

}  // namespace

namespace Telemetry {

bool available() { return strlen(TELEMETRY_URL) > 0; }

void event(const char *type, const String &detail) {
    if (gCount == TELEMETRY_QUEUE_MAX) {
        for (uint8_t i = 1; i < gCount; i++) gQueue[i - 1] = gQueue[i];
        gCount--;
        gDropped++;
    }
    gQueue[gCount++] = Event{nowUtc(), millis() / 1000, type, detail.substring(0, 80)};
}

void loop(bool inLiveWindow) {
    if (!available() || !settings.telemetry || gServerOff) return;

    if (!gStarted) {
        gStarted  = true;
        gNextSend = millis() + TELEMETRY_FIRST_MS;
        return;
    }
    if ((int32_t)(millis() - gNextSend) < 0) return;
    if (inLiveWindow) return;         // väntar in slutsignalen, och tar då kön med sig

    const bool ok = send();
    gNextSend = millis() + (ok ? gInterval : TELEMETRY_RETRY_MS);
    Serial.printf("[tele] %s\n", gStatus.c_str());
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
    return gStatus;
}

}  // namespace Telemetry
