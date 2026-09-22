#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BH1750.h>
#include <DHT.h>
#include <DFRobotDFPlayerMini.h>
#include <Preferences.h>
#include <time.h>

#include "config.h"

// =====================================================
// PlantPal — Final Bidirectional IoT Firmware
// Board: ESP32 Dev Module / ESP32-WROOM-32
// Plant profile: Golden Pothos (Epipremnum aureum)
// =====================================================

// -------------------------
// Pins
// -------------------------
#define SDA_PIN       21
#define SCL_PIN       22
#define SOIL_PIN      34
#define DHT_PIN       4
#define TOUCH_PIN     27
#define DF_RX         16   // ESP32 RX2  <- DFPlayer TX
#define DF_TX         17   // ESP32 TX2  -> DFPlayer RX

// -------------------------
// Display
// -------------------------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDR     0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// -------------------------
// Sensors / audio
// -------------------------
BH1750 lightMeter;
DHT dht(DHT_PIN, DHT11);
HardwareSerial dfSerial(2);
DFRobotDFPlayerMini player;
Preferences prefs;

bool oledOK = false;
bool bh1750OK = false;
bool dfPlayerOK = false;
bool dhtOK = false;

// Runtime controls
bool oledEnabled = true;
bool audioEnabled = true;
String displayMode = "AUTO";

// -------------------------
// PlantPal timing
// -------------------------
const unsigned long TELEMETRY_INTERVAL = 5000UL;
const unsigned long COMMAND_INTERVAL   = 1500UL;
const unsigned long OLED_INTERVAL       = 700UL;
const unsigned long OLED_SAVER_MS      = 60000UL;
const unsigned long MESSAGE_MS         = 5000UL;
const unsigned long CONDITION_AUDIO_COOLDOWN = 60000UL;
const unsigned long CHECK_AUDIO_DELAY = 2500UL;
const unsigned long WIFI_RETRY_MS = 10000UL;
const unsigned long NTP_RETRY_MS = 60000UL;

unsigned long lastTelemetry = 0;
unsigned long lastCommandPoll = 0;
unsigned long lastOLED = 0;
unsigned long lastInteraction = 0;
unsigned long lastConditionAudio = 0;
unsigned long lastWiFiAttempt = 0;
unsigned long lastNtpAttempt = 0;
unsigned long messageUntil = 0;
unsigned long delayedCheckAt = 0;

bool oledSleeping = false;
int pendingCheckTrack = 0;
String pendingCheckMessage;
String pendingCheckIcon = "leaf";

// -------------------------
// Sensor values
// -------------------------
int soilRaw = 0;
int soilPercent = 0;
float lux = NAN;
float temperature = NAN;
float humidity = NAN;
int touchValue = LOW;

// Sensor validity
bool soilValid = false;
bool lightValid = false;
bool temperatureValid = false;
bool humidityValid = false;

// -------------------------
// Plant health model
// These are engineering bands for PlantPal's Golden Pothos profile.
// Soil % is PROJECT CALIBRATION, not a universal horticultural unit.
// -------------------------
enum PlantCondition {
  CONDITION_HEALTHY,
  CONDITION_SOIL_VERY_DRY,
  CONDITION_SOIL_DRY,
  CONDITION_SOIL_WET,
  CONDITION_LIGHT_DARK,
  CONDITION_LIGHT_LOW,
  CONDITION_TEMP_HOT,
  CONDITION_TEMP_COLD,
  CONDITION_HUMIDITY_DRY,
  CONDITION_HUMIDITY_HIGH,
  CONDITION_SENSOR_ERROR
};

PlantCondition currentCondition = CONDITION_HEALTHY;
PlantCondition lastAnnouncedCondition = CONDITION_SENSOR_ERROR;

int healthScore = 0;
String healthLabel = "Unknown";
String soilState = "Unknown";
String lightState = "Unknown";
String temperatureState = "Unknown";
String humidityState = "Unknown";
int activeDryValue = SOIL_DRY_VALUE;
int activeWetValue = SOIL_WET_VALUE;
String plantStatus = "PlantPal is checking your plant.";

// -------------------------
// Cloud/event state
// -------------------------
int lastAudioTrack = 0;
String lastAudioMessage = "";
String lastAction = "";
String lastActionMessage = "";
unsigned long lastActionAt = 0;
long lastCommandId = 0;
String lastGreetingPeriod = "";
int lastGreetingDay = -1;

// -------------------------
// Audio library 0001-0073
// -------------------------
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
  "Buzzer test complete.",
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

const char* trackText(int track) {
  if (track < 1 || track > 73) return "";
  return TRACK_TEXT[track];
}

String trackIcon(int track) {
  switch (track) {
    case 3: case 18: case 19: case 21: case 36: case 38: return "water";
    case 8: case 9: case 10: case 11: case 40: case 41: case 42: return "sun";
    case 12: case 13: case 14: case 43: case 44: case 45: return "temp";
    case 15: case 16: case 17: case 46: case 47: case 48: return "humidity";
    case 50: case 51: case 68: case 69: case 70: case 71: case 72: case 73: return "warning";
    case 60: case 61: case 62: return "touch";
    case 64: case 66: return "quiet";
    default: return "leaf";
  }
}

// =====================================================
// Utility
// =====================================================

String jsonEscape(const String& input) {
  String out;
  out.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    char c = input[i];
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out += c;
  }
  return out;
}

void recordAction(const String& action, const String& message) {
  lastAction = action;
  lastActionMessage = message;
  lastActionAt = millis();
}

void saveControls() {
  prefs.putBool("oled", oledEnabled);
  prefs.putBool("audio", audioEnabled);
  prefs.putString("mode", displayMode);
}

void loadControls() {
  prefs.begin("plantpal", false);
  oledEnabled = prefs.getBool("oled", true);
  audioEnabled = prefs.getBool("audio", true);
  displayMode = prefs.getString("mode", "AUTO");
  lastCommandId = prefs.getLong("cmd", 0);
  activeDryValue = prefs.getInt("dry", SOIL_DRY_VALUE);
  activeWetValue = prefs.getInt("wet", SOIL_WET_VALUE);
}

void saveLastCommand(long commandId) {
  lastCommandId = commandId;
  prefs.putLong("cmd", lastCommandId);
}


void setOledPower(bool enabled) {
  oledEnabled = enabled;
  if (!oledEnabled) {
    display.clearDisplay();
    display.display();
    oledSleeping = false;
    return;
  }
  oledSleeping = false;
  lastInteraction = millis();
  display.ssd1306_command(SSD1306_DISPLAYON);
}

void wakeOLED() {
  if (!oledEnabled || !oledOK) return;
  oledSleeping = false;
  lastInteraction = millis();
  display.ssd1306_command(SSD1306_DISPLAYON);
}

void sleepOLED() {
  if (!oledEnabled || !oledOK) return;
  display.clearDisplay();
  display.display();
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  oledSleeping = true;
}

void drawLeafIcon(int x, int y, int scale = 1) {
  // Stylised PlantPal leaf icon, monochrome.
  display.drawCircle(x + 5 * scale, y + 6 * scale, 5 * scale, SSD1306_WHITE);
  display.drawLine(x + 2 * scale, y + 11 * scale, x + 12 * scale, y + 2 * scale, SSD1306_WHITE);
  display.drawLine(x + 6 * scale, y + 13 * scale, x + 6 * scale, y + 16 * scale, SSD1306_WHITE);
}

void drawDropletIcon(int x, int y) {
  display.drawCircle(x + 6, y + 8, 5, SSD1306_WHITE);
  display.fillTriangle(x + 6, y, x + 1, y + 8, x + 11, y + 8, SSD1306_WHITE);
}

void drawSunIcon(int x, int y) {
  display.drawCircle(x + 7, y + 7, 4, SSD1306_WHITE);
  for (int a = 0; a < 8; ++a) {
    float r1 = 7.0f;
    float r2 = 10.0f;
    float ang = a * PI / 4.0f;
    int x1 = x + 7 + (int)(cos(ang) * r1);
    int y1 = y + 7 + (int)(sin(ang) * r1);
    int x2 = x + 7 + (int)(cos(ang) * r2);
    int y2 = y + 7 + (int)(sin(ang) * r2);
    display.drawLine(x1, y1, x2, y2, SSD1306_WHITE);
  }
}

void drawWarningIcon(int x, int y) {
  display.drawTriangle(x + 7, y, x, y + 14, x + 14, y + 14, SSD1306_WHITE);
  display.drawLine(x + 7, y + 4, x + 7, y + 10, SSD1306_BLACK);
  display.drawPixel(x + 7, y + 12, SSD1306_BLACK);
}

void drawIconByName(const String& icon, int x, int y) {
  if (icon == "water") drawDropletIcon(x, y);
  else if (icon == "sun") drawSunIcon(x, y);
  else if (icon == "warning") drawWarningIcon(x, y);
  else drawLeafIcon(x, y, 1);
}

void drawWrappedText(const String& text, int x, int y, int maxWidth, uint8_t size) {
  display.setTextSize(size);
  display.setCursor(x, y);
  int maxChars = max(1, maxWidth / (6 * size));
  int start = 0;
  while (start < (int)text.length()) {
    int end = min(start + maxChars, (int)text.length());
    if (end < (int)text.length()) {
      int split = text.lastIndexOf(' ', end - 1);
      if (split > start) end = split;
    }
    String line = text.substring(start, end);
    line.trim();
    display.println(line);
    start = end;
    while (start < (int)text.length() && text[start] == ' ') ++start;
    y += 8 * size;
    display.setCursor(x, y);
  }
}

// =====================================================
// Sensor processing
// =====================================================

int averageSoilRead() {
  long total = 0;
  const int samples = 12;
  for (int i = 0; i < samples; ++i) {
    total += analogRead(SOIL_PIN);
    delay(3);
  }
  return (int)(total / samples);
}

void updateSoil() {
  soilRaw = averageSoilRead();
  int dry = activeDryValue;
  int wet = activeWetValue;
  if (dry == wet) {
    soilValid = false;
    soilPercent = 0;
    return;
  }

  soilPercent = map(soilRaw, dry, wet, 0, 100);
  soilPercent = constrain(soilPercent, 0, 100);
  soilValid = (soilRaw > 0 && soilRaw < 4095);

  if (!soilValid) soilState = "Error";
  else if (soilPercent < 20) soilState = "Very dry";
  else if (soilPercent < 38) soilState = "Getting dry";
  else if (soilPercent <= 75) soilState = "Moist / good";
  else soilState = "Quite wet";
}

void updateLight() {
  if (!bh1750OK) {
    lightValid = false;
    lightState = "Error";
    return;
  }
  float reading = lightMeter.readLightLevel();
  if (reading < 0 || reading > 100000) {
    lightValid = false;
    lightState = "Error";
    return;
  }
  lux = reading;
  lightValid = true;

  // Pothos prefers bright indirect light and tolerates some shade.
  // Lux values here are engineering bands for this device/placement.
  if (lux < 200) lightState = "Quite dark";
  else if (lux < 500) lightState = "A little dark";
  else if (lux <= 4000) lightState = "Good / indirect";
  else if (lux <= 10000) lightState = "Bright";
  else lightState = "Very bright";
}

void updateEnvironment() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t) && t > -20 && t < 80) {
    temperature = t;
    temperatureValid = true;
  } else {
    temperatureValid = false;
  }

  if (!isnan(h) && h >= 0 && h <= 100) {
    humidity = h;
    humidityValid = true;
  } else {
    humidityValid = false;
  }

  if (!temperatureValid) temperatureState = "Error";
  else if (temperature < 18) temperatureState = "Cool";
  else if (temperature <= 30) temperatureState = "Comfortable";
  else if (temperature <= 35) temperatureState = "Warm";
  else temperatureState = "Quite warm";

  if (!humidityValid) humidityState = "Error";
  else if (humidity < 40) humidityState = "Dry air";
  else if (humidity <= 80) humidityState = "Comfortable";
  else humidityState = "Humid";

  dhtOK = temperatureValid || humidityValid;
}

void readSensors() {
  updateSoil();
  updateLight();
  updateEnvironment();
  touchValue = digitalRead(TOUCH_PIN);

  // Determine the most relevant condition, prioritising root-zone moisture.
  if (!soilValid || !lightValid || !temperatureValid || !humidityValid) {
    currentCondition = CONDITION_SENSOR_ERROR;
  } else if (soilPercent < 20) {
    currentCondition = CONDITION_SOIL_VERY_DRY;
  } else if (soilPercent < 38) {
    currentCondition = CONDITION_SOIL_DRY;
  } else if (soilPercent > 85) {
    currentCondition = CONDITION_SOIL_WET;
  } else if (lux < 200) {
    currentCondition = CONDITION_LIGHT_DARK;
  } else if (lux < 500) {
    currentCondition = CONDITION_LIGHT_LOW;
  } else if (temperature > 35) {
    currentCondition = CONDITION_TEMP_HOT;
  } else if (temperature < 18) {
    currentCondition = CONDITION_TEMP_COLD;
  } else if (humidity < 40) {
    currentCondition = CONDITION_HUMIDITY_DRY;
  } else if (humidity > 80) {
    currentCondition = CONDITION_HUMIDITY_HIGH;
  } else {
    currentCondition = CONDITION_HEALTHY;
  }

  switch (currentCondition) {
    case CONDITION_SOIL_VERY_DRY:
      plantStatus = "Your plant needs some water.";
      break;
    case CONDITION_SOIL_DRY:
      plantStatus = "The soil is getting dry.";
      break;
    case CONDITION_SOIL_WET:
      plantStatus = "Your plant has enough moisture.";
      break;
    case CONDITION_LIGHT_DARK:
    case CONDITION_LIGHT_LOW:
      plantStatus = "Your plant needs more light.";
      break;
    case CONDITION_TEMP_HOT:
      plantStatus = "It's getting quite warm here.";
      break;
    case CONDITION_TEMP_COLD:
      plantStatus = "It's a little chilly for your plant.";
      break;
    case CONDITION_HUMIDITY_DRY:
      plantStatus = "The air is getting too dry.";
      break;
    case CONDITION_HUMIDITY_HIGH:
      plantStatus = "The humidity is quite high.";
      break;
    case CONDITION_SENSOR_ERROR:
      plantStatus = "I've found something that needs your attention.";
      break;
    default:
      plantStatus = "Your plant is happy and healthy.";
      break;
  }

  // PlantPal Health Index: an engineering indicator, not a botanical diagnosis.
  int soilScore = soilValid ? (soilPercent < 20 ? 20 : soilPercent < 38 ? 65 : soilPercent <= 75 ? 100 : soilPercent <= 85 ? 75 : 40) : 0;
  int lightScore = lightValid ? (lux < 200 ? 25 : lux < 500 ? 65 : lux <= 4000 ? 100 : lux <= 10000 ? 70 : 40) : 0;
  int tempScore = temperatureValid ? (temperature < 15 ? 25 : temperature < 18 ? 65 : temperature <= 30 ? 100 : temperature <= 35 ? 70 : 30) : 0;
  int humidityScore = humidityValid ? (humidity < 30 ? 30 : humidity < 40 ? 65 : humidity <= 80 ? 100 : humidity <= 90 ? 65 : 35) : 0;

  int available = 0;
  int total = 0;
  if (soilValid) { total += soilScore * 35; available += 35; }
  if (lightValid) { total += lightScore * 25; available += 25; }
  if (temperatureValid) { total += tempScore * 20; available += 20; }
  if (humidityValid) { total += humidityScore * 20; available += 20; }
  healthScore = available > 0 ? total / available : 0;

  if (!soilValid || !lightValid || !temperatureValid || !humidityValid) healthLabel = "Checking sensors";
  else if (healthScore >= 85) healthLabel = "Thriving";
  else if (healthScore >= 70) healthLabel = "Doing well";
  else if (healthScore >= 50) healthLabel = "Needs attention";
  else healthLabel = "Action needed";
}

// =====================================================
// NTP / time
// =====================================================

bool timeIsValid() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) return false;
  return (timeinfo.tm_year + 1900) >= 2025;
}

String currentTimeLabel() {
  struct tm t;
  if (!getLocalTime(&t, 100)) return "--:--";
  char buf[12];
  strftime(buf, sizeof(buf), "%I:%M %p", &t);
  return String(buf);
}

String greetingPeriod(int hour) {
  if (hour >= 5 && hour < 12) return "morning";
  if (hour >= 12 && hour < 17) return "afternoon";
  if (hour >= 17 && hour < 22) return "evening";
  return "night";
}

int greetingTrackForPeriod(const String& period) {
  if (period == "morning") return 25;
  if (period == "afternoon") return 26;
  if (period == "evening") return 27;
  return 67;
}

void syncTime() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (timeIsValid() && millis() - lastNtpAttempt < NTP_RETRY_MS) return;
  lastNtpAttempt = millis();
  configTime(19800, 0, "pool.ntp.org", "time.google.com", "time.nist.gov");
}

void maybeTimeGreeting() {
  if (!audioEnabled || !dfPlayerOK || !timeIsValid()) return;
  struct tm t;
  if (!getLocalTime(&t, 50)) return;
  String period = greetingPeriod(t.tm_hour);
  int track = greetingTrackForPeriod(period);

  // Announce each period once per calendar day.
  if (lastGreetingDay == t.tm_yday && lastGreetingPeriod == period) return;
  // Do not auto-greet immediately after boot unless at least 60 seconds have passed.
  if (millis() < 60000UL) return;

  playTrack(track, trackText(track), trackIcon(track), false);
  lastGreetingDay = t.tm_yday;
  lastGreetingPeriod = period;
}

// =====================================================
// OLED
// =====================================================

void drawTopHeader(const String& title) {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(title);
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);
}

void updateOLED() {
  if (!oledOK || !oledEnabled) return;

  if (millis() - lastInteraction >= OLED_SAVER_MS && !messageUntil) {
    sleepOLED();
    return;
  }

  if (oledSleeping) wakeOLED();

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (messageUntil && millis() < messageUntil) {
    drawIconByName(pendingCheckIcon, 104, 3);
    drawTopHeader("PlantPal  •  NOW");
    drawWrappedText(pendingCheckMessage, 0, 16, 126, 1);
    display.setCursor(0, 55);
    display.print("Time ");
    display.print(currentTimeLabel());
    display.display();
    return;
  }

  if (messageUntil && millis() >= messageUntil) messageUntil = 0;

  static uint8_t page = 0;
  static unsigned long lastPageChange = 0;
  if (millis() - lastPageChange > 4500) {
    page = (page + 1) % 3;
    lastPageChange = millis();
  }

  if (displayMode == "HEALTH") page = 1;
  if (displayMode == "SENSORS") page = 0;
  if (displayMode == "STATUS") page = 2;
  if (displayMode == "SAVER") { sleepOLED(); return; }

  if (page == 0) {
    drawTopHeader("PLANTPAL  •  LIVE");
    display.setCursor(0, 14);
    display.printf("Soil  %3d%%", soilPercent);
    display.setCursor(0, 26);
    if (lightValid) display.printf("Light %4.0f lx", lux);
    else display.print("Light  ERR");
    display.setCursor(0, 38);
    if (temperatureValid) display.printf("Temp  %4.1f C", temperature);
    else display.print("Temp  ERR");
    display.setCursor(65, 38);
    if (humidityValid) display.printf("Hum %3.0f%%", humidity);
    else display.print("Hum ERR");
    display.setCursor(0, 51);
    display.print("Cloud ");
    display.print(WiFi.status() == WL_CONNECTED ? "ON" : "OFF");
    display.setCursor(70, 51);
    display.print(dfPlayerOK && audioEnabled ? "AUDIO" : "MUTE");
  } else if (page == 1) {
    drawTopHeader("PLANTPAL  •  HEALTH");
    drawLeafIcon(4, 15, 1);
    display.setTextSize(2);
    display.setCursor(24, 14);
    display.printf("%d%%", healthScore);
    display.setTextSize(1);
    display.setCursor(24, 34);
    display.print(healthLabel);
    display.setCursor(0, 51);
    display.print(plantStatus.length() > 20 ? plantStatus.substring(0, 20) : plantStatus);
  } else {
    drawTopHeader("PLANTPAL  •  STATUS");
    display.setCursor(0, 14);
    display.print("WiFi: ");
    display.println(WiFi.status() == WL_CONNECTED ? "ONLINE" : "OFFLINE");
    display.setCursor(0, 26);
    display.print("Time: ");
    display.println(currentTimeLabel());
    display.setCursor(0, 38);
    display.print("Mode: ");
    display.println(displayMode);
    display.setCursor(0, 51);
    String msg = lastAudioMessage.length() ? lastAudioMessage : "PlantPal is ready.";
    if (msg.length() > 20) msg = msg.substring(0, 20);
    display.print(msg);
  }

  display.display();
}

void showMessage(const String& message, const String& icon) {
  if (!oledOK || !oledEnabled) return;
  pendingCheckMessage = message;
  pendingCheckIcon = icon;
  messageUntil = millis() + MESSAGE_MS;
  wakeOLED();
}

// Forward declaration for cloud event reporting.
void sendEventToCloud(const String& kind, int track, const String& message, const String& icon = "leaf");

// =====================================================
// Audio
// =====================================================

bool playTrack(int track, const String& message = "", const String& icon = "leaf", bool report = true) {
  if (!dfPlayerOK || !audioEnabled) return false;
  if (track < 1 || track > 73) return false;

  player.playMp3Folder(track);
  lastAudioTrack = track;
  lastAudioMessage = message.length() ? message : String(trackText(track));
  recordAction("audio", lastAudioMessage);
  showMessage(lastAudioMessage, icon);
  lastInteraction = millis();

  if (report) {
    sendEventToCloud("audio", track, lastAudioMessage, icon);
  }
  return true;
}

void maybeAnnounceCondition() {
  if (!audioEnabled || !dfPlayerOK) return;
  if (currentCondition == lastAnnouncedCondition) return;
  if (millis() - lastConditionAudio < CONDITION_AUDIO_COOLDOWN) return;

  int track = 22;
  switch (currentCondition) {
    case CONDITION_SOIL_VERY_DRY: track = 36; break;
    case CONDITION_SOIL_DRY: track = 5; break;
    case CONDITION_SOIL_WET: track = 4; break;
    case CONDITION_LIGHT_DARK: track = 11; break;
    case CONDITION_LIGHT_LOW: track = 41; break;
    case CONDITION_TEMP_HOT: track = 43; break;
    case CONDITION_TEMP_COLD: track = 45; break;
    case CONDITION_HUMIDITY_DRY: track = 47; break;
    case CONDITION_HUMIDITY_HIGH: track = 48; break;
    case CONDITION_SENSOR_ERROR: track = 68; break;
    default: track = 22; break;
  }

  if (playTrack(track, trackText(track), trackIcon(track), true)) {
    lastAnnouncedCondition = currentCondition;
    lastConditionAudio = millis();
  }
}

// =====================================================
// HTTP helpers
// =====================================================

bool httpPostJson(const String& url, const String& json, int& code, String& response) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(6000);
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");
  code = http.POST(json);
  if (code > 0) response = http.getString();
  else response = http.errorToString(code);
  http.end();
  return code > 0;
}

String buildTelemetryJson() {
  String json = "{";
  json += "\"deviceId\":\"" + jsonEscape(DEVICE_ID) + "\",";
  json += "\"soilRaw\":" + String(soilRaw) + ",";
  json += "\"soilPercent\":" + String(soilPercent) + ",";
  json += "\"lux\":" + String(lightValid ? lux : 0.0f, 1) + ",";
  json += "\"temperature\":" + String(temperatureValid ? temperature : 0.0f, 1) + ",";
  json += "\"humidity\":" + String(humidityValid ? humidity : 0.0f, 1) + ",";
  json += "\"touch\":" + String(touchValue == HIGH ? "true" : "false") + ",";
  json += "\"audio\":" + String(dfPlayerOK ? "true" : "false") + ",";
  json += "\"audioEnabled\":" + String(audioEnabled ? "true" : "false") + ",";
  json += "\"oledEnabled\":" + String(oledEnabled ? "true" : "false") + ",";
  json += "\"displayMode\":\"" + jsonEscape(displayMode) + "\",";
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
  json += "\"timestamp\":\"" + jsonEscape(timeIsValid() ? currentTimeISO() : "") + "\",";
  json += "\"status\":\"" + jsonEscape(plantStatus) + "\"";
  json += "}";
  return json;
}

String currentTimeISO() {
  struct tm t;
  if (!getLocalTime(&t, 50)) return "";
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+05:30", &t);
  return String(buf);
}

void sendEventToCloud(const String& kind, int track, const String& message, const String& icon) {
  if (WiFi.status() != WL_CONNECTED) return;
  String url = String(SERVER_BASE_URL) + "/api/events";
  String json = "{";
  json += "\"deviceId\":\"" + jsonEscape(DEVICE_ID) + "\",";
  json += "\"kind\":\"" + jsonEscape(kind) + "\",";
  json += "\"track\":" + String(track) + ",";
  json += "\"message\":\"" + jsonEscape(message) + "\",";
  json += "\"icon\":\"" + jsonEscape(icon) + "\",";
  json += "\"createdAt\":\"" + jsonEscape(timeIsValid() ? currentTimeISO() : "") + "\"";
  json += "}";

  int code;
  String response;
  httpPostJson(url, json, code, response);
}

void sendTelemetryToCloud() {
  if (WiFi.status() != WL_CONNECTED) return;
  String url = String(SERVER_BASE_URL) + "/api/sensor-data";
  String json = buildTelemetryJson();
  int code;
  String response;

  Serial.println();
  Serial.println("[CLOUD] POST telemetry");
  Serial.println(json);

  if (httpPostJson(url, json, code, response)) {
    Serial.printf("[CLOUD] HTTP %d\n", code);
    if (code == 201) Serial.println("[CLOUD] Telemetry accepted.");
    else Serial.println("[CLOUD] Server responded with non-201 status.");
  } else {
    Serial.println("[CLOUD] Telemetry POST failed.");
  }
}

// =====================================================
// Command handling
// =====================================================

void acknowledgeCommand(long commandId, bool success, const String& result) {
  if (WiFi.status() != WL_CONNECTED) return;
  String url = String(SERVER_BASE_URL) + "/api/commands/" + String(commandId) + "/ack";
  String json = "{";
  json += "\"deviceId\":\"" + jsonEscape(DEVICE_ID) + "\",";
  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"result\":\"" + jsonEscape(result) + "\"";
  json += "}";
  int code;
  String response;
  httpPostJson(url, json, code, response);
}

bool executeCommand(long commandId, const String& command, const String& value, String& result) {
  // Duplicate protection: if GET returns the same pending command again,
  // acknowledge it without repeating side effects such as audio playback.
  if (commandId > 0 && commandId <= lastCommandId) {
    result = "Already executed";
    return true;
  }

  lastInteraction = millis();
  if (oledEnabled) wakeOLED();

  if (command == "PLAY_AUDIO") {
    int track = value.toInt();
    if (!audioEnabled) {
      result = "Audio is disabled";
      saveLastCommand(commandId);
      return true;
    }
    if (!playTrack(track, trackText(track), trackIcon(track), true)) {
      result = "Audio unavailable or invalid track";
      return false;
    }
    result = "Playing track " + String(track);
  }
  else if (command == "CHECK_PLANT") {
    readSensors();
    playTrack(54, trackText(54), "leaf", true);
    showMessage("Checking your plant...", "leaf");
    delayedCheckAt = millis() + CHECK_AUDIO_DELAY;
    pendingCheckTrack = 0;
    result = "Plant check started";
  }
  else if (command == "TIME_GREETING") {
    if (!timeIsValid()) {
      result = "Time not synchronized";
      return false;
    }
    struct tm t;
    if (!getLocalTime(&t, 100)) return false;
    String period = greetingPeriod(t.tm_hour);
    int track = greetingTrackForPeriod(period);
    if (!playTrack(track, trackText(track), trackIcon(track), true)) return false;
    result = "Played " + period + " greeting";
  }
  else if (command == "OLED_ON") {
    setOledPower(true);
    showMessage("OLED display enabled.", "leaf");
    recordAction("oled", "OLED display enabled.");
    result = "OLED ON";
  }
  else if (command == "OLED_OFF") {
    recordAction("oled", "OLED display disabled.");
    setOledPower(false);
    result = "OLED OFF";
  }
  else if (command == "AUDIO_ON") {
    audioEnabled = true;
    saveControls();
    playTrack(65, trackText(65), "leaf", true);
    result = "Audio enabled";
  }
  else if (command == "AUDIO_OFF") {
    // 0066 is intentionally not played because turning audio off means staying quiet.
    audioEnabled = false;
    saveControls();
    showMessage("Audio disabled.", "quiet");
    recordAction("audio", "Audio disabled.");
    result = "Audio disabled";
  }
  else if (command == "SILENT_MODE") {
    audioEnabled = false;
    saveControls();
    showMessage("Quiet mode is now active.", "quiet");
    recordAction("quiet", "Quiet mode is now active.");
    result = "Audio disabled";
  }
  else if (command == "DISPLAY_MODE") {
    String mode = value;
    mode.toUpperCase();
    if (!(mode == "AUTO" || mode == "HEALTH" || mode == "SENSORS" || mode == "STATUS" || mode == "SAVER")) {
      result = "Invalid display mode";
      return false;
    }
    displayMode = mode;
    saveControls();
    if (mode == "SAVER") sleepOLED(); else wakeOLED();
    recordAction("display", "Display mode: " + mode);
    result = "Display mode set to " + mode;
  }
  else if (command == "WATERED") {
    // Human-assisted watering: PlantPal logs the action but never falsifies soil data.
    if (audioEnabled) playTrack(39, trackText(39), "water", true);
    showMessage("Watering logged. Keep an eye on the soil.", "water");
    recordAction("watered", "Watering logged.");
    result = "Manual watering event logged";
  }
  else if (command == "CALIBRATE_DRY") {
    int valueNow = averageSoilRead();
    if (valueNow <= activeWetValue) {
      result = "Dry calibration rejected: raw value not above wet value";
      return false;
    }
    activeDryValue = valueNow;
    prefs.putInt("dry", valueNow);
    result = "Dry calibration saved: " + String(valueNow);
  }
  else if (command == "CALIBRATE_WET") {
    int valueNow = averageSoilRead();
    if (valueNow >= activeDryValue) {
      result = "Wet calibration rejected: raw value not below dry value";
      return false;
    }
    activeWetValue = valueNow;
    prefs.putInt("wet", valueNow);
    result = "Wet calibration saved: " + String(valueNow);
  }
  else {
    result = "Unsupported command";
    return false;
  }

  saveLastCommand(commandId);
  return true;
}

void pollCommands() {
  if (WiFi.status() != WL_CONNECTED) return;

  String url = String(SERVER_BASE_URL) + "/api/commands/next?deviceId=" + String(DEVICE_ID);
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(5000);
  if (!http.begin(client, url)) return;

  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    Serial.print("[CMD] ");
    Serial.println(payload);

    // Lightweight JSON extraction for the controlled server response.
    int statusPos = payload.indexOf("\"status\":\"pending\"");
    if (statusPos >= 0) {
      auto extractString = [&](const String& key) -> String {
        String marker = "\"" + key + "\":\"";
        int p = payload.indexOf(marker);
        if (p < 0) return "";
        p += marker.length();
        int e = payload.indexOf('"', p);
        return e >= 0 ? payload.substring(p, e) : "";
      };
      auto extractLong = [&](const String& key) -> long {
        String marker = "\"" + key + "\":";
        int p = payload.indexOf(marker);
        if (p < 0) return -1;
        p += marker.length();
        int e = payload.indexOf(',', p);
        if (e < 0) e = payload.indexOf('}', p);
        return e >= 0 ? payload.substring(p, e).toInt() : -1;
      };

      long id = extractLong("id");
      String command = extractString("command");
      String value = extractString("value");
      if (id > 0 && command.length()) {
        String result;
        bool ok = executeCommand(id, command, value, result);
        acknowledgeCommand(id, ok, result);
        if (command != "PLAY_AUDIO" && command != "CHECK_PLANT" && command != "TIME_GREETING") {
          sendEventToCloud(ok ? "command" : "command_error", 0, result, ok ? "leaf" : "warning");
        }
      }
    }
  }
  http.end();
}

// =====================================================
// Wi-Fi / startup
// =====================================================

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (lastWiFiAttempt != 0 && millis() - lastWiFiAttempt < WIFI_RETRY_MS) return;
  lastWiFiAttempt = millis();

  Serial.println("[WIFI] Connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000UL) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WIFI] Connected. IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("[WIFI] RSSI: ");
    Serial.println(WiFi.RSSI());
    recordAction("cloud", "The cloud connection is active.");
    syncTime();
    if (oledEnabled) showMessage("Cloud connection active.", "leaf");
  } else {
    Serial.println("[WIFI] Connection failed; will retry.");
  }
}

void sendStartupEvents() {
  if (dfPlayerOK && audioEnabled) {
    playTrack(30, trackText(30), "leaf", true);
  }
}

// =====================================================
// Setup
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(TOUCH_PIN, INPUT);
  loadControls();
  lastInteraction = millis();

  Wire.begin(SDA_PIN, SCL_PIN);

  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    oledOK = true;
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(20, 12);
    display.println("PlantPal");
    display.setCursor(15, 28);
    display.println("Smart Plant IoT");
    display.setCursor(17, 46);
    display.println("Starting...");
    display.display();
  } else {
    Serial.println("[OLED] NOT FOUND");
  }

  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    bh1750OK = true;
    Serial.println("[BH1750] OK");
  } else {
    Serial.println("[BH1750] ERROR");
  }

  dht.begin();

  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);

  dfSerial.begin(9600, SERIAL_8N1, DF_RX, DF_TX);
  if (player.begin(dfSerial)) {
    dfPlayerOK = true;
    player.volume(18);
    Serial.println("[DFPLAYER] OK");
  } else {
    Serial.println("[DFPLAYER] ERROR");
  }

  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  connectWiFi();
  configTime(19800, 0, "pool.ntp.org", "time.google.com", "time.nist.gov");

  readSensors();
  updateOLED();

  // Give the audio system a moment after DFPlayer initialization.
  delay(500);
  sendStartupEvents();
}

// =====================================================
// Loop
// =====================================================

void loop() {
  unsigned long now = millis();

  // Wi-Fi maintenance
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  // Touch interaction
  static bool previousTouch = false;
  bool currentTouch = digitalRead(TOUCH_PIN) == HIGH;
  if (currentTouch && !previousTouch) {
    lastInteraction = now;
    wakeOLED();
    if (audioEnabled && dfPlayerOK) {
      playTrack(60, trackText(60), "touch", true);
    }
  }
  previousTouch = currentTouch;

  // Sensor + telemetry
  if (now - lastTelemetry >= TELEMETRY_INTERVAL) {
    lastTelemetry = now;
    readSensors();
    syncTime();
    if (WiFi.status() == WL_CONNECTED) {
      sendTelemetryToCloud();
    }
    maybeAnnounceCondition();
    maybeTimeGreeting();
  }

  // Remote command queue
  if (now - lastCommandPoll >= COMMAND_INTERVAL) {
    lastCommandPoll = now;
    pollCommands();
  }

  // Finish a two-step remote plant check without blocking the MCU.
  if (delayedCheckAt && now >= delayedCheckAt) {
    delayedCheckAt = 0;
    readSensors();
    int track = 22;
    switch (currentCondition) {
      case CONDITION_SOIL_VERY_DRY: track = 36; break;
      case CONDITION_SOIL_DRY: track = 3; break;
      case CONDITION_SOIL_WET: track = 4; break;
      case CONDITION_LIGHT_DARK: track = 11; break;
      case CONDITION_LIGHT_LOW: track = 41; break;
      case CONDITION_TEMP_HOT: track = 43; break;
      case CONDITION_TEMP_COLD: track = 45; break;
      case CONDITION_HUMIDITY_DRY: track = 47; break;
      case CONDITION_HUMIDITY_HIGH: track = 48; break;
      case CONDITION_SENSOR_ERROR: track = 68; break;
      default: track = 22; break;
    }
    if (audioEnabled && dfPlayerOK) {
      playTrack(track, trackText(track), trackIcon(track), true);
    }
    lastAnnouncedCondition = currentCondition;
    lastConditionAudio = now;
  }

  // OLED rendering
  if (now - lastOLED >= OLED_INTERVAL) {
    lastOLED = now;
    updateOLED();
  }

  delay(2);
}
