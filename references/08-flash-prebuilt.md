# 预编译整包固件（免装开发环境）

`d50_open_full_16MB.bin` 是 P4 的完整镜像（放在本仓库 GitHub 的 **Releases** 页面里下载，不在仓库文件里）（引导、分区表、应用、字库），**不含任何 Wi-Fi 密码**。
适合只想体验、不想装 ESP-IDF 的人。

```bash
pip install esptool
python -m esptool --chip esp32p4 -p <串口> -b 460800 write-flash 0x0 d50_open_full_16MB.bin
```

- 串口：设备运行固件时是带长序列号的 USB 串口；如果烧录连不上，按住板上 BOOT → 点一下 RST → 松开 BOOT 进入下载模式，再用新出现的串口。
- 板载 Wi-Fi 芯片（ESP8684）要先烧一次从机固件，见 `firmware/c2_firmware/README.md`，否则开机自检的 Wi-Fi 一项会 FAIL。
