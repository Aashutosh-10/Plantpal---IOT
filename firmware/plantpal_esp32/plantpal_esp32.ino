#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BH1750.h>
#include <DHT.h>
#include <DFRobotDFPlayerMini.h>

// =====================================================
// WIFI & SERVER CREDENTIALS
// =====================================================
const char* WIFI_SSID     = "YOUR_HOTSPOT_NAME";     // <--- Your exact Wi-Fi/Hotspot name
const char* WIFI_PASSWORD = "YOUR_HOTSPOT_PASSWORD"; // <--- Your exact password

// Your live Render endpoint
const char* SERVER_URL    = "https://plantpal-iot.onrender.com/api/sensor-data";

// =====================================================
// PIN ASSIGNMENTS
// =====================================================
#define SDA_PIN       21
#define SCL_PIN       22
#define SOIL_PIN      34
#define DHT_PIN       4
#define TOUCH_PIN     27
#define BUZZER_PIN    25

#define DF_RX         16
#define DF_TX         17

// =====================================================
// PERIPHERALS
// =====================================================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

BH1750 lightMeter;
#define DHT_TYPE DHT11
DHT dht(DHT_PIN, DHT_TYPE);

HardwareSerial dfSerial(2);
DFRobotDFPlayerMini player;

bool dfPlayerOK = false;
bool bh1750OK   = false;
bool oledOK     = false;

#define SOIL_DRY_VALUE 3000
#define SOIL_WET_VALUE 1200

int   soilValue   = 0;
int   soilPercent = 0;
int   touchValue  = 0;
float temperature = NAN;
float humidity    = NAN;
float lux         = NAN;

unsigned long lastCloudPost  = 0;
unsigned long lastOLEDUpdate = 0;
const unsigned long CLOUD_INTERVAL = 5000;
const unsigned long OLED_INTERVAL  = 1000;

void readSensors() {
  soilValue   = analogRead(SOIL_PIN);
  soilPercent = map(soilValue, SOIL_DRY_VALUE, SOIL_WET_VALUE, 0, 100);
  soilPercent = constrain(soilPercent, 0, 100);
  touchValue  = digitalRead(TOUCH_PIN);

  if (bh1750OK) {
    lux = lightMeter.readLightLevel();
  }

  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) temperature = t;
  if (!isnan(h)) humidity = h;
}

String getPlantStatus() {
  if (soilPercent < 25) return "Your plant needs some water.";
  if (bh1750OK && lux < 100) return "Your plant needs more light.";
  if (!isnan(temperature)) {
    if (temperature > 35) return "It's getting a little too warm.";
    if (temperature < 15) return "It's a little chilly for your plant.";
  }
  if (!isnan(humidity)) {
    if (humidity < 30) return "The air is getting too dry.";
    if (humidity > 85) return "The humidity is quite high.";
  }
  return "Your plant is happy and healthy.";
}

void updateOLED() {
  if (!oledOK) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("WiFi: ");
  display.println(WiFi.status() == WL_CONNECTED ? "ONLINE" : "OFFLINE");
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  display.setCursor(0, 14);
  display.printf("Soil: %d%%", soilPercent);

  display.setCursor(0, 26);
  if (bh1750OK) display.printf("Light: %.0f lx", lux);
  else display.print("Light: ERR");

  display.setCursor(0, 38);
  if (!isnan(temperature)) display.printf("T: %.1fC", temperature);
  else display.print("T: ERR");

  display.setCursor(65, 38);
  if (!isnan(humidity)) display.printf("H: %.0f%%", humidity);
  else display.print("H: ERR");

  display.setCursor(0, 51);
  display.printf("Touch: %s", touchValue == HIGH ? "YES" : "NO");

  display.setCursor(70, 51);
  display.print(dfPlayerOK ? "AUD OK" : "AUD ERR");

  display.display();
}

void sendTelemetryToCloud() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Not connected. Reconnecting...");
    WiFi.reconnect();
    return;
  }

  // Use WiFiClientSecure to handle HTTPS encryption cleanly
  WiFiClientSecure client;
  client.setInsecure(); // Allows HTTPS without manually embedding root certs

  HTTPClient http;
  if (http.begin(client, SERVER_URL)) {
    http.addHeader("Content-Type", "application/json");

    String json = "{";
    json += "\"soilRaw\":" + String(soilValue) + ",";
    json += "\"soilPercent\":" + String(soilPercent) + ",";
    json += "\"lux\":" + String(isnan(lux) ? 0.0 : lux, 1) + ",";
    json += "\"temperature\":" + String(isnan(temperature) ? 0.0 : temperature, 1) + ",";
    json += "\"humidity\":" + String(isnan(humidity) ? 0.0 : humidity, 1) + ",";
    json += "\"touch\":" + String(touchValue == HIGH ? "true" : "false") + ",";
    json += "\"audio\":" + String(dfPlayerOK ? "true" : "false") + ",";
    json += "\"status\":\"" + getPlantStatus() + "\"";
    json += "}";

    int httpCode = http.POST(json);
    if (httpCode > 0) {
      Serial.printf("[HTTP] POST Success! Response Code: %d\n", httpCode);
    } else {
      Serial.printf("[HTTP] POST Failed. Error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
  } else {
    Serial.println("[HTTP] Unable to connect to Render host");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(TOUCH_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin(SDA_PIN, SCL_PIN);

  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    oledOK = true;
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 20);
    display.println("Connecting WiFi...");
    display.display();
  }

  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    bh1750OK = true;
  }

  dht.begin();

  dfSerial.begin(9600, SERIAL_8N1, DF_RX, DF_TX);
  if (player.begin(dfSerial)) {
    dfPlayerOK = true;
    player.volume(20);
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed! Retrying in background...");
  }

  tone(BUZZER_PIN, 2000, 200);
  if (dfPlayerOK) {
    player.playMp3Folder(1);
  }

  readSensors();
  updateOLED();
}

void loop() {
  static bool prevTouch = false;
  int currentTouch = digitalRead(TOUCH_PIN);

  if (currentTouch == HIGH && !prevTouch) {
    tone(BUZZER_PIN, 2000, 150);
    if (dfPlayerOK) {
      player.playMp3Folder(1);
    }
  }
  prevTouch = (currentTouch == HIGH);

  if (millis() - lastCloudPost >= CLOUD_INTERVAL) {
    lastCloudPost = millis();
    readSensors();
    sendTelemetryToCloud();
  }

  if (millis() - lastOLEDUpdate >= OLED_INTERVAL) {
    lastOLEDUpdate = millis();
    updateOLED();
  }
}
