#pragma once
#include <stdint.h>
#define BIT0 1u
#define BIT1 2u
#define BIT2 4u
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(uint32_t ticks);
