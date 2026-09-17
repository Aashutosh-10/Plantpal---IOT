# 🌱 PlantPal — IoT Smart Plant Monitor

PlantPal is an automated, cloud-integrated plant monitoring system powered by an **ESP32**, delivering real-time telemetry to a remote **Flask** dashboard hosted on **Render**.

---

## ⚡ Features
- **Real-Time Soil Analytics:** Capacitive analog tracking with dry/wet mapped percentage.
- **Microclimate Tracking:** Ambient light levels (BH1750) and temperature/humidity (DHT11).
- **Audio & Haptic Feedback:** DFPlayer Mini voice playback and buzzer alarms triggered via capacitive touch (TTP223).
- **OLED HUD:** Live status display on a 0.96" SSD1306 screen.
- **Cloud Architecture:** ESP32 dispatches HTTP POST payloads every 5s to a REST API.
- **Responsive Dashboard:** Auto-refreshing web client polling backend SQLite records every 2.5s.

---

## 🛠️ Hardware Stack
- **Microcontroller:** ESP32 Development Board (30-pin)
- **Soil Sensor:** Analog Capacitive Soil Moisture Sensor (Pin 34)
- **Temp & Humidity:** DHT11 (Pin 4)
- **Light Sensor:** BH1750 (I2C: SDA 21, SCL 22)
- **Display:** 0.96" I2C OLED SSD1306 (0x3C)
- **Touch Sensor:** TTP223 (Pin 27)
- **Audio Output:** DFPlayer Mini (UART2: RX 16, TX 17) + Active Buzzer (Pin 25)

---

## 🚀 Cloud & Backend Setup
1. Clone the repository:
   ```bash
   git clone [https://github.com/](https://github.com/)<YOUR-USERNAME>/plantpal-iot.git
   cd plantpal-iot