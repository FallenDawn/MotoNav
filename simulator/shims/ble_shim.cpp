// BLE 桩实现
#include "ble_shim.h"

BLEServer *BLEDevice::createServer() { return new BLEServer(); }
void BLEDevice::startAdvertising() {}
BLEAdvertising *BLEDevice::getAdvertising() {
  static BLEAdvertising adv;
  return &adv;
}
