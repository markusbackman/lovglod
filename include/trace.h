#pragma once
#include <Arduino.h>

// Diagnostikloggning för live-kedjan: varje SSE-ram, återanslutning, ställning
// och reservpollning skrivs till seriellporten. Bara i esp32dev_diag — i
// vanliga byggen försvinner anropen helt.
#ifdef LIVE_TRACE
#define TRACE(...) Serial.printf(__VA_ARGS__)
#else
#define TRACE(...) do {} while (0)
#endif
