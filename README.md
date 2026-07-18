# THsip

Nativní macOS arm64 klient pro veřejně dostupné funkce [Odorik.cz](https://www.odorik.cz/w/api). Qt 6/QML + C++20 + PJSUA2; žádný Linphone, private API, DOM scraping ani automatizace samoobsluhy.

Závazné zdroje:

- [produktový brief](chatgpt-conversation://6a5b1d34-c384-83eb-b361-56f5bb62e5f6)
- [úplný katalog Odorik funkcí](/Users/samuel/.codex/attachments/1564cad6-7410-4888-a26e-352cb0fdc7ea/pasted-text.txt)
- [implementační plán](/Users/samuel/.codex/attachments/6e9f032d-f8ce-44c8-a83e-186dbf9721f1/pasted-text.txt)
- [PJSUA2 2.17](https://docs.pjsip.org/en/2.17/pjsua2/intro.html)

## Lokální build

Požadavky: macOS arm64, CMake 3.28+, Qt 6.11.x. Bez PJSIP lze ověřit REST, DB, katalog, dial compiler a portál:

```sh
cmake --preset macos-arm64-debug -DTHSIP_WITH_PJSIP=OFF
cmake --build --preset debug
ctest --preset debug
open build/debug/thsip.app
```

Výchozí build připne a stáhne PJSIP 2.17 přes CMake `FetchContent`; instalovaný `Pj::pjsua2` má přednost. Rychlý UI/REST build použije `-DTHSIP_WITH_PJSIP=OFF`. Produkční app bundle nesmí záviset na Homebrew.

## Bridge

```sh
cd bridge
npm install
npm test
npm run build
```

Produkce vyžaduje `DATABASE_URL`, `S3_BUCKET`, `INGRESS_TOKEN`, `CLIENT_TOKEN`; volitelně `S3_REGION`, `S3_ENDPOINT`, `PGSSL=disable` jen pro lokální PostgreSQL. Migrace spouštějte číselně: `bridge/migrations/001_initial.sql`, potom `002_recording_retention.sql`.

## Stav capability

`resources/capabilities.json` je strojově kontrolovaný coverage ledger. Každá položka má mechanismus, stav, zdroj, UI route a test ID. `implemented` znamená vytvoření podporovaného Odorik požadavku; `blocked-*` vyžaduje SDK, účet nebo aktivaci Odorikem. Web-only funkce otevírá pouze oficiální portál.

`resources/api_endpoints.json` obsahuje všechny veřejné Odorik REST operace včetně parametrů a dynamických cest. UI z něj vytváří kompletní API explorer a samostatné SMS, SIM a routing obrazovky. Citlivé mutace kontroluje C++ vrstva a vyžaduje potvrzovací příznak; QML dialog není jediná ochrana.

Kontakty lze načíst přes macOS Contacts.framework nebo importovat z CSV/vCard. Ve vcpkg buildu normalizuje veřejná čísla Google libphonenumber; SIP URI, šestimístné Odorik linky a hvězdičkové kódy zůstávají beze změny.

`resources/dial_actions.json` řídí pojmenované servisní, diagnostické, routingové, IVR, pickup, callback, voicemail a queue akce. Parametry validuje C++ podle ukotvených regulárních výrazů; advanced akce bez vědomého přepínače nevygeneruje.

Routing studio načítá typované veřejné linky, ringings a caller routes. Skládá delayed/main/random/time/concurrency, voicemail, greeting, queue a web-IVR cíle, ukazuje přesný dial string a před zápisem odmítá přímé smyčky i duplicity. Mazání vyžaduje potvrzení v UI i C++.

Nahrávky spojují lokální PJSUA2 WAV a Bridge uploady podle Call-ID. Bridge token je v Keychainu; přehrání používá Qt Multimedia, export atomický zápis, notes/retention se synchronizují a mazání vyžaduje potvrzení. Lokální soubor jde do Koše, Bridge smaže S3 objekt i DB řádek.

Přehled načítá jen oficiální read-only API: kredit, denní/týdenní/měsíční statistiky, aktivní a zmeškané hovory, linky, SIM a měsíční náklady podle linek a destinací. `sip_password` z odpovědi se před předáním do QML odstraní.

Diagnostika ukazuje PJSIP transporty/registrace/NAT události, média, zařízení, DNS a TCP/TLS porty 443/5060/6688/6699/5061/6689. Obsahuje test tónu, mikrofonní loopback, kameru a Odorik *080/*081/*0811. Export maskuje secrets, SIP identity, IP adresy a dlouhá čísla.

Citlivé údaje nejsou v repo ani SQLite. macOS ukládá API heslo přímo do systémového Keychainu; Windows/Linux target zapojí QtKeychain. Aktuální lokální build používá Qt `QSQLITE`; produkční build gate musí dodat a reopen testem ověřit SQLCipher Qt driver. Samotná položka `sqlcipher` ve `vcpkg.json` šifrování Qt databáze nezapíná.

## Licence

Interní distribuce nemění licenční povinnosti. Veřejný closed-source build vyžaduje vhodnou [PJSIP licenci](https://www.pjsip.org/licensing.htm) a splnění [Qt licence](https://doc.qt.io/qt-6/licensing.html).

## Podepsání a distribuce

Vývojový release je arm64. Přenosný bundle vyžaduje kompletní oficiální Qt 6.11 SDK; Homebrew Qt v tomto workspace postrádá moduly, které `macdeployqt` při deployment scan očekává. Potom:

```sh
macdeployqt build/release/thsip.app -qmldir=qml
codesign --force --deep --options runtime --entitlements macos/THsip.entitlements --sign "$THSIP_SIGNING_IDENTITY" build/release/thsip.app
ditto -c -k --keepParent build/release/thsip.app THsip.zip
xcrun notarytool submit THsip.zip --keychain-profile THsip --wait
xcrun stapler staple build/release/thsip.app
```

Signing identita a notarizační profil nejsou součást repo.
