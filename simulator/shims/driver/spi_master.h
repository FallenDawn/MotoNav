// PC/SDL 模拟器桩：SPI 主驱动。queue_trans 就是"往 SDL 物理帧缓冲写像素"，
// 快速刷屏 fastPush() 的每段像素经这里落到 360x360 的显示缓冲。
#pragma once
#include "../esp_shim.h"

#define SPI2_HOST 1
#define SPI3_HOST 2
#define SPI_CLK_SRC_DEFAULT 0
#define SPI_DEVICE_HALFDUPLEX (1 << 1)
#define SPI_TRANS_MODE_QIO (1 << 2)
#define SPI_TRANS_VARIABLE_CMD (1 << 3)
#define SPI_TRANS_VARIABLE_ADDR (1 << 4)
#define SPI_TRANS_VARIABLE_DUMMY (1 << 5)
#define SPI_TRANS_USE_RXDATA (1 << 6)
#define SPI_TRANS_USE_TXDATA (1 << 7)

typedef struct spi_transaction_t {
  uint32_t flags;
  uint16_t cmd;
  uint64_t addr;
  size_t length;        // 发送位数
  size_t rxlength;      // 接收位数
  void *user;
  const void *tx_buffer;
  void *rx_buffer;
  uint8_t rx_data[4];
} spi_transaction_t;

typedef struct {
  spi_transaction_t base;
  size_t dummy_bits;
} spi_transaction_ext_t;

typedef void *spi_device_handle_t;

typedef struct {
  int command_bits;
  int address_bits;
  int dummy_bits;
  int mode;
  int clock_source;
  int clock_speed_hz;
  int spics_io_num;
  int flags;
  int queue_size;
} spi_device_interface_config_t;

esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *config,
                             spi_device_handle_t *handle);
esp_err_t spi_bus_remove_device(spi_device_handle_t handle);
esp_err_t spi_device_acquire_bus(spi_device_handle_t device, uint32_t timeout);
void spi_device_release_bus(spi_device_handle_t device);
esp_err_t spi_device_queue_trans(spi_device_handle_t handle, spi_transaction_t *trans,
                                 uint32_t timeout);
esp_err_t spi_device_get_trans_result(spi_device_handle_t handle, spi_transaction_t **ret_trans,
                                      uint32_t timeout);
esp_err_t spi_device_polling_transmit(spi_device_handle_t handle, spi_transaction_t *trans);
esp_err_t spi_device_polling_start(spi_device_handle_t handle, spi_transaction_t *trans,
                                   uint32_t timeout);
