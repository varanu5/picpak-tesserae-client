#pragma once
#include <stdint.h>
typedef struct { int mode; uint64_t pin_bit_mask; } gpio_config_t;
#define GPIO_MODE_OUTPUT 1
#define GPIO_MODE_INPUT 2
int gpio_config(const gpio_config_t *c);
int gpio_set_level(int pin,int level);
int gpio_get_level(int pin);
