# AtomS3 + Voice Pyramid Wi-Fi 适配验收（2026-11-12）

## 最终设计与源码

按用户后续指令，由 BLE 改为 Wi-Fi，并采用设备热点 + 手机网页配置。AtomS3 主动连接 CyberOS 局域网 `/api/audio/device` WebSocket，配置 Wi-Fi、CyberOS HTTP 地址和独立设备访问令牌。StickS3 保留原 BLE 目标，未合入此前临时实现的主机 BLE 分段改动。

CyberController 基线 `ab4a0043d9e11d309f8b1d274a51df4fed3ac4a9`；CyberOS 基线 `eca2792a6604671558f2699e10429118f2736da5`。开发分支均为 `codex/atoms3-wifi-voice`；用户已授权开发完成后提交、合并 main 并推送。

普通 AtomS3 使用 8 MB Flash、无 PSRAM，官方 M5Echo-Pyramid 固定于 `773d837cb1e30452a6e4e8b02de1ad6b97eeffd2`。16 kHz 单声道、按住屏幕录音、TP1 两秒打开配置热点。64 KiB 压缩缓冲、增量解码、四个 240 帧 DMA 缓冲；实际发送回调确认排空才报告完成。网络独立任务与双向各八帧队列，不阻塞音频主循环。NVS 单个版本化配置块原子保存，保存失败明确报错。

CyberOS 复用现有语音协调器、持久会话、ASR、Agent、TTS、归档和审批，Wi-Fi 记录来源为 device，回复绑定发起连接。主机按 PCM 分段后独立编码/CRC/ID，等 COMPLETED 后发下一段。按下、取消、断连或失败终止后续音频，不取消 Agent、不改投主机。新增一个设备 WebSocket 接口，无数据库迁移，不修改 audiod。

## 自动验证

- 四组原生 C++ 测试通过 ASAN/UBSAN：协议/配置地址、应用与 StickS3 BLE、真实 Pyramid 适配器配假硬件 API、真实 Wi-Fi 队列配假连接启动。覆盖缓冲上限、分块与奇数样本、CRC、部分写入、DMA 实际排空、触摸锁存、取消、断连、分配失败、驱动错误及 READY 与主循环竞争时保留新会话握手。
- 两个 PlatformIO 目标均成功构建。AtomS3 静态 RAM 约 53 KiB，Flash 约 1.41 MB；实际动态可用内存仍需完整硬件初始化后验证。
- 最终 Vitest：273 文件通过，1829 项通过、5 项跳过。新增真实 WebSocket 认证/录音帧/断连测试、Wi-Fi 来源与审批/历史/回复路由测试、跨段独立编码、奇数尾样本、失败/取消/断连后停止及反压测试。
- CyberOS typecheck、全仓 fmt/Clippy/ESLint、完整 `npm run build`（Rust release workspace、所有 TS/Web、文档及搜索/链接检查）通过。
- Rust workspace 默认并发曾出现 indexer 慢扫描租约和 terminald stale socket 的既有时序失败；indexer 单独复测通过，最终串行全仓测试全部通过：`cargo +1.95.0 test --offline --locked --workspace -- --test-threads=1`。
- 文档测试 8 通过；iSCSI helper 23 项，2 跳过。
- 仓库路径带空格时，原 Vitest alias 使用 URL.pathname 留下 `%20`。验证使用忽略目录中的配置仅解码 alias，未修改产品配置。

## 构建工具与下载校验

PlatformIO 6.1.19 / Python 3.13；pioarduino 55.03.35 对应源码 tag `879780c5a96fe00c2d4df0be5e5fcb0c0c48c83f`。网络受阻时忽略配置引用同 tag 的本地平台；外部工具清单 URL 临时改为下载归档路径，产品 platformio.ini 保持原版本 URL。安装器存在 `idf_tools.py rc=1` 的环境诊断，但实际编译、链接和镜像生成均成功。

Arduino 3.3.5 归档通过 Espressif 官方镜像下载，并与 GitHub release asset SHA-256 比对一致：

- framework `84bf5027214f11ff67cae0bcbcdc2f46c498ff319e787bd6b7a8c253545c50e8`
- libs `394170d0dbe2c728c8985129275baea61c61d56eb976873ce4cae197a12c0ed0`
- M5GFX 0.2.36 `2ace355a57021b4a93bd38d3c51ec1a34561af91697f8885f70544b6f46b5684`，与 PlatformIO registry 比对一致。

## USB、私有备份与烧录

通过 esptool 确认 `/dev/cu.usbmodem3101`：ESP32-S3 QFN56 rev0.1，8 MB GD Flash，MAC `dc:54:75:cd:fb:a0`，ROM USB VID/PID `303a:1001`。

完整 8388608 字节原 Flash（包含原 NVS）已私有备份至忽略目录 `.context/device-backup/atoms3-dc5475cdfba0-original.bin`；目录 0700、备份 0600。SHA-256：`53d9b83adf4cb5435abd6ab8605c2e18db63307a8234aeded68b0c0d308b15e0`。首次高波特率读取中断，改用 115200 和 256 KiB 分块后完成；烧录前再次校验大小和哈希。

已烧录 Wi-Fi 固件，esptool 验证写入哈希。原生 USB 初次复位会重新枚举，首次配置端口失败后重试成功。一次手工烧录误设 QIO 导致 ROM loader 失败，已按构建镜像 DIO 头修正 bootloader，设备随后正常进入应用。

## 实机结果与明确阻塞

时钟 0x60、麦克风 ADC 0x40、codec 0x18、STM32 0x1a 可探测。功放 0x5b 在 STM32 释放复位后仍不响应，应用安全停在初始化错误。STM32 诊断读取 `bottom_usb_mv=998`；已请用户确认并接通 Voice Pyramid 底部 USB-C 稳定电源，AtomS3 USB 保持电脑连接。

因此尚未完成：热点网页真实访问与保存、Wi-Fi 重连、实际动态内存、低音量出声、清晰收音、超过 8.192 秒回复及至少 20 轮真实对话。人工听音尚未进行；模拟测试与软件完成不代表声学闭环通过。

## CyberMachine 部署

SSH、sudo 和已安装 API/audiod 可达。最终尝试既有组件部署 `--host cybermachine --components api,docs --build remote`，尝试 ID `dev-20261112060753813-44577`。

部署保护在任何替换或停服之前拒绝：本地与已安装基线的 `package-lock.json`、`packaging/debian/changelog`、`packaging/debian/install` 不一致，要求整包升级。没有绕过保护或执行整包升级，没有修改服务环境、凭据、数据库、已选音频设备或生产服务。独立 `CYBEROS_WIFI_VOICE_TOKEN` 尚未部署；硬件联网闭环同时受此阻塞。

本机 Docker 可执行文件 CPU 架构不兼容，因而显式选择 remote build，未做自动降级。没有成功部署回执。两仓源码提交/推送不表示生产部署或实机验收完成。
