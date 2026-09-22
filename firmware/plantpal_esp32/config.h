#ifndef CONFIG_H
#define CONFIG_H

// LOCAL FILE ONLY. This file is excluded by .gitignore.
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* SERVER_BASE_URL = "https://plantpal-iot.onrender.com";
const char* DEVICE_ID = "plantpal-01";

#define CLOUD_ENABLED_DEFAULT true
#define SERIAL_DEBUG_DEFAULT true
#define OLED_DEFAULT_ENABLED true
#define AUDIO_DEFAULT_ENABLED true
#define DISPLAY_DEFAULT_MODE "AUTO"

#define SOIL_DRY_VALUE 3000
#define SOIL_WET_VALUE 1200

#endif
