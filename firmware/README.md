# D50 开放固件（Open Firmware）

> 这是 [d50-openSkill](../README.md) 里的固件部分。助手入口见仓库根目录的 `SKILL.md`。

D50 的最简基础固件：把所有板载外设调通，开机即做一次自检。
你可以在它的基础上，直接开始写自己的应用。

## 开机会看到什么

1. JOBY 标志淡入、淡出。
2. 欢迎语，邀请你与我们一起共创、构建开源生态。
3. **外设自检**：显示屏、灯效、PSRAM、扬声器（开机提示音）、麦克风、按键旋钮、USB、Wi-Fi，逐项显示 `OK` / `FAIL`。
4. 开源社区地址与二维码（地址在 `main/open_config.h`）。

之后可以按 K1–K5（对应按键灯点亮）、转动或按下旋钮（按下有提示音）、拨动摇杆，屏幕会显示最近一次输入。

## 目录

| 路径 | 作用 |
| --- | --- |
| `main/app_main.c` | 入口：初始化外设、开机流程。**从这里开始改。** |
| `main/boot_ui.c` | 开机画面（LVGL） |
| `main/selftest.c` | 各外设自检 |
| `main/open_config.h` | 社区地址等可改项 |
| `components/bsp/` | 板级驱动：屏幕、按键/旋钮/摇杆、灯、音频、EEPROM。接口见 `include/bsp.h`，引脚见 `include/bsp_config.h` |
| `components/usb/` | USB：CDC（一键烧录）、键盘鼠标 HID、USB 麦克风 |
| `c2_firmware/` | 板载 Wi-Fi 芯片（ESP8684）的从机固件与烧录方法 |
| `../references/07-hardware-reference.md` | 硬件与引脚说明 |
| `docs/CDC_一键烧录说明.md` | 免按 BOOT 的一键烧录原理 |

## 构建与烧录

需要 **ESP-IDF v6.0.2**（`>=6.0.2,<6.1.0`），芯片 ESP32-P4。

```bash
source ~/esp/esp-idf-v6.0.2/export.sh
idf.py build
./flash.sh            # Windows 用 python flash.py；自动进下载模式，免按 BOOT
```

首次构建会自动下载依赖组件（需联网）。

## Wi-Fi 说明

D50 的 Wi-Fi 由板载 ESP8684 提供，P4 通过 SPI 使用（esp-hosted）。
**ESP8684 要先烧一次从机固件**，见 `c2_firmware/README.md`。烧好后，在 P4 里就是标准的 `esp_wifi_*` 接口，
连哪个网络、何时连、断线怎么办，全由你的 P4 代码（`main/wifi_manager.c`）决定。

> 不要把 Wi-Fi 名称和密码提交到公开仓库。

## 让 AI 助手帮你

仓库根目录的 `SKILL.md` 和 `references/` 是给你的 AI 助手读的：编程指导、引脚说明、联网配置、排障和售后流程都在里面。克隆仓库后对助手说"读一下 SKILL.md 然后帮我"即可。

## 联网

默认固件**不会**自动联网，也不会在界面上出现任何联网提示。想联网：
在本地创建 `main/wifi_credentials.h`（参考 `.example`）填入 Wi-Fi，重新烧录，设备开机后静默连接。
不想写密码，可以把 `main/open_config.h` 里的 `D50_WIFI_PORTAL_ENABLE` 改为 1，设备会进入手机配网页。详见 `../references/04-wifi.md`。

## 许可

（待补充）
