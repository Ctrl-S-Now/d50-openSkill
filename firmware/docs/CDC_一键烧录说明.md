# D50 固件 · CDC 一键烧录说明

> 面向：接手固件烧录/调试的同事。
> 最后更新：2026-09-18。
> 芯片：ESP32-P4 (rev v3.2)；ESP-IDF v6.0.2。

---

## 0. 一句话总结

以前每次烧录都要**手动按 BOOT + RST**进下载模式。现在固件里加了一个"CDC 下载钩子"，
配合项目根目录的 `flash.sh` 脚本，**一条命令就能烧录，全程不用碰 BOOT 键**。

```bash
idf.py build && ./flash.sh
```

> ⚠️ Windows 用户请看 [第 5 节](#5-windows-系统怎么做重要)，`flash.sh` 是给 Mac/Linux 的，
> Windows 有对应的 `flash.py`（跨平台）用法。

---

## 1. 为什么以前必须手按 BOOT？（背景）

- 本板唯一的 Type-C，其 D+/D- 直接接的是 **ESP32-P4 原生 USB-OTG PHY**（不是 USB-Serial-JTAG）。
- 固件运行时，这个 USB 口被 **TinyUSB 的 CDC** 占用（就是平时看日志的那个串口）。
- TinyUSB 不实现 esptool 需要的"通过 DTR/RTS 自动复位进下载模式"的控制接口。
- 对比：ESP32-S3 之所以"按一次就能自动烧"，是因为它走 **USB-Serial-JTAG**（芯片硬件级支持自动复位）。

**结论**：本板硬件决定了 esptool 无法直接自动复位，只能靠固件自己"听懂"复位信号后主动进下载模式。
这不是配置问题，是硬件走线决定的。

---

## 2. 我们做了什么？（修复原理）

### 2.1 固件端：加了一个"CDC 下载钩子"

在 CDC 串口的**线路状态回调**里，识别 esptool 复位时的 **DTR/RTS 信号时序**，一旦识别到，
固件就自己触发"进下载模式 + 软复位"。

核心动作（伪代码）：

```c
usb_dc_prepare_persist();                 // 交接 USB-OTG 控制器给 ROM
chip_usb_set_persist_flags(1u << 31);     // 让 ROM 复位后尽量保活 USB（USBDC_PERSIST_ENA）
REG_SET_BIT(LP_SYSTEM_REG_SYS_CTRL_REG,   // 置"强制下载启动"位
            LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
software_reset_cpu(0);                     // 软复位 → 芯片重启进 ROM 下载模式
```

识别时序的逻辑（避免误触发）：先看到 **RTS 拉高**（esptool 拉 EN 复位）进入"就绪"状态，
随后出现 **DTR 高 且 RTS 低**（esptool 拉 IO0 进下载）才触发。普通终端只拉 DTR、不拉 RTS，不会误触发。

### 2.2 有个开关（Kconfig）

这个钩子由 `CONFIG_USB_CDC_AUTO_DOWNLOAD` 控制，**默认开启**。
需要关掉时（比如纯看日志怕误触发），用 `idf.py menuconfig` → `D50 USB Device` 里关。

### 2.3 关键限制：不能"同一个串口秒连"（重要，务必理解）

固件运行时的 CDC 口和进下载模式后的 ROM 口，是**两个不同的 USB 身份**：

| 状态 | 串口名（Mac 示例） | 串口名（Windows 示例） |
|---|---|---|
| 固件运行（App） | `/dev/cu.usbmodemE8F60AE7ABCF1`（带一长串序列号） | 某个 `COMx` |
| 进下载模式（ROM）| `/dev/cu.usbmodem1101`（纯数字短名） | **另一个** `COMx` |

因为身份不同，进下载模式后**操作系统会分配一个新的串口号**。所以：

- ❌ 直接 `idf.py flash` 会失败——它在原来的 App 口上拨复位信号，钩子生效后 App 口消失，
  esptool 找不到口就报 `Device not configured` / 端口不存在。
- ✅ 正确做法：**先触发钩子进下载模式 → 等新串口出现 → 再对新串口烧录**。
  这套流程已经封装进 `flash.sh` / `flash.py`，你不用手动做。

> 这也是为什么我们没做成"和 S3 一模一样的秒连"——S3 的 USB-JTAG 是硬件固定身份，
> 进出下载模式串口号不变；本板 OTG 架构做不到。但"自动进下载 + 脚本重连"已经比手按 BOOT 强很多。

### 2.4 改了哪些文件

| 文件 | 改动 |
|---|---|
| `components/usb/Kconfig` | 新增 `CONFIG_USB_CDC_AUTO_DOWNLOAD` 开关 |
| `components/usb/usb_device.c` | 加钩子回调 `_cdc_line_state_callback` + 进下载函数 `_enter_download_mode` |
| `components/usb/CMakeLists.txt` | 加 `soc` 依赖（用到寄存器头文件） |
| `flash.sh` / `flash.py` | 一键烧录脚本（触发钩子 → 等口 → 烧录） |

---

## 3. 环境准备（第一次用先做）

1. 安装 **ESP-IDF v6.0.2**（本工程硬性要求 `idf >=6.0.2,<6.1.0`，5.x 不行）。
2. **每开一个新终端，先 source 环境**：

   ```bash
   # Mac / Linux
   source ~/esp/esp-idf-v6.0.2/export.sh
   ```

   ```bat
   REM Windows（在 ESP-IDF 安装目录下）
   export.bat
   ```

   > Windows 推荐直接用开始菜单里的 **"ESP-IDF 6.0 CMD"** 或 **"ESP-IDF 6.0 PowerShell"** 快捷方式，
   > 它会自动把环境准备好，省去手动 source。

---

## 4. 日常使用（Mac / Linux）

```bash
cd /path/to/d50_proj_emote
source ~/esp/esp-idf-v6.0.2/export.sh

# 编译
idf.py build

# 一键烧录（无需手按 BOOT）
./flash.sh

# 只烧不编译也行，flash.sh 直接读 build/ 里已编译好的固件

# 看日志（注意：monitor 的复位会触发下载钩子，只看日志请加 --no-reset）
idf.py -p <App串口> monitor --no-reset
```

`flash.sh` 内部会：找 App 串口 → 拨 DTR/RTS 触发钩子 → 等 ROM 下载串口出现（OTG 重枚举可能十几秒）
→ 用 `--before no-reset` 烧录 → 烧完自动重启回固件。

### 兜底：脚本没识别到口 / 想手动烧

任何时候都可以手动进下载模式：**按住 BOOT → 点一下 RST → 松开 BOOT**，
然后对新出现的下载串口烧：

```bash
idf.py -p <下载串口> flash
```

---

## 5. Windows 系统怎么做（重要）

Windows 没有 bash，`flash.sh` 用不了，请用跨平台的 **`flash.py`**（同目录，逻辑完全一致）。

### 5.1 前置：驱动

- ESP32-P4 的 USB-OTG CDC 在 Win10/Win11 一般**免驱**（识别为标准 USB 串行设备）。
- 若设备管理器里出现带感叹号的未知设备，装一下通用 USB-CDC / "USB Serial Device" 驱动即可。
- 串口号是 `COMx`（不是 Mac 的 `/dev/cu.*`）。可在**设备管理器 → 端口(COM 和 LPT)**里看。

### 5.2 用法

在 **"ESP-IDF 6.0 PowerShell"** 或 **"ESP-IDF 6.0 CMD"** 窗口里：

```bat
cd C:\path\to\d50_proj_emote

REM 编译
idf.py build

REM 一键烧录（无需手按 BOOT）
python flash.py

REM 如果自动找口不准，手动指定 App 串口
python flash.py -p COM5
```

> 注意：进下载模式后 Windows 会分配**另一个** COM 号（和 App 时不同），这是正常的（见 2.3 节）。
> `flash.py` 会自动等待并切换到新 COM 口，你不用管。

### 5.3 Windows 特有注意点

- **COM 口号会变**：App 模式一个号，下载模式另一个号。别把某个固定 COM 号写死。
- **端口被占用**：如果之前开着串口监视器（如 `idf.py monitor`、PuTTY、串口助手），烧录前**先关掉**，
  否则会提示端口被占用。
- **看日志误触发**：和 Mac 一样，`idf.py monitor` 的复位会触发下载钩子，只想看日志时加 `--no-reset`：

  ```bat
  idf.py -p COM5 monitor --no-reset
  ```

- **兜底手动烧**：同样支持"按住 BOOT → 点 RST → 松 BOOT"进下载模式后 `idf.py -p COMx flash`。

---

## 6. 常见问题排查

| 现象 | 原因 / 解决 |
|---|---|
| `Device not configured` / 端口不存在 | 直接 `idf.py flash` 打在了 App 口上（口会消失）。改用 `flash.sh` / `flash.py`。 |
| 脚本"超时未见下载口" | OTG 重枚举慢，或钩子没触发。重跑一次；或手动 BOOT+RST 后 `idf.py -p <口> flash`。 |
| `idf.py monitor` 一连就进下载模式了 | 正常，monitor 的复位触发了钩子。加 `--no-reset`，或临时关 Kconfig 开关。 |
| esptool 手动探测后设备跑回固件了 | esptool 命令默认结尾 hard-reset。探测请加 `--after no-reset`。 |
| 端口被占用（Windows 多见） | 关掉串口监视器/串口助手再烧。 |

---

## 7. 相关文件速查

- 一键烧录脚本：`flash.sh`（Mac/Linux）、`flash.py`（Windows/跨平台）
- 钩子实现：`components/usb/usb_device.c`、开关 `components/usb/Kconfig`
- 更详细的技术调研记录：`HANDOFF.md` 第 7 节
