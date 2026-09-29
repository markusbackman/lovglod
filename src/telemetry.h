#pragma once
#include <Arduino.h>

// Driftstatistik. Lampan skickar en liten hälsorapport till en Cloudflare
// Worker (tools/telemetri/) en stund efter uppkoppling och sedan var sjätte
// timme, med de händelser som köats sedan förra gången.
//
// Tre regler styr allt här:
//   · Aldrig under en match. Allt köas och går iväg efter slutsignalen.
//   · Misslyckas det ger vi upp tyst och försöker igen senare.
//   · Inget som pekar ut hushållet: inte SSID, inte IP, inte MAC. Lampan
//     identifieras av ett slumpat ID och ett smeknamn som ägaren själv sätter.
namespace Telemetry {

// Köar en händelse till nästa rapport. type är kort och maskinläsbar
// ("boot", "ota_ok", "ota_fail", "ota_rollback"), detail fritt men kort.
void event(const char *type, const String &detail = "");

// Körs varje varv i Online-läget. Skickar när det är dags och ingen match pågår.
void loop(bool inLiveWindow);

// Rapporten som den skulle se ut just nu. Visas på /debug så att ägaren kan se
// exakt vad som lämnar huset.
String preview();

// Text för /debug: senaste försöket och hur det gick.
const String &statusText();

// Sant om det finns en inbyggd mottagare. Utan den är inställningen verkningslös.
bool available();

}  // namespace Telemetry
