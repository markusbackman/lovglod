#pragma once
#include <Arduino.h>
#include "config.h"

enum LedMode : uint8_t {
    LED_BOOT,        // gult ljus som flödar in och blir stående
    LED_PORTAL,      // grön puls — inget WiFi sparat, eget nät uppe
    LED_PORTAL_RETRY,// röd puls — sparat WiFi men anslutningen misslyckades
    LED_CONNECTING,  // gult jagande ljus
    LED_WORKING,     // förloppsstapel under blockerande uppstartsarbete
    LED_STANDBY,     // långsam gul glöd (grundläget)
    LED_LIVE,        // matchen pågår: bärnstensglöd, slutspurt ovanpå när det är spännande
    LED_GOAL,        // MÅL! stroboskop och kometer, längre ju viktigare mål
    LED_VICTORY,     // vi vann: lugna kometer i timmar efteråt
    LED_UPDATING,    // förloppsindikator vid OTA
    LED_ERROR,       // rött andetag — ingen data
    LED_INTERMISSION,// paus: timglas som rinner ut över pausens antagna längd
    LED_OVERTIME,    // övertid och straffar: dragkamp mellan lagen
    LED_DANCE        // slutsignal, vi vann: segerdans, sedan segerläget
};

// Det listen behöver veta om matchen för att rita matchljuset. main.cpp räknar
// fram det ur ställningen och live-strömmen och lämnar över det varje varv.
struct MatchMood {
    uint8_t  intensity   = 0;     // 0–255, redan utjämnad — se MOOD_SMOOTH_MS
    int8_t   lead        = 0;     // >0 Björklöven leder, <0 ligger under
    int8_t   pressure    = 0;     // skottbalans -100–100, + = Björklöven trycker
    bool     usAtStart   = true;  // Björklövens sida är listens början (index 0)
    uint32_t pauseStartMs = 0;    // millis() när pausen började synas
    uint32_t pauseEstMs  = 0;     // pausens antagna längd
};

// Vilken list som sitter på. Värdet sparas i NVS — numrera aldrig om, lägg
// bara till.
enum LedStrip : uint8_t {
    LED_STRIP_UNSET  = 0,   // inte vald än: båda utgångarna drivs samtidigt
    LED_STRIP_WS2812 = 1,   // WS2812B/NeoPixel, en datatråd
    LED_STRIP_APA102 = 2,   // APA102/DotStar, data + klocka
};

// Listen ritas av en egen FreeRTOS-task, så att ett blockerande nätanrop i
// loop() inte längre fryser bilden. Allt nedan får anropas från loopTask som
// vanligt: rendertasken och funktionerna här delar ett och samma mutex, som
// varje funktion tar själv. Det är inte rekursivt — ingen funktion i
// namnrymden får därför anropa en annan publik funktion härifrån, den skulle
// låsa sig själv för gott.
namespace Leds {
// Registrerar drivrutiner för båda listorna och slår på den som hör till strip.
// Ritar ingenting — det gör tasken, och den startas först av start().
void begin(LedStrip strip, uint16_t count);

// Startar rendertasken. Skild från begin() för att setup() sätter ljusstyrka,
// mörkerläge och startläge först efter begin(). En task som redan ritade
// skulle hinna visa en bildruta på ljusstyrka 255 på en lampa som ska vara
// släckt — och det är just den strömlasten blank() nedan varnar för. Anropas
// en gång när listen är färdigställd; fler anrop gör ingenting.
void start();

// Minsta lediga stack rendertasken haft sedan start, i byte (0 innan
// start()). Finns för att LED_TASK_STACK ska kunna sänkas på mätdata i
// stället för på gissning — se config.h.
uint32_t stackFree();

// Byter list eller antal under drift, utan omstart. Omstart vore enklare men
// inte ofarligt: står en nyinstallerad OTA på prov rullar bootloadern tillbaka
// den vid nästa reset. Listen släcks först, så den som kopplas bort inte blir
// stående på sista bildrutan.
void configure(LedStrip strip, uint16_t count);
const char *stripName(LedStrip strip);

void setMode(LedMode m);
LedMode mode();
// Lägets namn som det står i docs/GLODEN.md, för statussidan.
const char *modeName(LedMode m);

// Gnistor läggs ovanpå glöden när laget vann sin senaste match; main.cpp
// håller dem tända fram till nästa nedsläpp. Segerläget glittrar alltid,
// oavsett den här flaggan — det vet redan att laget vann.
void setSparkles(bool on);
bool sparkles();

// Startar/förlänger målfyrverkeriet. Flera mål i rad staplar på varandra.
// delayMs > 0 köar målet istället för att tända direkt — tv-sändningen ligger
// efter SHL:s live-data, och lampan ska inte avslöja målet innan det syns på
// skärmen. Kön hålls i tidsordning och töms av rendertasken.
//
// importance, 0–255, avgör hur stort fyrverkeriet blir: längd, stroboskop och
// täthet i salvorna. Se GOAL_MIN_MS och goalImportance() i main.cpp.
void triggerGoal(uint32_t delayMs = 0, uint8_t importance = GOAL_TEST_IMPORTANCE);

// Motståndarmål: listen suckar — faller ihop mot mörker och hämtar sig. Köas
// med samma tv-fördröjning som våra mål, så att den inte avslöjar något heller.
void sigh(uint32_t delayMs = 0);

// Matchläget för LED_LIVE, LED_INTERMISSION och LED_OVERTIME.
void setMood(const MatchMood &m);

// Segerdansen. Tar över som målfyrverkeriet och lämnar till segerläget.
void dance();

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

// Lampläget (Av / Bara match): tonar ner listen till svart över LAMP_FADE_MS,
// och upp igen. Effekterna fortsätter ritas under, så listen tänds mitt i det
// som pågår i stället för att börja om. Undantag som alltid syns: demoläget
// (någon tryckte på en knapp) och portalen (lampan behöver en människa).
// instant hoppar över toningen — vid start, innan första bildrutan.
void setDark(bool dark, bool instant = false);

// Uppstart, anslutning och förloppsstapeln syns trots setDark() så länge det
// här är på. main.cpp slår av det efter första hämtningen — en omstart eller
// ett WiFi-tapp mitt i natten ska inte tända en släckt lampa.
void setShowSetup(bool on);

void setBrightness(uint8_t b);
void setUpdateProgress(uint8_t percent);

// Förlopp under uppstartsarbete som blockerar (nätverkstest, första
// datahämtningen). Stapeln har ingen egen rörelse: stegen är få och långa, och
// ett huvud som står still mellan dem säger ärligt att vi väntar på ett svar —
// en animation skulle lova ett flöde som inte finns.
void setWorkProgress(uint8_t done, uint8_t total);

// Släcker listen och skriver ut den direkt från anroparens task, under listens
// lås, så ingen bildruta kan hamna emellan.
//
// Finns för ett enda syfte: strömlasten precis innan WiFi-radion startar. Vid
// det laget står listen kvar på uppstartsflödets sista bildruta — 60 dioder på
// gold(255), ~1,4 A vid standardljusstyrkan — och det är exakt den lasten som
// ligger på när radion drar sin första TX-burst. Enheten brownoutade på den.
//
// Att bara byta läge räcker inte: drawConnecting() inleder med
// fadeToBlackBy(28), vilket sänker en fulltänd list med ~11 % per bildruta. Den
// måste släckas, inte tonas.
void blank();
}
