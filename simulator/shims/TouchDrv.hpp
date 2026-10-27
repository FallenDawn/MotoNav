// PC/SDL 模拟器桩：CST816 触摸。触点来自 SDL 鼠标（见 sim_input.cpp）。
#pragma once
#include "Arduino.h"
#include "Wire.h"

#define CST816_SLAVE_ADDRESS 0x15

class TouchDrv {
public:
  void setPins(int resetPin, int irqPin) { (void)resetPin; (void)irqPin; }
  bool begin(TwoWire &w, uint8_t addr, int sda, int scl);
  void disableAutoSleep() {}
  uint8_t getPoint(int16_t *x, int16_t *y, uint8_t maxPoints);
};
typedef TouchDrv TouchDrvCST816;
