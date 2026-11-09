// 433 MHz kumanda testi: ESP32 + RF alici modulu. Serial Monitor 115200.
// Baglanti: modul VCC -> 3V3, GND -> GND, DATA (orta 2 pinden biri) -> GPIO13
// Kutuphane: "rc-switch" (sui77) - Kutuphane Yoneticisi'nden kur.
#include <RCSwitch.h>
RCSwitch rf;
void setup() {
  Serial.begin(115200);
  rf.enableReceive(digitalPinToInterrupt(13));
  Serial.println("Kumandanin tuslarina bas...");
}
void loop() {
  if (rf.available()) {
    Serial.printf("Kod: %lu  | %u bit | protokol %u | pulse %u us\n", rf.getReceivedValue(),
                  rf.getReceivedBitlength(), rf.getReceivedProtocol(), rf.getReceivedDelay());
    rf.resetAvailable();
  }
}
