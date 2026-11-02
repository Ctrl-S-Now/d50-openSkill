# D50 开源硬件定义参考

## 1. 文档范围

本文档记录 D50 主板的硬件组成、信号连接、电气属性和固件宏映射，供硬件复刻、接口扩展及底层驱动适配时参考。

本文档不定义页面、按键动作、灯效、日志、USB 枚举、通信协议或其他应用行为。

| 项目 | 定义 |
| --- | --- |
| 主板 | D50 Main Board |
| 硬件版本 | V01 |
| 正式原理图 | `sch_d50_main_board_v01_20260812.pdf` |
| 主控模块 | WT0132P4-A1-N16R32 |
| 主控芯片 | ESP32-P4 v3.2 |
| 固件硬件映射 | `components/bsp/include/bsp_config.h` |

硬件信号的固件映射以当前 `bsp_config.h` 为维护基线。原理图使用外设芯片侧信号名、而源码宏使用 ESP32-P4 方向命名时，本文档会同时标明两种方向。

## 2. 主控与存储

| 项目 | 定义 |
| --- | --- |
| CPU 工作频率 | 400 MHz |
| Flash | 16 MB，QIO |
| PSRAM | 已配置，工作频率 200 MHz |
| EEPROM | AD24C02，256 字节，7 位 I2C 地址 `0x50`（**仅 V01 板装配；V1.1 原理图无此器件**，固件自检不检测它） |
| USB PHY | ESP32-P4 原生 USB PHY |
| 硬件复位 | `P4_EN`，外部 10 kΩ 上拉，按键接地，低电平复位 |

Reset 键直接作用于 `P4_EN`，不连接普通 GPIO。

EEPROM 使用独立 I2C0 总线：SDA=GPIO39，SCL=GPIO40；写保护信号连接
GPIO43，高电平禁止写入。对应固件宏统一定义在 `bsp_config.h`。

## 3. 电源与 USB

### 3.1 电源网络

| 电源网络 | 来源或用途 |
| --- | --- |
| `5V_USB` | Type-C VBUS 输入 |
| `5V_DC` | `5V_USB` 经磁珠后的主 5 V 电源 |
| `VCC33` | SY8120IABC 将 5 V 降压至 3.3 V |
| `5V_P4` | ESP32-P4 模块 5 V 供电支路 |
| `5V_RGB` | RGB 灯供电支路 |
| `3V3_LDO` | ETA5055V330DS2F 生成的模拟音频 3.3 V 电源 |

### 3.2 USB 接口

| 信号 | 连接 |
| --- | --- |
| VBUS | `5V_USB` |
| D- | `USB_DN`，连接 ESP32-P4 原生 USB PHY |
| D+ | `USB_DP`，连接 ESP32-P4 原生 USB PHY |
| GND | `DGND` |

USB D+/D- 是专用 USB 信号，不作为普通 GPIO 使用。

## 4. LCD 与背光

### 4.1 显示接口

| 项目 | 定义 |
| --- | --- |
| 面板分辨率 | 480 × 640 |
| 像素格式 | RGB888 |
| 显示接口 | MIPI-DSI，2 Lane |
| DSI 差分信号 | `DSI_D0P/N`、`DSI_D1P/N`、`DSI_CLKP/N` |
| DPI 像素时钟 | 27 MHz |
| DSI Lane Rate | 390 Mbps |
| DSI PHY LDO | Channel 3，2500 mV；`BSP_LCD_DSI_LDO_CHANNEL`、`BSP_LCD_DSI_LDO_VOLTAGE_MV` |

### 4.2 控制与背光

| 功能 | GPIO | 电气说明 | 源码宏 |
| --- | ---: | --- | --- |
| LCD Reset | 23 | 低电平复位 | `BSP_LCD_RESET_GPIO` |
| 背光 PWM | 22 | 连接背光升压驱动的亮度控制 | `BSP_LCD_BACKLIGHT_PWM_GPIO` |
| 背光使能 | 21 | 连接背光电源使能级 | `BSP_LCD_BACKLIGHT_ENABLE_GPIO` |

背光电路由 5 V 供电，使用 ETA1611S2G 升压驱动 LCD 的 `LEDA`/`LEDK` 网络。LCD 连接器同时提供 3.3 V I/O 电源、5 V 电源、DSI 差分信号、Reset 和背光端子。

## 5. 按键、旋钮与摇杆

### 5.1 普通按键与 POP

KEY1～KEY5 和 POP 均由外部电阻上拉，按键闭合时接地，因此为低电平有效。

| 输入 | GPIO | 有效电平 | 源码宏 |
| --- | ---: | --- | --- |
| KEY1 | 8 | 低 | `BSP_KEY_1_GPIO` |
| KEY2 | 1 | 低 | `BSP_KEY_2_GPIO` |
| KEY3 | 16 | 低 | `BSP_KEY_3_GPIO` |
| KEY4 | 17 | 低 | `BSP_KEY_4_GPIO` |
| KEY5 | 18 | 低 | `BSP_KEY_5_GPIO` |
| POP | 42 | 低 | `BSP_POP_BUTTON_GPIO` |

### 5.2 旋转编码器

| 信号 | GPIO | 电气说明 | 源码宏 |
| --- | ---: | --- | --- |
| 按键 K | 41 | 外部上拉，按下接地 | `BSP_ENCODER_BUTTON_GPIO` |
| A 相 | 45 | 正交编码信号 | `BSP_ENCODER_PHASE_A_GPIO` |
| B 相 | 44 | 正交编码信号 | `BSP_ENCODER_PHASE_B_GPIO` |

固件通过 `BSP_ENCODER_MODEL` 在编译期选择实际装配的编码器，默认使用
`BSP_ENCODER_MODEL_EC110204M2F_VH1_141`。

| 编码器型号宏 | 定位数 | 每定位点相位跳变数 | 方向处理 |
| --- | ---: | ---: | --- |
| `BSP_ENCODER_MODEL_30_STEP_HALF_CODE` | 30/360° | 2 | 原方向 |
| `BSP_ENCODER_MODEL_EC110204M2F_VH1_141` | 20/360° | 4 | 反转 A/B 解码方向 |

`EC110204M2F-VH1-141` 规格书定义每相 20 pulses/360°、每个定位点一个脉冲；
实机每格产生四次有效相位跳变，且 A/B 相序与原 30 步编码器相反。

### 5.3 摇杆

| 轴 | GPIO | ADC | 源码宏 |
| --- | ---: | --- | --- |
| X | 19 | ADC1 Channel 3 | `BSP_JOYSTICK_X_CHANNEL` |
| Y | 20 | ADC1 Channel 4 | `BSP_JOYSTICK_Y_CHANNEL` |

摇杆由 `VCC33` 供电，X/Y 电位器滑动端分别输出 `ADC_X` 和 `ADC_Y` 模拟电压。

ESP32-P4 使用 `ADC_ATTEN_DB_12` 采样，3.3 V 对应的原始值约为 3500。固件固定按
`0～3500` 有效量程、`1750` 中心值进行死区补偿和 `-100～100` 坐标映射。
实物 X 轴 ADC 增大表示向左，因此 X 坐标需要反相；Y 轴 ADC 减小表示向上，保持原符号。

## 6. 音频

主板包含 ES8311 Codec、直接连接 ES8311 的模拟麦克风输入、独立 AMIC ADC 输入链路以及 NS4150B 功放输出。

### 6.1 ES8311 控制接口

| 功能 | GPIO/地址 | 电气说明 | 源码宏 |
| --- | --- | --- | --- |
| I2C 控制器 | I2C1 | ES8311 控制总线 | `BSP_AUDIO_I2C_PORT` |
| I2C SDA | GPIO14 | 外部 4.7 kΩ 上拉至 `VCC33` | `BSP_AUDIO_I2C_SDA_GPIO` |
| I2C SCL | GPIO15 | 外部 4.7 kΩ 上拉至 `VCC33` | `BSP_AUDIO_I2C_SCL_GPIO` |
| CE/地址选择 | GPIO6 | 外部 10 kΩ 下拉，固件不驱动 | `BSP_AUDIO_CODEC_CE_GPIO` |
| I2C 地址 | 7 位 `0x18` | 对应 8 位写地址 `0x30` | - |

CE 是 ES8311 的地址选择信号，不是电源使能信号。主板安装 10 kΩ 下拉电阻，预留上拉电阻不安装。

### 6.2 ES8311 I2S 接口

ES8311 原理图网络按 Codec 引脚命名，源码中的 `DIN`/`DOUT` 宏按 ESP32-P4 控制器方向命名。

| ES8311 信号 | GPIO/控制器 | ESP32-P4 方向 | 源码宏 |
| --- | ---: | --- | --- |
| I2S 控制器 | I2S1 | 主机 | `BSP_AUDIO_I2S_PORT` |
| `ASDOUT` / DOUT | 11 | 输入 | `BSP_AUDIO_I2S_DIN_GPIO` |
| `DSDIN` / DIN | 9 | 输出 | `BSP_AUDIO_I2S_DOUT_GPIO` |
| `LRCK` / WS | 10 | 输出 | `BSP_AUDIO_I2S_WS_GPIO` |
| `SCLK` / BCLK | 12 | 输出 | `BSP_AUDIO_I2S_BCLK_GPIO` |
| `MCLK` | 13 | 输出 | `BSP_AUDIO_I2S_MCLK_GPIO` |

因此，`ES8311 ASDOUT/DOUT → GPIO11 → ESP32-P4 输入`；`ESP32-P4 输出 → GPIO9 → ES8311 DSDIN/DIN`。

### 6.3 麦克风与功放

| 功能 | 连接 | 源码宏 |
| --- | --- | --- |
| ES8311 模拟麦克风 | 差分连接 `MIC1P`/`MIC1N`，使用 `VMIC` 偏置 | - |
| 独立 AMIC ADC | `ADC_MIC` → GPIO54 / ADC2 Channel 5 | `BSP_AMIC_ADC_GPIO`、`BSP_AMIC_ADC_CHANNEL` |
| 功放使能 | `PA_CTRL` → GPIO53 | `BSP_AUDIO_PA_ENABLE_GPIO` |
| 扬声器输出 | NS4150B 差分输出 `PA_OUTL+`/`PA_OUTL-` | - |

独立 AMIC 链路使用 ETA5055V330DS2F 提供模拟 3.3 V 电源，并通过 RS721 运放输出 `ADC_MIC`。

## 7. RGB 灯

| 灯组 | 数量 | 数据 GPIO | 供电 | 源码宏 |
| --- | ---: | ---: | --- | --- |
| 按键 RGB 灯 | 5 | 7 | `RGB_5V` | `BSP_KEY_LED_DATA_GPIO` |
| 底部 RGB 灯板 | 12 | 0 | `5V_RGB` | `BSP_BOTTOM_LED_DATA_GPIO`、`BSP_BOTTOM_LED_COUNT` |

两组灯使用独立串行数据链路。五颗按键灯在主板上串联，底部灯板通过主板连接器引出电源、地和数据网络。

## 8. ESP8684 协处理器与预留接口

主板安装 ESP8684-WROOM-01C-H4X，并通过 SPI、握手和使能信号连接 ESP32-P4。

| 信号 | ESP32-P4 GPIO | ESP8684 连接 |
| --- | ---: | --- |
| `C2_CS` | 28 | IO10 |
| `C2_MOSI` | 29 | IO7 |
| `C2_SCLK` | 30 | IO6 |
| `C2_MISO` | 31 | IO2 |
| `C2_HAND`（握手） | 26 | IO4 |
| `C2_DRDY`（数据就绪） | 25 | IO5 |
| `C2_EN`（复位） | 27 | EN |

以上引脚依据 `SCH_D50_MAIN_Board_V1.1_20260819` 核对，并已在实机上验证（esp-hosted SPI 全双工）。
P4 侧固件通过 `esp_wifi_remote` + `esp_hosted` 使用该协处理器，配置见 `sdkconfig.defaults` 末尾。
ESP8684 出厂不一定带 esp-hosted 从机固件；预编译的从机固件和烧录方法见 `c2_firmware/README.md`。

主板另外预留 ESP8684 UART 测试连接，以及 ESP32-P4 `UART0_TX`/`UART0_RX` 接口；相关串联电阻为预留位时，以实际 BOM 装配状态为准。
