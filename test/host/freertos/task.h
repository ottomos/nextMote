#pragma once
#include "FreeRTOS.h"
BaseType_t xTaskCreate(void (*entry)(void *), const char *name, unsigned stack,
                      void *arg, unsigned priority, void *handle);
BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *name, unsigned stack,
                                   void *arg, unsigned priority, void *handle, int core);
int xPortGetCoreID(void);
TickType_t xTaskGetTickCount(void);
void vTaskDelayUntil(TickType_t *last, TickType_t period);
