#pragma once
#include <Arduino.h>

// Persistent konfiguration i NVS. Överlever omstart och OTA-uppdatering.
struct Settings {
    String  wifiSsid;
    String  wifiPass;
    String  otaSource;        // "owner/repo" eller manifest-URL. Tom = av.
    String  otaToken;         // GitHub-PAT. Krävs bara för privata repon.
    uint8_t brightness;
    bool    goalOnlyOurTeam;
    bool    debugPush;        // Ta emot matchläge på POST /push. Av = lampan
                              // hämtar bara från SHL och ignorerar nätet.
    uint8_t goalDelayS;       // TV-fördröjning i sekunder. 0 = tänd direkt.

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

    void load();
    void save();
    void clearWifi();
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
