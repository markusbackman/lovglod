// ─────────────────────────────────────────────────────────────────────────────
//  Kopplingstest
//
//  Egen firmware som bara testar hårdvaran — inget WiFi, ingen SHL-data.
//  Varje fas svarar på en specifik fråga, och seriemonitorn berättar vad du
//  ska se. Stämmer det inte överens vet du exakt vad som är fel.
//
//  Flasha:   pio run -e esp32dev_wiring -t upload
//  Titta:    pio device monitor
//  Tillbaka: pio run -e esp32dev -t upload
//
//  Båda listtyperna drivs samtidigt — WS2812B på sin pinne, APA102 på sina
//  två — så testet fungerar vilken list som än är inkopplad.
//
//  Annat antal, utan att röra config.h:
//    PLATFORMIO_BUILD_FLAGS="-DLED_COUNT=30" pio run -e esp32dev_wiring -t upload
// ─────────────────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include <FastLED.h>
#include "config.h"

#ifndef LED_COUNT
#define LED_COUNT LED_COUNT_DEFAULT
#endif

// Strömtak. Lågt som standard: räcker för att driva listen från USB ensamt
// utan att kortet browner ut. Har du externt nätaggregat inkopplat kan du
// höja med -DTEST_MAX_MA=3000.
#ifndef TEST_MAX_MA
#define TEST_MAX_MA 450
#endif

static CRGB leds[LED_COUNT];

static uint8_t  gPhase     = 0;
static uint32_t gPhaseAt   = 0;
static bool     gAnnounced = false;

static const uint8_t PHASE_COUNT = 6;

// ── Hjälpare ────────────────────────────────────────────────────────────────
// Samma gula som i den riktiga firmwaren: fast RGB-blandning, bara styrkan
// varierar. Se gold() i src/leds.cpp för varför den inte är en HSV-nyans.
static CRGB gold(uint8_t val) {
    const uint8_t v = scale8_video(val, val);
    return CRGB(scale8_video(YELLOW_R, v),
                scale8_video(YELLOW_G, v),
                scale8_video(YELLOW_B, v));
}

static void say(const char *what, const char *expect) {
    Serial.println();
    Serial.printf("── FAS %u/%u — %s\n", gPhase + 1, PHASE_COUNT, what);
    Serial.printf("   Du ska se: %s\n", expect);
}

static void nextPhase() {
    gPhase     = (gPhase + 1) % PHASE_COUNT;
    gPhaseAt   = millis();
    gAnnounced = false;
    FastLED.clear(true);
}

// ── Fas 1: vandrande punkt ──────────────────────────────────────────────────
// Svarar på: är datapinnen rätt? Hur många lysdioder finns? Åt vilket håll?
static void phaseWalk(uint32_t elapsed) {
    if (!gAnnounced) {
        say("Vandrande punkt",
            "EN vit punkt som långsamt går från listens ena ände till den andra");
        Serial.println("   → Inget alls lyser?  Fel pinne, ingen ström, eller listen kopplad i");
        Serial.println("      utgångsänden (DO/CO i stället för DIN, DI/CI).");
        Serial.println("   → Punkten stannar halvvägs?  Skadad LED eller glapp där den stannar.");
        Serial.println("   → Räkna dioderna den passerar — det är ditt LED_COUNT.");
        gAnnounced = true;
    }

    const uint16_t pos = (elapsed / 80) % LED_COUNT;
    fadeToBlackBy(leds, LED_COUNT, 60);
    leds[pos] = CRGB::White;

    static int16_t lastReported = -1;
    if ((int16_t)pos != lastReported && pos % 10 == 0) {
        lastReported = pos;
        Serial.printf("   punkt vid index %u\n", pos);
    }
}

// ── Fas 2: färgordning ──────────────────────────────────────────────────────
// Svarar på: stämmer LED_COLOR_ORDER? Byts rött och grönt är den fel.
static void phaseColors(uint32_t elapsed) {
    if (!gAnnounced) {
        say("Färgtest", "hela listen RÖD, sedan GRÖN, sedan BLÅ — 2 sekunder var");
        Serial.println("   → Fel färger?  Ändra LED_WS2812_ORDER (nu GRB) eller");
        Serial.println("      LED_APA102_ORDER (nu BGR) i config.h");
        Serial.println("   → Färgerna flimrar eller är slumpmässiga?  Signalproblem:");
        Serial.println("      kortare datakabel, motstånd 330–470 Ω, eller nivåomvandlare.");
        gAnnounced = true;
    }

    const uint8_t step = (elapsed / 2000) % 3;
    static int8_t announced = -1;
    if (step != announced) {
        announced = step;
        Serial.printf("   nu ska den vara: %s\n", step == 0 ? "RÖD" : step == 1 ? "GRÖN" : "BLÅ");
    }
    fill_solid(leds, LED_COUNT, step == 0 ? CRGB::Red : step == 1 ? CRGB::Green : CRGB::Blue);
}

// ── Fas 3: räkna dioder ─────────────────────────────────────────────────────
// Svarar på: stämmer LED_COUNT? Var tionde diod är röd som måttstock.
static void phaseCount(uint32_t) {
    if (!gAnnounced) {
        say("Räkna dioder",
            "hela listen svagt vit, med var TIONDE diod röd (index 0, 10, 20 …)");
        Serial.printf("   Firmware tror att listen har %d dioder.\n", LED_COUNT);
        Serial.println("   → Mörk svans i slutet?  LED_COUNT är för högt.");
        Serial.println("   → Släckta dioder efter sista röda?  LED_COUNT är för lågt.");
        gAnnounced = true;
    }

    for (uint16_t i = 0; i < LED_COUNT; i++)
        leds[i] = (i % 10 == 0) ? CRGB(60, 0, 0) : CRGB(18, 18, 18);
}

// ── Fas 4: strömtest ────────────────────────────────────────────────────────
// Svarar på: orkar strömförsörjningen? Ramp upp och se om kortet startar om.
static void phasePower(uint32_t elapsed) {
    if (!gAnnounced) {
        say("Strömtest", "listen lyser vitt och blir gradvis ljusare, sedan mörkare igen");
        Serial.printf("   Strömtaket är satt till %d mA (FastLED dimmar för att hålla det).\n",
                      TEST_MAX_MA);
        Serial.println("   → Startar kortet om här?  Otillräcklig ström — koppla in nätaggregat.");
        Serial.println("   → Rosa/gula toner istället för vitt?  Spänningsfall längs listen,");
        Serial.println("      mata 5 V även i listens andra ände.");
        gAnnounced = true;
    }

    const uint8_t v = triwave8((elapsed / 24) & 0xFF);
    fill_solid(leds, LED_COUNT, CRGB(v, v, v));
}

// ── Fas 5: rörelse ──────────────────────────────────────────────────────────
// Svarar på: hänger timingen med? Regnbågen ska flyta, inte hacka.
static void phaseRainbow(uint32_t elapsed) {
    if (!gAnnounced) {
        say("Rörelsetest", "en mjuk regnbåge som flyter längs listen");
        Serial.println("   → Hackar eller ryser den?  Signalstörning eller för lång datakabel.");
        gAnnounced = true;
    }
    fill_rainbow(leds, LED_COUNT, (uint8_t)(elapsed / 20), 255 / LED_COUNT + 1);
}

// ── Fas 6: förhandsvisning ──────────────────────────────────────────────────
// Det riktiga beteendet, så du ser hur lampan kommer att se ut.
static void phasePreview(uint32_t elapsed) {
    if (!gAnnounced) {
        say("Förhandsvisning",
            "Björklövens gula glöd med gnistor — och ett målfyrverkeri var 8:e sekund");
        gAnnounced = true;
    }

    // Långsam glöd, samma matematik som i den riktiga firmwaren: 16-bitars
    // sinus med gammakurva, och GLOW_MIN_VAL som hårt golv.
    const uint16_t phase  = beatsin16(GLOW_BPM, 0, 65535);
    const uint16_t eased  = ((uint32_t)phase * phase) >> 16;
    const uint16_t span   = GLOW_MAX_VAL - GLOW_MIN_VAL;
    const uint8_t  breath = GLOW_MIN_VAL + (uint8_t)(((uint32_t)eased * span + 32768) >> 16);
    const uint16_t t      = millis() / 24;
    for (uint16_t i = 0; i < LED_COUNT; i++) {
        const int16_t delta = ((int16_t)inoise8(i * 26, t) - 128) * breath / 700;
        const int16_t val   = constrain((int16_t)breath + delta, (int16_t)GLOW_MIN_VAL, 255);
        leds[i] = gold((uint8_t)val);
    }

    // Gnistor
    static uint8_t  sparkle[LED_COUNT];
    static uint32_t nextSparkle = 0;
    if ((int32_t)(millis() - nextSparkle) >= 0) {
        sparkle[random16(LED_COUNT)] = 255;
        nextSparkle = millis() + random16(300, 900);
    }
    for (uint16_t i = 0; i < LED_COUNT; i++) {
        if (!sparkle[i]) continue;
        leds[i]    = blend(leds[i], CRGB(255, 250, 225), sparkle[i]);
        sparkle[i] = qsub8(sparkle[i], SPARKLE_DECAY);
    }

    // Målfyrverkeri: 3 s av varje 8 s
    const uint32_t cyc = elapsed % 8000;
    if (cyc > 5000) {
        const uint32_t g = cyc - 5000;
        static bool told = false;
        if (g < 60 && !told) { Serial.println("   MÅL!"); told = true; }
        if (g > 2000) told = false;

        if (g < 900) {
            fill_solid(leds, LED_COUNT,
                       ((millis() / 36) % 2) ? CRGB(255, 244, 210) : gold(7));
        } else {
            fadeToBlackBy(leds, LED_COUNT, 48);
            const uint16_t center = LED_COUNT / 2;
            const uint16_t travel = ((g - 900) % 110) * center / 110;
            for (int8_t dir = -1; dir <= 1; dir += 2) {
                const int16_t pos = center + dir * (int16_t)travel;
                if (pos >= 0 && pos < LED_COUNT) leds[pos] = CRGB(255, 248, 220);
            }
            for (uint8_t k = 0; k < 3; k++)
                if (random8() < 90) leds[random16(LED_COUNT)] += gold(190);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(400);

    Serial.println("\n\n╔══════════════════════════════════════════════╗");
    Serial.println("║  LövGlöd — KOPPLINGSTEST                     ║");
    Serial.println("╚══════════════════════════════════════════════╝");
    Serial.printf("  WS2812B       DIN på GPIO%d\n", LED_WS2812_PIN);
    Serial.printf("  APA102        DI på GPIO%d, CI på GPIO%d\n", LED_APA102_DATA, LED_APA102_CLOCK);
    Serial.println("                (båda drivs samtidigt — koppla in den list du har)");
    Serial.printf("  Antal dioder  %d\n", LED_COUNT);
    Serial.printf("  Strömtak      %d mA\n", TEST_MAX_MA);
    Serial.println("  ────────────────────────────────────────────");
    Serial.println("  Tryck ENTER i monitorn för att hoppa till nästa fas.");
    Serial.println("  Varje fas kör 12 sekunder och loopar sedan runt.");

    FastLED.addLeds<WS2812B, LED_WS2812_PIN, LED_WS2812_ORDER>(leds, LED_COUNT)
        .setCorrection(LED_COLOR_CORRECTION);
    FastLED.addLeds<APA102, LED_APA102_DATA, LED_APA102_CLOCK, LED_APA102_ORDER,
                    DATA_RATE_MHZ(LED_APA102_MHZ)>(leds, LED_COUNT)
        .setCorrection(LED_COLOR_CORRECTION);
    // FastLED räknar strömmen för varje drivrutin för sig, fast de delar buffert.
    // Med en list inkopplad hamnar det verkliga taket därför på TEST_MAX_MA
    // först när budgeten dubblas.
    FastLED.setMaxPowerInVoltsAndMilliamps(LED_PSU_VOLTS, TEST_MAX_MA * 2);
    FastLED.setDither(LED_DITHER);
    FastLED.setBrightness(255);   // strömtaket sköter begränsningen
    FastLED.clear(true);

    gPhaseAt = millis();
}

void loop() {
    // ENTER i monitorn hoppar vidare
    if (Serial.available()) {
        while (Serial.available()) Serial.read();
        Serial.println("   → hoppar till nästa fas");
        nextPhase();
    }

    if (millis() - gPhaseAt > 12000) nextPhase();

    const uint32_t elapsed = millis() - gPhaseAt;
    switch (gPhase) {
        case 0: phaseWalk(elapsed);    break;
        case 1: phaseColors(elapsed);  break;
        case 2: phaseCount(elapsed);   break;
        case 3: phasePower(elapsed);   break;
        case 4: phaseRainbow(elapsed); break;
        case 5: phasePreview(elapsed); break;
    }

    FastLED.show();
    delay(8);
}
