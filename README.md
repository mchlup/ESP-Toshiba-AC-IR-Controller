# ESP IR Controller

Firmware pro ESP8266/ESP32, které umožňuje učit se IR kódy z libovolných ovladačů a z webového rozhraní je následně odesílat. Kód je neblokující a používá webové API pro správu uložených kódů.

## Funkce

- Učení IR kódu na vyžádání přes webové rozhraní.
- Ukládání naučených kódů do paměti zařízení (LittleFS).
- Odesílání kódů dle názvu zařízení / funkce.
- Přehled uložených kódů a jejich mazání přes webové UI.
- REST API pro integraci s dalšími systémy.

## Hardwarové požadavky

- Vývojová deska ESP8266 (např. NodeMCU) nebo ESP32.
- IR přijímač připojený na pin definovaný v `IR_RECV_PIN` (výchozí GPIO14 / D5).
- IR LED s tranzistorovým budičem připojená na pin `IR_SEND_PIN` (výchozí GPIO4 / D2).
- Napájení 3.3 V a společná zem.

## Sestavení

Projekt používá PlatformIO. Po úpravě souboru `src/main.cpp` s přihlašovacími údaji k Wi-Fi stačí spustit:

```bash
pio run -t upload
```

Výchozí nastavení obsahuje prostředí pro ESP8266 (`nodemcuv2`) i ESP32 (`esp32dev`).

## Webové rozhraní

Po připojení k Wi-Fi otevře zařízení webový server na portu 80. Hlavní stránka umožňuje:

- zahájit učení kódu (zadejte název a stiskněte "Spustit učení"),
- zobrazit seznam uložených kódů,
- poslat nebo smazat vybraný kód.

Status sekce průběžně informuje o stavu připojení a posledním naučeném kódu.

## REST API

| Metoda | Cesta | Popis |
| ------ | ----- | ----- |
| GET | `/api/codes` | Vrátí seznam uložených kódů. |
| POST | `/api/learn` | Zahájí učení. JSON tělo `{ "name": "TV On" }`. |
| POST | `/api/send` | Odešle kód dle názvu. JSON tělo `{ "name": "TV On" }`. |
| DELETE | `/api/codes?name=TV%20On` | Smaže uložený kód. |
| GET | `/api/status` | Informace o stavu učení a posledním zachyceném kódu. |

## Poznámky

- V nastavení vyplňte SSID a heslo vaší Wi-Fi sítě (`WIFI_SSID`, `WIFI_PASSWORD`).
- Učení se automaticky ukončí po 60 s, pokud není zachycen žádný kód.
- Kódy známých protokolů jsou ukládány společně se surovými daty, takže je možné je reprodukovat i pro neznámé protokoly.
