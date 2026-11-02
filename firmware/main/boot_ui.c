/*
 * boot_ui.c — 开机画面。四屏依次出现，每屏只讲一件事，信息不堆在一起。
 *
 * 屏幕逻辑尺寸 640×480，背景 #222222。所有 LVGL 对象操作都在 bsp_display_lock 保护下进行。
 */
#include "boot_ui.h"

#include <stdio.h>
#include <string.h>

#include "bsp.h"
#include "esp_check.h"
#include "font_asset.h"
#include "lvgl.h"
#include "dino_game.h"
#include "open_config.h"

LV_IMAGE_DECLARE(ui_image_boot_logo);

#define COLOR_BG        (0x000000U)
#define COLOR_TEXT      (0xFFFFFFU)
#define COLOR_MUTED     (0x8A8A8AU)
#define COLOR_ACCENT    (0x00C2FFU)
#define COLOR_OK        (0x6DFF50U)
#define COLOR_FAIL      (0xFF5050U)

#define SCREEN_W        (640)
#define FADE_MS         (450U)

static const char *tag = "boot_ui";

static const char *const item_names[BOOT_ITEM_COUNT] = {
    [BOOT_ITEM_DISPLAY] = "显示屏", [BOOT_ITEM_LED] = "灯效",
    [BOOT_ITEM_PSRAM] = "PSRAM",    [BOOT_ITEM_SPEAKER] = "扬声器",
    [BOOT_ITEM_MIC] = "麦克风",     [BOOT_ITEM_INPUT] = "按键旋钮",
    [BOOT_ITEM_USB] = "USB",        [BOOT_ITEM_WIFI] = "Wi-Fi",
};

static lv_obj_t *current_screen;   // 当前最上面一屏
static lv_obj_t *logo_image;
static lv_obj_t *status_labels[BOOT_ITEM_COUNT];   // 第 3 屏
static lv_obj_t *prov_line1;                        // 配网页
static lv_obj_t *prov_line2;
static char prov_text1[64];
static char prov_text2[64];
static const lv_font_t *text_font;

/*
 * 淡入淡出：背景是纯黑，所以直接把“颜色”在 黑 ↔ 目标色 之间插值，
 * 不依赖对象整体透明度（图层透明度在本屏上渲染不可靠）。
 *   target: 文字颜色 / 背景色 / 图片重着色 三选一。
 */
enum fade_kind { FADE_TEXT, FADE_BG, FADE_IMAGE };

static void color_fade_exec(lv_anim_t *anim, int32_t value)
{
    uint32_t packed = (uint32_t)(uintptr_t)lv_anim_get_user_data(anim);
    lv_color_t target = lv_color_hex(packed & 0xFFFFFFU);
    lv_color_t mixed = lv_color_mix(target, lv_color_black(), (lv_opa_t)value);
    lv_obj_t *obj = (lv_obj_t *)anim->var;
    switch ((enum fade_kind)(packed >> 24))
    {
    case FADE_TEXT:
        lv_obj_set_style_text_color(obj, mixed, 0);
        break;
    case FADE_BG:
        lv_obj_set_style_bg_color(obj, mixed, 0);
        break;
    default:
        lv_obj_set_style_image_recolor(obj, mixed, 0);
        break;
    }
}

// from/to 取 0（黑）…255（目标色）。调用前对象应已设为目标色，这里先压到 from。
static void fade_color(lv_obj_t *obj, enum fade_kind kind, uint32_t target_rgb,
                       int32_t from, int32_t to, uint32_t delay_ms, uint32_t time_ms)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_custom_exec_cb(&anim, color_fade_exec);
    lv_anim_set_user_data(&anim, (void *)(uintptr_t)(((uint32_t)kind << 24) | (target_rgb & 0xFFFFFFU)));
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_delay(&anim, delay_ms);
    lv_anim_set_duration(&anim, time_ms);
    lv_anim_start(&anim);
    // 动画有 delay 时，起点之前对象仍是目标色，先手动压到 from。
    color_fade_exec(&anim, from);
}

// 全宽居中的文字行。
static lv_obj_t *centered_label(lv_obj_t *parent, uint32_t color, int y, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, 0, y);
    lv_obj_set_width(label, SCREEN_W);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, text_font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

// 指定位置和宽度的文字行。
static lv_obj_t *label_at(lv_obj_t *parent, uint32_t color, int x, int y, int width,
                          lv_text_align_t align, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_style_text_font(label, text_font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *make_screen(void)
{
    lv_obj_t *screen = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, SCREEN_W, 480);
    lv_obj_set_pos(screen, 0, 0);
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_HIDDEN);   // 建好内容后由 present() 一次性显示
    return screen;
}

// 新一屏先隐藏着建好，再一次性换上并删除旧屏，整个切换只重绘一次（调用方持有显示锁）。
// 不再做全屏淡入：整屏半透明混合带宽大，容易闪烁。
static void present(lv_obj_t *screen)
{
    lv_obj_t *old = current_screen;
    current_screen = screen;
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_HIDDEN);
    if (old != NULL)
    {
        lv_obj_delete(old);
    }
}

static void use_font(void)
{
    const lv_font_t *dynamic_font = font_asset_get_24();
    text_font = dynamic_font != NULL ? dynamic_font : &ui_font_noto_sans_sc_bold_24;
}

/* ---------- 第 1 屏：标志 ---------- */

esp_err_t boot_ui_show_logo(void)
{
    ESP_RETURN_ON_ERROR(bsp_display_lock(-1), tag, "lock display");
    use_font();
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(COLOR_BG), 0);
    current_screen = make_screen();
    lv_obj_remove_flag(current_screen, LV_OBJ_FLAG_HIDDEN);
    logo_image = lv_image_create(current_screen);
    lv_image_set_src(logo_image, &ui_image_boot_logo);
    lv_obj_set_style_image_recolor(logo_image, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(logo_image, LV_OPA_COVER, 0);
    lv_obj_center(logo_image);
    lv_refr_now(NULL);
    bsp_display_unlock();
    return ESP_OK;
}

/* ---------- 第 2 屏：欢迎语 ---------- */

esp_err_t boot_ui_show_welcome(void)
{
    ESP_RETURN_ON_ERROR(bsp_display_lock(-1), tag, "lock display");
    lv_obj_t *screen = make_screen();
    lv_obj_t *line1 = centered_label(screen, COLOR_TEXT, 150, "欢迎来到 D50");
    lv_obj_t *line2 = centered_label(screen, COLOR_MUTED, 204, "一台属于你的、可以随意改造的设备");
    lv_obj_t *rule = lv_obj_create(screen);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 48, 3);
    lv_obj_set_pos(rule, (SCREEN_W - 48) / 2, 262);
    lv_obj_set_style_bg_color(rule, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_t *line3 = centered_label(screen, COLOR_ACCENT, 292, "与我们一起共创，构建开源生态");
    present(screen);
    // 三句话依次缓缓亮起。
    fade_color(line1, FADE_TEXT, COLOR_TEXT, 0, 255, 600, 600);
    fade_color(line2, FADE_TEXT, COLOR_MUTED, 0, 255, 1300, 600);
    fade_color(rule, FADE_BG, COLOR_ACCENT, 0, 255, 2000, 400);
    fade_color(line3, FADE_TEXT, COLOR_ACCENT, 0, 255, 2200, 600);
    bsp_display_unlock();
    return ESP_OK;
}

/* ---------- 第 3 屏：自检清单 ---------- */

esp_err_t boot_ui_show_selftest(void)
{
    ESP_RETURN_ON_ERROR(bsp_display_lock(-1), tag, "lock display");
    lv_obj_t *screen = make_screen();
    label_at(screen, COLOR_MUTED, 0, 28, SCREEN_W, LV_TEXT_ALIGN_CENTER, "外设自检");
    for (int item = 0; item < BOOT_ITEM_COUNT; item++)
    {
        int y = 88 + item * 44;
        label_at(screen, COLOR_TEXT, 150, y, 170, LV_TEXT_ALIGN_LEFT, item_names[item]);
        status_labels[item] = label_at(screen, COLOR_MUTED, 320, y, 170, LV_TEXT_ALIGN_RIGHT, "···");
    }
    present(screen);
    bsp_display_unlock();
    return ESP_OK;
}

void boot_ui_set_item(enum boot_item item, bool pending, bool ok, const char *detail)
{
    if (item >= BOOT_ITEM_COUNT || status_labels[item] == NULL ||
        bsp_display_lock(-1) != ESP_OK)
    {
        return;
    }
    char text[48];
    uint32_t color = COLOR_MUTED;
    if (pending)
    {
        snprintf(text, sizeof(text), "···");
    }
    else if (ok)
    {
        snprintf(text, sizeof(text), "OK%s%s", detail != NULL && detail[0] != '\0' ? "  " : "",
                 detail != NULL ? detail : "");
        color = COLOR_OK;
    }
    else
    {
        snprintf(text, sizeof(text), "FAIL");
        color = COLOR_FAIL;
    }
    lv_label_set_text(status_labels[item], text);
    lv_obj_set_style_text_color(status_labels[item], lv_color_hex(color), 0);
    bsp_display_unlock();
}

/* ---------- 第 4 屏：首页 ---------- */

esp_err_t boot_ui_show_home(const bool ok[BOOT_ITEM_COUNT])
{
    ESP_RETURN_ON_ERROR(bsp_display_lock(-1), tag, "lock display");
    lv_obj_t *screen = make_screen();

    // 上半屏：自检总结（一行）+ 二维码 + 社区信息。下半屏：K2 小恐龙。
    char summary[160];
    int failed = 0;
    size_t used = 0U;
    char names[96] = "";
    for (int item = 0; item < BOOT_ITEM_COUNT; item++)
    {
        if (!ok[item])
        {
            failed++;
            used += (size_t)snprintf(names + used, sizeof(names) - used, "%s%s",
                                     used > 0U ? "、" : "", item_names[item]);
        }
    }
    uint32_t summary_color = COLOR_OK;
    if (failed == 0)
    {
        snprintf(summary, sizeof(summary), "外设自检全部通过");
    }
    else
    {
        snprintf(summary, sizeof(summary), "%d 项未通过：%s", failed, names);
        summary_color = COLOR_FAIL;
    }
    if (failed > 0)   // 全部通过时不占位；有未通过项才在顶部用红字提示
    {
        label_at(screen, summary_color, 0, 16, SCREEN_W, LV_TEXT_ALIGN_CENTER, summary);
    }

    // 黑底灰白码（70% 白，黑色外圈即静区）。
    lv_obj_t *qr = lv_qrcode_create(screen);
    lv_qrcode_set_size(qr, 136);
    lv_qrcode_set_dark_color(qr, lv_color_hex(0xB3B3B3));
    lv_qrcode_set_light_color(qr, lv_color_black());
    lv_qrcode_update(qr, D50_COMMUNITY_URL, strlen(D50_COMMUNITY_URL));
    lv_obj_set_pos(qr, 90, 72);
    lv_obj_set_style_border_color(qr, lv_color_black(), 0);
    lv_obj_set_style_border_width(qr, 8, 0);

    label_at(screen, COLOR_TEXT, 290, 82, 300, LV_TEXT_ALIGN_LEFT, "加入开源社区");
    label_at(screen, COLOR_TEXT, 290, 126, 300, LV_TEXT_ALIGN_LEFT, D50_COMMUNITY_URL_TEXT);
    label_at(screen, COLOR_MUTED, 290, 170, 300, LV_TEXT_ALIGN_LEFT, "扫码或访问网址");

    // 下半屏小游戏（只在这一页）。
    dino_game_create(screen, 0, 262, SCREEN_W, 218, text_font);

    for (int item = 0; item < BOOT_ITEM_COUNT; item++)
    {
        status_labels[item] = NULL;   // 第 3 屏随之删除
    }
    present(screen);
    bsp_display_unlock();
    return ESP_OK;
}

/* ---------- Wi-Fi 配网页 ---------- */

esp_err_t boot_ui_show_provision(void)
{
    ESP_RETURN_ON_ERROR(bsp_display_lock(-1), tag, "lock display");
    lv_obj_t *screen = make_screen();
    centered_label(screen, COLOR_ACCENT, 40, "Wi-Fi 配网");
    centered_label(screen, COLOR_MUTED, 110, "用手机连接下面这个热点");
    prov_line1 = centered_label(screen, COLOR_TEXT, 170, prov_text1);
    prov_line2 = centered_label(screen, COLOR_TEXT, 216, prov_text2);
    centered_label(screen, COLOR_MUTED, 306, "然后在网页里填写 Wi-Fi 名称和密码");
    centered_label(screen, COLOR_MUTED, 350, "没有自动弹出？浏览器打开 192.168.4.1");
    present(screen);
    bsp_display_unlock();
    return ESP_OK;
}

void boot_ui_set_provision(const char *line1, const char *line2)
{
    snprintf(prov_text1, sizeof(prov_text1), "%s", line1 != NULL ? line1 : "");
    snprintf(prov_text2, sizeof(prov_text2), "%s", line2 != NULL ? line2 : "");
    if (prov_line1 == NULL || bsp_display_lock(200) != ESP_OK)
    {
        return;
    }
    lv_label_set_text(prov_line1, prov_text1);
    lv_label_set_text(prov_line2, prov_text2);
    bsp_display_unlock();
}
