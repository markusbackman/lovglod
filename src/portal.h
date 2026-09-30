#pragma once
#include <Arduino.h>

// Text som statussidan visar. Fylls i av main.cpp.
struct StatusInfo {
    String state       = "Startar";
    String nextGame    = "—";
    String lastResult  = "—";
    String liveScore   = "—";
    bool   sparkles     = false;   // gnistorna på: vinsten lyser till nästa match
    bool   sseLive      = false;
    bool   timeSynced   = false;   // utan klocka är matchläge och seger tyst av
    bool   otaOnTrial   = false;   // nyss installerad, inte kvitterad än
    String resetReason  = "okänd";  // varför enheten startade om sist
    bool   resetAbnormal = false;   // ...och om det var något att bry sig om
    bool   pushMode     = false;   // matchläget matas in via POST /push
    bool   inWindow     = false;   // matchfönstret öppet: nextGame är den som pågår
    bool   dark         = false;   // lampläget håller listen släckt just nu
    String demo;                   // demoläget som visas, tomt = av
};

// Push från mockservern: hela matchläget i ett anrop, istället för att lampan
// hämtar det själv. Enda vägen när en brandvägg i datorn stoppar inkommande
// anslutningar till testservern. Allt är valfritt — det som inte skickas
// lämnas orört.
struct PushState {
    bool   hasNext      = false;
    String homeCode;
    String awayCode;
    bool   homeIsUs     = false;
    String nextText;               // fritext till statussidan
    bool   live         = false;  // matchfönstret öppet
    bool   hasScore     = false;
    int    home         = -1;
    int    away         = -1;
    bool   hasLast      = false;
    bool   wonLast      = false;   // vann senaste matchen → gnistor

    String lastResult;

    // Uppspelning: ramar i strömmens format, som JSON-array. Tolkas i loopen
    // av Shl::injectFrame(), inte i webbhanteraren.
    bool   hasFrames    = false;
    String frames;
};

extern StatusInfo status;

namespace Portal {
// Startar AP + DNS-hijack + webbserver. Blockerar inte.
void startAccessPoint();
// Startar bara webbservern (station-läge) för status och inställningar.
void startStationServer();
void stop();
void loop();
bool isAccessPoint();
// true när användaren sparat nya uppgifter och enheten bör försöka ansluta.
bool credentialsSubmitted();
// Knappen "Hämta matchdata nu" — och automatiskt när datakällan bytts.
// Hämtningen tar sekunder och får inte ske inne i webbservern.
bool refreshRequested();
void clearRefresh();
// Senaste POST /push. Tas emot i webbservern men appliceras i loop(), av samma
// skäl som ovan: målfyrverkeriet ska inte starta inne i ett HTTP-anrop.
bool pushPending();
PushState takePush();
// Knapparna på /ljus: samma tangenter som demoläget i seriemonitorn. Tas emot
// i webbservern och spelas upp i loop(), som allt annat som tänder listen.
bool demoPending();
char takeDemo();
}
