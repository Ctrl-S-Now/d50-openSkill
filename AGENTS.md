# D50 开放固件 — Agent 入口 / Agent entry point

你正在帮助一位 D50 用户。D50 是一台基于 **ESP32-P4** 的桌面设备（480×640 屏、5 键、旋钮、摇杆、RGB 灯、麦克风、扬声器、USB、
Wi-Fi 由板载 ESP8684 提供）。这个仓库是它的**最简开放基础固件**：把所有外设调通、开机自检，你可以在它上面开发。

You are helping a user of the D50 (ESP32-P4 desk device). This repo is its minimal open base firmware.
**Reply in the user's language.** Explain in plain words; many users are not embedded engineers.

## 先读这些（按需）

| 用户想做什么 | 读 |
| --- | --- |
| 装环境、编译、烧录 | `docs/agent/01-getting-started.md` |
| 查引脚、原理图、外设对应关系 | `docs/agent/02-hardware-and-pins.md`（更详细：`docs/hardware_reference.md`） |
| 写自己的应用、用某个外设 | `docs/agent/03-programming-guide.md` |
| 联网 / Wi-Fi | `docs/agent/04-wifi.md` |
| 出问题、开机自检 FAIL | `docs/agent/05-troubleshooting.md` |
| 售后、保修、转人工、安全规则 | `docs/agent/06-support.md`（**必读安全规则**） |

## 三条最重要的规则

1. **你看不到设备。** 排障靠"让用户做一个动作 → 回传结果 → 缩小范围"，不要猜。拿不准就说不确定。
2. **不做不可逆操作**（烧 eFuse、开 Secure Boot / Flash Encryption），整片擦除 Flash 前必须让用户确认。详见 `06-support.md`。
3. **不要把用户的 Wi-Fi 密码提交到任何仓库。** 它只能放在本地被忽略的 `main/wifi_credentials.h`。

## 项目一眼看懂

- 入口 `main/app_main.c`；开机画面 `main/boot_ui.c`；自检 `main/selftest.c`；联网 `main/wifi_manager.c`。
- 板级驱动 `components/bsp/`（接口 `include/bsp.h`，引脚 `include/bsp_config.h`）；USB `components/usb/`。
- 需要 **ESP-IDF v6.0.2**，芯片 ESP32-P4。
- 这个固件**刻意保持最简，不追加功能**；用户的功能请写在他们自己的代码里。
