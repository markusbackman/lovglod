#pragma once
#include <Arduino.h>

// ─────────────────────────────────────────────────────────────
//  Firmware
// ─────────────────────────────────────────────────────────────
// Sätts av CI vid release (-DFW_VERSION='"1.2.3"'). Lokala byggen får -dev,
// vilket alltid skiljer sig från en publicerad version och därför alltid
// uppdaterar vid första OTA-kollen.
#ifndef FW_VERSION
#define FW_VERSION      "1.0.0-dev"
#endif
#define DEVICE_HOSTNAME "bjorkloven-led"

// ─────────────────────────────────────────────────────────────
//  LED-hårdvara
// ─────────────────────────────────────────────────────────────
// #ifndef så att kopplingstestet kan prova andra pinnar/antal via build-flagga:
//   PLATFORMIO_BUILD_FLAGS="-DLED_PIN=5 -DLED_COUNT=30" pio run -e esp32dev_wiring -t upload
#ifndef LED_PIN
#define LED_PIN         13          // Datapinne till WS2812B (via 330–470 Ω)
#endif
#ifndef LED_COUNT
#define LED_COUNT       60
#endif
#define LED_TYPE        WS2812B
#define LED_COLOR_ORDER GRB

// Strömbudget. FastLED dimmar automatiskt så att detta tak hålls.
// 60 LEDs på full vit ≈ 3.6 A. 3000 mA ger marginal på ett 5 V/4 A-nät.
#define LED_MAX_MILLIAMPS 3000
#define LED_PSU_VOLTS     5

// Global ljusstyrka (0–255). Kan ändras i webbportalen.
#define LED_DEFAULT_BRIGHTNESS 160

// ─────────────────────────────────────────────────────────────
//  Färger
// ─────────────────────────────────────────────────────────────
// FastLED HSV-nyans. 64 = rent gult, 42 = bärnsten. 52 ≈ varmt "guldgult".
#define YELLOW_HUE      52
#define YELLOW_SAT      248

// Standby-glöd: andningen pendlar mellan dessa värden.
#define GLOW_MIN_VAL    18
#define GLOW_MAX_VAL    130
#define GLOW_BPM        6           // ~10 s per andetag

// Gnistor (när laget vann igår)
#define SPARKLE_MEAN_INTERVAL_MS 700  // ungefär en gnista var 0,7 s
#define SPARKLE_DECAY            14   // högre = kortare gnista

// Målfyrverkeri
#define GOAL_DURATION_MS 12000
#define GOAL_STROBE_MS   2500       // inledande stroboskop innan "skotten"

// ─────────────────────────────────────────────────────────────
//  Björklöven / SHL-API
// ─────────────────────────────────────────────────────────────
// Team-UUID för IF Björklöven i SHL:s eget API (verifierat 2026-08-24).
#define SHL_TEAM_UUID   "4519-4519Rdei6"
#define SHL_TEAM_CODE   "IFB"

#define SHL_API_HOST    "www.shl.se"
#define SHL_LIVE_HOST   "game-broadcaster.s8y.se"

// true  = fyra bara när Björklöven gör mål
// false = fyra vid alla mål i matchen
#define GOAL_ONLY_OUR_TEAM true

// ─────────────────────────────────────────────────────────────
//  Pollningsintervall
// ─────────────────────────────────────────────────────────────
#define POLL_SCHEDULE_MS   (6UL * 60 * 60 * 1000)  // spelschema + gårdagens resultat
#define POLL_LIVE_FALLBACK_MS (45UL * 1000)        // reserv-poll under pågående match
#define OTA_CHECK_MS       (12UL * 60 * 60 * 1000) // kolla efter ny firmware

// ─────────────────────────────────────────────────────────────
//  OTA-källa
// ─────────────────────────────────────────────────────────────
// Skrivs in i webbportalen. Två format stöds:
//   "markusbackman/bjorkloven-led"     → GitHub Releases (rekommenderat)
//   "https://.../firmware.json"        → eget manifest, valfri webbserver
// Privat repo kräver dessutom en token, se README.
#define OTA_DEFAULT_SOURCE "markusbackman/bjorkloven-led"

// Filnamnet på .bin-filen i releasen som ska installeras.
#define OTA_ASSET_NAME  "firmware.bin"

// Ge upp efter så här många misslyckade försök på samma version, så att en
// trasig release inte får lampan att ladda ner 1 MB var 12:e timme för alltid.
#define OTA_MAX_FAILURES 3

// Matchfönster: när enheten anses vara "på matchdag/live".
#define LIVE_WINDOW_PRE_MS  (15UL * 60 * 1000)     // öppna 15 min före nedsläpp
#define LIVE_WINDOW_POST_MS (4UL * 60 * 60 * 1000) // stäng 4 h efter start

// ─────────────────────────────────────────────────────────────
//  Nätverk
// ─────────────────────────────────────────────────────────────
#define AP_SSID_PREFIX  "Bjorkloven-Setup"
#define AP_PASSWORD     ""          // tomt = öppet nät (enklast för captive portal)
#define OTA_PASSWORD    "***"

// Hur länge vi försöker ansluta till sparat WiFi innan portalen startar.
#define WIFI_CONNECT_TIMEOUT_MS 25000
// Hur länge portalen står öppen innan enheten försöker igen med sparade uppgifter.
#define PORTAL_RETRY_MS (5UL * 60 * 1000)

// Tidszon Stockholm med automatisk sommartid.
#define TZ_STOCKHOLM "CET-1CEST,M3.5.0,M10.5.0/3"
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.google.com"
