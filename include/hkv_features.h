#pragma once
// Compile time feature selection of the ESP32 prototype.
// Can also be overridden from platformio.ini, e.g.  build_flags = -DHKV_RADIATOR_SENSOR=1

// Radiator sensor: 1 = MLX90614 on I2C (default),
//                  0 = emulated radiator (random heating cycles) - fallback if the sensor fails
#ifndef HKV_RADIATOR_SENSOR
#define HKV_RADIATOR_SENSOR 1
#endif

// Speeds up the emulated radiator (e.g. 10 = ten times faster heating cycles).
#ifndef HKV_EMULATION_TIME_SCALE
#define HKV_EMULATION_TIME_SCALE 1.0f
#endif

// MLX90614 diagnostics in the serial log. The I2C scan and the sensor info (emissivity,
// config, ID) are always logged at boot; 1 = additionally all raw values every 10 s.
#ifndef HKV_MLX_DIAG
#define HKV_MLX_DIAG 0
#endif

// Display test at boot: 2 s white, then 1 s red, green, blue (wiring check).
// Also runs a wiring check of the display lines (shorts, stuck pins) - see serial log.
#ifndef HKV_DISPLAY_SELFTEST
#define HKV_DISPLAY_SELFTEST 0
#endif

#ifndef HKV_ENABLE_DISPLAY
#define HKV_ENABLE_DISPLAY 1
#endif

#ifndef HKV_ENABLE_WIFI
#define HKV_ENABLE_WIFI 1
#endif

// Light sleep between measurements while WiFi and display are off.
#ifndef HKV_ENABLE_LIGHT_SLEEP
#define HKV_ENABLE_LIGHT_SLEEP 1
#endif

#define HKV_FIRMWARE_VERSION "0.3.0"

// Button timing
#define HKV_LONG_PRESS_MS 3000

// WiFi setup portal
#define HKV_PORTAL_TIMEOUT_MS (10UL * 60UL * 1000UL)
