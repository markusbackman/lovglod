#!/usr/bin/env bash
# Genererar nyckelparet som OTA-signaturen vilar på.
#
#   privat nyckel  ota_signing_key.pem   ligger kvar lokalt, .gitignore:ad,
#                                        och ska in som GitHub-secret
#   publik nyckel  include/ota_pubkey.h  byggs in i firmwaren och SKA committas
#
# Kör en gång. Byter du nyckel måste både secreten och headern bytas i samma
# veva — CI kontrollerar att de hör ihop och vägrar släppa en release annars.
set -euo pipefail

cd "$(dirname "$0")/.."

KEY=${1:-ota_signing_key.pem}
HDR=include/ota_pubkey.h

# Nyckeln genereras bara om den saknas — headern skrivs alltid om, så skriptet
# går att köra igen för att laga en trasig eller bortglömd ota_pubkey.h utan att
# byta nyckel och därmed låsa ute varje lampa som redan går på den gamla.
if [ -e "$KEY" ]; then
    echo "Nyckeln $KEY finns redan — behåller den, skriver bara om headern."
else
    openssl genrsa -out "$KEY" 2048 2>/dev/null
    chmod 600 "$KEY"
    echo "Genererade $KEY."
fi

PUB=$(openssl rsa -in "$KEY" -pubout 2>/dev/null)

cat > "$HDR" <<HEADER
#pragma once

// Publik nyckel för OTA-signaturkontroll. Genererad av
// tools/generate-ota-key.sh — redigera inte för hand.
//
// Den här filen SKA ligga i repot. En publik nyckel är publik; det är den
// privata motsvarigheten (ota_signing_key.pem, .gitignore:ad och lagrad som
// GitHub-secret OTA_SIGNING_KEY) som är hemligheten.
//
// Enheten vägrar installera firmware som inte är signerad med motsvarande
// privata nyckel. Saknas eller går nyckeln inte att tolka stängs self-update
// av helt — den faller aldrig tillbaka på att installera osignerat.
//
// Array och inte #define: ett makro slutar vid radslutet, så ett flerradigt
// raw-string-literal går inte att stoppa in i ett. Headern inkluderas bara av
// updater.cpp, så den interna länkningen kostar ingenting.
static const char OTA_PUBLIC_KEY_PEM[] = R"PEM(
$PUB
)PEM";
HEADER

echo "Skrev $HDR (publik, committa)."
echo
echo "Lägg in den privata nyckeln som GitHub-secret:"
echo "    gh secret set OTA_SIGNING_KEY < $KEY"
