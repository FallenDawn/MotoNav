// PC/SDL 模拟器桩：NVS。数据存在 simulator 目录下的 prefs.txt（key=value 文本）。
#pragma once
#include "Arduino.h"

class Preferences {
public:
  bool begin(const char *name, bool readOnly = false, const char *partition = nullptr);
  void end();
  int getInt(const char *key, int defaultValue = 0);
  bool getBool(const char *key, bool defaultValue = false);
  size_t putInt(const char *key, int value);
  size_t putBool(const char *key, bool value);
  size_t putString(const char *key, const String &value);
  String getString(const char *key, const String &defaultValue = String());
  bool remove(const char *key);
  void clear();
};
