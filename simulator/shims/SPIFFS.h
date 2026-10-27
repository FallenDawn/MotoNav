// PC/SDL 模拟器桩：SPIFFS → simulator/spiffs/ 目录
#pragma once
#include "Arduino.h"

#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

class File {
public:
  File() {}
  File(void *impl);
  ~File();
  operator bool() const;
  uint32_t size();
  size_t read(void *buf, size_t len);
  size_t write(const void *buf, size_t len);
  void flush();
  void close();

private:
  void *impl = nullptr;   // std::FILE*
};

class FS {
public:
  bool begin(bool formatOnFail = false, const char *basePath = nullptr);
  File open(const char *path, const char *mode = FILE_READ);
  bool remove(const char *path);
  bool rename(const char *from, const char *to);
  bool exists(const char *path);
};
extern FS SPIFFS;
