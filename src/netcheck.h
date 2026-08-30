#pragma once
#include <Arduino.h>

// Nätverksdiagnostik. Skiljer på DNS, routing och portblockering, så att
// "det funkar inte" blir en konkret orsak. Körs vid uppstart och visas på
// enhetens /debug-sida.
namespace NetCheck {
// Anropas efter varje avklarat delprov. Proberna blockerar internt och går inte
// att avbryta, så det här är enda tillfället att rita om listen — mellan stegen,
// inte under dem.
using StepCb = void (*)(uint8_t done, uint8_t total);

// Antal delprov run() går igenom: 3 DNS-uppslag + 6 TCP-anslutningar.
const uint8_t STEP_TOTAL = 9;

// Kör alla test, skriver till Serial och returnerar rapporten.
const String &run(StepCb onStep = nullptr);
// Senaste rapporten utan att köra om.
const String &report();
}
