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

## Översiktssidan

`https://lovglod-telemetri.lovglod.workers.dev/` visar flottan: hur många
lampor som finns, vilka som tystnat eller är värda en titt, versionerna, de
senaste händelserna, och för varje lampa kurvor över WiFi-signal och minne.
Logga in med `ADMIN_TOKEN`. Inloggningen sparas i en cookie i 90 dagar.

En lampa räknas som **tyst** när den inte hörts av på 12 timmar, alltså två
missade rapporter. **Titta** betyder onormala omstarter, brownout, krasch eller
watchdog vid senaste starten, signal under −80 dBm eller under 30 kB minne.

Sidan ligger i `src/dashboard.html`. Så här provar du den lokalt:

```sh
npx wrangler d1 execute lovglod-telemetri --local --file=schema.sql
echo ADMIN_TOKEN=test > .dev.vars
npx wrangler dev
```

## Signal om nya lampor

När ett okänt lamp-ID rapporterar för första gången skickar Worker:n en
pushnotis via [ntfy](https://ntfy.sh): namn, firmware, antal lysdioder, WiFi och
hur stor flottan nu är. Hitta på ett långt, svårgissat ämne — den som känner
till ämnet kan läsa notiserna — och prenumerera på det i ntfy-appen:

```sh
npx wrangler secret put NTFY_TOPIC    # t.ex. lovglod-7f3c9a1e5b2d
```

Egen ntfy-server: sätt `NTFY_URL` i `wrangler.toml`, och `NTFY_TOKEN` som
hemlighet om servern kräver inloggning. Saknas `NTFY_TOPIC` går signalen som
mejl i stället, om veckobrevet är inställt. En lampa som fabriksåterställts får
nytt ID och räknas alltså som ny.

## Veckobrevet

Varje måndag morgon (05:41 UTC) kommer ett mejl om veckan som gått: hur många
lampor som är i drift jämfört med veckan innan, nya lampor, versionerna,
uppdateringar som lyckats och misslyckats — och en lista över lampor att
möjligen agera på:

- tysta sedan mer än 12 timmar (men mindre än 30 dagar — sedan räknas lampan
  som nedplockad och nämns inte längre),
- onormala omstarter, brownout, krasch eller watchdog,
- WiFi under −80 dBm eller under 30 kB minne någon gång under veckan,
- misslyckade eller tillbakarullade uppdateringar,
- stabila lampor som ligger efter den nyaste stabila versionen i flottan.

Mejlet skickas med [Resend](https://resend.com). Skapa ett konto och en
API-nyckel. Utan egen domän går det bara att skicka till adressen kontot är
registrerat på, vilket räcker här; med en verifierad domän sätter du
`MAIL_FROM` i `wrangler.toml`.

```sh
npx wrangler secret put RESEND_API_KEY
npx wrangler secret put MAIL_TO       # flera mottagare: kommaseparerat
npx wrangler deploy
```

Förhandsvisa brevet, eller skicka det direkt för att prova:

```sh
curl -H "Authorization: Bearer $ADMIN_TOKEN" https://…/v1/veckobrev            # HTML
curl -X POST -H "Authorization: Bearer $ADMIN_TOKEN" https://…/v1/veckobrev    # skicka
```

Lokalt: `npx wrangler dev --test-scheduled` och sedan
`curl "localhost:8787/__scheduled?cron=41+5+*+*+1"`.

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
