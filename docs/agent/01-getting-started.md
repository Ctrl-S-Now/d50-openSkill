# 01 环境、编译、烧录

## 环境

- 芯片 **ESP32-P4**，需要 **ESP-IDF v6.0.2**（工程限定 `>=6.0.2,<6.1.0`，别的版本会报依赖错误）。
- 首次构建会自动下载依赖组件，需要联网；下载慢可换 IDF 组件镜像。
- 每个新终端先激活 IDF：

```bash
source ~/esp/esp-idf-v6.0.2/export.sh
```

> 如果终端里已经激活过别的 IDF 版本，先 `unset IDF_PATH IDF_PYTHON_ENV_PATH IDF_TOOLS_PATH` 再 source，否则会报 Python 版本错误。

## 编译与烧录

```bash
idf.py build
./flash.sh            # Windows 用 python flash.py
```

`flash.sh` 会让固件自己进入下载模式（免按 BOOT），烧完自动重启。原理见 `docs/CDC_一键烧录说明.md`。

只改了代码、没动字库等资源时，用 `./flash.sh --app` 只烧应用分区（约 17 秒；完整烧录要 40 秒以上，因为字库 6 MB 每次都重烧）。第一次烧录或改过分区/字库后，仍要完整烧录。

- **端口会变是正常的**：固件运行时是带长序列号的 USB 串口（如 `cu.usbmodemE8F6…`），下载模式是短名（如 `cu.usbmodem1201`）。
- 烧录失败的兜底：按住板上 **BOOT** → 点一下 **RST** → 松开 BOOT，然后 `idf.py -p <下载口> flash`。
- 只看日志、不想触发复位：`idf.py monitor --no-reset`。设备主日志在 UART0（板上测试点），USB 口上通常没有日志。

## 烧录前先确认

- 确认要写的串口确实是 D50，再执行烧录。
- 两个固件互不相干：**P4 主固件**（本仓库）和 **ESP8684 从机固件**（`c2_firmware/`，只需烧一次，见 `04-wifi.md`）。
