#pragma once
#include <Arduino.h>
#include "leds.h"

// När listen får lysa. Värdet sparas i NVS — numrera aldrig om, lägg bara till.
enum LampMode : uint8_t {
    LAMP_ALWAYS = 0,   // som förut: glöd dygnet runt
    LAMP_MATCH  = 1,   // släckt, tänds matchLeadMin före nedsläpp, släcks vid slutsignal
    LAMP_OFF    = 2,   // helt släckt
};

// Persistent konfiguration i NVS. Överlever omstart och OTA-uppdatering.
struct Settings {
    String  wifiSsid;
    String  wifiPass;

    // Vilken list som är inkopplad och hur lång den är. Väljs i setup-portalen
    // och går att ändra på statussidan. Rörs inte av "Glöm WiFi" — listen sitter
    // kvar även när nätet byts.
    LedStrip ledStrip;
    uint16_t ledCount;

    String  otaSource;        // "owner/repo" eller manifest-URL. Tom = av.
    bool    otaBeta;          // Betaprogrammet: tar även pre-releases, och kollar
                              // oftare (OTA_CHECK_BETA_MS). Av = bara stabila.
    uint8_t brightness;
    bool    debugPush;        // Ta emot matchläge på POST /push. Av = lampan
                              // hämtar bara från SHL och ignorerar nätet.
    uint8_t goalDelayS;       // TV-fördröjning i sekunder. 0 = tänd direkt.
    LampMode lampMode;
    uint8_t matchLeadMin;     // LAMP_MATCH: minuter före nedsläpp som listen tänds

    // Backoff: räknare för en version som inte går att installera.
    String  otaBadVersion;
    uint8_t otaBadCount;

    // Versionen som installerades och som enheten nu startar om i. Ligger kvar
    // tills den kvitterat sig frisk. Kör vi något annat vid nästa start har
    // bootloadern rullat tillbaka den, och då är det den här strängen som säger
    // oss vilken version som ska svartlistas — annars hämtas samma trasiga
    // release om igen var tolfte timme.
    String  otaPendingVersion;

    // Segerläget. Fönstret räknas från när lampan fick veta om vinsten, så det
    // går inte att räkna fram på nytt efter en omstart — sluttiden måste ligga
    // kvar i NVS. victoryGame är matchens nedsläppstid och används som identitet
    // så att samma vinst inte firas två gånger; den ligger kvar när fönstret
    // löpt ut, annars skulle nästa hämtning tända om det direkt.
    uint32_t victoryUntil;    // UTC epoch, 0 = ingen seger att fira
    uint32_t victoryGame;     // startUtc för matchen vi redan firat

    // Omstartsräknare. Omstartsorsaken ensam säger inte hur ofta: en lampa som
    // brownoutar var tionde minut såg likadan ut som en som gjort det en gång.
    // abnormalBoots nollas vid strömpåslag, så den räknar omstarter i följd sedan
    // lampan senast kopplades in — tillsammans med upptiden ger det frekvensen.
    // Matchen som pågår. En match som startat finns varken i upcoming-games
    // eller played-games, så en omstart mitt i matchen tappade den helt och
    // lampan stod i standby resten av kvällen. Sparas när matchfönstret öppnar.
    String   liveUuid;
    uint32_t liveStart;       // UTC epoch, 0 = ingen
    String   liveHome;
    String   liveAway;
    int8_t   liveScoreHome;   // högsta ställning vi sett, -1 = ingen
    int8_t   liveScoreAway;

    // Driftstatistik (telemetry.cpp). På från start, går att slå av i setup-
    // portalen och på admin-sidan. lampId slumpas vid första starten och är
    // det enda som identifierar lampan utåt — inte MAC, inte SSID. Det rörs
    // inte av "Glöm WiFi", så en lampa som byter nät är fortfarande samma lampa.
    bool    telemetry;
    String  lampId;           // 16 hex-tecken
    String  lampName;         // valfritt smeknamn, t.ex. "Mamma"

    uint32_t bootCount;       // alla starter, sedan lampan först flashades
    uint32_t abnormalBoots;   // onormala omstarter sedan senaste strömpåslag

    void load();
    void noteBoot(bool poweredOn, bool abnormal);
    void save();
    void clearWifi();
    void noteLiveGame(const String &uuid, uint32_t startUtc,
                      const String &home, const String &away);
    void noteLiveScore(int home, int away);
    void noteVictory(uint32_t gameStartUtc, uint32_t untilUtc);
    void clearVictory();
    void noteOtaFailure(const String &version);
    void noteOtaRollback(const String &version);
    void noteOtaPending(const String &version);
    void clearOtaPending();
    void clearOtaFailures();
    bool otaBlocked(const String &version) const;
    bool hasWifi() const { return wifiSsid.length() > 0; }
};

extern Settings settings;
