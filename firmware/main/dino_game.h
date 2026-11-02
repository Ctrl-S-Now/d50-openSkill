#pragma once
#include "lvgl.h"

/*
 * 谷歌小恐龙风格的跑酷小游戏，用 K2 起跳（K2 的灯会随起跳亮一下）。
 * 只在开源社区页（最后一页）使用。整个游戏就是 dino_game.c 一个文件，不需要可直接删掉。
 *
 * parent: 所在页面；(x, y, w, h): 游戏区域；font: 提示文字用的字体。
 * 必须在持有显示锁时调用；游戏由 LVGL 定时器驱动，之后不需要再管。
 */
void dino_game_create(lv_obj_t *parent, int x, int y, int w, int h, const lv_font_t *font);
