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
}
