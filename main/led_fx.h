#pragma once
#include <stdint.h>

#include "bsp.h"
#include "esp_err.h"

/* 五颗按键灯的渐亮渐暗效果：目标亮度变化时，当前亮度平滑逼近（亮得快、暗得慢）。 */
esp_err_t led_fx_start(void);
/* 持续点亮某颗灯（长按期间），release 后渐暗。index 0…4。 */
void led_fx_hold(uint8_t index);
void led_fx_release_all(void);
/* 点一下：快速渐亮，保持 hold_ms 后渐暗。 */
void led_fx_pulse(uint8_t index, uint32_t hold_ms);
/* 给某颗灯设置颜色和“常亮底光”亮度（0 = 平时熄灭）。例如提示用户该按哪颗键。 */
void led_fx_set_glow(uint8_t index, struct bsp_led_color color, uint8_t floor_level);
