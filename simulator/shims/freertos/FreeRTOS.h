// PC/SDL 模拟器桩：FreeRTOS 任务/信号量/临界区。
// 任务 = std::thread（detached）；信号量 = mutex+condvar 的二元信号量；
// 临界区 = 全局递归锁的指针句柄；tick = 1ms。
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef std::thread *TaskHandle_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0xFFFFFFFFu
#define portTICK_PERIOD_MS 1
#define configMAX_PRIORITIES 25

// ---- 信号量 ----
struct SimSemaphore {
  std::mutex m;
  std::condition_variable cv;
  int count = 0;
};
typedef SimSemaphore *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateBinary();
SemaphoreHandle_t xSemaphoreCreateMutex();
void xSemaphoreGive(SemaphoreHandle_t s);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks);

// ---- 任务 ----
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack,
                                   void *arg, UBaseType_t prio, TaskHandle_t *handle,
                                   BaseType_t core);
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                       UBaseType_t prio, TaskHandle_t *handle);
void vTaskDelete(TaskHandle_t task);   // 模拟器上任务自然返回，这里只做登记
void vTaskDelay(TickType_t ticks);
void taskYIELD();

// ---- 临界区：portMUX_TYPE 是一个持有全局递归锁指针的聚合 ----
struct SimPortMux {
  std::recursive_mutex *m;
};
std::recursive_mutex *sim_portmux();
#define portMUX_TYPE SimPortMux
#define portMUX_INITIALIZER_UNLOCKED {sim_portmux()}
#define portENTER_CRITICAL(mux) (mux)->m->lock()
#define portEXIT_CRITICAL(mux) (mux)->m->unlock()

// freertos/task.h、freertos/semphr.h、freertos/queue.h 都是转发
