#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
class Print {
 public:
  virtual ~Print() {}
  virtual size_t write(uint8_t c) = 0;
  virtual size_t write(const uint8_t* b, size_t n) { size_t r = 0; while (n--) r += write(*b++); return r; }
  size_t write(const char* s) { return write(reinterpret_cast<const uint8_t*>(s), strlen(s)); }
  size_t print(const char* s) { return write(s); }
  size_t print(char c) { return write(static_cast<uint8_t>(c)); }
  size_t print(int v) { char b[16]; snprintf(b, sizeof b, "%d", v); return print(b); }
  size_t print(unsigned v) { char b[16]; snprintf(b, sizeof b, "%u", v); return print(b); }
  size_t print(long v) { char b[24]; snprintf(b, sizeof b, "%ld", v); return print(b); }
  size_t print(unsigned long v) { char b[24]; snprintf(b, sizeof b, "%lu", v); return print(b); }
  size_t print(double v, int d = 2) { char b[32]; snprintf(b, sizeof b, "%.*f", d, v); return print(b); }
};
