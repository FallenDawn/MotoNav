// PC/SDL 模拟器桩：NimBLE-Arduino 库的 BLE 类空壳。
// 模拟器上蓝牙栈不存在：init 后所有操作都是空转，连接状态由 UDP/键盘控制。
#pragma once
#include "Arduino.h"
#include "esp_shim.h"
#include <stdint.h>
#include "host/ble_gatt.h"

// HID 用途常量（BLEAdvertising::setAppearance）
#define GENERIC_HID 964

class BLEUUID {
public:
  BLEUUID() {}
  BLEUUID(const char *s) { (void)s; }
  BLEUUID(const String &s) { (void)s; }
  BLEUUID(uint16_t u) { (void)u; }
  String toString() const { return String(""); }
};

class BLECharacteristic;
class BLEServer;
class BLECharacteristicCallbacks {
public:
  enum Status { SUCCESS_NOTIFY = 0, ERROR_NOTIFY = 1 };
  virtual ~BLECharacteristicCallbacks() {}
  virtual void onWrite(BLECharacteristic *c) { (void)c; }
  virtual void onRead(BLECharacteristic *c) { (void)c; }
  virtual void onStatus(BLECharacteristic *c, Status status, uint32_t arg) { (void)c; (void)status; (void)arg; }
};

class BLEDescriptor {
public:
  BLEDescriptor() {}
};

class BLE2902 : public BLEDescriptor {
public:
  BLE2902() {}
};

class BLECharacteristic {
public:
  static const int PROPERTY_READ = 0x02;
  static const int PROPERTY_WRITE = 0x08;
  static const int PROPERTY_WRITE_NR = 0x10;
  static const int PROPERTY_NOTIFY = 0x20;
  static const int PROPERTY_INDICATE = 0x40;

  void setValue(const uint8_t *data, size_t len) { (void)data; (void)len; }
  void setValue(const String &s) { (void)s; }
  size_t getLength() { return 0; }
  uint8_t *getData() { return nullptr; }
  void setCallbacks(BLECharacteristicCallbacks *cb) { (void)cb; }
  void addDescriptor(BLEDescriptor *d) { (void)d; }
  void notify() {}
  String getValue() { return String(""); }
};

class BLEService;
class BLEServerCallbacks {
public:
  virtual ~BLEServerCallbacks() {}
  virtual void onConnect(BLEServer *s) { (void)s; }
  virtual void onDisconnect(BLEServer *s) { (void)s; }
};

class BLEService {
public:
  BLECharacteristic *createCharacteristic(const char *uuid, int properties) {
    (void)uuid; (void)properties; return new BLECharacteristic();
  }
  BLECharacteristic *createCharacteristic(const BLEUUID &uuid, int properties) {
    (void)uuid; (void)properties; return new BLECharacteristic();
  }
  void start() {}
  void stop() {}
  BLECharacteristic *getCharacteristic(const char *uuid) { (void)uuid; static BLECharacteristic c; return &c; }
  BLECharacteristic *getCharacteristic(const BLEUUID &uuid) { (void)uuid; static BLECharacteristic c; return &c; }
};

class BLEAdvertising {
public:
  void addServiceUUID(const char *uuid) { (void)uuid; }
  void addServiceUUID(BLEUUID uuid) { (void)uuid; }
  void setAppearance(uint16_t appearance) { (void)appearance; }
  void setScanResponse(bool on) { (void)on; }
  void start() {}
  void stop() {}
};

class BLEServer {
public:
  BLEService *createService(const char *uuid) { (void)uuid; return new BLEService(); }
  BLEService *createService(const BLEUUID &uuid) { (void)uuid; return new BLEService(); }
  void setCallbacks(BLEServerCallbacks *cb) { (void)cb; }
  BLEAdvertising *getAdvertising() { static BLEAdvertising adv; return &adv; }
  void startAdvertising() {}
};

// ---- HID（HidMediaKeys.h 空转用）----
class BLEHIDDevice {
public:
  BLEHIDDevice(BLEServer *server) { (void)server; }
  void reportMap(uint8_t *map, uint16_t len) { (void)map; (void)len; }
  void startServices() {}
  BLECharacteristic *manufacturer() { static BLECharacteristic c; return &c; }
  BLEService *deviceInfo() { static BLEService s; return &s; }
  void pnp(uint8_t sig, uint16_t vid, uint16_t pid, uint16_t version) {
    (void)sig; (void)vid; (void)pid; (void)version;
  }
  void hidInfo(uint8_t country, uint8_t flags) { (void)country; (void)flags; }
  BLECharacteristic *inputReport(uint8_t reportId) { (void)reportId; static BLECharacteristic c; return &c; }
  BLECharacteristic *outputReport(uint8_t reportId) { (void)reportId; static BLECharacteristic c; return &c; }
  BLECharacteristic *getBootInput() { static BLECharacteristic c; return &c; }
  BLECharacteristic *hidControl() { static BLECharacteristic c; return &c; }
  BLECharacteristic *protocolMode() { static BLECharacteristic c; return &c; }
  void setBatteryLevel(uint8_t level) { (void)level; }
};

// ---- 配对安全回调 ----
class BLESecurityCallbacks {
public:
  virtual ~BLESecurityCallbacks() {}
  virtual uint32_t onPassKeyRequest() { return 0; }
  virtual void onPassKeyNotify(uint32_t key) { (void)key; }
  virtual bool onSecurityRequest() { return true; }
  virtual bool onConfirmPIN(uint32_t pin) { (void)pin; return false; }
  virtual bool onAuthorizationRequest(uint16_t connHandle, uint16_t attrHandle, bool isRead) {
    (void)connHandle; (void)attrHandle; (void)isRead; return true;
  }
  virtual void onAuthenticationComplete() {}
};

#define ESP_IO_CAP_OUT 0
#define ESP_IO_CAP_IO 1
#define ESP_IO_CAP_NONE 2

class BLESecurity {
public:
  void setCapability(uint8_t cap) { (void)cap; }
  void setAuthenticationMode(bool bond, bool mitm, bool secure) {
    (void)bond; (void)mitm; (void)secure;
  }
  void setPassKey(bool randomKey) { (void)randomKey; }
  void setPassKey(uint32_t key, uint8_t digits) { (void)key; (void)digits; }
  void regenPassKeyOnConnect(bool regen) { (void)regen; }
};

class BLEDevice {
public:
  static void init(const String &name) { (void)name; }
  static void init(const char *name) { (void)name; }
  static void setMTU(uint16_t mtu) { (void)mtu; }
  static BLEServer *createServer();
  static BLEAdvertising *getAdvertising();
  static void setSecurityCallbacks(BLESecurityCallbacks *cb) { (void)cb; }
  static void setCustomGapHandler(int (*fn)(ble_gap_event *, void *)) { (void)fn; }
  static void startAdvertising();
  static void deinit() {}
};
