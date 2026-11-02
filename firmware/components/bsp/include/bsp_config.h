/*
 * 文件：bsp_config.h
 * 描述：D50 主板硬件参数的唯一代码来源，集中定义已确认的引脚和电气配置。
 * 约束：底部灯已确认为六颗，数据引脚和数量均在本文件定义。
 */

#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "driver/i2s_types.h"
#include "esp_adc/adc_oneshot.h"

// LCD 与 MIPI-DSI 参数，依据正式主板原理图和已验证屏幕配置。
#define BSP_LCD_H_RES                         (480U)
#define BSP_LCD_V_RES                         (640U)
#define BSP_LCD_DPI_CLOCK_MHZ                 (27U)
#define BSP_LCD_DSI_LANE_COUNT                (2U)
#define BSP_LCD_DSI_LANE_RATE_MBPS            (390U)
#define BSP_LCD_FRAME_BUFFER_COUNT            (3U)
#define BSP_LCD_RESET_GPIO                    (GPIO_NUM_23)
#define BSP_LCD_BACKLIGHT_PWM_GPIO            (GPIO_NUM_22)
#define BSP_LCD_BACKLIGHT_ENABLE_GPIO         (GPIO_NUM_21)
#define BSP_LCD_DSI_LDO_CHANNEL               (3)
#define BSP_LCD_DSI_LDO_VOLTAGE_MV            (2500)

// 五个普通按键和 POP 键均由外部电阻上拉，按下时为低电平。
#define BSP_KEY_1_GPIO                        (GPIO_NUM_8)
#define BSP_KEY_2_GPIO                        (GPIO_NUM_1)
#define BSP_KEY_3_GPIO                        (GPIO_NUM_16)
#define BSP_KEY_4_GPIO                        (GPIO_NUM_17)
#define BSP_KEY_5_GPIO                        (GPIO_NUM_18)
#define BSP_POP_BUTTON_GPIO                   (GPIO_NUM_42)

// 旋钮 A/B 相及按键引脚，旋钮按下时为低电平。
#define BSP_ENCODER_BUTTON_GPIO               (GPIO_NUM_41)
#define BSP_ENCODER_PHASE_A_GPIO              (GPIO_NUM_45)
#define BSP_ENCODER_PHASE_B_GPIO              (GPIO_NUM_44)

// 编码器型号由硬件装配决定；默认使用 20 步 EC110204M2F-VH1-141。
#define BSP_ENCODER_MODEL_30_STEP_HALF_CODE    (0)
#define BSP_ENCODER_MODEL_EC110204M2F_VH1_141 (1)
#define BSP_ENCODER_MODEL                     BSP_ENCODER_MODEL_EC110204M2F_VH1_141

#if BSP_ENCODER_MODEL == BSP_ENCODER_MODEL_30_STEP_HALF_CODE
#define BSP_ENCODER_DETENTS_PER_REVOLUTION    (30U)
#define BSP_ENCODER_TRANSITIONS_PER_DETENT    (2)
#define BSP_ENCODER_DIRECTION_INVERTED        (0)
#elif BSP_ENCODER_MODEL == BSP_ENCODER_MODEL_EC110204M2F_VH1_141
#define BSP_ENCODER_DETENTS_PER_REVOLUTION    (20U)
#define BSP_ENCODER_TRANSITIONS_PER_DETENT    (4)
#define BSP_ENCODER_DIRECTION_INVERTED        (1)
#else
#error "Unsupported BSP_ENCODER_MODEL"
#endif

// 摇杆 X/Y 分别连接 GPIO19/GPIO20，即 ADC1 通道 3/4。
#define BSP_JOYSTICK_ADC_UNIT                 (ADC_UNIT_1)
#define BSP_JOYSTICK_X_CHANNEL                (ADC_CHANNEL_3)
#define BSP_JOYSTICK_Y_CHANNEL                (ADC_CHANNEL_4)

// AD24C02 EEPROM 使用独立 I2C0 总线，WP 高电平时禁止写入。
#define BSP_EEPROM_I2C_PORT                   (I2C_NUM_0)
#define BSP_EEPROM_I2C_SDA_GPIO               (GPIO_NUM_39)
#define BSP_EEPROM_I2C_SCL_GPIO               (GPIO_NUM_40)
#define BSP_EEPROM_WRITE_PROTECT_GPIO         (GPIO_NUM_43)
#define BSP_EEPROM_I2C_ADDRESS                (0x50U)
#define BSP_EEPROM_SIZE_BYTES                 (256U)
#define BSP_EEPROM_PAGE_SIZE_BYTES            (8U)

// 录音输入源编译期选择：1 使用独立 AMIC ADC 链路，0 使用 ES8311 I2S 链路。
#define BSP_AUDIO_USE_AMIC_INPUT              (0)

#if BSP_AUDIO_USE_AMIC_INPUT != 0 && BSP_AUDIO_USE_AMIC_INPUT != 1
#error "BSP_AUDIO_USE_AMIC_INPUT must be 0 or 1"
#endif

// 独立 AMIC 模拟输出连接 GPIO54，即 ADC2 通道 5。
#define BSP_AMIC_ADC_GPIO                     (GPIO_NUM_54)
#define BSP_AMIC_ADC_UNIT                     (ADC_UNIT_2)
#define BSP_AMIC_ADC_CHANNEL                  (ADC_CHANNEL_5)

// ES8311 的 I2C、I2S、CE 地址选择和功放控制引脚，I2S 数据方向以 ESP32-P4 为基准。
#define BSP_AUDIO_I2C_PORT                    (I2C_NUM_1)
#define BSP_AUDIO_I2C_SDA_GPIO                (GPIO_NUM_14)
#define BSP_AUDIO_I2C_SCL_GPIO                (GPIO_NUM_15)
#define BSP_AUDIO_I2S_PORT                    (I2S_NUM_1)
#define BSP_AUDIO_I2S_DOUT_GPIO               (GPIO_NUM_9)  // P4 输出到 ES8311 DSDIN
#define BSP_AUDIO_I2S_WS_GPIO                 (GPIO_NUM_10)
#define BSP_AUDIO_I2S_DIN_GPIO                (GPIO_NUM_11)   // ES8311 ASDOUT 输入到 P4
#define BSP_AUDIO_I2S_BCLK_GPIO               (GPIO_NUM_12)
#define BSP_AUDIO_I2S_MCLK_GPIO               (GPIO_NUM_13)
// CE 由主板外部 10 kΩ 下拉为 0，选择 8 位 I2C 地址 0x30，固件不得配置。
#define BSP_AUDIO_CODEC_CE_GPIO               (GPIO_NUM_6)
#define BSP_AUDIO_PA_ENABLE_GPIO              (GPIO_NUM_53)

// 主板五颗按键灯和六颗底部 RGB 灯使用独立数据链路。
#define BSP_KEY_LED_DATA_GPIO                 (GPIO_NUM_7)
#define BSP_BOTTOM_LED_DATA_GPIO              (GPIO_NUM_0)
#define BSP_BOTTOM_LED_COUNT                  (6U)
#define BSP_BOTTOM_LED_BREATH_MAX_BRIGHTNESS  (64U)   // 呼吸效果最大亮度
#define BSP_BOTTOM_LED_BREATH_SATURATION      (128U)  // 呼吸效果饱和度
