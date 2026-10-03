# LHC–IFB 2026-10-03, 2–3

SHL:s live-kedja inspelad från 18:01:10 till 20:33:05, 14 minuter efter
slutsignalen. Inspelningen skulle ha börjat 17:15 men startade först efter
nedsläppet, så intåget och de första 55 sekunderna saknas (se nedan). Lampan
på 192.168.1.20 (Beta LövGlöd) loggades över HTTP och USB.

Inspelad från stugan: nätet är en 4G-router som tappar förbindelsen ibland,
och matchen streamades på samma uppkoppling samtidigt. Lampan och datorn låg
på samma WiFi (Gammstugan).

Alla filer utom `*.status`, `lampa.jsonl` och den här är gzippade. Tiderna är
lokala i Stockholm.

| Fil | Vad |
|---|---|
| `sse1–3.jsonl.gz` | Tre samtidiga SSE-anslutningar mot `game-broadcaster.s8y.se/live/game?gameUuid=rxyki9ytet`. En JSON-rad per ram: `rx`, `id`, `event`, `data`. 4 962 ramar per ström. |
| `sse1–3.log.gz` | Samma ramar i läsbar form från `tools/live.py --rå` |
| `sse1–3.status` | Anslutningar och fel per ström. Alla tre anslöt 18:01:11 och tappade aldrig anslutningen. |
| `poll.log.gz` | `www.bjorkloven.com/api/gameday/game-overview/rxyki9ytet` var 15:e sekund |
| `http.jsonl.gz` | Hela svaret från game-overview, play-by-play, gameheader, upcoming-games, played-games och today-games, sparat bara när innehållet ändrats |
| `lamp_http.log.gz` | Lampans status- (`/`) och felsökningssida (`/debug`) var 30:e sekund, 18:01–20:33, som text. 31 misslyckade läsningar, se Lampan. |
| `serial.log.gz` | Lampans USB-utskrift. Tom fram till 19:02, därefter 61 rader från det lokala bygget (se Lampan). |
| `lampa.jsonl` | `tools/forbehandla.py` körd på inspelningen, 6 392 rader |

Perioderna enligt `gamePeriod`: 18:00:16–18:34:02, 18:52:57–19:27:52 och
19:45:48–20:18:16. Strömmen sa `decided` 20:18:43 och game-overview
`GameEnded` 20:18:47.

## Målen

| Ställning | Lag | Spelare | Typ | Gjordes | Registrerat av SHL | I strömmen | game-overview | Lampans status |
|---|---|---|---|---|---|---|---|---|
| 0–1 | IFB | Axel Ottosson #18, P1 02:31 | PP1 | 18:03:58 | 18:04:24 (+26 s) | 18:04:40 (+16 s) | 18:04:49 | 18:04:49 |
| 0–2 | IFB | Axel Ottosson #18, P1 08:27 | PP1 | 18:13:30 | 18:13:55 (+25 s) | 18:14:00 (+5 s) | 18:14:09 | 18:15:42 |
| 1–2 | LHC | Brendan Shinnimin #24, P2 11:32 | SH1 | 19:14:00 | 19:14:25 (+25 s) | 19:14:40 (+15 s) | 19:14:51 | 19:14:59 |
| 1–3 | IFB | Tristen Robins #93, P3 03:01 | EQ | 19:51:18 | 19:52:22 (+64 s) | 19:52:40 (+18 s) | 19:52:57 | 19:52:51 |
| 2–3 | LHC | Cooper Marody #20, P3 11:34 | EQ | 20:04:20 | 20:04:55 (+35 s) | 20:05:19 (+24 s) | 20:05:20 | 20:05:28 |

`Gjordes` är `realWorldTime`. `Registrerat` är `updatedTime` i den första
revisionen som kom i strömmen, omräknad från UTC (+2 h). Alla tre
anslutningarna fick varje mål inom 0,4 s. `Lampans status` är första gången
statussidan visade den nya ställningen; den lästes var 30:e sekund. 0–2 syns
sent för att lampan inte svarade 18:13:52–18:15:01 (se Lampan), och 1–3 syns
på lampan före game-overview eftersom lampan läser strömmen.

## Utvisningarna

| Utvisning | Lag | Gjordes | I strömmen | Slut |
|---|---|---|---|---|
| P1 00:43, Sami Niku #88, 2 min hooking | LHC | 18:01:06 | 18:01:40 | Powerplaymål 0–1, P1 02:31 |
| P1 07:11, lagstraff 2 min, för många spelare | LHC | 18:10:46 | 18:11:20 | Powerplaymål 0–2, P1 08:27 |
| P2 09:38, lagstraff 2 min, för många spelare | LHC | 19:09:43 | 19:10:40 | På tid. LHC gjorde 1–2 i numerärt underläge under den |
| P2 12:29, Marcus Björk #47, 2 min crosschecking | IFB | 19:16:00 | 19:16:41 | På tid |
| P3 07:47, Chris DiDomenico #88, 2 min hooking | IFB | 19:57:47 | 19:58:40 | På tid |

LHC tog timeout P3 19:53 (20:17:00). Strömmen säger aldrig när en utvisning
tar slut, men varje utvisning fick en andra revision utan nytt innehåll: 11 och
22 s efter powerplaymålen, och 3–6 minuter efter att de andra tre började,
ungefär när de bör ha gått ut.

## Vad inspelningen visar

- **Strömmen höll takten hela matchen.** De tre anslutningarna var i fas, och
  ingen fastnade eller släpade. Nya revisioner kom i median 15 s och i 90 % av
  fallen inom 24 s efter `updatedTime`.
- **Tiden från mål till registrering var 25–64 s.** 1–3 tog längst.
- **Omsändningsskurarna kom vid periodbytena**, som mot VLH: 85 `liveEvent`
  19:45:59 (start P3), 82 19:28:01 (slut P2), 68 20:18:40 (slutsignalen) och
  46 18:34:20 (slut P1). Totalt 4 098 omsända händelser.
- **Vid första pausen kom nya revisioner av gamla händelser**, bland dem
  målvaktsbytena före nedsläpp med `updatedTime` från 17:54. De säger inget om
  eftersläpning; firmwaren räknar bara händelser nyare än de den redan sett.
- **upcoming-games släppte matchen 18:29**, 29 minuter efter nedsläpp.
  played-games hade inte tagit upp den när inspelningen stannade 20:33, 15
  minuter efter slutsignalen. today-games svarade med noll byte.
- **Ramtyper i strömmen (sse1):** `playerStatistics` 2 638, `liveEvent` 1 504,
  `liveState` 451, `teamStatistics` 232, `gameTime` 132, `gamePeriod` 5.

## Lampan

Två firmwareversioner under kvällen: 1.6.0 (betakanalen) fram till 19:02,
därefter ett lokalt bygge, 1.6.0-numerar, flashat över USB mitt i andra
perioden. Bygget är inte incheckat. Inspelarens huvudprocess pausades under
flashningen, 19:02:13–19:02:43; de tre SSE-strömmarna gick vidare.

| Tid | Läge | Listen |
|---|---|---|
| 18:01:10 | Match pågår, 0–0 | bärnstensglöd, live-ström ansluten |
| 18:04:49 | Mål, 0–1 | fyrverkeri |
| 18:15:42 | Mål, 0–2 | fyrverkeri |
| 18:34:43 | Paus | timglas |
| 18:53:25 | Match pågår | bärnstensglöd |
| 19:02:44 | Omstart efter flashning | 1.6.0-numerar |
| 19:28:06 | Paus | timglas |
| 19:46:18 | Match pågår | bärnstensglöd |
| 20:18:45 | Slutsignal, vi vann | segerdans |

- **Lampan svarade inte på HTTP i fem perioder:** 18:13:52–18:15:01,
  18:29:00–18:29:54, 18:57:36–18:57:46, 19:02:43–19:02:47 (omstarten efter
  flashningen) och från 20:26:43 till slutet. Ingen av de tre första var en
  omstart: upptiden växte jämnt och live-strömmen stod ansluten. Det var
  4G-routern under streaming, inte lampan. Från 20:26:32 var USB-kabeln
  urdragen och lampan nåddes inte mer.
- Ingen onormal omstart. 146 starter totalt före flashningen, 147 efter.
- Ledigt minne 138–145 kB under matchen. LED-taskens stack hade som minst
  4 940 B kvar.
- Signal −57 till −76 dBm, sämre än hemma (−42 till −48 mot VLH).
- "Senaste fel" på `/debug` visade `HTTP -1 /api/gameday/game-overview/…` från
  18:05 till omstarten 19:02, en misslyckad reservpollning över 4G-nätet.
- Lampan var avstängd mellan ungefär 16:06 och 16:52, innan inspelningen.

## Inspelningen startade sent

Inspelaren väntade på 17:15 med en enda `time.sleep()`. På macOS räknas inte
tiden medan datorn sover, så väntan blev för lång. Den startades om för hand
18:01, en minut efter nedsläppet. `tools/spela_in.py` väntar nu i korta steg och
börjar så fort datorn vaknar om starttiden passerat.

## Spela upp

```python
import gzip, json
for line in gzip.open("sse1.jsonl.gz", "rt"):
    frame = json.loads(line)      # frame["rx"], frame["id"], frame["data"]
```

En bortamatch med fem utvisningar: två powerplaymål för Löven, ett mål i
numerärt underläge för LHC och två utvisningar på Löven som gick ut på tid.
Bra att spela upp mot allt som har med numerär att göra. Strömmen var hel hela
kvällen.
