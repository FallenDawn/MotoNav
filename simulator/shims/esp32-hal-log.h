// WString.cpp 在 PC 上编译时不需要 ESP 日志
#pragma once
#define log_w(...) do {} while (0)
#define log_e(...) do {} while (0)
#define log_i(...) do {} while (0)
#define log_d(...) do {} while (0)
#define log_v(...) do {} while (0)
