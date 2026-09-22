# ESP32 OLED-Infodisplay mit Konfigurationsportal & OTA

Firmware für ein ESP32 DevKit v1 mit SH1106-OLED. Das Gerät verbindet sich mit
dem WLAN und zeigt Netzwerk-Infos auf dem Display an. Konfiguriert wird alles
bequem über ein Web-Portal, und neue Firmware lässt sich drahtlos (OTA)
aufspielen – ohne USB-Kabel.

## Installation per Browser (für Endnutzer)

Am einfachsten geht's über die **Web-Install-Seite** – kein PlatformIO, keine
Toolchain, kein Terminal:

1. Die Seite in **Chrome** oder **Edge** (Desktop) öffnen:
   `https://tkxriz.github.io/esp32/`
2. ESP32 per USB anschließen, **„Firmware installieren"** klicken, Port wählen.
3. Nach dem Flashen mit dem WLAN **`ESP32-Setup`** verbinden und im Portal
   (`http://192.168.4.1`) das eigene WLAN eintragen. Fertig.

Danach hält sich das Gerät per **Auto-Update** von selbst aktuell (siehe unten).

> Die Install-Seite wird bei jedem Release automatisch über GitHub Pages
> bereitgestellt und flasht die jeweils neueste Release-Firmware. Web Serial
> funktioniert nur in Chrome/Edge (Desktop) über **HTTPS oder `localhost`**.

## Features

- **4 Anzeige-Screens**, umschaltbar per BOOT-Button:
  1. Öffentliche IPv4 (via `api.ipify.org`)
  2. Lokale IPv4
  3. WLAN-Signalstärke (dBm + Balken)
  4. Uhr (Datum + Uhrzeit per NTP, Zeitzone wählbar)
- **Web-Konfigurationsportal** unter der lokalen IP bzw. `http://<hostname>.local`
  - WLAN-SSID/-Passwort (mit Netz-Scan), Gerätename, Start-Screen,
    Öffentliche-IP-Intervall, Portal-Passwort (Basic-Auth)
  - Einstellungen dauerhaft im Flash (NVS/Preferences)
- **AP-Fallback**: Bei nicht erreichbarem WLAN öffnet das Gerät ein eigenes Netz
  `ESP32-Setup` mit Captive Portal (`http://192.168.4.1`)
- **Automatische Updates**: holt neue Firmware selbstständig von GitHub Releases
  (beim Start + täglich), plus manueller Update-Button im Portal
- **OTA-Updates**: drahtlos aus PlatformIO (ArduinoOTA) oder per `.bin`-Upload
  im Browser (`/update`)
- **Dreh-Encoder**: Screens durchdrehen, Tastendruck aktualisiert die öff. IP
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

### Verkabelung (Dreh-Encoder ↔ ESP32, optional)

| Encoder | ESP32       |
|---------|-------------|
| +       | 3V3         |
| GND     | GND         |
| CLK     | GPIO **18** |
| DT      | GPIO **19** |
| SW      | GPIO **5**  |

> GPIO 5 ist ein Strapping-Pin: den Encoder-Taster **nicht beim Einschalten
> gedrückt halten**.

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

> `pio run` / `pio run -t upload` **ohne `-e`** verwenden nur das Standard-Env
> `esp32doit-devkit-v1` (USB, Dev-Build). Die Envs `ota` und `installer` müssen
> explizit mit `-e` gewählt werden – so kann die neutrale Installer-Firmware nie
> versehentlich über den eigenen Dev-Stand geflasht werden.

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

## Sicherheitshinweise & bekannte Einschränkungen

Dies ist ein Hobby-Projekt für das eigene Heimnetz. Bewusst einfach gehalten:

- **TLS ohne Zertifikatsprüfung:** Update-Check und -Download laufen zwar über
  HTTPS, aber ohne CA-Pinning (`setInsecure()`). Firmware wird **nicht
  signiert**. Wer den Netzwerkpfad kontrolliert, könnte theoretisch eine fremde
  Firmware unterschieben. Für den Betrieb im eigenen WLAN vertretbar, nicht für
  unsichere Netze.
- **Setup-WLAN ist offen:** `ESP32-Setup` hat kein Passwort. Während der
  Einrichtung kann jeder in Reichweite das Portal aufrufen. Danach wird das
  Setup-Netz nicht mehr geöffnet (außer bei WLAN-Verlust).
- **Portal-Passwort ist optional** und schützt per HTTP-Basic-Auth (unverschlüsselt
  im LAN). Im AP-Modus ist das Portal bewusst ungeschützt (Recovery).
- **Versionsvergleich ist „ungleich = Update":** Das Gerät installiert immer das
  *neueste* Release, auch wenn es formal älter wäre. Keine Downgrades veröffentlichen.
- **Flash-Reserve:** Die App-Partition (1,31 MB) ist zu ~82 % belegt. Wächst die
  Firmware darüber hinaus, schlägt OTA fehl – dann wäre ein neues
  Partitionsschema + Neu-Flash per USB nötig.

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

## Firmware-Updates (OTA von GitHub Releases)

Die Firmware kann sich selbst über **GitHub Releases** aktualisieren:

- **Automatisch:** Ist im Portal „Automatische Firmware-Updates" aktiv (Standard),
  prüft das Gerät beim Start und danach täglich, ob ein neueres Release vorliegt,
  lädt es und flasht sich selbst (Fortschritt auf dem Display), dann Neustart.
- **Manuell:** Im Portal → Abschnitt **Firmware** → „Jetzt auf Updates prüfen &
  installieren". Alternativ weiterhin `.bin` per Browser hochladen (`/update`).

Das Gerät kennt seine eigene Version (`FIRMWARE_VERSION`, beim Release-Build aus
dem Git-Tag gesetzt), fragt `…/releases/latest` ab und lädt bei einer neueren
Version `…/releases/latest/download/firmware.bin`. **Dev-Builds** (Version
`dev`) aktualisieren sich nie automatisch.

> Voraussetzung: **öffentliches Repo** – Release-Assets privater Repos sind nicht
> ohne Token herunterladbar.

## Release veröffentlichen (für Maintainer)

Updates werden ausschließlich über Releases verteilt. Ein neues Release
veröffentlichen:

```bash
# Version taggen und Release anlegen (Beispiel v1.1.0)
gh release create v1.1.0 --title "v1.1.0" --notes "Was ist neu ..."
```

Das löst `.github/workflows/release.yml` aus. Der Workflow:

1. baut die **neutrale** Firmware mit `FIRMWARE_VERSION = <Tag>`
   (`pio run -e installer`, ohne persönliche Zugangsdaten),
2. hängt zwei Assets an den Release:
   - `firmware.bin` – App-Image für **OTA-Selbstupdate**
   - `firmware-merged.bin` – vollständiges Image für **USB/Browser-Installation**
3. deployt die **Install-Seite** (`docs/index.html` + `firmware-merged.bin`)
   nach GitHub Pages.

Danach ziehen Install-Seite **und** OTA automatisch das neueste Release.

### GitHub Pages aktivieren (einmalig, sobald das Repo öffentlich ist)

1. Repo → **Settings** → **Pages** → **Source:** „**GitHub Actions**"
2. **Tag-Deploys erlauben.** Der Release-Workflow läuft vom *Tag* (z. B.
   `v1.0.0`), das automatisch angelegte `github-pages`-Environment lässt aber
   standardmäßig nur den `main`-Branch deployen – der Deploy-Job scheitert
   sonst sofort. Settings → **Environments** → `github-pages` →
   *Deployment branches and tags* → Regel für **Tag** `v*` hinzufügen.
   Oder per CLI:
   ```bash
   gh api -X PUT  repos/<user>/esp32/environments/github-pages \
     --input - <<< '{"deployment_branch_policy":{"protected_branches":false,"custom_branch_policies":true}}'
   gh api -X POST repos/<user>/esp32/environments/github-pages/deployment-branch-policies \
     -f name='v*' -f type=tag
   ```
3. Ein Release veröffentlichen → Seite erscheint unter
   `https://tkxriz.github.io/esp32/`. Ein manueller „Run workflow" baut nur zum
   Test und deployt **nicht** (bewusstes Gate gegen versehentliche Dev-Builds
   auf der öffentlichen Seite).

### Lokal testen (vor der Veröffentlichung)

```bash
pio run -e installer                     # neutrale Firmware (SKIP_SECRETS)
# App- und Merged-Image nach docs/ bauen (siehe Merge-Schritt in release.yml),
# firmware-merged.bin nach docs/ kopieren, dann:
cd docs && python3 -m http.server 8123   # http://localhost:8123 in Chrome
```

Web Serial erlaubt `localhost`, damit lässt sich die Install-Seite auch **vor**
dem Veröffentlichen testen.

## Lizenz

[MIT](LICENSE) – frei nutzbar, veränderbar und weitergebbar; der
Copyright-Hinweis muss erhalten bleiben. Keine Gewährleistung.
