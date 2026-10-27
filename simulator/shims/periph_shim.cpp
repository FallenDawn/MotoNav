// 外设桩实现：触摸 / 电量计 / IMU / Preferences / SPIFFS
#include "TouchDrv.hpp"
#include "GaugeDrv.hpp"
#include "sensor/imu/qmi8658/SensorQMI8658.hpp"
#include "Preferences.h"
#include "SPIFFS.h"
#include "sim_state.h"
#include <cstdio>
#include <string>
#include <unordered_map>
#include <filesystem>
namespace fs = std::filesystem;

// ---- 触摸 ----
bool TouchDrv::begin(TwoWire &w, uint8_t addr, int sda, int scl) {
  (void)w; (void)addr; (void)sda; (void)scl; return true;
}
uint8_t TouchDrv::getPoint(int16_t *x, int16_t *y, uint8_t maxPoints) {
  (void)maxPoints;
  if (!sim_touch_down.load()) return 0;
  if (x) x[0] = (int16_t)sim_touch_x.load();
  if (y) y[0] = (int16_t)sim_touch_y.load();
  return 1;
}

// ---- 电量计：可用 UDP 指令改（!bat <pct> [chg]）----
static int g_batPct = 78;
static bool g_batChg = false;
int GaugeDrv::getVoltage() { return 3300 + g_batPct * 8; }
int GaugeDrv::getStateOfCharge() { return g_batPct; }
bool GaugeDrv::isCharging() { return g_batChg; }
void sim_set_battery(int pct, bool chg) { g_batPct = pct; g_batChg = chg; }

// ---- IMU：静止姿态，UDP 可注入加速度 ----
static float g_ax = 0, g_ay = 0, g_az = 1.0f;   // 静止：重力 +1g
static float g_gx = 0, g_gy = 0, g_gz = 0;
bool SensorQMI8658::begin(TwoWire &w, uint8_t addr, int sda, int scl) {
  (void)w; (void)addr; (void)sda; (void)scl; return true;
}
bool SensorQMI8658::readAccel(AccelerometerData &d) {
  d.mps2.x = g_ax * 9.80665f; d.mps2.y = g_ay * 9.80665f; d.mps2.z = g_az * 9.80665f; return true;
}
bool SensorQMI8658::readGyro(GyroscopeData &d) {
  d.dps.x = g_gx; d.dps.y = g_gy; d.dps.z = g_gz; return true;
}
void sim_set_imu(float ax, float ay, float az, float gx, float gy, float gz) {
  g_ax = ax; g_ay = ay; g_az = az; g_gx = gx; g_gy = gy; g_gz = gz;
}

// ---- Preferences：prefs.txt，每行 key=value ----
static fs::path prefs_path() {
  return fs::path("prefs.txt");
}
static std::unordered_map<std::string, std::string> g_prefs;
static bool g_prefsLoaded = false;
static void prefs_load() {
  g_prefs.clear();
  std::FILE *f = fopen(prefs_path().string().c_str(), "r");
  if (!f) return;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    char *nl = strchr(eq + 1, '\n');
    if (nl) *nl = 0;
    g_prefs[line] = eq + 1;
  }
  fclose(f);
  g_prefsLoaded = true;
}
static void prefs_save() {
  std::FILE *f = fopen(prefs_path().string().c_str(), "w");
  if (!f) return;
  for (auto &kv : g_prefs) fprintf(f, "%s=%s\n", kv.first.c_str(), kv.second.c_str());
  fclose(f);
}
static std::string g_prefsNamespace;
bool Preferences::begin(const char *name, bool readOnly, const char *partition) {
  (void)readOnly; (void)partition;
  if (!g_prefsLoaded) prefs_load();
  g_prefsNamespace = name ? name : "";
  return true;
}
void Preferences::end() { prefs_save(); }
static std::string pkey(const char *key) { return g_prefsNamespace + "/" + key; }
int Preferences::getInt(const char *key, int defaultValue) {
  auto it = g_prefs.find(pkey(key));
  return it == g_prefs.end() ? defaultValue : atoi(it->second.c_str());
}
bool Preferences::getBool(const char *key, bool defaultValue) {
  auto it = g_prefs.find(pkey(key));
  return it == g_prefs.end() ? defaultValue : it->second == "1";
}
size_t Preferences::putInt(const char *key, int value) {
  g_prefs[pkey(key)] = std::to_string(value);
  return 4;
}
size_t Preferences::putBool(const char *key, bool value) {
  g_prefs[pkey(key)] = value ? "1" : "0";
  return 1;
}
size_t Preferences::putString(const char *key, const String &value) {
  g_prefs[pkey(key)] = value.c_str();
  return value.length();
}
String Preferences::getString(const char *key, const String &defaultValue) {
  auto it = g_prefs.find(pkey(key));
  return it == g_prefs.end() ? defaultValue : String(it->second.c_str());
}
bool Preferences::remove(const char *key) { g_prefs.erase(pkey(key)); return true; }
void Preferences::clear() {
  std::string ns = g_prefsNamespace + "/";
  for (auto it = g_prefs.begin(); it != g_prefs.end();) {
    if (it->first.rfind(ns, 0) == 0) it = g_prefs.erase(it); else ++it;
  }
}

// ---- SPIFFS：simulator/spiffs/ 目录 ----
static fs::path spiffs_root = "spiffs";
static fs::path spiffs_map(const char *path) {
  std::string p = path ? path : "";
  if (!p.empty() && p[0] == '/') p = p.substr(1);
  return spiffs_root / p;
}
struct FileImpl { std::FILE *f; };
bool FS::begin(bool formatOnFail, const char *basePath) {
  (void)formatOnFail; (void)basePath;
  std::error_code ec;
  fs::create_directories(spiffs_root, ec);
  return true;
}
File::File(void *impl) : impl(impl) {}
File::~File() { close(); }
File::operator bool() const { return impl != nullptr; }
uint32_t File::size() {
  if (!impl) return 0;
  FILE *f = ((FileImpl *)impl)->f;
  long cur = ftell(f);
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, cur, SEEK_SET);
  return (uint32_t)sz;
}
size_t File::read(void *buf, size_t len) {
  if (!impl) return 0;
  return fread(buf, 1, len, ((FileImpl *)impl)->f);
}
size_t File::write(const void *buf, size_t len) {
  if (!impl) return 0;
  return fwrite(buf, 1, len, ((FileImpl *)impl)->f);
}
void File::flush() {
  if (impl) fflush(((FileImpl *)impl)->f);
}
void File::close() {
  if (impl) { fclose(((FileImpl *)impl)->f); delete (FileImpl *)impl; impl = nullptr; }
}
File FS::open(const char *path, const char *mode) {
  auto p = spiffs_map(path);
  std::error_code ec;
  auto parent = p.parent_path();
  if (strchr(mode, 'w')) fs::create_directories(parent, ec);
  FILE *f = fopen(p.string().c_str(), mode);
  if (!f) return File();
  return File(new FileImpl{f});
}
bool FS::remove(const char *path) {
  std::error_code ec;
  return fs::remove(spiffs_map(path), ec);
}
bool FS::rename(const char *from, const char *to) {
  std::error_code ec;
  fs::rename(spiffs_map(from), spiffs_map(to), ec);
  return !ec;
}
bool FS::exists(const char *path) {
  std::error_code ec;
  return fs::exists(spiffs_map(path), ec);
}
static FS g_spiffs;
FS SPIFFS;   // SPIFFS.h 里的 extern 定义点
