# CyberController

CyberController 是面向 CyberOS / CyberMachine 的设备端控制器。设备适配器包括普通 AtomS3 + Voice Pyramid，以及
M5Stack StickS3（ESP32-S3 N8R8、ES8311、AW8737）。不使用 Wi-Fi、A2DP 或 HID，
不保存 CyberAudio 地址、Token 或模型配置。

## 构建与烧录

固定 PlatformIO 6.1.19、pioarduino 55.03.35 / Arduino ESP32 3.3.5、M5Unified 0.2.29、
M5GFX 0.2.36 和 NimBLE-Arduino 2.5.2。Flash 8 MiB、OPI PSRAM 8 MiB，配置见
`platformio.ini`。构建 Python 必须为 3.10–3.13（不要使用 Homebrew 默认 Python 3.14）。

```sh
python3.13 -m venv .context/pio-venv
.context/pio-venv/bin/pip install platformio==6.1.19 PyYAML
.context/pio-venv/bin/pio run -e sticks3
.context/pio-venv/bin/pio run -e sticks3 -t upload --upload-port /dev/cu.usbmodem101
.context/pio-venv/bin/pio device monitor --port /dev/cu.usbmodem101 --baud 115200
```

首次烧录前备份原 Flash/NVS；完整 Flash dump 可能含绑定密钥，不得提交、公开或用于
其他设备。烧录不自动清 NVS，绑定跨重启保留。依赖安装与构建产物不进入 Git。
PlatformIO 工具环境需具备 esptool 的依赖；可复用已经安装完整的 PlatformIO Python 环境。

## 使用

1. 按住 B 2 秒进入 60 秒配对模式。屏幕显示六位验证码，主机在系统蓝牙页配对、信任、
   连接。要求 Secure Connections + MITM + bonding；非配对模式拒绝未知主机。
   已绑定设备只恢复使用 Secure Connections、MITM 和 128 bit 密钥的绑定；旧 Legacy
   绑定需先在主机删除，再按 B 重新配对。验证码在成功或到期后失效，重配对仍需显式开启。
2. CyberOS 设置 → Audio 选择 CyberController StickS3 输入；返回播放选择**同一设备**的
   “StickS3 内置扬声器”。CyberAudio ASR/TTS、凭据和系统语音会话都沿用主机原有配置。
3. 按住 A 录音，松开投递。录音经标准 ATVV 上行；每段回复完整收齐并通过 CRC 后播放。
   再按 A 立即停止旧扬声器，不取消已经执行的 Agent。断连清空本轮录音/播放，不离线补发。
4. 首次扬声器测试从低音量开始。固件把设置百分比映射到 0–64/255 硬上限；这不是
   声压安全认证。ES8311 冷启动/切换后先预热至少 1.1 秒；保持按住 A，屏幕进入 Recording
   后再说话。松开早于准备完成不会投递；仍需实机核验首音完整性。

状态为 Disconnected、Ready、Recording、Waiting reply、Receiving、Playing、Error。
Mac 系统蓝牙面板不能作为完整 BLE 扫描结果。曾使用旧固件时，系统可能仍缓存旧设备名；
BLE 扫描工具中的 Local Name 应为 `CyberController StickS3`，广播服务 UUID 应为标准 ATVV
`ab5e0001-5a21-4f05-bc7d-af01f617b664`。发现广播不等于已配对或完成语音验收。
采集缓冲与扬声器互斥；接收压缩段最多 2 MiB、130 秒；收齐后校验再解码播放。
断连、重新认证均隔离连接状态及排队数据。播放状态通知暂时发送失败时每 20 ms 重试，
连续 5 秒无法发送则断连并丢弃本轮音频；未订阅播放状态的纯 ATVV 输入不受此期限影响。
设备播放期限为音频时长加 5 秒，超时停止扬声器、清理缓冲并报错。
`src/device.hpp` 是设备适配边界，`src/devices/sticks3.*` 承载具体硬件，BLE 和协议不混入
M5 的引脚/编解码器驱动。两端规范统一维护在相邻 CyberOS 仓库的
`docs/src/content/docs/reference/ble-playback.md`；UUID、字节序、CRC 和 golden vectors 以该文档为准。

## 协议测试与 USB 诊断

```sh
python3 tests/run_native.py
```

该命令使用主机 C++ 编译器，以 ASAN/UBSAN 运行协议测试和实际固件主循环、BLE 回调的
回归测试；蓝牙栈和硬件驱动由测试适配器替代。仅运行纯协议测试也可使用：

```sh
c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I include tests/protocol_test.cpp -o /tmp/cybercontroller-protocol-test
/tmp/cybercontroller-protocol-test
```

USB 串口 115200，每条命令以换行结束：

| 命令 | 用途 |
| --- | --- |
| `STATUS` | 连接、协商、状态、Flash/PSRAM/空闲内存诊断 |
| `PAIR` | 与 B 长按相同的显式 60 秒配对模式；验证码只用于当次配对 |
| `SPEAKER_TEST` | 仅未连接 BLE 时，播放一秒 440 Hz、10% 低音量固定音频 |
| `MIC_TEST` | 仅未连接 BLE 时，采集两秒；输出 `MIC_WAV 64044` 后紧接二进制 WAV，随后清内存，不投递 Agent |
| `PTT 1` / `PTT 0` | USB 复现 A 按下/松开，用于标准 ATVV 主机集成诊断；不离线排队 |

协议测试覆盖高半字节优先、独立 IMA/CRC 参考向量、完整接收、错误序号、CRC、超时、
取消及缓冲清理。固件回归覆盖配对窗口及旧绑定、快速重连、认证时序、通知重试与期限、
播放超时、时钟回绕和纯 ATVV 输入。主机另测 MTU 分片、流控、完成确认、输出匹配、WAV 转换和 IPC 超时。
这些测试以及软件的 COMPLETED 都不能代替实际收音、出声、方向键/HID 回归、至少 20 轮
真实对话、审批、繁忙、重启和设备切换验收；声学验收必须另行记录。

本次构建、烧录、主机整包升级的证据与未完成实机验收项见
`docs/validation-2026-11-04.md`。当前不能宣称双向语音闭环已经打通。

## AtomS3 + Voice Pyramid（Wi-Fi）

普通 AtomS3（8 MB Flash、无 PSRAM）使用独立构建；StickS3 继续使用原 BLE 目标：

```sh
pio run -e atoms3-pyramid
pio run -e sticks3
pio run -e atoms3-pyramid -t upload --upload-port /dev/cu.usbmodem3101
```

烧录前先确认 USB 身份并私有备份完整 Flash/NVS；端口可能变化。使用构建工具的默认烧录参数，不要将 bootloader 的 DIO 头强行改成 QIO。
Voice Pyramid 底部 USB-C 负责供电，AtomS3 USB 用于烧录和诊断。

首次启动自动开启 **CyberController-Setup** 热点，屏幕显示八位密码。手机连接后打开
`http://192.168.4.1/`，填写 Wi-Fi 名称/密码、CyberOS 的局域网 `http://主机[:端口]` 地址及独立设备访问令牌。
保存到 NVS 后热点关闭并连接 CyberOS；长按 TP1 两秒可重新配网，必须松开才能再次触发。
有已保存配置时，手动热点五分钟后返回原配置；首次配网一直等待保存。首版不支持 HTTPS、IPv6 字面地址或地址子路径。
设备通过 WebSocket `/api/audio/device` 主动连接主机，不使用 BLE、A2DP 或设备直连模型服务。

CyberOS 需在 API 服务环境中配置至少 32 位随机 `CYBEROS_WIFI_VOICE_TOKEN`（字母、数字、`_`、`-`），
未配置时设备端点保持关闭。手机填写同一个令牌，不能填写 CyberAudio 的 Token。
API 保持 loopback 监听，局域网通过既有 Nginx 入口接入。首版用于可信局域网；设备 HTTP/WebSocket 连接不加密。

按住屏幕按键，进入 Recording 后说话，松开发送。复用 CyberOS 的持久语音会话、ASR、Agent、TTS、历史和审批；
按下立即停止旧播报，不取消 Agent。断连丢弃未提交录音，抑制本轮尚未播放的回复，不自动重试或改投其他扬声器。
TP1 不承担审批或音量操作。USB `PAIR` 兼容命令打开配网，`STATUS`、`MIC_TEST`、`SPEAKER_TEST` 保留。

官方 M5Echo-Pyramid 驱动固定于 `773d837cb1e30452a6e4e8b02de1ad6b97eeffd2`。
16 kHz I2S：SDA38、SCL39、BCLK6、WS8、ESP TX7/RX5，采集左麦克风声道。
网络运行在独立任务，输入输出队列各八帧；音频主循环不执行连接重试。
静态 RAM 约 53 KiB，压缩缓冲最多 64 KiB（131072 样本 / 8.192 秒），收齐并验证 CRC 后增量解码，
不分配整段 PCM，不暂存音频到 Flash。四个 240 帧 DMA 缓冲采用非阻塞读写，实际发送回调确认排空才报告完成。
每段独立编码和播放 ID，主机等上一段 COMPLETED 后发下一段；总音频最多 130 秒，允许段间等待。

音量百分比映射到 codec 0–25%，首轮使用 `SPEAKER_TEST` 的 10% 输入值。驱动完成不代表人工听音通过。
灯效：断连灰、就绪绿、录音红、等待黄、接收蓝、播放青、错误粉红；配网紫色优先。
自动检查、Flash 备份和实机记录见 [2026-11-12 验收记录](docs/validation-2026-11-12-atoms3-pyramid.md)。
