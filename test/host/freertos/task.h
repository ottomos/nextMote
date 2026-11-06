#pragma once
#include "FreeRTOS.h"
BaseType_t xTaskCreate(void (*entry)(void *), const char *name, unsigned stack,
                      void *arg, unsigned priority, void *handle);
TickType_t xTaskGetTickCount(void);
void vTaskDelayUntil(TickType_t *last, TickType_t period);
