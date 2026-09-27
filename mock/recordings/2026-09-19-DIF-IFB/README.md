# DIF–IFB 2026-09-19, inspelad till 0–1 i andra pausen

Säsongspremiären, och den första matchen som spelades in. Inspelningen är
ofullständig: den slutar 19:44 i andra pausen, och den inspelade strömmen hamnade
på en backend som låg långt efter — målet finns inte i den alls. Lampan, på en
egen anslutning, fick målet i tid. Det är därför inspelningen är sparad: den
visar hur olika två samtidiga anslutningar till samma ström kan vara.

Alla filer utom `lampa.jsonl` är gzippade. Tiderna är lokala i Stockholm.

| Fil | Vad |
|---|---|
| `sse_raw.log.gz` | En SSE-anslutning mot `game-broadcaster.s8y.se/live/game?gameUuid=wza53fczpy`, rå text rad för rad med tidsstämpel, chunk-längderna kvar. 3323 ramar, fyra anslutningar (bröts 18:47, 18:49 och 18:50). |
| `poll.log.gz` | `today-games` och `played-games` var 30:e sekund, längd och hash per svar |
| `serial.log.gz` | Lampan på 192.168.1.58, diag-bygge, tidsstämplad per rad |
| `lamp_http.log.gz` | Samma lampas statussida var 15:e sekund |
| `lampa.jsonl` | `sse_raw.log` städad av `tools/forbehandla.py` — se nedan innan du spelar upp den |

## Vad som hände

| Tid | Strömmen i inspelningen | Lampan |
|---|---|---|
| 18:03 | `ongoing`, klockan börjar på 1 00:00 | |
| 18:42:06 | | Ram med ställningen 0–1, `[MÅL] Björklöven!` |
| 18:42:21 | | Fyrverkeriet tänds efter 15 s tv-fördröjning |
| 18:42:22 | | USB-porten försvinner, lampan startar om med *strömpåslag* |
| 18:44–18:47 | Händelser 7–10 minuter efter att de hänt | |
| 18:48 | | `intermission`, första pausen, 0–1 |
| 18:50–18:51 | `unknown`, sedan `ongoing` igen efter återanslutning | Händelser 17–52 minuter sena, sorteras bort som för sena |
| 19:44 | Senaste klockramen är fortfarande 1 18:24; ställningen står på 0–0 | `intermission`, andra pausen, 0–1 |

Lampan startade om med strömpåslag också 18:47, 18:49 och 18:53. Loggen säger
inte varför.

## Vad inspelningen visar

- **Två anslutningar till samma ström kan få helt olika data.** Lampan fick
  målet 18:42; inspelningen fick aldrig någon målhändelse, och dess
  klockramar stannade i första perioden. Det här är bakgrunden till att
  lampan sorterar bort sena händelser och att senare inspelningar görs med
  tre samtidiga anslutningar.
- **`today-games` svarar 200 med noll byte** hela dagen, även under matchen.
- **`played-games` ändrades inte** under inspelningen.
- **Ramtyper:** `playerStatistics` 2132, `liveState` 659, `liveEvent` 280,
  `teamStatistics` 268, `gameTime` 125, `gamePeriod` 2. Inga `goal`-händelser.

## Spela upp

`lampa.jsonl` kommer ur den inspelade strömmen och har därför **inget mål**
och ingen paus. Uppspelad med `tools/spela_upp.py` ger den en match som står
på 0–0 i första perioden. Den duger för att se hur lampan hanterar en
eftersläpande ström, inte för att se matchljuset. För det, använd
`2026-09-24-OHK-IFB` eller `2026-09-26-IFB-FBK`.
