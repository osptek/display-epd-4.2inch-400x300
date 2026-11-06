#ifndef PIN_MAP_H
#define PIN_MAP_H

#include "driver/gpio.h"

/* screen — 4.2" E6 300x400 */
#define eink_sda    GPIO_NUM_47
#define eink_clk    GPIO_NUM_38
#define eink_cs     GPIO_NUM_39
#define eink_dc     GPIO_NUM_40
#define eink_rst    GPIO_NUM_41
#define eink_busy   GPIO_NUM_42

#endif /* PIN_MAP_H */
