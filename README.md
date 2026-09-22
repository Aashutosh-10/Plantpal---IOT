# 🌱 PlantPal — Bidirectional Cloud IoT Smart Plant Monitor

PlantPal is a three-person Embedded Systems + IoT project built around an ESP32. It combines environmental sensing, local human interaction, voice feedback, an OLED interface, and a cloud dashboard with remote commands.

## Architecture

```text
                 ┌─────────────────────┐
                 │   PlantPal Website  │
                 │  Monitor + Control  │
                 └──────────┬──────────┘
                            ↕ HTTPS/REST
                 ┌──────────┴──────────┐
                 │      Render         │
                 │ Flask + SQLite API  │
                 └──────────┬──────────┘
                            ↕ HTTPS/REST
                 ┌──────────┴──────────┐
                 │       ESP32         │
                 │ Wi-Fi + control     │
                 └──┬──┬──┬──┬──┬─────┘
                    │  │  │  │  │
                  Soil DHT BH OLED DFPlayer
                  34   4  21/22 27  UART2
```

The ESP32 uploads telemetry every 5 seconds and polls the cloud command queue every 1.5 seconds. Website controls therefore travel through the cloud and can trigger physical ESP32 actions remotely, as long as the device remains online.

## Hardware pin map

| Component | Pin | ESP32 |
|---|---|---|
| OLED | SDA | GPIO21 |
| OLED | SCL | GPIO22 |
| OLED | VCC | 3.3V |
| OLED | GND | GND |
| BH1750 | SDA | GPIO21 |
| BH1750 | SCL | GPIO22 |
| BH1750 | VCC | 3.3V |
| BH1750 | GND | GND |
| BH1750 | ADD/ADO | NC |
| Soil sensor | AOUT | GPIO34 |
| Soil sensor | VCC | 3.3V |
| Soil sensor | GND | GND |
| DHT11 | DATA | GPIO4 |
| DHT11 | VCC | 3.3V |
| DHT11 | GND | GND |
| TTP223 | SIG | GPIO27 |
| TTP223 | VCC | 3.3V |
| TTP223 | GND | GND |
| DFPlayer | RX | GPIO17 / TX2 |
| DFPlayer | TX | GPIO16 / RX2 |
| DFPlayer | VCC | ESP32 VIN/VN (5V) |
| DFPlayer | GND | GND |
| DFPlayer | SPK1 | Passive speaker terminal 1 |
| DFPlayer | SPK2 | Passive speaker terminal 2 |

**Important:** There is no separate buzzer in the final hardware. DFPlayer SPK1/SPK2 connect directly to the 4Ω 5W passive speaker. There is no GPIO25 connection.

### Final I²C breadboard correction

Keep one breadboard column as SDA and a separate column as SCL. The following arrangement is recommended:

- Column 5 = SDA: ESP32 GPIO21 + BH1750 SDA + OLED SDA
- Column 6 = SCL: ESP32 GPIO22 + BH1750 SCL + OLED SCL

The OLED SDA/SCL connections must not be crossed.

## Remote commands

Supported commands:

- `PLAY_AUDIO` — play MP3 file 0001–0073
- `CHECK_PLANT` — read sensors, show a checking message, then play the most relevant status response
- `TIME_GREETING` — choose morning/afternoon/evening/night audio using NTP time in IST
- `OLED_ON`, `OLED_OFF`
- `AUDIO_ON`, `AUDIO_OFF`
- `SILENT_MODE`
- `DISPLAY_MODE` = AUTO / HEALTH / SENSORS / STATUS / SAVER
- `WATERED` — log a human watering event without fabricating sensor values
- `CALIBRATE_DRY` / `CALIBRATE_WET` — project-specific soil calibration

## Time awareness

The ESP32 uses NTP time over Wi-Fi with an India Standard Time offset (+05:30). No RTC module is required for this design.

Current period mapping:

- 05:00–11:59 → 0025 morning
- 12:00–16:59 → 0026 afternoon
- 17:00–21:59 → 0027 evening
- 22:00–04:59 → 0067 night

The firmware announces each period at most once per calendar day and avoids repeating it on every telemetry packet.

## Plant profile: Golden Pothos

PlantPal's default profile is **Golden Pothos (Epipremnum aureum)**. It was selected because it is attractive, beginner-friendly, adaptable, and suitable for a compact planter. RHS guidance describes Epipremnum as an easy-to-grow houseplant that prefers bright indirect light, tolerates some shade, should not be overwatered, and grows best around 18–30°C. RHS also recommends allowing the top ~2 cm (1 inch) of compost to dry before watering and keeping the growing medium moisture-retentive but well-drained. See the sources below.

The PlantPal lux, humidity and sensor-percentage bands are **engineering thresholds for this particular prototype**, not universal scientific limits for all Pothos plants. Soil moisture percentage is derived from the user's sensor's dry/wet calibration and must be calibrated in the actual local soil.

Sources:
- https://www.rhs.org.uk/plants/epipremnum/growing-guide
- https://www.rhs.org.uk/plants/91403/epipremnum-aureum/details

## Local garden soil

Garden soil can compact in a container and hold water unevenly. PlantPal therefore treats soil moisture as a calibrated sensor condition and does not pretend a raw ADC number is a universal moisture percentage. Use the actual final soil in the final pot for dry/wet calibration.

## Setup

### Backend

```bash
cd backend
python -m venv .venv
# Windows
.venv\\Scripts\\activate
# macOS/Linux
# source .venv/bin/activate
pip install -r requirements.txt
python app.py
```

### Render

Use the backend service as a Python web service with Gunicorn, for example:

```text
gunicorn app:app
```

Keep `plantpal.db` untracked. On Render's ephemeral filesystem, SQLite data is suitable for a college prototype but is not durable storage across all restarts/redeployments. A persistent hosted database should be used for a production deployment.

### ESP32

1. Open `firmware/plantpal_esp32/plantpal_esp32.ino`.
2. Copy `firmware/plantpal_esp32/config.example.h` to `firmware/plantpal_esp32/config.h` (the real `config.h` is ignored by Git).
3. Set your Wi-Fi credentials locally in that ignored file.
4. Use **ESP32 Dev Module**.
5. Upload to the correct COM port.
6. Open Serial Monitor at **115200 baud**.

Do not commit real Wi-Fi credentials to GitHub.

## Final audio library

The SD card contains 73 files, `0001.mp3` through `0073.mp3`, generated using ElevenLabs HOPE (Professional & Clear), covering:

- personality/greetings
- soil/water
- light
- temperature
- humidity
- plant health
- cloud/remote commands
- touch interaction
- quiet/audio modes
- day/night greetings
- sensor/cloud errors

Whenever an audio track is played, PlantPal publishes an event to the cloud. The website displays the latest spoken text, and the OLED shows the same message temporarily before returning to its normal display cycle.

## OLED behavior

The OLED has:

- live sensor pages
- health index page
- device/status page
- temporary action/message overlays
- custom monochrome iconography for water, sunlight, warnings and the plant
- remote ON/OFF
- remote display modes
- automatic screen saver after inactivity
- touch wake-up

Unicode emoji are used in the website UI. The OLED uses hand-drawn monochrome icons because a standard 0.96-inch SSD1306 cannot reliably render full Unicode emoji fonts.

## Design philosophy

PlantPal is intentionally **human-assisted**, not fully automated. There is no automatic water pump. The system measures and interprets plant conditions, alerts the caretaker, supports remote interaction, and allows the human to decide when and how the plant is watered.

## Important prototype limitation

The remote control API is designed for a college prototype and currently has no production-grade user authentication. For a public deployment, add authentication/authorization before exposing actuator controls broadly.
