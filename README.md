# ESP IR Controller

Firmware pro ESP8266/ESP32, které umožňuje učit se IR kódy z libovolných ovladačů a z webového rozhraní je následně odesílat. Kód je neblokující a používá webové API pro správu uložených kódů.

## Funkce

- Učení IR kódu na vyžádání přes webové rozhraní.
- Ukládání naučených kódů do paměti zařízení (LittleFS).
- Odesílání kódů dle názvu zařízení / funkce.
- Přehled uložených kódů a jejich mazání přes webové UI.
- REST API pro integraci s dalšími systémy.
- Správa připojení k Wi-Fi přes portál [WiFiManager](https://github.com/tzapu/WiFiManager) včetně vlastních parametrů zařízení.

## Hardwarové požadavky

- Vývojová deska ESP8266 (např. NodeMCU) nebo ESP32.
- IR přijímač připojený na pin definovaný v `IR_RECV_PIN` (výchozí GPIO14 / D5).
- IR LED s tranzistorovým budičem připojená na pin `IR_SEND_PIN` (výchozí GPIO4 / D2).
- Napájení 3.3 V a společná zem.

## Příprava projektu pro Arduino IDE

1. Naklonujte repozitář a otevřete soubor `ESPToshibaACIRController.ino` v Arduino IDE. (IDE vás může požádat o přejmenování složky na `ESPToshibaACIRController`.)
2. V menu **Nástroje → Deska** zvolte odpovídající desku ESP8266 nebo ESP32 a nakonfigurujte správný port.
3. Do adresáře `sketchbook/libraries` nainstalujte (např. přes Správce knihoven) následující knihovny:
   - [ArduinoJson](https://arduinojson.org/) verze 6 nebo novější,
   - [IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266) (automaticky poskytuje `IRrecv`/`IRsend`),
   - [WiFiManager](https://github.com/tzapu/WiFiManager),
   - `LittleFS` (ESP8266 používá vestavěnou implementaci, pro ESP32 nainstalujte knihovnu [LittleFS_esp32](https://github.com/lorol/LITTLEFS)).
4. Nahrajte firmware standardním tlačítkem **Nahrát**.

Po nahrání se zařízení přepne do režimu konfigurace Wi-Fi (přístupový bod `ESP-IR-Bridge`, bez hesla). Připojte se k němu, otevřete `http://192.168.4.1/` a v portálu WiFiManageru zadejte přístupové údaje k vaší síti. Na stejné stránce najdete také vlastní parametry:

- **Název zařízení** – text použitý v uživatelském rozhraní a odpovědích API.
- **Doba učení (ms)** – maximální délka čekání na zachycení IR kódu.

Po uložení se zařízení restartuje a připojí k vybrané síti. Konfigurace se ukládá do souboru `/config.json` v LittleFS (kódy zůstávají v `/codes.json`). Pokud chcete vymazat konfiguraci, smažte tyto soubory nebo spusťte `LITTLEFS.format()`.

## Webové rozhraní

Po připojení k Wi-Fi otevře zařízení webový server na portu 80. Hlavní stránka umožňuje:

- zahájit učení kódu (zadejte název a stiskněte "Spustit učení"),
- zobrazit seznam uložených kódů,
- poslat nebo smazat vybraný kód.

Status sekce průběžně informuje o stavu připojení, posledním naučeném kódu a využívá nastavený název zařízení.

## REST API

| Metoda | Cesta | Popis |
| ------ | ----- | ----- |
| GET | `/api/codes` | Vrátí seznam uložených kódů. |
| POST | `/api/learn` | Zahájí učení. JSON tělo `{ "name": "TV On" }`. |
| POST | `/api/send` | Odešle kód dle názvu. JSON tělo `{ "name": "TV On" }`. |
| DELETE | `/api/codes?name=TV%20On` | Smaže uložený kód. |
| GET | `/api/status` | Informace o stavu učení a posledním zachyceném kódu. |

## Poznámky

- Pokud potřebujete změnit Wi-Fi síť nebo parametry zařízení, odpojte se od známé Wi-Fi (např. vypnutím routeru). Po několika neúspěšných pokusech o připojení WiFiManager automaticky znovu otevře konfigurační portál.
- Učení se automaticky ukončí po uplynutí nastaveného limitu (výchozí 60 s), pokud není zachycen žádný kód.
- Kódy známých protokolů jsou ukládány společně se surovými daty, takže je možné je reprodukovat i pro neznámé protokoly.

## Toshiba IR control (ESP32-C3 + IRremote 3.3.2)

Tento firmware obsahuje nativní vysílání IR kódů pro klimatizace Toshiba (kompaktní 72bit rámec). Pro správnou funkci:

1. **Zapojení IR LED** – zvolený GPIO (např. GPIO4) propojte přes rezistor 100–220 Ω na anodu IR LED, katodu veďte na GND.
2. **Nastavení TX pinu** – v hlavním WebUI na kartě nastavení zadejte číslo pinu a uložte. Změna okamžitě inicializuje jedinou instanci `IrSender`.
3. **Odeslání příkazu** – buď použijte panel „Toshiba IR“ ve WebUI, nebo REST API:

```
GET /api/toshiba_send?power=1&mode=heat&temp=23&fan=3
GET /api/toshiba_send?power=1&mode=cool&temp=25&fan=auto
GET /api/toshiba_send?power=0
```

Parametry `power`, `mode`, `temp` (17–30 °C) a `fan` (`auto` nebo 1–5) jsou volitelné; nevyplněné hodnoty doplní výchozí stav (zapnuto, auto, 24 °C, auto ventilátor).
