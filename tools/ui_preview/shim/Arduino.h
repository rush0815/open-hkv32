#pragma once
// Minimal Arduino shim so Adafruit_GFX and the UI renderer compile on the PC.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <string>
typedef bool boolean;
typedef uint8_t byte;
#define PROGMEM
#define pgm_read_byte(a) (*(const uint8_t*)(a))
#define pgm_read_word(a) (*(const uint16_t*)(a))
#define pgm_read_dword(a) (*(const uint32_t*)(a))
#define pgm_read_pointer(a) (*(void* const*)(a))
#define radians(deg) ((deg) * 0.017453292519943295)
#define degrees(rad) ((rad) * 57.29577951308232)
#define _swap_int16_t(a, b) { int16_t t = a; a = b; b = t; }
class __FlashStringHelper;
class String {
 public:
  String(const char* s = "") : s_(s) {}
  const char* c_str() const { return s_.c_str(); }
  unsigned length() const { return s_.size(); }
 private:
  std::string s_;
};
#include "Print.h"
