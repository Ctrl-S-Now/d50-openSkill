/*
 * 文件：bsp_eeprom.c
 * 描述：D50 AD24C02 EEPROM 驱动，提供原始字节读写能力。
 */

#include "bsp.h"

#include <string.h>

#include "bsp_config.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#define EEPROM_I2C_SPEED_HZ                    (100000U)
#define EEPROM_IO_TIMEOUT_MS                   (100)
#define EEPROM_WRITE_TIMEOUT_US                (10000LL)
#define EEPROM_ACK_POLL_INTERVAL_US            (1000U)
#define EEPROM_WRITE_PROTECT_ENABLED           (1)
#define EEPROM_WRITE_PROTECT_DISABLED          (0)

static const char *tag = "bsp_eeprom";
// EEPROM 独占 I2C0，句柄在 BSP 生命周期内保持有效。
static i2c_master_bus_handle_t bus_handle;
static i2c_master_dev_handle_t device_handle;
static bool initialized;

// 检查访问范围，防止读写跨过 EEPROM 容量边界。
static bool eeprom_range_is_valid(uint16_t address, size_t size)
{
    return size > 0U && address < BSP_EEPROM_SIZE_BYTES &&
           size <= BSP_EEPROM_SIZE_BYTES - address;
}

// 轮询器件应答，等待 EEPROM 内部写周期完成。
static esp_err_t eeprom_wait_ready(void)
{
    const int64_t deadline_us = esp_timer_get_time() + EEPROM_WRITE_TIMEOUT_US;

    do
    {
        esp_err_t err = i2c_master_probe(bus_handle,
                                         BSP_EEPROM_I2C_ADDRESS,
                                         EEPROM_IO_TIMEOUT_MS);
        if (err == ESP_OK)
        {
            return ESP_OK;
        }
        if (err != ESP_ERR_NOT_FOUND)
        {
            return err;
        }
        esp_rom_delay_us(EEPROM_ACK_POLL_INTERVAL_US);
    } while (esp_timer_get_time() < deadline_us);

    return ESP_ERR_TIMEOUT;
}

// 写入单个物理页片段，调用方保证数据不跨页。
static esp_err_t eeprom_write_page(uint8_t address,
                                   const uint8_t *data,
                                   size_t size)
{
    uint8_t buffer[1U + BSP_EEPROM_PAGE_SIZE_BYTES];

    buffer[0] = address;
    memcpy(&buffer[1], data, size);

    esp_err_t err = i2c_master_transmit(device_handle,
                                        buffer,
                                        size + 1U,
                                        EEPROM_IO_TIMEOUT_MS);
    if (err != ESP_OK)
    {
        return err;
    }

    return eeprom_wait_ready();
}

esp_err_t bsp_eeprom_init(void)
{
    if (initialized)
    {
        return ESP_OK;
    }

    gpio_config_t write_protect_config = {
        .pin_bit_mask = 1ULL << BSP_EEPROM_WRITE_PROTECT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    i2c_master_bus_config_t bus_config = {
        .i2c_port = BSP_EEPROM_I2C_PORT,
        .sda_io_num = BSP_EEPROM_I2C_SDA_GPIO,
        .scl_io_num = BSP_EEPROM_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_EEPROM_I2C_ADDRESS,
        .scl_speed_hz = EEPROM_I2C_SPEED_HZ,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&write_protect_config), tag,
                        "Configure write protect GPIO failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_EEPROM_WRITE_PROTECT_GPIO,
                                       EEPROM_WRITE_PROTECT_ENABLED),
                        tag, "Enable write protection failed");

    esp_err_t err = i2c_new_master_bus(&bus_config, &bus_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(tag, "Create I2C bus failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_master_bus_add_device(bus_handle, &device_config, &device_handle);
    if (err != ESP_OK)
    {
        (void)i2c_del_master_bus(bus_handle);
        bus_handle = NULL;
        ESP_LOGE(tag, "Add EEPROM device failed: %s", esp_err_to_name(err));
        return err;
    }

    initialized = true;
    ESP_LOGI(tag, "EEPROM initialized");
    return ESP_OK;
}

esp_err_t bsp_eeprom_read(uint16_t address, void *data, size_t size)
{
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, tag,
                        "EEPROM is not initialized");
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, tag,
                        "Read buffer is null");
    ESP_RETURN_ON_FALSE(eeprom_range_is_valid(address, size),
                        ESP_ERR_INVALID_SIZE, tag, "Invalid read range");

    uint8_t word_address = (uint8_t)address;
    return i2c_master_transmit_receive(device_handle,
                                       &word_address,
                                       sizeof(word_address),
                                       data,
                                       size,
                                       EEPROM_IO_TIMEOUT_MS);
}

esp_err_t bsp_eeprom_write(uint16_t address, const void *data, size_t size)
{
    ESP_RETURN_ON_FALSE(initialized, ESP_ERR_INVALID_STATE, tag,
                        "EEPROM is not initialized");
    ESP_RETURN_ON_FALSE(data != NULL, ESP_ERR_INVALID_ARG, tag,
                        "Write buffer is null");
    ESP_RETURN_ON_FALSE(eeprom_range_is_valid(address, size),
                        ESP_ERR_INVALID_SIZE, tag, "Invalid write range");

    esp_err_t err = gpio_set_level(BSP_EEPROM_WRITE_PROTECT_GPIO,
                                   EEPROM_WRITE_PROTECT_DISABLED);
    const uint8_t *source = data;
    size_t offset = 0U;

    while (err == ESP_OK && offset < size)
    {
        const uint16_t current_address = address + offset;
        const size_t page_remaining = BSP_EEPROM_PAGE_SIZE_BYTES -
                                      current_address % BSP_EEPROM_PAGE_SIZE_BYTES;
        const size_t remaining = size - offset;
        const size_t write_size = page_remaining < remaining ? page_remaining : remaining;

        err = eeprom_write_page((uint8_t)current_address,
                                &source[offset],
                                write_size);
        offset += write_size;
    }

    esp_err_t protect_err = gpio_set_level(BSP_EEPROM_WRITE_PROTECT_GPIO,
                                            EEPROM_WRITE_PROTECT_ENABLED);
    if (protect_err != ESP_OK)
    {
        ESP_LOGE(tag, "Restore write protection failed: %s",
                 esp_err_to_name(protect_err));
    }
    return err != ESP_OK ? err : protect_err;
}
