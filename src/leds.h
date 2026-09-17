#pragma once
#include <Arduino.h>

enum LedMode : uint8_t {
    LED_BOOT,        // gult ljus som flödar in och blir stående
    LED_PORTAL,      // grön puls — inget WiFi sparat, eget nät uppe
    LED_PORTAL_RETRY,// röd puls — sparat WiFi men anslutningen misslyckades
    LED_CONNECTING,  // gult jagande ljus
    LED_WORKING,     // förloppsstapel under blockerande uppstartsarbete
    LED_STANDBY,     // långsam gul glöd (grundläget)
    LED_LIVE,        // matchen pågår: snabbare glöd, dragen åt bärnsten
    LED_GOAL,        // MÅL! snabb gul eldgivning
    LED_VICTORY,     // vi vann: lugna kometer i timmar efteråt
    LED_UPDATING,    // förloppsindikator vid OTA
    LED_ERROR        // rött andetag — ingen data
};

// Vilken list som sitter på. Värdet sparas i NVS — numrera aldrig om, lägg
// bara till.
enum LedStrip : uint8_t {
    LED_STRIP_UNSET  = 0,   // inte vald än: båda utgångarna drivs samtidigt
    LED_STRIP_WS2812 = 1,   // WS2812B/NeoPixel, en datatråd
    LED_STRIP_APA102 = 2,   // APA102/DotStar, data + klocka
};

namespace Leds {
// Registrerar drivrutiner för båda listorna och slår på den som hör till strip.
void begin(LedStrip strip, uint16_t count);

// Byter list eller antal under drift, utan omstart. Omstart vore enklare men
// inte ofarligt: står en nyinstallerad OTA på prov rullar bootloadern tillbaka
// den vid nästa reset. Listen släcks först, så den som kopplas bort inte blir
// stående på sista bildrutan.
void configure(LedStrip strip, uint16_t count);
const char *stripName(LedStrip strip);

void setMode(LedMode m);
LedMode mode();

// Gnistor läggs ovanpå glöden när laget vann dagen innan. Segerläget glittrar
// alltid, oavsett den här flaggan — det vet redan att laget vann.
void setSparkles(bool on);
bool sparkles();

// Startar/förlänger målfyrverkeriet. Flera mål i rad staplar på varandra.
// delayMs > 0 köar målet istället för att tända direkt — tv-sändningen ligger
// efter SHL:s live-data, och lampan ska inte avslöja målet innan det syns på
// skärmen. Kön hålls i tidsordning och töms i render().
void triggerGoal(uint32_t delayMs = 0);

// Köade mål som ännu inte tänts, och tiden kvar till det första (0 om kön är
// tom). Statussidan visar dem så att en tyst list under match går att förklara.
uint8_t  pendingGoals();
uint32_t pendingGoalInMs();

// Slänger kön utan att tända. Används när matchfönstret stängs — ett mål som
// aldrig hann visas hör inte hemma i nästa match.
void clearPendingGoals();

// Demoläge. Låser läget så att app-logiken inte skriver över det som visas —
// annars återställer loop() valt läge till standby/live inom en bildruta.
// Målfyrverkeriet får fortfarande spelas, och faller tillbaka till det låsta
// läget när det tar slut. lockMode() startar alltid om läget från början, så
// ett anrop till redan låst läge spelar upp t.ex. uppstartssvepet igen.
void lockMode(LedMode m);
void unlockMode();
bool locked();

void setBrightness(uint8_t b);
void setUpdateProgress(uint8_t percent);

// Förlopp under uppstartsarbete som blockerar (nätverkstest, första
// datahämtningen). Anropen går inte att avbryta, så listen står stilla mellan
// stegen — en stapel som stannar mitt i ser avsiktlig ut, det gör inte en
// animation som fryser.
void setWorkProgress(uint8_t done, uint8_t total);

// Släcker listen och skriver ut den direkt, förbi bildrutegrinden.
//
// Finns för ett enda syfte: strömlasten precis innan WiFi-radion startar. Vid
// det laget står listen kvar på uppstartsflödets sista bildruta — 60 dioder på
// gold(255), ~1,4 A vid standardljusstyrkan — och det är exakt den lasten som
// ligger på när radion drar sin första TX-burst. Enheten brownoutade på den.
//
// Att bara byta läge räcker inte: drawConnecting() inleder med
// fadeToBlackBy(28), vilket sänker en fulltänd list med ~11 % per bildruta. Den
// måste släckas, inte tonas. Se B0 i PRODUKTIONSKLAR.md.
void blank();

// Anropas varje varv i loop(). Ritar bara om när det är dags för ny bildruta.
void render();

// Ritar om omedelbart, förbi bildrutegrinden. Behövs när nästa rad kod
// blockerar i sekunder — annars hinner stapeln inte visas innan den fryser.
void renderNow();
}
