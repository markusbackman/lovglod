# Textljus — en andra LED-list bakom texten (planerat)

[← Tillbaka till README](../README.md)

> **Status: idé, inte byggd.** Beslut taget 2026-11-15: vi går vidare med
> varianten **Genomlysta bokstäver** ("Backlit letters" i
> [`tools/textljus.html`](../tools/textljus.html)). Inget i `hardware/v2` är
> ändrat ännu.

Målet är en andra, separat styrd LED-list som lyser upp "BJÖRKLÖVEN / UMEÅ"
inifrån, oberoende av listen bakom det gula bandet.

## Utgångsläge (uppmätt ur STL-filerna i `hardware/v2/plate-1`)

Koordinater som i STL-filerna: framsidan är z = 0 (den ligger nedåt vid
utskrift) och lövets spets är y = 0. Sett framifrån är x spegelvänt.

- Bokstäverna (`bjorkloven_letters`) ligger på z 0 → 1 mm. Bakom dem finns 1 mm
  grönt i `bjorkloven_sign` (z 1 → 2), så inget ljus kommer igenom i dag.
- Kärnan (`bjorkloven_core`) är en **ring med ca 3 mm vägg** och går från
  z 2 till z 15. Ytterväggen är 488 mm runt. Innanför ringen finns ett **tomt
  hålrum**, ca 148 × 151 mm och 16 400 mm², som går från framsidans baksida
  (z 2) till bakstycket. Det ligger direkt bakom texten.
- Bandlisten står i spåret *utanför* kärnan och lyser utåt. Hålrummet är alltså
  mörkt och skilt från bandets ljus.
- Texten fyller nästan hela hålrummet. Bokstäverna går ut till kärnans
  innervägg på sidorna och nästan ner till botten.
- Fyra skruvtorn för bakstycket sticker in i hålrummet (upptill, nertill och på
  båda sidor).

## Planerade ändringar i 3D-modellen

1. **`bjorkloven_letters`**: full tjocklek, z 0 → 2 mm, utskrivna i
   genomskinlig PLA (natur/vit eller genomskinlig gul, inte bestämt).
2. **`bjorkloven_sign`**: skär ut bokstävernas form ur det gröna lagret
   z 1 → 2, så att inget opakt ligger bakom texten.
3. **Ny del, `bjorkloven_text_carrier`**: en 1,6 mm platta i hålrummets form,
   utskriven i **vitt** så att den reflekterar ljuset. Listen limmas på dess
   framsida med dioderna mot texten.
4. **`bjorkloven_core`**: 4–6 små hyllor (ca 1,5 mm) på kärnväggens insida som
   bäraren vilar på. Bakstycket håller den på plats, så det behövs inga nya
   skruvar. Hyllornas höjd bestämmer avståndet mellan dioderna och framsidan.
   Börja med **ca 10 mm**: under ~8 mm syns varje diod som en egen ljusfläck.
5. **`bjorkloven_core`**: ett urtag på ca 4 × 3 mm i kärnväggens botten, nära
   spetsen, så att textlistens trådar kommer ut i bandspåret och kan följa det
   befintliga kabelspåret ner.
6. **Rekommenderat**: stoppa ljus som läcker genom det gröna fältet. Antingen
   en 0,4 mm vit eller svart skiva bakom fältet, eller 3 mm front just där.

## Listen

- **23 dioder** i tre raka rader vid 60/m, ca 40 cm list. En list kan inte
  böjas i sidled, så raderna är raka bitar som skarvas med korta trådar.
- Raderna i STL-koordinater (y = höjd över spetsen, x i framvy):

  | Rad | Text | y | x | Dioder |
  |---|---|---|---|---|
  | 1 (först i datakedjan) | UMEÅ | 64 mm | −62 → 50 | 7 |
  | 2 (vänd riktning) | LÖVEN | 108 mm | −68 → 62 | 8 |
  | 3 | BJÖRK | 148 mm | −58 → 62 | 8 |

  Raderna går i serpentin från botten, eftersom trådarna kommer in vid spetsen.
- Strömmen är högst ca 1,4 A extra vid fullt vitt. `LED_MAX_MILLIAMPS` begränsar
  hela lampan, eftersom FastLEDs effektbudget gäller alla controllers.

## Koppling

- **Förstahandsval:** kedja textlisten efter bandlisten. Bandlistens DOUT och
  5V/GND kopplas till textlistens DIN. Bandlisten går hela varvet runt, så dess
  slut ligger vid spetsen bredvid starten. Det behövs ingen ny tråd ner i
  sockeln, och firmwaren ser 30 + 23 dioder.
- **Alternativ:** en egen datapinne, till exempel `GPIO27`, med en extra tråd
  genom kabelspåret.

## Öppna frågor

- **Var finns källmodellen?** I repot finns bara STL-exporter. Ändringarna görs
  bäst i original-CAD:en.
- Hur högt ska hyllorna sitta? Provskriv en bit och testa 8, 10 och 12 mm.
- Vilket filament ska bokstäverna ha: genomskinlig natur/vit eller gul? Hur ser
  de ut i dagsljus när listen är släckt?
- Räcker 3 mm vägg i kärnan, eller lyser bandlisten igenom in i hålrummet?
- Firmware:
  - Dela upp listen i ett band-segment och ett text-segment.
  - Lägg till en inställning i portalen för textens antal dioder.
  - Bestäm vilka lägen som ska lysa i texten.

Animationer och jämförelse med de andra varianterna finns i
[`tools/textljus.html`](../tools/textljus.html).
