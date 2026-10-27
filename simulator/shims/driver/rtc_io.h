#pragma once
#include "../esp_shim.h"

typedef int gpio_num_t;
#define GPIO_NUM_0 0
#define GPIO_NUM_2 2
#define GPIO_NUM_4 4
#define GPIO_NUM_5 5
#define RTC_GPIO_MODE_INPUT_ONLY 0
#define RTC_GPIO_MODE_OUTPUT_ONLY 1

static inline void rtc_gpio_init(gpio_num_t n) { (void)n; }
static inline void rtc_gpio_set_direction(gpio_num_t n, int mode) { (void)n; (void)mode; }
static inline void rtc_gpio_set_level(gpio_num_t n, int level) { (void)n; (void)level; }
static inline int rtc_gpio_get_level(gpio_num_t n) { (void)n; return 0; }
static inline void rtc_gpio_hold_en(gpio_num_t n) { (void)n; }
static inline void rtc_gpio_hold_dis(gpio_num_t n) { (void)n; }
static inline void rtc_gpio_deinit(gpio_num_t n) { (void)n; }
static inline void rtc_gpio_pullup_en(gpio_num_t n) { (void)n; }
static inline void rtc_gpio_pulldown_dis(gpio_num_t n) { (void)n; }
static inline void rtc_gpio_pulldown_en(gpio_num_t n) { (void)n; }
static inline void rtc_gpio_pullup_dis(gpio_num_t n) { (void)n; }
