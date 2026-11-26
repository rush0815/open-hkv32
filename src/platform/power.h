#pragma once
// Power helpers for the TinyPICO prototype.

#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_sleep.h>

#include "board_tinypico.h"

namespace port {

// Switches the on-board APA102 RGB LED off (it draws current even when dark).
inline void disableDotstar() {
  pinMode(board::kDotstarPower, OUTPUT);
  digitalWrite(board::kDotstarPower, HIGH);
  pinMode(board::kDotstarData, INPUT_PULLDOWN);
  pinMode(board::kDotstarClock, INPUT_PULLDOWN);
}

// Light sleep keeps RAM, peripherals' state and millis(); wakes on timer or one of the
// buttons (LOW level).
inline void lightSleep(uint32_t ms, int buttonPin, int wifiButtonPin) {
  Serial.flush();
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(ms) * 1000ULL);
  gpio_wakeup_enable(static_cast<gpio_num_t>(buttonPin), GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(static_cast<gpio_num_t>(wifiButtonPin), GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_light_sleep_start();
  gpio_wakeup_disable(static_cast<gpio_num_t>(buttonPin));
  gpio_wakeup_disable(static_cast<gpio_num_t>(wifiButtonPin));
}

}  // namespace port
