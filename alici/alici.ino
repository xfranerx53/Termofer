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
#define FW_VERSION       "1.4"
#define FROST_TEMP       5.0f   // DON KORUMASI: oda bu degerin altina inerse kapali olsa bile kombi calisir
#define FROST_HYST       1.0f
#define WINDOW_DROP      1.0f   // PENCERE ACIK: 5 dk'da bu kadar dusus (uygulamadan acilirsa)
#define WINDOW_PAUSE_MS  1200000UL  // pencere algilaninca isitma 20 dk durur
#define TZ_OFFSET_S      (3 * 3600)  // Turkiye UTC+3 (yaz saati yok)

// DIKKAT: TxPacket vericideki (verici.ino) ile BIREBIR ayni olmali
typedef struct { float temp; float hum; float target; uint8_t power; uint8_t changed; uint8_t motion; } TxPacket;
typedef struct { float target; uint8_t power; uint8_t heating; int8_t calib; } RxPacket;   // calib: sensor ofseti, 0.1 C birimi

TxPacket inBuf;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool rxFlag = false;
uint8_t txMac[6];
bool peerAdded = false;
unsigned long lastRxMs = 0;
int8_t calibTenth = -128;   // -128 = henuz Firebase'den okunmadi (verici bu degeri yok sayar). DHT22 kalibrasyon ofseti (uygulamadan), vericiye aktarilir

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

// Kombi sagligi + ogrenen on isitma + gecici artirma (boost) + tatil modu
unsigned long heatStartMs = 0; float heatStartTemp = NAN;
bool noRise = false, preheat = false;
uint32_t heatSecToday = 0, cyclesToday = 0; int healthDay = -1;
float heatRate = 1.5f;                     // oda isinma hizi C/saat (isitirken olculur, ogrenilir)
uint32_t boostUntil = 0, boostSeen = 0, holFrom = 0, holTo = 0;
float boostTemp = 24.0f, holTemp = 15.0f, boostPrev = 21.0f, holPrev = 21.0f;
bool boostActive = false, holActive = false;

// Ozel program (uygulamada cizilir): "saat:mod,saat:mod" ornek "0:1,7.5:2,18:0"  (mod 0=ev 1=uyku 2=disari)
struct CSeg { float h; uint8_t m; };
CSeg cWd[8], cWe[8]; int cWdN = 0, cWeN = 0;
int parseSegs(const char *s, CSeg *out) {
  int n = 0;
  while (s && *s && n < 8) {
    char *e; float h = strtof(s, &e);
    if (e == s || *e != ':') break;
    out[n].h = h; out[n].m = (uint8_t)constrain(atoi(e + 1), 0, 2); n++;
    const char *c = strchr(e, ','); if (!c) break;
    s = c + 1;
  }
  return n;
}

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
    if (peerAdded) {   // hemen cevap ver: loop() HTTPS isiyle mesgulken verici "baglanti yok" demesin
      RxPacket r = {target, (uint8_t)power, (uint8_t)heating, calibTenth};
      esp_now_send(mac, (uint8_t *)&r, sizeof(r));
    }
  }
}

void sendReply() {
  if (!peerAdded) return;
  RxPacket r = {target, (uint8_t)power, (uint8_t)heating, calibTenth};
  esp_now_send(txMac, (uint8_t *)&r, sizeof(r));
}

// Tek ortak TLS baglantisi: her istekte yeniden el sikismak 1-3 sn suruyordu
WiFiClientSecure fbc; bool fbcInit = false;
void fbBegin(HTTPClient &h, const String &url) {
  if (!fbcInit) { fbc.setInsecure(); fbcInit = true; }
  h.setReuse(true); h.setConnectTimeout(3000); h.setTimeout(4000);
  h.begin(fbc, url);
}
void fbEnd(HTTPClient &h, int code) { h.end(); if (code < 0) fbc.stop(); }

String dbUrl() { return String(DB_URL) + "/thermostat.json?auth=" + DB_SECRET; }

// Firebase hatasini seri monitore yaz (en fazla 10 sn'de bir). 401/403 = secret/kurallar, negatif = baglanti/SSL
void logFbErr(const char *what, int code) {
  static unsigned long last = 0;
  if (code == 200 || millis() - last < 10000) return;
  last = millis();
  Serial.printf("Firebase %s HATA: %d (%s)\n", what, code, code < 0 ? HTTPClient::errorToString(code).c_str() : code == 401 || code == 403 ? "secret yanlis/iptal veya kurallar" : code == 404 ? "DB_URL yanlis" : "?");
}

bool pushState() {
  HTTPClient h; fbBegin(h, dbUrl());
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
  JsonObject hl = d["health"].to<JsonObject>();
  hl["noRise"] = noRise;
  hl["runMin"] = (heatSecToday + (heating ? (millis() - heatStartMs) / 1000 : 0)) / 60;
  hl["cycles"] = cyclesToday;
  hl["rate"] = heatRate;
  d["preheat"] = preheat;
  d["boostOn"] = boostActive;
  d["holOn"] = holActive;
  String body; serializeJson(d, body);
  int code = h.PATCH(body);
  fbEnd(h, code);
  lastPush = millis();
  logFbErr("yazma", code);
  return code == 200;
}

void pollState() {
  HTTPClient h; fbBegin(h, dbUrl());
  int getCode = h.GET();
  logFbErr("okuma", getCode);
  if (getCode == 200) {
    JsonDocument d;
    if (!deserializeJson(d, h.getString()) && d["targetTemp"].is<float>() && d["power"].is<bool>()) {
      awayEnabled = d["awayEnabled"] | false;
      awayMinutes = d["awayMinutes"] | 180;
      awayTemp = d["awayTemp"] | 17.0f;
      windowDetect = d["windowDetect"] | false;
      { float cal = d["calib"] | 0.0f; int8_t ct = (int8_t)constrain((int)lroundf(cal * 10.0f), -50, 50);
        if (ct != calibTenth) { calibTenth = ct; sendReply(); Serial.printf("Kalibrasyon ofseti: %.1f C\n", ct / 10.0f); } }
      boostUntil = (uint32_t)(d["boostUntil"] | 0UL); boostTemp = d["boostTemp"] | 24.0f;
      holFrom = (uint32_t)(d["holFrom"] | 0UL); holTo = (uint32_t)(d["holTo"] | 0UL); holTemp = d["holTemp"] | 15.0f;
      JsonObject sc = d["sched"];
      if (!sc.isNull()) {
        const char *m = sc["mode"] | "manual"; schedMode = m;
        const char *pr = sc["prog"] | "tasarruf"; schedProg = pr;
        schedTemps[0] = sc["temps"]["ev"] | 22.0f;
        schedTemps[1] = sc["temps"]["uyku"] | 20.0f;
        schedTemps[2] = sc["temps"]["disari"] | 18.0f;
        cWdN = parseSegs(sc["cwd"] | "", cWd);
        cWeN = parseSegs(sc["cwe"] | "", cWe);
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
        boostActive = false;                 // elle degisti: gecici artirma biter
        if (away) away = false;              // kullanici elle degistirdi: bos ev modundan cik
        sendReply();                         // vericiye ilet
        Serial.printf("Uygulamadan: hedef=%.1f power=%d\n", target, power);
      }
    }
  }
  fbEnd(h, getCode);
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
  if (prev != heating) {
    if (heating) { heatStartMs = millis(); heatStartTemp = temp; cyclesToday++; }
    else {
      unsigned long dur = millis() - heatStartMs;
      heatSecToday += dur / 1000;
      if (dur >= 900000UL && !windowOpen && !frost && !isnan(temp) && !isnan(heatStartTemp)) {   // >=15 dk surmus isitma: isinma hizini ogren
        float r = (temp - heatStartTemp) / (dur / 3600000.0f);
        if (r >= 0.2f && r <= 8.0f) { heatRate = constrain(heatRate * 0.7f + r * 0.3f, 0.3f, 6.0f); Serial.printf("Isinma hizi ogrenildi: %.2f C/saat\n", heatRate); }
      }
      noRise = false;
    }
    dirty = true; sendReply();
  }
  // Kombi sagligi: 40 dk surekli isitiyor ama sicaklik 0.3 C bile artmadiysa ariza/gaz yok uyarisi
  if (heating && !frost && !isnan(temp) && !isnan(heatStartTemp) && millis() - heatStartMs > 2400000UL && temp - heatStartTemp < 0.3f) {
    if (!noRise) { noRise = true; dirty = true; Serial.println("UYARI: isitiyor ama sicaklik artmiyor (kombi/gaz?)"); }
  }
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
  if (p == "ozel") {
    const CSeg *c = we ? cWe : cWd; int cn = we ? cWeN : cWdN;
    if (cn == 0) return 0;
    int m = c[0].m;
    for (int i = 0; i < cn; i++) if (c[i].h <= h) m = c[i].m;
    return m;
  }
  const Seg *s; int n;
  if (p == "is") { if (we) { s = IS_WE; n = 2; } else { s = IS_WD; n = 4; } }
  else if (p == "konfor") { s = KON; n = 2; }
  else { if (we) { s = TAS_WE; n = 2; } else { s = TAS_WD; n = 3; } }
  int m = s[0].m;
  for (int i = 0; i < n; i++) if (s[i].h <= h) m = s[i].m;
  return m;
}
// Gun doner (saglik sayaclari), gecici artirma (boost) ve tatil modu
void applyOverrides() {
  time_t nowT = time(nullptr);
  if (nowT < 1700000000) return;
  uint32_t now = (uint32_t)nowT;
  int day = (int)((now + TZ_OFFSET_S) / 86400);
  if (day != healthDay) { healthDay = day; heatSecToday = 0; cyclesToday = 0; }
  if (boostActive && (boostUntil == 0 || now >= boostUntil)) {            // sure doldu ya da uygulamadan iptal
    boostActive = false; target = boostPrev; dirty = true; sendReply(); Serial.println("Artirma bitti");
  } else if (!boostActive && boostUntil > now && boostUntil != boostSeen) {
    boostSeen = boostUntil; boostPrev = target; target = constrain(boostTemp, 10.0f, 30.0f);
    power = true; away = false; boostActive = true; dirty = true; sendReply(); Serial.println("Artirma basladi");
  }
  bool inHol = holFrom && holTo && now >= holFrom && now < holTo;
  if (inHol && !holActive) {
    holActive = true; holPrev = target; boostActive = false;
    target = constrain(holTemp, 10.0f, 30.0f); dirty = true; sendReply(); Serial.println("Tatil modu basladi");
  } else if (!inHol && holActive) {
    holActive = false; target = holPrev; dirty = true; sendReply(); Serial.println("Tatil modu bitti");
  }
}

void applyProgram() {
  if (boostActive || holActive) { preheat = false; return; }
  if (!schedValid || schedMode != "program" || !power || away) { preheat = false; return; }
  if (millis() - lastProg < 30000) return;
  lastProg = millis();
  time_t now = time(nullptr);
  if (now < 1700000000) return;                 // saat henuz NTP'den alinmadi
  struct tm t; localtime_r(&now, &t);
  int mi = progModeIdx(schedProg, t.tm_wday == 0 || t.tm_wday == 6, t.tm_hour + t.tm_min / 60.0f);
  float want = constrain(schedTemps[mi], 10.0f, 30.0f);
  preheat = false;
  if (!isnan(temp)) {   // ON ISITMA: yaklasan daha sicak dilimi hedefe tam zamaninda ulasmak icin erken baslat
    for (int k = 10; k <= 180; k += 10) {
      time_t fut = now + k * 60; struct tm ft; localtime_r(&fut, &ft);
      int fi = progModeIdx(schedProg, ft.tm_wday == 0 || ft.tm_wday == 6, ft.tm_hour + ft.tm_min / 60.0f);
      float fw = constrain(schedTemps[fi], 10.0f, 30.0f);
      if (fw > want) {
        if (fw > temp && (fw - temp) / heatRate * 60.0f >= k - 10) { want = fw; preheat = true; }
        break;
      }
    }
  }
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
  char kova[12]; snprintf(kova, sizeof(kova), "%08u", (unsigned)b);
  HTTPClient h; fbBegin(h, String(DB_URL) + "/termofer/history/" + kova + ".json?auth=" + DB_SECRET);
  h.addHeader("Content-Type", "application/json");
  JsonDocument d;
  d["t"] = temp; if (!isnan(hum)) d["h"] = hum;
  d["g"] = target; d["o"] = heating ? 1 : 0;
  String body; serializeJson(d, body);
  int code = h.PUT(body);
  fbEnd(h, code);
  logFbErr("gecmis", code);
  if (code == 200) lastBucket = b;
}

// Verici tusuyla programdan cikildi: uygulama kapali olsa bile modu "manual" yap
void pushModeManual() {
  HTTPClient h; fbBegin(h, String(DB_URL) + "/termofer.json?auth=" + DB_SECRET);
  h.addHeader("Content-Type", "application/json");
  int code = h.PATCH("{\"mode\":\"manual\"}");
  fbEnd(h, code);
  if (code != 200) return;
  HTTPClient h2; fbBegin(h2, String(DB_URL) + "/thermostat/sched.json?auth=" + DB_SECRET);
  h2.addHeader("Content-Type", "application/json");
  code = h2.PATCH("{\"mode\":\"manual\"}");
  fbEnd(h2, code);
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
  { static unsigned long lastDiag = 0;   // Teshis: 5 sn'de bir
    if (millis() - lastDiag > 5000) { lastDiag = millis();
      Serial.printf("[durum] wifi=%s kanal=%d rssi=%d sonVericiPaketi=%s%lu ms once\n", WiFi.status() == WL_CONNECTED ? "BAGLI" : "YOK",
                    WiFi.channel(), WiFi.RSSI(), lastRxMs ? "" : "HIC-GELMEDI ", lastRxMs ? millis() - lastRxMs : 0UL); } }

  // Vericiden paket geldi
  if (rxFlag) {
    TxPacket in;
    portENTER_CRITICAL(&mux);
    in = inBuf;
    rxFlag = false;
    portEXIT_CRITICAL(&mux);
    lastRxMs = millis();
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
      boostActive = false;
      if (away) away = false;   // elle degisti: bos ev modundan cik
      if (schedMode == "program") { schedMode = "manual"; needModePush = true; }   // tusla elle degisti: programdan cik
      dirty = true;
      Serial.printf("Vericiden: hedef=%.1f power=%d\n", target, power);
    }
    sendReply();
  }

  checkWindow();
  applyOverrides();
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
