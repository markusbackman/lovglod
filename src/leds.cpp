#include <FastLED.h>
#include "leds.h"
#include "config.h"

namespace {

CRGB      leds[LED_COUNT_MAX];
uint8_t   sparkleLevel[LED_COUNT_MAX];   // separat lager så gnistor kan tona ut ovanpå glöden
uint16_t  gCount = LED_COUNT_DEFAULT;    // dioder på den inkopplade listen

CLEDController *gWs2812 = nullptr;
CLEDController *gApa102 = nullptr;

LedMode   gMode          = LED_BOOT;
bool      gSparkles      = false;
uint32_t  gGoalUntil     = 0;        // millis() då fyrverkeriet ska sluta
uint32_t  gGoalStart     = 0;
uint8_t   gUpdatePercent = 0;
uint32_t  gModeSince     = 0;
uint32_t  gLastFrame     = 0;
uint32_t  gLastSparkle   = 0;
uint16_t  gSparkleGap    = 0;
uint16_t  gWorkLit       = 0;        // antal tända LEDs i uppstartsstapeln

// Mål som väntar på tv-fördröjningen, i tidsordning (tidigast först).
uint32_t  gPendingAt[GOAL_QUEUE_MAX];
uint8_t   gPendingCount = 0;

bool      gLocked     = false;      // demoläge: app-logiken får inte byta läge
LedMode   gLockedMode = LED_STANDBY;

// Lägesbyte förbi låset. Används internt av demoläget och av målfyrverkeriet,
// som ska få tända även när ett läge står låst.
inline void forceMode(LedMode m) {
    if (m == gMode) return;
    gMode      = m;
    gModeSince = millis();
}

const uint8_t FPS = 120;

// Grundfärgen: en och samma gula, i vald styrka.
//
// Röd och grön skalas med samma faktor och samma avrundning, så nyansen ligger
// still hela vägen från full styrka ner till nästan släckt — bara ljuset
// ändras. Det är därför färgen sitter i YELLOW_R/YELLOW_G och inte i en
// HSV-nyans: CHSV skalar visserligen också kanalerna, men grönkanalen ligger
// lägre och trunkeras till noll medan den röda fortfarande lyser, så varje
// utfadning slutar i orange och till sist rent rött.
// `level` är ljusstyrkan rakt av, 0-255. Ingen gammakurva här inne: den satt
// tidigare i gold() och åt upp upplösningen i botten, där glöden bor — ett
// femtiotal olika `val` mappade till samma utnivå. Kurvan ligger nu i
// drawGlow(), som räknar i 16 bitar och rundar av först på slutet.
// `green` gör det möjligt att dra nyansen åt bärnsten utan att tappa det som
// gör gold() värd besväret: röd och grön skalas fortfarande med samma faktor,
// så vilken nyans man än väljer står den still hela vägen ner.
inline CRGB gold(uint8_t level, uint8_t green = YELLOW_G) {
    return CRGB(scale8_video(YELLOW_R, level),
                scale8_video(green, level),
                scale8_video(YELLOW_B, level));
}

// ── Långsam glöd ────────────────────────────────────────────────────────────
// Andetaget är en sinus, Perlin-bruset ovanpå gör att listen "lever" istället
// för att pulsera som en enda platt yta.
//
// Hela räkningen görs i 16 bitar. Det är gammakurvan nedan som kräver det, inte
// spannets bredd: `eased` är fasen i kvadrat, och kring vändningen rör den sig
// knappt alls. Där mappas många bildrutor i rad till samma utnivå med
// beatsin8:s 8-bitarsupplösning, och andetaget hoppar sedan ett helt steg —
// precis det man ser som ryck, och precis i den nedre delen av kurvan där
// glöden tillbringar mest tid. beatsin16 med avrundning först på slutet ger en
// jämn ramp, och dithern (LED_DITHER) fyller i mellanlägena.
//
// Funktionen ritar alla glödlägen, och spannen skiljer sig kraftigt åt:
// standby 22-140, live 30-156, segerlägets bädd 26-90.
void drawGlow(uint8_t minVal, uint8_t maxVal, uint8_t bpm, uint8_t green = YELLOW_G) {
    // Gammakurvan läggs på sinusen, inte på utnivån. Ögat ser små
    // ljusskillnader i botten mycket tydligare än i toppen, så en rå sinus
    // känns som om den rusar genom den mörka halvan och dröjer i den ljusa.
    const uint16_t phase = beatsin16(bpm, 0, 65535);
    const uint16_t eased = ((uint32_t)phase * phase) >> 16;

    const uint16_t span   = maxVal - minVal;
    const uint8_t  breath = minVal + (uint8_t)(((uint32_t)eased * span + 32768) >> 16);
    const uint16_t t      = millis() / 24;

    for (uint16_t i = 0; i < gCount; i++) {
        // ±18 % variation per LED, långsamt vandrande längs listen
        const uint8_t n     = inoise8(i * 26, t);
        const int16_t delta = ((int16_t)n - 128) * breath / 700;
        int16_t       val   = (int16_t)breath + delta;

        // Skyddsklämma mot hårdvarugolvet, inte mot andetagets botten. Bruset
        // får sänka enskilda dioder under minVal — det är det som håller
        // strukturen levande i den nedre vändningen — men aldrig ner i det
        // område där gulen bryts upp i rött. Se GLOW_FLOOR_VAL i config.h.
        val = constrain(val, (int16_t)GLOW_FLOOR_VAL, 255);

        leds[i] = gold((uint8_t)val, green);
    }
}

// ── Gnistor ─────────────────────────────────────────────────────────────────
// `force` tänder gnistorna oavsett gSparkles. Segerläget vill alltid glittra —
// det vet redan att laget vann, medan gSparkles står för vinsten som ligger
// kvar fram till nästa match.
void updateSparkles(bool force = false) {
    // Förfluten tid och inte en tidsstämpel framåt: gnistorna kan ha stått av i
    // månader, och en så gammal deadline ser ut att ligga i framtiden så fort
    // millis() passerat 2^31. Då uteblev glittret i upp till 25 dygn.
    if ((gSparkles || force) && millis() - gLastSparkle >= gSparkleGap) {
        sparkleLevel[random16(gCount)] = 255;
        // Slumpad väntan runt medelvärdet ger ett oregelbundet, naturligt glitter
        gLastSparkle = millis();
        gSparkleGap  = random16(SPARKLE_MEAN_INTERVAL_MS / 2,
                                SPARKLE_MEAN_INTERVAL_MS * 3 / 2);
    }

    for (uint16_t i = 0; i < gCount; i++) {
        if (!sparkleLevel[i]) continue;
        // Adderas ovanpå glöden, blandas inte in i den — se SPARKLE_R i
        // config.h. CRGB::operator+= mättar vid 255, så en gnista på full
        // styrka slår igenom mot vitt var i andetaget den än landar.
        leds[i] += CRGB(SPARKLE_R, SPARKLE_G, SPARKLE_B).nscale8(sparkleLevel[i]);
        sparkleLevel[i] = qsub8(sparkleLevel[i], SPARKLE_DECAY);
    }
}

// ── Målfyrverkeri ───────────────────────────────────────────────────────────
// Två faser: hårt stroboskop först, sedan "eldgivning" — kometer som skjuter
// ut från mitten åt båda håll med vitglödande kärna.
void drawGoal() {
    const uint32_t elapsed = millis() - gGoalStart;

    if (elapsed < GOAL_STROBE_MS) {
        // ~14 Hz. Varannan blixt vit, varannan mättat gul.
        const uint16_t phase = (millis() / 36) % 2;
        const bool     white = ((millis() / 72) % 2) == 0;
        if (phase) {
            fill_solid(leds, gCount, white ? CRGB(255, 244, 210) : gold(255));
        } else {
            fill_solid(leds, gCount, gold(7));
        }
        return;
    }

    fadeToBlackBy(leds, gCount, 48);

    // Kometer: en ny salva skjuts ut från mitten var 110:e ms.
    const uint16_t center = gCount / 2;
    const uint16_t travel = (elapsed % 110) * center / 110;

    for (int8_t dir = -1; dir <= 1; dir += 2) {
        const int16_t pos = center + dir * (int16_t)travel;
        if (pos < 0 || pos >= gCount) continue;
        leds[pos] = CRGB(255, 248, 220);                       // vitglödande kärna
        if (pos - dir >= 0 && pos - dir < gCount) leds[pos - dir] += gold(190);
        if (pos - 2 * dir >= 0 && pos - 2 * dir < gCount) leds[pos - 2 * dir] += gold(32);
    }

    // Slumpade "gnistregn" ovanpå så det inte blir mekaniskt
    for (uint8_t k = 0; k < 3; k++) {
        if (random8() < 90) leds[random16(gCount)] += gold(random8(101, 255));
    }

    // Sista sekunden tonar ner mot standby istället för att slockna tvärt
    const int32_t left = (int32_t)(gGoalUntil - millis());
    if (left < 1200 && left > 0) {
        nscale8(leds, gCount, (uint8_t)map(left, 0, 1200, 60, 255));
    }
}

// ── Segerläge ───────────────────────────────────────────────────────────────
// Målfyrverkeriets gest, uttänjd så att den går att leva med. Skillnaden mot
// drawGoal() är inte bara tempot: här ligger en glöd under kometerna, så listen
// vilar på något i stället för att falla mot svart mellan salvorna, och
// kometerna tonar ut på vägen ut i stället för att slå i kanten.
void drawVictory() {
    // Botten först — kometerna adderas ovanpå.
    drawGlow(VICTORY_GLOW_MIN, VICTORY_GLOW_MAX, VICTORY_GLOW_BPM, LIVE_YELLOW_G);

    const uint16_t center = gCount / 2;

    for (uint8_t v = 0; v < VICTORY_VOLLEYS; v++) {
        // Salvorna är samma resa förskjuten i tid, så strömmen aldrig tar slut.
        const uint32_t phase =
            (millis() + (uint32_t)v * VICTORY_TRAVEL_MS / VICTORY_VOLLEYS) % VICTORY_TRAVEL_MS;
        const uint16_t travel = (uint32_t)phase * center / VICTORY_TRAVEL_MS;

        // Kometen tonar ut längs resan i stället för att slockna vid kanten.
        const uint8_t life = 255 - (uint8_t)((uint32_t)phase * 255 / VICTORY_TRAVEL_MS);

        for (int8_t dir = -1; dir <= 1; dir += 2) {
            const int16_t pos = center + dir * (int16_t)travel;
            if (pos < 0 || pos >= gCount) continue;
            leds[pos] += gold(scale8(VICTORY_HEAD_VAL, life), LIVE_YELLOW_G);
            const int16_t tail = pos - dir;
            if (tail >= 0 && tail < gCount)
                leds[tail] += gold(scale8(VICTORY_TAIL_VAL, life), LIVE_YELLOW_G);
        }
    }

    updateSparkles(/*force=*/true);
}

// ── Övriga lägen ────────────────────────────────────────────────────────────
// Samma rytm och samma amplitud i båda portallägena — det är färgen som
// skiljer dem åt.
//   grön = "anslut till mitt nät"     röd = "ditt WiFi svarar inte"
//
// Mättnaden skiljer sig också, och inte av slarv: grönt måste köras på full
// mättnad, för den gnutta vitt som gör rött mjukare gör grönt mintfärgat och
// därmed omöjligt att läsa som lagets färg. Rött tål 235 och blir mindre platt
// av det.
void drawPortal(uint8_t hue, uint8_t sat) {
    const uint8_t v = beatsin8(20, 25, 190);
    fill_solid(leds, gCount, CHSV(hue, sat, v));
}

void drawConnecting() {
    fadeToBlackBy(leds, gCount, 28);
    const uint16_t pos = (millis() / 18) % gCount;
    leds[pos] = gold(255);
}

// Ljuset flödar in från ena änden och blir stående. Vid 700 ms lyser hela
// listen, och det är det enda tillfället då varje diod syns tänd samtidigt —
// bättre diagnostik än ett svep, där en död diod bara ser ut som en lucka i
// efterglöden.
//
// Medvetet en flod och inte en stapel: förloppsstapeln (LED_WORKING) fyller
// också från noll, men med mörk kropp och ett ensamt ljust huvud. Här är
// kroppen full styrka och kanten mjuk, så de två går inte att förväxla. Och
// inte heller ett jagande ljus — LED_CONNECTING tar vid direkt efteråt, och
// två gula punkter med svans efter varandra läses som en enda animation.
//
// Kanten räknas i 8.8-fixpunkt (256 steg per diod) så att den kan tona mellan
// dioderna i stället för att hoppa en hel diod i taget.
void drawBoot() {
    const uint32_t e = millis() - gModeSince;

    // Kanten går 3 dioder förbi listens slut, annars hinner de sista aldrig
    // upp i full styrka innan tiden är ute.
    const int32_t edge = map(constrain(e, 0, BOOT_FILL_MS), 0, BOOT_FILL_MS,
                             0, gCount * 256 + BOOT_EDGE_SOFTNESS);

    for (uint16_t i = 0; i < gCount; i++) {
        const int32_t d = edge - (int32_t)i * 256;      // hur långt bakom kanten
        uint8_t level;
        if      (d <= 0)                    level = 0;
        else if (d >= BOOT_EDGE_SOFTNESS)   level = 255;
        else                                level = (uint8_t)(d * 255 / BOOT_EDGE_SOFTNESS);
        leds[i] = gold(level);
    }
}

// ── Uppstartsförlopp ────────────────────────────────────────────────────────
// Huvudet markerar steget som pågår just nu. Eftersom bildrutan ofta fryser
// mitt i ett blockerande anrop är det medvetet en stapel utan egen rörelse:
// det som står stilla ska se ut att stå stilla med flit.
void drawWorking() {
    fill_solid(leds, gCount, CRGB::Black);
    for (uint16_t i = 0; i < gWorkLit && i < gCount; i++) leds[i] = gold(WORK_BODY_VAL);
    if (gWorkLit < gCount) leds[gWorkLit] = gold(255);
}

// Till skillnad från drawWorking() får den här röra sig: en OTA-nedladdning är
// en ström, förloppet uppdateras kontinuerligt och processorn är ledig. Ett
// pulserande huvud betyder "data flödar"; ett stillastående betyder "väntar".
//
// Medvetet dämpad. Det här är den enda animationen som ritas medan radion tar
// emot kontinuerligt och flashen skrivs, och den växer dessutom mot full list
// just som nedladdningen är som längst gången. Se UPDATE_BODY_VAL i config.h.
void drawUpdating() {
    // Samma golv som uppstartsstapeln — se BAR_MIN_LIT i config.h.
    const uint16_t lit = BAR_MIN_LIT +
                         (uint16_t)((uint32_t)gUpdatePercent * (gCount - BAR_MIN_LIT) / 100);
    fill_solid(leds, gCount, CRGB::Black);
    for (uint16_t i = 0; i < lit && i < gCount; i++) leds[i] = gold(UPDATE_BODY_VAL);
    if (lit < gCount) leds[lit] = gold(beatsin8(120, 4, UPDATE_HEAD_VAL));
}

void drawError() {
    const uint8_t v = beatsin8(12, 6, 70);
    fill_solid(leds, gCount, CHSV(0, 230, v));
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
namespace Leds {

// Båda drivrutinerna är registrerade hela tiden; det är den här som avgör vilken
// som skriver ut. FastLED kan inte ta bort en drivrutin, så att byta list utan
// omstart går bara till så här.
//
// Den avstängda får dessutom längden noll. show() hoppar över avstängda, men
// strömtaket räknar ihop varje registrerad drivrutins buffert oavsett — med
// full längd på båda skulle budgeten i praktiken halveras. Så är det medan
// ingen list är vald, och det är ett rimligt pris för att portalen syns.
void applyStrip(LedStrip strip, uint16_t count) {
    const bool ws  = strip != LED_STRIP_APA102;
    const bool apa = strip != LED_STRIP_WS2812;
    gWs2812->setLeds(leds, ws ? count : 0);
    gWs2812->setEnabled(ws);
    gApa102->setLeds(leds, apa ? count : 0);
    gApa102->setEnabled(apa);
    gCount = count;
}

void begin(LedStrip strip, uint16_t count) {
    gWs2812 = &FastLED.addLeds<WS2812B, LED_WS2812_PIN, LED_WS2812_ORDER>(leds, LED_COUNT_MAX);
    gApa102 = &FastLED.addLeds<APA102, LED_APA102_DATA, LED_APA102_CLOCK, LED_APA102_ORDER,
                               DATA_RATE_MHZ(LED_APA102_MHZ)>(leds, LED_COUNT_MAX);
    gWs2812->setCorrection(LED_COLOR_CORRECTION);
    gApa102->setCorrection(LED_COLOR_CORRECTION);
    applyStrip(strip, constrain(count, LED_COUNT_MIN, LED_COUNT_MAX));

    FastLED.setMaxPowerInVoltsAndMilliamps(LED_PSU_VOLTS, LED_MAX_MILLIAMPS);
    FastLED.setDither(LED_DITHER);       // mjukar upp glödens smala nivåband
    FastLED.clear(true);
    memset(sparkleLevel, 0, sizeof(sparkleLevel));
    gModeSince = millis();
    Serial.printf("[led] %s, %u dioder\n", stripName(strip), gCount);
}

void configure(LedStrip strip, uint16_t count) {
    count = constrain(count, LED_COUNT_MIN, LED_COUNT_MAX);

    // Släck med den gamla uppsättningen innan bytet: en list som kopplas bort
    // fryser annars på sista bildrutan, och en list som kortas behåller svansen
    // tänd bortom det nya slutet.
    fill_solid(leds, LED_COUNT_MAX, CRGB::Black);
    FastLED.show();

    applyStrip(strip, count);
    memset(sparkleLevel, 0, sizeof(sparkleLevel));
    Serial.printf("[led] byter till %s, %u dioder\n", stripName(strip), gCount);
}

const char *stripName(LedStrip strip) {
    switch (strip) {
        case LED_STRIP_WS2812: return "WS2812B";
        case LED_STRIP_APA102: return "APA102/DotStar";
        default:               return "inte vald";
    }
}

void setMode(LedMode m) {
    if (gLocked) return;
    forceMode(m);
}

void lockMode(LedMode m) {
    gLocked     = true;
    gLockedMode = m;
    gMode       = m;
    gModeSince  = millis();          // alltid om från början, även samma läge
}

void unlockMode() { gLocked = false; }
bool locked()     { return gLocked; }

LedMode mode() { return gMode; }

void setSparkles(bool on) { gSparkles = on; }
bool sparkles() { return gSparkles; }

void triggerGoal(uint32_t delayMs) {
    const uint32_t now = millis();

    if (delayMs) {
        if (gPendingCount >= GOAL_QUEUE_MAX) {
            // Sex obesvarade mål inom fördröjningen händer inte i en hockeymatch.
            // Skulle det ändå ske är det bättre att tappa ett än att tappa kön.
            Serial.println("[led] målkön full — hoppar över fördröjningen");
            delayMs = 0;
        } else {
            const uint32_t at = now + delayMs;
            // Insättning i tidsordning. Fördröjningen kan ha ändrats mellan två
            // mål, så ankomstordningen är inte nödvändigtvis tidsordningen.
            uint8_t i = gPendingCount;
            while (i && (int32_t)(gPendingAt[i - 1] - at) > 0) {
                gPendingAt[i] = gPendingAt[i - 1];
                i--;
            }
            gPendingAt[i] = at;
            gPendingCount++;
            return;
        }
    }

    // Nytt mål under pågående fyrverkeri: förläng istället för att starta om,
    // annars tappar man stroboskopet vid snabba 2-mål.
    if (gMode != LED_GOAL) gGoalStart = now;
    gGoalUntil = now + GOAL_DURATION_MS;
    forceMode(LED_GOAL);
}

uint8_t pendingGoals() { return gPendingCount; }

uint32_t pendingGoalInMs() {
    if (!gPendingCount) return 0;
    const int32_t left = (int32_t)(gPendingAt[0] - millis());
    return left > 0 ? (uint32_t)left : 0;
}

void clearPendingGoals() { gPendingCount = 0; }

void setBrightness(uint8_t b) { FastLED.setBrightness(b); }

void setUpdateProgress(uint8_t percent) {
    gUpdatePercent = percent > 100 ? 100 : percent;
    setMode(LED_UPDATING);
}

void setWorkProgress(uint8_t done, uint8_t total) {
    if (done > total) done = total;
    // Förloppet skalas in i intervallet [WORK_MIN_LIT, gCount] i stället för
    // [0, gCount]. Nollförloppet ska synas som en stapel som just startat,
    // inte som en släckt list.
    gWorkLit = total
        ? BAR_MIN_LIT + (uint16_t)((uint32_t)done * (gCount - BAR_MIN_LIT) / total)
        : BAR_MIN_LIT;
    setMode(LED_WORKING);
}

void blank() {
    fill_solid(leds, gCount, CRGB::Black);
    FastLED.show();
}

void renderNow() {
    gLastFrame = millis() - (1000u / FPS);
    render();
}

void render() {
    const uint32_t now = millis();
    if (now - gLastFrame < (1000u / FPS)) return;
    gLastFrame = now;

    // Målfyrverkeriet tar slut av sig självt
    if (gMode == LED_GOAL && (int32_t)(now - gGoalUntil) >= 0)
        forceMode(gLocked ? gLockedMode : LED_STANDBY);

    // Köade mål vars tv-fördröjning gått ut. Flera kan förfalla samma bildruta;
    // triggerGoal() staplar dem då precis som två snabba mål i realtid.
    while (gPendingCount && (int32_t)(now - gPendingAt[0]) >= 0) {
        for (uint8_t i = 1; i < gPendingCount; i++) gPendingAt[i - 1] = gPendingAt[i];
        gPendingCount--;
        triggerGoal();
    }

    switch (gMode) {
        case LED_BOOT:       drawBoot();       break;
        case LED_PORTAL:       drawPortal(104, 255); break;   // Björklövens gröna
        case LED_PORTAL_RETRY: drawPortal(0, 235);   break;   // röd
        case LED_CONNECTING: drawConnecting(); break;
        case LED_WORKING:    drawWorking();    break;
        case LED_UPDATING:   drawUpdating();   break;
        case LED_ERROR:      drawError();      break;
        case LED_GOAL:       drawGoal();       break;
        case LED_VICTORY:    drawVictory();    break;

        case LED_LIVE:
            // Under match: kortare andetag och högre botten — listen "väntar".
            drawGlow(GLOW_MIN_VAL + GLOW_LIVE_FLOOR_LIFT,
                     GLOW_MAX_VAL + GLOW_LIVE_LIFT, GLOW_BPM * 2,
                     LIVE_YELLOW_G);
            updateSparkles();
            break;

        case LED_STANDBY:
        default:
            drawGlow(GLOW_MIN_VAL, GLOW_MAX_VAL, GLOW_BPM);
            updateSparkles();
            break;
    }

    FastLED.show();
}

}  // namespace Leds
