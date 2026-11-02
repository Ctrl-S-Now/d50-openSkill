# 02 硬件与引脚（以原理图 V1.1 核对）

主控模块 WT0132P4-A1-N16R32（ESP32-P4，16 MB Flash，32 MB PSRAM）。固件里的引脚宏在 `components/bsp/include/bsp_config.h`，
下表已与原理图 `D50 MAIN Board V1.1 (2026-08-19)` 逐项核对一致。

| 外设 | 信号 → P4 GPIO |
| --- | --- |
| 屏幕（MIPI-DSI，ST7701，480×640，旋转后逻辑 640×480） | 复位 GPIO23；背光 PWM GPIO22、背光使能 GPIO21 |
| 按键 K1…K5 | GPIO8 / 1 / 16 / 17 / 18 |
| Pop 键（`KEY_PP`） | GPIO42 |
| 旋钮 | A=GPIO45，B=GPIO44，按下（`KNOB_K`）=GPIO41 |
| 摇杆 | X=GPIO19（ADC1_CH3），Y=GPIO20（ADC1_CH4） |
| 按键 RGB 灯（5 颗串联） | 数据 GPIO7 |
| 底部 RGB 灯 | 数据 GPIO0 |
| 音频编解码 ES8311 | I2C：SDA=GPIO14、SCL=GPIO15；I2S：MCLK=13、BCLK=12、WS=10、DOUT=9、DIN=11；使能 `8311_EN`=GPIO6 |
| 功放使能 `PA_CTRL` | GPIO53 |
| 模拟麦克风 `ADC_MIC` | GPIO54（ADC2_CH5） |
| 板载 Wi-Fi 芯片 ESP8684（SPI） | CS=28、MOSI=29、SCLK=30、MISO=31、握手 `C2_HAND`=26、数据就绪 `C2_DRDY`=25、复位 `C2_EN`=27 |
| USB | 原生 USB PHY（专用 D+/D-，不是普通 GPIO） |
| UART0（调试日志） | TX=GPIO37、RX=GPIO38（引到测试点） |

## 注意

- **V1.1 板没有 EEPROM**（旧的 V01 板才有，固件里仍保留驱动，但自检不检测它）。GPIO39 / 40 / 43 只是测试点。
- ESP8684 一侧的引脚：MOSI=IO7、MISO=IO2、CLK=IO6、CS=IO10、HANDSHAKE=IO4、DATA_READY=IO5；其 UART0（TXD0/RXD0）只引到测试点 TP42 / TP44，IO9 引到 TP49。
- USB D+/D-、PSRAM、Flash 相关引脚不要当普通 GPIO 使用。
- 完整说明见 `docs/hardware_reference.md`。
