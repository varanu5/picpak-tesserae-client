#pragma once
#include <assert.h>
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) do { assert(*(lock) == 0); *(lock) = 1; } while (0)
#define portEXIT_CRITICAL(lock) do { assert(*(lock) == 1); *(lock) = 0; } while (0)
