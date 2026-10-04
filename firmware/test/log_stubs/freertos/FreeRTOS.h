#pragma once
#include <assert.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) do { assert(*(m) == 0); *(m) = 1; } while (0)
#define portEXIT_CRITICAL(m) do { assert(*(m) == 1); *(m) = 0; } while (0)
#define portENTER_CRITICAL_SAFE(m) portENTER_CRITICAL(m)
#define portEXIT_CRITICAL_SAFE(m) portEXIT_CRITICAL(m)
