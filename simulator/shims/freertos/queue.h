#pragma once
#include "FreeRTOS.h"
// 模拟器队列空实现（AppleMediaClient 引用但连不上 Apple 设备就永远不会用）
typedef void *QueueHandle_t;
static inline QueueHandle_t xQueueCreate(uint32_t len, uint32_t itemSize) { (void)len; (void)itemSize; return nullptr; }
static inline BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks) { (void)q; (void)item; (void)ticks; return pdFALSE; }
static inline BaseType_t xQueueSendToFront(QueueHandle_t q, const void *item, TickType_t ticks) { (void)q; (void)item; (void)ticks; return pdFALSE; }
static inline BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks) { (void)q; (void)item; (void)ticks; return pdFALSE; }
static inline UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) { (void)q; return 0; }
