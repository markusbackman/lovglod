# Installera firmwaren

[← Tillbaka till README](../README.md)

Hur en färdig LövGlöd-binär hamnar i en ESP32, hur lampan ställs in första
gången och hur den håller sig uppdaterad. Vill du bygga firmwaren själv, se
[Bygga projektet](BYGGA.md).

- [Vilken väg?](#vilken-väg)
- [Flasha över USB](#flasha-över-usb)
- [Första start — setup-portalen](#första-start--setup-portalen)
- [Uppdateringar](#uppdateringar)

---

## Vilken väg?

| Läge | Gör så här |
|---|---|
| Kortet är nytt, eller kör något annat än LövGlöd | [Flasha över USB](#flasha-över-usb), en gång |
| Lampan kör redan LövGlöd | Ingenting — den [uppdaterar sig själv](#uppdateringar) över WiFi |
| En uppdatering gick igenom men är trasig | Flasha över USB igen, se [Tillbaka till en tidigare version](#tillbaka-till-en-tidigare-version) |

Det finns ingen push-OTA: en lampa tar bara emot signerad firmware den själv
hämtat från GitHub, eller det du flashar över USB.

---

## Flasha över USB

### 1. Installera esptool

[esptool](https://docs.espressif.com/projects/esptool/) är Espressifs
flashverktyg. Det behöver Python:

```bash
pip install esptool
```

Koppla in kortet med en USB-kabel som för **data**, inte bara ström. Många
laddkablar gör bara det senare, och då dyker ingen seriell port upp.

### 2. Hämta den kompletta flashbilden

Från [Releases](https://github.com/markusbackman/lovglod/releases),
den version du vill ha — senaste stabila är märkt *Latest*:

| Fil | Vad det är |
|---|---|
| **`lovglod-full.bin`** | Allt ett nytt kort behöver: bootloader, partitionstabell och appen, i en fil |
| `lovglod-full.bin.sha256` | Kontrollsumma för filen ovan |
| `firmware.bin` | Bara appen — för kort som redan kört LövGlöd, och det enheterna hämtar över OTA |
| `firmware.json`, `firmware.sig` | Manifest och signatur, används av OTA |

Kontrollera nedladdningen innan du skriver den till ett kort:

```bash
shasum -a 256 -c lovglod-full.bin.sha256      # macOS/Linux
```

### 3. Skriv till kortet

```bash
esptool.py --chip esp32 --baud 460800 write_flash -z 0x0 lovglod-full.bin
```

Bilden skrivs från flashens början och **nollställer inställningarna** — WiFi,
listval, ljusstyrka och uppdateringskanal. På ett nytt kort finns inget att
förlora, men ska en lampa i drift bara byta version, ta [bara
appen](#bara-appen) i stället.

**Håll in BOOT-knappen** när esptool skriver `Connecting...` — kortet har
trasig auto-reset och kommer annars inte in i nedladdningsläget. Släpp när
skrivningen startat.

esptool hittar porten själv. Har du flera seriella enheter: lägg till
`--port /dev/cu.usbserial-XXXX` (macOS), `/dev/ttyUSB0` (Linux) eller `COM3`
(Windows).

Tryck på EN/RESET när det är klart. Listen ska flöda in i gult och sedan pulsa
grönt — då är kortet igång och väntar på att bli inställt.

### Bara appen

Har kortet redan kört LövGlöd sitter bootloadern och partitionstabellen redan
där. Då räcker `firmware.bin`, som skrivs till appens offset:

```bash
esptool.py --chip esp32 --baud 460800 write_flash -z 0x10000 firmware.bin
```

Det är samma binär som enheterna hämtar över OTA, och den går att kontrollera
mot `sha256` i `firmware.json`.

### Tillbaka till en tidigare version

Samma kommandon med en äldre release. `lovglod-full.bin` fungerar alltid;
`firmware.bin` räcker om kortet redan kör LövGlöd.

`firmware.bin` rör inte inställningarna: WiFi, listval, ljusstyrka och
uppdateringskanal ligger i NVS och står kvar. `lovglod-full.bin` skriver över
NVS och lampan vaknar i setup-portalen, som ett nytt kort.

Vill du tömma allt oavsett väg: kör `esptool.py erase_flash` först.

Observera att en lampa på stabila kanalen installerar senaste stabila release
vid nästa kontroll, även om du nyss flashat en äldre. Ska den stå kvar på en
gammal version, töm **Uppdateringskälla** på statussidan.

---

## Första start — setup-portalen

Setup sker i två steg, av två olika personer.

**Administratören — innan lampan lämnas ut:**

1. Starta lampan utan WiFi → **grön puls** och den startar eget nät
   `LövGlöd-Setup-XXXX` (öppet). Innan en list är vald drivs båda
   utgångarna, så pulsen syns vilken list som än sitter i.
2. Anslut med mobilen. Inloggningsrutan poppar upp av sig själv (iOS, Android
   och Windows kontroll-URL:er är hanterade). Gör den inte det: gå till
   `http://192.168.4.1`.
3. Portalen visar bara listvalet: välj **WS2812B** eller **APA102** och antal
   dioder, tryck *Spara list*. Inget WiFi behövs. Pulsen flyttar direkt till
   den valda listen — syns den inte är valet fel.
4. Dra ur strömmen och lämna ut lampan.

Listvalet ligger i NVS och överlever omstart, OTA och "Glöm WiFi". Ändra det i
efterhand på `http://192.168.4.1/strip` i portalen, eller via *Felsökning* på
statussidan.

**Kunden — hemma:**

1. Kopplar in lampan → grön puls, nätet `LövGlöd-Setup-XXXX` dyker upp.
2. Ansluter med mobilen och ser bara WiFi-formuläret: välj nätverk, skriv
   lösenord, spara.
3. Lampan ansluter och glöden blir gul.

Lampor som uppgraderas över OTA från en version utan listval behåller sin
WS2812B — firmwaren känner igen dem på att WiFi redan är sparat.

Går anslutningen inte igenom öppnas portalen igen. Står portalen öppen utan att
någon gör något provar lampan de sparade uppgifterna på nytt var 5:e minut —
praktiskt om det bara var routern som startade om.

### Statussidan

När lampan är online nås den på `http://lovglod.local` eller
enhetens IP. Där finns nästa match, senaste resultat, ljusstyrka, en knapp som testar
målfyrverkeriet, och "Glöm WiFi".

---

## Uppdateringar

Lampan kollar GitHub Releases var 12:e timme — aldrig under pågående match —
och installerar själv när en ny version finns. Förloppet syns som en gul stapel
som fylls på listen, sedan startar den om.

Knappen **Sök efter uppdatering nu** på statussidan tvingar fram en kontroll
direkt.

Varje binär är signerad. Stämmer inte signaturen eller kontrollsumman
installeras ingenting och lampan fortsätter på den version den har. Hur det
fungerar beskrivs i [Signerad firmware](BYGGA.md#signerad-firmware).

### Uppdateringskälla

Standardvärdet `markusbackman/lovglod` är rätt för alla lampor som ska
följa det här projektet. Ange ett eget `owner/repo` för att följa en fork, eller
en full URL till ett `firmware.json` på en egen webbserver.

Källan måste vara publik: lampan hämtar releaser anonymt, se
[Källan måste vara publik](BYGGA.md#källan-måste-vara-publik).

### Betaprogrammet

Välj **Uppdateringskanal: Beta** på statussidan för att få pre-releaser
(`v1.2.0-rc1`) före alla andra. Betalampor kollar var 3:e timme och tar den
nyaste releasen oavsett sort, så de glider över till stabila versionen när den
släpps.

Byter du tillbaka till **Stabil** kollar lampan direkt och installerar senaste
stabila — även om den är äldre än betan du kör. Detaljer i
[Betaprogrammet](BYGGA.md#betaprogrammet).
