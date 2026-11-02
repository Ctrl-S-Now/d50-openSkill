#pragma once
#include "esp_err.h"
#include "lvgl.h"

/* 映射 font_assets 分区里的完整 24 px 字库（含汉字）。 */
esp_err_t font_asset_init(void);
/* 返回字库；未映射成功时返回 NULL，调用方回退到内置字体。 */
const lv_font_t *font_asset_get_24(void);
