# Driftstatistik — Cloudflare Worker

Tar emot lampornas hälsorapporter (`src/telemetry.cpp` i firmwaren) och sparar
dem i en D1-databas. Worker:n läser aldrig avsändarens IP-adress.

## Driftsätta första gången

```sh
cd tools/telemetri
npm install
npx wrangler login
npx wrangler d1 create lovglod-telemetri      # klistra in database_id i wrangler.toml
npx wrangler d1 execute lovglod-telemetri --remote --file=schema.sql
npx wrangler secret put ADMIN_TOKEN           # valfri lång sträng, för att läsa ut data
npx wrangler deploy
```

`deploy` skriver ut adressen, t.ex. `https://lovglod-telemetri.<konto>.workers.dev`.
Lägg in den i `include/config.h`:

```c
#define TELEMETRY_URL "https://lovglod-telemetri.lovglod.workers.dev/v1/rapport"
```

Så länge `TELEMETRY_URL` är tom skickar lamporna ingenting, och inställningen
syns inte i portalen.

## Läsa ut

```sh
curl -H "Authorization: Bearer $ADMIN_TOKEN" https://…/v1/lampor
curl -H "Authorization: Bearer $ADMIN_TOKEN" https://…/v1/lampor/<id>
```

Eller direkt i SQL:

```sh
npx wrangler d1 execute lovglod-telemetri --remote --command \
  "SELECT name, fw, datetime(last_seen,'unixepoch') FROM lamps"
```

## Rapporten

```json
{
  "v": 1, "id": "3fa1c09e7b2d4410", "name": "Mamma", "fw": "1.4.0", "beta": false,
  "t": 1790619090, "up": 21600, "boots": 12, "badBoots": 0, "reset": "poweron",
  "heap": 121000, "minHeap": 84000, "rssi": -63, "strip": "WS2812B", "leds": 30,
  "bright": 160, "mode": 0, "goalDelay": 0, "shlErr": 2, "sseRe": 0,
  "events": [{ "type": "ota_ok", "t": 1790597700, "up": 190, "detail": "1.4.0" }]
}
```

`shlErr` och `sseRe` räknar sedan lampans start, inte sedan förra rapporten.
En tappad rapport gör därför inget, och skillnaden räknas ut i efterhand.
`t = 0` betyder att lampans klocka inte var synkad. Då räknar Worker:n fram
händelsens tid från upptiden.

Händelser: `boot`, `ota_ok` (ny firmware kvitterad), `ota_fail`,
`ota_rollback`.

## Styra lamporna

Svaret på en rapport får innehålla `interval` (sekunder, 3600–172800) och
`enabled: false`. Båda gäller i lampan fram till nästa omstart. Sätt `INTERVAL_S`
i `wrangler.toml` för att ändra intervallet för alla lampor.

Rapporter äldre än `RETENTION_DAYS` (365) rensas varje natt.
