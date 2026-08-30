#include "netcheck.h"
#include "config.h"
#include <WiFi.h>

namespace {

String            gReport;
NetCheck::StepCb  gOnStep     = nullptr;
uint8_t           gStepsDone  = 0;
uint8_t           gStepTotal  = NetCheck::STEP_TOTAL;

// Räknar upp och rapporterar. Varje delprov är ett steg, oavsett utfall.
void step() {
    if (gOnStep) gOnStep(++gStepsDone, gStepTotal);
}

void line(const String &s) {
    Serial.println("[net] " + s);
    gReport += s;
    gReport += '\n';
}

// Ren TCP-anslutning, ingen TLS. Skiljer routing/brandvägg från certifikatstrul.
bool tcpProbe(const char *label, const char *host, uint16_t port, uint16_t timeoutMs = 6000) {
    WiFiClient c;
    const uint32_t t0 = millis();
    const bool ok = c.connect(host, port, timeoutMs) == 1;
    const uint32_t dt = millis() - t0;
    c.stop();
    line(String(ok ? "  OK      " : "  BLOCKAD ") + label + "  (" + dt + " ms)");
    step();
    return ok;
}

bool tcpProbeIp(const char *label, IPAddress ip, uint16_t port, uint16_t timeoutMs = 6000) {
    WiFiClient c;
    const uint32_t t0 = millis();
    const bool ok = c.connect(ip, port, timeoutMs) == 1;
    const uint32_t dt = millis() - t0;
    c.stop();
    line(String(ok ? "  OK      " : "  BLOCKAD ") + label + "  (" + dt + " ms)");
    step();
    return ok;
}

void dnsProbe(const char *host) {
    IPAddress ip;
    const uint32_t t0 = millis();
    const bool ok = WiFi.hostByName(host, ip);
    line(String(ok ? "  OK      " : "  MISSLYCKAS ") + host + " -> " +
         (ok ? ip.toString() : String("(ingen adress)")) + "  (" + (millis() - t0) + " ms)");
    step();
}

}  // namespace

namespace NetCheck {

const String &run(StepCb onStep) {
    gReport    = "";
    gOnStep    = onStep;
    gStepsDone = 0;
    if (gOnStep) gOnStep(0, gStepTotal);

    line("── Nätverksdiagnostik ──");
    line("SSID      " + WiFi.SSID() + "   " + String(WiFi.RSSI()) + " dBm");
    line("IP        " + WiFi.localIP().toString() + "/" + WiFi.subnetMask().toString());
    line("Gateway   " + WiFi.gatewayIP().toString());
    line("DNS       " + WiFi.dnsIP().toString());
    line("Heap      " + String(ESP.getFreeHeap() / 1024) + " kB fritt");

    line("");
    line("DNS-uppslag:");
    dnsProbe(SHL_API_HOST);
    dnsProbe(SHL_LIVE_HOST);
    dnsProbe("api.github.com");

    line("");
    line("TCP-anslutningar:");
    // Gateway på 80 — kontroll att lokal trafik alls fungerar.
    tcpProbeIp("gateway:80        (lokalt nät)", WiFi.gatewayIP(), 80, 3000);
    // Rå IP, ingen DNS inblandad — skiljer namnuppslag från routing.
    tcpProbeIp("1.1.1.1:443       (internet, utan DNS)", IPAddress(1, 1, 1, 1), 443);
    tcpProbe  ("www.shl.se:80     (HTTP)", SHL_API_HOST, 80);
    tcpProbe  ("www.shl.se:443    (HTTPS — den vi behöver)", SHL_API_HOST, 443);
    tcpProbe  ("game-broadcaster.s8y.se:443", SHL_LIVE_HOST, 443);
    tcpProbe  ("api.github.com:443 (OTA)", "api.github.com", 443);

    line("");
    line("Tolkning:");
    line("  Allt OK              -> nätet är fint, felet ligger i TLS/appen");
    line("  DNS OK men TCP blockad -> brandvägg/VLAN släpper inte ut trafiken");
    line("  1.1.1.1:443 blockad  -> nätet har ingen väg ut till internet alls");
    line("  Bara gateway OK      -> isolerat gästnät/IoT-VLAN");
    line("────────────────────────");

    gOnStep = nullptr;
    return gReport;
}

const String &report() { return gReport; }

}  // namespace NetCheck
