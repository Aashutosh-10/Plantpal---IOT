/*
  ============================================================================
  PLANTPAL - TULSI SMART PLANT SYSTEM
  FINAL COLLEGE SUBMISSION FIRMWARE
  ============================================================================

  Platform : ESP32 Dev Module / ESP32-WROOM-32 / ESP32-D0WD-V3
  Plant    : Tulsi (Ocimum tenuiflorum / holy basil)

  PURPOSE
  -------
  PlantPal is an embedded-first smart plant companion. The ESP32 reads the
  environmental sensors locally, calculates a project-specific Tulsi status,
  drives the OLED, reacts to touch, controls the buzzer and DFPlayer, and
  exposes a local Wi-Fi dashboard for a phone.

  The local plant logic NEVER depends on Internet access.

  HARDWARE PIN MAP
  ----------------
  I2C SDA          GPIO21
  I2C SCL          GPIO22
  Soil AOUT        GPIO34 (ADC)
  DHT11 DATA       GPIO4
  TTP223 SIG       GPIO27
  Buzzer +         GPIO25
  DFPlayer TX      -> ESP32 GPIO16 / RX2
  DFPlayer RX      <- ESP32 GPIO17 / TX2

  OLED + BH1750 share the I2C bus on GPIO21/22.

  DFPLAYER POWER
  --------------
  DFPlayer VCC must receive a proper 5V supply.
  DFPlayer GND MUST share ground with the ESP32.
  Do NOT use GPIO39 / VN as a power output. On this board VN is GPIO39.

  DFPLAYER AUDIO
  --------------
  Final audio library:
      /mp3/0001.mp3 ... /mp3/0073.mp3
  Four-digit filenames are recommended and playMp3Folder() is used.

  SPEAKER
  -------
  Passive speaker connects only between DFPlayer SPK1 and SPK2.
  Neither SPK1 nor SPK2 is connected to GND.

  LOCAL + CLOUD IOT
  -----------------
  The ESP32 creates a local Wi-Fi access point:
      SSID     : PlantPal
      Password : plantpal123
      Address  : http://192.168.4.1

  At the same time, it connects as a Wi-Fi station to the credentials in
  config.h and uploads telemetry to SERVER_BASE_URL. This allows the final
  Render dashboard to show live device data and queue remote commands.

  Local embedded operation continues even when the Internet/cloud is down.

  IMPORTANT BOTANICAL / SENSOR NOTE
  ---------------------------------
  The soil percentage is a calibrated project scale, NOT a laboratory
  volumetric water-content measurement. Soil calibration must be done using
  the actual final Tulsi pot and soil.

  Light, temperature and humidity bands below are engineering bands selected
  for this project. They are not claimed to be universal botanical limits.

  LOCAL TOUCH CONTROLS
  --------------------
  Single tap : greeting / wake display
  Double tap : plant check with spoken result
  Long press: cycle AUTO -> SENSORS -> HEALTH -> STATUS -> AUTO

  PHONE DASHBOARD
  ---------------
  Live values, plant health, condition, audio controls, 73-track library,
  buzzer test, watering log, OLED controls, display mode and soil calibration.

  ============================================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#if __has_include("config.h")
  #include "config.h"
  #define PLANTPAL_HAS_CONFIG 1
#else
  #define PLANTPAL_HAS_CONFIG 0
  const char* WIFI_SSID = "";
  const char* WIFI_PASSWORD = "";
  const char* SERVER_BASE_URL = "";
  const char* DEVICE_ID = "plantpal-01";
  #define CLOUD_ENABLED_DEFAULT true
  #define SERIAL_DEBUG_DEFAULT true
  #define OLED_DEFAULT_ENABLED true
  #define AUDIO_DEFAULT_ENABLED true
  #define DISPLAY_DEFAULT_MODE "AUTO"
#endif

#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BH1750.h>
#include <DHT.h>
#include <DFRobotDFPlayerMini.h>
#include <Preferences.h>
#include <math.h>
#include <time.h>

// ============================================================================
 // 01. WIFI + CLOUD
 // ============================================================================
 // Local AP is kept for direct phone testing.
 // STA mode simultaneously connects to the configured phone/router hotspot so
 // telemetry and remote commands can reach the Render backend.

static const char* AP_SSID     = "PlantPal";
static const char* AP_PASSWORD = "plantpal123";
static const char* AP_IP_TEXT  = "192.168.4.1";

WebServer server(80);

bool cloudEnabled = CLOUD_ENABLED_DEFAULT;
bool cloudConnected = false;
uint32_t lastCloudTelemetryAt = 0;
uint32_t lastCloudCommandPollAt = 0;
uint32_t lastCloudRetryAt = 0;
uint32_t lastCloudEventAt = 0;
uint32_t lastCloudConnectReportAt = 0;
bool wifiAttemptActive = false;
uint32_t wifiAttemptStartedAt = 0;
bool localAPActive = false;

static const uint32_t CLOUD_TELEMETRY_INTERVAL_MS = 5000UL;
static const uint32_t CLOUD_COMMAND_POLL_INTERVAL_MS = 1800UL;
static const uint32_t CLOUD_RETRY_INTERVAL_MS = 10000UL;
static const uint32_t CLOUD_HTTP_TIMEOUT_MS = 12000UL;
static const uint32_t CLOUD_CONNECT_TIMEOUT_MS = 8000UL;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000UL;
static const uint32_t CLOUD_EVENT_MIN_INTERVAL_MS = 1000UL;

// ============================================================================
// 02. PIN DEFINITIONS
// ============================================================================

static const uint8_t PIN_SDA    = 21;
static const uint8_t PIN_SCL    = 22;
static const uint8_t PIN_SOIL   = 34;
static const uint8_t PIN_DHT    = 4;
static const uint8_t PIN_TOUCH  = 27;
static const uint8_t PIN_BUZZER = 25;
static const uint8_t PIN_DF_RX  = 16; // ESP32 RX2 <- DFPlayer TX
static const uint8_t PIN_DF_TX  = 17; // ESP32 TX2 -> DFPlayer RX

static const uint8_t OLED_ADDR_A = 0x3C;
static const uint8_t OLED_ADDR_B = 0x3D;
static const uint8_t BH1750_ADDR = 0x23;

// ============================================================================
// 03. HARDWARE OBJECTS
// ============================================================================

Adafruit_SSD1306 display(128, 64, &Wire, -1);
BH1750 lightMeter;
DHT dht(PIN_DHT, DHT11);
HardwareSerial dfSerial(2);
DFRobotDFPlayerMini player;
Preferences prefs;

// ============================================================================
// 04. APPLICATION CONSTANTS
// ============================================================================

static const char* FW_VERSION = "PlantPal Tulsi Final 1.0";
static const char* PLANT_NAME = "Tulsi";
static const char* PLANT_SPECIES = "Ocimum tenuiflorum";

static const uint32_t SENSOR_INTERVAL_MS = 2500UL;
static const uint32_t OLED_INTERVAL_MS = 200UL;
static const uint32_t AUTO_PAGE_INTERVAL_MS = 4000UL;
static const uint32_t OLED_SAVER_TIMEOUT_MS = 90000UL;
static const uint32_t MESSAGE_SCREEN_MS = 5000UL;
static const uint32_t CONDITION_PERSIST_READS = 3UL;
static const uint32_t CONDITION_AUDIO_COOLDOWN_MS = 300000UL;
static const uint32_t WATER_EVENT_COOLDOWN_MS = 120000UL;
static const uint32_t AUDIO_WATCHDOG_MS = 15000UL;
static const uint32_t TOUCH_DEBOUNCE_MS = 70UL;
static const uint32_t TOUCH_DOUBLE_WINDOW_MS = 750UL;
static const uint32_t TOUCH_LONG_PRESS_MS = 2200UL;

// ============================================================================
// 05. TULSI ENGINEERING BANDS
// ============================================================================
// These are project bands. Soil percentage is calibration-dependent.

// Soil percentage
static const int SOIL_VERY_DRY_PCT = 20;
static const int SOIL_DRY_PCT = 35;
static const int SOIL_GOOD_MAX_PCT = 70;
static const int SOIL_WET_PCT = 85;

// BH1750 lux bands used by this project
static const float LIGHT_DARK_LUX = 100.0f;
static const float LIGHT_LOW_LUX = 1000.0f;
static const float LIGHT_GOOD_MAX_LUX = 15000.0f;
static const float LIGHT_VERY_BRIGHT_LUX = 30000.0f;

// DHT11 temperature bands
static const float TEMP_COLD_C = 18.0f;
static const float TEMP_GOOD_MAX_C = 32.0f;
static const float TEMP_WARM_C = 35.0f;

// DHT11 humidity bands
static const float HUMIDITY_DRY_PCT = 35.0f;
static const float HUMIDITY_GOOD_MAX_PCT = 80.0f;
static const float HUMIDITY_HIGH_PCT = 85.0f;

// Default soil calibration. Replace through phone dashboard after final pot.
static const int DEFAULT_SOIL_DRY_RAW = 3000;
static const int DEFAULT_SOIL_WET_RAW = 1200;

// ============================================================================
// 06. RUNTIME FLAGS
// ============================================================================

bool oledReady = false;
bool oledEnabled = true;
bool oledSleeping = false;
bool bh1750Ready = false;
bool dfPlayerReady = false;
bool audioEnabled = true;
bool autoAudioEnabled = true;

int audioVolume = 20;
int soilDryCalibration = DEFAULT_SOIL_DRY_RAW;
int soilWetCalibration = DEFAULT_SOIL_WET_RAW;

String displayMode = "AUTO";
uint8_t autoPage = 0;

// ============================================================================
// 07. SENSOR STATE
// ============================================================================

struct SensorState {
  int soilRaw = 0;
  int soilPercent = 0;
  float lux = NAN;
  float temperature = NAN;
  float humidity = NAN;

  bool soilValid = false;
  bool lightValid = false;
  bool temperatureValid = false;
  bool humidityValid = false;

  uint8_t soilBadReads = 0;
  uint8_t lightBadReads = 0;
  uint8_t tempBadReads = 0;
  uint8_t humidityBadReads = 0;
};

SensorState sensors;

// ============================================================================
// 08. STATUS MODEL
// ============================================================================

enum PlantCondition : uint8_t {
  CONDITION_SENSOR_ERROR = 0,
  CONDITION_SOIL_VERY_DRY,
  CONDITION_SOIL_DRY,
  CONDITION_SOIL_GOOD,
  CONDITION_SOIL_WET,
  CONDITION_LIGHT_DARK,
  CONDITION_LIGHT_LOW,
  CONDITION_LIGHT_GOOD,
  CONDITION_LIGHT_BRIGHT,
  CONDITION_TEMP_COLD,
  CONDITION_TEMP_GOOD,
  CONDITION_TEMP_WARM,
  CONDITION_TEMP_HOT,
  CONDITION_HUMIDITY_DRY,
  CONDITION_HUMIDITY_GOOD,
  CONDITION_HUMIDITY_HIGH,
  CONDITION_HEALTHY
};

PlantCondition currentCondition = CONDITION_SENSOR_ERROR;
PlantCondition candidateCondition = CONDITION_SENSOR_ERROR;
PlantCondition lastSpokenCondition = CONDITION_SENSOR_ERROR;
uint32_t candidateConditionCount = 0;
uint32_t lastConditionAudioAt = 0;

int healthScore = 0;
String healthLabel = "CHECKING";
String plantStatus = "PlantPal is checking your Tulsi.";
String soilState = "CHECKING";
String lightState = "CHECKING";
String temperatureState = "CHECKING";
String humidityState = "CHECKING";

// ============================================================================
// 09. INTERACTION / UI STATE
// ============================================================================

String messageTitle = "PLANTPAL";
String messageText = "";
uint32_t messageUntil = 0;
uint32_t lastUserInteraction = 0;
uint32_t lastAutoPageChange = 0;
uint32_t lastOLEDRefresh = 0;

// ============================================================================
// 10. AUDIO QUEUE
// ============================================================================

static const uint8_t AUDIO_QUEUE_SIZE = 6;

struct AudioJob {
  int track = 0;
  String message = "";
};

AudioJob audioQueue[AUDIO_QUEUE_SIZE];
uint8_t audioHead = 0;
uint8_t audioTail = 0;
uint8_t audioCount = 0;

bool audioPlaying = false;
uint32_t audioStartedAt = 0;
int lastAudioTrack = 0;
String lastAudioMessage = "";

String lastAction = "BOOT";
String lastActionMessage = "PlantPal is starting.";
uint32_t lastActionAt = 0;

// ============================================================================
// 11. TOUCH STATE
// ============================================================================

bool rawTouch = false;
bool stableTouch = false;
uint32_t touchChangedAt = 0;
uint32_t touchPressedAt = 0;
uint32_t lastTapAt = 0;
uint8_t tapCount = 0;
bool longPressHandled = false;

// ============================================================================
// 12. WATERING EVENT STATE
// ============================================================================

float previousSoilPercent = NAN;
uint32_t lastWaterEventAt = 0;

// ============================================================================
// 13. SCHEDULER STATE
// ============================================================================

uint32_t lastSensorRead = 0;
uint32_t lastPeriodicSummaryAt = 0;
uint32_t lastWebTouchAt = 0;

// ============================================================================
// 14. 73-TRACK AUDIO LIBRARY
// ============================================================================

const char* TRACK_TEXT[74] = {
  "",
  "Hello! I'm PlantPal.",
  "Your plant is doing great.",
  "Your plant needs some water.",
  "Your plant has enough moisture.",
  "The soil is getting dry.",
  "The soil is nicely moist.",
  "It's a little too dry for your plant.",
  "Your plant is getting plenty of light.",
  "Your plant needs more light.",
  "The light level looks good.",
  "It's quite dark here.",
  "It's getting a little too warm.",
  "The temperature looks comfortable.",
  "It's a little chilly for your plant.",
  "The humidity looks good.",
  "The air is getting too dry.",
  "The humidity is quite high.",
  "Time for a little drink.",
  "Your plant is thirsty.",
  "Your plant doesn't need water right now.",
  "Please water your plant.",
  "Your plant is happy and healthy.",
  "Everything looks good.",
  "I'll keep an eye on your plant.",
  "Good morning! Let's check on your plant.",
  "Good afternoon! Here's your plant update.",
  "Good evening! Let's see how your plant is doing.",
  "Plant check complete.",
  "Sensor check complete.",
  "PlantPal is ready.",
  "I'm right here!",
  "Thanks for checking on me.",
  "It's time for a plant check.",
  "I'll keep an eye on your plant.",
  "Everything is under control.",
  "The soil is very dry.",
  "The soil moisture is looking good.",
  "Please check the soil before watering.",
  "Thanks for taking care of me.",
  "The light level is comfortable for your plant.",
  "It's a little dark for your plant.",
  "That's plenty of light for now.",
  "It's getting quite warm here.",
  "The temperature is in a comfortable range.",
  "It's getting a little cold here.",
  "The air feels comfortable for your plant.",
  "The air is a little too dry.",
  "The air is quite humid right now.",
  "Your plant is doing well overall.",
  "Your plant needs a little attention.",
  "I've found something that needs your attention.",
  "The plant check is complete. Everything looks normal.",
  "Command received.",
  "I'm checking your plant now.",
  "The remote command is complete.",
  "I've received your request.",
  "Remote control is active.",
  "The cloud connection is active.",
  "The cloud connection has been restored.",
  "Touch detected!",
  "Hello there!",
  "Thanks for checking on me.",
  "Speaker test complete.",
  "Quiet mode is now active.",
  "Audio has been enabled.",
  "I'll stay quiet for now.",
  "Good night! I'll keep monitoring your plant.",
  "I'm having trouble reading a sensor.",
  "The light sensor needs attention.",
  "The temperature sensor needs attention.",
  "The humidity sensor needs attention.",
  "The soil sensor needs attention.",
  "The cloud connection is unavailable right now."
};

// ============================================================================
// 15. FORWARD DECLARATIONS
// ============================================================================

const char* trackText(int track);
String conditionName(PlantCondition condition);
bool requestAudio(int track, const String& message, bool interruptCurrent, bool allowQueue);
void runPlantCheck();
void updateSensors();
void updatePlantModel();
void renderOLED();
void showMessage(const String& title, const String& text, uint32_t durationMs = 5000UL);
void serviceAudio();

// ============================================================================
// 15. BASIC HELPERS
// ============================================================================

bool finiteFloat(float value) {
  return !isnan(value) && !isinf(value);
}

String jsonEscape(const String& value) {
  String out;
  out.reserve(value.length() + 8);

  for (size_t i = 0; i < value.length(); ++i) {
    char c = value[i];
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out += c;
  }

  return out;
}

String htmlEscape(const String& value) {
  String out;
  out.reserve(value.length() + 8);

  for (size_t i = 0; i < value.length(); ++i) {
    switch (value[i]) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += value[i]; break;
    }
  }

  return out;
}

const char* trackText(int track) {
  if (track < 1 || track > 73) return "Invalid track.";
  return TRACK_TEXT[track];
}

String conditionName(PlantCondition condition) {
  switch (condition) {
    case CONDITION_SENSOR_ERROR: return "SENSOR ERROR";
    case CONDITION_SOIL_VERY_DRY: return "VERY DRY SOIL";
    case CONDITION_SOIL_DRY: return "DRY SOIL";
    case CONDITION_SOIL_GOOD: return "GOOD SOIL";
    case CONDITION_SOIL_WET: return "WET SOIL";
    case CONDITION_LIGHT_DARK: return "VERY LOW LIGHT";
    case CONDITION_LIGHT_LOW: return "LOW LIGHT";
    case CONDITION_LIGHT_GOOD: return "GOOD LIGHT";
    case CONDITION_LIGHT_BRIGHT: return "BRIGHT LIGHT";
    case CONDITION_TEMP_COLD: return "COOL";
    case CONDITION_TEMP_GOOD: return "COMFORTABLE TEMP";
    case CONDITION_TEMP_WARM: return "WARM";
    case CONDITION_TEMP_HOT: return "HOT";
    case CONDITION_HUMIDITY_DRY: return "DRY AIR";
    case CONDITION_HUMIDITY_GOOD: return "GOOD HUMIDITY";
    case CONDITION_HUMIDITY_HIGH: return "HIGH HUMIDITY";
    case CONDITION_HEALTHY: return "HEALTHY";
  }
  return "UNKNOWN";
}

int clampTrack(int track) {
  return constrain(track, 1, 73);
}

// ============================================================================
// 16. PREFERENCES
// ============================================================================

void loadSettings() {
  prefs.begin("plantpal", false);

  oledEnabled = prefs.getBool("oled", true);
  audioEnabled = prefs.getBool("audio", true);
  autoAudioEnabled = prefs.getBool("autoAudio", true);
  audioVolume = constrain((int)prefs.getUInt("volume", 20), 0, 30);
  soilDryCalibration = prefs.getInt("dry", DEFAULT_SOIL_DRY_RAW);
  soilWetCalibration = prefs.getInt("wet", DEFAULT_SOIL_WET_RAW);
  displayMode = prefs.getString("mode", "AUTO");

  displayMode.toUpperCase();

  if (displayMode != "AUTO" &&
      displayMode != "SENSORS" &&
      displayMode != "HEALTH" &&
      displayMode != "STATUS") {
    displayMode = "AUTO";
  }
}

void saveSettings() {
  prefs.putBool("oled", oledEnabled);
  prefs.putBool("audio", audioEnabled);
  prefs.putBool("autoAudio", autoAudioEnabled);
  prefs.putUInt("volume", audioVolume);
  prefs.putInt("dry", soilDryCalibration);
  prefs.putInt("wet", soilWetCalibration);
  prefs.putString("mode", displayMode);
}

// ============================================================================
// 17. SOIL READING / CALIBRATION
// ============================================================================

int readSoilRawAveraged() {
  const uint8_t samples = 9;
  int values[samples];

  for (uint8_t i = 0; i < samples; ++i) {
    values[i] = analogRead(PIN_SOIL);
    delay(2);
  }

  // Sort small sample set and trim the highest/lowest two.
  for (uint8_t i = 1; i < samples; ++i) {
    int key = values[i];
    int j = i - 1;

    while (j >= 0 && values[j] > key) {
      values[j + 1] = values[j];
      --j;
    }

    values[j + 1] = key;
  }

  long sum = 0;
  for (uint8_t i = 2; i < samples - 2; ++i) sum += values[i];

  return (int)(sum / (samples - 4));
}

int soilPercentFromRaw(int raw) {
  if (soilDryCalibration <= soilWetCalibration) {
    return 0;
  }

  long value = map(
    raw,
    soilDryCalibration,
    soilWetCalibration,
    0,
    100
  );

  return constrain((int)value, 0, 100);
}

void updateSoil() {
  sensors.soilRaw = readSoilRawAveraged();
  sensors.soilValid = sensors.soilRaw > 80 && sensors.soilRaw < 4090;

  if (!sensors.soilValid) {
    sensors.soilBadReads = min((uint8_t)10, (uint8_t)(sensors.soilBadReads + 1));
    return;
  }

  sensors.soilBadReads = 0;
  sensors.soilPercent = soilPercentFromRaw(sensors.soilRaw);

  if (sensors.soilPercent < SOIL_VERY_DRY_PCT) {
    soilState = "VERY DRY";
  } else if (sensors.soilPercent < SOIL_DRY_PCT) {
    soilState = "GETTING DRY";
  } else if (sensors.soilPercent <= SOIL_GOOD_MAX_PCT) {
    soilState = "GOOD";
  } else if (sensors.soilPercent <= SOIL_WET_PCT) {
    soilState = "WET";
  } else {
    soilState = "VERY WET";
  }
}

// ============================================================================
// 18. BH1750 READING
// ============================================================================

void updateLight() {
  if (!bh1750Ready) {
    sensors.lightValid = false;
    sensors.lightBadReads = min((uint8_t)10, (uint8_t)(sensors.lightBadReads + 1));
    return;
  }

  float value = lightMeter.readLightLevel();

  if (!finiteFloat(value) || value < 0.0f || value > 100000.0f) {
    sensors.lightValid = false;
    sensors.lightBadReads = min((uint8_t)10, (uint8_t)(sensors.lightBadReads + 1));
    return;
  }

  sensors.lightBadReads = 0;
  sensors.lux = value;
  sensors.lightValid = true;

  if (value < LIGHT_DARK_LUX) {
    lightState = "VERY DARK";
  } else if (value < LIGHT_LOW_LUX) {
    lightState = "LOW";
  } else if (value <= LIGHT_GOOD_MAX_LUX) {
    lightState = "GOOD";
  } else if (value <= LIGHT_VERY_BRIGHT_LUX) {
    lightState = "BRIGHT";
  } else {
    lightState = "VERY BRIGHT";
  }
}

// ============================================================================
// 19. DHT11 READING
// ============================================================================

void updateDHT() {
  float temp = dht.readTemperature();
  float hum = dht.readHumidity();

  if (finiteFloat(temp) && temp > -10.0f && temp < 60.0f) {
    sensors.temperature = temp;
    sensors.temperatureValid = true;
    sensors.tempBadReads = 0;
  } else {
    sensors.tempBadReads = min((uint8_t)10, (uint8_t)(sensors.tempBadReads + 1));
    if (sensors.tempBadReads >= 3) sensors.temperatureValid = false;
  }

  if (finiteFloat(hum) && hum >= 0.0f && hum <= 100.0f) {
    sensors.humidity = hum;
    sensors.humidityValid = true;
    sensors.humidityBadReads = 0;
  } else {
    sensors.humidityBadReads = min((uint8_t)10, (uint8_t)(sensors.humidityBadReads + 1));
    if (sensors.humidityBadReads >= 3) sensors.humidityValid = false;
  }

  if (!sensors.temperatureValid) {
    temperatureState = "ERROR";
  } else if (sensors.temperature < TEMP_COLD_C) {
    temperatureState = "COOL";
  } else if (sensors.temperature <= TEMP_GOOD_MAX_C) {
    temperatureState = "COMFORTABLE";
  } else if (sensors.temperature <= TEMP_WARM_C) {
    temperatureState = "WARM";
  } else {
    temperatureState = "HOT";
  }

  if (!sensors.humidityValid) {
    humidityState = "ERROR";
  } else if (sensors.humidity < HUMIDITY_DRY_PCT) {
    humidityState = "DRY";
  } else if (sensors.humidity <= HUMIDITY_GOOD_MAX_PCT) {
    humidityState = "COMFORTABLE";
  } else if (sensors.humidity <= HUMIDITY_HIGH_PCT) {
    humidityState = "HIGH";
  } else {
    humidityState = "VERY HIGH";
  }
}

// ============================================================================
// 20. WATERING DETECTION
// ============================================================================

void detectWatering() {
  if (!sensors.soilValid) return;

  if (finiteFloat(previousSoilPercent)) {
    float rise = (float)sensors.soilPercent - previousSoilPercent;

    if (rise >= 12.0f && millis() - lastWaterEventAt >= WATER_EVENT_COOLDOWN_MS) {
      lastWaterEventAt = millis();

      showMessage("WATERING DETECTED", "Soil moisture has increased.", MESSAGE_SCREEN_MS);

      if (audioEnabled && dfPlayerReady) {
        requestAudio(39, trackText(39), true, true);
      }
    }
  }

  previousSoilPercent = sensors.soilPercent;
}

// ============================================================================
// 21. COMPLETE SENSOR CYCLE
// ============================================================================

void updateSensors() {
  updateSoil();
  updateLight();
  updateDHT();
  detectWatering();
}

// ============================================================================
// 22. PLANT CONDITION SELECTION
// ============================================================================

PlantCondition evaluateCondition() {
  if (!sensors.soilValid ||
      !sensors.lightValid ||
      !sensors.temperatureValid ||
      !sensors.humidityValid) {
    return CONDITION_SENSOR_ERROR;
  }

  // Watering/soil has highest priority because it is the most direct
  // root-zone condition measured by PlantPal.
  if (sensors.soilPercent < SOIL_VERY_DRY_PCT) return CONDITION_SOIL_VERY_DRY;
  if (sensors.soilPercent < SOIL_DRY_PCT) return CONDITION_SOIL_DRY;
  if (sensors.soilPercent > SOIL_WET_PCT) return CONDITION_SOIL_WET;

  // Light is especially important for Tulsi.
  if (sensors.lux < LIGHT_DARK_LUX) return CONDITION_LIGHT_DARK;
  if (sensors.lux < LIGHT_LOW_LUX) return CONDITION_LIGHT_LOW;
  if (sensors.lux > LIGHT_VERY_BRIGHT_LUX) return CONDITION_LIGHT_BRIGHT;

  // Temperature
  if (sensors.temperature < TEMP_COLD_C) return CONDITION_TEMP_COLD;
  if (sensors.temperature > TEMP_WARM_C) return CONDITION_TEMP_HOT;
  if (sensors.temperature > TEMP_GOOD_MAX_C) return CONDITION_TEMP_WARM;

  // Humidity
  if (sensors.humidity < HUMIDITY_DRY_PCT) return CONDITION_HUMIDITY_DRY;
  if (sensors.humidity > HUMIDITY_HIGH_PCT) return CONDITION_HUMIDITY_HIGH;

  return CONDITION_HEALTHY;
}

// ============================================================================
// 23. HEALTH SCORE
// ============================================================================

int scoreSoil() {
  if (!sensors.soilValid) return 0;

  const int p = sensors.soilPercent;

  if (p < 20) return 20;
  if (p < 35) return 65;
  if (p <= 70) return 100;
  if (p <= 85) return 75;
  return 45;
}

int scoreLight() {
  if (!sensors.lightValid) return 0;

  const float l = sensors.lux;

  if (l < 100) return 20;
  if (l < 1000) return 55;
  if (l <= 15000) return 100;
  if (l <= 30000) return 75;
  return 45;
}

int scoreTemperature() {
  if (!sensors.temperatureValid) return 0;

  const float t = sensors.temperature;

  if (t < 15) return 25;
  if (t < 18) return 65;
  if (t <= 32) return 100;
  if (t <= 35) return 70;
  return 25;
}

int scoreHumidity() {
  if (!sensors.humidityValid) return 0;

  const float h = sensors.humidity;

  if (h < 25) return 25;
  if (h < 35) return 65;
  if (h <= 80) return 100;
  if (h <= 85) return 70;
  return 40;
}

void updateHealth() {
  int availableWeight = 0;
  long total = 0;

  if (sensors.soilValid) {
    total += (long)scoreSoil() * 40;
    availableWeight += 40;
  }

  if (sensors.lightValid) {
    total += (long)scoreLight() * 25;
    availableWeight += 25;
  }

  if (sensors.temperatureValid) {
    total += (long)scoreTemperature() * 20;
    availableWeight += 20;
  }

  if (sensors.humidityValid) {
    total += (long)scoreHumidity() * 15;
    availableWeight += 15;
  }

  healthScore = availableWeight > 0 ? (int)(total / availableWeight) : 0;

  if (availableWeight < 100) {
    healthLabel = "CHECKING";
  } else if (healthScore >= 85) {
    healthLabel = "THRIVING";
  } else if (healthScore >= 70) {
    healthLabel = "DOING WELL";
  } else if (healthScore >= 50) {
    healthLabel = "NEEDS ATTENTION";
  } else {
    healthLabel = "ACTION NEEDED";
  }
}

void updatePlantModel() {
  PlantCondition newCondition = evaluateCondition();

  if (newCondition != candidateCondition) {
    candidateCondition = newCondition;
    candidateConditionCount = 1;
  } else {
    candidateConditionCount++;
  }

  if (candidateConditionCount >= CONDITION_PERSIST_READS) {
    currentCondition = candidateCondition;
  }

  updateHealth();

  switch (currentCondition) {
    case CONDITION_SENSOR_ERROR:
      plantStatus = "I've found something that needs your attention.";
      break;

    case CONDITION_SOIL_VERY_DRY:
      plantStatus = "Your Tulsi needs water soon.";
      break;

    case CONDITION_SOIL_DRY:
      plantStatus = "Your Tulsi is getting thirsty.";
      break;

    case CONDITION_SOIL_GOOD:
    case CONDITION_HEALTHY:
      plantStatus = "Your Tulsi is happy and healthy.";
      break;

    case CONDITION_SOIL_WET:
      plantStatus = "The soil is quite wet. Avoid extra watering for now.";
      break;

    case CONDITION_LIGHT_DARK:
      plantStatus = "Your Tulsi needs substantially more light.";
      break;

    case CONDITION_LIGHT_LOW:
      plantStatus = "Your Tulsi could use more light.";
      break;

    case CONDITION_LIGHT_GOOD:
      plantStatus = "The light level looks good.";
      break;

    case CONDITION_LIGHT_BRIGHT:
      plantStatus = "Your Tulsi is receiving very bright light.";
      break;

    case CONDITION_TEMP_COLD:
      plantStatus = "The temperature is a little cool for your Tulsi.";
      break;

    case CONDITION_TEMP_GOOD:
      plantStatus = "The temperature looks comfortable.";
      break;

    case CONDITION_TEMP_WARM:
      plantStatus = "It's getting warm for your Tulsi.";
      break;

    case CONDITION_TEMP_HOT:
      plantStatus = "It's quite hot for your Tulsi.";
      break;

    case CONDITION_HUMIDITY_DRY:
      plantStatus = "The air is getting dry.";
      break;

    case CONDITION_HUMIDITY_GOOD:
      plantStatus = "The humidity looks comfortable.";
      break;

    case CONDITION_HUMIDITY_HIGH:
      plantStatus = "The humidity is quite high.";
      break;
  }
}

// ============================================================================
// 24. AUDIO TRACK MAPPING FOR SMART EVENTS
// ============================================================================

int trackForCondition(PlantCondition condition) {
  switch (condition) {
    case CONDITION_SENSOR_ERROR: return 68;
    case CONDITION_SOIL_VERY_DRY: return 36;
    case CONDITION_SOIL_DRY: return 5;
    case CONDITION_SOIL_GOOD: return 6;
    case CONDITION_SOIL_WET: return 4;
    case CONDITION_LIGHT_DARK: return 11;
    case CONDITION_LIGHT_LOW: return 41;
    case CONDITION_LIGHT_GOOD: return 40;
    case CONDITION_LIGHT_BRIGHT: return 8;
    case CONDITION_TEMP_COLD: return 45;
    case CONDITION_TEMP_GOOD: return 44;
    case CONDITION_TEMP_WARM: return 43;
    case CONDITION_TEMP_HOT: return 12;
    case CONDITION_HUMIDITY_DRY: return 47;
    case CONDITION_HUMIDITY_GOOD: return 46;
    case CONDITION_HUMIDITY_HIGH: return 48;
    case CONDITION_HEALTHY: return 22;
  }
  return 22;
}

// ============================================================================
// 25. AUDIO QUEUE UTILITIES
// ============================================================================

void clearAudioQueue() {
  audioHead = 0;
  audioTail = 0;
  audioCount = 0;
}

bool queueAudio(int track, const String& message) {
  if (track < 1 || track > 73) return false;

  if (audioCount >= AUDIO_QUEUE_SIZE) {
    // Drop the oldest job to preserve the newest request.
    audioHead = (audioHead + 1) % AUDIO_QUEUE_SIZE;
    audioCount--;
  }

  audioQueue[audioTail].track = track;
  audioQueue[audioTail].message = message;
  audioTail = (audioTail + 1) % AUDIO_QUEUE_SIZE;
  audioCount++;
  return true;
}

bool popAudio(AudioJob& job) {
  if (audioCount == 0) return false;

  job = audioQueue[audioHead];
  audioHead = (audioHead + 1) % AUDIO_QUEUE_SIZE;
  audioCount--;
  return true;
}

// ============================================================================
// 26. OLED MESSAGE
// ============================================================================

void wakeOLED() {
  if (!oledReady || !oledEnabled) return;

  if (oledSleeping) {
    display.ssd1306_command(SSD1306_DISPLAYON);
    oledSleeping = false;
  }

  lastUserInteraction = millis();
}

void sleepOLED() {
  if (!oledReady) return;

  display.clearDisplay();
  display.display();
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  oledSleeping = true;
}

void showMessage(const String& title, const String& text, uint32_t durationMs) {
  messageTitle = title;
  messageText = text;
  messageUntil = millis() + durationMs;
  wakeOLED();
}

bool messageActive() {
  return messageUntil != 0 && (int32_t)(messageUntil - millis()) > 0;
}

void clearMessage() {
  messageUntil = 0;
  messageTitle = "PLANTPAL";
  messageText = "";
}

// ============================================================================
// 27. DFPLAYER INITIALIZATION
// ============================================================================

bool initializeDFPlayer() {
  dfSerial.begin(9600, SERIAL_8N1, PIN_DF_RX, PIN_DF_TX);
  delay(800);

  player.setTimeOut(1000);

  if (!player.begin(dfSerial, true, true)) {
    dfPlayerReady = false;
    Serial.println("[DFPLAYER] NOT FOUND");
    return false;
  }

  dfPlayerReady = true;
  player.setTimeOut(1000);
  player.volume(audioVolume);
  player.EQ(DFPLAYER_EQ_NORMAL);
  player.outputDevice(DFPLAYER_DEVICE_SD);
  delay(400);
  player.stop();
  delay(150);

  Serial.println("[DFPLAYER] READY");
  Serial.print("[DFPLAYER] SD files reported: ");
  Serial.println(player.readFileCounts());
  Serial.println("[DFPLAYER] Expected path: /mp3/0001.mp3 ... /mp3/0073.mp3");

  return true;
}

// ============================================================================
// 28. AUDIO PLAYBACK
// ============================================================================

bool startTrackNow(int track, const String& message, bool interruptCurrent) {
  if (!dfPlayerReady || !audioEnabled) return false;
  if (track < 1 || track > 73) return false;

  if (interruptCurrent) {
    player.stop();
    delay(60);
    audioPlaying = false;
  }

  player.volume(audioVolume);
  player.playMp3Folder(track);

  lastAudioTrack = track;
  lastAudioMessage = message.length() ? message : String(trackText(track));
  lastAction = "AUDIO";
  lastActionMessage = lastAudioMessage;
  lastActionAt = millis();
  audioPlaying = true;
  audioStartedAt = millis();

  showMessage("NOW PLAYING", lastAudioMessage, MESSAGE_SCREEN_MS);

  Serial.print("[AUDIO] Playing ");
  if (track < 10) Serial.print("000");
  else if (track < 100) Serial.print("00");
  Serial.print(track);
  Serial.print(" -> ");
  Serial.println(lastAudioMessage);

  return true;
}

bool requestAudio(int track, const String& message, bool interruptCurrent, bool allowQueue) {
  if (!dfPlayerReady || !audioEnabled) return false;

  if (!audioPlaying) {
    return startTrackNow(clampTrack(track), message, false);
  }

  if (interruptCurrent) {
    return startTrackNow(clampTrack(track), message, true);
  }

  if (allowQueue) {
    return queueAudio(clampTrack(track), message);
  }

  return false;
}

void serviceAudio() {
  if (!dfPlayerReady) return;

  while (player.available()) {
    uint8_t type = player.readType();
    int value = player.read();

    if (type == DFPlayerPlayFinished) {
      Serial.print("[AUDIO] Finished track ");
      Serial.println(value);
      audioPlaying = false;

      AudioJob next;
      if (popAudio(next) && audioEnabled) {
        startTrackNow(next.track, next.message, false);
      }
    }

    else if (type == DFPlayerCardOnline) {
      Serial.println("[DFPLAYER] SD card online.");
    }

    else if (type == DFPlayerCardInserted) {
      Serial.println("[DFPLAYER] SD card inserted.");
    }

    else if (type == DFPlayerCardRemoved) {
      Serial.println("[DFPLAYER] SD card removed.");
      audioPlaying = false;
      clearAudioQueue();
    }

    else if (type == DFPlayerError) {
      Serial.print("[DFPLAYER] Error code: ");
      Serial.println(value);
      audioPlaying = false;
      clearAudioQueue();
    }
  }

  // Safety watchdog for clone modules that fail to emit PlayFinished.
  if (audioPlaying && millis() - audioStartedAt > AUDIO_WATCHDOG_MS) {
    audioPlaying = false;
    AudioJob next;
    if (popAudio(next) && audioEnabled) {
      startTrackNow(next.track, next.message, false);
    }
  }
}

void maybeAutomaticAudio() {
  if (!autoAudioEnabled || !audioEnabled || !dfPlayerReady) return;

  if (candidateConditionCount < CONDITION_PERSIST_READS) return;
  if (currentCondition == lastSpokenCondition) return;
  if (millis() - lastConditionAudioAt < CONDITION_AUDIO_COOLDOWN_MS) return;

  int track = trackForCondition(currentCondition);

  // Do not interrupt another spoken event for a condition update.
  if (audioPlaying) return;

  if (startTrackNow(track, trackText(track), false)) {
    lastSpokenCondition = currentCondition;
    lastConditionAudioAt = millis();
  }
}

// ============================================================================
// 29. BUZZER
// ============================================================================

void shortBeep() {
  tone(PIN_BUZZER, 2000, 130);
}

void doubleBeep() {
  tone(PIN_BUZZER, 2100, 100);
  delay(150);
  tone(PIN_BUZZER, 2300, 100);
}

// ============================================================================
// 30. TOUCH INTERACTION
// ============================================================================

void cycleDisplayMode() {
  if (displayMode == "AUTO") displayMode = "SENSORS";
  else if (displayMode == "SENSORS") displayMode = "HEALTH";
  else if (displayMode == "HEALTH") displayMode = "STATUS";
  else displayMode = "AUTO";

  saveSettings();
  showMessage("DISPLAY MODE", displayMode, 3000);
}

void runPlantCheck();

void processTouch() {
  const bool raw = digitalRead(PIN_TOUCH) == HIGH;
  const uint32_t now = millis();

  if (raw != rawTouch) {
    rawTouch = raw;
    touchChangedAt = now;
  }

  if (now - touchChangedAt < TOUCH_DEBOUNCE_MS) return;

  if (stableTouch != rawTouch) {
    stableTouch = rawTouch;

    if (stableTouch) {
      touchPressedAt = now;
      longPressHandled = false;
      wakeOLED();
    }
    else {
      const uint32_t duration = now - touchPressedAt;

      if (duration >= TOUCH_LONG_PRESS_MS) {
        longPressHandled = true;
        cycleDisplayMode();
        return;
      }

      if (!longPressHandled) {
        if (tapCount == 0 || now - lastTapAt > TOUCH_DOUBLE_WINDOW_MS) {
          tapCount = 1;
        } else {
          tapCount = 2;
        }
        lastTapAt = now;
      }
    }
  }

  if (tapCount == 1 && now - lastTapAt > TOUCH_DOUBLE_WINDOW_MS) {
    tapCount = 0;

    shortBeep();
    showMessage("HELLO", "Thanks for checking on me.", 4000);

    if (audioEnabled && dfPlayerReady) {
      requestAudio(61, trackText(61), true, false);
    }
  }

  if (tapCount == 2) {
    tapCount = 0;
    doubleBeep();
    runPlantCheck();
  }
}

// ============================================================================
// 31. PLANT CHECK SEQUENCE
// ============================================================================

uint8_t plantCheckStage = 0;
uint32_t plantCheckNextAt = 0;

void runPlantCheck() {
  wakeOLED();
  updateSensors();
  updatePlantModel();

  showMessage("PLANT CHECK", "I'm checking your Tulsi now.", 3500);

  if (audioEnabled && dfPlayerReady) {
    clearAudioQueue();
    // A manual plant check has priority over any currently playing audio.
    startTrackNow(54, trackText(54), true);
    queueAudio(trackForCondition(currentCondition), trackText(trackForCondition(currentCondition)));
    queueAudio(28, trackText(28));
  }

  plantCheckStage = 1;
  plantCheckNextAt = millis() + 2000UL;
}

void servicePlantCheck() {
  if (plantCheckStage == 0) return;
  if ((int32_t)(millis() - plantCheckNextAt) < 0) return;

  if (plantCheckStage == 1) {
    plantCheckStage = 2;
    showMessage("PLANT STATUS", plantStatus, 5000);
    plantCheckNextAt = millis() + 7000UL;
  }
  else {
    plantCheckStage = 0;
  }
}

// ============================================================================
// 32. OLED TEXT HELPERS
// ============================================================================

void oledHeader(const String& left, const String& right) {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(left);

  if (right.length()) {
    int width = right.length() * 6;
    display.setCursor(max(0, 127 - width), 0);
    display.print(right);
  }

  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);
}

void oledWrapped(const String& text, int x, int y, int widthChars, uint8_t maxLines) {
  int start = 0;
  uint8_t line = 0;

  while (start < (int)text.length() && line < maxLines) {
    int end = min(start + widthChars, (int)text.length());

    if (end < (int)text.length()) {
      int split = text.lastIndexOf(' ', end - 1);
      if (split > start) end = split;
    }

    String part = text.substring(start, end);
    part.trim();

    display.setTextSize(1);
    display.setCursor(x, y + line * 10);
    display.print(part);

    start = end;
    while (start < (int)text.length() && text[start] == ' ') start++;
    line++;
  }
}

// ============================================================================
// 33. OLED PAGES
// ============================================================================

void renderSmartPage() {
  oledHeader("PLANTPAL", "TULSI");

  display.setTextSize(1);
  display.setCursor(0, 14);
  display.print("STATUS:");

  display.setCursor(43, 14);
  display.print(conditionName(currentCondition));

  display.setCursor(0, 27);
  display.print("HEALTH");

  display.setTextSize(2);
  display.setCursor(68, 23);
  display.print(healthScore);
  display.print("%");

  display.setTextSize(1);
  display.setCursor(0, 40);
  display.print(healthLabel);

  display.setCursor(0, 52);

  if (sensors.soilValid) {
    display.print("Soil ");
    display.print(sensors.soilPercent);
    display.print("%  ");
  }

  if (sensors.lightValid) {
    display.print("L ");
    display.print(sensors.lux, 0);
    display.print("lx");
  }
}

void renderSensorsPage() {
  oledHeader("PLANTPAL", "SENSORS");

  display.setTextSize(1);
  display.setCursor(0, 14);
  display.print("Soil   ");
  if (sensors.soilValid) {
    display.print(sensors.soilPercent);
    display.print("% / ");
    display.print(sensors.soilRaw);
  } else display.print("ERROR");

  display.setCursor(0, 26);
  display.print("Light  ");
  if (sensors.lightValid) {
    display.print(sensors.lux, 0);
    display.print(" lux");
  } else display.print("ERROR");

  display.setCursor(0, 38);
  display.print("Temp   ");
  if (sensors.temperatureValid) {
    display.print(sensors.temperature, 1);
    display.print(" C");
  } else display.print("ERROR");

  display.setCursor(68, 38);
  display.print("Hum ");
  if (sensors.humidityValid) {
    display.print(sensors.humidity, 0);
    display.print("%");
  } else display.print("ERR");

  display.setCursor(0, 52);
  display.print("Touch  ");
  display.print(stableTouch ? "YES" : "NO");

  display.setCursor(72, 52);
  display.print(dfPlayerReady ? "AUDIO OK" : "AUDIO ERR");
}

void renderHealthPage() {
  oledHeader("PLANTPAL", "HEALTH");

  display.setTextSize(2);
  display.setCursor(35, 15);
  display.print(healthScore);
  display.print("%");

  display.setTextSize(1);
  display.setCursor(35, 35);
  display.print(healthLabel);

  display.drawRect(1, 49, 126, 10, SSD1306_WHITE);
  int bar = map(constrain(healthScore, 0, 100), 0, 100, 0, 122);
  if (bar > 1) {
    display.fillRect(3, 51, bar, 6, SSD1306_WHITE);
  }
}

void renderStatusPage() {
  oledHeader("PLANTPAL", "STATUS");

  display.setTextSize(1);
  display.setCursor(0, 14);
  display.print("Plant    ");
  display.println(PLANT_NAME);

  display.setCursor(0, 26);
  display.print("WiFi     LOCAL");

  display.setCursor(0, 38);
  display.print("IP       ");
  display.println(AP_IP_TEXT);

  display.setCursor(0, 50);
  display.print("Audio    ");
  display.print(dfPlayerReady ? (audioEnabled ? "READY" : "MUTED") : "ERROR");
}

void renderMessagePage() {
  oledHeader(messageTitle, "PLANTPAL");
  oledWrapped(messageText, 0, 17, 21, 4);
}

void renderAutoPage() {
  if (millis() - lastAutoPageChange >= AUTO_PAGE_INTERVAL_MS) {
    autoPage = (autoPage + 1) % 6;
    lastAutoPageChange = millis();
  }

  switch (autoPage) {
    case 0: renderSmartPage(); break;
    case 1: renderSensorsPage(); break;
    case 2:
      oledHeader("PLANTPAL", "SOIL");
      display.setTextSize(2);
      display.setCursor(0, 16);
      display.print(sensors.soilValid ? sensors.soilPercent : 0);
      display.print("%");
      display.setTextSize(1);
      display.setCursor(0, 40);
      display.print("State: ");
      display.println(soilState);
      display.setCursor(0, 53);
      display.print("Raw: ");
      display.println(sensors.soilRaw);
      break;

    case 3:
      oledHeader("PLANTPAL", "LIGHT");
      display.setTextSize(2);
      display.setCursor(0, 17);
      if (sensors.lightValid) {
        display.print(sensors.lux, 0);
      } else {
        display.print("ERR");
      }
      display.setTextSize(1);
      display.setCursor(0, 44);
      display.print("Lux state: ");
      display.println(lightState);
      break;

    case 4:
      oledHeader("PLANTPAL", "CLIMATE");
      display.setTextSize(1);
      display.setCursor(0, 16);
      display.print("Temp: ");
      if (sensors.temperatureValid) display.print(sensors.temperature, 1); else display.print("ERR");
      display.println(" C");
      display.setCursor(0, 30);
      display.print("Hum : ");
      if (sensors.humidityValid) display.print(sensors.humidity, 0); else display.print("ERR");
      display.println("%");
      display.setCursor(0, 45);
      display.print(temperatureState);
      display.print(" / ");
      display.println(humidityState);
      break;

    case 5:
      oledHeader("PLANTPAL", "INFO");
      display.setTextSize(1);
      display.setCursor(0, 15);
      display.println("TULSI SMART MONITOR");
      display.setCursor(0, 28);
      display.print("Health: ");
      display.print(healthScore);
      display.println("%");
      display.setCursor(0, 41);
      display.print("Mode: ");
      display.println(displayMode);
      display.setCursor(0, 54);
      display.print("Touch: ");
      display.println(stableTouch ? "YES" : "NO");
      break;
  }
}

void renderOLED() {
  if (!oledReady || !oledEnabled) return;

  if (displayMode == "SAVER") {
    if (!oledSleeping) sleepOLED();
    return;
  }

  if (!messageActive() && millis() - lastUserInteraction >= OLED_SAVER_TIMEOUT_MS) {
    sleepOLED();
    return;
  }

  if (oledSleeping) wakeOLED();

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  if (messageActive()) {
    renderMessagePage();
  } else {
    if (displayMode == "SENSORS") renderSensorsPage();
    else if (displayMode == "HEALTH") renderHealthPage();
    else if (displayMode == "STATUS") renderStatusPage();
    else renderAutoPage();
  }

  display.display();
}

// ============================================================================
// 34. INITIALIZATION HELPERS
// ============================================================================

bool initializeOLED() {
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR_A)) {
    oledReady = true;
  } else if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR_B)) {
    oledReady = true;
  } else {
    oledReady = false;
    return false;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(7, 8);
  display.println("PlantPal");
  display.setTextSize(1);
  display.setCursor(12, 34);
  display.println("TULSI SMART SYSTEM");
  display.setCursor(34, 49);
  display.println("STARTING...");
  display.display();
  delay(500);

  return true;
}

bool initializeBH1750() {
  Wire.beginTransmission(BH1750_ADDR);
  if (Wire.endTransmission() != 0) return false;

  return lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
}

void initializeSoil() {
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_SOIL, ADC_11db);
  pinMode(PIN_SOIL, INPUT);
}

void initializeTouch() {
  pinMode(PIN_TOUCH, INPUT);
  rawTouch = false;
  stableTouch = false;
  touchChangedAt = millis();
}

// ============================================================================
// 35. PHONE WEB PAGE
// ============================================================================

String buildWebPage() {
  String html;
  html.reserve(24000);

  html += R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PlantPal - Tulsi</title>
<style>
*{box-sizing:border-box}
body{margin:0;background:#0d1110;color:#f2f2f2;font-family:Arial,Helvetica,sans-serif}
.container{max-width:720px;margin:auto;padding:16px}
.header{text-align:center;padding:14px 8px 8px}
.logo{font-size:32px;font-weight:800}
.subtitle{color:#9aa5a0;margin-top:4px}
.card{background:#171c1a;border:1px solid #2a312d;border-radius:18px;padding:16px;margin-top:12px}
.hero{background:#172218;border-color:#334d36;text-align:center}
.status{font-size:24px;font-weight:800;margin:8px 0}
.muted{color:#9aa5a0;font-size:13px}
.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:10px;margin-top:12px}
.metric{background:#151a18;border:1px solid #29302d;border-radius:16px;padding:14px}
.metric .name{color:#909b95;font-size:13px}
.metric .value{font-size:28px;font-weight:800;margin-top:6px}
.metric .sub{font-size:12px;color:#88918c;margin-top:4px}
button,select,input{width:100%;border:1px solid #343c37;background:#202722;color:#fff;border-radius:12px;padding:13px;margin-top:7px;font-size:15px}
button{font-weight:700;cursor:pointer}
button:active{transform:scale(.99)}
.section-title{font-size:19px;font-weight:800;margin-bottom:7px}
.controls{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}
.controls button{margin:0}
.tracklist{max-height:520px;overflow:auto;margin-top:8px;border-radius:12px}
.track{display:flex;gap:8px;align-items:center;border-bottom:1px solid #282e2b;padding:7px 0}
.track span{font-size:12px;color:#919b96;min-width:42px}
.track button{margin:0;padding:9px;font-size:13px;text-align:left}
.footer{text-align:center;color:#66706b;font-size:12px;padding:22px 5px}
.good{color:#dff6e1}.warn{color:#fff0c2}
@media(max-width:520px){.grid{grid-template-columns:1fr 1fr}.controls{grid-template-columns:1fr}}
</style>
</head>
<body>
<div class="container">

<div class="header">
  <div class="logo">PlantPal</div>
  <div class="subtitle">Smart Tulsi Monitor · ESP32 Local IoT</div>
</div>

<div class="card hero">
  <div class="muted">CURRENT PLANT STATUS</div>
  <div class="status" id="status">Checking...</div>
  <div class="muted" id="healthLabel">Health: --</div>
</div>

<div class="grid">
  <div class="metric"><div class="name">SOIL MOISTURE</div><div class="value"><span id="soil">--</span>%</div><div class="sub">Raw: <span id="soilRaw">--</span> · <span id="soilState">--</span></div></div>
  <div class="metric"><div class="name">LIGHT</div><div class="value"><span id="lux">--</span></div><div class="sub">lux · <span id="lightState">--</span></div></div>
  <div class="metric"><div class="name">TEMPERATURE</div><div class="value"><span id="temp">--</span>°C</div><div class="sub"><span id="tempState">--</span></div></div>
  <div class="metric"><div class="name">HUMIDITY</div><div class="value"><span id="humidity">--</span>%</div><div class="sub"><span id="humState">--</span></div></div>
  <div class="metric"><div class="name">HEALTH SCORE</div><div class="value"><span id="health">--</span>%</div><div class="sub"><span id="condition">--</span></div></div>
  <div class="metric"><div class="name">SYSTEM</div><div class="value" id="audioState">--</div><div class="sub">Touch: <span id="touch">--</span></div></div>
</div>

<div class="card">
  <div class="section-title">Plant Actions</div>
  <div class="controls">
    <button onclick="act('check')">Check Plant</button>
    <button onclick="act('hello')">Say Hello</button>
    <button onclick="act('watered')">I Watered It</button>
    <button onclick="act('buzz')">Test Buzzer</button>
    <button onclick="act('sensor')">Sensor Check</button>
    <button onclick="act('stop')">Stop Audio</button>
  </div>
</div>

<div class="card">
  <div class="section-title">OLED</div>
  <div class="controls">
    <button onclick="act('oled_on')">OLED ON</button>
    <button onclick="act('oled_off')">OLED OFF</button>
  </div>
  <select id="modeSelect" onchange="setMode(this.value)">
    <option value="AUTO">AUTO</option>
    <option value="SENSORS">SENSORS</option>
    <option value="HEALTH">HEALTH</option>
    <option value="STATUS">STATUS</option>
  </select>
</div>

<div class="card">
  <div class="section-title">Audio Settings</div>
  <div class="muted">Volume 0–30</div>
  <input id="volume" type="range" min="0" max="30" value="20" oninput="setVolume(this.value)">
  <div class="controls">
    <button onclick="act('audio_on')">Audio ON</button>
    <button onclick="act('audio_off')">Audio OFF</button>
    <button onclick="toggleAutoAudio()" id="autoAudioBtn">Auto Audio</button>
    <button onclick="act('test_audio')">Speaker Test</button>
  </div>
</div>

<div class="card">
  <div class="section-title">Soil Calibration</div>
  <div class="muted">Use the actual final Tulsi pot. First expose the sensor to very dry soil, then to well-moistened soil.</div>
  <button onclick="act('cal_dry')">Capture CURRENT as DRY</button>
  <button onclick="act('cal_wet')">Capture CURRENT as WET</button>
  <button onclick="act('cal_reset')">Reset Calibration</button>
  <div class="muted" style="margin-top:8px">Dry raw: <span id="dryCal">--</span> · Wet raw: <span id="wetCal">--</span></div>
</div>

<div class="card">
  <div class="section-title">All 73 PlantPal Voice Files</div>
  <div class="muted">Tap any track to test the final SD-card voice library.</div>
  <div class="tracklist">
)rawliteral";

  for (int i = 1; i <= 73; ++i) {
    String num;
    if (i < 10) num = "000" + String(i);
    else if (i < 100) num = "00" + String(i);
    else num = String(i);

    html += "<div class='track'><span>";
    html += num;
    html += "</span><button onclick='play(";
    html += String(i);
    html += ")'>";
    html += htmlEscape(trackText(i));
    html += "</button></div>";
  }

  html += R"rawliteral(
  </div>
</div>

<div class="footer">
PlantPal · Tulsi Smart Plant System<br>
Local ESP32 IoT · http://192.168.4.1
</div>

<script>
let currentData={};
function getData(){
  fetch('/api/data').then(r=>r.json()).then(d=>{
    currentData=d;
    document.getElementById('status').innerText=d.status;
    document.getElementById('healthLabel').innerText='Health: '+d.healthScore+'% · '+d.healthLabel;
    document.getElementById('soil').innerText=d.soilPercent;
    document.getElementById('soilRaw').innerText=d.soilRaw;
    document.getElementById('soilState').innerText=d.soilState;
    document.getElementById('lux').innerText=d.lux.toFixed(0);
    document.getElementById('lightState').innerText=d.lightState;
    document.getElementById('temp').innerText=d.temperature.toFixed(1);
    document.getElementById('tempState').innerText=d.temperatureState;
    document.getElementById('humidity').innerText=d.humidity.toFixed(0);
    document.getElementById('humState').innerText=d.humidityState;
    document.getElementById('health').innerText=d.healthScore;
    document.getElementById('condition').innerText=d.condition;
    document.getElementById('audioState').innerText=d.audioReady?(d.audioEnabled?'AUDIO OK':'MUTED'):'ERROR';
    document.getElementById('touch').innerText=d.touch?'YES':'NO';
    document.getElementById('volume').value=d.volume;
    document.getElementById('dryCal').innerText=d.soilDry;
    document.getElementById('wetCal').innerText=d.soilWet;
    document.getElementById('modeSelect').value=d.displayMode;
    document.getElementById('autoAudioBtn').innerText=d.autoAudio?'Auto Audio: ON':'Auto Audio: OFF';
  }).catch(()=>{});
}
function act(c){ fetch('/api/action?cmd='+encodeURIComponent(c)).then(()=>getData()); }
function play(n){ fetch('/api/play?id='+n); }
function setMode(v){ fetch('/api/action?cmd=mode&value='+encodeURIComponent(v)); }
function setVolume(v){ fetch('/api/action?cmd=volume&value='+v); }
function toggleAutoAudio(){ fetch('/api/action?cmd=auto_audio&value='+(currentData.autoAudio?'off':'on')); }
getData();
setInterval(getData,2000);
</script>
</body>
</html>
)rawliteral";

  return html;
}

// ============================================================================
// 36. WEB JSON
// ============================================================================

String buildDataJson() {
  String json;
  json.reserve(2600);

  json += "{";
  json += "\"plant\":\"" + jsonEscape(String(PLANT_NAME)) + "\",";
  json += "\"species\":\"" + jsonEscape(String(PLANT_SPECIES)) + "\",";
  json += "\"soilRaw\":" + String(sensors.soilRaw) + ",";
  json += "\"soilPercent\":" + String(sensors.soilPercent) + ",";
  json += "\"lux\":" + String(sensors.lightValid ? sensors.lux : 0.0f, 1) + ",";
  json += "\"temperature\":" + String(sensors.temperatureValid ? sensors.temperature : 0.0f, 1) + ",";
  json += "\"humidity\":" + String(sensors.humidityValid ? sensors.humidity : 0.0f, 1) + ",";
  json += "\"touch\":" + String(stableTouch ? "true" : "false") + ",";
  json += "\"audioReady\":" + String(dfPlayerReady ? "true" : "false") + ",";
  json += "\"audioEnabled\":" + String(audioEnabled ? "true" : "false") + ",";
  json += "\"autoAudio\":" + String(autoAudioEnabled ? "true" : "false") + ",";
  json += "\"volume\":" + String(audioVolume) + ",";
  json += "\"displayMode\":\"" + jsonEscape(displayMode) + "\",";
  json += "\"healthScore\":" + String(healthScore) + ",";
  json += "\"healthLabel\":\"" + jsonEscape(healthLabel) + "\",";
  json += "\"condition\":\"" + jsonEscape(conditionName(currentCondition)) + "\",";
  json += "\"soilState\":\"" + jsonEscape(soilState) + "\",";
  json += "\"lightState\":\"" + jsonEscape(lightState) + "\",";
  json += "\"temperatureState\":\"" + jsonEscape(temperatureState) + "\",";
  json += "\"humidityState\":\"" + jsonEscape(humidityState) + "\",";
  json += "\"status\":\"" + jsonEscape(plantStatus) + "\",";
  json += "\"lastAudioTrack\":" + String(lastAudioTrack) + ",";
  json += "\"lastAudioMessage\":\"" + jsonEscape(lastAudioMessage) + "\",";
  json += "\"soilDry\":" + String(soilDryCalibration) + ",";
  json += "\"soilWet\":" + String(soilWetCalibration) + ",";
  json += "\"uptime\":" + String(millis() / 1000UL) + ",";
  json += "\"cloudConnected\":" + String(cloudConnected ? "true" : "false") + ",";
  json += "\"wifiRssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) + ",";
  json += "\"lastAction\":\"" + jsonEscape(lastAction) + "\",";
  json += "\"lastActionMessage\":\"" + jsonEscape(lastActionMessage) + "\"";
  json += "}";

  return json;
}

// ============================================================================
// 37. WEB ROUTES
// ============================================================================

void handleRoot() {
  server.send(200, "text/html", buildWebPage());
}

void handleData() {
  server.send(200, "application/json", buildDataJson());
}

void handlePlay() {
  if (!server.hasArg("id")) {
    server.send(400, "text/plain", "Missing id");
    return;
  }

  int track = server.arg("id").toInt();

  if (track < 1 || track > 73) {
    server.send(400, "text/plain", "Track must be 1-73");
    return;
  }

  if (!dfPlayerReady || !audioEnabled) {
    server.send(503, "text/plain", "Audio unavailable or disabled");
    return;
  }

  if (requestAudio(track, trackText(track), true, false)) {
    server.send(200, "text/plain", "Playing");
  } else {
    server.send(500, "text/plain", "Playback failed");
  }
}

void handleAction() {
  if (!server.hasArg("cmd")) {
    server.send(400, "text/plain", "Missing command");
    return;
  }

  String cmd = server.arg("cmd");
  cmd.toLowerCase();

  String value = server.hasArg("value") ? server.arg("value") : "";
  value.toUpperCase();

  bool success = true;
  String response = "OK";

  if (cmd == "check") {
    runPlantCheck();
    response = "Plant check started";
  }

  else if (cmd == "hello") {
    shortBeep();
    requestAudio(61, trackText(61), true, false);
    showMessage("HELLO", "Hello there!", 4000);
    response = "Hello played";
  }

  else if (cmd == "watered") {
    showMessage("WATERING LOGGED", "Keep watching the soil moisture.", 4500);
    if (audioEnabled && dfPlayerReady) requestAudio(39, trackText(39), true, false);
    response = "Watering logged";
  }

  else if (cmd == "buzz") {
    shortBeep();
    response = "Buzzer triggered";
  }

  else if (cmd == "sensor") {
    if (audioEnabled && dfPlayerReady) {
      requestAudio(29, trackText(29), true, false);
    }
    showMessage("SENSOR CHECK", "Sensor values are being updated.", 3500);
    updateSensors();
    updatePlantModel();
    response = "Sensor check complete";
  }

  else if (cmd == "stop") {
    if (dfPlayerReady) player.stop();
    audioPlaying = false;
    clearAudioQueue();
    showMessage("AUDIO STOPPED", "Playback stopped.", 3000);
    response = "Audio stopped";
  }

  else if (cmd == "oled_on") {
    oledEnabled = true;
    wakeOLED();
    saveSettings();
    response = "OLED enabled";
  }

  else if (cmd == "oled_off") {
    oledEnabled = false;
    saveSettings();
    sleepOLED();
    response = "OLED disabled";
  }

  else if (cmd == "mode") {
    if (value == "AUTO" || value == "SENSORS" || value == "HEALTH" || value == "STATUS") {
      displayMode = value;
      oledEnabled = true;
      wakeOLED();
      saveSettings();
      response = "Display mode set to " + displayMode;
    } else {
      success = false;
      response = "Invalid display mode";
    }
  }

  else if (cmd == "audio_on") {
    audioEnabled = true;
    saveSettings();
    if (dfPlayerReady) requestAudio(65, trackText(65), true, false);
    response = "Audio enabled";
  }

  else if (cmd == "audio_off") {
    audioEnabled = false;
    if (dfPlayerReady) player.stop();
    audioPlaying = false;
    clearAudioQueue();
    saveSettings();
    showMessage("QUIET", "I'll stay quiet for now.", 3500);
    response = "Audio disabled";
  }

  else if (cmd == "auto_audio") {
    if (value == "ON") autoAudioEnabled = true;
    else if (value == "OFF") autoAudioEnabled = false;
    else success = false;
    saveSettings();
    response = autoAudioEnabled ? "Auto audio ON" : "Auto audio OFF";
  }

  else if (cmd == "test_audio") {
    if (audioEnabled && dfPlayerReady) {
      requestAudio(63, trackText(63), true, false);
      response = "Speaker test started";
    } else {
      success = false;
      response = "Audio unavailable";
    }
  }

  else if (cmd == "volume") {
    int v = constrain(server.hasArg("value") ? server.arg("value").toInt() : 20, 0, 30);
    audioVolume = v;
    if (dfPlayerReady) player.volume(audioVolume);
    saveSettings();
    response = "Volume set to " + String(audioVolume);
  }

  else if (cmd == "cal_dry") {
    int valueNow = readSoilRawAveraged();
    if (valueNow > soilWetCalibration) {
      soilDryCalibration = valueNow;
      saveSettings();
      updateSensors();
      updatePlantModel();
      response = "Dry calibration stored: " + String(valueNow);
    } else {
      success = false;
      response = "Rejected: dry reading must be greater than wet reading";
    }
  }

  else if (cmd == "cal_wet") {
    int valueNow = readSoilRawAveraged();
    if (valueNow < soilDryCalibration) {
      soilWetCalibration = valueNow;
      saveSettings();
      updateSensors();
      updatePlantModel();
      response = "Wet calibration stored: " + String(valueNow);
    } else {
      success = false;
      response = "Rejected: wet reading must be lower than dry reading";
    }
  }

  else if (cmd == "cal_reset") {
    soilDryCalibration = DEFAULT_SOIL_DRY_RAW;
    soilWetCalibration = DEFAULT_SOIL_WET_RAW;
    saveSettings();
    updateSensors();
    updatePlantModel();
    response = "Calibration reset";
  }

  else {
    success = false;
    response = "Unknown command";
  }

  server.send(success ? 200 : 400, "text/plain", response);
}

// ============================================================================
// 38. I2C SCANNER FOR SERIAL DIAGNOSTICS
// ============================================================================

void scanI2C() {
  Serial.println("[I2C] Scan started");
  uint8_t count = 0;

  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    uint8_t error = Wire.endTransmission();

    if (error == 0) {
      Serial.print("[I2C] Found 0x");
      if (address < 16) Serial.print('0');
      Serial.println(address, HEX);
      count++;
    }
  }

  Serial.print("[I2C] Devices found: ");
  Serial.println(count);
}

// ============================================================================
// 39. SERIAL DIAGNOSTICS
// ============================================================================

void printStatus() {
  Serial.println();
  Serial.println("================ PLANTPAL STATUS ================");
  Serial.print("Firmware       : "); Serial.println(FW_VERSION);
  Serial.print("Plant          : "); Serial.println(PLANT_NAME);
  Serial.print("Species        : "); Serial.println(PLANT_SPECIES);
  Serial.print("OLED           : "); Serial.println(oledReady && oledEnabled ? "ON" : "OFF/ERROR");
  Serial.print("BH1750         : "); Serial.println(bh1750Ready ? "READY" : "ERROR");
  Serial.print("DFPlayer       : "); Serial.println(dfPlayerReady ? "READY" : "ERROR");
  Serial.print("Audio          : "); Serial.println(audioEnabled ? "ON" : "OFF");
  Serial.print("Auto audio     : "); Serial.println(autoAudioEnabled ? "ON" : "OFF");
  Serial.print("Volume         : "); Serial.println(audioVolume);
  Serial.print("Display mode   : "); Serial.println(displayMode);
  Serial.print("Soil           : "); Serial.print(sensors.soilPercent); Serial.print("% / RAW "); Serial.println(sensors.soilRaw);
  Serial.print("Light          : "); Serial.print(sensors.lux, 1); Serial.println(" lux");
  Serial.print("Temperature    : "); Serial.print(sensors.temperature, 1); Serial.println(" C");
  Serial.print("Humidity       : "); Serial.print(sensors.humidity, 1); Serial.println(" %");
  Serial.print("Condition      : "); Serial.println(conditionName(currentCondition));
  Serial.print("Health         : "); Serial.print(healthScore); Serial.print("% / "); Serial.println(healthLabel);
  Serial.print("Status         : "); Serial.println(plantStatus);
  Serial.print("Last audio     : "); Serial.print(lastAudioTrack); Serial.print(" / "); Serial.println(lastAudioMessage);
  Serial.print("WiFi AP        : "); Serial.println(AP_SSID);
  Serial.print("WiFi STA       : "); Serial.println(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "OFFLINE");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi STA IP    : "); Serial.println(WiFi.localIP());
    Serial.print("WiFi RSSI      : "); Serial.println(WiFi.RSSI());
  }
  Serial.print("Cloud          : "); Serial.println(cloudConnected ? "CONNECTED" : "OFFLINE");
  Serial.print("IP             : "); Serial.println(AP_IP_TEXT);
  Serial.print("Uptime         : "); Serial.print(millis() / 1000UL); Serial.println(" s");
  Serial.println("=================================================");
}

void printSensors() {
  Serial.println();
  Serial.println("---------------- SENSOR DATA ----------------");
  Serial.print("Soil RAW       : "); Serial.println(sensors.soilRaw);
  Serial.print("Soil %         : "); Serial.println(sensors.soilPercent);
  Serial.print("Soil state     : "); Serial.println(soilState);
  Serial.print("Light          : "); Serial.println(sensors.lux, 1);
  Serial.print("Light state    : "); Serial.println(lightState);
  Serial.print("Temperature    : "); Serial.println(sensors.temperature, 1);
  Serial.print("Temp state     : "); Serial.println(temperatureState);
  Serial.print("Humidity       : "); Serial.println(sensors.humidity, 1);
  Serial.print("Humidity state : "); Serial.println(humidityState);
  Serial.print("Touch          : "); Serial.println(stableTouch ? "YES" : "NO");
  Serial.print("Condition      : "); Serial.println(conditionName(currentCondition));
  Serial.println("----------------------------------------------");
}

void printHelp() {
  Serial.println();
  Serial.println("================ PLANTPAL COMMANDS ================");
  Serial.println("help                  Show commands");
  Serial.println("status                Full system status");
  Serial.println("sensors               Current sensor data");
  Serial.println("i2c                   Scan I2C bus");
  Serial.println("check                 Run plant check");
  Serial.println("audio N               Play track 1-73");
  Serial.println("stop                  Stop audio");
  Serial.println("audio info            DFPlayer diagnostic");
  Serial.println("audio on              Enable audio");
  Serial.println("audio off             Disable audio");
  Serial.println("auto audio on         Enable condition audio");
  Serial.println("auto audio off        Disable condition audio");
  Serial.println("test audio            Play speaker test");
  Serial.println("volume N              Volume 0-30");
  Serial.println("oled on               Enable OLED");
  Serial.println("oled off              Disable OLED");
  Serial.println("mode auto             OLED auto pages");
  Serial.println("mode sensors          OLED sensors page");
  Serial.println("mode health           OLED health page");
  Serial.println("mode status           OLED status page");
  Serial.println("cal dry               Capture current as dry");
  Serial.println("cal wet               Capture current as wet");
  Serial.println("cal reset             Reset soil calibration");
  Serial.println("watered               Log watering event");
  Serial.println("===================================================");
}

void handleSerialCommand(String command) {
  command.trim();
  if (!command.length()) return;

  String upper = command;
  upper.toUpperCase();

  if (upper == "HELP") {
    printHelp();
    return;
  }

  if (upper == "STATUS") {
    printStatus();
    return;
  }

  if (upper == "SENSORS") {
    printSensors();
    return;
  }

  if (upper == "I2C") {
    scanI2C();
    return;
  }

  if (upper == "CHECK") {
    runPlantCheck();
    return;
  }

  if (upper == "STOP") {
    if (dfPlayerReady) player.stop();
    audioPlaying = false;
    clearAudioQueue();
    return;
  }

  if (upper == "AUDIO INFO") {
    Serial.println("[DFPLAYER] INFO");
    Serial.print("Ready      : "); Serial.println(dfPlayerReady ? "YES" : "NO");
    Serial.print("Volume     : "); Serial.println(audioVolume);
    Serial.print("Audio      : "); Serial.println(audioEnabled ? "ON" : "OFF");
    if (dfPlayerReady) {
      Serial.print("SD files   : "); Serial.println(player.readFileCounts());
      Serial.print("Current    : "); Serial.println(player.readCurrentFileNumber());
      Serial.print("State      : "); Serial.println(player.readState());
    }
    return;
  }

  if (upper == "AUDIO ON") {
    audioEnabled = true;
    saveSettings();
    if (dfPlayerReady) requestAudio(65, trackText(65), true, false);
    return;
  }

  if (upper == "AUDIO OFF") {
    audioEnabled = false;
    if (dfPlayerReady) player.stop();
    audioPlaying = false;
    clearAudioQueue();
    saveSettings();
    return;
  }

  if (upper == "AUTO AUDIO ON") {
    autoAudioEnabled = true;
    saveSettings();
    return;
  }

  if (upper == "AUTO AUDIO OFF") {
    autoAudioEnabled = false;
    saveSettings();
    return;
  }

  if (upper == "TEST AUDIO") {
    if (dfPlayerReady && audioEnabled) requestAudio(63, trackText(63), true, false);
    return;
  }

  if (upper.startsWith("AUDIO ")) {
    int track = command.substring(6).toInt();
    if (track >= 1 && track <= 73) requestAudio(track, trackText(track), true, false);
    return;
  }

  if (upper.startsWith("VOLUME ")) {
    int volume = constrain(command.substring(7).toInt(), 0, 30);
    audioVolume = volume;
    if (dfPlayerReady) player.volume(audioVolume);
    saveSettings();
    return;
  }

  if (upper == "OLED ON") {
    oledEnabled = true;
    wakeOLED();
    saveSettings();
    return;
  }

  if (upper == "OLED OFF") {
    oledEnabled = false;
    saveSettings();
    sleepOLED();
    return;
  }

  if (upper.startsWith("MODE ")) {
    String mode = command.substring(5);
    mode.toUpperCase();
    if (mode == "AUTO" || mode == "SENSORS" || mode == "HEALTH" || mode == "STATUS") {
      displayMode = mode;
      oledEnabled = true;
      wakeOLED();
      saveSettings();
    }
    return;
  }

  if (upper == "CAL DRY") {
    int valueNow = readSoilRawAveraged();
    if (valueNow > soilWetCalibration) {
      soilDryCalibration = valueNow;
      saveSettings();
    }
    updateSensors();
    updatePlantModel();
    return;
  }

  if (upper == "CAL WET") {
    int valueNow = readSoilRawAveraged();
    if (valueNow < soilDryCalibration) {
      soilWetCalibration = valueNow;
      saveSettings();
    }
    updateSensors();
    updatePlantModel();
    return;
  }

  if (upper == "CAL RESET") {
    soilDryCalibration = DEFAULT_SOIL_DRY_RAW;
    soilWetCalibration = DEFAULT_SOIL_WET_RAW;
    saveSettings();
    updateSensors();
    updatePlantModel();
    return;
  }

  if (upper == "WATERED") {
    showMessage("WATERING LOGGED", "Keep watching the soil moisture.", 4500);
    if (audioEnabled && dfPlayerReady) requestAudio(39, trackText(39), true, false);
    return;
  }

  Serial.println("[SERIAL] Unknown command. Type help.");
}

void serviceSerial() {
  static String buffer;

  while (Serial.available()) {
    char c = (char)Serial.read();

    if (c == '\n' || c == '\r') {
      if (buffer.length()) {
        handleSerialCommand(buffer);
        buffer = "";
      }
    } else if (buffer.length() < 120) {
      buffer += c;
    }
  }
}

// ============================================================================

// ============================================================================
// 39A. CLOUD / RENDER IOT LAYER
// ============================================================================

String buildCloudSensorJson() {
  String json;
  json.reserve(2200);

  json += "{";
  json += "\"deviceId\":\"" + jsonEscape(String(DEVICE_ID)) + "\",";
  json += "\"firmware\":\"" + jsonEscape(String(FW_VERSION)) + "\",";
  json += "\"plant\":\"" + jsonEscape(String(PLANT_NAME)) + "\",";
  json += "\"species\":\"" + jsonEscape(String(PLANT_SPECIES)) + "\",";
  json += "\"soilRaw\":" + String(sensors.soilRaw) + ",";
  json += "\"soilPercent\":" + String(sensors.soilPercent) + ",";
  json += "\"lux\":" + String(sensors.lightValid ? sensors.lux : 0.0f, 1) + ",";
  json += "\"temperature\":" + String(sensors.temperatureValid ? sensors.temperature : 0.0f, 1) + ",";
  json += "\"humidity\":" + String(sensors.humidityValid ? sensors.humidity : 0.0f, 1) + ",";
  json += "\"touch\":" + String(stableTouch ? "true" : "false") + ",";
  json += "\"audio\":" + String(dfPlayerReady ? "true" : "false") + ",";
  json += "\"audioEnabled\":" + String(audioEnabled ? "true" : "false") + ",";
  json += "\"oledEnabled\":" + String(oledEnabled ? "true" : "false") + ",";
  json += "\"displayMode\":\"" + jsonEscape(displayMode) + "\",";
  json += "\"volume\":" + String(audioVolume) + ",";
  json += "\"healthScore\":" + String(healthScore) + ",";
  json += "\"healthLabel\":\"" + jsonEscape(healthLabel) + "\",";
  json += "\"soilState\":\"" + jsonEscape(soilState) + "\",";
  json += "\"lightState\":\"" + jsonEscape(lightState) + "\",";
  json += "\"temperatureState\":\"" + jsonEscape(temperatureState) + "\",";
  json += "\"humidityState\":\"" + jsonEscape(humidityState) + "\",";
  json += "\"lastAudioTrack\":" + String(lastAudioTrack) + ",";
  json += "\"lastAudioMessage\":\"" + jsonEscape(lastAudioMessage) + "\",";
  json += "\"lastAction\":\"" + jsonEscape(lastAction) + "\",";
  json += "\"lastActionMessage\":\"" + jsonEscape(lastActionMessage) + "\",";
  json += "\"wifiRssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  json += "}";

  return json;
}

String buildCloudEventJson(const String& kind, int track,
                           const String& message, const String& icon) {
  String json;
  json.reserve(700);
  json += "{";
  json += "\"deviceId\":\"" + jsonEscape(String(DEVICE_ID)) + "\",";
  json += "\"kind\":\"" + jsonEscape(kind) + "\",";
  json += "\"track\":" + String(track) + ",";
  json += "\"message\":\"" + jsonEscape(message) + "\",";
  json += "\"icon\":\"" + jsonEscape(icon) + "\"";
  json += "}";
  return json;
}

void startLocalAPFallback() {
  if (localAPActive) return;

  WiFi.mode(WIFI_AP_STA);
  bool ok = WiFi.softAP(AP_SSID, AP_PASSWORD, 1, 0, 4);
  localAPActive = ok;

  Serial.print("[WEB] Local AP fallback: ");
  Serial.println(ok ? "STARTED" : "FAILED");
  if (ok) {
    Serial.print("[WEB] Local IP: ");
    Serial.println(WiFi.softAPIP());
  }
}

bool cloudRequest(const String& method, const String& path, const String& body,
                  int& httpCode, String& response) {
  httpCode = -1;
  response = "";

  if (!cloudEnabled || !PLANTPAL_HAS_CONFIG) return false;
  if (WiFi.status() != WL_CONNECTED) return false;
  if (String(SERVER_BASE_URL).length() == 0) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(CLOUD_CONNECT_TIMEOUT_MS);
  http.setTimeout(CLOUD_HTTP_TIMEOUT_MS);
  // Render/proxy responses are short; HTTP/1.0 + close avoids persistent/chunked
  // response handling issues on embedded clients.
  http.useHTTP10(true);
  http.setReuse(false);

  String url = String(SERVER_BASE_URL) + path;

  if (!http.begin(client, url)) {
    Serial.print("[CLOUD] HTTP begin failed: ");
    Serial.println(url);
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-PlantPal-Device", String(DEVICE_ID));
  http.addHeader("Connection", "close");

  if (method == "POST") {
    httpCode = http.POST(body);
  } else {
    httpCode = http.GET();
  }

  // Only command polling needs the response body. POST endpoints only need
  // the HTTP result code, avoiding unnecessary response-body waits.
  if (httpCode > 0 && method != "POST") {
    response = http.getString();
  }

  if (httpCode < 0) {
    Serial.print("[CLOUD] ");
    Serial.print(method);
    Serial.print(" ");
    Serial.print(path);
    Serial.print(" -> ");
    Serial.println(http.errorToString(httpCode));
  }

  http.end();
  return httpCode >= 200 && httpCode < 300;
}

void cloudConnect() {
  if (!cloudEnabled || !PLANTPAL_HAS_CONFIG) {
    cloudConnected = false;
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    cloudConnected = true;
    wifiAttemptActive = false;
    return;
  }

  // Use STA alone while attempting the Internet connection. This avoids the
  // ESP32 radio sharing a second channel with the fallback SoftAP.
  if (!wifiAttemptActive) {
    if (localAPActive) {
      WiFi.softAPdisconnect(true);
      localAPActive = false;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(false);

    Serial.print("[WIFI] Connecting to: ");
    Serial.println(WIFI_SSID);
    Serial.print("[WIFI] Current channel/band must be 2.4 GHz for ESP32.");
    Serial.println();

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    wifiAttemptActive = true;
    wifiAttemptStartedAt = millis();
    return;
  }

  if (millis() - wifiAttemptStartedAt >= WIFI_CONNECT_TIMEOUT_MS) {
    wifiAttemptActive = false;
    WiFi.disconnect(false, false);
    cloudConnected = false;
    Serial.print("[WIFI] Connection attempt timed out. status=");
    Serial.println((int)WiFi.status());
    startLocalAPFallback();
    lastCloudRetryAt = millis();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiAttemptActive = false;
    cloudConnected = true;

    Serial.print("[WIFI] STA connected. IP: ");
    Serial.println(WiFi.localIP());

    Serial.print("[WIFI] RSSI: ");
    Serial.println(WiFi.RSSI());

    Serial.print("[CLOUD] Server: ");
    Serial.println(SERVER_BASE_URL);

    configTime(19800, 0, "pool.ntp.org", "time.nist.gov");
    lastCloudRetryAt = millis();
  }
}

bool sendCloudTelemetry() {
  if (!cloudEnabled || WiFi.status() != WL_CONNECTED) {
    cloudConnected = false;
    return false;
  }

  int code = -1;
  String response;

  bool ok = cloudRequest(
    "POST",
    "/api/sensor-data",
    buildCloudSensorJson(),
    code,
    response
  );

  if (ok) {
    cloudConnected = true;
    Serial.print("[CLOUD] Telemetry uploaded. HTTP ");
    Serial.println(code);
    return true;
  }

  cloudConnected = false;
  Serial.print("[CLOUD] Telemetry failed. HTTP ");
  Serial.println(code);
  return false;
}

bool sendCloudEvent(const String& kind, int track,
                    const String& message, const String& icon) {
  if (!cloudEnabled || WiFi.status() != WL_CONNECTED) return false;

  if (millis() - lastCloudEventAt < CLOUD_EVENT_MIN_INTERVAL_MS) {
    return false;
  }

  int code = -1;
  String response;

  bool ok = cloudRequest(
    "POST",
    "/api/events",
    buildCloudEventJson(kind, track, message, icon),
    code,
    response
  );

  if (ok) {
    lastCloudEventAt = millis();
    return true;
  }

  return false;
}

String jsonFieldString(const String& json, const String& key) {
  String needle = "\"" + key + "\":\"";
  int start = json.indexOf(needle);

  if (start < 0) return "";

  start += needle.length();

  String value;
  bool escaped = false;

  for (int i = start; i < json.length(); ++i) {
    char c = json[i];

    if (escaped) {
      if (c == 'n') value += '\n';
      else if (c == 'r') value += '\r';
      else value += c;
      escaped = false;
      continue;
    }

    if (c == '\\') {
      escaped = true;
      continue;
    }

    if (c == '"') break;

    value += c;
  }

  return value;
}

long jsonFieldLong(const String& json, const String& key, long fallback) {
  String needle = "\"" + key + "\":";
  int start = json.indexOf(needle);

  if (start < 0) return fallback;

  start += needle.length();

  while (start < json.length() &&
         (json[start] == ' ' || json[start] == '\t')) {
    start++;
  }

  int end = start;

  if (end < json.length() && json[end] == '-') end++;

  while (end < json.length() && isDigit(json[end])) end++;

  if (end == start) return fallback;

  return json.substring(start, end).toInt();
}

bool acknowledgeCloudCommand(long commandId,
                             bool success,
                             const String& result) {
  if (!cloudEnabled || WiFi.status() != WL_CONNECTED || commandId <= 0) {
    return false;
  }

  String body = "{";
  body += "\"deviceId\":\"" + jsonEscape(String(DEVICE_ID)) + "\",";
  body += "\"success\":" + String(success ? "true" : "false") + ",";
  body += "\"result\":\"" + jsonEscape(result) + "\"";
  body += "}";

  int code = -1;
  String response;
  String path = "/api/commands/" + String(commandId) + "/ack";

  bool ok = cloudRequest("POST", path, body, code, response);

  Serial.print("[CLOUD] Command ack #");
  Serial.print(commandId);
  Serial.print(": ");
  Serial.println(ok ? "OK" : "FAILED");

  return ok;
}

bool executeCloudCommand(const String& commandIn,
                         const String& value,
                         String& result) {
  String command = commandIn;
  command.toUpperCase();

  result = "Unknown command.";

  if (command == "PING") {
    lastAction = "PING";
    lastActionMessage = "PlantPal is online.";
    showMessage("PING", "PlantPal is online.", 2500);
    sendCloudEvent("wifi", 0, "PlantPal is online.", "wifi");
    result = "PlantPal is online.";
    return true;
  }

  if (command == "PLAY_AUDIO") {
    int track = value.toInt();

    if (track < 1 || track > 73) {
      result = "Track must be 1-73.";
      return false;
    }

    bool ok = requestAudio(track, trackText(track), true, false);

    if (ok) sendCloudEvent("audio", track, trackText(track), "audio");

    lastAction = "REMOTE_AUDIO";
    lastActionMessage = ok ? String("Playing track ") + track : "Audio unavailable.";
    result = lastActionMessage;
    return ok;
  }

  if (command == "STOP_AUDIO") {
    if (dfPlayerReady) player.stop();

    audioPlaying = false;
    clearAudioQueue();

    lastAction = "STOP_AUDIO";
    lastActionMessage = "Playback stopped.";
    showMessage("AUDIO STOPPED", "Playback stopped.", 2500);

    result = "Audio stopped.";
    return true;
  }

  if (command == "CHECK_PLANT") {
    runPlantCheck();

    lastAction = "CHECK_PLANT";
    lastActionMessage = "Remote plant check started.";
    result = "Plant check started.";
    return true;
  }

  if (command == "TIME_GREETING") {
    int track = 27;

    time_t nowTime = time(nullptr);

    if (nowTime > 100000) {
      struct tm timeinfo;

      if (getLocalTime(&timeinfo, 1000)) {
        if (timeinfo.tm_hour >= 5 && timeinfo.tm_hour < 12) {
          track = 25;
        } else if (timeinfo.tm_hour >= 12 && timeinfo.tm_hour < 17) {
          track = 26;
        } else if (timeinfo.tm_hour >= 17 && timeinfo.tm_hour < 22) {
          track = 27;
        } else {
          track = 67;
        }
      }
    }

    bool ok = requestAudio(track, trackText(track), true, false);

    result = ok
      ? String("Time greeting played: ") + track
      : "Audio unavailable.";

    lastAction = "TIME_GREETING";
    lastActionMessage = result;

    return ok;
  }

  if (command == "OLED_ON") {
    oledEnabled = true;
    oledSleeping = false;
    wakeOLED();
    saveSettings();

    lastAction = "OLED_ON";
    lastActionMessage = "OLED enabled.";
    result = "OLED enabled.";
    return true;
  }

  if (command == "OLED_OFF") {
    oledEnabled = false;
    saveSettings();
    sleepOLED();

    lastAction = "OLED_OFF";
    lastActionMessage = "OLED disabled.";
    result = "OLED disabled.";
    return true;
  }

  if (command == "AUDIO_ON") {
    audioEnabled = true;
    saveSettings();

    if (dfPlayerReady) requestAudio(65, trackText(65), true, false);

    lastAction = "AUDIO_ON";
    lastActionMessage = "Audio enabled.";
    result = "Audio enabled.";
    return true;
  }

  if (command == "AUDIO_OFF") {
    audioEnabled = false;

    if (dfPlayerReady) player.stop();

    audioPlaying = false;
    clearAudioQueue();
    saveSettings();

    lastAction = "AUDIO_OFF";
    lastActionMessage = "Audio disabled.";
    showMessage("QUIET", "I'll stay quiet for now.", 3000);

    result = "Audio disabled.";
    return true;
  }

  if (command == "SILENT_MODE") {
    audioEnabled = false;
    autoAudioEnabled = false;

    if (dfPlayerReady) player.stop();

    audioPlaying = false;
    clearAudioQueue();
    saveSettings();

    showMessage("QUIET MODE", "PlantPal will stay quiet.", 3000);

    lastAction = "SILENT_MODE";
    lastActionMessage = "Quiet mode enabled.";
    result = "Quiet mode enabled.";
    return true;
  }

  if (command == "DISPLAY_MODE") {
    String mode = value;
    mode.toUpperCase();

    if (mode != "AUTO" &&
        mode != "SENSORS" &&
        mode != "HEALTH" &&
        mode != "STATUS") {
      result = "Invalid display mode.";
      return false;
    }

    displayMode = mode;
    oledEnabled = true;
    oledSleeping = false;
    wakeOLED();
    saveSettings();

    lastAction = "DISPLAY_MODE";
    lastActionMessage = String("Display mode set to ") + mode;
    result = lastActionMessage;
    return true;
  }

  if (command == "VOLUME") {
    int v = constrain(value.toInt(), 0, 30);
    audioVolume = v;

    if (dfPlayerReady) player.volume(audioVolume);

    saveSettings();

    lastAction = "VOLUME";
    lastActionMessage = String("Volume set to ") + audioVolume;
    result = lastActionMessage;
    return true;
  }

  if (command == "WATERED") {
    showMessage("WATERING LOGGED",
                "Keep watching the soil moisture.",
                4500);

    if (audioEnabled && dfPlayerReady) {
      requestAudio(39, trackText(39), true, false);
    }

    lastAction = "WATERED";
    lastActionMessage = "Watering logged.";
    sendCloudEvent("watering", 39, trackText(39), "watering");

    result = "Watering logged.";
    return true;
  }

  if (command == "CALIBRATE_DRY") {
    int raw = readSoilRawAveraged();

    if (raw > soilWetCalibration) {
      soilDryCalibration = raw;
      saveSettings();
      updateSensors();
      updatePlantModel();

      lastAction = "CALIBRATE_DRY";
      lastActionMessage = String("Dry calibration stored: ") + raw;
      result = lastActionMessage;
      return true;
    }

    result = "Rejected: dry raw must be greater than wet raw.";
    return false;
  }

  if (command == "CALIBRATE_WET") {
    int raw = readSoilRawAveraged();

    if (raw < soilDryCalibration) {
      soilWetCalibration = raw;
      saveSettings();
      updateSensors();
      updatePlantModel();

      lastAction = "CALIBRATE_WET";
      lastActionMessage = String("Wet calibration stored: ") + raw;
      result = lastActionMessage;
      return true;
    }

    result = "Rejected: wet raw must be lower than dry raw.";
    return false;
  }

  if (command == "RESET_CALIBRATION") {
    soilDryCalibration = DEFAULT_SOIL_DRY_RAW;
    soilWetCalibration = DEFAULT_SOIL_WET_RAW;

    saveSettings();
    updateSensors();
    updatePlantModel();

    lastAction = "RESET_CALIBRATION";
    lastActionMessage = "Soil calibration reset.";
    result = lastActionMessage;
    return true;
  }

  if (command == "SHOW_MESSAGE") {
    String msg = value.length() ? value : "PlantPal message.";

    showMessage("PLANTPAL", msg, MESSAGE_SCREEN_MS);

    lastAction = "SHOW_MESSAGE";
    lastActionMessage = msg;
    result = "Message displayed.";
    return true;
  }

  if (command == "SCREEN_SAVER") {
    oledSleeping = true;
    display.clearDisplay();
    display.display();

    lastAction = "SCREEN_SAVER";
    lastActionMessage = "Screen saver enabled.";
    result = "Screen saver enabled.";
    return true;
  }

  if (command == "WAKE_SCREEN") {
    oledSleeping = false;
    oledEnabled = true;
    wakeOLED();

    lastAction = "WAKE_SCREEN";
    lastActionMessage = "OLED awakened.";
    result = "OLED awakened.";
    return true;
  }

  return false;
}

bool pollCloudCommand() {
  if (!cloudEnabled || WiFi.status() != WL_CONNECTED) return false;

  String path = "/api/commands/next?deviceId=" + String(DEVICE_ID);

  int code = -1;
  String response;

  bool ok = cloudRequest("GET", path, "", code, response);

  if (!ok) {
    cloudConnected = false;
    return false;
  }

  String status = jsonFieldString(response, "status");

  if (status != "pending") return true;

  long commandId = jsonFieldLong(response, "id", 0);
  String command = jsonFieldString(response, "command");
  String value = jsonFieldString(response, "value");

  Serial.print("[CLOUD] Command #");
  Serial.print(commandId);
  Serial.print(" -> ");
  Serial.print(command);

  if (value.length()) {
    Serial.print(" = ");
    Serial.print(value);
  }

  Serial.println();

  String result;
  bool success = executeCloudCommand(command, value, result);

  lastActionAt = millis();
  acknowledgeCloudCommand(commandId, success, result);

  return success;
}

void serviceCloud() {
  if (!cloudEnabled || !PLANTPAL_HAS_CONFIG) return;

  uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    cloudConnected = false;

    if (!wifiAttemptActive && now - lastCloudRetryAt >= CLOUD_RETRY_INTERVAL_MS) {
      lastCloudRetryAt = now;
      cloudConnect();
    } else if (wifiAttemptActive) {
      cloudConnect();
    }

    return;
  }

  cloudConnected = true;
  wifiAttemptActive = false;

  if (now - lastCloudTelemetryAt >= CLOUD_TELEMETRY_INTERVAL_MS) {
    lastCloudTelemetryAt = now;
    sendCloudTelemetry();
  }

  if (now - lastCloudCommandPollAt >= CLOUD_COMMAND_POLL_INTERVAL_MS) {
    lastCloudCommandPollAt = now;
    pollCloudCommand();
  }
}

// 40. SETUP
// ============================================================================

void setup() {
  Serial.begin(115200);
  delay(600);

  Serial.println();
  Serial.println("====================================================");
  Serial.println("              PLANTPAL TULSI FINAL                  ");
  Serial.println("====================================================");
  Serial.print("Firmware: "); Serial.println(FW_VERSION);
  Serial.print("Plant: "); Serial.println(PLANT_NAME);
  Serial.print("Species: "); Serial.println(PLANT_SPECIES);
  Serial.println("Embedded-first local IoT architecture");

  loadSettings();

  // --------------------------------------------------
  // GPIO
  // --------------------------------------------------
  pinMode(PIN_TOUCH, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  // --------------------------------------------------
  // I2C
  // --------------------------------------------------
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);

  // --------------------------------------------------
  // OLED
  // --------------------------------------------------
  Serial.println("Starting OLED...");
  Serial.println(initializeOLED() ? "[OLED] READY" : "[OLED] ERROR");

  // --------------------------------------------------
  // Sensors
  // --------------------------------------------------
  initializeSoil();
  initializeTouch();

  dht.begin();
  Serial.println("[DHT11] Initialized on GPIO4");

  bh1750Ready = initializeBH1750();
  Serial.println(bh1750Ready ? "[BH1750] READY" : "[BH1750] ERROR");

  // --------------------------------------------------
  // DFPlayer
  // --------------------------------------------------
  initializeDFPlayer();

  // --------------------------------------------------
  // Internet STA first; local AP is a fallback only.
  // This avoids radio-channel contention with a phone hotspot.
  // --------------------------------------------------
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  cloudConnect();

  uint32_t wifiWaitStart = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - wifiWaitStart < WIFI_CONNECT_TIMEOUT_MS) {
    cloudConnect();
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    cloudConnected = true;
    wifiAttemptActive = false;
    Serial.print("[WIFI] Internet/STA IP: ");
    Serial.println(WiFi.localIP());
  } else {
    cloudConnected = false;
    wifiAttemptActive = false;
    Serial.println("[WIFI] STA unavailable at startup; entering local fallback.");
    startLocalAPFallback();
    lastCloudRetryAt = millis();
  }

  // --------------------------------------------------
  // Web routes
  // --------------------------------------------------
  server.on("/", handleRoot);
  server.on("/api/data", handleData);
  server.on("/api/play", handlePlay);
  server.on("/api/action", handleAction);
  server.begin();

  Serial.println("[WEB] PlantPal dashboard started");
  Serial.println("[WEB] Open http://192.168.4.1 on your phone");

  // --------------------------------------------------
  // First sensor model
  // --------------------------------------------------
  updateSensors();
  updatePlantModel();

  lastUserInteraction = millis();
  lastAutoPageChange = millis();
  lastOLEDRefresh = millis();
  lastSensorRead = millis();

  if (dfPlayerReady && audioEnabled) {
    shortBeep();
    delay(150);
    startTrackNow(30, trackText(30), false);
  }

  renderOLED();

  // Initial cloud sync.
  if (cloudEnabled && WiFi.status() == WL_CONNECTED) {
    sendCloudTelemetry();
    sendCloudEvent("wifi", 0, "PlantPal cloud link is active.", "wifi");
  }

  Serial.println("----------------------------------------------------");
  Serial.println("PLANTPAL IS READY");
  Serial.println("Phone: connect Wi-Fi 'PlantPal'");
  Serial.println("Password: plantpal123");
  Serial.println("Browser: http://192.168.4.1");
  Serial.println("Serial Monitor: 115200 baud");
  Serial.println("Type help for diagnostics.");
  Serial.println("====================================================");
}

// ============================================================================
// 41. MAIN LOOP
// ============================================================================

void loop() {
  const uint32_t now = millis();

  // Web control always remains responsive.
  server.handleClient();

  // Cloud telemetry and remote commands.
  serviceCloud();

  // Local interaction and audio are higher priority than UI refresh.
  processTouch();
  serviceAudio();
  servicePlantCheck();
  serviceSerial();

  // Sensor cycle.
  if (now - lastSensorRead >= SENSOR_INTERVAL_MS) {
    lastSensorRead = now;
    updateSensors();
    updatePlantModel();
    maybeAutomaticAudio();
  }

  // Display cycle.
  if (now - lastOLEDRefresh >= OLED_INTERVAL_MS) {
    lastOLEDRefresh = now;
    renderOLED();
  }

  delay(2);
}

/*
  ============================================================================
  FINAL VALIDATION / COLLEGE DEMONSTRATION CHECKLIST
  ============================================================================

  1. Upload with Board = ESP32 Dev Module.
  2. Serial Monitor = 115200 baud.
  3. Confirm:
       [OLED] READY
       [BH1750] READY
       [DFPLAYER] READY
       [WIFI] AP started: YES
  4. Phone Wi-Fi -> PlantPal -> password plantpal123.
  5. Open http://192.168.4.1.
  6. Verify live soil/light/temp/humidity values.
  7. Tap Say Hello and confirm DFPlayer speaker output.
  8. Tap Check Plant and confirm multi-step voice response.
  9. Tap Test Buzzer.
 10. Tap several of the 73 voice tracks.
 11. Calibrate soil AFTER the actual Tulsi pot and soil are installed.
 12. Verify dry -> lower moisture percentage; wet -> higher percentage.
 13. Verify long touch cycles the OLED pages.
 14. Verify double touch launches plant check.
 15. Verify automatic voice messages only appear after a condition persists.

  FINAL FILE / SD CARD STRUCTURE
  ------------------------------
  SD card root:
      MP3/
        0001.mp3
        0002.mp3
        ...
        0073.mp3

  FINAL IMPORTANT POWER RULE
  --------------------------
  The DFPlayer is powered from a proper 5V source and shares GND with ESP32.
  The code does not attempt to power DFPlayer from VN/GPIO39.

  WHY THE FINAL OLED IS SMARTER
  -----------------------------
  AUTO mode has six real pages and a message overlay:
      1. Smart plant status
      2. Sensors
      3. Soil
      4. Light
      5. Climate
      6. System/info

  A temporary event message (touch, watering, plant check, voice playback,
  etc.) takes priority and then the display returns to its normal state.

  WHY THE FINAL AUDIO IS SMARTER
  ------------------------------
  Automatic condition audio requires the condition to persist for multiple
  sensor cycles and observes a five-minute cooldown for condition announcements.
  Manual controls can immediately interrupt audio. Plant check uses a queue,
  allowing an introduction, a condition result and a completion message.
  The entire 73-track library remains available from the phone dashboard.

  ============================================================================
*/
