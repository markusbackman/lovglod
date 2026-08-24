#pragma once
#include <Arduino.h>

enum LedMode : uint8_t {
    LED_BOOT,        // kort uppstartssvep
    LED_PORTAL,      // blå puls — inget WiFi sparat, eget nät uppe
    LED_PORTAL_RETRY,// röd puls — sparat WiFi men anslutningen misslyckades
    LED_CONNECTING,  // gult jagande ljus
    LED_STANDBY,     // långsam gul glöd (grundläget)
    LED_LIVE,        // matchen pågår: samma glöd men lite piggare
    LED_GOAL,        // MÅL! snabb gul eldgivning
    LED_UPDATING,    // förloppsindikator vid OTA
    LED_ERROR        // rött andetag — ingen data
};

namespace Leds {
void begin();
void setMode(LedMode m);
LedMode mode();

// Gnistor läggs ovanpå glöden när laget vann dagen innan.
void setSparkles(bool on);
bool sparkles();

// Startar/förlänger målfyrverkeriet. Flera mål i rad staplar på varandra.
void triggerGoal();

void setBrightness(uint8_t b);
void setUpdateProgress(uint8_t percent);

// Anropas varje varv i loop(). Ritar bara om när det är dags för ny bildruta.
void render();
}
