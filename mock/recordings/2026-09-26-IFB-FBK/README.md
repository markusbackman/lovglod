# IFB–FBK 2026-09-26, 0–5

SHL:s live-kedja inspelad från 14:30 till nästa morgon 09:38. Ingen lampa var
igång, så inspelningen innehåller bara det som SHL och klubbsajten skickade.

Alla filer är gzippade. Tiderna är lokala i Stockholm.

| Fil | Vad |
|---|---|
| `sse1–3.jsonl.gz` | Tre samtidiga SSE-anslutningar mot `game-broadcaster.s8y.se/live/game?gameUuid=n9m6y5xe6d`. En JSON-rad per ram: `rx`, `id`, `event`, `data`. 8 479–9 650 ramar per ström. |
| `sse1–3.log.gz` | Samma ramar i läsbar form från `tools/live.py --rå` |
| `sse1–3.status` | Anslutningar och fel per ström |
| `poll.log.gz` | `www.bjorkloven.com/api/gameday/game-overview/n9m6y5xe6d` var 15:e sekund |
| `http.jsonl.gz` | Hela svaret från game-overview (15 s), play-by-play (30 s), gameheader, upcoming-games, played-games och today-games (60 s), sparat bara när innehållet ändrats. En rad: `rx`, `src`, `status`, `ms`, `len`, `sha`, `data`. |

Nedsläpp 15:14:48 enligt `gamePeriod`; en tidigare ram sa 15:07:11. Slutsignal
i game-overview 17:45:40.

## Målen

Alla målen gjordes av Färjestad, som var bortalag.

| Ställning | Gjordes | Registrerat av SHL | I strömmen | game-overview |
|---|---|---|---|---|
| 0–1 | 15:20:06 | 15:20:46 (+40 s) | 15:20:56 (+11 s) | 15:21:13 |
| 0–2 | 15:24:50 | 15:26:15 (+85 s) | 15:26:37 (+22 s) | 15:26:51 |
| 0–3 | 15:30:28 | 15:31:15 (+47 s) | 15:31:37 (+22 s) | 15:31:57 |
| 0–4 | 17:10:48 | 17:12:29 (+101 s) | 17:12:45 (+15 s) | 17:12:54 |
| 0–5 | 17:29:42 | 17:30:06 (+24 s) | **08:21 dagen efter** | 17:31:50 |

`Gjordes` är `realWorldTime`. `Registrerat` är `updatedTime`, som är i UTC i
ramen och här har räknats om till svensk tid (+2 h). Tiden i strömmen är när
målet kom första gången på någon av de tre anslutningarna.

## Vad inspelningen visar

- **Strömmen fastnade under tredje perioden, på alla tre anslutningarna
  samtidigt.** Fram till 17:10 kom händelserna 3–23 s efter registreringen.
  Därefter växte fördröjningen: 6 min vid 17:20, 17 min vid 17:30 och 37 min
  vid 17:50. Under tiden skickades mål 93 (0–4) och utvisning 86 om i en slinga
  var tionde sekund. Resten av matchens händelser kom 22:42, med
  `updatedTime: "0001-01-01T00:00:00"`. Målet till 0–5 (eventId 107) kom först
  08:21 nästa morgon. En lampa som bara läser strömmen hade alltså stannat på
  0–4.
- **game-overview höll takten när strömmen inte gjorde det.** Den visade 0–5
  två minuter efter målet, och play-by-play hade målet 17:32:02. Reservpollningen
  är alltså inte bara en reserv.
- **game-overview kan också gå bakåt.** 0–4 17:12:54, sedan 0–3 17:13:10 och
  0–4 igen 17:13:25. 0–5 17:31:50, sedan 0–4 17:32:06 och 0–5 igen 17:32:36.
  Samma klämma som för strömmen behövs alltså för pollningen.
- **game-overview säger `state: "Ongoing"` redan före nedsläpp**, med
  `period: null`, från 14:30. Det är perioden, inte `state`, som visar att
  matchen har börjat.
- **Tiden från målet till att SHL registrerar det varierade mer än mot ÖRE**,
  från 24 s till 101 s.
- **Alla tre anslutningarna fick samma ramar.** Ingen av dem fastnade i
  `unknown` efter nedsläpp, till skillnad från 19 september.
- **upcoming-games släppte matchen 15:23**, efter nedsläpp. played-games tog upp
  den 17:43, tappade den 04:13 och fick tillbaka den 05:00. today-games svarade
  med noll byte hela dagen.
- **Ramtyper i strömmen fram till 18:26 (sse1):** `playerStatistics` 3765,
  `liveEvent` 915, `liveState` 730, `teamStatistics` 328, `gameTime` 186,
  `gamePeriod` 5.

## Spela upp

```python
import gzip, json
for line in gzip.open("sse1.jsonl.gz", "rt"):
    frame = json.loads(line)      # frame["rx"], frame["id"], frame["data"]
```

För att spela upp matchen som den faktiskt gick är det enklast att ta
målen från `poll.log.gz` eller från `game-overview` i `http.jsonl.gz`, eftersom
strömmen saknar 0–5 under matchen. Vill man testa hur lampan klarar en ström
som fastnar räcker det att spela upp `sse1.jsonl.gz` mellan 17:10 och 18:00.
