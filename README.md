📡 ESP-Toshiba-IR-Controller

Inteligentní IR bridge pro klimatizace Toshiba (WH-L11SE protokol)
ESP8266 / ESP8285 – IR odesílání, WebUI, MQTT, Modbus TCP

✨ Funkce

Ovládání klimatizace Toshiba přes IR
Bez originálního ovladače (kompatibilní s WH-L11SE IR protokolem).
Odesílání plně vygenerovaných IR rámců (zap/vyp, teplota, režim, ventilátor, swing…).

WebUI
Moderní a responzivní uživatelské rozhraní:

Režimy s ikonami (Auto / Chlazení / Topení / Dry / Ventilátor)

Slider teploty s live náhledem

Výkon: ECO / NORMAL / HI-POWER

Ventilátor (Auto / Quiet / Low / Medium / High / Max)

Swing (Fix / V-swing / H-swing / Auto)

MQTT + Modbus TCP konfigurace

Volitelný identifikátor zařízení (pro víc jednotek v LAN)

MQTT

Publikace stavu (/state)

Příjem příkazů (/set/...)

Podpora MQTT AUTH (user/pass)

Automatické obnovení spojení

Modbus TCP

Standardní Modbus TCP server na portu 502

Holding registry pro zápis (power, mode, fan, swing, setpoint)

Lze připojit na Loxone, Home Assistant, OpenHAB…

Toshiba IR generátor (WH-L11SE)
Vlastní implementace enkódování 72bit / 9-byte rámců
(včetně správného OFF rámce, který knihovna IRremoteESP8266 neumí).

Hybridní režim IR

Zapnutí / změna parametrů: knihovna IRToshibaAC

Vypnutí (OFF): vlastní ToshibaIrGenerator → 100% kompatibilita

📦 Hardwarové požadavky

ESP8266 / ESP8285

IR LED + tranzistor (např. 2N2222, 2N2222A)
– doporučeno kvůli výkonu pro 8–10 m dosah

Rezistor + napájení 3.3 V

Volitelně: krabička, externí IR LED

🧱 Struktura projektu
Soubor	Popis
ESP-Toshiba-IR-Controller.ino	Hlavní aplikace, WebUI, konfigurace, routy, Modbus TCP, MQTT
IRControl.cpp / .h	Vysílání IR rámců (hybridní režim)
ToshibaIrGenerator.cpp / .h	100% implementace Toshiba WH-L11SE rámců
AcState.cpp / .h	Virtuální stav klimatizace
MqttHandler.cpp / .h	MQTT klient (auth, reconnect, publish)
ModbusHandler.cpp / .h	Modbus TCP server a registry
🌐 Webové rozhraní
Hlavní vlastnosti

Responzivní UI (desktop, tablet, mobil)

Moderní design (dark theme)

Ikony režimů:

⚙ Auto

❄ Chlazení

🔥 Topení

💧 Dry

🌀 Ventilátor

Nastavení teploty přes slider (17–30 °C)

Tří-stavový výkon:

ECO (50%)

NORMAL (75%)

HI-POWER (100%)

Konfigurace

⬥ MQTT host / port / topic / user / pass

⬥ Povolit / zakázat MQTT

⬥ Modbus TCP (Unit ID, enable/disable)

⬥ Device ID – pro LAN s více IR bridge jednotkami

📡 MQTT
Topics (default)
toshiba/ac/state
toshiba/ac/set/power
toshiba/ac/set/mode
toshiba/ac/set/temp
toshiba/ac/set/fan
toshiba/ac/set/swing
toshiba/ac/set/pselect
toshiba/ac/heartbeat

Příklady příkazů

Zapnutí:

{"power":"on"}


Nastavení teploty:

{"temp":23}


Výkon HI-POWER:

{"pselect":"100%"}

🧰 Modbus TCP registry

Plně kompatibilní se šablonou pro Loxone.

Holding registry (zápis)
Adresa	Název	Hodnoty
HR0	Power	0=off, 1=on
HR1	Setpoint	17–30
HR2	Mode	0 auto, 1 cool, 2 heat, 3 dry, 4 fan, 5 off
HR3	Fan	0 auto,1 quiet,2 low,3 medium,4 high,5 max
HR4	Swing	0 fix,1 v,2 h,3 hv,4 auto

Každý zápis okamžitě odesílá IR rámec.

Čtení (read-only)

HR0–HR4: aktuální stav (virtuální)

HR7: „operating state“

HR8: flags

HR9: error

HR10: status bits

HR11: uptime

🔧 IR protokol Toshiba WH-L11SE

Projekt obsahuje kompletní implementaci:

9-byte frame

XOR checksum

mode bits, temp nibble, fan bits, swing codes

OFF mód (mode=7) → nutný pro vypnutí jednotky

Rámce lze generovat čistě softwarově, bez potřeby knihovny.

🚀 Instalace
1. Překlad & upload

PlatformIO nebo Arduino IDE

Board: ESP8266 (Wemos, Witty, ESP-12F, ESP-01S…)

Připoj IR LED + tranzistor

Nahraj FW

2. První spuštění

ESP vytvoří AP nebo se připojí k uložené WiFi

Otevři WebUI ve webovém prohlížeči

Nastav:

WiFi

MQTT / Modbus

Device ID

Režim ovládání AC

📦 Integrace s Loxone

K dispozici je hotová LoxConfig Modbus šablona:

Power (write)

Setpoint (write)

Mode / Fan / Swing (write)

Všechny stavy (read)

Automatické odeslání IR po zápisu registru

Loxone pak může ovládat klimatizaci jako standardní Modbus zařízení.
