#include <FastLED.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
#include "leds.h"
#include "trace.h"
#include "config.h"

namespace {

CRGB      leds[LED_COUNT_MAX];
CRGB      fx[LED_COUNT_MAX];             // slutspurten ritas här och blandas in över glöden
uint8_t   sparkleLevel[LED_COUNT_MAX];   // separat lager så gnistor kan tona ut ovanpå glöden
uint16_t  gCount = LED_COUNT_DEFAULT;    // dioder på den inkopplade listen

CLEDController *gWs2812 = nullptr;
CLEDController *gApa102 = nullptr;

LedMode   gMode          = LED_BOOT;
bool      gSparkles      = false;
uint32_t  gGoalUntil     = 0;        // millis() då fyrverkeriet ska sluta
uint32_t  gGoalStart     = 0;
uint8_t   gGoalImp       = GOAL_TEST_IMPORTANCE;  // pågående fyrverkeris vikt
uint8_t   gUpdatePercent = 0;
uint32_t  gModeSince     = 0;
uint32_t  gLastSparkle   = 0;
uint16_t  gSparkleGap    = 0;
uint16_t  gWorkLit       = 0;        // antal tända LEDs i uppstartsstapeln

// Mål som väntar på tv-fördröjningen, i tidsordning (tidigast först).
uint32_t  gPendingAt[GOAL_QUEUE_MAX];
uint8_t   gPendingImp[GOAL_QUEUE_MAX];
uint8_t   gPendingCount = 0;

// Matchljuset
MatchMood gMood;
uint32_t  gPrevFrame     = 0;        // för fasackumulatorerna nedan
float     gHeartPhase    = 0;        // varv, bara bråkdelen används
float     gWavePhase     = 0;
uint32_t  gSighStart     = 0;        // 0 = ingen suck
uint32_t  gSighAt        = 0;        // köad suck (tv-fördröjning)
bool      gSighPending   = false;

bool      gLocked     = false;      // demoläge: app-logiken får inte byta läge
LedMode   gLockedMode = LED_STANDBY;

bool      gDark       = false;      // lampläget vill ha listen släckt
bool      gShowSetup  = true;       // uppstart och anslutning syns ändå
float     gFade       = 1.0f;       // 0 = släckt, 1 = full, glider mot målet

SemaphoreHandle_t gLock = nullptr;
TaskHandle_t      gTask = nullptr;

// Listens lås. Varje publik funktion i Leds tar det, och rendertasken håller
// det genom hela bildrutan — även FastLED.show(), som tar 1–2 ms. Det går inte
// att släppa före utskriften: RMT-drivrutinens statiska räknare tål inte två
// show() samtidigt, så blank() och configure() måste vänta ut bildrutan.
//
// Ett vanligt mutex, inte rekursivt. Tar samma task det två gånger står den
// och väntar på sig själv för alltid, och FreeRTOS säger ingenting. Därför går
// alla interna vägar via de olåsta funktionerna (setModeImpl() med flera) och
// aldrig via det publika API:t.
struct Guard {
    Guard()  { xSemaphoreTake(gLock, portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(gLock); }
    Guard(const Guard &)            = delete;
    Guard &operator=(const Guard &) = delete;
};

// Lägesbyte förbi demolåset (gLocked). Används internt av demoläget och av
// målfyrverkeriet, som ska få tända även när ett läge står låst.
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
void drawGlow(uint8_t minVal, uint8_t maxVal, uint8_t bpm, uint8_t green = YELLOW_G,
              CRGB *out = leds) {
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

        out[i] = gold((uint8_t)val, green);
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
uint32_t goalDurationMs(uint8_t imp) {
    return GOAL_MIN_MS + (uint32_t)(GOAL_MAX_MS - GOAL_MIN_MS) * imp / 255;
}

void drawGoal() {
    const uint32_t elapsed = millis() - gGoalStart;
    const uint32_t strobe  = GOAL_STROBE_MIN_MS +
                             (uint32_t)(GOAL_STROBE_MAX_MS - GOAL_STROBE_MIN_MS) * gGoalImp / 255;
    const uint32_t volley  = GOAL_VOLLEY_SLOW_MS -
                             (uint32_t)(GOAL_VOLLEY_SLOW_MS - GOAL_VOLLEY_FAST_MS) * gGoalImp / 255;

    if (elapsed < strobe) {
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

    // Kometer: en ny salva skjuts ut från mitten varje `volley` ms — tätare
    // ju viktigare målet. De viktigaste skjuter varannan salva i lagets gröna.
    const uint16_t center = gCount / 2;
    const uint16_t travel = (elapsed % volley) * center / volley;
    const bool     green  = gGoalImp >= GOAL_TEAM_COLORS_AT && (elapsed / volley) % 2;
    const CRGB     head   = green ? CRGB(170, 255, 170) : CRGB(255, 248, 220);
    const CRGB     tail1  = green ? CRGB(0, 190, 20) : gold(190);
    const CRGB     tail2  = green ? CRGB(0, 32, 4)   : gold(32);

    for (int8_t dir = -1; dir <= 1; dir += 2) {
        const int16_t pos = center + dir * (int16_t)travel;
        if (pos < 0 || pos >= gCount) continue;
        leds[pos] = head;                                      // vitglödande kärna
        if (pos - dir >= 0 && pos - dir < gCount) leds[pos - dir] += tail1;
        if (pos - 2 * dir >= 0 && pos - 2 * dir < gCount) leds[pos - 2 * dir] += tail2;
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

// ── Matchljus ───────────────────────────────────────────────────────────────
// Lägena nedan drivs av gMood, som main.cpp räknar fram ur ställningen och
// live-strömmen. Inget av dem räknar sekunder mot en slutsignal — se
// "Matchljus" i config.h för varför det inte går.
//
// Flyttal rakt igenom. Det här är inte glöden, som bor i botten av skalan och
// behöver varje bit; här rör sig allt i övre halvan och ESP32:n har FPU.
inline float clampf(float x, float lo = 0, float hi = 1) { return x < lo ? lo : x > hi ? hi : x; }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float smoothf(float x) { x = clampf(x); return x * x * (3 - 2 * x); }
inline float sqf(float x) { return x * x; }

// En färg i vald styrka, samma princip som gold(): alla kanaler skalas lika.
inline CRGB level(CRGB c, float v) {
    return c.nscale8_video((uint8_t)clampf(v, 0, 255));
}

// Guld → orange → rött. Bara grönkanalen sänks, så nyansen står still i
// utfadningen av samma skäl som i gold().
CRGB heatColor(float h) {
    h = clampf(h);
    const float g = h < 0.5f ? lerpf(200, 110, h * 2) : lerpf(110, 22, (h - 0.5f) * 2);
    return CRGB(255, (uint8_t)g, 0);
}

// Mjuk ljusfläck på en position mellan dioderna, adderad.
void splat(CRGB *out, float x, CRGB c, float v, float w) {
    const int a = (int)floorf(x - 3 * w), b = (int)ceilf(x + 3 * w);
    for (int i = a; i <= b; i++) {
        if (i < 0 || i >= gCount) continue;
        out[i] += level(c, v * expf(-sqf((i - x) / w)));
    }
}

// Gnistor med vald täthet, i samma lager som vinstgnistorna. updateSparkles()
// ritar och tonar ut dem.
void spawnSparkles(float perSecond, float dt) {
    if (perSecond <= 0) return;
    if (random16() < (uint16_t)clampf(perSecond * dt * 65535, 0, 65535))
        sparkleLevel[random16(gCount)] = 255;
}

// Slutspurt vid jämnt läge: dubbelslag som sprider sig från mitten. Pulsen går
// från vilopuls mot HEART_MAX_BPM och färgen värms mot rött.
void drawHeart(CRGB *out, float I, float dt) {
    gHeartPhase += lerpf(HEART_REST_BPM, HEART_MAX_BPM, I) / 60.0f * dt;
    gHeartPhase -= floorf(gHeartPhase);
    const CRGB  col  = heatColor(lerpf(0.1f, MOOD_HEAT, I));
    const float c    = (gCount - 1) / 2.0f;
    const float base = lerpf(22, 34, I), peak = lerpf(110, 240, I);
    for (uint16_t i = 0; i < gCount; i++) {
        const float d = fabsf(i - c) / (gCount / 2.0f);
        float f = gHeartPhase - d * 0.15f;
        f -= floorf(f);
        const float pulse = expf(-sqf((f - 0.03f) / 0.035f)) + 0.55f * expf(-sqf((f - 0.2f) / 0.045f));
        out[i] = level(col, base + pulse * peak * (1 - 0.35f * d));
    }
}

// Slutspurt när Löven leder: guldglöd där gnistorna tätnar med intensiteten.
// Gnistorna läggs i sparkleLevel och syns därför ovanpå blandningen.
void drawGoldRain(CRGB *out, float I, float w, float dt) {
    drawGlow(40, (uint8_t)lerpf(110, 150, I), 20, 190, out);
    spawnSparkles(lerpf(1, 40, I) * w, dt);
}

// Slutspurt i underläge: vågor rullar från Lövens ände mot motståndarens,
// snabbare med intensiteten och ännu snabbare när Löven skjuter mest.
void drawAttack(CRGB *out, float I, float dt) {
    const float push = gMood.pressure > 0 ? gMood.pressure / 100.0f : 0;
    gWavePhase += lerpf(0.4f, 2.2f, I) * (1 + 0.8f * push) * dt;
    gWavePhase -= floorf(gWavePhase);
    const CRGB col = heatColor(lerpf(0.3f, MOOD_HEAT, I));
    for (uint16_t i = 0; i < gCount; i++) {
        float x = gCount > 1 ? (float)i / (gCount - 1) : 0;
        if (!gMood.usAtStart) x = 1 - x;
        const float s = (sinf(2 * PI * (x * 2.5f - gWavePhase)) + 1) / 2;
        out[i] = level(col, 18 + powf(s, 5) * lerpf(120, 240, I));
    }
}

void drawLive(float dt) {
    // Grunden är samma glöd som alltid: kortare andetag och högre botten än
    // standby, dragen åt bärnsten — listen "väntar".
    drawGlow(GLOW_MIN_VAL + GLOW_LIVE_FLOOR_LIFT, GLOW_MAX_VAL + GLOW_LIVE_LIFT,
             GLOW_BPM * 2, LIVE_YELLOW_G);

    const float I = gMood.intensity / 255.0f;
    const float w = smoothf((I - MOOD_THRESHOLD) / MOOD_FADE);
    if (w > 0.004f) {
        if      (gMood.lead > 0) drawGoldRain(fx, I, w, dt);
        else if (gMood.lead < 0) drawAttack(fx, I, dt);
        else                     drawHeart(fx, I, dt);
        const uint8_t amount = (uint8_t)(w * 255);
        for (uint16_t i = 0; i < gCount; i++) nblend(leds[i], fx[i], amount);
    }
    updateSparkles();
}

// Paus: en bärnstensstapel krymper mot mitten över pausens antagna längd, och
// korn faller ut mot kanterna. Längden står inte i datan, så när glaset runnit
// ut andas listen lugnt tills nästa period faktiskt rapporteras.
void drawIntermission() {
    const uint32_t now  = millis();
    const float    est  = gMood.pauseEstMs ? (float)gMood.pauseEstMs : 1000.0f * PAUSE_EST_S;
    const float    left = est - (float)(now - gMood.pauseStartMs);
    const float    c    = (gCount - 1) / 2.0f;

    if (left <= 0) {
        const float s = (sinf(2 * PI * now / 3000.0f) + 1) / 2;
        fill_solid(leds, gCount, gold((uint8_t)(25 + 45 * s * s), LIVE_YELLOW_G));
        return;
    }

    // Sista minuten pulserar stapeln — dags att hämta chipsen.
    const bool  soon  = left < 60000;
    const float pulse = soon ? (sinf(2 * PI * now / 1500.0f) + 1) / 2
                             : (sinf(2 * PI * now / 10000.0f) + 1) / 2 * 0.3f;
    const float v     = soon ? 55 + 70 * pulse : 45 + 25 * pulse;
    const float half  = left / est * gCount / 2;
    for (uint16_t i = 0; i < gCount; i++) {
        const float d = fabsf(i - c);
        const float val = d < half ? v : d < half + 1 ? lerpf(GLOW_FLOOR_VAL, v, half + 1 - d)
                                                      : GLOW_FLOOR_VAL;
        leds[i] = gold((uint8_t)val, LIVE_YELLOW_G);
    }

    const float age = (now % 1400) / 900.0f;
    if (age < 1 && half < gCount / 2.0f - 1) {
        const float x = half + age * age * (gCount / 2.0f - half);
        const CRGB  w(SPARKLE_R, SPARKLE_G, SPARKLE_B);
        splat(leds, c + x, w, 200 * (1 - age * 0.5f), 0.7f);
        splat(leds, c - x, w, 200 * (1 - age * 0.5f), 0.7f);
    }
}

// Övertid och straffar: guld från Lövens sida, motståndarens färg från den
// andra. Skottrycket flyttar gränsen — skjuter Löven mest tar guldet mark — och
// den darrar och gnistrar.
void drawOvertime() {
    const uint32_t now = millis();
    const float n1  = inoise8((uint16_t)(now * 0.0896f)) / 255.0f;
    const float n2  = inoise8((uint16_t)(now * 0.5376f) + 9000) / 255.0f;
    const float dir = gMood.usAtStart ? 1 : -1;
    const float bd  = gCount / 2.0f + dir * gMood.pressure / 100.0f * gCount * 0.32f +
                      (n1 - 0.5f) * gCount * 0.25f + (n2 - 0.5f) * gCount * 0.08f;
    const CRGB us(255, 190, 0), them(OPP_R, OPP_G, OPP_B);
    const CRGB first = gMood.usAtStart ? us : them, last = gMood.usAtStart ? them : us;
    for (uint16_t i = 0; i < gCount; i++) {
        const float s = clampf((i - bd) / 1.5f + 0.5f);
        leds[i] = level(blend(first, last, (uint8_t)(s * 255)), 100);
    }
    splat(leds, bd, CRGB(255, 250, 235), 230, 0.9f);
}

// Segerdansen: guld- och gröna block jagar utåt från mitten medan laget tackar
// publiken, och de sista sekunderna tonar över i segerläget.
void drawDance(float dt) {
    const uint32_t age = millis() - gModeSince;
    const float    c   = (gCount - 1) / 2.0f;
    const CRGB     green = CHSV(104, 255, 170);
    for (uint16_t i = 0; i < gCount; i++) {
        const int ph = (int)floorf((fabsf(i - c) - age / 1000.0f * 12) / 4);
        fx[i] = (ph & 1) ? gold(170, 190) : green;
    }
    spawnSparkles(20, dt);

    const uint32_t blendFrom = DANCE_MS - 5000;
    if (age < blendFrom) {
        memcpy(leds, fx, gCount * sizeof(CRGB));
        updateSparkles(/*force=*/true);
        return;
    }
    drawVictory();
    const uint8_t keep = 255 - (uint8_t)clampf((age - blendFrom) / 5000.0f * 255, 0, 255);
    for (uint16_t i = 0; i < gCount; i++) nblend(leds[i], fx[i], keep);
}

// Suck vid motståndarmål: allt faller ihop mot mörker och hämtar sig.
void applySigh() {
    if (!gSighStart) return;
    const uint32_t a = millis() - gSighStart;
    if (a >= SIGH_MS) { gSighStart = 0; return; }
    const float m = a < 400 ? lerpf(1, 0.05f, a / 400.0f)
                            : lerpf(0.05f, 1, smoothf((a - 400) / (float)(SIGH_MS - 400)));
    nscale8(leds, gCount, (uint8_t)(m * 255));
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
// Huvudet markerar steget som pågår just nu. Medvetet en stapel utan egen
// rörelse: stegen är få och långa, och mellan dem väntar lampan på ett svar.
// Det som står stilla ska se ut att stå stilla med flit.
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

// ── Olåsta interna ──────────────────────────────────────────────────────────
// Det publika API:ts kroppar, för anrop inifrån där låset redan hålls. Se Guard.
void setModeImpl(LedMode m) {
    if (gLocked) return;
    forceMode(m);
}

// Nytt mål under pågående fyrverkeri: förläng istället för att starta om,
// annars tappar man stroboskopet vid snabba 2-mål. Vikten får bara växa —
// en kvittering direkt efter ett vanligt mål ska få den stora varianten.
void fireGoal(uint32_t now, uint8_t importance) {
    const uint32_t until = now + goalDurationMs(importance);
    if (gMode != LED_GOAL) {
        gGoalStart = now;
        gGoalImp   = importance;
        gGoalUntil = until;
    } else {
        if (importance > gGoalImp) gGoalImp = importance;
        if ((int32_t)(until - gGoalUntil) > 0) gGoalUntil = until;
    }
    forceMode(LED_GOAL);
}

void startSigh() {
    gSighStart = millis() | 1;       // 0 betyder "ingen suck"
}

// ── Bildrutan ───────────────────────────────────────────────────────────────
// Anropas bara av rendertasken, med låset taget.
void renderFrame() {
    const uint32_t now = millis();
    // Klämman gör en stall — flashskrivning, en tappad takt — osynlig för
    // fasackumulatorerna: de tar ett normalt steg i stället för ett språng.
    const float dt = gPrevFrame ? std::min(now - gPrevFrame, 100u) / 1000.0f : 0;
    gPrevFrame = now;

    // Målfyrverkeriet och segerdansen tar slut av sig själva
    if (gMode == LED_GOAL && (int32_t)(now - gGoalUntil) >= 0)
        forceMode(gLocked ? gLockedMode : LED_STANDBY);
    if (gMode == LED_DANCE && now - gModeSince >= DANCE_MS)
        forceMode(gLocked ? gLockedMode : LED_VICTORY);

    if (gSighPending && (int32_t)(now - gSighAt) >= 0) {
        gSighPending = false;
        startSigh();
    }

    // Köade mål vars tv-fördröjning gått ut. Flera kan förfalla samma bildruta;
    // fireGoal() staplar dem då precis som två snabba mål i realtid.
    while (gPendingCount && (int32_t)(now - gPendingAt[0]) >= 0) {
        const uint8_t imp = gPendingImp[0];
        for (uint8_t i = 1; i < gPendingCount; i++) {
            gPendingAt[i - 1]  = gPendingAt[i];
            gPendingImp[i - 1] = gPendingImp[i];
        }
        gPendingCount--;
        TRACE("[led] köat mål tänds nu, vikt %u (%u kvar i kön)\n", imp, gPendingCount);
        fireGoal(now, imp);
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
        case LED_LIVE:       drawLive(dt);     break;
        case LED_INTERMISSION: drawIntermission(); break;
        case LED_OVERTIME:   drawOvertime();   break;
        case LED_DANCE:      drawDance(dt);    break;

        case LED_STANDBY:
        default:
            drawGlow(GLOW_MIN_VAL, GLOW_MAX_VAL, GLOW_BPM);
            updateSparkles();
            break;
    }

    // Sucken läggs över de lugna lägena, aldrig över ett fyrverkeri.
    if (gMode == LED_LIVE || gMode == LED_INTERMISSION || gMode == LED_OVERTIME ||
        gMode == LED_STANDBY)
        applySigh();

    // Lampläget skalar bara det som skickas ut — leds[] lämnas orört, flera
    // effekter bygger vidare på förra bildrutan.
    const bool setup = gMode == LED_PORTAL || gMode == LED_PORTAL_RETRY ||
                       (gShowSetup && (gMode == LED_BOOT || gMode == LED_CONNECTING ||
                                       gMode == LED_WORKING));
    const float target = gDark && !gLocked && !setup ? 0.0f : 1.0f;
    const float step   = dt * 1000.0f / LAMP_FADE_MS;
    gFade = target > gFade ? std::min(target, gFade + step) : std::max(target, gFade - step);

    FastLED.show(scale8(FastLED.getBrightness(), (uint8_t)(gFade * 255)));
}

// Rendertasken. Takten hålls av vTaskDelayUntil, inte av millis()-jämförelser,
// så bildrutorna ligger jämnt oavsett hur länge ritandet tog.
void renderTask(void *) {
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        { Guard g; renderFrame(); }
        esp_task_wdt_reset();
        // Efter en stall (flashskrivning, ett show() som fick ge upp) ligger
        // `last` långt efter. vTaskDelayUntil skulle då rita ikapp med en skur
        // bildrutor rygg mot rygg — det som gått förlorat är ändå förlorat.
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - last) > (int32_t)pdMS_TO_TICKS(50)) last = now;
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000 / FPS));
    }
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
    if (!gLock) gLock = xSemaphoreCreateMutex();

    gWs2812 = &FastLED.addLeds<WS2812B, LED_WS2812_PIN, LED_WS2812_ORDER>(leds, LED_COUNT_MAX);
    gApa102 = &FastLED.addLeds<APA102, LED_APA102_DATA, LED_APA102_CLOCK, LED_APA102_ORDER,
                               DATA_RATE_MHZ(LED_APA102_MHZ)>(leds, LED_COUNT_MAX);
    gWs2812->setCorrection(LED_COLOR_CORRECTION);
    gApa102->setCorrection(LED_COLOR_CORRECTION);
    applyStrip(strip, constrain(count, LED_COUNT_MIN, LED_COUNT_MAX));

    FastLED.setMaxPowerInVoltsAndMilliamps(LED_PSU_VOLTS, LED_MAX_MILLIAMPS);
    FastLED.setDither(LED_DITHER);       // mjukar upp glödens smala nivåband
    // Den första show() avgör var RMT-avbrottet allokeras, och den sker här i
    // loopTask — alltså på kärna 1, samma kärna som rendertasken sedan ritar på.
    FastLED.clear(true);
    memset(sparkleLevel, 0, sizeof(sparkleLevel));
    gModeSince = millis();
    Serial.printf("[led] %s, %u dioder\n", stripName(strip), gCount);
}

// Kärna 1 är loopTasks, och det är där RMT-avbrottet ligger; WiFi och lwIP har
// kärna 0 för sig själva. Prio 2, ett steg över loopTask, så att bildrutan går
// före även mitt i ett CPU-bundet TLS-handslag — den tar bara någon millisekund
// och lämnar sedan tillbaka kärnan i vTaskDelayUntil.
//
// Tasken prenumererar på vakthunden. Hänger show() trots tidsgränsen i
// platformio.ini blir det en task_wdt-omstart som syns i telemetrin, i stället
// för en lampa som står still och en loop() som tyst väntar på låset.
void start() {
    if (gTask) return;
    xTaskCreatePinnedToCore(renderTask, "leds", LED_TASK_STACK, nullptr, 2, &gTask, 1);
    if (esp_task_wdt_add(gTask) != ESP_OK)
        Serial.println("[led] rendertasken står utan vakthund");
}

// Utan lås: läser bara taskens egen bokföring.
uint32_t stackFree() {
    return gTask ? uxTaskGetStackHighWaterMark(gTask) : 0;
}

const char *modeName(LedMode m) {
    switch (m) {
        case LED_BOOT:         return "Uppstart — gult flödar in";
        case LED_PORTAL:       return "Setup — grön puls";
        case LED_PORTAL_RETRY: return "WiFi svarar inte — röd puls";
        case LED_CONNECTING:   return "Ansluter — gul punkt som jagar";
        case LED_WORKING:      return "Arbetar — gul stapel";
        case LED_STANDBY:      return "Standby — långsam gul glöd";
        case LED_LIVE:         return "Match pågår — bärnstensglöd";
        case LED_GOAL:         return "Mål! — fyrverkeri";
        case LED_VICTORY:      return "Vann senaste matchen — lugna kometer";
        case LED_UPDATING:     return "Uppdaterar — gul stapel som fylls";
        case LED_ERROR:        return "Ingen data — rött andetag";
        case LED_INTERMISSION: return "Paus — timglas";
        case LED_OVERTIME:     return "Övertid — dragkamp";
        case LED_DANCE:        return "Segerdans — slutsignal, vi vann";
    }
    return "?";
}

void configure(LedStrip strip, uint16_t count) {
    count = constrain(count, LED_COUNT_MIN, LED_COUNT_MAX);

    {
        Guard g;
        // Släck med den gamla uppsättningen innan bytet: en list som kopplas bort
        // fryser annars på sista bildrutan, och en list som kortas behåller svansen
        // tänd bortom det nya slutet.
        fill_solid(leds, LED_COUNT_MAX, CRGB::Black);
        FastLED.show();

        applyStrip(strip, count);
        memset(sparkleLevel, 0, sizeof(sparkleLevel));
    }
    Serial.printf("[led] byter till %s, %u dioder\n", stripName(strip), gCount);
}

const char *stripName(LedStrip strip) {
    switch (strip) {
        case LED_STRIP_WS2812: return "WS2812B";
        case LED_STRIP_APA102: return "APA102/DotStar";
        default:               return "inte vald";
    }
}

void setMode(LedMode m) { Guard g; setModeImpl(m); }

bool setModeIfIdle(LedMode m) {
    Guard g;
    if (gMode == LED_GOAL || gMode == LED_DANCE || gMode == LED_UPDATING) return false;
    setModeImpl(m);
    return true;
}

void lockMode(LedMode m) {
    Guard g;
    gLocked     = true;
    gLockedMode = m;
    gMode       = m;
    gModeSince  = millis();          // alltid om från början, även samma läge
}

void unlockMode() { Guard g; gLocked = false; }
bool locked()     { Guard g; return gLocked; }

LedMode mode() { Guard g; return gMode; }

void setSparkles(bool on) { Guard g; gSparkles = on; }
bool sparkles() { Guard g; return gSparkles; }

void triggerGoal(uint32_t delayMs, uint8_t importance) {
    Guard g;
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
                gPendingAt[i]  = gPendingAt[i - 1];
                gPendingImp[i] = gPendingImp[i - 1];
                i--;
            }
            gPendingAt[i]  = at;
            gPendingImp[i] = importance;
            gPendingCount++;
            return;
        }
    }

    fireGoal(now, importance);
}

void sigh(uint32_t delayMs) {
    Guard g;
    if (delayMs) {
        gSighAt      = millis() + delayMs;
        gSighPending = true;
    } else {
        startSigh();
    }
}

void setMood(const MatchMood &m) { Guard g; gMood = m; }

void dance() { Guard g; forceMode(LED_DANCE); }

uint8_t pendingGoals() { Guard g; return gPendingCount; }

uint32_t pendingGoalInMs() {
    Guard g;
    if (!gPendingCount) return 0;
    const int32_t left = (int32_t)(gPendingAt[0] - millis());
    return left > 0 ? (uint32_t)left : 0;
}

void clearPendingGoals() { Guard g; gPendingCount = 0; gSighPending = false; }

void setDark(bool dark, bool instant) {
    Guard g;
    gDark = dark;
    if (instant) gFade = dark ? 0.0f : 1.0f;
}
void setShowSetup(bool on)   { Guard g; gShowSetup = on; }

void setBrightness(uint8_t b) { Guard g; FastLED.setBrightness(b); }

void setUpdateProgress(uint8_t percent) {
    Guard g;
    gUpdatePercent = percent > 100 ? 100 : percent;
    setModeImpl(LED_UPDATING);
}

void setWorkProgress(uint8_t done, uint8_t total) {
    Guard g;
    if (done > total) done = total;
    // Förloppet skalas in i intervallet [WORK_MIN_LIT, gCount] i stället för
    // [0, gCount]. Nollförloppet ska synas som en stapel som just startat,
    // inte som en släckt list.
    gWorkLit = total
        ? BAR_MIN_LIT + (uint16_t)((uint32_t)done * (gCount - BAR_MIN_LIT) / total)
        : BAR_MIN_LIT;
    setModeImpl(LED_WORKING);
}

void blank() {
    Guard g;
    fill_solid(leds, gCount, CRGB::Black);
    FastLED.show();
}

}  // namespace Leds
