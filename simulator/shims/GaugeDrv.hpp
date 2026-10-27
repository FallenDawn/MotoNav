// PC/SDL 模拟器桩：BQ27220 电量计（固定返回一块 78% 的电池，可用 UDP 指令改）
#pragma once
#include "Arduino.h"
#include "Wire.h"

class GaugeBatteryStatus {
public:
  bool isBatteryPresent() { return true; }
};

class GaugeDrv {
public:
  bool begin(TwoWire &w, int sda, int scl) { (void)w; (void)sda; (void)scl; return true; }
  bool refresh() { return true; }
  int getVoltage();               // 毫伏
  int getStateOfCharge();         // 百分比
  bool isCharging();
  GaugeBatteryStatus getBatteryStatus() { return GaugeBatteryStatus(); }
};
typedef GaugeDrv GaugeBQ27220;   // 固件里声明的是 GaugeBQ27220 类型
