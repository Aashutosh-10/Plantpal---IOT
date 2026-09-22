#ifndef PLANTPAL_CONFIG_H
#define PLANTPAL_CONFIG_H

// Copy this file to firmware/plantpal_esp32/config.h before compiling if desired.
// Keep the real Wi-Fi credentials OUT of GitHub.

const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char* SERVER_BASE_URL = "https://plantpal-iot.onrender.com";
const char* DEVICE_ID = "plantpal-01";

// Project-specific initial soil calibration.
// Recalibrate after placing the sensor in your actual local garden soil.
const int SOIL_DRY_VALUE = 3000;
const int SOIL_WET_VALUE = 1200;

#endif
