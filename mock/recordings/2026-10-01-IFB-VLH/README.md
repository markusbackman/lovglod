# IFB–VLH 2026-10-01, 4–2

SHL:s live-kedja inspelad från 18:15 till 21:53:48, 40 minuter efter
slutsignalen. Lampan på 192.168.1.99 (Beta LövGlöd, 1.5.1) var igång hela
kvällen och loggades över HTTP från första till sista minuten.

Alla filer utom `*.status`, `lampa.jsonl` och den här är gzippade. Tiderna är
lokala i Stockholm.

| Fil | Vad |
|---|---|
| `sse1–3.jsonl.gz` | Tre samtidiga SSE-anslutningar mot `game-broadcaster.s8y.se/live/game?gameUuid=bxrnnm4256`. En JSON-rad per ram: `rx`, `id`, `event`, `data`. 3 246 ramar per ström. |
| `sse1–3.log.gz` | Samma ramar i läsbar form från `tools/live.py --rå` |
| `sse1–3.status` | Anslutningar och fel per ström. Alla tre anslöt 18:15:01 och tappade aldrig anslutningen. |
| `poll.log.gz` | `www.bjorkloven.com/api/gameday/game-overview/bxrnnm4256` var 15:e sekund |
| `http.jsonl.gz` | Hela svaret från game-overview, play-by-play, gameheader, upcoming-games, played-games och today-games, sparat bara när innehållet ändrats. En rad: `rx`, `src`, `status`, `ms`, `len`, `sha`, `data`. |
| `lamp_http.log.gz` | Lampans status- (`/`) och felsökningssida (`/debug`) var 30:e sekund, 18:15–21:53, som text. Inga misslyckade anrop. |
| `serial.log.gz` | Tom. Porten öppnades 18:15:00 men lampan skrev inget på den. |
| `lampa.jsonl` | `tools/forbehandla.py` körd på inspelningen, 3 778 rader |

Nedsläpp 18:59:16 enligt `gamePeriod`. Perioderna gick 18:59:16–19:30:28,
19:48:11–20:21:47 och 20:39:19–21:13:18. game-overview sa `GameEnded` 21:13:48.

`startedAt` och `finishedAt` i `gamePeriod` slutar på `Z` men är svensk tid,
inte UTC: första perioden "startade" `18:59:16.043Z`, och strömmen bytte till
`ongoing` 18:59:36. `updatedTime` i `liveEvent` är däremot UTC, utan `Z`.

## Målen

| Ställning | Lag | Spelare | Gjordes | Registrerat av SHL | I strömmen | game-overview | Lampans status |
|---|---|---|---|---|---|---|---|
| 1–0 | IFB | Tristen Robins #93, P1 02:25 | 19:04:36 | 19:04:57 (+21 s) | 19:05:16 (+19 s) | 19:05:19 | 19:05:44 |
| 2–0 | IFB | Fredrik Forsberg #56, P1 05:22 | 19:11:01 | 19:11:44 (+43 s) | 19:11:57 (+13 s) | 19:12:00 | 19:12:18 |
| 3–0 | IFB | Oscar Tellström #91, P2 00:25 | 19:48:53 | 19:49:26 (+33 s) | 19:49:36 (+10 s) | 19:49:46 | 19:49:59 |
| 4–0 | IFB | Axel Ottosson #18, P2 07:40 | 19:59:48 | 20:00:20 (+32 s) | 20:00:36 (+16 s) | 20:01:00 | 20:01:04 |
| 4–1 | VLH | Eemeli Suomi #10, P2 08:34 | 20:02:38 | 20:03:11 (+33 s) | 20:03:36 (+25 s) | 20:03:49 | 20:04:04 |
| 4–2 | VLH | Olivier Nadeau #20, P3 01:20 | 20:42:04 | 20:43:20 (+76 s) | 20:43:37 (+17 s) | 20:43:40 | 20:43:48 |

`Gjordes` är `realWorldTime`. `Registrerat` är `updatedTime` i den första
revisionen som kom i strömmen, omräknad från UTC (+2 h). Tiden i strömmen är när
målet kom första gången; alla tre anslutningarna fick det inom 0,5 s.
`Lampans status` är första gången lampans statussida visade den nya ställningen.
Sidan lästes var 30:e sekund, så lampan kan ha bytt upp till 30 s tidigare.

## Vad inspelningen visar

- **Strömmen höll takten hela matchen.** Alla tre anslutningarna var i fas,
  utan eftersläpning, utan `unknown`-backends och utan att någon ström fastnade.
  Målen kom 10–25 s efter registreringen, och game-overview kom 3–24 s efter
  strömmen. Ingen frysning efter första pausen, som mot DIF 19 september.
- **Tiden från mål till registrering var 21–76 s.** 4–2 tog längst.
- **SHL skickar om händelselistan i skurar vid periodslut och periodstart.**
  De största i sse1: 68 `liveEvent` 21:13:38 (slutsignalen), 64 20:39:38
  (start P3) och 62 20:22:17 (slut P2). Totalt 1 152 omsända händelser och 240
  rader per ström med lägre ställning än redan visad. Samma mål kom upp till
  fyra gånger (mål 8 i revision 2, 3 och 4 inom två minuter).
- **Ingen felregistrering** av den typ som syntes mot HV71. Ställningen i
  `liveEvent` gick aldrig till ett läge som inte funnits.
- **upcoming-games släppte matchen 19:25**, 25 minuter efter nedsläpp.
  played-games tog upp den 21:38, 24 minuter efter slutsignalen. today-games
  svarade med noll byte.
- **Ramtyper i strömmen (sse1):** `playerStatistics` 1 799, `liveState` 654,
  `liveEvent` 525, `teamStatistics` 203, `gameTime` 59, `gamePeriod` 6.

## Lampan

Hela kvällen utan anmärkning. Ingen omstart (136 starter totalt, 0 onormala,
upptiden växte jämnt), ingen förlorad live-ström, och ingen ställning som gick
bakåt eller räknades dubbelt trots omsändningarna.

| Tid | Läge | Listen |
|---|---|---|
| 18:15 | Standby | långsam gul glöd, gnistor efter vinsten mot HV71 |
| 18:45:08 | Match pågår, 0–0 | bärnstensglöd, live-ström ansluten |
| 19:31:23 | Paus | timglas |
| 19:48:58 | Match pågår | bärnstensglöd |
| 20:22:40 | Paus | timglas |
| 20:39:45 | Match pågår | bärnstensglöd |
| 21:10:56 | Match pågår · slutspurt 23–38 % | bärnstensglöd |
| 21:13:57 | Match pågår · slutspurt 38 % | Segerdans — slutsignal, vi vann |
| 21:44:07 | Seger — firar | Vann senaste matchen — lugna kometer |

- Matchfönstret öppnade 18:45, 15 minuter före nedsläpp, som det ska.
- Lampan bytte till paus inom en minut efter periodslut (strömmen sa
  `intermission` 29 s efter `finishedAt`), och tillbaka till match inom en
  minut efter periodstart.
- Segerdansen höll i 30 minuter och gick sedan över i gröna kometer.
- Ledigt minne 188 kB i standby, 140–146 kB med live-strömmen öppen, lägst
  133 kB. Det sjönk inte under matchens gång. LED-taskens stack hade som minst
  5 004 B kvar, från första pausen (timglaset); innan dess 5 196 B.
- Signal −42 till −48 dBm.
- Målfördröjningen stod på 0 s.
- Målfirandet syns inte i loggen: statussidan lästes var 30:e sekund och hade
  redan gått tillbaka till bärnstensglöd vid varje läsning.
- **"Senaste fel" på `/debug` stod på "Ingen match i matchfönstret" hela
  kvällen**, även när matchen pågick och lampan visade rätt ställning. Felet
  sattes före 18:45 och nollställs inte när matchen väl hittas. Rättat i
  1.5.2: att ingen match pågår räknas inte längre som fel.

## Spela upp

```python
import gzip, json
for line in gzip.open("sse1.jsonl.gz", "rt"):
    frame = json.loads(line)      # frame["rx"], frame["id"], frame["data"]
```

En lugn referensmatch: allt fungerade och strömmen var hel. Bra som kontroll
att en ändring inte förstör det normala fallet. Omsändningsskuren vid
slutsignalen, 21:13:38–21:13:40, är bra att spela upp mot segerdansen: lampan
ska gå till segerdans en gång och stå kvar på 4–2.
