"""
Manus för produktfilmen — allt som bestämmer vad filmen visar, på sekunden.

Tre spår som löper parallellt:

  LJUS       vad lampan gör. Lägena är samma som i firmwaren och räknas fram
             av ljusspar.py, diod för diod.
  TAGNINGAR  kamerorna. Varje tagning är en egen kamera med en rörelse från
             `fran` till `till`; klippen ligger där nästa tagning börjar.
  TEXTER     titlarna som ffmpeg lägger ovanpå i slutet.

Koordinater i millimeter i lampans eget system: x åt höger sett framifrån,
y bakåt in i bilden (lövets framsida ligger på y = 32, sockelns front på
y = 0), z uppåt från bordet. Lövets mitt är ungefär (0, 42, 140).

Ändra här och kör om — bygg.py räknar om ljusspåret och renderar bara om
det som behövs.
"""

FPS = 30
LANGD_S = 38.0

# Musiken styr klippen. Lone Skate Glide går i 120 bpm och dropet ligger på
# 16,6 s i filmen, så takterna (fyra slag, 2 s) faller på 0,6, 2,6, 4,6 … och
# slagen var 0,5 s däremellan. Klippen ligger på takter före dropet och på slag
# i de korta inklippen efter.
#
# Upplägget följer hur produktfilmer i Apples stil brukar byggas: en kort
# teaser i mörker, hela lampan redan efter 4,6 s, och kameran står still när
# det är ljuset som är poängen. En halv sekunds mörker före dropet gör att
# målet slår in ur svart.

# (start i sekunder, läge, parametrar). Ett läge gäller tills nästa börjar.
# Kortare än i verkligheten — målfyrverkeriet är 22 s och segerdansen 45 s i
# firmwaren — men varje läge ritas precis som lampan ritar det.
LJUS = [
    (0.0,  "av",       {}),
    (0.6,  "uppstart", {"hall_s": 0.3}),        # listen flödar in, tonar till glöd
    (2.6,  "glod",     {}),                     # standby, ~6,7 s andetag
    (8.6,  "live",     {}),                     # matchfönstret öppet
    (12.6, "hjarta",   {"fran": 0.55, "till": 1.0}),  # slutspurt, jämnt läge
    (16.2, "av",       {}),                     # andhämtningen före dropet
    (16.6, "mal",      {"vikt": 255, "strobe_s": 2.5}),  # avgörande mål
    (20.6, "dans",     {}),                     # slutsignal — segerdansen
    (30.6, "seger",    {}),                     # segerläget, gnistor ovanpå
]

# Studioljuset, som faktor på full styrka: (sekund, faktor), linjärt emellan.
# FYLL är nyckel- och takljuset som visar själva lampan, KANT de smala
# motljusen som ritar upp konturen. Filmen börjar med bara konturen, fyllet
# kommer upp när lampan visas hel, allt släcks i andhämtningen och efter målet
# är det lampan som lyser upp rummet.
FYLL = [(0.0, 0.0), (4.6, 0.0), (7.6, 1.0), (16.15, 1.0), (16.2, 0.0),
        (16.6, 0.0), (16.7, 0.15), (30.6, 0.15), (32.8, 0.6)]
KANT = [(0.0, 0.5), (4.6, 0.6), (7.6, 1.0), (16.15, 1.0), (16.2, 0.0),
        (16.6, 0.0), (16.7, 0.35), (30.6, 0.35), (32.8, 1.0)]

# Kamerarörelser. `fran`/`till` = (kamerans position, punkten den tittar på).
# `bland` är bländartal (lägre = grundare skärpedjup), `lins` i mm. Lampan är
# liten: på 30 cm avstånd med 100 mm-objektiv är skärpedjupet vid f/4 bara ett
# par millimeter, så närbilderna behöver f/11 och uppåt.
#
# `skift` flyttar bildutsnittet i sidled utan att ändra perspektivet (Blenders
# shift_x): −0,20 lägger lampan i högra tredjedelen och lämnar den vänstra fri
# för text. Rörelserna börjar i full fart och bromsar in mot slutet — klippet
# kommer innan kameran hunnit stanna.
TAGNINGAR = [
    # Teaser: bara ljus och kontur.
    dict(namn="vakna", start=0.0, slut=2.6, lins=100, bland=11,
         fran=((60, -250, 262), (14, 32, 222)),
         till=((52, -232, 258), (12, 32, 221))),
    dict(namn="kantglid", start=2.6, slut=4.6, lins=100, bland=13,
         fran=((215, -150, 150), (72, 32, 150)),
         till=((205, -160, 200), (66, 32, 188))),
    # Hela lampan: backar ut ur bandet medan fyllet tänds.
    dict(namn="avslojande", start=4.6, slut=8.6, lins=70, bland=8,
         fran=((-20, -420, 190), (0, 32, 178)),
         till=((-130, -1230, 165), (0, 42, 130))),
    dict(namn="trekvart", start=8.6, slut=12.6, lins=85, bland=8, skift=-0.20,
         fran=((830, -950, 200), (0, 42, 132)),
         till=((420, -1190, 175), (0, 42, 132))),
    dict(namn="uppbyggnad", start=12.6, slut=16.2, lins=70, bland=8, skift=-0.20,
         fran=((330, -1050, 70), (0, 42, 122)),
         till=((260, -860, 80), (0, 42, 126))),
    # Mörker, sedan målet. Stilla kamera: det är strobe-blixten som ska synas.
    dict(namn="mal", start=16.2, slut=19.1, lins=70, bland=8,
         fran=((0, -1120, 118), (0, 42, 135)),
         till=((0, -1090, 118), (0, 42, 135))),
    # Tre inklipp på slagen.
    dict(namn="inklipp_kant", start=19.1, slut=19.6, lins=100, bland=13,
         fran=((180, -200, 235), (50, 32, 205)),
         till=((176, -205, 232), (48, 32, 204))),
    dict(namn="inklipp_golv", start=19.6, slut=20.1, lins=50, bland=8,
         fran=((-220, -430, 10), (0, 32, 70)),
         till=((-205, -440, 10), (0, 32, 70))),
    dict(namn="inklipp_sida", start=20.1, slut=20.6, lins=85, bland=8,
         fran=((560, -160, 160), (0, 42, 150)),
         till=((555, -175, 158), (0, 42, 150))),
    # Segerdansen: hela lampan, nästan stilla kamera.
    dict(namn="dans_front", start=20.6, slut=24.6, lins=60, bland=8, skift=-0.20,
         fran=((190, -860, 150), (0, 42, 135)),
         till=((180, -830, 150), (0, 42, 135))),
    dict(namn="dans_hog", start=24.6, slut=28.6, lins=60, bland=8, skift=-0.20,
         fran=((260, -640, 560), (0, 42, 120)),
         till=((200, -670, 550), (0, 42, 120))),
    dict(namn="spegling", start=28.6, slut=30.6, lins=50, bland=8,
         fran=((-150, -560, 12), (0, 42, 110)),
         till=((-70, -575, 12), (0, 42, 112))),
    # Nedvarvning och slutbild.
    dict(namn="landning", start=30.6, slut=33.6, lins=85, bland=8,
         fran=((-520, -1120, 90), (0, 42, 132)),
         till=((-320, -1190, 96), (0, 42, 132))),
    dict(namn="slut", start=33.6, slut=LANGD_S, lins=85, bland=8, skift=-0.20,
         fran=((200, -1330, 112), (0, 42, 130)),
         till=((195, -1300, 112), (0, 42, 130))),
]

# (start, slut, text, stil). Stilarna finns i bygg.py. Text bara i tagningar
# där lampan står i högra tredjedelen, så att den aldrig täcker lampan.
TEXTER = [
    (6.0, 8.4, "LövGlöd", "titel"),
    (9.2, 12.4, "Vet när det är match.", "rad"),
    (13.0, 16.0, "Känner slutminuterna.", "rad"),
    (21.2, 24.4, "Och när Löven vinner —", "rad"),
    (25.0, 28.4, "vet hela rummet om det.", "rad"),
    (34.2, 37.4, "LövGlöd", "titel"),
    (34.8, 37.4, "Ljuset som följer Björklöven.", "under"),
]

# Musiken, och var i låten filmen börjar. Lone Skate Glide går i 120 bpm och
# dropet slår in på 98,9 s — lagt på 16,6 s i filmen, samma bildruta som
# målets strobe. Flyttas målet i LJUS ska `start_s` flyttas lika mycket.
MUSIK = dict(fil="musik/lone-skate-glide.mp3", start_s=98.9 - 16.6, tona_ut_s=3.0)

# Fade från och till svart, sekunder.
TONA_IN_S = 0.3
TONA_UT_S = 1.4


def antal_rutor() -> int:
    return round(LANGD_S * FPS)
