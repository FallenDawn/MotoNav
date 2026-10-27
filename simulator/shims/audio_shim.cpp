// 音频桩实现：es8311 建不出来（create 返回 null）→ 开机提示音静默跳过
#include "es8311.h"
#include "driver/i2s.h"

es8311_handle_t es8311_create(int port, int addr) { (void)port; (void)addr; return nullptr; }
esp_err_t es8311_init(es8311_handle_t h, const es8311_clock_config_t *c, int ri, int ro) {
  (void)h; (void)c; (void)ri; (void)ro; return ESP_FAIL;
}
esp_err_t es8311_voice_volume_set(es8311_handle_t h, int v, void *r) { (void)h; (void)v; (void)r; return ESP_OK; }
esp_err_t es8311_microphone_config(es8311_handle_t h, bool m) { (void)h; (void)m; return ESP_OK; }
esp_err_t es8311_sample_frequency_config(es8311_handle_t h, int m, int s) { (void)h; (void)m; (void)s; return ESP_OK; }

esp_err_t i2s_driver_install(i2s_port_t port, const i2s_config_t *cfg, int queueSize, void *queue) {
  (void)port; (void)cfg; (void)queueSize; (void)queue; return ESP_OK;
}
esp_err_t i2s_driver_uninstall(i2s_port_t port) { (void)port; return ESP_OK; }
esp_err_t i2s_set_pin(i2s_port_t port, const i2s_pin_config_t *pins) { (void)port; (void)pins; return ESP_OK; }
esp_err_t i2s_write(i2s_port_t port, const void *src, size_t size, size_t *bytesWritten, uint32_t timeout) {
  (void)port; (void)src; (void)timeout;
  if (bytesWritten) *bytesWritten = size;
  return ESP_OK;
}
