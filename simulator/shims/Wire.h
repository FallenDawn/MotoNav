// PC/SDL 模拟器桩：I2C 总线
#pragma once
#include "Arduino.h"

class TwoWire {
public:
  void begin(int sda = -1, int scl = -1) { (void)sda; (void)scl; }
  void end() {}
};
extern TwoWire Wire;
