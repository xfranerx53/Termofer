// ===== AKILLI TERMOSTAT - ALICI (kombi yanı) =====
// WiFi + Firebase Realtime Database (REST) + ESP-NOW + role
// Arduino-ESP32 core 3.x, kutuphane: ArduinoJson v7

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_now.h>
#include <time.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

// DB_URL ve DB_SECRET "secrets.h" dosyasindan gelir (GitHub'a YUKLENMEZ, .gitignore'da).
// Kurulum: secrets.h.example dosyasini secrets.h olarak kopyala, icindeki degerleri doldur.
#include "secrets.h"

#define RELAY_PIN        26
#define RELAY_ACTIVE_LOW true   // optokuplorlu role modulleri genelde LOW'da calisir
#define HYST             0.3    // histerezis (C)
#define DATA_TIMEOUT_MS  120000 // verici 2 dk susarsa kombi guvenlik icin kapanir
#define FW_VERSION       "1.2"
#define FROST_TEMP       5.0f   // DON KORUMASI: oda bu degerin altina inerse kapali olsa bile kombi calisir
#define FROST_HYST       1.0f
#define WINDOW_DROP      1.0f   // PENCERE ACIK: 5 dk'da bu kadar dusus (uygulamadan acilirsa)
#define WINDOW_PAUSE_MS  1200000UL  // pencere algilaninca isitma 20 dk durur
#define TZ_OFFSET_S      (3 * 3600)  // Turkiye UTC+3 (yaz saati yok)

// DIKKAT: TxPacket vericideki (verici.ino) ile BIREBIR ayni olmali
typedef struct { float temp; float hum; float target; uint8_t power; uint8_t changed; uint8_t motion; } TxPacket;
typedef struct { float target; uint8_t power; uint8_t heating; } RxPacket;

TxPacket inBuf;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool rxFlag = false;
uint8_t txMac[6];
bool peerAdded = false;

float temp = NAN, hum = NAN, target = 21.0;
bool power = true, heating = false, dirty = false;

// Evde kimse yok modu (ayarlar uygulamadan gelir)
bool awayEnabled = false, away = false, firstPoll = true;
int awayMinutes = 180;
float awayTemp = 17.0f, savedTarget = 21.0f;
unsigned long lastMotionMs = 0;
uint32_t lastMotionEpoch = 0;
unsigned long lastData = 0, lastCycle = 0, lastPush = 0;

// Don korumasi / pencere algilama
bool frost = false, windowDetect = false, windowOpen = false;
float tHist[6]; uint8_t tHistN = 0;
unsigned long lastSample = 0, windowSince = 0;

// Haftalik program (uygulama "thermostat/sched" olarak yansitir; uygulama kapaliyken de calisir)
String schedMode = "manual", schedProg = "tasarruf";
float schedTemps[3] = {22.0f, 20.0f, 18.0f};   // ev, uyku, disari
bool schedValid = false, needModePush = false;
uint32_t progAt = 0, lastBucket = 0;
unsigned long lastProg = 0;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  const uint8_t *mac = info->src_addr;
#else
void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  if (len == sizeof(TxPacket)) {          // en son paket her zaman kazanir
    portENTER_CRITICAL(&mux);
    memcpy(&inBuf, data, len);
    memcpy(txMac, mac, 6);
    rxFlag = true;
    portEXIT_CRITICAL(&mux);
  }
}

void sendReply() {
  if (!peerAdded) return;
  RxPacket r = {target, (uint8_t)power, (uint8_t)heating};
  esp_now_send(txMac, (uint8_t *)&r, sizeof(r));
}

String dbUrl() { return String(DB_URL) + "/thermostat.json?auth=" + DB_SECRET; }

bool pushState() {
  WiFiClientSecure c; c.setInsecure();
  HTTPClient h; h.begin(c, dbUrl());
  h.addHeader("Content-Type", "application/json");
  JsonDocument d;
  d["targetTemp"] = target;
  d["power"] = power;
  d["heating"] = heating;
  if (!isnan(temp)) { d["currentTemp"] = temp; d["humidity"] = hum; }
  d["lastSeen"] = (uint32_t)time(nullptr);
  d["away"] = away;
  d["savedTarget"] = savedTarget;
  d["lastMotion"] = lastMotionEpoch;
  d["frost"] = frost;
  d["windowOpen"] = windowOpen;
  d["progAt"] = progAt;
  d["rssi"] = WiFi.RSSI();
  d["uptime"] = (uint32_t)(millis() / 1000);
  d["fw"] = FW_VERSION;
  String body; serializeJson(d, body);
  int code = h.PATCH(body);
  h.end();
  lastPush = millis();
  return code == 200;
}

void pollState() {
  WiFiClientSecure c; c.setInsecure();
  HTTPClient h; h.begin(c, dbUrl());
  if (h.GET() == 200) {
    JsonDocument d;
    if (!deserializeJson(d, h.getString()) && d["targetTemp"].is<float>() && d["power"].is<bool>()) {
      awayEnabled = d["awayEnabled"] | false;
      awayMinutes = d["awayMinutes"] | 180;
      awayTemp = d["awayTemp"] | 17.0f;
      windowDetect = d["windowDetect"] | false;
      JsonObject sc = d["sched"];
      if (!sc.isNull()) {
        const char *m = sc["mode"] | "manual"; schedMode = m;
        const char *pr = sc["prog"] | "tasarruf"; schedProg = pr;
        schedTemps[0] = sc["temps"]["ev"] | 22.0f;
        schedTemps[1] = sc["temps"]["uyku"] | 20.0f;
        schedTemps[2] = sc["temps"]["disari"] | 18.0f;
        schedValid = true;
      }
      float t = d["targetTemp"]; bool p = d["power"];
      t = constrain(t, 10.0f, 30.0f);
      if (firstPoll) {                       // acilis: Firebase'deki durumu aynen devral
        target = t; power = p;
        away = d["away"] | false;
        savedTarget = d["savedTarget"] | t;
        firstPoll = false;
      } else if (fabs(t - target) > 0.01 || p != power) {   // uygulamadan degisiklik geldi
        target = t; power = p;
        if (away) away = false;              // kullanici elle degistirdi: bos ev modundan cik
        sendReply();                         // vericiye ilet
        Serial.printf("Uygulamadan: hedef=%.1f power=%d\n", target, power);
      }
    }
  }
  h.end();
}

void enterAway() {
  savedTarget = target;
  if (awayTemp < target) target = awayTemp;
  away = true; dirty = true; sendReply();
  Serial.println("Ev bos modu: ACIK");
}
void exitAway() {
  target = savedTarget;
  away = false; dirty = true; sendReply();
  Serial.println("Ev bos modu: KAPALI");
}
void checkAway() {
  if (!away && awayEnabled && millis() - lastMotionMs > (unsigned long)awayMinutes * 60000UL) enterAway();
  else if (away && !awayEnabled) exitAway();
}

void updateRelay() {
  bool fresh = !isnan(temp) && (millis() - lastData < DATA_TIMEOUT_MS);
  bool prev = heating, prevFrost = frost;
  if (fresh && temp < FROST_TEMP) frost = true;
  else if (!fresh || temp >= FROST_TEMP + FROST_HYST) frost = false;
  if (frost) heating = true;                                  // don korumasi her seyden once gelir
  else if (!power || !fresh || windowOpen) heating = false;
  else if (temp < target - HYST) heating = true;
  else if (temp >= target + HYST) heating = false;
  bool level = RELAY_ACTIVE_LOW ? !heating : heating;
  digitalWrite(RELAY_PIN, level ? HIGH : LOW);
  if (prevFrost != frost) dirty = true;
  if (prev != heating) { dirty = true; sendReply(); }
}

// Pencere acik algilama: dakikada bir ornek, 5 dk'da WINDOW_DROP kadar dusus -> isitma 20 dk durur
void checkWindow() {
  if (!windowDetect) { if (windowOpen) { windowOpen = false; dirty = true; } return; }
  if (windowOpen && millis() - windowSince > WINDOW_PAUSE_MS) { windowOpen = false; tHistN = 0; dirty = true; return; }
  if (millis() - lastSample < 60000 || isnan(temp)) return;
  lastSample = millis();
  for (int i = 5; i > 0; i--) tHist[i] = tHist[i - 1];
  tHist[0] = temp; if (tHistN < 6) tHistN++;
  if (!windowOpen && !frost && tHistN >= 6 && tHist[5] - tHist[0] >= WINDOW_DROP) {
    windowOpen = true; windowSince = millis(); dirty = true;
    Serial.println("Pencere acik algilandi: isitma 20 dk duruyor");
  }
}

// ---- Haftalik program (uygulamadaki PROGS ile ayni) ----  m: 0=ev 1=uyku 2=disari
struct Seg { uint8_t h; uint8_t m; };
const Seg TAS_WD[] = {{0,1},{7,2},{18,0}}, TAS_WE[] = {{0,1},{9,0}};
const Seg IS_WD[]  = {{0,1},{7,0},{9,2},{18,0}}, IS_WE[] = {{0,1},{9,0}};
const Seg KON[]    = {{0,1},{7,0}};
int progModeIdx(const String &p, bool we, float h) {
  const Seg *s; int n;
  if (p == "is") { if (we) { s = IS_WE; n = 2; } else { s = IS_WD; n = 4; } }
  else if (p == "konfor") { s = KON; n = 2; }
  else { if (we) { s = TAS_WE; n = 2; } else { s = TAS_WD; n = 3; } }
  int m = s[0].m;
  for (int i = 0; i < n; i++) if (s[i].h <= h) m = s[i].m;
  return m;
}
void applyProgram() {
  if (!schedValid || schedMode != "program" || !power || away) return;
  if (millis() - lastProg < 30000) return;
  lastProg = millis();
  time_t now = time(nullptr);
  if (now < 1700000000) return;                 // saat henuz NTP'den alinmadi
  struct tm t; localtime_r(&now, &t);
  int mi = progModeIdx(schedProg, t.tm_wday == 0 || t.tm_wday == 6, t.tm_hour + t.tm_min / 60.0f);
  float want = constrain(schedTemps[mi], 10.0f, 30.0f);
  if (fabs(want - target) > 0.01) {
    target = want; progAt = (uint32_t)now; dirty = true; sendReply();
    Serial.printf("Program: hedef=%.1f\n", target);
  }
}

// Gecmis kaydi (uygulama kapaliyken de): 10 dk'da bir termofer/history/<kova>
void logHistory() {
  time_t now = time(nullptr);
  if (now < 1700000000 || isnan(temp)) return;
  uint32_t b = (uint32_t)now / 600;
  if (b == lastBucket) return;
  char path[96]; snprintf(path, sizeof(path), "%s/termofer/history/%08u.json?auth=%s", DB_URL, (unsigned)b, DB_SECRET);
  WiFiClientSecure c; c.setInsecure();
  HTTPClient h; h.begin(c, path);
  h.addHeader("Content-Type", "application/json");
  JsonDocument d;
  d["t"] = temp; if (!isnan(hum)) d["h"] = hum;
  d["g"] = target; d["o"] = heating ? 1 : 0;
  String body; serializeJson(d, body);
  int code = h.PUT(body);
  h.end();
  if (code == 200) lastBucket = b;
}

// Verici tusuyla programdan cikildi: uygulama kapali olsa bile modu "manual" yap
void pushModeManual() {
  WiFiClientSecure c; c.setInsecure();
  HTTPClient h; h.begin(c, String(DB_URL) + "/termofer.json?auth=" + DB_SECRET);
  h.addHeader("Content-Type", "application/json");
  int code = h.PATCH("{\"mode\":\"manual\"}");
  h.end();
  if (code != 200) return;
  WiFiClientSecure c2; c2.setInsecure();
  HTTPClient h2; h2.begin(c2, String(DB_URL) + "/thermostat/sched.json?auth=" + DB_SECRET);
  h2.addHeader("Content-Type", "application/json");
  code = h2.PATCH("{\"mode\":\"manual\"}");
  h2.end();
  if (code == 200) needModePush = false;
}

// ================= WiFi KURULUM (telefonla ag secme) =================
// Kayitli ag yoksa / bulunamazsa "Termostat-Kurulum" agi acilir. Telefonla baglan, acilan sayfadan agi sec.
// ESP32 uzerindeki BOOT tusu (GPIO0) 5 sn basili tutulursa kayitli WiFi silinir.
#define RESET_PIN 0
#define LED_PIN   2
#define AP_NAME   "Termostat-Kurulum"

enum WifiState { WST_CONNECTING, WST_CONNECTED, WST_PORTAL };
WifiState wifiState = WST_CONNECTING;
Preferences prefs;
WebServer server(80);
DNSServer dns;
String savedSsid, savedPass;
unsigned long stateSince = 0, lostSince = 0, lastHttp = 0, retryAt = 0, retryStart = 0, resetHeldSince = 0;
bool retrying = false, ntpStarted = false;

String esc(String s) {
  s.replace("&", "&amp;"); s.replace("<", "&lt;"); s.replace(">", "&gt;"); s.replace("\"", "&quot;");
  return s;
}

void handleRoot() {
  lastHttp = millis();
  String h = "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\"><title>Termostat WiFi</title>"
             "<style>body{font-family:system-ui,sans-serif;max-width:420px;margin:auto;padding:16px;background:#121212;color:#eee}"
             "h2{margin-top:0}.n{padding:12px;margin:6px 0;background:#222;border-radius:8px}"
             "input,button{width:100%;box-sizing:border-box;padding:12px;margin:6px 0;border-radius:8px;border:0;font-size:16px}"
             "button{background:#ff6d00;color:#fff;font-weight:bold}a{color:#ffab40}small{color:#999}</style></head><body>"
             "<h2>Termostat WiFi Kurulumu</h2><p>Bağlanmak istediğin ağa dokun, şifreyi yaz ve kaydet.</p>";
  int n = WiFi.scanComplete();
  if (n > 0) {
    String seen;
    for (int i = 0; i < n; i++) {
      String ss = WiFi.SSID(i);
      if (ss.length() == 0 || seen.indexOf("\n" + ss + "\n") >= 0) continue;
      seen += "\n" + ss + "\n";
      h += "<div class=n data-s=\"" + esc(ss) + "\">" + esc(ss) + " <small>(" + String(WiFi.RSSI(i)) + " dBm)" +
           (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "" : " 🔒") + "</small></div>";
    }
  } else {
    h += "<p><small>Ağ bulunamadı. Aşağıdan yeniden tara veya ağ adını elle yaz.</small></p>";
  }
  h += "<form method=POST action=/save><input id=ssid name=ssid placeholder=\"Ağ adı\" required>"
       "<input id=pass name=pass type=password placeholder=\"Şifre\"><button>Kaydet ve bağlan</button></form>"
       "<a href=/scan>Ağları yeniden tara</a>"
       "<p><small>Alıcı MAC (vericiye yazılacak): " + WiFi.macAddress() + "</small></p>"
       "<script>document.querySelectorAll('.n').forEach(function(e){e.onclick=function(){"
       "document.getElementById('ssid').value=e.dataset.s;document.getElementById('pass').focus()}})</script></body></html>";
  server.send(200, "text/html; charset=utf-8", h);
}

void handleScan() {
  lastHttp = millis();
  WiFi.scanDelete();
  WiFi.scanNetworks();
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

void handleSave() {
  lastHttp = millis();
  String ssid = server.arg("ssid"), pass = server.arg("pass");
  if (ssid.length() == 0) { server.sendHeader("Location", "/"); server.send(302, "text/plain", ""); return; }
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  server.send(200, "text/html; charset=utf-8",
              "<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
              "<body style=\"font-family:sans-serif;padding:24px;background:#121212;color:#eee\"><h2>Kaydedildi ✓</h2>"
              "<p>Cihaz yeniden başlıyor ve ağa bağlanıyor. Bağlanamazsa <b>Termostat-Kurulum</b> ağı yeniden açılır.</p></body>");
  delay(1500);
  ESP.restart();
}

void startPortal() {
  wifiState = WST_PORTAL;
  lastHttp = millis(); retrying = false; retryAt = millis() + 60000;
  WiFi.disconnect();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_NAME);
  delay(200);
  dns.start(53, "*", WiFi.softAPIP());
  server.on("/", handleRoot);
  server.on("/scan", handleScan);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleRoot);           // telefonun "oturum ac" penceresini tetikler
  server.begin();
  WiFi.scanNetworks();
  Serial.printf("WiFi kurulum portali ACIK: telefonda '%s' agina baglan (192.168.4.1)\n", AP_NAME);
}

void onConnected() {
  wifiState = WST_CONNECTED; lostSince = 0;
  Serial.printf("WiFi baglandi: %s  IP: %s  kanal: %d\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.channel());
  if (!ntpStarted) { configTime(TZ_OFFSET_S, 0, "pool.ntp.org"); ntpStarted = true; }
  pollState();                              // ilk baglantida son hedefi Firebase'den al
}

void checkResetButton() {
  if (digitalRead(RESET_PIN) == LOW) {
    if (resetHeldSince == 0) resetHeldSince = millis();
    if (millis() - resetHeldSince > 5000) {
      Serial.println("WiFi ayarlari SIFIRLANDI. Tusu birak, cihaz yeniden basliyor...");
      prefs.clear();
      digitalWrite(LED_PIN, HIGH);          // LED sabit yanar = sifirlandi, tusu birak
      while (digitalRead(RESET_PIN) == LOW) delay(10);   // tus basiliyken reset olursa ESP32 yukleme moduna girer
      delay(300);
      ESP.restart();
    }
  } else {
    resetHeldSince = 0;
  }
}

void wifiTask() {
  checkResetButton();
  switch (wifiState) {
    case WST_CONNECTING:
      digitalWrite(LED_PIN, (millis() / 250) % 2);
      if (WiFi.status() == WL_CONNECTED) onConnected();
      else if (millis() - stateSince > 25000) { Serial.println("Kayitli ag bulunamadi/baglanilamadi"); startPortal(); }
      break;
    case WST_CONNECTED:
      digitalWrite(LED_PIN, LOW);
      if (WiFi.status() == WL_CONNECTED) lostSince = 0;
      else {
        if (lostSince == 0) lostSince = millis();
        if (millis() - lostSince > 120000) { Serial.println("WiFi 2 dk'dir yok"); startPortal(); }
      }
      break;
    case WST_PORTAL:
      digitalWrite(LED_PIN, (millis() / 100) % 2);
      dns.processNextRequest();
      server.handleClient();
      if (WiFi.status() == WL_CONNECTED) ESP.restart();   // kayitli ag geri geldi (ornegin router gec acildi)
      // Kayitli ag varsa ve portalda kimse yoksa dakikada bir 15 sn tekrar dene
      if (savedSsid.length() && WiFi.softAPgetStationNum() == 0 && millis() - lastHttp > 60000) {
        if (!retrying && millis() > retryAt) { WiFi.begin(savedSsid.c_str(), savedPass.c_str()); retrying = true; retryStart = millis(); }
        else if (retrying && millis() - retryStart > 15000) { WiFi.disconnect(); retrying = false; retryAt = millis() + 60000; }
      }
      break;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  pinMode(RESET_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  prefs.begin("wifi", false);
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");

  WiFi.mode(WIFI_STA);
  Serial.print("ALICI MAC (vericiye yaz): "); Serial.println(WiFi.macAddress());

  // ESP-NOW internet beklemeden hemen baslar: WiFi olmasa da verici-role haberlesmesi (kombi) calisir
  if (esp_now_init() != ESP_OK) { Serial.println("ESP-NOW hata"); ESP.restart(); }
  esp_now_register_recv_cb(onRecv);

  if (savedSsid.length()) {
    Serial.printf("Kayitli aga baglaniliyor: %s\n", savedSsid.c_str());
    WiFi.begin(savedSsid.c_str(), savedPass.c_str());
    wifiState = WST_CONNECTING;
    stateSince = millis();
  } else {
    Serial.println("Kayitli WiFi yok.");
    startPortal();
  }
  lastMotionMs = millis();   // acilista evde biri var say
}

void loop() {
  wifiTask();

  // Vericiden paket geldi
  if (rxFlag) {
    TxPacket in;
    portENTER_CRITICAL(&mux);
    in = inBuf;
    rxFlag = false;
    portEXIT_CRITICAL(&mux);
    if (!peerAdded) {
      esp_now_peer_info_t p = {};
      memcpy(p.peer_addr, txMac, 6);
      p.channel = 0; p.encrypt = false;
      peerAdded = esp_now_add_peer(&p) == ESP_OK;
    }
    if (!isnan(in.temp)) { temp = in.temp; hum = in.hum; lastData = millis(); }
    if (in.motion) {                                                  // PIR hareket gordu
      lastMotionMs = millis();
      lastMotionEpoch = (uint32_t)time(nullptr);
      if (away) exitAway();
    }
    if (in.changed && (in.target != target || in.power != power)) {   // tusla degisti
      target = constrain(in.target, 10.0f, 30.0f);
      power = in.power;
      if (away) away = false;   // elle degisti: bos ev modundan cik
      if (schedMode == "program") { schedMode = "manual"; needModePush = true; }   // tusla elle degisti: programdan cik
      dirty = true;
      Serial.printf("Vericiden: hedef=%.1f power=%d\n", target, power);
    }
    sendReply();
  }

  checkWindow();
  applyProgram();
  updateRelay();
  checkAway();

  // 2 sn'de bir Firebase: once bekleyen degisikligi yaz, yoksa uygulamadan oku
  if (millis() - lastCycle > 2000 && wifiState == WST_CONNECTED && WiFi.status() == WL_CONNECTED) {
    lastCycle = millis();
    if (millis() - lastPush > 20000) dirty = true;   // periyodik canlilik/sicaklik guncellemesi
    if (needModePush) pushModeManual();
    else if (dirty) { if (pushState()) dirty = false; }
    else { pollState(); logHistory(); }
  }
}
