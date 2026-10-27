#pragma once
#include "../esp_shim.h"

typedef int i2s_port_t;
#define I2S_NUM_0 0
#define ESP_INTR_FLAG_LEVEL1 1
typedef int i2s_mode_t;
typedef int i2s_comm_format_t;
typedef int i2s_channel_fmt_t;
typedef int i2s_bits_per_sample_t;
#define I2S_MODE_MASTER 1
#define I2S_MODE_TX 2
#define I2S_BITS_PER_SAMPLE_16BIT 16
#define I2S_CHANNEL_FMT_ONLY_LEFT 1
#define I2S_COMM_FORMAT_STAND_I2S 1

typedef struct {
  int mode;
  int sample_rate;
  int bits_per_sample;
  int channel_format;
  int communication_format;
  int intr_alloc_flags;
  int dma_buf_count;
  int dma_buf_len;
  bool use_apll;
  bool tx_desc_auto_clear;
  int fixed_mclk;
} i2s_config_t;

typedef struct {
  int mck_io_num, bck_io_num, ws_io_num, data_out_num, data_in_num;
} i2s_pin_config_t;

esp_err_t i2s_driver_install(i2s_port_t port, const i2s_config_t *cfg, int queueSize, void *queue);
esp_err_t i2s_driver_uninstall(i2s_port_t port);
esp_err_t i2s_set_pin(i2s_port_t port, const i2s_pin_config_t *pins);
esp_err_t i2s_write(i2s_port_t port, const void *src, size_t size, size_t *bytesWritten, uint32_t timeout);
