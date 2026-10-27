// PC/SDL 模拟器桩
#pragma once
#include "esp_shim.h"
#include <time.h>

// esp_system.h
int esp_reset_reason();
void esp_restart();

// esp_sleep.h
typedef enum {
  ESP_SLEEP_WAKEUP_UNDEFINED = 0,
  ESP_SLEEP_WAKEUP_ALL = 255,
} esp_sleep_source_t;
esp_sleep_source_t esp_sleep_get_wakeup_cause();
void esp_deep_sleep_start();          // 模拟器：打印并退出
esp_err_t esp_deep_sleep(uint64_t us);
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us);
esp_err_t esp_sleep_enable_ext0_wakeup(int gpio, int level);
esp_err_t esp_sleep_disable_wakeup_source(int source);

// esp_mac.h
#define ESP_MAC_WIFI_STA 0
#define ESP_MAC_BT 2
esp_err_t esp_read_mac(uint8_t *mac, int type);
esp_err_t esp_efuse_mac_get_default(uint8_t *mac);
