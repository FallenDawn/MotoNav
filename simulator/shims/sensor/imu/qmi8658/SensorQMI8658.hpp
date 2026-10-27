// PC/SDL 模拟器桩：QMI8658 IMU。数据由模拟输入（键盘/UDP）驱动，见 sim_net.cpp。
#pragma once
#include "Arduino.h"
#include "Wire.h"

// SensorLib 的 IMU 数据结构（字段名与真库一致：a.mps2.x / g.dps.x）
struct AccelerometerData {
  struct { float x, y, z; } mps2;   // 米/秒²
};
struct GyroscopeData {
  struct { float x, y, z; } dps;    // 度/秒
};

class SensorQMI8658 {
public:
  enum class AccelRange { FS_2G, FS_4G, FS_8G, FS_16G };
  enum class GyroRange { FS_16DPS, FS_32DPS, FS_64DPS, FS_128DPS, FS_256DPS, FS_512DPS, FS_1024DPS };
  enum class AccelODR { ODR_1000Hz = 0, ODR_112Hz = 6 };
  enum class GyroODR { ODR_1000Hz = 0, ODR_112Hz = 6 };

  bool begin(TwoWire &w, uint8_t addr, int sda, int scl);
  void configAccel(AccelRange range, AccelODR odr) { (void)range; (void)odr; }
  void configGyro(GyroRange range, GyroODR odr) { (void)range; (void)odr; }
  void enableAccel() {}
  void enableGyro() {}
  bool readAccel(AccelerometerData &d);
  bool readGyro(GyroscopeData &d);
};
