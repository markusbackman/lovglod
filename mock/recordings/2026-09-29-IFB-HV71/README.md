# IFB–HV71 2026-09-29, 4–2

SHL:s live-kedja inspelad från 18:46 till 21:17, fyra minuter efter
slutsignalen. Lampan på 192.168.1.99 var igång hela kvällen. Den började på
1.3.0-rc1, kraschade vid varje uppdateringskontroll och flashades med rc3 via
USB i andra pausen. Den har ingen `lamp_http.log`, men har en egen logg över
drifttid och heap, se nedan.

Alla filer är gzippade. Tiderna är lokala i Stockholm.

| Fil | Vad |
|---|---|
| `sse1–3.jsonl.gz` | Tre samtidiga SSE-anslutningar mot `game-broadcaster.s8y.se/live/game?gameUuid=o70ryaotag`. En JSON-rad per ram: `rx`, `id`, `event`, `data`. 3 978 ramar per ström. |
| `sse1–3.log.gz` | Samma ramar i läsbar form från `tools/live.py --rå` |
| `sse1–3.status` | Anslutningar och fel per ström. Alla tre anslöt 18:46:53 och tappade aldrig anslutningen. |
| `poll.log.gz` | `www.bjorkloven.com/api/gameday/game-overview/o70ryaotag` var 15:e sekund. Glapp 19:44:58–19:48:07, när inspelningen var pausad medan lampan flashades. |
| `http.jsonl.gz` | Hela svaret från game-overview, play-by-play, gameheader, upcoming-games, played-games och today-games, sparat bara när innehållet ändrats. En rad: `rx`, `src`, `status`, `ms`, `len`, `sha`, `data`. |
| `lamp99_debug.log.gz` | Lampans `/debug` var 15:e sekund, 18:54–21:17: drifttid, onormala omstarter, orsak till senaste omstart, heap, minsta heap, RSSI, SSE-återanslutningar, SHL-fel och senaste fel. `*** OMSTART ***` markerar när drifttiden gick bakåt. |
| `serial.log.gz` | Nästan tom. USB var inkopplat 19:42–19:51, men porten gav ingen läsbar utskrift. |
| `lampa.jsonl` | `tools/forbehandla.py` körd på inspelningen, 3 790 rader |

Nedsläpp 18:59:52 enligt `gamePeriod`. Perioderna slutade 19:31:09, 20:24:13
och 21:12:42. game-overview sa `GameEnded` 21:13:17.

## Målen

| Ställning | Lag | Gjordes | Registrerat av SHL | I strömmen | game-overview |
|---|---|---|---|---|---|
| 0–1 | HV71 | 19:05:58 | 19:09:11 (+193 s) | 19:09:27 (+16 s) | 19:09:39 |
| 1–1 | IFB | 19:10:43 | 19:11:26 (+43 s) | 19:11:47 (+21 s) | 19:11:57 |
| 1–2 | HV71 | 19:18:54 | 19:19:07 (+13 s) | 19:19:28 (+21 s) | 19:19:39 |
| 2–2 | IFB | 19:27:43 | 19:28:10 (+26 s) | 19:28:28 (+18 s) | 19:28:36 |
| 3–2 | IFB | 20:06:07 | 20:06:52 (+45 s) | 20:07:08 (+16 s) | 20:07:18 |
| 4–2 | IFB | 20:19:25 | 20:19:50 (+25 s) | 20:20:10 (+20 s) | 20:20:21 |

`Gjordes` är `realWorldTime`. `Registrerat` är `updatedTime`, som är i UTC i
ramen och här har räknats om till svensk tid (+2 h). Tiden i strömmen är när
målet kom första gången på någon av de tre anslutningarna. Vid 4–2 kom sse1
2 s före de andra två; annars kom alla tre samtidigt.

## Vad inspelningen visar

- **3–2 registrerades först i fel period, och då skrev SHL om ställningen i
  äldre mål.** Klockan 20:07:08 kom mål 66 i revision 2 med `period: 1`,
  `time: "09:54"` och `homeGoals/awayGoals` 2–1. Det skulle ha varit period 2
  och 3–2. I samma skur skickades de äldre målen om med ställningen räknad som
  om målet gjorts i första perioden: mål 26 (1–2) blev 2–2 i revision 6, och mål
  35 (2–2) blev 3–2 i revision 7. 20 sekunder senare, 20:07:28, kom revision
  3, 7 och 8, som rättade allt. Därför syns en ställning 2–1 i
  `forbehandla.py`-sammanställningen, i alla tre strömmarna men inte i
  game-overview. En lampa som läser ställningen ur ett enskilt `liveEvent` kan
  alltså få en ställning som aldrig funnits. `teamStatistics` i samma skur hade
  rätt antal mål. Ställningen ska därför inte hämtas från ett ensamt
  `liveEvent`.
- **Strömmen höll takten hela matchen.** Målen kom 16–21 s efter
  registreringen, och game-overview kom 8–12 s efter strömmen. Inget fastnade,
  till skillnad från 26 september.
- **Tiden från mål till registrering varierade från 13 till 193 s.** Det första
  målet tog över tre minuter.
- **SHL skickar om hela händelselistan i skurar**, oftast vid periodstart,
  periodslut och när ett mål rättas. De största: 85 `liveEvent` 21:13:08, 79
  20:42:08 och 77 20:24:28. Totalt 1 518 omsända händelser, och 383 rader per
  ström med lägre ställning än redan visad.
- **upcoming-games släppte matchen 19:20**, 20 minuter efter nedsläpp.
  played-games hade inte tagit upp den när inspelningen stoppades 21:17.
  today-games svarade med noll byte.
- **Ramtyper i strömmen (sse1):** `playerStatistics` 2 520, `liveEvent` 640,
  `liveState` 449, `teamStatistics` 233, `gameTime` 130, `gamePeriod` 6.

## Lampan

- **På 1.3.0-rc1 kraschade den med `panic` vid varje uppdateringskontroll.**
  Innan loggen startade hade det blivit 39 onormala omstarter.
  `checkAndApply()` hade vuxit från 160 till 2 160 byte stack, eftersom
  kompilatorn lade in `downloadAndInstall()` i den. Det sprängde loop-taskens
  8 kB under TLS-handslaget mot GitHub. I matchfönstret hoppas den automatiska
  kontrollen över, så lampan gick stabilt under matchen tills uppdatering
  trycktes 19:16. Det gav två krascher, 19:16:22 och 19:17:44. Rättat i
  1.3.0-rc3.
- **Kl. 18:54 sa lampan "Ingen match i matchfönstret"**, och den senaste
  live-ramen hade `liveState: "unknown"`, trots att matchen skulle börja 19:00.
  Den hittade matchen till slut och visade rätt ställning resten av kvällen.
- **19:43–19:51 kopplades lampan till USB och flashades med rc3.** Omstarterna
  i loggen kommer från det. Från 19:51:18 gick den på rc3 utan omstart till
  inspelningens slut: heap 150–167 kB, minst 104 kB, RSSI −74 till −85 dBm.
- **Första rapporten till driftstatistiken gick fram 19:52:11.** Den finns i
  D1-tabellen `reports`, id 1.
- **Paus-animationen fick en synlig lucka ungefär var 45:e sekund.** Det
  beror på att reservpollningen av game-overview blockerar `loop()` i 2–3 s
  medan TLS-handslaget pågår.

## Spela upp

```python
import gzip, json
for line in gzip.open("sse1.jsonl.gz", "rt"):
    frame = json.loads(line)      # frame["rx"], frame["id"], frame["data"]
```

Felregistreringen av 3–2 är bra att testa ställningstolkningen mot. Spela upp
`sse1.jsonl.gz` mellan 20:06:30 och 20:08:30: lampan ska gå från 2–2 till 3–2
utan att någonsin visa 2–1 och utan att tända ett mål för fel lag.
