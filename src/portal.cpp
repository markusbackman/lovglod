#include "portal.h"
#include "config.h"
#include "settings.h"
#include "shl.h"
#include "updater.h"
#include "netcheck.h"
#include "leds.h"
#include "logo_svg.h"
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
#define LED_COUNT_MIN_STR  STRINGIFY(LED_COUNT_MIN)
#define LED_COUNT_MAX_STR  STRINGIFY(LED_COUNT_MAX)

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

// IF Björklövens formspråk (bjorkloven-design): skogsgrönt som yta och
// handling, guld bara som accent, rött enbart för LIVE, raka hörn överallt.
// Antonio och Barlow bäddas inte in — portalen har inget internet och
// typsnitten väger mer än sidorna — så rubrikerna faller tillbaka på
// systemets smala sans-serif.
const char PAGE_CSS[] PROGMEM = R"CSS(
:root{--g9:#071912;--g8:#0C2A1F;--g7:#124734;--g5:#1C5F45;--gold:#FFD000;
--bg:#F7F7F7;--line:#E8E8E8;--edge:#B5B5B5;--mut:#484848;
--disp:Antonio,"Avenir Next Condensed","Roboto Condensed","Arial Narrow",sans-serif-condensed,Impact,sans-serif;
--body:Barlow,-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Arial,sans-serif}
*{box-sizing:border-box}
html{-webkit-text-size-adjust:100%}
body{margin:0;background:var(--bg);color:#000;font:16px/1.5 var(--body)}
.in{max-width:560px;margin:0 auto;padding:0 16px}
.bar{position:sticky;top:0;z-index:2;background:var(--g8);color:#fff}
.bar .in{display:flex;align-items:center;gap:10px;height:50px}
.bar img{height:26px;width:auto;display:block}
.bar small{margin-left:auto;font-size:12px;font-weight:600;letter-spacing:.1em;
 text-transform:uppercase;color:rgba(255,255,255,.7)}
.hero{background:var(--g7);color:#fff;padding:28px 0 32px}
.eye{margin:0 0 8px;font-size:13px;font-weight:700;letter-spacing:.12em;
 text-transform:uppercase;color:var(--gold)}
h1{margin:0;font:700 44px/.95 var(--disp);text-transform:uppercase;overflow-wrap:anywhere}
.lead{margin:12px 0 0;font-size:17px;color:rgba(255,255,255,.88)}
.lead b{color:#fff}
main.in{padding-top:24px;padding-bottom:48px}
h2{display:flex;align-items:center;gap:10px;margin:32px 0 12px;
 font:700 26px/1 var(--disp);text-transform:uppercase}
h2:before{content:"";flex:none;width:7px;height:26px;background:var(--g7)}
main>h2:first-child{margin-top:0}
.card{background:#fff;padding:20px;margin-bottom:16px}
label{display:block;margin:18px 0 6px;font-size:13px;font-weight:600;
 letter-spacing:.04em;text-transform:uppercase;color:var(--mut)}
form>label:first-child{margin-top:0}
input,select{width:100%;min-height:48px;padding:10px 12px;border:1px solid var(--edge);
 border-radius:0;background:#fff;color:#000;font:16px var(--body)}
input{-webkit-appearance:none;appearance:none}
input:focus,select:focus{outline:2px solid var(--g7);outline-offset:0;border-color:var(--g7)}
button{width:100%;min-height:48px;margin-top:22px;padding:12px 20px;border:2px solid var(--g7);
 border-radius:0;background:var(--g7);color:#fff;font:700 15px var(--body);
 letter-spacing:.06em;text-transform:uppercase;cursor:pointer}
button:hover{filter:brightness(1.25)}
button:focus-visible,a:focus-visible{outline:2px solid var(--g7);outline-offset:2px}
button.ghost{margin-top:10px;background:#fff;color:var(--g7)}
button.ghost:hover{filter:none;background:var(--g7);color:#fff}
table{width:100%;border-collapse:collapse;font-size:15px}
td{padding:10px 0;vertical-align:top;overflow-wrap:anywhere}
tr+tr td{border-top:1px solid var(--line)}
td:first-child{width:40%;padding-right:12px;font-size:13px;font-weight:600;
 letter-spacing:.03em;text-transform:uppercase;color:var(--mut)}
.ok{font-weight:700;color:var(--g7)}
.note{margin:12px 0 0;font-size:14px;color:var(--mut)}
.card>.note:first-child{margin-top:0}
pre{margin:0 0 4px;white-space:pre-wrap;word-break:break-all;font:12px/1.45 ui-monospace,Menlo,monospace;
 background:var(--g9);color:#D8E3DD;padding:12px;max-height:260px;overflow:auto}
a{color:var(--g7);font-weight:700}
.back{display:inline-flex;align-items:center;min-height:44px;text-transform:uppercase;
 letter-spacing:.06em;font-size:14px;text-decoration:none}
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
            case '\'': o += F("&#39;"); break;
            default:  o += c;
        }
    }
    return o;
}

// Sidhuvud: mörkgrön list med klubbmärket, grön hero med rubrik, sedan <main>.
// eyebrow och lead är färdig HTML — anroparen escapar det som kommer utifrån.
String head(const char *title, const char *eyebrow, const String &heading,
            const String &lead = String()) {
    String h = F("<!doctype html><html lang=sv><head><meta charset=utf-8>"
                 "<meta name=viewport content='width=device-width,initial-scale=1'>"
                 "<meta name=theme-color content='#0C2A1F'>"
                 "<link rel=icon href=/icon.svg><title>");
    h += title;
    h += F("</title><style>");
    h += FPSTR(PAGE_CSS);
    h += F("</style></head><body><header class=bar><div class=in>"
           "<img src=/logo.svg alt='LövGlöd'><small>");
    h += gApMode ? F("Setup") : F("Admin");
    h += F("</small></div></header><section class=hero><div class=in><p class=eye>");
    h += eyebrow;
    h += F("</p><h1>");
    h += heading;
    h += F("</h1>");
    if (lead.length()) h += "<p class=lead>" + lead + "</p>";
    h += F("</div></section><main class=in>");
    return h;
}

const char PAGE_END[] PROGMEM = "</main></body></html>";

void handleLogo() {
    server.sendHeader("Cache-Control", "public, max-age=604800");
    server.send_P(200, "image/svg+xml", LOGO_SVG);
}

void handleIcon() {
    server.sendHeader("Cache-Control", "public, max-age=604800");
    server.send_P(200, "image/svg+xml", LOGO_ICON_SVG);
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

// Listval och antal dioder. Samma fält i setup-formuläret och på statussidan.
// Så länge ingen list är vald finns ett tomt förval, och required gör att
// setup inte går att skicka utan att man tagit ställning.
String stripFields() {
    const auto opt = [](LedStrip s, const char *text) -> String {
        return String("<option value=") + String((int)s) +
               (settings.ledStrip == s ? " selected>" : ">") + text + "</option>";
    };
    String f = F("<label>LED-list</label><select name=strip required>");
    if (settings.ledStrip == LED_STRIP_UNSET)
        f += F("<option value='' selected>— välj list —</option>");
    f += opt(LED_STRIP_WS2812, "WS2812B / NeoPixel — 3 trådar: 5V, GND, DIN");
    f += opt(LED_STRIP_APA102, "APA102 / DotStar — 4 trådar: 5V, GND, DI, CI");
    f += F("</select><label>Antal dioder</label><input name=count type=number required "
           "min=" LED_COUNT_MIN_STR " max=" LED_COUNT_MAX_STR " value='");
    f += String(settings.ledCount);
    f += F("'>");
    return f;
}

// Läser listval och antal ur formuläret och byter drivrutin direkt om något
// ändrats. Anroparen sparar. false = ingen giltig list vald.
bool takeStripArgs() {
    const long strip = server.arg("strip").toInt();
    if (strip != LED_STRIP_WS2812 && strip != LED_STRIP_APA102) return false;

    const uint16_t count =
        server.hasArg("count")
            ? (uint16_t)constrain(server.arg("count").toInt(), LED_COUNT_MIN, LED_COUNT_MAX)
            : settings.ledCount;

    if ((LedStrip)strip == settings.ledStrip && count == settings.ledCount) return true;
    settings.ledStrip = (LedStrip)strip;
    settings.ledCount = count;
    Leds::configure(settings.ledStrip, settings.ledCount);
    return true;
}

// Listvalet är administratörens steg, inte kundens. Den som monterar lampan vet
// vilken list som sitter i; den som packar upp den hemma vet det inte, och ska
// bara behöva sitt eget WiFi. Därför sparas listen på en egen sida, utan WiFi,
// och portalen visar den i stället för WiFi-formuläret så länge ingen list är
// vald. Nås även på statussidan via Felsökning.
void handleStripPage() {
    String p = head("LövGlöd — LED-list", gApMode ? "Steg 1 · För montören" : "Admin",
                    F("Välj LED-list"),
                    F("Välj vilken list som sitter i. Valet sparas i lampan och ligger "
                      "kvar när den sedan kopplas till ett WiFi."));
    p += F("<div class=card><form method=POST action=/strip>");
    p += stripFields();
    p += F("<button type=submit>Spara list</button></form>"
           "<p class=note>Listen byter direkt — den gröna pulsen flyttar till den "
           "valda listen. Syns den inte är valet fel.</p></div>");
    if (settings.ledStrip != LED_STRIP_UNSET)
        p += F("<a class=back href=/>← Tillbaka</a>");
    p += FPSTR(PAGE_END);
    server.send(200, "text/html; charset=utf-8", p);
}

void handleStripSave() {
    if (!takeStripArgs()) { server.send(400, "text/plain", "välj LED-list"); return; }
    settings.save();
    server.sendHeader("Location", "/");
    server.send(303);
}

void handleSetup() {
    if (settings.ledStrip == LED_STRIP_UNSET) { handleStripPage(); return; }

    String p = head("LövGlöd — WiFi", "Välkommen hem", F("Anslut din LövGlöd"),
                    F("Välj ditt WiFi så börjar lampan följa Björklöven."));
    p += F("<div class=card><form method=POST action=/save>"
           "<label>Nätverk</label><select name=ssid_pick "
           "onchange=\"document.getElementById('ssid').value=this.value\">");
    p += scanNetworks();
    p += F("</select>"
           "<label>Nätverksnamn (SSID)</label><input id=ssid name=ssid required value='");
    p += htmlEscape(settings.wifiSsid);
    p += F("'><label>Lösenord</label><input name=pass type=password value=''>"
           "<button type=submit>Spara och anslut</button></form>"
           "<p class=note>Lampan startar om och ansluter. Lyckas det inte dyker "
           "det här nätverket upp igen.</p></div>");
    p += FPSTR(PAGE_END);
    server.send(200, "text/html; charset=utf-8", p);
}

void handleSave() {
    if (!server.hasArg("ssid")) { server.send(400, "text/plain", "ssid saknas"); return; }
    // Portalen visar inte WiFi-formuläret förrän listen är vald; det här fångar
    // en gammal cachad sida.
    if (settings.ledStrip == LED_STRIP_UNSET) {
        server.send(400, "text/plain", "LED-list inte vald");
        return;
    }

    settings.wifiSsid = server.arg("ssid");
    settings.wifiPass = server.arg("pass");
    settings.save();
    gSubmitted = true;

    String p = head("LövGlöd — Sparat", "Sparat", F("Nu ansluter vi"),
                    "Lampan ansluter till <b>" + htmlEscape(settings.wifiSsid) +
                    "</b> och startar om.");
    p += F("<h2>Så ser du hur det gick</h2><div class=card><table>"
           "<tr><td>Gul glöd</td><td>Anslutningen lyckades</td></tr>"
           "<tr><td>Röd puls</td><td>Det gick inte — kontrollera lösenordet och "
           "anslut till setup-nätet igen</td></tr></table></div>");
    p += FPSTR(PAGE_END);
    server.send(200, "text/html; charset=utf-8", p);
}

void handleStatus() {
    String p = head("LövGlöd — Admin",
                    ("v" FW_VERSION " · " + WiFi.localIP().toString()).c_str(),
                    F("LövGlöd"), "Läge: <b>" + htmlEscape(status.state) + "</b>");
    p += F("<h2>Status</h2><div class=card><table>");

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
    row("LED-list",         String(Leds::stripName(settings.ledStrip)) + ", " +
                            String(settings.ledCount) + " dioder");
    row("WiFi-nät",         WiFi.SSID() + "  (" + WiFi.localIP().toString() + ")");
    row("Signal",           String(WiFi.RSSI()) + " dBm");
    row("Ledigt minne",     String(ESP.getFreeHeap() / 1024) + " kB");
    row("Upptid",           String(millis() / 60000) + " min");
    row("Senaste omstart",  status.resetReason, status.resetAbnormal);
    row("Omstarter",        String(settings.abnormalBoots) +
                            " onormala sedan strömpåslag  (" +
                            String(settings.bootCount) + " starter totalt)",
                            settings.abnormalBoots > 0);
    row("Firmware",         status.otaOnTrial
                                ? String(FW_VERSION) + "  — på prov, inte kvitterad"
                                : String(FW_VERSION),
                            status.otaOnTrial);
    row("Uppdatering",      Updater::statusText());
    row("Kanal",            String(settings.otaBeta ? "Beta" : "Stabil"));

    p += F("</table></div>"
           "<h2>Inställningar</h2><div class=card><form method=POST action=/settings>"
           "<label>Ljusstyrka (5–255)</label><input name=bright type=number min=5 max=255 value='");
    p += String(settings.brightness);
    p += F("'><label>Fördröjning på mål (sekunder — tv-sändningen ligger efter)</label>"
           "<input name=goaldly type=number min=0 max=" GOAL_DELAY_MAX_STR " value='");
    p += String(settings.goalDelayS);
    p += F("'><label>Uppdateringskälla (tom = av)</label>"
           "<input name=otasrc placeholder='markusbackman/lovglod' value='");
    p += htmlEscape(settings.otaSource);
    p += F("'><label>Uppdateringskanal</label><select name=otabeta>");
    p += settings.otaBeta
             ? F("<option value=0>Stabil</option>"
                 "<option value=1 selected>Beta — pre-releases, kollar var 3:e timme</option>")
             : F("<option value=0 selected>Stabil</option>"
                 "<option value=1>Beta — pre-releases, kollar var 3:e timme</option>");
    p += F("</select><label>Felsökningsläge — ta emot matchläge på /push</label>"
           "<select name=dbgpush>");
    p += settings.debugPush
             ? F("<option value=1 selected>På — mockservern får styra</option>"
                 "<option value=0>Av</option>")
             : F("<option value=1>På — mockservern får styra</option>"
                 "<option value=0 selected>Av</option>");
    p += F("</select><button type=submit>Spara inställningar</button></form></div>"
           "<h2>Åtgärder</h2><div class=card>"
           "<form method=POST action=/test><button class=ghost type=submit>"
           "Testa målfyrverkeriet</button></form>"
           "<form method=POST action=/refresh><button class=ghost type=submit>"
           "Hämta matchdata nu</button></form>"
           "<form method=POST action=/update><button class=ghost type=submit>"
           "Sök efter uppdatering nu</button></form>"
           "<form method=POST action=/forget onsubmit=\"return confirm('Glöm WiFi och starta setup-portalen?')\">"
           "<button class=ghost type=submit>Glöm WiFi</button></form>"
           "</div><a class=back href=/debug>Felsökning →</a>");
    p += FPSTR(PAGE_END);

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
    if (server.hasArg("otabeta")) {
        const bool next = server.arg("otabeta") == "1";
        // Kolla direkt i nya kanalen i stället för att vänta ut det gamla
        // intervallet. Lämnar man betan betyder det att lampan går tillbaka
        // till senaste stabila — versionen skiljer sig, och det räcker.
        if (next != settings.otaBeta) Updater::requestCheck();
        settings.otaBeta = next;
    }
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
                head("LövGlöd — Nollställt", "Nollställt", F("WiFi glömt"),
                     F("Lampan startar om i setup-läge. Anslut till nätet "
                       "LövGlöd-Setup för att välja ett nytt WiFi.")) +
                    FPSTR(PAGE_END));
    delay(600);
    ESP.restart();
}

void handleDebug() {
    if (stationOnly()) return;
    String p = head("LövGlöd — Felsökning", "Admin", F("Felsökning"));
    p += F("<h2>Senaste live-ram</h2><div class=card><p class=note>Från "
           SHL_LIVE_HOST "</p><pre>");
    p += htmlEscape(Shl::lastRawFrame().length() ? Shl::lastRawFrame()
                                                 : String("(ingen ram mottagen ännu)"));
    p += F("</pre></div><h2>Källor</h2><div class=card><table>");
    p += "<tr><td>Senaste fel</td><td>" + htmlEscape(Shl::lastError()) + "</td></tr>";
    p += "<tr><td>Team-UUID</td><td>" SHL_TEAM_UUID "</td></tr>";
    p += "<tr><td>API-URL</td><td>" + htmlEscape(Shl::apiBaseUrl()) + "</td></tr>";
    p += "<tr><td>Live-URL</td><td>" + htmlEscape(Shl::liveBaseUrl()) + "</td></tr>";
    p += "<tr><td>OTA-URL</td><td>" +
         htmlEscape(Updater::resolveSourceUrl(settings.otaSource, settings.otaBeta)) + "</td></tr>";
    p += "<tr><td>OTA-status</td><td>" + htmlEscape(Updater::statusText()) + "</td></tr>";
    if (settings.otaBadCount)
        p += "<tr><td>Misslyckad version</td><td>" + htmlEscape(settings.otaBadVersion) +
             " (" + String(settings.otaBadCount) + " försök)</td></tr>";
    p += F("</table></div>");
    p += F("<h2>Nätverk</h2><div class=card><pre>");
    p += htmlEscape(NetCheck::report().length() ? NetCheck::report()
                                                : String("(inte körd)"));
    p += F("</pre><form method=POST action=/nettest>"
           "<button class=ghost type=submit>Kör om nätverkstestet</button></form></div>");
    p += F("<h2>LED-list</h2><div class=card><p class=note>");
    p += htmlEscape(String(Leds::stripName(settings.ledStrip)) + ", " +
                    String(settings.ledCount) + " dioder");
    p += F("</p><a class=back href=/strip>Ändra LED-list →</a></div>"
           "<a class=back href=/>← Tillbaka</a>");
    p += FPSTR(PAGE_END);
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
    server.on("/logo.svg", HTTP_GET, handleLogo);
    server.on("/icon.svg", HTTP_GET, handleIcon);

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

    // Listvalet i båda lägena: i portalen före WiFi, på statussidan i efterhand.
    server.on("/strip",  HTTP_GET,  handleStripPage);
    server.on("/strip",  HTTP_POST, handleStripSave);

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
