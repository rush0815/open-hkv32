#pragma once
// OMS / wireless M-Bus (EN 13757-4/-7) telegram builder for a heat cost allocator.
//
// Frame produced (link layer without block CRCs, as used by most radio drivers and
// accepted by wmbusmeters):
//   L C M M A A A A V T | CI ACC STS CFG CFG | [2F 2F] records [2F padding]
//   C   = 0x44 SND_NR, T = 0x08 heat cost allocator, CI = 0x7A short transport header
//   CFG = security mode 0 (none) or mode 5 (AES-128-CBC, IV = M+A+ACC*8)
//
// Data records:
//   04 6D  date/time (type F)                       current time
//   03 6E  24 bit int, units for HCA                current billing period
//   42 6C  date (type G), storage 1                 last due date
//   43 6E  24 bit int, units for HCA, storage 1     value at last due date
//   02 59  int16, flow temperature 0.01 C           radiator surface temperature
//   02 65  int16, external temperature 0.01 C       room temperature

#include <stddef.h>
#include <stdint.h>

#include "hkv/calendar.h"
#include "hkv/hal.h"

namespace hkv {

namespace oms {

constexpr uint8_t kDeviceTypeHca = 0x08;
constexpr size_t kMaxFrame = 128;

// Status byte (EN 13757-7, table "Status field")
constexpr uint8_t kStatusErrorAny = 0x02;
constexpr uint8_t kStatusPowerLow = 0x04;
constexpr uint8_t kStatusPermanentError = 0x08;
constexpr uint8_t kStatusTemporaryError = 0x10;

struct HcaTelegram {
  const char* manufacturer = "XXX";
  uint32_t meterId = 0;
  uint8_t version = 1;
  uint8_t accessNo = 0;
  uint8_t status = 0;
  uint8_t security = 0;          // 0 or 5
  const uint8_t* aesKey = nullptr;  // 16 bytes, required for mode 5

  bool timeValid = false;
  DateTime now;                  // local time

  uint32_t currentUnits = 0;
  bool dueDateValid = false;
  DateTime dueDate;
  uint32_t dueDateUnits = 0;

  bool radiatorValid = false;
  float radiatorC = 0.0f;
  bool roomValid = false;
  float roomC = 0.0f;
};

uint16_t manufacturerCode(const char* letters);
uint16_t encodeDateG(const DateTime& d);
uint32_t encodeDateTimeF(const DateTime& d, bool valid);

// Builds the frame into out. cipher is only used for mode 5. Returns length or 0 on error.
size_t buildHcaTelegram(const HcaTelegram& t, IBlockCipher* cipher, uint8_t* out, size_t cap);

// CRC as used in wM-Bus frames (poly 0x3D65, final XOR 0xFFFF).
uint16_t crc16(const uint8_t* data, size_t len);

// Converts a frame without CRCs to frame format A (CRC after first 10 bytes, then every 16).
size_t toFrameFormatA(const uint8_t* frame, size_t len, uint8_t* out, size_t cap);

size_t toHex(const uint8_t* data, size_t len, char* out, size_t cap);

}  // namespace oms
}  // namespace hkv
