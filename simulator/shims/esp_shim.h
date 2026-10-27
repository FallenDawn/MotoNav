// PC/SDL 模拟器桩：esp_err_t 和少量 esp API 共用的小头
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef int esp_err_t;
uint32_t esp_random();
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
