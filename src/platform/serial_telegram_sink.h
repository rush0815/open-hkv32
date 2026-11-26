#pragma once
// Prototype "radio": prints every OMS telegram as hex. Decode on the PC with
//   wmbusmeters --analyze <hex>              (security mode 0)
//   wmbusmeters --analyze=<aes-key> <hex>    (security mode 5)
// On the CC1312 this is replaced by the RF driver (wM-Bus T1/C1 at 868.95 MHz).

#include <Arduino.h>

#include "hkv/hal.h"
#include "hkv/oms.h"

namespace port {

class SerialTelegramSink : public hkv::ITelegramSink {
 public:
  void send(const uint8_t* frame, size_t len) override {
    char hex[2 * hkv::oms::kMaxFrame + 1];
    hkv::oms::toHex(frame, len, hex, sizeof(hex));
    Serial.printf("TELEGRAM %s\n", hex);
  }
};

}  // namespace port
