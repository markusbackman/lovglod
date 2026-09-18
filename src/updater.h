#pragma once
#include <Arduino.h>

namespace Updater {

// Löser upp källan till en manifest-/API-URL för en kanal.
//   "owner/repo", stabil → https://api.github.com/repos/owner/repo/releases/latest
//   "owner/repo", beta   → https://api.github.com/repos/owner/repo/releases?per_page=5
//   "https://…/x.json", stabil → oförändrad
//   "https://…/x.json", beta   → https://…/x-beta.json
//
// /releases/latest hoppar alltid över pre-releases. Beta läser listan i stället
// och tar den nyaste som inte är ett utkast — stabil eller pre-release — så att
// en beta-lampa följer med när en rc befordras till skarp release.
String resolveSourceUrl(const String &source, bool beta);

// Allt enheten behöver veta om en publicerad release — både för att kunna
// installera den och för att kunna vägra. sha256Hex och sigB64 kommer från
// manifestet (firmware.json), som CI lägger intill binären i releasen.
struct ReleaseInfo {
    String   version;
    String   binUrl;
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
