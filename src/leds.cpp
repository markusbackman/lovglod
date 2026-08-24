#include <FastLED.h>
#include "leds.h"
#include "config.h"

namespace {

CRGB      leds[LED_COUNT];
uint8_t   sparkleLevel[LED_COUNT];   // separat lager så gnistor kan tona ut ovanpå glöden

LedMode   gMode          = LED_BOOT;
bool      gSparkles      = false;
uint32_t  gGoalUntil     = 0;        // millis() då fyrverkeriet ska sluta
uint32_t  gGoalStart     = 0;
uint8_t   gUpdatePercent = 0;
uint32_t  gModeSince     = 0;
uint32_t  gLastFrame     = 0;
uint32_t  gNextSparkle   = 0;

const uint8_t FPS = 120;

// Grundfärgen. Value fylls i av respektive effekt.
inline CRGB gold(uint8_t val) { return CHSV(YELLOW_HUE, YELLOW_SAT, val); }

// ── Långsam glöd ────────────────────────────────────────────────────────────
// beatsin8 ger andningen. Perlin-brus ovanpå gör att listen "lever" istället
// för att pulsera som en enda platt yta.
void drawGlow(uint8_t minVal, uint8_t maxVal, uint8_t bpm) {
    const uint8_t  breath = beatsin8(bpm, minVal, maxVal);
    const uint16_t t      = millis() / 24;

    for (uint16_t i = 0; i < LED_COUNT; i++) {
        // ±18 % variation per LED, långsamt vandrande längs listen
        const uint8_t n     = inoise8(i * 26, t);
        const int16_t delta = ((int16_t)n - 128) * breath / 700;
        int16_t       val   = (int16_t)breath + delta;
        val = constrain(val, 0, 255);

        // Nyansen vandrar ett par steg — bärnsten i botten, ljusare gult i toppen
        const uint8_t hue = YELLOW_HUE - 4 + (val >> 6);
        leds[i] = CHSV(hue, YELLOW_SAT, (uint8_t)val);
    }
}

// ── Gnistor ─────────────────────────────────────────────────────────────────
void updateSparkles() {
    if (gSparkles && (int32_t)(millis() - gNextSparkle) >= 0) {
        sparkleLevel[random16(LED_COUNT)] = 255;
        // Slumpad väntan runt medelvärdet ger ett oregelbundet, naturligt glitter
        gNextSparkle = millis() + random16(SPARKLE_MEAN_INTERVAL_MS / 2,
                                          SPARKLE_MEAN_INTERVAL_MS * 3 / 2);
    }

    for (uint16_t i = 0; i < LED_COUNT; i++) {
        if (!sparkleLevel[i]) continue;
        // Blanda in en kall vit topp — det är den som ger "glittret"
        leds[i] = blend(leds[i], CRGB(255, 250, 225), sparkleLevel[i]);
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
            fill_solid(leds, LED_COUNT, white ? CRGB(255, 244, 210) : gold(255));
        } else {
            fill_solid(leds, LED_COUNT, gold(40));
        }
        return;
    }

    fadeToBlackBy(leds, LED_COUNT, 48);

    // Kometer: en ny salva skjuts ut från mitten var 110:e ms.
    const uint16_t center = LED_COUNT / 2;
    const uint16_t travel = (elapsed % 110) * center / 110;

    for (int8_t dir = -1; dir <= 1; dir += 2) {
        const int16_t pos = center + dir * (int16_t)travel;
        if (pos < 0 || pos >= LED_COUNT) continue;
        leds[pos] = CRGB(255, 248, 220);                       // vitglödande kärna
        if (pos - dir >= 0 && pos - dir < LED_COUNT) leds[pos - dir] += gold(220);
        if (pos - 2 * dir >= 0 && pos - 2 * dir < LED_COUNT) leds[pos - 2 * dir] += gold(90);
    }

    // Slumpade "gnistregn" ovanpå så det inte blir mekaniskt
    for (uint8_t k = 0; k < 3; k++) {
        if (random8() < 90) leds[random16(LED_COUNT)] += gold(random8(160, 255));
    }

    // Sista sekunden tonar ner mot standby istället för att slockna tvärt
    const int32_t left = (int32_t)(gGoalUntil - millis());
    if (left < 1200 && left > 0) {
        nscale8(leds, LED_COUNT, (uint8_t)map(left, 0, 1200, 60, 255));
    }
}

// ── Övriga lägen ────────────────────────────────────────────────────────────
void drawPortal() {
    const uint8_t v = beatsin8(20, 25, 190);
    fill_solid(leds, LED_COUNT, CHSV(150, 220, v));   // lugnt blå/cyan
}

void drawConnecting() {
    fadeToBlackBy(leds, LED_COUNT, 28);
    const uint16_t pos = (millis() / 18) % LED_COUNT;
    leds[pos] = gold(255);
}

void drawBoot() {
    fadeToBlackBy(leds, LED_COUNT, 20);
    const uint32_t e = millis() - gModeSince;
    const uint16_t pos = map(constrain(e, 0, 900), 0, 900, 0, LED_COUNT - 1);
    leds[pos] = gold(255);
    if (pos > 0) leds[pos - 1] = gold(120);
}

void drawUpdating() {
    const uint16_t lit = (uint32_t)gUpdatePercent * LED_COUNT / 100;
    fill_solid(leds, LED_COUNT, CRGB::Black);
    for (uint16_t i = 0; i < lit && i < LED_COUNT; i++) leds[i] = gold(200);
    if (lit < LED_COUNT) leds[lit] = gold(beatsin8(120, 30, 255));
}

void drawError() {
    const uint8_t v = beatsin8(12, 6, 70);
    fill_solid(leds, LED_COUNT, CHSV(0, 230, v));
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
namespace Leds {

void begin() {
    FastLED.addLeds<LED_TYPE, LED_PIN, LED_COLOR_ORDER>(leds, LED_COUNT)
        .setCorrection(TypicalLEDStrip);
    FastLED.setMaxPowerInVoltsAndMilliamps(LED_PSU_VOLTS, LED_MAX_MILLIAMPS);
    FastLED.setDither(DISABLE_DITHER);   // dither flimrar synligt vid låg glöd
    FastLED.clear(true);
    memset(sparkleLevel, 0, sizeof(sparkleLevel));
    gModeSince = millis();
}

void setMode(LedMode m) {
    if (m == gMode) return;
    gMode      = m;
    gModeSince = millis();
}

LedMode mode() { return gMode; }

void setSparkles(bool on) { gSparkles = on; }
bool sparkles() { return gSparkles; }

void triggerGoal() {
    const uint32_t now = millis();
    // Nytt mål under pågående fyrverkeri: förläng istället för att starta om,
    // annars tappar man stroboskopet vid snabba 2-mål.
    if (gMode != LED_GOAL) gGoalStart = now;
    gGoalUntil = now + GOAL_DURATION_MS;
    setMode(LED_GOAL);
}

void setBrightness(uint8_t b) { FastLED.setBrightness(b); }

void setUpdateProgress(uint8_t percent) {
    gUpdatePercent = percent;
    setMode(LED_UPDATING);
}

void render() {
    const uint32_t now = millis();
    if (now - gLastFrame < (1000u / FPS)) return;
    gLastFrame = now;

    // Målfyrverkeriet tar slut av sig självt
    if (gMode == LED_GOAL && (int32_t)(now - gGoalUntil) >= 0) setMode(LED_STANDBY);

    switch (gMode) {
        case LED_BOOT:       drawBoot();       break;
        case LED_PORTAL:     drawPortal();     break;
        case LED_CONNECTING: drawConnecting(); break;
        case LED_UPDATING:   drawUpdating();   break;
        case LED_ERROR:      drawError();      break;
        case LED_GOAL:       drawGoal();       break;

        case LED_LIVE:
            // Under match: kortare andetag och högre botten — listen "väntar".
            drawGlow(GLOW_MIN_VAL + 18, GLOW_MAX_VAL + 40, GLOW_BPM * 2);
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
