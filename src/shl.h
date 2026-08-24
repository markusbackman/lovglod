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

namespace Shl {

// ── Pollning (HTTPS GET mot www.shl.se) ────────────────────────────────────
// Nästa match i spelschemat. Returnerar false vid nätverks-/parsfel.
bool fetchNextGame(NextGame &out);

// Senast spelade matchen. Sätter wonYesterday=true om Björklöven vann en
// match vars lokala datum var igår.
bool fetchLastResult(bool &wonYesterday, String &summary);

// Reservväg under pågående match om SSE-strömmen inte ger något.
bool pollLiveScore(const String &gameUuid, LiveScore &out);

// ── Live-ström (Server-Sent Events mot game-broadcaster.s8y.se) ────────────
void sseStart(const String &gameUuid);
void sseStop();
bool sseConnected();

// Anropas varje varv i loop(). Returnerar true när en ny ställning lästs in.
bool ssePump(LiveScore &out);

// ── Felsökning ─────────────────────────────────────────────────────────────
// Senaste råa SSE-ramen (kapad). Visas på enhetens /debug-sida så att
// fältformatet kan verifieras när säsongen väl drar igång.
const String &lastRawFrame();
const String &lastError();

}  // namespace Shl
