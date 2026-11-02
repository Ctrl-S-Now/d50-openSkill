#pragma once
#include "esp_err.h"

/* 开机提示音（原 D50 提示音，约 2.2 s）。异步播放，立即返回。 */
esp_err_t sound_play_boot(void);
/* 短促点击音（约 0.1 s），旋钮按下等场景使用。正在播放时忽略。 */
esp_err_t sound_play_click(void);
