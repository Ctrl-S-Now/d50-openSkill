#!/usr/bin/env python3
"""
一键烧录 D50（无需手按 BOOT）—— 跨平台版本，Windows / Mac / Linux 通用。

原理见 docs/CDC_一键烧录说明.md：
  1) 找到当前 App CDC 串口；
  2) 拨一次 DTR/RTS，复刻 esptool 经典复位时序，触发固件里的 CDC 下载钩子；
  3) 等待进下载模式后新出现的 ROM 下载串口；
  4) 用 esptool --before no-reset 直接烧录，--after hard-reset 烧完重启回固件。
若设备已在下载模式，则跳过触发直接烧。

用法：
  python flash.py                 # 自动检测端口
  python flash.py -p COM5         # (Windows) 指定 App 串口
  python flash.py -p /dev/cu.xxx  # (Mac/Linux) 指定 App 串口

依赖：pyserial、esptool（ESP-IDF 的 python 环境自带）。请先在 IDF 环境里 idf.py build。
"""
import argparse
import os
import subprocess
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("!! 缺少 pyserial。请在 ESP-IDF 环境里运行本脚本。")

CHIP = "esp32p4"
BAUD = "460800"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.join(SCRIPT_DIR, "build")
POLL_TIMEOUT_S = 30.0


def _is_usb_serial(p):
    """判断是否像 USB 串口（排除蓝牙/调试等无关口）。跨平台。"""
    dev = (p.device or "")
    name = dev.lower()
    # Windows: COMx；macOS: /dev/cu.usbmodem*；Linux: ttyUSB*/ttyACM*
    if dev.upper().startswith("COM"):
        return True
    if "usbmodem" in name or "ttyusb" in name or "ttyacm" in name:
        return True
    return False


def list_serial_ports():
    """返回当前所有“像 USB 串口”的设备名集合（已过滤蓝牙等无关口）。"""
    return {p.device for p in list_ports.comports() if _is_usb_serial(p)}


def trigger_download_hook(port):
    """在 App 串口上复刻 esptool 经典复位时序，触发固件下载钩子。"""
    s = serial.Serial(port)
    try:
        s.setDTR(False)
        s.setRTS(True)   # armed: RTS 高（EN 复位）
        time.sleep(0.1)
        s.setDTR(True)
        s.setRTS(False)  # trigger: DTR 高 + RTS 低（IO0 进下载）
        time.sleep(0.05)
        s.setDTR(False)
    finally:
        try:
            s.close()
        except Exception:
            pass


def wait_for_new_port(before_ports, timeout=POLL_TIMEOUT_S):
    """等待一个 before_ports 里没有的新串口出现，返回其设备名。"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(0.5)
        new = list_serial_ports() - before_ports
        if new:
            return sorted(new)[0]
    return None


def run_flash(port):
    """对指定（下载模式）串口执行 esptool write-flash。"""
    # flash 参数（flash-mode/freq/size + 各分区偏移与 bin）全部来自 build/flash_args，
    # 不在此重复，避免与 flash_args 冲突。
    args = [
        sys.executable, "-m", "esptool",
        "--chip", CHIP, "-p", port, "-b", BAUD,
        "--before", "no-reset", "--after", "hard-reset",
        "write-flash", "@flash_args",
    ]
    print(">> 烧录中 (%s)..." % port)
    return subprocess.call(args, cwd=BUILD_DIR)


def main():
    ap = argparse.ArgumentParser(description="D50 一键烧录（无需手按 BOOT）")
    ap.add_argument("-p", "--port", help="App 串口（Windows: COMx；Mac/Linux: /dev/cu.*）")
    opts = ap.parse_args()

    if not os.path.isfile(os.path.join(BUILD_DIR, "flash_args")):
        sys.exit("!! 未找到 %s/flash_args，请先 idf.py build" % BUILD_DIR)

    ports = list_serial_ports()
    if not ports:
        sys.exit("!! 未发现任何串口，检查连线/驱动")

    # 已在下载模式的判定较难跨平台精确区分，这里采用统一策略：
    # 先记录当前串口集合，触发钩子后等“新增”串口。若触发前后串口没变化（可能本就在下载模式），
    # 则直接对当前串口烧录。
    app_port = opts.port
    if app_port is None:
        app_port = sorted(ports)[0]
        if len(ports) > 1:
            print(">> 检测到多个串口，默认用 %s；如不对请用 -p 指定" % app_port)

    print(">> App 串口: %s，拨 DTR/RTS 触发下载钩子..." % app_port)
    before = list_serial_ports()
    try:
        trigger_download_hook(app_port)
    except Exception as e:
        print(">> 触发时串口异常（可能钩子已生效使其断开）: %s" % e)

    print(">> 等待 ROM 下载串口出现（OTG 重枚举可能十几秒）...")
    dl_port = wait_for_new_port(before)

    if dl_port is None:
        # 没有新增口：可能设备本就在下载模式，直接用当前口试烧。
        still = list_serial_ports()
        if app_port in still:
            print(">> 未见新串口，尝试直接对 %s 烧录（可能已在下载模式）" % app_port)
            dl_port = app_port
        else:
            sys.exit("!! 超时未见下载串口。请手动进下载模式"
                     "(按住BOOT→点RST→松BOOT)后重试，或用 -p 指定。")
    else:
        print(">> 已进入下载模式: %s" % dl_port)

    rc = run_flash(dl_port)
    if rc == 0:
        print(">> 完成，设备已重启进固件。")
    sys.exit(rc)


if __name__ == "__main__":
    main()
