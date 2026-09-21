/*
 * ESP32 + SH1106 OLED: 4 Screens + Web-Konfigurationsportal.
 *
 *  Screens (BOOT-Button GPIO0 schaltet weiter):
 *    1) Oeffentliche IPv4   2) Lokale IPv4   3) WLAN-Signal   4) Animation
 *
 *  Konfigurationsportal:
 *    - STA-Modus (WLAN verbunden): Webserver unter der lokalen IP,
 *      zusaetzlich per mDNS erreichbar (http://<hostname>.local).
 *    - AP-Fallback (kein/falsches WLAN): eigenes Netz "ESP32-Setup",
 *      Captive Portal unter http://192.168.4.1
 *    - Einstellungen: SSID, Passwort, Hostname, Start-Screen,
 *      Oeff.-IP-Intervall, Portal-Passwort (Basic-Auth in STA).
 *    - Alle Einstellungen dauerhaft im Flash (Preferences/NVS).
 *
 *  Werksreset: BOOT beim Einschalten gedrueckt halten (~2 s).
 *
 *  OLED: SH1106 128x64, I2C 0x3C, SDA=21, SCL=22.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <math.h>
// secrets.h ist optional: Wenn vorhanden (lokale Entwicklung), liefert es die
// Standard-Zugangsdaten. Fehlt es (CI-/Installer-Build) oder ist SKIP_SECRETS
// gesetzt, bleiben die Defaults leer -> das Geraet startet im Setup-AP-Modus.
#if __has_include("secrets.h") && !defined(SKIP_SECRETS)
  #include "secrets.h"
#endif
#ifndef WIFI_SSID_DEFAULT
  #define WIFI_SSID_DEFAULT ""
#endif
#ifndef WIFI_PASS_DEFAULT
  #define WIFI_PASS_DEFAULT ""
#endif
#ifndef OTA_PASSWORD_STR
  #define OTA_PASSWORD_STR "esp32-ota"
#endif

// OTA-Passwort fuer PlatformIO/ArduinoOTA-Uploads (in platformio.ini als --auth).
static const char *OTA_PASSWORD = OTA_PASSWORD_STR;

// ---- Firmware-Version & Update-Quelle (GitHub Releases) ----
// FIRMWARE_VERSION wird beim Release-Build per Build-Flag gesetzt (Git-Tag).
// Ohne Flag (lokale Entwicklung) ist es "dev" -> kein Auto-Update.
#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION "dev"
#endif
static const char *GH_REPO = "TKxRIZ/esp32";   // <owner>/<repo> fuer Releases

// ---- OLED ----
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
static const int I2C_SDA = 21;
static const int I2C_SCL = 22;
static const int BTN     = 0;   // BOOT-Button, active-low

// ---- Dreh-Encoder (EC11) ----
static const int ENC_CLK = 18;  // CLK -> D18
static const int ENC_DT  = 19;  // DT  -> D19
static const int ENC_SW  = 5;   // SW  -> D5  (Achtung: GPIO5 ist Strapping-Pin,
                                //             beim Booten nicht gedrueckt halten)

// ---- Persistente Konfiguration ----
Preferences prefs;
struct Config {
  String   ssid;
  String   pass;
  String   host      = "esp32-oled";
  int      startScr  = 1;       // 0..3
  int      pubIntMin = 15;      // 0 = nur manuell
  String   portalPw;            // leer = kein Schutz
  bool     autoUpdate = true;   // automatische Firmware-Updates von GitHub
} cfg;

void loadConfig() {
  prefs.begin("cfg", true);
  // Vorbelegung mit den bekannten Zugangsdaten -> verbindet direkt beim 1. Start.
  // Sobald im Portal gespeichert wird, gelten die dort eingegebenen Werte.
  cfg.ssid      = prefs.getString("ssid", WIFI_SSID_DEFAULT);
  cfg.pass      = prefs.getString("pass", WIFI_PASS_DEFAULT);
  cfg.host      = prefs.getString("host", "esp32-oled");
  cfg.startScr  = prefs.getInt("startscr", 1);
  cfg.pubIntMin = prefs.getInt("pubint", 15);
  cfg.portalPw  = prefs.getString("portalpw", "");
  cfg.autoUpdate = prefs.getBool("autoupd", true);
  prefs.end();
}
void saveConfig() {
  prefs.begin("cfg", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("host", cfg.host);
  prefs.putInt("startscr", cfg.startScr);
  prefs.putInt("pubint", cfg.pubIntMin);
  prefs.putString("portalpw", cfg.portalPw);
  prefs.putBool("autoupd", cfg.autoUpdate);
  prefs.end();
}
void factoryReset() {
  prefs.begin("cfg", false);
  prefs.clear();
  prefs.end();
}

// ---- Laufzeit ----
enum Mode { MODE_STA, MODE_AP };
Mode mode = MODE_STA;

WebServer server(80);
DNSServer dns;

enum Screen { SCR_PUBLIC_IP, SCR_LOCAL_IP, SCR_RSSI, SCR_ANIM, SCR_COUNT };
int screen = SCR_LOCAL_IP;

String        publicIP = "";
unsigned long lastPublicFetch = 0;

// ---- Firmware-Update-Status ----
String        latestVersion = "";      // zuletzt ermittelte Release-Version
String        updateStatus  = "";      // letzte Meldung (fuer Portal)
unsigned long lastUpdateCheck = 0;
const uint32_t UPDATE_CHECK_INTERVAL = 24UL * 3600UL * 1000UL;  // 24 h

// ============================================================
//  OLED-Helfer
// ============================================================
void oledMsg(const char *l1, const char *l2, const char *l3 = "") {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(0, 12, l1);
  u8g2.setFont(u8g2_font_7x14B_tr);
  u8g2.drawStr(0, 34, l2);
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(0, 52, l3);
  u8g2.sendBuffer();
}

// ============================================================
//  WLAN
// ============================================================
void fetchPublicIP() {
  if (WiFi.status() != WL_CONNECTED) { publicIP = "keine Verb."; return; }
  HTTPClient http;
  http.setConnectTimeout(4000);
  http.begin("http://api.ipify.org/");
  int code = http.GET();
  if (code == 200) { publicIP = http.getString(); publicIP.trim(); }
  else             { publicIP = "Fehler " + String(code); }
  http.end();
  lastPublicFetch = millis();
  Serial.printf("Oeffentliche IPv4: %s\n", publicIP.c_str());
}

// ============================================================
//  Firmware-Update von GitHub Releases
// ============================================================

// Neueste Release-Version (Tag) via GitHub-API ermitteln. "" bei Fehler.
String fetchLatestVersion() {
  if (WiFi.status() != WL_CONNECTED) return "";
  WiFiClientSecure client;
  client.setInsecure();                 // ohne CA-Pinning (Hobby-Setup)
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  String url = "https://api.github.com/repos/" + String(GH_REPO) + "/releases/latest";
  if (!http.begin(client, url)) return "";
  http.addHeader("User-Agent", "esp32-oled-updater");   // von GitHub verlangt
  int code = http.GET();
  String tag = "";
  if (code == 200) {
    String body = http.getString();
    int i = body.indexOf("\"tag_name\"");
    if (i >= 0) {
      int q1 = body.indexOf('"', body.indexOf(':', i) + 1);
      int q2 = body.indexOf('"', q1 + 1);
      if (q1 >= 0 && q2 > q1) tag = body.substring(q1 + 1, q2);
    }
  } else {
    Serial.printf("Update-Check HTTP %d\n", code);
  }
  http.end();
  return tag;
}

// Firmware vom neuesten Release ziehen und flashen (rebootet bei Erfolg).
void performUpdate() {
  updateStatus = "Update laeuft...";
  oledMsg("Firmware-Update", "laedt...", latestVersion.c_str());
  Serial.println("Starte Firmware-Update von GitHub ...");

  WiFiClientSecure client;
  client.setInsecure();

  httpUpdate.rebootOnUpdate(true);
  httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  httpUpdate.onProgress([](int cur, int total) {
    int pct = total ? (int)((int64_t)cur * 100 / total) : 0;
    char b[16]; snprintf(b, sizeof(b), "%d %%", pct);
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_tr); u8g2.drawStr(0, 12, "Firmware-Update");
    u8g2.setFont(u8g2_font_9x15B_tr); u8g2.drawStr(0, 34, b);
    u8g2.drawFrame(0, 48, 128, 10);
    u8g2.drawBox(0, 48, pct * 128 / 100, 10);
    u8g2.sendBuffer();
  });

  String url = "https://github.com/" + String(GH_REPO) +
               "/releases/latest/download/firmware.bin";
  t_httpUpdate_return ret = httpUpdate.update(client, url);
  // Nur erreichbar, wenn KEIN Reboot erfolgte (also bei Fehler / no update):
  if (ret == HTTP_UPDATE_FAILED) {
    updateStatus = "Fehler: " + httpUpdate.getLastErrorString();
    Serial.printf("Update fehlgeschlagen: %s\n", httpUpdate.getLastErrorString().c_str());
    oledMsg("Update-Fehler", "", "");
  } else if (ret == HTTP_UPDATE_NO_UPDATES) {
    updateStatus = "Keine Aktualisierung noetig.";
  }
}

// Prueft auf neue Version; flasht bei Bedarf (auto) bzw. immer bei manual.
void checkForUpdate(bool manual) {
  lastUpdateCheck = millis();
  latestVersion = fetchLatestVersion();
  if (latestVersion == "") {
    updateStatus = "Update-Pruefung fehlgeschlagen.";
    Serial.println(updateStatus);
    return;
  }
  bool newer = latestVersion != String(FIRMWARE_VERSION);
  Serial.printf("Aktuell: %s | Neueste: %s | %s\n",
                FIRMWARE_VERSION, latestVersion.c_str(),
                newer ? "Update verfuegbar" : "aktuell");
  if (newer) {
    updateStatus = "Update " + latestVersion + " verfuegbar.";
    performUpdate();               // laedt & flasht (rebootet bei Erfolg)
  } else {
    updateStatus = "Firmware ist aktuell (" + String(FIRMWARE_VERSION) + ").";
  }
}

bool connectSTA(uint32_t timeoutMs) {
  if (cfg.ssid == "") return false;
  Serial.printf("Verbinde mit \"%s\" ...\n", cfg.ssid.c_str());
  oledMsg("WLAN:", cfg.ssid.c_str(), "verbinde...");
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(cfg.host.c_str());
  WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

// ============================================================
//  Web-Portal
// ============================================================
bool authOK() {
  if (mode == MODE_AP || cfg.portalPw == "") return true;   // AP-Recovery: kein Schutz
  if (!server.authenticate("admin", cfg.portalPw.c_str())) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

String htmlEscape(const String &s) {
  String o; o.reserve(s.length() + 8);
  for (char c : s) {
    if      (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else               o += c;
  }
  return o;
}

String optionRow(int val, int cur, const char *label) {
  return "<option value='" + String(val) + "'" +
         (val == cur ? " selected" : "") + ">" + label + "</option>";
}

String buildPage(const String &notice = "") {
  String ip   = (mode == MODE_STA) ? WiFi.localIP().toString() : "192.168.4.1";
  String rssi = (mode == MODE_STA) ? String(WiFi.RSSI()) + " dBm" : "-";

  String h;
  h.reserve(4000);
  h += "<!DOCTYPE html><html lang='de'><head><meta charset='utf-8'>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>ESP32 Konfiguration</title><style>";
  h += "body{font-family:system-ui,sans-serif;max-width:520px;margin:0 auto;padding:16px;background:#111;color:#eee}";
  h += "h1{font-size:1.3rem}h2{font-size:1rem;margin-top:1.4rem;color:#8fd}";
  h += "label{display:block;margin:.6rem 0 .2rem;font-size:.9rem}";
  h += "input,select{width:100%;padding:.5rem;border-radius:8px;border:1px solid #444;background:#1c1c1c;color:#eee;box-sizing:border-box}";
  h += "button{margin-top:1.2rem;width:100%;padding:.7rem;border:0;border-radius:8px;background:#2b8;color:#012;font-weight:700;font-size:1rem}";
  h += ".card{background:#1a1a1a;border:1px solid #333;border-radius:12px;padding:14px;margin-bottom:14px}";
  h += ".muted{color:#999;font-size:.82rem}.ok{color:#3d8}.chip{display:inline-block;background:#233;color:#8fd;border:1px solid #355;border-radius:6px;padding:.15rem .4rem;margin:.15rem;font-size:.82rem;cursor:pointer}";
  h += "</style></head><body>";
  h += "<h1>&#128225; ESP32 Konfiguration</h1>";
  if (notice != "") h += "<div class='card ok'>" + notice + "</div>";

  h += "<div class='card'><b>Status</b><br>";
  h += "<span class='muted'>Modus:</span> " + String(mode == MODE_STA ? "verbunden (STA)" : "Setup (AP)") + "<br>";
  h += "<span class='muted'>IP:</span> " + ip + "<br>";
  h += "<span class='muted'>Signal:</span> " + rssi + "<br>";
  h += "<span class='muted'>Oeff. IP:</span> " + (publicIP == "" ? String("-") : htmlEscape(publicIP)) + "<br>";
  h += "<span class='muted'>Hostname:</span> " + htmlEscape(cfg.host) + ".local</div>";

  h += "<form method='POST' action='/save'>";

  h += "<div class='card'><h2>WLAN</h2>";
  h += "<label>SSID</label><input name='ssid' id='ssid' value='" + htmlEscape(cfg.ssid) + "'>";
  h += "<label>Passwort <span class='muted'>(leer lassen = unveraendert)</span></label>";
  h += "<input name='pass' type='password' placeholder='********'>";
  h += "<div id='nets' class='muted' style='margin-top:.6rem'>Suche Netze...</div></div>";

  h += "<div class='card'><h2>Anzeige</h2>";
  h += "<label>Geraetename / Hostname</label><input name='host' value='" + htmlEscape(cfg.host) + "'>";
  h += "<label>Start-Screen</label><select name='startscr'>";
  h += optionRow(0, cfg.startScr, "Oeffentliche IP");
  h += optionRow(1, cfg.startScr, "Lokale IP");
  h += optionRow(2, cfg.startScr, "WLAN-Signal");
  h += optionRow(3, cfg.startScr, "Animation");
  h += "</select>";
  h += "<label>Oeffentliche IP aktualisieren</label><select name='pubint'>";
  h += optionRow(0,  cfg.pubIntMin, "nur manuell");
  h += optionRow(5,  cfg.pubIntMin, "alle 5 min");
  h += optionRow(15, cfg.pubIntMin, "alle 15 min");
  h += optionRow(60, cfg.pubIntMin, "alle 60 min");
  h += "</select></div>";

  h += "<div class='card'><h2>Sicherheit</h2>";
  h += "<label>Portal-Passwort <span class='muted'>(leer lassen = unveraendert)</span></label>";
  h += "<input name='portalpw' type='password' placeholder='********'>";
  h += "<label><input type='checkbox' name='clearpw' value='1' style='width:auto'> Passwortschutz entfernen</label>";
  h += "<div class='muted'>Benutzername ist immer <b>admin</b>.</div></div>";

  h += "<div class='card'><h2>Updates</h2>";
  h += "<label><input type='checkbox' name='autoupd' value='1' style='width:auto'";
  if (cfg.autoUpdate) h += " checked";
  h += "> Automatische Firmware-Updates (taeglich pruefen)</label></div>";

  h += "<button type='submit'>Speichern &amp; neu starten</button></form>";

  h += "<div class='card'><h2>Firmware</h2>";
  h += "<span class='muted'>Installiert:</span> " + String(FIRMWARE_VERSION) + "<br>";
  h += "<span class='muted'>Neueste (GitHub):</span> " +
       (latestVersion == "" ? String("noch nicht geprueft") : htmlEscape(latestVersion)) + "<br>";
  if (updateStatus != "") h += "<span class='muted'>Status:</span> " + htmlEscape(updateStatus) + "<br>";
  h += "<form method='POST' action='/checkupdate' style='margin-top:.6rem'>";
  h += "<button type='submit'>&#128259; Jetzt auf Updates pruefen &amp; installieren</button></form>";
  h += "<a href='/update' style='color:#8fd;display:inline-block;margin-top:.8rem'>&#128260; Alternativ: .bin manuell hochladen</a></div>";

  // WLAN-Scan per JS nachladen (fuellt die Netz-Liste)
  h += "<script>fetch('/scan').then(r=>r.json()).then(a=>{"
       "var d=document.getElementById('nets');d.innerHTML='';"
       "a.sort((x,y)=>y.rssi-x.rssi).forEach(n=>{var c=document.createElement('span');"
       "c.className='chip';c.textContent=n.ssid+' ('+n.rssi+')';"
       "c.onclick=()=>{document.getElementById('ssid').value=n.ssid;};d.appendChild(c);});"
       "if(!a.length)d.textContent='keine Netze gefunden';}).catch(()=>{"
       "document.getElementById('nets').textContent='Scan nicht verfuegbar';});</script>";

  h += "</body></html>";
  return h;
}

void handleRoot() {
  if (!authOK()) return;
  server.send(200, "text/html; charset=utf-8", buildPage());
}

void handleScan() {
  if (!authOK()) return;
  int n = WiFi.scanNetworks();
  String j = "[";
  for (int i = 0; i < n; i++) {
    if (i) j += ",";
    String s = WiFi.SSID(i); s.replace("\\", "\\\\"); s.replace("\"", "\\\"");
    j += "{\"ssid\":\"" + s + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  j += "]";
  WiFi.scanDelete();
  server.send(200, "application/json", j);
}

void handleSave() {
  if (!authOK()) return;
  bool wifiChanged = false;

  if (server.hasArg("ssid")) {
    String s = server.arg("ssid");
    if (s != cfg.ssid) { cfg.ssid = s; wifiChanged = true; }
  }
  if (server.hasArg("pass") && server.arg("pass").length() > 0) {
    cfg.pass = server.arg("pass"); wifiChanged = true;
  }
  if (server.hasArg("host") && server.arg("host").length() > 0)
    cfg.host = server.arg("host");
  if (server.hasArg("startscr"))
    cfg.startScr = constrain(server.arg("startscr").toInt(), 0, 3);
  if (server.hasArg("pubint"))
    cfg.pubIntMin = server.arg("pubint").toInt();
  if (server.hasArg("clearpw"))
    cfg.portalPw = "";
  else if (server.hasArg("portalpw") && server.arg("portalpw").length() > 0)
    cfg.portalPw = server.arg("portalpw");
  cfg.autoUpdate = server.hasArg("autoupd");   // Checkbox: nur gesetzt wenn aktiv

  saveConfig();

  String msg = "Gespeichert. Das Geraet startet jetzt neu";
  if (wifiChanged) msg += " und verbindet sich mit dem neuen WLAN";
  msg += ".";
  String page = "<!DOCTYPE html><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<body style='font-family:system-ui;background:#111;color:#eee;text-align:center;padding:40px'>"
                "<h2>&#10003; " + msg + "</h2>"
                "<p class='muted'>Nach dem Neustart ggf. neue IP auf dem Display beachten.</p></body>";
  server.send(200, "text/html; charset=utf-8", page);
  delay(1500);
  ESP.restart();
}

void handleNotFound() {
  // Captive-Portal: im AP-Modus alles auf die Startseite umleiten
  if (mode == MODE_AP) {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "not found");
  }
}

// ---- OTA: Web-Upload (.bin ueber Browser) ----
bool otaAuthed = false;

String otaPage() {
  String h;
  h += "<!DOCTYPE html><html lang='de'><head><meta charset='utf-8'>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'><title>OTA-Update</title>";
  h += "<style>body{font-family:system-ui,sans-serif;max-width:520px;margin:0 auto;padding:16px;background:#111;color:#eee}";
  h += "input,button{width:100%;padding:.6rem;border-radius:8px;box-sizing:border-box;margin-top:.6rem}";
  h += "input{background:#1c1c1c;color:#eee;border:1px solid #444}button{border:0;background:#2b8;color:#012;font-weight:700}";
  h += ".card{background:#1a1a1a;border:1px solid #333;border-radius:12px;padding:14px}a{color:#8fd}#bar{height:10px;background:#333;border-radius:6px;overflow:hidden;margin-top:1rem;display:none}#fill{height:100%;width:0;background:#2b8}</style></head><body>";
  h += "<h1>&#128260; OTA-Update</h1><div class='card'>";
  h += "Firmware (<code>.bin</code>) hochladen &ndash; das Geraet flasht sich selbst und startet neu.";
  h += "<form id='f' method='POST' action='/update' enctype='multipart/form-data'>";
  h += "<input type='file' name='firmware' accept='.bin' required>";
  h += "<button type='submit'>Hochladen &amp; flashen</button></form>";
  h += "<div id='bar'><div id='fill'></div></div><div id='s' style='margin-top:.6rem;color:#8fd'></div>";
  h += "<p style='margin-top:1rem'><a href='/'>&larr; zurueck</a></p></div>";
  // Upload mit Fortschrittsanzeige via XHR
  h += "<script>var f=document.getElementById('f');f.onsubmit=function(e){e.preventDefault();"
       "var x=new XMLHttpRequest();x.open('POST','/update');"
       "document.getElementById('bar').style.display='block';"
       "x.upload.onprogress=function(ev){if(ev.lengthComputable){var p=Math.round(ev.loaded/ev.total*100);"
       "document.getElementById('fill').style.width=p+'%';document.getElementById('s').textContent='Upload '+p+'%';}};"
       "x.onload=function(){document.getElementById('s').textContent=x.responseText;};"
       "x.onerror=function(){document.getElementById('s').textContent='Fehler beim Upload';};"
       "x.send(new FormData(f));};</script>";
  h += "</body></html>";
  return h;
}

void handleUpdatePage() {
  if (!authOK()) return;
  server.send(200, "text/html; charset=utf-8", otaPage());
}

void handleUpdateDone() {
  bool ok = otaAuthed && !Update.hasError();
  String msg = ok ? "Update OK - Geraet startet neu."
                   : (otaAuthed ? "Fehler beim Update." : "Nicht autorisiert.");
  server.send(ok ? 200 : 401, "text/plain; charset=utf-8", msg);
  if (ok) { delay(1200); ESP.restart(); }
}

void handleUpdateUpload() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaAuthed = (mode == MODE_AP) || cfg.portalPw == "" ||
                server.authenticate("admin", cfg.portalPw.c_str());
    if (!otaAuthed) { Serial.println("OTA-Web: nicht autorisiert"); return; }
    Serial.printf("OTA-Web: %s\n", up.filename.c_str());
    oledMsg("OTA-Update", "Web-Upload...", "");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE && otaAuthed) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
    char b[24]; snprintf(b, sizeof(b), "%u KB", (unsigned)(Update.progress() / 1024));
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_tr); u8g2.drawStr(0, 12, "OTA Web-Upload");
    u8g2.setFont(u8g2_font_9x15B_tr); u8g2.drawStr(0, 36, b);
    u8g2.sendBuffer();
  } else if (up.status == UPLOAD_FILE_END && otaAuthed) {
    if (Update.end(true)) Serial.printf("OTA-Web ok: %u Bytes\n", up.totalSize);
    else                  Update.printError(Serial);
  }
}

// ---- OTA: ArduinoOTA (Upload aus PlatformIO ueber WLAN) ----
void setupArduinoOTA() {
  ArduinoOTA.setHostname(cfg.host.c_str());
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    oledMsg("OTA-Update", "PlatformIO", "startet...");
    Serial.println("ArduinoOTA: Start");
  });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    int pct = t ? (int)(p * 100 / t) : 0;
    char b[16]; snprintf(b, sizeof(b), "%d %%", pct);
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x12_tr); u8g2.drawStr(0, 12, "OTA-Update");
    u8g2.setFont(u8g2_font_9x15B_tr); u8g2.drawStr(0, 34, b);
    u8g2.drawFrame(0, 48, 128, 10);
    u8g2.drawBox(0, 48, pct * 128 / 100, 10);
    u8g2.sendBuffer();
  });
  ArduinoOTA.onEnd([]() {
    oledMsg("OTA fertig", "Neustart...", "");
    Serial.println("ArduinoOTA: Ende");
  });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("ArduinoOTA-Fehler: %u\n", e);
    oledMsg("OTA-FEHLER", "siehe Serial", "");
  });
  ArduinoOTA.begin();   // startet auch mDNS mit dem Hostnamen
}

void handleCheckUpdate() {
  if (!authOK()) return;
  String page = "<!DOCTYPE html><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<meta http-equiv='refresh' content='60;url=/'>"
                "<body style='font-family:system-ui;background:#111;color:#eee;text-align:center;padding:40px'>"
                "<h2>&#128259; Suche nach Updates &hellip;</h2>"
                "<p>Falls ein Update vorliegt, laedt das Geraet die neue Firmware "
                "und startet neu (~1 Min). Diese Seite aktualisiert sich automatisch.</p></body>";
  server.send(200, "text/html; charset=utf-8", page);
  delay(200);
  checkForUpdate(true);   // rebootet bei erfolgreichem Update
}

void startPortal() {
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/scan", handleScan);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.on("/checkupdate", HTTP_POST, handleCheckUpdate);
  server.onNotFound(handleNotFound);
  server.begin();
}

void startAPMode() {
  mode = MODE_AP;
  Serial.println("Starte AP-Setupmodus ...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32-Setup");            // offenes Netz
  IPAddress ip = WiFi.softAPIP();
  dns.start(53, "*", ip);                // Captive Portal: alle DNS -> uns
  startPortal();
  oledMsg("SETUP-Modus. WLAN:", "ESP32-Setup", ip.toString().c_str());
  Serial.printf("AP: ESP32-Setup  ->  http://%s/\n", ip.toString().c_str());
}

void startSTAServices() {
  mode = MODE_STA;
  setupArduinoOTA();                       // startet mDNS mit dem Hostnamen
  MDNS.addService("http", "tcp", 80);      // Portal per <host>.local erreichbar
  Serial.printf("mDNS: http://%s.local/\n", cfg.host.c_str());
  startPortal();
  fetchPublicIP();
  Serial.printf("Portal: http://%s/   OTA: aktiv\n", WiFi.localIP().toString().c_str());
}

// ============================================================
//  Screens
// ============================================================
void drawPageDots(int active) {
  int gap = 8, x0 = 64 - (SCR_COUNT - 1) * gap / 2;
  for (int i = 0; i < SCR_COUNT; i++) {
    int x = x0 + i * gap;
    if (i == active) u8g2.drawDisc(x, 61, 2);
    else             u8g2.drawCircle(x, 61, 2);
  }
}
void drawIPScreen(const char *title, const String &ip) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(0, 12, title);
  u8g2.setFont(u8g2_font_7x14B_tr);
  u8g2.drawStr(0, 38, ip.c_str());
  drawPageDots(screen);
  u8g2.sendBuffer();
}
void renderRSSI() {
  int rssi = WiFi.RSSI();
  int bars = (rssi >= -55) ? 5 : (rssi >= -65) ? 4 : (rssi >= -72) ? 3
             : (rssi >= -80) ? 2 : (rssi >= -90) ? 1 : 0;
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(0, 12, "WLAN-Signal:");
  char buf[16]; snprintf(buf, sizeof(buf), "%d dBm", rssi);
  u8g2.setFont(u8g2_font_9x15B_tr);
  u8g2.drawStr(0, 34, buf);
  int bx = 6, bw = 8, gap = 4, baseY = 55;
  for (int i = 0; i < 5; i++) {
    int hh = 4 + i * 4, x = bx + i * (bw + gap);
    if (i < bars) u8g2.drawBox(x, baseY - hh, bw, hh);
    else          u8g2.drawFrame(x, baseY - hh, bw, hh);
  }
  drawPageDots(screen);
  u8g2.sendBuffer();
}
void renderAnim() {
  static float phase = 0; static int bx = 0;
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);
  u8g2.drawStr(0, 12, "ESP32 :)");
  const float k = 0.16f;
  for (int x = 0; x < 128; x++) {
    int y = 36 + (int)(16.0f * sinf(x * k + phase));
    u8g2.drawPixel(x, y); u8g2.drawPixel(x, y + 1);
  }
  bx = (bx + 3) % 128;
  int by = 36 + (int)(16.0f * sinf(bx * k + phase));
  u8g2.drawDisc(bx, by, 3);
  phase += 0.30f;
  drawPageDots(screen);
  u8g2.sendBuffer();
}
void renderScreen() {
  switch (screen) {
    case SCR_PUBLIC_IP: drawIPScreen("Oeffentliche IPv4:", publicIP == "" ? String("---") : publicIP); break;
    case SCR_LOCAL_IP:  drawIPScreen("Lokale IPv4:", WiFi.localIP().toString()); break;
    case SCR_RSSI:      renderRSSI(); break;
    case SCR_ANIM:      renderAnim(); break;
  }
}

// ============================================================
//  Button
// ============================================================
void handleButton() {
  static bool stable = HIGH, lastRead = HIGH;
  static unsigned long tChange = 0;
  bool r = digitalRead(BTN);
  if (r != lastRead) { tChange = millis(); lastRead = r; }
  if (millis() - tChange > 30 && r != stable) {
    stable = r;
    if (stable == LOW) {
      screen = (screen + 1) % SCR_COUNT;
      if (screen == SCR_PUBLIC_IP && publicIP == "") fetchPublicIP();
    }
  }
}

// ---- Dreh-Encoder: interrupt-basierter Gray-Code-Decoder ----
volatile int32_t encDelta = 0;
volatile uint8_t encPrevAB = 0;
// Zustandstabelle (im DRAM, damit der Zugriff aus dem ISR sicher ist)
DRAM_ATTR static const int8_t ENC_TABLE[16] =
    {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

void IRAM_ATTR encoderISR() {
  uint8_t ab = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
  encDelta += ENC_TABLE[(encPrevAB << 2) | ab];
  encPrevAB = ab;
}

void handleEncoder() {
  // Drehung auswerten: 4 Schritte (= eine Rastung) -> ein Screen weiter/zurueck
  static int32_t acc = 0;
  noInterrupts();
  acc += encDelta; encDelta = 0;
  interrupts();
  while (acc >= 4) {
    acc -= 4;
    screen = (screen + 1) % SCR_COUNT;
    if (screen == SCR_PUBLIC_IP && publicIP == "") fetchPublicIP();
  }
  while (acc <= -4) {
    acc += 4;
    screen = (screen - 1 + SCR_COUNT) % SCR_COUNT;
    if (screen == SCR_PUBLIC_IP && publicIP == "") fetchPublicIP();
  }

  // Taster (SW): entprellt -> oeffentliche IP aktualisieren + hinspringen
  static bool stable = HIGH, lastRead = HIGH;
  static unsigned long tChange = 0;
  bool r = digitalRead(ENC_SW);
  if (r != lastRead) { tChange = millis(); lastRead = r; }
  if (millis() - tChange > 30 && r != stable) {
    stable = r;
    if (stable == LOW) {                     // gedrueckt
      oledMsg("Aktualisiere", "oeff. IP...", "");
      fetchPublicIP();
      screen = SCR_PUBLIC_IP;
    }
  }
}

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(BTN, INPUT_PULLUP);

  // Dreh-Encoder
  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  encPrevAB = (digitalRead(ENC_CLK) << 1) | digitalRead(ENC_DT);
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_DT), encoderISR, CHANGE);

  Wire.begin(I2C_SDA, I2C_SCL);
  u8g2.setI2CAddress(0x3C * 2);
  u8g2.begin();

  // Werksreset: BOOT beim Start gedrueckt halten
  if (digitalRead(BTN) == LOW) {
    oledMsg("BOOT gedrueckt...", "halten fuer", "Werksreset");
    uint32_t t0 = millis();
    while (digitalRead(BTN) == LOW && millis() - t0 < 2000) delay(50);
    if (millis() - t0 >= 2000) {
      factoryReset();
      oledMsg("Werksreset OK.", "Neustart...", "");
      Serial.println("Werksreset ausgefuehrt.");
      delay(1200);
      ESP.restart();
    }
  }

  loadConfig();
  screen = constrain(cfg.startScr, 0, SCR_COUNT - 1);

  if (connectSTA(15000)) {
    Serial.printf("Verbunden! IP: %s\n", WiFi.localIP().toString().c_str());
    startSTAServices();
  } else {
    startAPMode();
  }
}

void loop() {
  server.handleClient();
  if (mode == MODE_AP) {
    dns.processNextRequest();
    delay(5);
    return;                       // im Setupmodus feste AP-Info auf dem OLED
  }

  ArduinoOTA.handle();
  handleButton();
  handleEncoder();

  // WLAN verloren? -> reconnecten, sonst AP-Setup
  if (WiFi.status() != WL_CONNECTED) {
    if (!connectSTA(15000)) { startAPMode(); return; }
    startSTAServices();
  }

  // Oeffentliche IP periodisch auffrischen
  if (cfg.pubIntMin > 0 &&
      millis() - lastPublicFetch > (uint32_t)cfg.pubIntMin * 60000UL) {
    fetchPublicIP();
  }

  // Automatische Firmware-Updates: beim Start (lastUpdateCheck==0) + taeglich.
  // Dev-Builds (FIRMWARE_VERSION=="dev") aktualisieren sich nie automatisch.
  if (cfg.autoUpdate && String(FIRMWARE_VERSION) != "dev" &&
      (lastUpdateCheck == 0 || millis() - lastUpdateCheck > UPDATE_CHECK_INTERVAL)) {
    checkForUpdate(false);   // rebootet bei erfolgreichem Update
  }

  renderScreen();
  delay(30);
}
