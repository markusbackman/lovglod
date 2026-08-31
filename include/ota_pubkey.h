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
-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA0aqG3Mpzj/7cxyl9HvxC
ogqjJCXCG8V9i49PwrNksU0KTuibiLin/e4yPNlxfwm4IzpQ4aTEwrIQmA3glZ2t
QSBxvvmg19Eg72uZdkomwCVSUcbnQkfumV7zc/wiyrN2qGBYb0l5EXB98CKhwNru
lSiLCnkKhJovCaRnbZupq0TYTYK6r5Xkr5nW5772xvQIg2+J3JqfBYaq1yVfdXkx
BDLD/VK6wrKu2nzfCUBfMdmIwvc8PjPfq8xRICjnu30FBIfcWFOjOxDqiXTjDqnY
bBF78x+yg4AyHqG6B3wiXDMnhYUyi6ecVgbYgQM4FzWWOG/6ZlAfzuTtV0r3ik8T
XQIDAQAB
-----END PUBLIC KEY-----
)PEM";
