# THsip

Nativní macOS arm64 klient pro veřejně dostupné funkce [Odorik.cz](https://www.odorik.cz/w/api). Qt 6/QML + C++20 + PJSUA2; žádný Linphone, private API, DOM scraping ani automatizace samoobsluhy.

Závazné zdroje:

- [produktový brief](chatgpt-conversation://6a5b1d34-c384-83eb-b361-56f5bb62e5f6)
- [coverage ledger](resources/capabilities.json)
- [veřejné REST operace](resources/api_endpoints.json)
- [Odorik dial a fórum-only akce](resources/dial_actions.json)
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

Release, testy a přenositelný ZIP:

```sh
cmake --preset macos-arm64-release
cmake --build --preset release -j 3
ctest --test-dir build/release --output-on-failure
scripts/package-macos.sh
```

## Bridge

```sh
cd bridge
npm install
npm test
npm run build
```

Produkce vyžaduje `DATABASE_URL`, `S3_BUCKET`, `INGRESS_TOKEN`, `CLIENT_TOKEN`; volitelně `S3_REGION`, `S3_ENDPOINT`, `PGSSL=disable` jen pro lokální PostgreSQL. Migrace spouštějte číselně: `bridge/migrations/001_initial.sql`, potom `002_recording_retention.sql`.

Bridge přijímá oficiální webhook sloty 98/99, uploady nahrávek a web-IVR `*007X`. IVR editor validuje `answer`, `play`, `play2`, `playnumber`, `dial`, `setclip`, `hangup` a `uri`; živé události posílá klientovi přes autorizovaný WebSocket.

## Stav capability

`resources/capabilities.json` je strojově kontrolovaný coverage ledger. Každá položka má mechanismus, stav, zdroj, UI route a test ID. `implemented` znamená vytvoření podporovaného Odorik požadavku; `blocked-*` vyžaduje SDK, účet nebo aktivaci Odorikem. Web-only funkce otevírá pouze oficiální portál.

`resources/api_endpoints.json` obsahuje všechny veřejné Odorik REST operace včetně parametrů a dynamických cest. UI z něj vytváří kompletní API explorer a samostatné SMS, SIM a routing obrazovky. Citlivé mutace kontroluje C++ vrstva a vyžaduje potvrzovací příznak; QML dialog není jediná ochrana.

Historie hovorů podporuje období, směr, stav, linku, číslo, stránkování přes `Odorik-Pages`, lokální cache a potvrzené vzdálené zavěšení aktivní větve. SMS obrazovka načítá povolené identity a metadata historie; text nově odeslaných zpráv a šablony drží lokálně, protože Odorik text historie neposkytuje. SIM obrazovka nabízí typovaný detail, datovou historii a potvrzené změny stavů, balíčků, roamingu, LTE, prémiových služeb, kreditu, FUP restartu a přiřazení ICCID.

Kontakty lze načíst přes macOS Contacts.framework nebo importovat z CSV/vCard. Ve vcpkg buildu normalizuje veřejná čísla Google libphonenumber; SIP URI, šestimístné Odorik linky a hvězdičkové kódy zůstávají beze změny.

`resources/dial_actions.json` řídí pojmenované servisní, diagnostické, routingové, IVR, pickup, callback, voicemail a queue akce. Parametry validuje C++ podle ukotvených regulárních výrazů; advanced akce bez vědomého přepínače nevygeneruje.

Routing studio načítá typované veřejné linky, ringings a caller routes. Skládá delayed/main/random/time/concurrency, voicemail, greeting, queue a web-IVR cíle, ukazuje přesný dial string a před zápisem odmítá přímé smyčky i duplicity. Mazání vyžaduje potvrzení v UI i C++.

Nahrávky spojují lokální PJSUA2 WAV a Bridge uploady podle Call-ID. Bridge token je v Keychainu; přehrání používá Qt Multimedia, export atomický zápis, notes/retention se synchronizují a mazání vyžaduje potvrzení. Lokální soubor jde do Koše, Bridge smaže S3 objekt i DB řádek.

Přehled načítá jen oficiální read-only API: kredit, denní/týdenní/měsíční statistiky, aktivní a zmeškané hovory, linky, SIM a měsíční náklady podle linek a destinací. `sip_password` z odpovědi se před předáním do QML odstraní.

Diagnostika ukazuje PJSIP transporty/registrace/NAT události, média, zařízení, DNS a TCP/TLS porty 443/5060/6688/6699/5061/6689. Obsahuje test tónu, mikrofonní loopback, kameru a Odorik *080/*081/*0811. Export maskuje secrets, SIP identity, IP adresy a dlouhá čísla.

macOS adaptér používá Notification Center, Dock badge, systémový ringtone, oprávnění mikrofonu/kamery a IOKit sleep assertion po dobu hovoru. Vše je za zachovaným `IPlatformIntegration`; ostatní platformy mají bezpečný stub pro budoucí nativní adaptéry.

PJSUA2 endpoint, transporty, příkazy, callbacky i destrukce objektů běží na jediném telephony workeru. Qt/QML dostává jen hodnotové události přes queued connections; GUI thread nikdy přímo nevolá PJSIP.

Citlivé údaje nejsou v repo ani plaintext SQLite. macOS ukládá API heslo i náhodný 256bit databázový klíč do systémového Keychainu; Windows/Linux target zapojí QtKeychain. Release bundle přepíná Qt `QSQLITE` driver na bundlovaný SQLCipher, starou plaintext DB jednorázově atomicky migruje a fail-closed self-testem ověřuje šifrované znovuotevření. Debug build ponechává běžný `QSQLITE` pro rychlý vývoj.

## Licence

THsip je vydán pod GPL-3.0-or-later; viz [LICENSE](LICENSE). Jednotlivé závislosti mají vlastní licence. Alternativní closed-source distribuce vyžaduje vhodnou [PJSIP licenci](https://www.pjsip.org/licensing.htm) a splnění [Qt licence](https://doc.qt.io/qt-6/licensing.html).

## Podepsání a distribuce

`scripts/package-macos.sh` vytvoří ad-hoc podepsaný `build/package/THsip.zip`. Pro Developer ID podpis a notarizaci nastavte:

```sh
THSIP_SIGNING_IDENTITY="Developer ID Application: …" \
THSIP_NOTARY_PROFILE=THsip \
scripts/package-macos.sh
```

Signing identita a notarizační profil nejsou součást repo. Stejný build, test a package gate běží v [GitHub Actions](.github/workflows/ci.yml).
