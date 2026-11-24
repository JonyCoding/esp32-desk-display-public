# ESP32 桌面像素屏

一块 64×64 HUB75E 桌面屏。Mac 解锁时轮播 Codex 额度、行情和场景时钟，锁屏后只轮播时钟和行情。ESP32 独立生成画面，关掉 Mac 也能继续播放本地动画。装上可选的姿态模块后，可以轻敲翻页，倾斜、摇晃或翻转屏幕会触发表情。

## 下载后从这里开始

| 你想做什么 | 打开哪里 |
| --- | --- |
| 组装屏幕、接线、打印外壳 | [硬件说明](docs/hardware.md) · [STL 与建模源文件](hardware/enclosure/) |
| 烧录 ESP32 | [固件与配置模板](firmware/desk_display/) · [烧录和 OTA 工具](tools/) |
| 在浏览器看画面 | [网页预演台](preview/) |
| 让设备同步 Mac 的数据和锁屏状态 | [Mac 数据服务](server/) |

下载或解压后，先在终端进入包含本 README 的项目根目录。下文命令以 macOS 为例。

## 1. 准备硬件和工具

硬件使用经典 ESP32 开发板、带 E 线的 64×64 HUB75E 面板（1/32 扫描）和稳压 5V 电源。成品把同一个 5V 电源分成两路，一路给 ESP32 的 USB-C，一路给面板的 4 针电源口。排针、极性、可选 MPU6050／MPU6500 和外壳装配详见 [硬件说明](docs/hardware.md)。

安装 Python 3.9+ 和 Arduino IDE。打开 IDE 设置，把以下稳定版地址加入「附加开发板管理器网址」，再在板卡管理器安装 Espressif 的 **ESP32 3.3.12**。

```text
https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

在库管理器安装 **ArduinoJson 7、Adafruit GFX、ESP32-HUB75-MatrixPanel-DMA**。若 IDE 提示安装 Adafruit BusIO，一并安装。烧录脚本默认从 `~/Documents/Arduino/libraries` 找库。[Espressif 官方安装说明](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html)

## 2. 启动 Mac 数据服务

```bash
python3 server/server.py
```

服务默认监听 `8765`，第一次启动会生成 `.local/token`。保持此终端运行。另开终端，执行 `curl http://127.0.0.1:8765/health`，看到 `"ok": true` 就可以继续。

## 3. 配置自己的 Wi-Fi

```bash
cp firmware/desk_display/config.example.h firmware/desk_display/config.h
```

编辑复制出来的 `config.h`，保留各值外侧的双引号。

| 字段 | 填写内容 |
| --- | --- |
| `DESK_WIFI_SSID` | ESP32 要连接的 **2.4 GHz** Wi-Fi 名称 |
| `DESK_WIFI_PASS` | 这个 Wi-Fi 的密码 |
| `DESK_MAC_HOST` | `http://你的Mac局域网IP:8765`。IP 在 Mac「系统设置 → 网络 → 当前连接 → 详细信息 → TCP/IP」中查看 |
| `DESK_API_TOKEN` | 复制本机 `.local/token` 里的一整行 |
| `DESK_TIMEZONE_OFFSET` | 时区偏移秒数，默认 UTC+8 |

Mac 与 ESP32 需要在可以互相访问的局域网；只有 ESP32 这侧要求 2.4 GHz。真实配置和 token 默认被 Git 忽略。

## 4. 第一次用 USB 烧录

断开外置供电和 HUB75 信号线，再用 USB **数据线**把 ESP32 直连 Mac。查看串口，然后把示例中的串口替换成实际值。

```bash
./tools/firmware.sh info
./tools/firmware.sh flash /dev/cu.usbserial-10
```

脚本会编译并写入固件。烧录后断开 Mac 数据线，再恢复屏幕接线和外置供电。设备第一次取得 Wi-Fi 地址前会显示 SYSTEM。连接后，可以通过 SYSTEM 页读设备 IP，并在浏览器打开该 IP 下的 `/api/status`。`wifi` 和 `mac_online` 都为 true 表示 Wi-Fi 与 Mac 服务已连通。修改 Wi-Fi 后需重新编译烧录。

## 5. 在浏览器预览或无线更新

预演只需要 Python、C++17 编译器、Adafruit GFX 和 ArduinoJson，可在未接屏幕时运行。Mac 上可以通过 `xcode-select --install` 安装编译工具。

```bash
python3 preview/preview.py
```

打开 [本机预演台](http://127.0.0.1:8766)。模拟预览和固件共用绘制代码。设备成功同步数据后，预演台可在「控制真实屏幕」模式切页；也可以把设备 IP 手动写入 `.local/esp32-host`。

首次 USB 烧录后，可以用 `./tools/firmware.sh deploy <ESP32_IP>` 更新固件。将 `<ESP32_IP>` 替换成屏幕显示的实际 IP。OTA 使用步骤 2 生成的 token。

## 许可证

原创代码和外壳模型采用 [MIT 许可证](LICENSE)。第三方库按各自许可证使用。
