#pragma once
#include <Arduino.h>

namespace Updater {

// ArduinoOTA — push från PlatformIO över nätverket.
void beginPush();
void loop();

// Löser upp källan till en manifest-/API-URL.
//   "owner/repo"  → https://api.github.com/repos/owner/repo/releases/latest
//   "https://…"   → oförändrad
String resolveSourceUrl(const String &source);

// Hämtar senaste publicerade versionen utan att installera något.
// Returnerar false vid nätverks-/parsfel.
bool queryLatest(const String &source, String &version, String &binUrl,
                 uint32_t *assetId = nullptr);

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
