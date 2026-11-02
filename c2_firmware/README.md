# ESP8684 (C2) 从机固件

D50 的 Wi-Fi 来自板载 ESP8684。P4 通过 SPI 使用它，C2 里必须有 **esp-hosted SPI 从机固件**，
否则 P4 开机自检的 Wi-Fi 项会显示 FAIL。这里是按 D50 引脚编好的成品，**只需烧一次**。
（以后可由 P4 经 esp-hosted OTA 升级 C2，不必再飞线。）

引脚（C2 侧）：MOSI=IO7 MISO=IO2 CLK=IO6 CS=IO10 HANDSHAKE=IO4 DATA_READY=IO5，配置见 `sdkconfig.defaults.d50`。

## 烧录（需要飞线，一次性）

C2 的下载串口只引到测试点，没有连到 P4。准备一个 **3.3V 电平** 的 USB 转串口（VCC 不接）：

| 转串口 | D50 测试点 |
| --- | --- |
| TXD | TP44（C2 RXD0） |
| RXD | TP42（C2 TXD0） |
| GND | 任意 GND |
| — | TP49（C2 IO9）经按钮/杜邦线接 GND，进入下载模式 |

1. 拔掉 D50 的 USB。
2. **按住** TP49↔GND（P4 上电后会复位一次 C2，所以要一直按住）。
3. 插上 D50 的 USB，保持按住，然后运行（把端口换成你的）：

```bash
python -m esptool --chip esp32c2 -p /dev/cu.usbserial-XXXX -b 115200 \
  --before no-reset --after no-reset write-flash \
  --flash-mode dio --flash-freq 60m --flash-size 2MB \
  0x0 bootloader.bin 0x8000 partition-table.bin 0xd000 ota_data_initial.bin 0x10000 esp_hosted_slave.bin
```

4. 烧完松开按钮、拆掉 TP49 的线，重新拔插 D50 的 USB。开机自检 Wi-Fi 项显示 `OK n APs` 即成功。

> 若串口读到一直重复的垃圾数据，把转串口线从电脑上拔掉重插再试。
