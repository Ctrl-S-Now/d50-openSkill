/*
 * font_asset.c — 把 font_assets 分区内存映射为 LVGL 字体。
 * 字库主体留在 Flash，仅按需解码字形，不占用 RAM。
 */
#include "font_asset.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "ui_font_bin.h"

static const char *tag = "font_asset";
static lv_font_t *font_24;
static esp_partition_mmap_handle_t font_mmap_handle;

esp_err_t font_asset_init(void)
{
    if (font_24 != NULL)
    {
        return ESP_OK;
    }
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "font_assets");
    if (partition == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }
    const void *mapped = NULL;
    esp_err_t result = esp_partition_mmap(partition, 0U, partition->size,
                                          ESP_PARTITION_MMAP_DATA, &mapped,
                                          &font_mmap_handle);
    if (result != ESP_OK)
    {
        return result;
    }
    font_24 = ui_font_bin_create(mapped, partition->size);
    if (font_24 == NULL)
    {
        esp_partition_munmap(font_mmap_handle);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(tag, "24 px font mapped (%lu bytes)", (unsigned long)partition->size);
    return ESP_OK;
}

const lv_font_t *font_asset_get_24(void)
{
    return font_24;
}
