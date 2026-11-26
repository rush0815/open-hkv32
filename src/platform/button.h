#pragma once
// Debounced push button with short / long press detection (active low).

#include <Arduino.h>

namespace port {

class Button {
 public:
  enum class Event { None, Short, Long };

  Button(int pin, uint32_t longMs) : pin_(pin), longMs_(longMs) {}

  void begin() { pinMode(pin_, INPUT_PULLUP); }

  Event poll() {
    const bool down = digitalRead(pin_) == LOW;
    const uint32_t now = millis();
    if (down != raw_) {
      raw_ = down;
      changedAt_ = now;
    }
    if (now - changedAt_ < kDebounceMs) return Event::None;

    if (down && !pressed_) {
      pressed_ = true;
      longFired_ = false;
      pressedAt_ = changedAt_;
    } else if (down && pressed_ && !longFired_ && now - pressedAt_ >= longMs_) {
      longFired_ = true;
      return Event::Long;
    } else if (!down && pressed_) {
      pressed_ = false;
      if (!longFired_) return Event::Short;
    }
    return Event::None;
  }

  bool active() const { return raw_ || pressed_; }

 private:
  static constexpr uint32_t kDebounceMs = 30;
  int pin_;
  uint32_t longMs_;
  bool raw_ = false;
  bool pressed_ = false;
  bool longFired_ = false;
  uint32_t changedAt_ = 0;
  uint32_t pressedAt_ = 0;
};

}  // namespace port
