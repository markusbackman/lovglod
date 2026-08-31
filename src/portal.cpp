#include "portal.h"
#include "config.h"
#include "settings.h"
#include "shl.h"
#include "updater.h"
#include "netcheck.h"
#include "leds.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>

StatusInfo status;

namespace {

// Måltaket ska stå både som tal och som text i formulärets max-attribut.
// Två nivåer krävs för att argumentet ska expanderas innan det blir sträng.
#define STRINGIFY_(x) #x
#define STRINGIFY(x)  STRINGIFY_(x)
#define GOAL_DELAY_MAX_STR STRINGIFY(GOAL_DELAY_MAX_S)

WebServer  server(80);
DNSServer  dns;
bool       gApMode     = false;
bool       gRunning    = false;

// Rutterna registreras en enda gång. WebServer kan inte ta bort en handler —
// server.stop() stänger bara lyssnaren — och _addRequestHandler lägger nya sist
// i listan medan Parsing.cpp väljer den *första* träffen. Ett andra
// registerRoutes() lämnade därför de gamla raderna kvar överst: "/" fastnade på
// setup-formuläret så fort enheten passerat portalen, och varje
// återanslutning läckte ytterligare åtta handler-objekt. Vilket läge vi är i
// avgörs numera inne i handlern i stället för vid registreringen.
bool       gRoutesRegistered = false;
bool       gSubmitted  = false;
bool       gRefresh    = false;
PushState  gPush;
bool       gPushPending = false;
String     gScanCache;
uint32_t   gScanAt     = 0;

const char PAGE_CSS[] PROGMEM = R"CSS(
:root{--bg:#0d1210;--card:#161d1a;--edge:#26332d;--txt:#e8f0ea;--dim:#8fa398;--gold:#ffc21a}
*{box-sizing:border-box}
body{margin:0;padding:20px;background:var(--bg);color:var(--txt);
     font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
.wrap{max-width:440px;margin:0 auto}
h1{font-size:22px;margin:0 0 4px;letter-spacing:-.01em}
h1 span{color:var(--gold)}
.sub{color:var(--dim);font-size:13px;margin:0 0 22px}
.card{background:var(--card);border:1px solid var(--edge);border-radius:14px;padding:18px;margin-bottom:14px}
label{display:block;font-size:13px;color:var(--dim);margin:14px 0 6px}
label:first-child{margin-top:0}
input,select{width:100%;padding:11px 12px;border-radius:9px;border:1px solid var(--edge);
     background:#0b100e;color:var(--txt);font-size:16px}
button{width:100%;margin-top:20px;padding:13px;border:0;border-radius:9px;
     background:var(--gold);color:#10160f;font-size:16px;font-weight:640;cursor:pointer}
button.ghost{background:transparent;color:var(--dim);border:1px solid var(--edge);font-weight:400;margin-top:10px}
table{width:100%;border-collapse:collapse;font-size:14px}
td{padding:7px 0;border-bottom:1px solid var(--edge);vertical-align:top}
td:first-child{color:var(--dim);width:42%}
tr:last-child td{border-bottom:0}
.ok{color:var(--gold)}
pre{white-space:pre-wrap;word-break:break-all;font-size:11px;color:var(--dim);
     background:#0b100e;padding:10px;border-radius:8px;max-height:220px;overflow:auto}
a{color:var(--gold)}
)CSS";

String formatNow(const char *fmt) {
    const time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    char buf[48];
    strftime(buf, sizeof(buf), fmt, &lt);
    return String(buf);
}

String htmlEscape(const String &s) {
    String o;
    o.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); i++) {
        const char c = s[i];
        switch (c) {
            case '&': o += F("&amp;");  break;
            case '<': o += F("&lt;");   break;
            case '>': o += F("&gt;");   break;
            case '"': o += F("&quot;"); break;
            default:  o += c;
        }
    }
    return o;
}

String head(const char *title) {
    String h = F("<!doctype html><html lang=sv><head><meta charset=utf-8>"
                 "<meta name=viewport content='width=device-width,initial-scale=1'>"
                 "<title>");
    h += title;
    h += F("</title><style>");
    h += FPSTR(PAGE_CSS);
    h += F("</style></head><body><div class=wrap>");
    return h;
}

// Nätverksscan cachas — en scan tar ~2 s och blockerar webbservern.
String scanNetworks() {
    if (gScanCache.length() && millis() - gScanAt < 30000) return gScanCache;

    const int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/false);
    String opts = F("<option value=''>— välj nätverk —</option>");
    for (int i = 0; i < n && i < 20; i++) {
        const String ssid = WiFi.SSID(i);
        if (!ssid.length()) continue;
        opts += "<option value='" + htmlEscape(ssid) + "'>" + htmlEscape(ssid) +
                "  (" + String(WiFi.RSSI(i)) + " dBm)</option>";
    }
    WiFi.scanDelete();
    gScanCache = opts;
    gScanAt    = millis();
    return opts;
}

// Rutter som bara hör hemma i ett av lägena. När allt registreras en gång måste
// de säga ifrån själva — förut föll det ut av att de inte ens var registrerade.
bool stationOnly() {
    if (!gApMode) return false;
    server.send(404, "text/plain", "404");
    return true;
}

bool apOnly() {
    if (gApMode) return false;
    server.send(404, "text/plain", "404");
    return true;
}

void handleSetup() {
    String p = head("Björklöven — WiFi");
    p += F("<h1>Björk<span>löven</span></h1><p class=sub>Anslut lampan till ditt WiFi</p>"
           "<div class=card><form method=POST action=/save>"
           "<label>Nätverk</label><select name=ssid_pick "
           "onchange=\"document.getElementById('ssid').value=this.value\">");
    p += scanNetworks();
    p += F("</select>"
           "<label>SSID</label><input id=ssid name=ssid required value='");
    p += htmlEscape(settings.wifiSsid);
    p += F("'><label>Lösenord</label><input name=pass type=password value=''>"
           "<button type=submit>Spara och anslut</button></form></div>"
           "<p class=sub>Lampan startar om och ansluter. Lyckas det inte dyker "
           "det här nätverket upp igen.</p></div></body></html>");
    server.send(200, "text/html; charset=utf-8", p);
}

void handleSave() {
    if (!server.hasArg("ssid")) { server.send(400, "text/plain", "ssid saknas"); return; }

    settings.wifiSsid = server.arg("ssid");
    settings.wifiPass = server.arg("pass");
    settings.save();
    gSubmitted = true;

    String p = head("Sparat");
    p += F("<h1>Sparat</h1><p class=sub>Lampan ansluter till <b>");
    p += htmlEscape(settings.wifiSsid);
    p += F("</b> och startar om.</p><div class=card>Glöden blir gul när "
           "anslutningen lyckats. Pulserar listen <b>rött</b> gick det inte "
           "att ansluta — kontrollera lösenordet."
           "</div></div></body></html>");
    server.send(200, "text/html; charset=utf-8", p);
}

void handleStatus() {
    String p = head("Björklöven-lampan");
    p += F("<h1>Björk<span>löven</span></h1><p class=sub>");
    p += F("v" FW_VERSION " · ");
    p += WiFi.localIP().toString();
    p += F("</p><div class=card><table>");

    auto row = [&](const char *k, const String &v, bool hi = false) {
        p += "<tr><td>" + String(k) + "</td><td" + (hi ? " class=ok" : "") + ">" +
             htmlEscape(v) + "</td></tr>";
    };

    row("Läge",             status.state, true);
    row("Datakälla",        status.pushMode ? String("push från mockservern")
                                          : String("shl.se  (skarp)"),
                            status.pushMode);
    if (settings.debugPush)
        row("Felsökningsläge", "På — tar emot push på /push", true);
    row("Nästa match",      status.nextGame);
    row("Senaste resultat", status.lastResult);
    row("Ställning nu",     status.liveScore);
    row("Måldröjning",      settings.goalDelayS
                                ? String(settings.goalDelayS) + " s  (väntar in tv-bilden)"
                                : String("Av — tänder direkt"));
    if (Leds::pendingGoals()) {
        row("Mål på gång",  String(Leds::pendingGoals()) + " st — tänder om " +
                            String((Leds::pendingGoalInMs() + 999) / 1000) + " s", true);
    }
    // Osynkad klocka är annars helt tyst utåt, och stänger ändå av matchläget,
    // målen och segerläget. Den ska gå att se utan att koppla in seriekabeln.
    row("Klocka",           status.timeSynced
                                ? formatNow("%Y-%m-%d %H:%M")
                                : String("INTE synkad — matchläge, mål och "
                                         "seger är avstängda"),
                            status.timeSynced);
    row("Vann igår",        status.wonYesterday ? "Ja — gnistor på" : "Nej");
    row("Live-ström",       status.sseLive ? "Ansluten" : "Av");
    row("WiFi-nät",         WiFi.SSID() + "  (" + WiFi.localIP().toString() + ")");
    row("Signal",           String(WiFi.RSSI()) + " dBm");
    row("Ledigt minne",     String(ESP.getFreeHeap() / 1024) + " kB");
    row("Upptid",           String(millis() / 60000) + " min");
    row("Firmware",         status.otaOnTrial
                                ? String(FW_VERSION) + "  — på prov, inte kvitterad"
                                : String(FW_VERSION),
                            status.otaOnTrial);
    row("Uppdatering",      Updater::statusText());

    p += F("</table></div>"
           "<div class=card><form method=POST action=/settings>"
           "<label>Ljusstyrka (0–255)</label><input name=bright type=number min=5 max=255 value='");
    p += String(settings.brightness);
    p += F("'><label>Uppdateringskälla (tom = av)</label>"
           "<input name=otasrc placeholder='markusbackman/bjorkloven-led' value='");
    p += htmlEscape(settings.otaSource);
    p += F("'><label>GitHub-token (krävs för privat repo)</label>"
           "<input name=otatok type=password autocomplete=off placeholder='");
    p += settings.otaToken.length() ? F("•••••• sparad — lämna tomt för att behålla")
                                    : F("github_pat_… (tomt för publikt repo)");
    p += F("'><label>Fördröjning på mål (sekunder — tv-sändningen ligger efter)</label>"
           "<input name=goaldly type=number min=0 max=" GOAL_DELAY_MAX_STR " value='");
    p += String(settings.goalDelayS);
    p += F("'><label>Fyra endast vid Björklövens mål</label><select name=ouronly>");
    p += settings.goalOnlyOurTeam
             ? F("<option value=1 selected>Ja</option><option value=0>Nej</option>")
             : F("<option value=1>Ja</option><option value=0 selected>Nej</option>");
    p += F("</select><label>Felsökningsläge — ta emot matchläge på /push</label>"
           "<select name=dbgpush>");
    p += settings.debugPush
             ? F("<option value=1 selected>På — mockservern får styra</option>"
                 "<option value=0>Av</option>")
             : F("<option value=1>På — mockservern får styra</option>"
                 "<option value=0 selected>Av</option>");
    p += F("</select><button type=submit>Spara</button></form>"
           "<form method=POST action=/test><button class=ghost type=submit>"
           "Testa målfyrverkeriet</button></form>"
           "<form method=POST action=/refresh><button class=ghost type=submit>"
           "Hämta matchdata nu</button></form>"
           "<form method=POST action=/update><button class=ghost type=submit>"
           "Sök efter uppdatering nu</button></form>"
           "<form method=POST action=/forget onsubmit=\"return confirm('Glöm WiFi och starta setup-portalen?')\">"
           "<button class=ghost type=submit>Glöm WiFi</button></form>"
           "</div><p class=sub><a href=/debug>Felsökning</a></p></div></body></html>");

    server.send(200, "text/html; charset=utf-8", p);
}

void handleSettings() {
    if (stationOnly()) return;
    if (server.hasArg("bright"))
        settings.brightness = (uint8_t)constrain(server.arg("bright").toInt(), 5, 255);
    if (server.hasArg("otasrc")) {
        const String next = server.arg("otasrc");
        // Ny källa → nollställ backoff-räknaren.
        if (next != settings.otaSource) settings.clearOtaFailures();
        settings.otaSource = next;
    }
    if (server.hasArg("otatok")) {
        // Tomt fält betyder "rör inte" — annars skulle token raderas varje
        // gång man justerar ljusstyrkan. Skriv "-" för att nollställa.
        const String tok = server.arg("otatok");
        if (tok == "-")            { settings.otaToken = ""; settings.clearOtaFailures(); }
        else if (tok.length())     { settings.otaToken = tok; settings.clearOtaFailures(); }
    }
    if (server.hasArg("ouronly")) settings.goalOnlyOurTeam = server.arg("ouronly") == "1";
    if (server.hasArg("dbgpush")) {
        const bool next = server.arg("dbgpush") == "1";
        // Slår man av mitt i en pågående push ska lampan hämta från SHL igen
        // direkt, inte stå kvar på det påhittade läget tills leasen tar slut.
        if (!next && settings.debugPush) gRefresh = true;
        settings.debugPush = next;
    }
    if (server.hasArg("goaldly"))
        settings.goalDelayS =
            (uint8_t)constrain(server.arg("goaldly").toInt(), 0, GOAL_DELAY_MAX_S);
    settings.save();
    Leds::setBrightness(settings.brightness);
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleTest() {
    if (stationOnly()) return;
    // Utan fördröjning med flit: en testknapp som står tyst i 15 sekunder ser
    // trasig ut. Fördröjningen gäller riktiga mål från SHL.
    Leds::triggerGoal();
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleNetTest() {
    if (stationOnly()) return;
    // Testet blockerar i upp till en halv minut på ett trasigt nät. Samma
    // förloppsstapel som vid uppstart, annars ser listen bara död ut så länge.
    NetCheck::run([](uint8_t done, uint8_t total) {
        Leds::setWorkProgress(done, total);
        Leds::renderNow();
    });
    server.sendHeader("Location", "/debug");
    server.send(303);
}

void handleRefresh() {
    if (stationOnly()) return;
    // Hämtningen blockerar i flera sekunder — låt loopen göra jobbet.
    gRefresh = true;
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleUpdate() {
    if (stationOnly()) return;
    // Kontrollen tar upp till 30 s och laddar ner ~1 MB. Kör den i loopen
    // istället för här, annars timeoutar webbläsaren mitt i.
    Updater::requestCheck();
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleForget() {
    settings.clearWifi();
    server.send(200, "text/html; charset=utf-8",
                head("Nollställt") + F("<h1>WiFi glömt</h1><p class=sub>Startar om i setup-läge…"
                                       "</p></div></body></html>"));
    delay(600);
    ESP.restart();
}

void handleDebug() {
    if (stationOnly()) return;
    String p = head("Felsökning");
    p += F("<h1>Felsökning</h1><p class=sub>Senaste live-ramen från "
           SHL_LIVE_HOST "</p><div class=card><pre>");
    p += htmlEscape(Shl::lastRawFrame().length() ? Shl::lastRawFrame()
                                                 : String("(ingen ram mottagen ännu)"));
    p += F("</pre></div><div class=card><table>");
    p += "<tr><td>Senaste fel</td><td>" + htmlEscape(Shl::lastError()) + "</td></tr>";
    p += "<tr><td>Team-UUID</td><td>" SHL_TEAM_UUID "</td></tr>";
    p += "<tr><td>API-URL</td><td>" + htmlEscape(Shl::apiBaseUrl()) + "</td></tr>";
    p += "<tr><td>Live-URL</td><td>" + htmlEscape(Shl::liveBaseUrl()) + "</td></tr>";
    p += "<tr><td>OTA-URL</td><td>" +
         htmlEscape(Updater::resolveSourceUrl(settings.otaSource)) + "</td></tr>";
    p += "<tr><td>OTA-status</td><td>" + htmlEscape(Updater::statusText()) + "</td></tr>";
    p += String("<tr><td>OTA-token</td><td>") +
         (settings.otaToken.length() ? "sparad (" + String(settings.otaToken.length()) +
                                       " tecken)" : "ingen — publikt repo") + "</td></tr>";
    if (settings.otaBadCount)
        p += "<tr><td>Misslyckad version</td><td>" + htmlEscape(settings.otaBadVersion) +
             " (" + String(settings.otaBadCount) + " försök)</td></tr>";
    p += F("</table></div>");
    p += F("<div class=card><b>Nätverksdiagnostik</b><pre>");
    p += htmlEscape(NetCheck::report().length() ? NetCheck::report()
                                                : String("(inte körd)"));
    p += F("</pre><form method=POST action=/nettest>"
           "<button class=ghost type=submit>Kör om nätverkstestet</button></form></div>");
    p += F("<p class=sub><a href=/>Tillbaka</a></p></div></body></html>");
    server.send(200, "text/html; charset=utf-8", p);
}

void handlePush() {
    if (stationOnly()) return;
    // Felsökningsläget är porten. Är det av finns endpointen inte — lampan ska
    // inte gå att styra från nätet bara för att någon känner till adressen.
    if (!settings.debugPush) {
        server.send(404, "text/plain", "404");
        return;
    }

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, server.arg("plain"));
    if (err) {
        server.send(400, "text/plain", String("json: ") + err.c_str());
        return;
    }

    PushState p;

    const JsonObjectConst n = doc["next"].as<JsonObjectConst>();
    if (!n.isNull()) {
        p.hasNext  = true;
        p.homeCode = n["home"].as<const char *>()     ? n["home"].as<const char *>() : "";
        p.awayCode = n["away"].as<const char *>()     ? n["away"].as<const char *>() : "";
        p.nextText = n["text"].as<const char *>()     ? n["text"].as<const char *>() : "";
        p.homeIsUs = n["homeIsUs"] | false;
    }

    p.live = doc["live"] | false;

    const JsonObjectConst sc = doc["score"].as<JsonObjectConst>();
    if (!sc.isNull()) {
        p.hasScore = true;
        p.home     = sc["home"] | -1;
        p.away     = sc["away"] | -1;
    }

    const JsonObjectConst la = doc["last"].as<JsonObjectConst>();
    if (!la.isNull()) {
        p.hasLast      = true;
        p.wonYesterday = la["won"] | false;
        p.lastResult   = la["text"].as<const char *>() ? la["text"].as<const char *>() : "";
    }

    // Målfyrverkeriet får inte starta här inne — loopen plockar upp det.
    gPush        = p;
    gPushPending = true;
    server.send(200, "application/json", "{\"ok\":true}");
}

void handleNotFound() {
    if (gApMode) {
        server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
        server.send(302, "text/plain", "");
        return;
    }
    server.send(404, "text/plain", "404");
}

// Samma adress, olika sida beroende på läge. Det är den här förgreningen som
// ersätter de två uppsättningarna rutter.
void handleRoot() {
    if (gApMode) handleSetup();
    else         handleStatus();
}

// Operativsystemens kontroll-URL:er för "finns internet?". Svarar vi inte som
// förväntat här öppnas aldrig inloggningsrutan. Utanför portalläget betyder de
// ingenting.
void handleCaptiveProbe() {
    if (apOnly()) return;
    handleSetup();
}

void registerRoutes() {
    if (gRoutesRegistered) return;
    gRoutesRegistered = true;

    server.on("/", HTTP_GET, handleRoot);

    for (const char *probe : {"/hotspot-detect.html",       // iOS/macOS
                              "/library/test/success.html",
                              "/generate_204",              // Android
                              "/gen_204",
                              "/ncsi.txt",                  // Windows
                              "/connecttest.txt",
                              "/redirect",
                              "/canonical.html"})
        server.on(probe, HTTP_GET, handleCaptiveProbe);

    server.on("/settings", HTTP_POST, handleSettings);
    server.on("/test",     HTTP_POST, handleTest);
    server.on("/debug",    HTTP_GET,  handleDebug);
    server.on("/update",   HTTP_POST, handleUpdate);
    server.on("/refresh",  HTTP_POST, handleRefresh);
    server.on("/nettest",  HTTP_POST, handleNetTest);
    server.on("/push",     HTTP_POST, handlePush);

    server.on("/save",   HTTP_POST, handleSave);
    server.on("/forget", HTTP_POST, handleForget);
    server.onNotFound(handleNotFound);
}

}  // namespace

namespace Portal {

void startAccessPoint() {
    stop();
    gApMode    = true;
    gSubmitted = false;

    // AP+STA krävs för att kunna scanna efter nätverk medan portalen är uppe.
    WiFi.mode(WIFI_AP_STA);

    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    char ssid[40];
    snprintf(ssid, sizeof(ssid), "%s-%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);

    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                      IPAddress(255, 255, 255, 0));
    WiFi.softAP(ssid, strlen(AP_PASSWORD) ? AP_PASSWORD : nullptr);

    // Wildcard-DNS: varje uppslag pekar tillbaka på oss.
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns.start(53, "*", WiFi.softAPIP());

    registerRoutes();
    server.begin();
    gRunning = true;

    Serial.printf("[portal] AP igång: %s → http://%s/\n", ssid,
                  WiFi.softAPIP().toString().c_str());
}

void startStationServer() {
    stop();
    gApMode = false;
    registerRoutes();
    server.begin();
    gRunning = true;
    Serial.printf("[portal] Statussida: http://%s/\n", WiFi.localIP().toString().c_str());
}

void stop() {
    if (!gRunning) return;
    server.stop();
    dns.stop();
    gRunning = false;
}

void loop() {
    if (!gRunning) return;
    if (gApMode) dns.processNextRequest();
    server.handleClient();
}

bool isAccessPoint()          { return gApMode; }
bool credentialsSubmitted()   { return gSubmitted; }
bool refreshRequested()       { return gRefresh; }
void clearRefresh()           { gRefresh = false; }
bool pushPending()            { return gPushPending; }
PushState takePush()          { gPushPending = false; return gPush; }

}  // namespace Portal
