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

    // Backoff: räknare för en version som inte går att installera.
    String  otaBadVersion;
    uint8_t otaBadCount;

    void load();
    void save();
    void clearWifi();
    void noteOtaFailure(const String &version);
    void clearOtaFailures();
    bool otaBlocked(const String &version) const;
    bool hasWifi() const { return wifiSsid.length() > 0; }
};

extern Settings settings;
