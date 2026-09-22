# 🌱 PlantPal — Embedded-First Smart Plant System

PlantPal is a 3-person college Embedded Systems project built around an ESP32. The design deliberately treats **embedded operation as the primary system** and the cloud/I​​oT layer as an optional extension.

## Architecture

```text
Plant + Sensors
      ↓
     ESP32
      ↓
 ┌────┴───────────────────┐
 │ Local embedded system  │
 │ sensors / health /     │
 │ OLED / touch / audio   │
 └────┬───────────────────┘
      │ optional Wi-Fi
      ↓
     Render
      ↕
   Dashboard
```

The plant-monitoring functions continue to work without Wi-Fi, Internet, Render, or a database connection.

## Hardware

- ESP32 Dev Module / ESP32-WROOM-32
- 0.96-inch SSD1306 OLED, I²C, address 0x3C or 0x3D
- BH1750 light sensor
- Capacitive soil moisture sensor V2.0
- DHT11 temperature/humidity module
- TTP223 capacitive touch module
- DFPlayer Mini
- 4Ω 5W passive speaker
- 4GB microSD card containing the PlantPal audio library
- Solderless breadboard and jumpers

**There is no separate buzzer. GPIO25 is unused.**

## Exact final pins

| Component | Connection |
|---|---|
| OLED SDA | GPIO21 |
| OLED SCL | GPIO22 |
| BH1750 SDA | GPIO21 |
| BH1750 SCL | GPIO22 |
| Soil AOUT | GPIO34 |
| DHT11 DATA | GPIO4 |
| TTP223 SIG | GPIO27 |
| DFPlayer TX | GPIO16 / ESP32 RX2 |
| DFPlayer RX | GPIO17 / ESP32 TX2 |
| DFPlayer VCC | ESP32 VIN/VN (5V while USB powered) |
| DFPlayer GND | GND |
| Speaker | DFPlayer SPK1/SPK2 |

### Breadboard I²C topology

Because A–E in a single numbered breadboard column are electrically common:

```text
Column 5 = SDA
A5 → GPIO21
C5 → BH1750 SDA
E5 → OLED SDA

Column 6 = SCL
A6 → GPIO22
C6 → BH1750 SCL
E6 → OLED SCL
```

Do **not** swap the OLED SDA/SCL positions.

## Audio library

The firmware knows all 73 PlantPal tracks and displays the spoken text on the OLED/cloud dashboard when that track is triggered.

Recommended SD layout:

```text
/MP3/0001.mp3
/MP3/0002.mp3
...
/MP3/0073.mp3
```

The code uses the DFRobot `playMp3Folder()` API by default. The serial console also supports a root-file playback mode for boards whose SD layout requires it.

The official DFRobot library documents `playMp3Folder()`, `play()`, `readFileCounts()`, `readCurrentFileNumber()` and the DFPlayer event/error API. citeturn802609search0turn802609search1

## Local behaviour

The firmware provides:

- Sensor reading and filtering
- Soil calibration
- Plant-specific interpretation
- Composite health score
- Plant status messages
- OLED pages
- Hand-drawn monochrome icons
- OLED message overlays
- Automatic screen saver
- TTP223 single/double/long-touch interactions
- Local speaker playback
- DFPlayer diagnostics
- Local serial command console
- Time-aware morning/afternoon/evening/night greetings when NTP is available
- Local operation with Wi-Fi disconnected

## Plant profile

The selected plant is **Golden Pothos (Epipremnum aureum)**. University Extension guidance describes pothos as a low-maintenance houseplant that prefers moderate-to-bright/bright indirect light, avoids direct sun, and should be watered after the soil/medium dries rather than kept continuously saturated. Penn State also gives average room-temperature guidance of roughly 60–80°F. citeturn234926search0turn234926search1turn234926search2

PlantPal converts those qualitative requirements into engineering bands suitable for the device. Lux bands and soil percentages are **project-specific operating bands**, not universal botanical units.

## Soil calibration

The initial firmware defaults are:

```cpp
SOIL_DRY_VALUE  = 3000
SOIL_WET_VALUE  = 1200
```

These are only starting values. Calibrate again after the actual local garden soil and final pot are installed.

Serial commands:

```text
cal dry
cal wet
cal reset
```

The dry point should be measured from the final dry reference and the wet point from the intended watered reference. The sensor percentage is a calibrated relative scale.

## Serial console

Open Serial Monitor at **115200 baud** and use:

```text
help
status
sensors
check
hello
plant
show
scan / i2c
audio 1
test audio
audio info
stop
audio on
audio off
audiopath folder
audiopath root
oled on
oled off
mode auto
mode sensors
mode health
mode status
mode saver
volume 18
time
wifi
cal dry
cal wet
cal reset
cloud on
cloud off
quiet
```

## Optional cloud layer

The backend provides:

- `POST /api/sensor-data`
- `GET /api/latest`
- `GET /api/history`
- `POST /api/commands`
- `GET /api/commands/next`
- `POST /api/commands/<id>/ack`
- `GET /api/commands/history`
- `POST /api/events`
- `GET /api/events`
- `GET /health`

The command layer supports remote:

- Plant check
- Time greeting
- Any of the 73 audio tracks
- Speaker stop
- OLED on/off
- Audio on/off
- Quiet mode
- OLED display mode
- Speaker volume
- Manual watering log
- Soil dry/wet calibration
- Remote message
- Screen saver/wake

The ESP32 polls commands over HTTPS. A command ID is stored in ESP32 Preferences so a duplicated pending command is not replayed as a second physical action.

## Time

The ESP32 uses NTP only when Wi-Fi is available and formats time for IST (+05:30):

- 05:00–11:59 → track 0025
- 12:00–16:59 → track 0026
- 17:00–21:59 → track 0027
- 22:00–04:59 → track 0067

No RTC is required for the IoT-enhanced build. Environmental monitoring does not depend on time synchronization.

## Render

A Render service can be used for the dashboard/API. A cron job is not required to keep it awake while PlantPal is operating because the ESP32 itself provides regular inbound requests. Free Render filesystems can be ephemeral; SQLite is acceptable for a college demonstration, but a persistent managed database is preferable for production history.

## Security

`config.h` is intentionally gitignored. Keep actual Wi-Fi credentials out of the public repository. The example file is `config.example.h`.

## Deployment

### Backend

Use the repository root or the backend directory according to your Render configuration. A typical Python web service command is:

```text
gunicorn backend.app:app
```

### Firmware

Open `firmware/plantpal_esp32/plantpal_esp32.ino` in Arduino IDE with `config.h` in the same sketch folder.

Select:

```text
Board: ESP32 Dev Module
Port: COM7 (or the detected ESP32 COM port)
Serial: 115200
```

Install:

- ESP32 by Espressif Systems
- Adafruit GFX Library
- Adafruit SSD1306
- BH1750
- DHT sensor library
- Adafruit Unified Sensor
- DFRobotDFPlayerMini

## Final hardware validation

Do not cut the breadboard until these all pass:

```text
OLED                 ✓
BH1750               ✓
Soil sensor          ✓
DHT11                ✓
TTP223               ✓
DFPlayer             ✓
Speaker              ✓
Local plant logic    ✓
Wi-Fi (optional)     ✓
Cloud telemetry      ✓
Remote command       ✓
```

Then run the system for an extended period before permanent enclosure assembly.
