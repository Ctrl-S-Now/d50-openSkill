/*
 * dino_game.c — 小恐龙跑酷（K2 跳）。
 *
 * 一个 16 ms 的 LVGL 定时器驱动：读 K2 → 更新物理 → 移动障碍 → 碰撞检测 → 更新画面。
 * 全部用矩形拼图，不依赖图片资源。最高分存在设备闪存（NVS）里，断电不丢。
 */
#include "dino_game.h"

#include <stdio.h>

#include "bsp.h"
#include "esp_random.h"
#include "led_fx.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sound.h"

#define KEY_JUMP            (1U)       // K2
#define FRAME_MS            (16U)
#define GROUND_Y            (150)      // 地面线，区域内坐标
#define DINO_X              (70)
#define DINO_W              (36)
#define DINO_H              (40)
#define GRAVITY             (0.95f)
#define JUMP_SPEED          (13.2f)
#define START_SPEED         (6.0f)
#define MAX_SPEED           (13.0f)
#define SPEED_GAIN          (0.0025f)
#define OBSTACLE_POOL       (3)
#define RESTART_LOCK_FRAMES (40)

#define COLOR_FG            (0xB3B3B3U)   // 70% 白，和二维码一致
#define COLOR_DIM           (0x6A6A6AU)
#define ORANGE_TAG          "#FF8000 发光按键#"   // LVGL recolor 语法：#RRGGBB 文字#

enum game_state { STATE_IDLE, STATE_RUN, STATE_OVER };

struct obstacle
{
    lv_obj_t *obj;
    float x;
    int w, h;
    bool active;
};

static lv_obj_t *area;
static lv_obj_t *dino;
static lv_obj_t *leg_a, *leg_b;
static lv_obj_t *score_label, *msg_label;
static struct obstacle obstacles[OBSTACLE_POOL];
static enum game_state state = STATE_IDLE;
static bool key_was_down;
static float dino_lift;       // 离地高度（像素，>=0）
static float dino_velocity;
static float speed;
static float spawn_in;        // 距离下一个障碍还要滚动多少像素
static float score, best;
static uint32_t frame, lock_frames;

static lv_obj_t *rect(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static void build_dino(void)
{
    dino = lv_obj_create(area);
    lv_obj_remove_style_all(dino);
    lv_obj_set_size(dino, DINO_W, DINO_H);
    lv_obj_clear_flag(dino, LV_OBJ_FLAG_CLICKABLE);
    rect(dino, 0, 16, 10, 6, COLOR_FG, 2);       // 尾巴
    rect(dino, 6, 12, 24, 20, COLOR_FG, 4);      // 身体
    rect(dino, 16, 0, 20, 16, COLOR_FG, 4);      // 头
    rect(dino, 28, 4, 3, 3, 0x000000, 1);        // 眼睛
    rect(dino, 32, 12, 4, 3, 0x000000, 1);       // 嘴缝
    leg_a = rect(dino, 10, 32, 6, 8, COLOR_FG, 1);
    leg_b = rect(dino, 22, 32, 6, 8, COLOR_FG, 1);
}

static void place_dino(void)
{
    lv_obj_set_pos(dino, DINO_X, GROUND_Y - DINO_H - (int)dino_lift);
}

// 仙人掌：树干 + 左右各一条弯臂。
static void build_obstacle(struct obstacle *obstacle, int w, int h)
{
    lv_obj_clean(obstacle->obj);
    lv_obj_set_size(obstacle->obj, w, h);
    rect(obstacle->obj, 0, 0, w, h, COLOR_FG, 3);
    rect(obstacle->obj, -7, h * 2 / 5, 7, 4, COLOR_FG, 1);
    rect(obstacle->obj, -7, h * 2 / 5 - 9, 4, 13, COLOR_FG, 1);
    rect(obstacle->obj, w, h / 3, 7, 4, COLOR_FG, 1);
    rect(obstacle->obj, w + 3, h / 3 - 9, 4, 13, COLOR_FG, 1);
    obstacle->w = w;
    obstacle->h = h;
}

static void text_opa_exec(void *obj, int32_t value)
{
    lv_obj_set_style_text_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

// 底部唯一的一行提示：dim 为真时文字（含橙色关键词）渐变到 50% 亮度，否则渐变到 100%。
static void set_message(const char *text, uint32_t color, bool dim)
{
    lv_label_set_text(msg_label, text);
    lv_obj_set_style_text_color(msg_label, lv_color_hex(color), 0);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, msg_label);
    lv_anim_set_exec_cb(&anim, text_opa_exec);
    lv_anim_set_values(&anim, lv_obj_get_style_text_opa(msg_label, 0), dim ? LV_OPA_50 : LV_OPA_COVER);
    lv_anim_set_duration(&anim, 350);
    lv_anim_start(&anim);
}

// 最高分读写（设备闪存 NVS）。写入只发生在刷新纪录时，次数很少。
static void load_best(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        nvs_flash_erase();
        result = nvs_flash_init();
    }
    nvs_handle_t handle;
    int32_t saved = 0;
    if (result == ESP_OK && nvs_open("d50game", NVS_READONLY, &handle) == ESP_OK)
    {
        nvs_get_i32(handle, "best", &saved);
        nvs_close(handle);
    }
    best = saved > 0 ? (float)saved : 0.0f;
}

static void save_best(void)
{
    nvs_handle_t handle;
    if (nvs_open("d50game", NVS_READWRITE, &handle) == ESP_OK)
    {
        nvs_set_i32(handle, "best", (int32_t)best);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void update_score_label(void)
{
    char text[40];
    snprintf(text, sizeof(text), "HI %05d   %05d", (int)best, (int)score);
    lv_label_set_text(score_label, text);
}

static void reset_run(void)
{
    for (int index = 0; index < OBSTACLE_POOL; index++)
    {
        obstacles[index].active = false;
        lv_obj_add_flag(obstacles[index].obj, LV_OBJ_FLAG_HIDDEN);
    }
    dino_lift = 0.0f;
    dino_velocity = 0.0f;
    speed = START_SPEED;
    spawn_in = 300.0f;
    score = 0.0f;
    frame = 0U;
    place_dino();
    update_score_label();
}

static void spawn_obstacle(void)
{
    for (int index = 0; index < OBSTACLE_POOL; index++)
    {
        struct obstacle *obstacle = &obstacles[index];
        if (obstacle->active)
        {
            continue;
        }
        int w = 14 + (int)(esp_random() % 3U) * 6;        // 14 / 20 / 26
        int h = 32 + (int)(esp_random() % 3U) * 10;       // 32 / 42 / 52
        build_obstacle(obstacle, w, h);
        obstacle->x = (float)lv_obj_get_width(area) + 10.0f;
        obstacle->active = true;
        lv_obj_clear_flag(obstacle->obj, LV_OBJ_FLAG_HIDDEN);
        return;
    }
}

static bool collides(const struct obstacle *obstacle)
{
    // 碰撞盒各向内缩几个像素，更宽容。
    float dino_left = DINO_X + 6, dino_right = DINO_X + DINO_W - 4;
    float dino_bottom = GROUND_Y - dino_lift - 3;
    float dino_top = GROUND_Y - dino_lift - DINO_H + 4;
    float obstacle_left = obstacle->x + 2, obstacle_right = obstacle->x + obstacle->w - 2;
    float obstacle_top = GROUND_Y - obstacle->h + 2;
    return dino_right > obstacle_left && dino_left < obstacle_right && dino_bottom > obstacle_top &&
           dino_top < GROUND_Y;
}

static void game_over(void)
{
    state = STATE_OVER;
    lock_frames = RESTART_LOCK_FRAMES;
    if (score > best)
    {
        best = score;
        save_best();
    }
    update_score_label();
    set_message("游戏结束 · 按 " ORANGE_TAG " 再来一次", 0xFFFFFFU, false);
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    bool down = bsp_key_is_pressed(KEY_JUMP);
    bool pressed_edge = down && !key_was_down;
    key_was_down = down;
    if (lock_frames > 0U)
    {
        lock_frames--;
    }

    if (state != STATE_RUN)
    {
        if (pressed_edge && lock_frames == 0U)
        {
            reset_run();
            state = STATE_RUN;
            set_message("按 " ORANGE_TAG " 跳跃", COLOR_FG, true);   // 开始后整行变暗 50%
        }
        return;
    }

    // 起跳：只在地面上响应。
    if (pressed_edge && dino_lift <= 0.0f)
    {
        dino_velocity = JUMP_SPEED;
        led_fx_pulse(KEY_JUMP, 90);
        (void)sound_play_click();
    }
    dino_lift += dino_velocity;
    dino_velocity -= GRAVITY;
    if (dino_lift <= 0.0f)
    {
        dino_lift = 0.0f;
        dino_velocity = 0.0f;
    }
    place_dino();

    // 跑步：脚在地面时左右脚交替；腾空时两脚并拢。
    bool step = dino_lift <= 0.0f && (frame / 6U) % 2U == 0U;
    lv_obj_set_height(leg_a, step ? 8 : 4);
    lv_obj_set_height(leg_b, step ? 4 : 8);
    frame++;

    // 障碍物：向左滚动，间隔随速度拉大，保证来得及跳。
    speed = speed + SPEED_GAIN > MAX_SPEED ? MAX_SPEED : speed + SPEED_GAIN;
    spawn_in -= speed;
    if (spawn_in <= 0.0f)
    {
        spawn_obstacle();
        spawn_in = speed * 30.0f + 90.0f + (float)(esp_random() % 180U);
    }
    for (int index = 0; index < OBSTACLE_POOL; index++)
    {
        struct obstacle *obstacle = &obstacles[index];
        if (!obstacle->active)
        {
            continue;
        }
        obstacle->x -= speed;
        if (obstacle->x < -(float)obstacle->w - 20.0f)
        {
            obstacle->active = false;
            lv_obj_add_flag(obstacle->obj, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_set_pos(obstacle->obj, (int)obstacle->x, GROUND_Y - obstacle->h);
        if (collides(obstacle))
        {
            game_over();
            return;
        }
    }

    score += speed * 0.04f;
    if (frame % 6U == 0U)
    {
        update_score_label();
    }
}

void dino_game_create(lv_obj_t *parent, int x, int y, int w, int h, const lv_font_t *font)
{
    area = lv_obj_create(parent);
    lv_obj_remove_style_all(area);
    lv_obj_set_pos(area, x, y);
    lv_obj_set_size(area, w, h);
    lv_obj_set_style_clip_corner(area, false, 0);
    lv_obj_clear_flag(area, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // 地面线；障碍物先建好放进池里，之后只切换显示/位置。
    rect(area, 40, GROUND_Y, w - 80, 2, COLOR_DIM, 0);
    for (int index = 0; index < OBSTACLE_POOL; index++)
    {
        obstacles[index].obj = lv_obj_create(area);
        lv_obj_remove_style_all(obstacles[index].obj);
        lv_obj_add_flag(obstacles[index].obj, LV_OBJ_FLAG_HIDDEN);
        obstacles[index].active = false;
    }
    build_dino();

    score_label = lv_label_create(area);
    lv_obj_set_width(score_label, 280);
    lv_obj_set_pos(score_label, w - 280 - 40, 8);
    lv_obj_set_style_text_align(score_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(score_label, font, 0);
    lv_obj_set_style_text_color(score_label, lv_color_hex(COLOR_DIM), 0);

    msg_label = lv_label_create(area);
    lv_obj_set_width(msg_label, w);
    lv_obj_set_pos(msg_label, 0, GROUND_Y + 24);
    lv_obj_set_style_text_align(msg_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(msg_label, font, 0);
    lv_label_set_recolor(msg_label, true);

    load_best();
    state = STATE_IDLE;
    key_was_down = bsp_key_is_pressed(KEY_JUMP);
    lock_frames = 0U;
    reset_run();
    set_message("按 " ORANGE_TAG " 开始", COLOR_FG, false);
    // 用户并不知道哪颗是 K2：让 K2 的灯发橙光，提示"按发光按键"。
    led_fx_set_glow(KEY_JUMP, (struct bsp_led_color){.red = 200, .green = 80, .blue = 0}, 150);
    lv_timer_create(tick, FRAME_MS, NULL);
}
