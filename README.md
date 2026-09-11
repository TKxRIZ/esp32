# ESP32 OLED-Infodisplay mit Konfigurationsportal & OTA

Firmware für ein ESP32 DevKit v1 mit SH1106-OLED. Das Gerät verbindet sich mit
dem WLAN und zeigt Netzwerk-Infos auf dem Display an. Konfiguriert wird alles
bequem über ein Web-Portal, und neue Firmware lässt sich drahtlos (OTA)
aufspielen – ohne USB-Kabel.

## Features

- **4 Anzeige-Screens**, umschaltbar per BOOT-Button:
  1. Öffentliche IPv4 (via `api.ipify.org`)
  2. Lokale IPv4
  3. WLAN-Signalstärke (dBm + Balken)
  4. Animation (Sinuswelle mit Ball)
- **Web-Konfigurationsportal** unter der lokalen IP bzw. `http://<hostname>.local`
  - WLAN-SSID/-Passwort (mit Netz-Scan), Gerätename, Start-Screen,
    Öffentliche-IP-Intervall, Portal-Passwort (Basic-Auth)
  - Einstellungen dauerhaft im Flash (NVS/Preferences)
- **AP-Fallback**: Bei nicht erreichbarem WLAN öffnet das Gerät ein eigenes Netz
  `ESP32-Setup` mit Captive Portal (`http://192.168.4.1`)
- **OTA-Updates**: drahtlos aus PlatformIO (ArduinoOTA) oder per `.bin`-Upload
  im Browser (`/update`)
- **Werksreset**: BOOT beim Einschalten ~2 s gedrückt halten

## Hardware

| Komponente        | Detail                                             |
|-------------------|----------------------------------------------------|
| Board             | ESP32 DevKit v1 (ESP32-WROOM-32 / ESP32-D0WD-V3)   |
| USB-Chip          | CP2102 (Port z. B. `/dev/cu.usbserial-0001`)       |
| Display           | OLED 128×64, Controller **SH1106**, I²C-Adresse `0x3C` |
| Flash             | 4 MB                                               |

### Verkabelung (OLED ↔ ESP32)

| OLED | ESP32          |
|------|----------------|
| VCC  | 3V3            |
| GND  | GND            |
| SDA  | GPIO **21**    |
| SCL  | GPIO **22**    |

> **Wichtig:** Das Display ist ein **SH1106** (nicht SSD1306). Mit dem falschen
> Treiber gibt es Rauschen/Streifen. Der Code nutzt daher die U8g2-Library mit
> `U8G2_SH1106_128X64_NONAME_F_HW_I2C`.

## Projektstruktur

```
.
├── platformio.ini            # Build-Config (USB- + OTA-Environment)
├── platformio_local.ini      # OTA-Passwort (NICHT im Git) – aus .example erstellen
├── src/
│   ├── main.cpp              # gesamte Firmware
│   ├── secrets.h             # WLAN-/OTA-Zugangsdaten (NICHT im Git)
│   └── secrets.h.example     # Vorlage für secrets.h
└── .gitignore
```

## Voraussetzungen

- [PlatformIO Core](https://platformio.org/) (`pio`)
- Installiert wurde es hier via pip:
  ```bash
  python3 -m pip install --user platformio esptool
  ```
  Die Tools liegen dann unter `~/Library/Python/3.x/bin` – ggf. in den PATH:
  ```bash
  export PATH="$HOME/Library/Python/3.13/bin:$PATH"
  ```

## Erstinbetriebnahme

Die Zugangsdaten sind aus dem Repo ausgelagert. Nach dem Klonen einmalig die
Vorlagen kopieren und eigene Werte eintragen:

```bash
cp src/secrets.h.example src/secrets.h
cp platformio_local.ini.example platformio_local.ini
# danach in beiden Dateien WLAN- bzw. OTA-Passwort setzen
# (OTA_PASSWORD_STR in secrets.h muss zu --auth in platformio_local.ini passen)
```

## Flashen

### Per USB (erstes Mal / Rettungsanker)

```bash
pio run -e esp32doit-devkit-v1 -t upload
```

### Drahtlos über WLAN (OTA, aus PlatformIO)

```bash
pio run -e ota -t upload
```

Ziel ist `esp32-oled.local` (in `platformio.ini` als `upload_port`). Bei
mDNS-Problemen dort stattdessen die feste IP eintragen.

> ⚠️ **`pio run -t upload` ohne `-e`** baut/flasht **beide** Environments
> nacheinander (erst USB, dann OTA). Für gezieltes Flashen immer `-e` angeben.

### Drahtlos über den Browser (Web-Upload)

1. `pio run -e esp32doit-devkit-v1` baut die Datei
   `.pio/build/esp32doit-devkit-v1/firmware.bin`
2. Im Portal auf **„OTA-Update"** klicken bzw. `http://esp32-oled.local/update`
3. Die `.bin` hochladen – das Gerät flasht sich selbst und startet neu

### Serieller Monitor

```bash
pio device monitor        # 115200 Baud, im echten Terminal
```

## Konfigurationsportal

Im Browser (gleiches WLAN) die auf dem OLED angezeigte IP oder
`http://esp32-oled.local/` öffnen. Einstellbar:

| Feld                    | Beschreibung                                        |
|-------------------------|-----------------------------------------------------|
| WLAN-SSID / -Passwort   | Passwort leer lassen = unverändert                  |
| Gerätename / Hostname   | mDNS-Name → `http://<name>.local`                   |
| Start-Screen            | Welcher Screen nach dem Einschalten zuerst kommt    |
| Öff.-IP-Intervall       | manuell / 5 / 15 / 60 min                           |
| Portal-Passwort         | schützt Portal & Web-Upload (Benutzer: `admin`)     |

Speichern → das Gerät startet neu und übernimmt die Werte.

## Bedienung am Gerät

- **BOOT-Button (GPIO0):** schaltet durch die 4 Screens (Punkte unten zeigen die Position)
- **EN-Button:** Reset
- **Werksreset:** BOOT beim Einschalten ~2 s halten → alle Einstellungen gelöscht,
  danach AP-Setupmodus

## Troubleshooting

- **Display zeigt Rauschen/Streifen:** falscher Controller. Dieser Code nutzt
  SH1106. Für echte SSD1306-Displays in `src/main.cpp` den Konstruktor auf
  `U8G2_SSD1306_128X64_NONAME_F_HW_I2C` umstellen.
- **Zeichensalat beim seriellen Auslesen mit eigenen Skripten:** Beim Öffnen des
  Ports löst pyserial DTR/RTS aus und triggert die Auto-Reset-Schaltung falsch.
  Zuverlässig ist `pio device monitor`, oder vorher
  `esptool --port <port> --after hard_reset run`.
- **Boot-Loop nach Experimenten mit GPIOs:** Die Strapping-Pins 0, 2, 5, 12, 15
  nicht beim Booten auf ungünstige Pegel ziehen (GPIO12 kann die Flash-Spannung
  auf 1,8 V stellen → Boot-Loop).
- **OTA schlägt fehl:** Prüfen, ob `--auth` in `platformio_local.ini` zu
  `OTA_PASSWORD_STR` in `secrets.h` passt. Im Zweifel per USB flashen.
