#pragma once
// Device settings that belong to the meter itself (portable).
// Network/WiFi settings are platform specific and live in the port.

#include <stdint.h>

namespace hkv {

enum class ScaleMode : uint8_t {
  Unit = 0,     // "Einheitsskala": units = Kc * (dT/dTn)^n per hour
  Product = 1,  // "Produktskala": additionally multiplied by the rated radiator power Kq (kW)
};

enum class OmsSecurity : uint8_t {
  None = 0,  // unencrypted (only for development!)
  Mode5 = 5, // AES-128-CBC, OMS security profile A
};

struct Settings {
  // --- identification (wM-Bus A-field / M-field) ---
  uint32_t meterId;        // 0..99999999, sent as 8 BCD digits
  char manufacturer[4];    // 3 letters A..Z. Placeholder until a FLAG id is registered.
  uint8_t deviceVersion;

  // --- allocation algorithm (EN 834 style two-sensor device) ---
  ScaleMode scale;
  float kc;                 // thermal coupling / rating factor
  float kq;                 // rated radiator power in kW (product scale only)
  float exponent;           // radiator exponent n (typically 1.3)
  float nominalDeltaK;      // reference excess temperature (50 K per EN 442)
  float startDeltaK;        // counting starts when radiator - room >= this
  float minRadiatorC;       // ... and radiator >= this
  float summerMinRadiatorC; // stricter threshold inside the summer period
  uint8_t summerStartMonth; // 0 = no summer suppression
  uint8_t summerEndMonth;
  float radiatorOffsetC;    // calibration offsets
  float roomOffsetC;

  // --- billing ---
  uint8_t dueMonth;         // Stichtag
  uint8_t dueDay;

  // --- timing ---
  uint16_t measureIntervalS;
  uint16_t telegramIntervalS;
  int16_t utcOffsetMin;
  uint8_t euDst;            // 1 = apply EU summer time

  // --- power ---
  uint16_t batteryLowMv;

  // --- OMS radio ---
  uint8_t omsSecurity;      // OmsSecurity
  uint8_t aesKey[16];

  // --- local user interface ---
  uint16_t displayTimeoutS;

  // --- added in settings version 2 (keep new fields at the end: migration) ---
  float emissivity;         // radiator surface for IR sensors, 1.0 = no correction

  // --- added in settings version 3 ---
  uint8_t displayInvert;    // 1 = invert panel colours (many IPS modules need it)
  uint8_t displayFlip;      // 1 = rotate the display content by 180 degrees

  // --- added in settings version 4 ---
  // Thermal coupling of the room sensor to the radiator in a device mounted on the radiator:
  // T_measured = T_room + c * (T_radiator - T_room)  ->  corrected out. 0 = off.
  float roomCoupling;
};

void setDefaults(Settings& s, uint32_t defaultMeterId);

// Returns nullptr if valid, otherwise a short English error text.
const char* validate(const Settings& s);

bool isSummer(const Settings& s, uint8_t month);

}  // namespace hkv
