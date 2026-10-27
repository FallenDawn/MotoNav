// PC 模拟器桩：progrespace 相关全是空操作（PC 上数据就在内存里）
#pragma once

#ifndef PROGMEM
#define PROGMEM
#endif
#define memcpy_P memcpy
#define strcpy_P strcpy
#define strncpy_P strncpy
#define strcat_P strcat
#define strlen_P strlen
#define strcmp_P strcmp
#define memcmp_P memcmp
#define PSTR(s) (s)
#define PGM_P const char *

static inline uint8_t pgm_read_byte(const void *p) { return *(const uint8_t *)p; }
static inline uint16_t pgm_read_word(const void *p) { return *(const uint16_t *)p; }
static inline uint32_t pgm_read_dword(const void *p) { return *(const uint32_t *)p; }
