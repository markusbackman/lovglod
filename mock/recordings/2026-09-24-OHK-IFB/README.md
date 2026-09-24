# ÖRE–IFB 2026-09-24, 3–4 efter förlängning

Hela live-kedjan inspelad, från SHL:s stream till lampornas ljus. Matchen är
Björklövens andra för säsongen och den första där två lampor kördes samtidigt
på olika firmware — därför fångar den både buggarna och rättningarna.

Alla filer är gzippade. Tiderna är lokala i Stockholm, med millisekunder där de
går att lita på.

| Fil | Vad |
|---|---|
| `sse1–3.jsonl.gz` | Tre samtidiga SSE-anslutningar mot `game-broadcaster.s8y.se/live/game?gameUuid=m0jymzp5c5`. En JSON-rad per ram: `rx` (mottagen), `id`, `event`, `data`. 4138 ramar per ström. |
| `sse1–3.log.gz` | Samma ramar i läsbar form, en rad var, från `tools/live.py --rå` |
| `sse1–3.status` | Anslutningar och fel per ström |
| `poll.log.gz` | `www.bjorkloven.com/api/gameday/game-overview/m0jymzp5c5` var 15:e sekund |
| `serial.log.gz` | Lampan på 192.168.1.58, diag-bygge med `LIVE_TRACE`, tidsstämplad per rad. Bröts 21:15 när USB-kabeln drogs. |
| `lamp_http.log.gz` | Samma lampas status- och felsökningssida var 30:e sekund |

## Målen

| Ställning | Gjordes | Registrerat av SHL | I strömmen | Mål → lampa | Omsänt |
|---|---|---|---|---|---|
| 0–1 | 19:05:05 | 19:05:48 (+43 s) | 19:06:13,21 (+25 s) | 68 s | 10 ggr |
| 0–2 | 19:25:16 | 19:26:01 (+45 s) | 19:26:13,05 (+12 s) | 57 s | 9 ggr |
| 1–2 | 20:03:41 | 20:04:28 (+47 s) | 20:04:53,06 (+25 s) | 72 s | 7 ggr |
| 2–2 | 20:10:42 | 20:11:23 (+41 s) | 20:11:35,53 (+13 s) | 54 s | 7 ggr |
| 2–3 | 20:44:21 | 20:45:05 (+44 s) | 20:45:33,46 (+28 s) | 73 s | 6 ggr |
| 3–3 | 20:46:08 | 20:46:49 (+41 s) | 20:47:14,59 (+26 s) | 67 s | 5 ggr |
| 3–4 | 21:14:57 | 21:16:08 (+71 s) | 21:16:13,85 (+6 s) | 77 s | 1 gg |

`Gjordes` är `realWorldTime` i ramen, `registrerat` är `updatedTime` (UTC i
ramen, +2 h här). Lampan läste ramen 0,2–0,8 s efter datorn varje gång.

## Vad inspelningen visar

- **Fördröjningen ligger hos SHL, inte hos lampan.** 41–47 sekunder går åt
  innan sekretariatet registrerar målet, sedan 6–28 sekunder till strömmen.
  Lampans egen del är under en sekund. Tv-fördröjningen på lampan stängdes av
  mitt i matchen och syns i loggen som skillnaden mellan `[MÅL]` och
  `[led] LIVE → GOAL`.
- **Strömmen skickar om gamla målhändelser hela tiden.** 45 omsändningar av 7
  mål under matchen, ibland alla tidigare mål i rad (20:40:15–17). Mellan dem
  kommer ramar med den gamla ställningen. Utan klämman i 1.0.14 tänder varje
  sådan omgång fyrverkeriet igen — lampan på 1.0.12 firade 0–1 tre gånger.
- **`today-games` svarar 200 med noll byte** hela matchdagen. Reservpollningen
  läser därför `game-overview` sedan 1.0.16.
- **`upcoming-games` behöll den pågående matchen** med ett inaktuellt
  `state: "pre-game"`, till skillnad från 19 september då den försvann. Båda
  beteendena förekommer, så lampan får inte lita på någotdera.
- **Ramtyper i strömmen:** `playerStatistics` 2470, `liveEvent` 829,
  `liveState` 458, `teamStatistics` 239, `gameTime` 134, `gamePeriod` 8.
  Ställningen finns bara i `liveEvent`, vilket är varför en lampa som startar
  mitt i matchen står utan ställning tills den pollar.

## Spela upp

Ramarna ligger i mottagningsordning med tidsstämpel, så de kan spelas upp i
rätt takt mot en lampa eller mot en testrigg:

```python
import gzip, json
for line in gzip.open("sse1.jsonl.gz", "rt"):
    frame = json.loads(line)      # frame["rx"], frame["id"], frame["data"]
```

Målen ligger på `data.liveEvent` med `type == "goal"` och ställningen i
`homeTeam.score` / `awayTeam.score`. Vill man återskapa buggen med
trippeltända mål räcker det att spela upp `ev14` med mellanliggande ramar från
19:06 till 19:12.
