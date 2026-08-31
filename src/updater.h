#pragma once
#include <Arduino.h>

namespace Updater {

// Löser upp källan till en manifest-/API-URL.
//   "owner/repo"  → https://api.github.com/repos/owner/repo/releases/latest
//   "https://…"   → oförändrad
String resolveSourceUrl(const String &source);

// Allt enheten behöver veta om en publicerad release — både för att kunna
// installera den och för att kunna vägra. sha256Hex och sigB64 kommer från
// manifestet (firmware.json), som CI lägger intill binären i releasen.
struct ReleaseInfo {
    String   version;
    String   binUrl;
    uint32_t binAssetId = 0;   // GitHub-assetens id, behövs för privata repon
    String   sha256Hex;        // 64 hex-tecken över firmware.bin
    String   sigB64;           // signatur över samma binär, base64
    uint32_t size       = 0;   // 0 = manifestet uppgav ingen storlek
};

// Hämtar senaste publicerade versionen utan att installera något.
// Returnerar false vid nätverks-/parsfel.
bool queryLatest(const String &source, ReleaseInfo &out);

// Kollar och installerar om versionen skiljer sig. Returnerar true om en
// uppdatering startades — enheten startar då om av sig själv.
bool checkAndApply(const String &source);

// Begär en kontroll vid nästa varv i loop() (används av knappen i portalen,
// så att webbservern inte blockeras i 30 sekunder).
void requestCheck();
bool checkRequested();
void clearRequest();

// Text för statussidan.
const String &statusText();

}  // namespace Updater
