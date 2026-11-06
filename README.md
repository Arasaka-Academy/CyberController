# CyberController

CyberController 是面向 CyberOS / CyberMachine 的设备端控制器。首个设备适配器是
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
