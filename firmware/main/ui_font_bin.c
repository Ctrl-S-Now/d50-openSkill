/*
 * 文件：ui_font_bin.c
 * 描述：直接查询内存映射的 LVGL BIN 字体，并按需解码单个字形。
 */

#include "ui_font_bin.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#define UI_FONT_BIN_HEAD_MIN_SIZE            (48U)
#define UI_FONT_BIN_CMAP_HEADER_SIZE         (16U)
#define UI_FONT_BIN_CMAP_TABLE_MIN_SIZE      (12U)
#define UI_FONT_BIN_LOCA_TABLE_MIN_SIZE      (12U)
#define UI_FONT_BIN_GLYPH_TABLE_MIN_SIZE     (8U)
#define UI_FONT_BIN_KERN_TABLE_MIN_SIZE      (16U)
#define UI_FONT_BIN_MAX_BOX_WIDTH            (64U)
#define UI_FONT_BIN_EXPECTED_SIZE_PX         (24U)
#define UI_FONT_BIN_EXPECTED_BPP             (4U)
#define UI_FONT_BIN_COMPRESSED_PREFILTER     (1U)
#define UI_FONT_BIN_COMPRESSED_PLAIN         (2U)

enum ui_font_bin_cmap_format
{
    UI_FONT_BIN_CMAP_FORMAT0_FULL = 0,
    UI_FONT_BIN_CMAP_SPARSE_FULL,
    UI_FONT_BIN_CMAP_FORMAT0_TINY,
    UI_FONT_BIN_CMAP_SPARSE_TINY,
};

enum ui_font_bin_rle_state
{
    UI_FONT_BIN_RLE_SINGLE = 0,
    UI_FONT_BIN_RLE_REPEATED,
    UI_FONT_BIN_RLE_COUNTER,
};

struct ui_font_bin_context
{
    lv_font_t font;
    const uint8_t *data;
    size_t size;
    size_t cmap_offset;
    size_t cmap_length;
    size_t loca_offset;
    size_t loca_length;
    size_t glyph_offset;
    size_t glyph_length;
    size_t kern_ids_offset;
    size_t kern_values_offset;
    uint32_t cmap_count;
    uint32_t glyph_count;
    uint32_t kern_pair_count;
    uint16_t default_advance_width;
    uint16_t kerning_scale;
    uint8_t index_to_loc_format;
    uint8_t glyph_id_format;
    uint8_t advance_width_format;
    uint8_t xy_bits;
    uint8_t wh_bits;
    uint8_t advance_width_bits;
    uint8_t compression_id;
    uint8_t glyph_header_bits;
};

struct ui_font_bin_bit_reader
{
    const uint8_t *data;
    size_t bit_size;
    size_t bit_position;
    bool valid;
};

struct ui_font_bin_rle_decoder
{
    struct ui_font_bin_bit_reader reader;
    size_t stream_start;
    uint8_t bpp;
    uint8_t previous_value;
    uint8_t count;
    enum ui_font_bin_rle_state state;
};

struct ui_font_bin_glyph_metrics
{
    uint32_t advance_width;
    uint16_t box_width;
    uint16_t box_height;
    int16_t offset_x;
    int16_t offset_y;
};

static const char *tag = "ui_font_bin";

// 从可能未对齐的映射地址读取小端整数。
static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U);
}

static int16_t read_le16_signed(const uint8_t *bytes)
{
    return (int16_t)read_le16(bytes);
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

// 校验 [offset, offset + length) 是否完全落在指定缓冲区内。
static bool range_is_valid(size_t total_size, size_t offset, size_t length)
{
    return offset <= total_size && length <= total_size - offset;
}

// 校验带长度和四字节标签的 BIN 表，并返回表总长度。
static bool read_table(const struct ui_font_bin_context *context,
                       size_t offset, const char label[4], size_t minimum_length,
                       size_t *length)
{
    if (!range_is_valid(context->size, offset, 8U))
    {
        return false;
    }

    size_t table_length = read_le32(&context->data[offset]);
    if (table_length < minimum_length ||
        !range_is_valid(context->size, offset, table_length) ||
        memcmp(&context->data[offset + 4U], label, 4U) != 0)
    {
        return false;
    }

    *length = table_length;
    return true;
}

// 读取位流中的高位优先字段，任何越界都会使读取器进入无效状态。
static uint32_t read_bits(struct ui_font_bin_bit_reader *reader, uint8_t bit_count)
{
    uint32_t value = 0U;

    if (!reader->valid || bit_count > 32U ||
        reader->bit_position > reader->bit_size ||
        bit_count > reader->bit_size - reader->bit_position)
    {
        reader->valid = false;
        return 0U;
    }

    for (uint8_t bit = 0U; bit < bit_count; bit++)
    {
        size_t position = reader->bit_position++;
        uint8_t byte_value = reader->data[position >> 3U];
        uint8_t bit_value = (byte_value >> (7U - (position & 7U))) & 1U;
        value = (value << 1U) | bit_value;
    }
    return value;
}

static int32_t read_bits_signed(struct ui_font_bin_bit_reader *reader,
                                uint8_t bit_count)
{
    uint32_t value = read_bits(reader, bit_count);
    if (!reader->valid || bit_count == 0U)
    {
        return 0;
    }

    uint32_t sign_bit = 1U << (bit_count - 1U);
    if ((value & sign_bit) != 0U)
    {
        value |= UINT32_MAX << bit_count;
    }
    return (int32_t)value;
}

// 读取一个 cmap 子表头，调用者需先保证索引有效。
static const uint8_t *get_cmap_header(const struct ui_font_bin_context *context,
                                      uint32_t index)
{
    return &context->data[context->cmap_offset +
                          UI_FONT_BIN_CMAP_TABLE_MIN_SIZE +
                          (size_t)index * UI_FONT_BIN_CMAP_HEADER_SIZE];
}

// 校验全部 cmap 子表及其变长数据，避免运行期读取损坏的偏移。
static bool validate_cmaps(const struct ui_font_bin_context *context)
{
    uint32_t previous_start = 0U;

    for (uint32_t index = 0U; index < context->cmap_count; index++)
    {
        const uint8_t *header = get_cmap_header(context, index);
        uint32_t data_offset = read_le32(&header[0]);
        uint32_t range_start = read_le32(&header[4]);
        uint16_t range_length = read_le16(&header[8]);
        uint16_t entry_count = read_le16(&header[12]);
        uint8_t format = header[14];
        size_t data_length = 0U;

        if (range_length == 0U || (index > 0U && range_start <= previous_start))
        {
            return false;
        }
        previous_start = range_start;

        switch (format)
        {
            case UI_FONT_BIN_CMAP_FORMAT0_FULL:
                if (entry_count < range_length)
                {
                    return false;
                }
                data_length = entry_count;
                break;

            case UI_FONT_BIN_CMAP_SPARSE_FULL:
                data_length = (size_t)entry_count * 4U;
                break;

            case UI_FONT_BIN_CMAP_FORMAT0_TINY:
                data_length = 0U;
                break;

            case UI_FONT_BIN_CMAP_SPARSE_TINY:
                data_length = (size_t)entry_count * 2U;
                break;

            default:
                return false;
        }

        if (data_offset > context->cmap_length ||
            data_length > context->cmap_length - data_offset)
        {
            return false;
        }
    }
    return true;
}

// 顺序校验 loca 表，启动时只访问紧凑索引，不读取全部字形数据。
static bool validate_glyph_locations(const struct ui_font_bin_context *context)
{
    size_t entry_size = context->index_to_loc_format == 0U ? 2U : 4U;
    size_t entries_offset = context->loca_offset + UI_FONT_BIN_LOCA_TABLE_MIN_SIZE;
    uint32_t previous_offset = 0U;

    for (uint32_t glyph_id = 0U; glyph_id < context->glyph_count; glyph_id++)
    {
        const uint8_t *entry = &context->data[entries_offset +
                                              (size_t)glyph_id * entry_size];
        uint32_t offset = entry_size == 2U ? read_le16(entry) : read_le32(entry);
        if (offset < UI_FONT_BIN_GLYPH_TABLE_MIN_SIZE ||
            offset < previous_offset || offset > context->glyph_length)
        {
            return false;
        }
        previous_offset = offset;
    }
    return true;
}

// 取得指定字形在 glyf 表内的原始记录范围。
static bool get_glyph_record(const struct ui_font_bin_context *context,
                             uint32_t glyph_id, const uint8_t **record,
                             size_t *record_length)
{
    if (glyph_id >= context->glyph_count)
    {
        return false;
    }

    size_t entry_size = context->index_to_loc_format == 0U ? 2U : 4U;
    size_t entry_offset = context->loca_offset + UI_FONT_BIN_LOCA_TABLE_MIN_SIZE +
                          (size_t)glyph_id * entry_size;
    uint32_t current_offset = entry_size == 2U ?
                              read_le16(&context->data[entry_offset]) :
                              read_le32(&context->data[entry_offset]);
    uint32_t next_offset = (uint32_t)context->glyph_length;

    if (glyph_id + 1U < context->glyph_count)
    {
        entry_offset += entry_size;
        next_offset = entry_size == 2U ?
                      read_le16(&context->data[entry_offset]) :
                      read_le32(&context->data[entry_offset]);
    }
    if (current_offset > next_offset || next_offset > context->glyph_length)
    {
        return false;
    }

    *record = &context->data[context->glyph_offset + current_offset];
    *record_length = (size_t)(next_offset - current_offset);
    return true;
}

// 从字形记录开头解析紧凑度量信息，不访问后续压缩位图。
static bool read_glyph_metrics(const struct ui_font_bin_context *context,
                               uint32_t glyph_id,
                               struct ui_font_bin_glyph_metrics *metrics)
{
    const uint8_t *record;
    size_t record_length;
    if (!get_glyph_record(context, glyph_id, &record, &record_length) ||
        (size_t)context->glyph_header_bits > record_length * 8U)
    {
        return false;
    }

    struct ui_font_bin_bit_reader reader = {
        .data = record,
        .bit_size = record_length * 8U,
        .valid = true,
    };

    uint32_t advance_width = context->advance_width_bits == 0U ?
                             context->default_advance_width :
                             read_bits(&reader, context->advance_width_bits);
    int32_t offset_x = read_bits_signed(&reader, context->xy_bits);
    int32_t offset_y = read_bits_signed(&reader, context->xy_bits);
    uint32_t box_width = read_bits(&reader, context->wh_bits);
    uint32_t box_height = read_bits(&reader, context->wh_bits);
    if (!reader.valid || box_width >= UI_FONT_BIN_MAX_BOX_WIDTH ||
        box_height >= UI_FONT_BIN_MAX_BOX_WIDTH)
    {
        return false;
    }

    if (context->advance_width_format == 0U)
    {
        advance_width *= 16U;
    }
    if (advance_width > UINT16_MAX ||
        offset_x < INT16_MIN || offset_x > INT16_MAX ||
        offset_y < INT16_MIN || offset_y > INT16_MAX)
    {
        return false;
    }

    metrics->advance_width = advance_width;
    metrics->box_width = (uint16_t)box_width;
    metrics->box_height = (uint16_t)box_height;
    metrics->offset_x = (int16_t)offset_x;
    metrics->offset_y = (int16_t)offset_y;
    return true;
}

// 在已排序的 uint16 列表中查找相对 Unicode 编码。
static bool find_sparse_entry(const uint8_t *data, uint16_t entry_count,
                              uint16_t relative_codepoint, uint16_t *entry_index)
{
    uint32_t low = 0U;
    uint32_t high = entry_count;

    while (low < high)
    {
        uint32_t middle = low + (high - low) / 2U;
        uint16_t value = read_le16(&data[middle * 2U]);
        if (value < relative_codepoint)
        {
            low = middle + 1U;
        }
        else
        {
            high = middle;
        }
    }

    if (low >= entry_count || read_le16(&data[low * 2U]) != relative_codepoint)
    {
        return false;
    }
    *entry_index = (uint16_t)low;
    return true;
}

// 将 Unicode 编码映射为 BIN 内部字形编号。
static uint32_t get_glyph_id(const struct ui_font_bin_context *context,
                             uint32_t codepoint)
{
    if (codepoint == 0U)
    {
        return 0U;
    }

    uint32_t low = 0U;
    uint32_t high = context->cmap_count;
    while (low < high)
    {
        uint32_t middle = low + (high - low) / 2U;
        uint32_t range_start = read_le32(&get_cmap_header(context, middle)[4]);
        if (range_start <= codepoint)
        {
            low = middle + 1U;
        }
        else
        {
            high = middle;
        }
    }
    if (low == 0U)
    {
        return 0U;
    }

    const uint8_t *header = get_cmap_header(context, low - 1U);
    uint32_t range_start = read_le32(&header[4]);
    uint16_t range_length = read_le16(&header[8]);
    uint32_t relative_codepoint = codepoint - range_start;
    if (relative_codepoint >= range_length)
    {
        return 0U;
    }

    uint16_t glyph_id_start = read_le16(&header[10]);
    uint16_t entry_count = read_le16(&header[12]);
    uint8_t format = header[14];
    const uint8_t *subtable_data = &context->data[context->cmap_offset +
                                                   read_le32(&header[0])];
    uint32_t glyph_id = 0U;

    if (format == UI_FONT_BIN_CMAP_FORMAT0_TINY)
    {
        glyph_id = glyph_id_start + relative_codepoint;
    }
    else if (format == UI_FONT_BIN_CMAP_FORMAT0_FULL)
    {
        uint8_t glyph_offset = subtable_data[relative_codepoint];
        if (glyph_offset == 0U && relative_codepoint != 0U)
        {
            return 0U;
        }
        glyph_id = glyph_id_start + glyph_offset;
    }
    else
    {
        uint16_t entry_index;
        if (relative_codepoint > UINT16_MAX ||
            !find_sparse_entry(subtable_data, entry_count,
                               (uint16_t)relative_codepoint, &entry_index))
        {
            return 0U;
        }
        if (format == UI_FONT_BIN_CMAP_SPARSE_TINY)
        {
            glyph_id = glyph_id_start + entry_index;
        }
        else
        {
            size_t offsets_start = (size_t)entry_count * 2U;
            glyph_id = glyph_id_start +
                       read_le16(&subtable_data[offsets_start +
                                                (size_t)entry_index * 2U]);
        }
    }

    return glyph_id < context->glyph_count ? glyph_id : 0U;
}

// 从排序的 kerning pair 表中按字形编号二分查询间距值。
static int8_t get_kerning_value(const struct ui_font_bin_context *context,
                                uint32_t left_glyph, uint32_t right_glyph)
{
    if (context->kern_pair_count == 0U)
    {
        return 0;
    }

    size_t pair_size = context->glyph_id_format == 0U ? 2U : 4U;
    uint32_t low = 0U;
    uint32_t high = context->kern_pair_count;
    while (low < high)
    {
        uint32_t middle = low + (high - low) / 2U;
        const uint8_t *pair = &context->data[context->kern_ids_offset +
                                             (size_t)middle * pair_size];
        uint32_t stored_left = pair_size == 2U ? pair[0] : read_le16(pair);
        uint32_t stored_right = pair_size == 2U ? pair[1] : read_le16(&pair[2]);

        if (stored_left < left_glyph ||
            (stored_left == left_glyph && stored_right < right_glyph))
        {
            low = middle + 1U;
        }
        else
        {
            high = middle;
        }
    }
    if (low >= context->kern_pair_count)
    {
        return 0;
    }

    const uint8_t *pair = &context->data[context->kern_ids_offset +
                                         (size_t)low * pair_size];
    uint32_t stored_left = pair_size == 2U ? pair[0] : read_le16(pair);
    uint32_t stored_right = pair_size == 2U ? pair[1] : read_le16(&pair[2]);
    if (stored_left != left_glyph || stored_right != right_glyph)
    {
        return 0;
    }
    return (int8_t)context->data[context->kern_values_offset + low];
}

// 解码一个 RLE 像素值；状态机与 lv_font_conv 的压缩格式一致。
static bool rle_next(struct ui_font_bin_rle_decoder *decoder, uint8_t *result)
{
    uint8_t value = 0U;
    uint8_t output = 0U;

    if (decoder->state == UI_FONT_BIN_RLE_SINGLE)
    {
        size_t symbol_position = decoder->reader.bit_position;
        output = (uint8_t)read_bits(&decoder->reader, decoder->bpp);
        if (symbol_position != decoder->stream_start &&
            decoder->previous_value == output)
        {
            decoder->count = 0U;
            decoder->state = UI_FONT_BIN_RLE_REPEATED;
        }
        decoder->previous_value = output;
    }
    else if (decoder->state == UI_FONT_BIN_RLE_REPEATED)
    {
        value = (uint8_t)read_bits(&decoder->reader, 1U);
        decoder->count++;
        if (value == 1U)
        {
            output = decoder->previous_value;
            if (decoder->count == 11U)
            {
                decoder->count = (uint8_t)read_bits(&decoder->reader, 6U);
                if (decoder->count != 0U)
                {
                    decoder->state = UI_FONT_BIN_RLE_COUNTER;
                }
                else
                {
                    output = (uint8_t)read_bits(&decoder->reader, decoder->bpp);
                    decoder->previous_value = output;
                    decoder->state = UI_FONT_BIN_RLE_SINGLE;
                }
            }
        }
        else
        {
            output = (uint8_t)read_bits(&decoder->reader, decoder->bpp);
            decoder->previous_value = output;
            decoder->state = UI_FONT_BIN_RLE_SINGLE;
        }
    }
    else
    {
        output = decoder->previous_value;
        decoder->count--;
        if (decoder->count == 0U)
        {
            output = (uint8_t)read_bits(&decoder->reader, decoder->bpp);
            decoder->previous_value = output;
            decoder->state = UI_FONT_BIN_RLE_SINGLE;
        }
    }

    if (!decoder->reader.valid)
    {
        return false;
    }
    *result = output;
    return true;
}

// LVGL 字形描述回调：只读取当前字形度量及相邻字形 kerning。
static bool get_glyph_description(const lv_font_t *font,
                                  lv_font_glyph_dsc_t *description,
                                  uint32_t codepoint, uint32_t next_codepoint)
{
    const struct ui_font_bin_context *context = font->dsc;
    bool is_tab = codepoint == '\t';
    if (is_tab)
    {
        codepoint = ' ';
    }

    uint32_t glyph_id = get_glyph_id(context, codepoint);
    if (glyph_id == 0U)
    {
        return false;
    }

    struct ui_font_bin_glyph_metrics metrics;
    if (!read_glyph_metrics(context, glyph_id, &metrics))
    {
        return false;
    }

    int32_t kerning = 0;
    uint32_t next_glyph_id = get_glyph_id(context, next_codepoint);
    if (next_glyph_id != 0U)
    {
        kerning = ((int32_t)get_kerning_value(context, glyph_id, next_glyph_id) *
                   context->kerning_scale) >> 4;
    }

    uint32_t advance_width = metrics.advance_width;
    if (is_tab)
    {
        advance_width *= 2U;
    }
    int32_t adjusted_advance = (int32_t)advance_width + kerning;
    if (adjusted_advance < 0)
    {
        adjusted_advance = 0;
    }

    description->adv_w = (uint16_t)((adjusted_advance + 8) >> 4);
    description->box_w = is_tab ? metrics.box_width * 2U : metrics.box_width;
    description->box_h = metrics.box_height;
    description->ofs_x = metrics.offset_x;
    description->ofs_y = metrics.offset_y;
    description->stride = 0U;
    description->format = LV_FONT_GLYPH_FORMAT_A4;
    description->is_placeholder = false;
    description->gid.index = glyph_id;
    return true;
}

// LVGL 位图回调：仅将本次绘制所需的一个压缩字形展开为 A8。
static const void *get_glyph_bitmap(lv_font_glyph_dsc_t *description,
                                    lv_draw_buf_t *draw_buffer)
{
    const lv_font_t *font = description->resolved_font;
    const struct ui_font_bin_context *context = font->dsc;
    const uint8_t *record;
    size_t record_length;
    struct ui_font_bin_glyph_metrics metrics;

    if (description->req_raw_bitmap || draw_buffer == NULL ||
        draw_buffer->data == NULL ||
        !get_glyph_record(context, description->gid.index, &record,
                          &record_length) ||
        !read_glyph_metrics(context, description->gid.index, &metrics))
    {
        return NULL;
    }
    if (metrics.box_width == 0U || metrics.box_height == 0U)
    {
        return NULL;
    }

    uint32_t stride = lv_draw_buf_width_to_stride(metrics.box_width,
                                                   LV_COLOR_FORMAT_A8);
    size_t output_size = (size_t)stride * metrics.box_height;
    if (output_size > draw_buffer->data_size ||
        (size_t)context->glyph_header_bits > record_length * 8U)
    {
        return NULL;
    }

    struct ui_font_bin_rle_decoder decoder = {
        .reader = {
            .data = record,
            .bit_size = record_length * 8U,
            .bit_position = context->glyph_header_bits,
            .valid = true,
        },
        .stream_start = context->glyph_header_bits,
        .bpp = UI_FONT_BIN_EXPECTED_BPP,
        .state = UI_FONT_BIN_RLE_SINGLE,
    };
    uint8_t previous_line[UI_FONT_BIN_MAX_BOX_WIDTH] = {0};
    memset(draw_buffer->data, 0, output_size);

    for (uint16_t y = 0U; y < metrics.box_height; y++)
    {
        uint8_t *output = &draw_buffer->data[(size_t)y * stride];
        for (uint16_t x = 0U; x < metrics.box_width; x++)
        {
            uint8_t value;
            if (!rle_next(&decoder, &value))
            {
                return NULL;
            }
            if (context->compression_id == UI_FONT_BIN_COMPRESSED_PREFILTER && y > 0U)
            {
                value ^= previous_line[x];
            }
            previous_line[x] = value;
            output[x] = value * 17U;
        }
    }

    lv_draw_buf_flush_cache(draw_buffer, NULL);
    return draw_buffer;
}

// 解析可选的排序 kerning pair 表；不支持的格式按无 kerning 处理。
static bool parse_kerning(struct ui_font_bin_context *context, size_t offset)
{
    size_t kern_length;
    if (!read_table(context, offset, "kern", UI_FONT_BIN_KERN_TABLE_MIN_SIZE,
                    &kern_length))
    {
        return false;
    }
    if (context->data[offset + 8U] != 0U)
    {
        ESP_LOGW(tag, "Unsupported kerning table format: %u",
                 context->data[offset + 8U]);
        return true;
    }

    uint32_t pair_count = read_le32(&context->data[offset + 12U]);
    size_t pair_size = context->glyph_id_format == 0U ? 2U : 4U;
    size_t ids_size = (size_t)pair_count * pair_size;
    size_t values_offset = UI_FONT_BIN_KERN_TABLE_MIN_SIZE + ids_size;
    if (values_offset > kern_length || pair_count > kern_length - values_offset)
    {
        return false;
    }

    context->kern_pair_count = pair_count;
    context->kern_ids_offset = offset + UI_FONT_BIN_KERN_TABLE_MIN_SIZE;
    context->kern_values_offset = offset + values_offset;
    return true;
}

// 解析固定头和三张必需表，成功后上下文可被绘制回调只读访问。
static bool parse_font(struct ui_font_bin_context *context)
{
    size_t header_length;
    if (!read_table(context, 0U, "head", UI_FONT_BIN_HEAD_MIN_SIZE,
                    &header_length))
    {
        return false;
    }

    const uint8_t *header = context->data;
    uint32_t version = read_le32(&header[8]);
    uint16_t table_count = read_le16(&header[12]);
    uint16_t font_size = read_le16(&header[14]);
    uint16_t ascent = read_le16(&header[16]);
    int16_t descent = read_le16_signed(&header[18]);
    uint8_t bits_per_pixel = header[37];
    int16_t underline_position = read_le16_signed(&header[44]);
    uint16_t underline_thickness = read_le16(&header[46]);

    context->default_advance_width = read_le16(&header[30]);
    context->kerning_scale = read_le16(&header[32]);
    context->index_to_loc_format = header[34];
    context->glyph_id_format = header[35];
    context->advance_width_format = header[36];
    context->xy_bits = header[38];
    context->wh_bits = header[39];
    context->advance_width_bits = header[40];
    context->compression_id = header[41];

    if (version != 1U || table_count < 3U ||
        font_size != UI_FONT_BIN_EXPECTED_SIZE_PX ||
        bits_per_pixel != UI_FONT_BIN_EXPECTED_BPP ||
        context->index_to_loc_format > 1U ||
        context->glyph_id_format > 1U ||
        context->advance_width_format > 1U ||
        context->xy_bits == 0U || context->xy_bits >= 32U ||
        context->wh_bits == 0U || context->wh_bits > 6U ||
        context->advance_width_bits >= 32U ||
        (context->compression_id != UI_FONT_BIN_COMPRESSED_PREFILTER &&
         context->compression_id != UI_FONT_BIN_COMPRESSED_PLAIN) ||
        header[42] > LV_FONT_SUBPX_BOTH ||
        underline_position < INT8_MIN || underline_position > INT8_MAX ||
        underline_thickness > INT8_MAX)
    {
        return false;
    }

    uint32_t header_bits = context->advance_width_bits +
                           2U * context->xy_bits + 2U * context->wh_bits;
    if (header_bits > UINT8_MAX)
    {
        return false;
    }
    context->glyph_header_bits = (uint8_t)header_bits;

    context->cmap_offset = header_length;
    if (!read_table(context, context->cmap_offset, "cmap",
                    UI_FONT_BIN_CMAP_TABLE_MIN_SIZE, &context->cmap_length))
    {
        return false;
    }
    context->cmap_count = read_le32(&context->data[context->cmap_offset + 8U]);
    size_t cmap_headers_size = (size_t)context->cmap_count *
                               UI_FONT_BIN_CMAP_HEADER_SIZE;
    if (context->cmap_count == 0U ||
        cmap_headers_size > context->cmap_length - UI_FONT_BIN_CMAP_TABLE_MIN_SIZE ||
        !validate_cmaps(context))
    {
        return false;
    }

    context->loca_offset = context->cmap_offset + context->cmap_length;
    if (!read_table(context, context->loca_offset, "loca",
                    UI_FONT_BIN_LOCA_TABLE_MIN_SIZE, &context->loca_length))
    {
        return false;
    }
    context->glyph_count = read_le32(&context->data[context->loca_offset + 8U]);
    size_t loca_entry_size = context->index_to_loc_format == 0U ? 2U : 4U;
    size_t loca_entries_size = (size_t)context->glyph_count * loca_entry_size;
    if (context->glyph_count < 2U || context->glyph_count > UINT16_MAX + 1U ||
        loca_entries_size > context->loca_length - UI_FONT_BIN_LOCA_TABLE_MIN_SIZE)
    {
        return false;
    }

    context->glyph_offset = context->loca_offset + context->loca_length;
    if (!read_table(context, context->glyph_offset, "glyf",
                    UI_FONT_BIN_GLYPH_TABLE_MIN_SIZE, &context->glyph_length) ||
        !validate_glyph_locations(context))
    {
        return false;
    }

    if (table_count >= 4U)
    {
        size_t kern_offset = context->glyph_offset + context->glyph_length;
        if (!parse_kerning(context, kern_offset))
        {
            return false;
        }
    }

    context->font.get_glyph_dsc = get_glyph_description;
    context->font.get_glyph_bitmap = get_glyph_bitmap;
    context->font.line_height = (int32_t)ascent - descent;
    context->font.base_line = -(int32_t)descent;
    context->font.subpx = header[42];
    context->font.kerning = context->kern_pair_count > 0U ?
                            LV_FONT_KERNING_NORMAL : LV_FONT_KERNING_NONE;
    context->font.static_bitmap = 0U;
    context->font.underline_position = (int8_t)underline_position;
    context->font.underline_thickness = (int8_t)underline_thickness;
    context->font.dsc = context;
    context->font.fallback = NULL;
    return true;
}

lv_font_t *ui_font_bin_create(const uint8_t *data, size_t size)
{
    if (data == NULL || size < UI_FONT_BIN_HEAD_MIN_SIZE)
    {
        return NULL;
    }

    struct ui_font_bin_context *context = calloc(1U, sizeof(*context));
    if (context == NULL)
    {
        return NULL;
    }
    context->data = data;
    context->size = size;

    if (!parse_font(context))
    {
        ESP_LOGE(tag, "Invalid or unsupported LVGL BIN font");
        free(context);
        return NULL;
    }

    ESP_LOGI(tag, "Font index ready: %lu glyphs, %lu cmap tables, %lu kern pairs",
             (unsigned long)context->glyph_count,
             (unsigned long)context->cmap_count,
             (unsigned long)context->kern_pair_count);
    return &context->font;
}

void ui_font_bin_destroy(lv_font_t *font)
{
    free(font);
}
