// SPI 桩实现：queue_trans 就是快速刷屏的数据通道。
// 每帧第一个事务 cmd=0x32（写 RAM 起始），其后所有 tx_buffer 像素按顺序落到 sim_phys。
#include "driver/spi_master.h"
#include "sim_state.h"
#include <cstring>

static int g_spiDeviceSeq = 0;
static size_t g_frameOffset = 0;

esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *config,
                             spi_device_handle_t *handle) {
  (void)host; (void)config;
  *handle = (spi_device_handle_t)(intptr_t)(++g_spiDeviceSeq);
  return ESP_OK;
}
esp_err_t spi_bus_remove_device(spi_device_handle_t handle) { (void)handle; return ESP_OK; }
esp_err_t spi_device_acquire_bus(spi_device_handle_t device, uint32_t timeout) {
  (void)device; (void)timeout; return ESP_OK;
}
void spi_device_release_bus(spi_device_handle_t device) { (void)device; }

esp_err_t spi_device_queue_trans(spi_device_handle_t handle, spi_transaction_t *trans,
                                 uint32_t timeout) {
  (void)handle; (void)timeout;
  if (!trans || !trans->tx_buffer) return ESP_OK;
  if (trans->cmd == 0x32) g_frameOffset = 0;   // 新帧开始
  size_t count = trans->length / 16;
  const uint16_t *src = (const uint16_t *)trans->tx_buffer;
  std::lock_guard<std::mutex> lk(sim_phys_mu);
  for (size_t i = 0; i < count && g_frameOffset < 360 * 360; i++, g_frameOffset++) {
    // fastPush 发给屏幕的是大端（字节交换过），这里换回来
    uint16_t p = src[i];
    sim_phys[g_frameOffset] = (uint16_t)((p >> 8) | (p << 8));
  }
  return ESP_OK;
}
esp_err_t spi_device_get_trans_result(spi_device_handle_t handle, spi_transaction_t **ret_trans,
                                      uint32_t timeout) {
  (void)handle; (void)timeout;
  if (ret_trans) *ret_trans = nullptr;
  return ESP_OK;
}
esp_err_t spi_device_polling_transmit(spi_device_handle_t handle, spi_transaction_t *trans) {
  (void)handle;
  // 面板 ID 读：固件用失败回退分支（readOK=false → variant 1 初始化序列）
  if (trans && (trans->flags & SPI_TRANS_USE_RXDATA)) {
    memset(trans->rx_data, 0, sizeof(trans->rx_data));
  }
  return ESP_OK;
}
esp_err_t spi_device_polling_start(spi_device_handle_t handle, spi_transaction_t *trans,
                                   uint32_t timeout) {
  (void)handle; (void)trans; (void)timeout; return ESP_OK;
}
