#pragma once
#include <Arduino.h>

struct NextGame {
    bool    valid    = false;
    String  uuid;
    String  homeCode;
    String  awayCode;
    time_t  startUtc = 0;      // nedsläpp, UTC epoch
    bool    homeIsUs = false;
};

struct LiveScore {
    bool valid = false;
    int  home  = -1;
    int  away  = -1;
};

// Senast spelade matchen. Att den alls dyker upp i played-games betyder att den
// är slut — SHL har ingen "matchen tog slut"-händelse, och matchfönstret i
// main.cpp är bara en timer på fyra timmar efter nedsläpp.
struct LastResult {
    bool   valid        = false;
    bool   won          = false;   // Björklöven vann (gnistorna, se main.cpp)
    time_t startUtc     = 0;       // nedsläpp, används som matchens identitet
    String summary;
};

namespace Shl {

// ── Pollning (HTTPS GET mot www.shl.se) ────────────────────────────────────
// Nästa match i spelschemat. Returnerar false vid nätverks-/parsfel.
bool fetchNextGame(NextGame &out);

// Senast spelade matchen. Returnerar false vid nätverks-/parsfel.
bool fetchLastResult(LastResult &out);

// Matchen som pågår just nu, från klubbsajten. En match som startat finns
// varken i upcoming-games eller played-games, så det här är enda vägen för en
// lampa som startar mitt i en match. Returnerar false när ingen match pågår.
bool fetchOngoingGame(NextGame &out);

// Reservväg under pågående match om SSE-strömmen inte ger något.
bool pollLiveScore(const String &gameUuid, LiveScore &out);

// ── Live-ström (Server-Sent Events mot game-broadcaster.s8y.se) ────────────
void sseStart(const String &gameUuid, time_t startUtc);
void sseStop();
bool sseConnected();

// Anropas varje varv i loop(). Returnerar true när en ny ställning lästs in.
bool ssePump(LiveScore &out);

// ── Datakälla ──────────────────────────────────────────────────────────────
// Bas-URL:erna lampan hämtar från. Visas på felsökningssidan.
String apiBaseUrl();
String liveBaseUrl();

// ── Felsökning ─────────────────────────────────────────────────────────────
// Senaste råa SSE-ramen (kapad). Visas på enhetens /debug-sida så att
// fältformatet kan verifieras när säsongen väl drar igång.
const String &lastRawFrame();
const String &lastError();

// Räknare för diagnostikloggen.
uint32_t sseFrames();
uint32_t sseReconnects();
uint32_t sseComments();
uint32_t sseSilentMs();

}  // namespace Shl
