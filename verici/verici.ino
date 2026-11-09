// ===== AKILLI TERMOSTAT - VERICI (oda ici) =====
// DHT22 + OLED + 3 tus (+ / - / ac-kapa) + PIR hareket sensoru (HC-SR501), ESP-NOW ile aliciya baglanir.
// Arduino-ESP32 core 3.x, kutuphaneler: DHT sensor library, Adafruit SSD1306, Adafruit GFX

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>

// !!! ALICI ESP32'nin MAC adresini yaz (alici acilista Serial'e yazdirir)
uint8_t aliciMac[] = {0x88, 0xF1, 0x55, 0x32, 0x03, 0x70};  // 88:f1:55:32:03:70

#define DHT_PIN   4
#define BTN_UP    32   // tus diger ucu GND'ye
#define BTN_DOWN  33
#define BTN_POWER 25
#define PIR_ENABLED 1  // 0: PIR yok / kapali
#define PIR_PIN   27   // HC-SR501 OUT pini (VCC=5V veya 3.3V, GND=GND). Hareket varken HIGH
#define PIR_WARMUP_MS 60000  // sensor acilista ~1 dk kararsiz: bu sure hareket sayilmaz
#define TEMP_OFFSET 0.0f  // sensor kalibrasyonu: gercek termometreyle fark varsa buraya yaz (ornek -0.7)
#define TEMP_MIN  10.0f
#define TEMP_MAX  30.0f
#define TEMP_STEP 0.5f
#define BTN_POLARITY 2   // 0: OTOMATIK (acilista tuslara DOKUNMA; bosta okunan seviye "basilmamis" kabul edilir)  1: basinca HIGH  2: basinca LOW
#define HOLD_REPEAT 0   // 1 yaparsan artir/azalt basili tutunca tekrar eder
#define DEBUG_PINS  0   // Serial'e saniyede bir tus pin durumlarini yazar (ham deger; basili seviye BTN_ACTIVE_HIGH'a gore degisir)

DHT dht(DHT_PIN, DHT22);
Adafruit_SSD1306 oled(128, 64, &Wire, -1);

// DIKKAT: TxPacket alicidaki (alici.ino) ile BIREBIR ayni olmali
typedef struct { float temp; float hum; float target; uint8_t power; uint8_t changed; uint8_t motion; } TxPacket;
typedef struct { float target; uint8_t power; uint8_t heating; } RxPacket;

TxPacket tx = {NAN, NAN, 21.0, 1, 0, 0};
bool motionPending = false, pirWas = false;
RxPacket rx;
volatile bool rxFlag = false;
volatile unsigned long lastReply = 0;

float temp = NAN, hum = NAN;
bool heating = false;
uint8_t channel = 1;
unsigned long lastSend = 0, lastRead = 0, lastPress = 0, lastScan = 0, lastDraw = 0;
unsigned long btnLast[3] = {0, 0, 0};
const uint8_t btnPins[3] = {BTN_UP, BTN_DOWN, BTN_POWER};

bool isLinked() { return millis() - lastReply < 20000 && lastReply != 0; }

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
#else
void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
#endif
  if (len == sizeof(RxPacket)) {
    memcpy(&rx, data, len);
    lastReply = millis();
    rxFlag = true;
  }
}

void sendNow() {
  tx.temp = temp;
  tx.hum = hum;
  tx.motion = motionPending ? 1 : 0;
  esp_now_send(aliciMac, (uint8_t *)&tx, sizeof(tx));
  motionPending = false;   // PIR hala HIGH ise loop() tekrar isaretler
  lastSend = millis();
}

// PIR: hareket varken isaretle, ilk algilamada paketi hemen gonder (en fazla saniyede 1)
void handlePir() {
#if PIR_ENABLED
  if (millis() < PIR_WARMUP_MS) return;
  if (digitalRead(PIR_PIN) == HIGH) {
    motionPending = true;
    if (!pirWas) {
      pirWas = true;
      Serial.println("PIR: hareket");
      if (millis() - lastSend > 1000) sendNow();
    }
  } else {
    pirWas = false;
  }
#endif
}

void setChannel(uint8_t ch) {
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

bool bActiveHigh[3];
inline bool isPressed(int i) { return digitalRead(btnPins[i]) == (bActiveHigh[i] ? HIGH : LOW); }

// --- Tus yonetimi: 30 ms debounce, her basista tek islem, +/- icin 0.5 sn sonra tekrar ---
bool bRaw[3], bState[3], bBlocked[3];
unsigned long bT[3], bPressT[3], bRepT[3];

void applyButton(int i) {
  if (i == 0) tx.target = min(TEMP_MAX, tx.target + TEMP_STEP);
  if (i == 1) tx.target = max(TEMP_MIN, tx.target - TEMP_STEP);
  if (i == 2) tx.power = !tx.power;
  tx.changed = 1;
  lastPress = millis();
  sendNow();
}

void initButtons() {
  for (int i = 0; i < 3; i++)
    pinMode(btnPins[i], BTN_POLARITY == 1 ? INPUT_PULLDOWN : BTN_POLARITY == 2 ? INPUT_PULLUP : INPUT);
  delay(150);
  for (int i = 0; i < 3; i++) {
    int high = 0;
    for (int k = 0; k < 20; k++) { high += digitalRead(btnPins[i]); delay(2); }
    if (BTN_POLARITY == 1) bActiveHigh[i] = true;
    else if (BTN_POLARITY == 2) bActiveHigh[i] = false;
    else bActiveHigh[i] = (high < 10);          // bosta LOW ise basinca HIGH gelir
    bRaw[i] = bState[i] = false;
    bBlocked[i] = (high > 2 && high < 18);     // kararsiz okuma: kablo bagli degil / yanlis
    Serial.printf("GPIO%d: bosta=%s, basinca=%s%s\n", btnPins[i], bActiveHigh[i] ? "LOW" : "HIGH",
                  bActiveHigh[i] ? "HIGH" : "LOW", bBlocked[i] ? "  UYARI: okuma kararsiz, kabloyu kontrol et!" : "");
  }
}

void handleButtons() {
  for (int i = 0; i < 3; i++) {
    bool raw = isPressed(i);
    if (raw != bRaw[i]) { bRaw[i] = raw; bT[i] = millis(); }
    if (millis() - bT[i] >= 30 && raw != bState[i]) {
      bState[i] = raw;
      Serial.printf("Tus GPIO%d: %s\n", btnPins[i], raw ? "BASILDI" : "BIRAKILDI");
      if (!raw) bBlocked[i] = false;
      else if (!bBlocked[i]) { bPressT[i] = bRepT[i] = millis(); applyButton(i); }
    }
    if (HOLD_REPEAT && i < 2 && bState[i] && !bBlocked[i] && millis() - bPressT[i] > 500 && millis() - bRepT[i] > 150) {
      bRepT[i] = millis();
      applyButton(i);
    }
  }
}

void draw() {
  oled.clearDisplay();
  oled.setTextColor(WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print(isLinked() ? "BAGLI" : "BAGLANTI YOK");
#if PIR_ENABLED
  if (pirWas) { oled.setCursor(66, 0); oled.print("*"); }   // hareket algilaniyor
#endif
  if (heating) { oled.setCursor(86, 0); oled.print("ISITMA"); }
  oled.setTextSize(3);
  oled.setCursor(0, 14);
  if (isnan(temp)) oled.print("--"); else oled.print(temp, 1);
  oled.setTextSize(1);
  oled.setCursor(0, 46);
  oled.printf("Hedef: %.1f C %s", tx.target, tx.power ? "" : "(KAPALI)");
  oled.setCursor(0, 56);
  oled.printf("Nem: %%%.0f", isnan(hum) ? 0.0 : hum);
  oled.display();
}

void setup() {
  Serial.begin(115200);
  initButtons();
#if PIR_ENABLED
  pinMode(PIR_PIN, INPUT_PULLDOWN);   // bagli degilse kayan pin sahte hareket uretmesin
#endif
  dht.begin();
  Wire.begin(21, 22);
  oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  setChannel(channel);

  if (esp_now_init() != ESP_OK) { Serial.println("ESP-NOW hata"); ESP.restart(); }
  esp_now_register_recv_cb(onRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, aliciMac, 6);
  peer.channel = 0;      // mevcut kanal
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

void loop() {
  handleButtons();
  handlePir();

#if DEBUG_PINS
  static unsigned long lastDbg = 0;
  if (millis() - lastDbg > 1000) {
    lastDbg = millis();
    Serial.printf("Pinler -> UP(32)=%d DOWN(33)=%d PWR(25)=%d | hedef=%.1f\n",
                  digitalRead(BTN_UP), digitalRead(BTN_DOWN), digitalRead(BTN_POWER), tx.target);
  }
#endif

  // Sicaklik oku
  if (millis() - lastRead > 2000) {
    lastRead = millis();
    float t = dht.readTemperature(), h = dht.readHumidity();
    if (!isnan(t)) { temp = t + TEMP_OFFSET; hum = h; }
  }

  // Aliciden cevap geldi
  if (rxFlag) {
    rxFlag = false;
    RxPacket r = rx;
    heating = r.heating;
    if (tx.changed) {
      // Tusla yaptigim degisiklik beklemede: alici AYNI degeri geri dondurene kadar onun degerini KABUL ETME
      if (fabsf(r.target - tx.target) < 0.01f && r.power == tx.power) tx.changed = 0;
    } else if (millis() - lastPress > 1500) {
      tx.target = r.target;   // uygulamadan gelen degisiklik
      tx.power = r.power;
    }
  }

  // Baglanti yoksa kanal tara (alici router kanalinda calisir), varsa 2 sn'de bir gonder
  if (!isLinked()) {
    if (millis() - lastScan > 1500) { lastScan = millis(); channel = channel % 13 + 1; setChannel(channel); }
    if (millis() - lastSend > 300) sendNow();
  } else if (millis() - lastSend > (tx.changed ? 250 : 2000)) {   // bekleyen degisiklik onaylanana kadar 250 ms'de bir tekrar gonder
    sendNow();
  }

  if (millis() - lastDraw > 200) { lastDraw = millis(); draw(); }
}
