# PlantPal — Tulsi Embedded + IoT Final Package

This package extends the supplied PlantPal exhibition firmware and Flask/SQLite backend with a consistent OLED mode selector, Tulsi-specific soil guidance, and 100% default speaker level.

## What changed

- Nine matching OLED modes on the Render dashboard and local ESP32 page: `AUTO`, `OVERVIEW`, `SENSORS`, `SOIL`, `LIGHT`, `CLIMATE`, `HEALTH`, `STATUS`, `SAVER`.
- Remote mode commands are validated by Flask and accepted/executed by the ESP32. The mode is persisted in Preferences and reported in telemetry.
- `SAVER` turns the OLED off; **Wake OLED** or a touch interaction restores `AUTO`.
- Volume UI uses 0–100%; it maps to DFPlayer software level 0–30. New/updated firmware initializes to **30/30 (100%) once**, then remembers subsequent choices.
- Tulsi soil guidance distinguishes botanical care from the sensor scale. The firmware's project-defined bands are: 0–19 very dry, 20–34 getting dry, 35–70 target/good band, 71–85 wet, >85 very wet.
- The 35–70% figure is a relative probe reading mapped between calibration points; it is **not** a scientific volumetric water-content percentage.
- Suggested care: keep Tulsi soil moist but well-drained; check the upper ~2.5 cm / 1 inch of soil and water when it feels dry and the sensor is getting low. Do not keep it waterlogged.
- Existing hardware pin map, audio tracks, command names and API endpoints are preserved.

## OLED modes

| Mode | Display purpose |
|---|---|
| AUTO | Rotates through overview, sensor, soil, light, climate and status pages |
| OVERVIEW | Current plant state, health score and compact readings |
| SENSORS | Soil raw/index, lux, temperature, humidity, touch and audio validity |
| SOIL | Moisture index, target band and soil state |
| LIGHT | BH1750 lux reading and light state |
| CLIMATE | DHT11 temperature and relative humidity |
| HEALTH | Composite project health percentage and label |
| STATUS | Local device / Wi-Fi / audio status |
| SAVER | OLED display off; wake/touch returns to AUTO |

## Soil guidance for Tulsi

The botanical source describes Tulsi (*Ocimum tenuiflorum*) as preferring moist, well-drained soil and doing poorly in waterlogged soils. A CBSE Academic herbal-care guide recommends watering when the top inch of soil is dry. The dashboard uses these as qualitative plant-care guidance, while 35–70% is the project's chosen relative sensor band.

- Below 20%: very dry — inspect soil and water if the upper layer is dry.
- 20–34%: getting dry — check the upper ~2.5 cm and consider watering.
- 35–70%: project target band — do not water just to increase the number.
- 71–85%: wet — allow drainage and drying before the next watering.
- Above 85%: very wet — avoid adding water; check drainage.

Calibrate with the actual final pot and soil. The defaults `SOIL_DRY_VALUE=3000` and `SOIL_WET_VALUE=1200` are starting points only.

Care sources:
- NC State Extension Plant Toolbox, *Ocimum tenuiflorum*: https://plants.ces.ncsu.edu/plants/ocimum-tenuiflorum/
- CBSE Academic, *Herbal Heritage* (Tulsi care guidance): https://cbseacademic.nic.in/web_material/Curriculum23/publication/HERBAL_HERITAGE7.pdf

## Hardware pin map

| Part | ESP32 connection |
|---|---|
| OLED SDA + BH1750 SDA | GPIO21 |
| OLED SCL + BH1750 SCL | GPIO22 |
| Capacitive soil sensor AOUT | GPIO34 |
| DHT11 DATA | GPIO4 |
| TTP223 SIG | GPIO27 |
| Buzzer + | GPIO25 |
| DFPlayer TX → ESP32 RX2 | GPIO16 |
| ESP32 TX2 → DFPlayer RX | GPIO17 |
| DFPlayer power | Dedicated 5 V supply, with common GND |
| Passive speaker | DFPlayer SPK1 / SPK2 only; neither terminal to GND |

`VN` is GPIO39 on the user's board and must not be used as a 5 V supply pin.

## Install / update

1. Open `firmware/PlantPal_Tulsi_Exhibition_Final/PlantPal_Tulsi_Exhibition_Final.ino` in Arduino IDE.
2. Copy `config.example.h` to `config.h` in the same sketch directory, then enter your own Wi-Fi credentials. Keep `config.h` private and excluded from GitHub.
3. Select **ESP32 Dev Module** and the detected COM port. Close Serial Monitor before uploading.
4. Upload firmware. Open Serial Monitor at 115200 baud and verify OLED, BH1750, DHT11, DFPlayer and Wi-Fi startup.
5. Replace the current cloud backend `app.py` and add the package's `templates/index.html` to the same backend deployment, retaining the existing `requirements.txt`.
6. Run one full on-device test before using remote controls. Website commands require ESP32 station Wi-Fi plus a reachable cloud server.

## Volume note

The dashboard's 100% equals DFPlayer's documented software level 30/30. It does not guarantee a distortion-free acoustic output. Test at full level using a stable 5 V supply and reduce it if the speaker distorts, the ESP32 resets, or power becomes unstable.

## Validation status

Source-level checks are included in this delivery. A real ESP32 compile/upload and a live Render integration test still need to be done with the user's board and deployment because they cannot be proven by static source inspection alone.
