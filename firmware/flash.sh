#!/usr/bin/env bash
#
# 一键烧录 D50（无需手按 BOOT）。
#
# 本板 Type-C 走 ESP32-P4 原生 USB-OTG，运行固件时是 TinyUSB CDC 口；固件内置的
# CDC 下载钩子（CONFIG_USB_CDC_AUTO_DOWNLOAD）会在识别到 esptool 的 DTR/RTS 复位
# 时序后自复位进 ROM 下载模式。但 App 与 ROM 呈现的 USB 身份不同，进下载后端口名会
# 变（App 口带 MAC 序列号，ROM 口形如 usbmodem1101），无法“同口秒连”，因此本脚本：
#   1) 找到当前 App CDC 口；
#   2) 拨一次 DTR/RTS 触发钩子（等价于 esptool 经典复位时序）；
#   3) 等待 ROM 下载口出现；
#   4) 用 --before no-reset 直接烧录，--after hard-reset 烧完重启回固件。
#
# 若设备已在下载模式（只有 ROM 口、无 App 口），跳过触发直接烧。
#
# 用法：
#   ./flash.sh                # 自动检测端口并烧录
#   ./flash.sh -p /dev/cu.xxx # 指定 App 口
#   ./flash.sh --app          # 只烧应用分区（改代码后快速迭代）
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
CHIP="esp32p4"
BAUD="460800"
PY="${PYTHON:-python}"

list_ports() { ls /dev/cu.usbmodem* 2>/dev/null || true; }

# App 口带长序列号后缀（cu.usbmodemE8F60AE7ABCF1，含十六进制字母）；
# ROM 下载口是纯数字短名（cu.usbmodem1101）。basename 带 cu. 前缀。
is_rom_port() { [[ "$(basename "$1")" =~ ^cu\.usbmodem[0-9]+$ ]]; }

# --app：只烧应用分区（约 1.5 MB），不重烧 6 MB 的字库等没变的分区，只改代码时快很多。
APP_ONLY=0
if [[ "${1:-}" == "--app" ]]; then
    APP_ONLY=1
    shift
fi

APP_PORT=""
if [[ "${1:-}" == "-p" && -n "${2:-}" ]]; then
    APP_PORT="$2"
fi

if [[ ! -f "${BUILD_DIR}/flash_args" ]]; then
    echo "!! 未找到 ${BUILD_DIR}/flash_args，请先 idf.py build" >&2
    exit 1
fi

# 已在下载模式：直接烧。
for p in $(list_ports); do
    if is_rom_port "$p"; then
        echo ">> 检测到 ROM 下载口 ${p}，直接烧录"
        DL_PORT="$p"
        break
    fi
done

if [[ -z "${DL_PORT:-}" ]]; then
    # 找 App 口
    if [[ -z "$APP_PORT" ]]; then
        APP_PORT="$(list_ports | head -1)"
    fi
    if [[ -z "$APP_PORT" ]]; then
        echo "!! 未发现任何 usbmodem 端口，检查连线" >&2
        exit 1
    fi
    echo ">> App 口: ${APP_PORT}，拨 DTR/RTS 触发下载钩子..."

    BEFORE_PORTS="$(list_ports)"

    # 复刻 esptool 经典复位时序：先 RTS 拉高(EN 复位)，再 DTR 高且 RTS 低(IO0 进下载)。
    "$PY" - "$APP_PORT" <<'PYEOF' || true
import sys, time
import serial
port = sys.argv[1]
s = serial.Serial(port)
s.setDTR(False); s.setRTS(True)   # armed: RTS 高
time.sleep(0.1)
s.setDTR(True);  s.setRTS(False)  # trigger: DTR 高 + RTS 低
time.sleep(0.05)
s.setDTR(False)
try:
    s.close()
except Exception:
    pass
PYEOF

    echo ">> 等待 ROM 下载口出现（OTG 重枚举可能需十几秒）..."
    DL_PORT=""
    for _ in $(seq 1 60); do
        sleep 0.5
        for p in $(list_ports); do
            if is_rom_port "$p"; then DL_PORT="$p"; break; fi
        done
        [[ -n "$DL_PORT" ]] && break
    done

    if [[ -z "$DL_PORT" ]]; then
        echo "!! 超时未见 ROM 下载口。可手动进下载模式(按住BOOT→点RST→松BOOT)后重试" >&2
        exit 1
    fi
    echo ">> 已进入下载模式: $DL_PORT"
fi

echo ">> 烧录中 ($DL_PORT)..."
cd "$BUILD_DIR"
# flash 参数（flash-mode/freq/size + 分区偏移与 bin）全部来自 build/flash_args。
if [[ "$APP_ONLY" == "1" ]]; then
    # 第一行是 flash 模式/频率/大小选项；偏移 0x20000 那一行是应用（ota_0）。
    # shellcheck disable=SC2046
    FLASH_ARGS=($(head -1 flash_args) $(grep -E '^0x20000 ' flash_args))
else
    FLASH_ARGS=(@flash_args)
fi
"$PY" -m esptool --chip "$CHIP" -p "$DL_PORT" -b "$BAUD" \
    --before no-reset --after hard-reset \
    write-flash "${FLASH_ARGS[@]}"

echo ">> 完成，设备已重启进固件。"
