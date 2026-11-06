# Termofer
https://xfranerx53.github.io/Termofer/

Akıllı termostat: oda içi **verici** (ESP32), kombi yanı **alıcı** (ESP32 + röle) ve telefon uygulaması (PWA, Firebase).

## Donanım
- **Verici:** ESP32, DHT22 (GPIO4), OLED SSD1306 (SDA21/SCL22), tuşlar +/−/aç-kapa (GPIO32/33/25, diğer uç GND), PIR HC-SR501 OUT (GPIO27).
- **Alıcı:** ESP32, röle (GPIO26, LOW'da çalışır).

## Alıcı kurulumu
1. `alici/secrets.h.example` dosyasını aynı klasörde `secrets.h` olarak kopyala.
2. İçindeki `DB_SECRET` değerini doldur. `secrets.h` GitHub'a yüklenmez (`.gitignore`).
3. `alici.ino` yükle. Wi-Fi yoksa "Termostat-Kurulum" ağı açılır; telefondan ağı seç.

## Boş Ev Modu
Verici PIR ile hareketi alıcıya iletir. Ayarlar → Boş Ev Modu'nda süre ve hedef sıcaklık seçilir. Süre boyunca hareket yoksa hedef düşer; hareket veya elle değişiklikle eski değere döner.

## Notlar
- `TxPacket` yapısı `verici.ino` ve `alici.ino`'da birebir aynı olmalı.
- Firebase anahtarlarını koda/depoya yazma.
