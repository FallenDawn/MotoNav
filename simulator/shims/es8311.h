// PC/SDL 模拟器桩：ES8311 codec（模拟器无声，create 失败让开机提示音跳过）
#pragma once
#include "esp_shim.h"
#include <stdbool.h>

typedef void *es8311_handle_t;
#define ES8311_ADDRRES_0 0
#define I2C_NUM_0 0
#define ES8311_RESOLUTION_16 3
#define ES8311_RESOLUTION_32 4

typedef struct {
  bool mclk_inverted;
  bool sclk_inverted;
  bool mclk_from_mclk_pin;
  uint32_t mclk_frequency;
  uint32_t sample_frequency;
} es8311_clock_config_t;

es8311_handle_t es8311_create(int port, int addr);
esp_err_t es8311_init(es8311_handle_t handle, const es8311_clock_config_t *clock, int resIn,
                      int resOut);
esp_err_t es8311_voice_volume_set(es8311_handle_t handle, int volume, void *reserved);
esp_err_t es8311_microphone_config(es8311_handle_t handle, bool microphone);
esp_err_t es8311_sample_frequency_config(es8311_handle_t handle, int sampleFrequencyMclk,
                                         int sampleFrequency);
