# 03 编程指南

从 `firmware/main/app_main.c` 的 `app_main()` 末尾开始写自己的应用。`bsp_init()` 之后所有板载外设都已就绪。
接口在 `firmware/components/bsp/include/bsp.h`，每个函数都有注释。

## 常用外设

| 想做 | 用什么 |
| --- | --- |
| 显示内容 | LVGL 9。访问 LVGL 对象前 `bsp_display_lock(-1)`，用完 `bsp_display_unlock()`。逻辑分辨率 640×480 |
| 开 / 关背光 | `bsp_display_set_backlight(bool)` |
| 按键 / 旋钮 / 摇杆事件 | 在 `struct bsp_config` 里提供 `input_callback`（单击、长按、旋钮转动/按下、摇杆四方向+回中）；`joystick_callback` 给连续坐标（-100…100） |
| 按键灯 | `bsp_led_show()` 逐颗设色，`bsp_led_set_all()` 统一设色 |
| 底部灯 | `bsp_bottom_led_set_state()`（呼吸/流动等）或 `bsp_bottom_led_set_solid_color()` |
| 播放声音 | `bsp_audio_set_speaker_enabled(true)` 后 `bsp_audio_write()` 写 16 kHz 单声道 16 位 PCM；示例见 `firmware/main/sound.c` |
| 录音 | `bsp_audio_read()`（同格式）。**一旦 `usb_init()` 启动，麦克风由 USB 音频占用**，不要再读 |
| USB | `usb_init()`：CDC（一键烧录）、键盘/鼠标 HID、USB 麦克风；HID 接口见 `usb.h` |

## 约定与坑

- 输入回调运行在 BSP 输入任务里，**不要在回调里做耗时操作**，也不要直接操作 LVGL（先 `bsp_display_lock`）。
- 屏幕在背光开启前先渲染一帧，避免白闪（见 `app_main.c` 开机顺序）。
- 自定义字体：默认字体是内置的 24 px 子集（ASCII 为主）；汉字等走 `font_assets` 分区里的完整 24 px 字库（`firmware/main/font_asset.c`）。
- 音频输出没有新数据时会自动播放静音（`auto_clear_after_cb` 已在 BSP 里打开）；不要关掉，否则最后一段声音会循环播放。
- 想要更多功能（网络请求、MQTT、OTA 等），用 ESP-IDF 的标准组件即可；联网见 `04-wifi.md`。
- 修改 `firmware/sdkconfig.defaults` 后需要删掉 `sdkconfig` 再 `idf.py build`，新值才会生效。

## 添加自己的代码

在 `firmware/main/` 新增 `.c` 文件，并加进 `firmware/main/CMakeLists.txt` 的 `SRCS`；需要新的 ESP-IDF 组件就加到 `PRIV_REQUIRES`。
