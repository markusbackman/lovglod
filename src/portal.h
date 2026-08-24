#pragma once
#include <Arduino.h>

// Text som statussidan visar. Fylls i av main.cpp.
struct StatusInfo {
    String state       = "Startar";
    String nextGame    = "—";
    String lastResult  = "—";
    String liveScore   = "—";
    bool   wonYesterday = false;
    bool   sseLive      = false;
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
}
