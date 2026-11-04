# StickS3 双向语音验收记录：2026-11-04

## 结论

固件与主机代码已实现，StickS3 已烧录，CyberMachine 已通过完整 Debian 包升级。
**不能据此宣称 ATVV 双向语音闭环已打通。** 当前存在两个实机前提阻塞：

1. CyberMachine 确认进入扫描，但没有发现 StickS3，尚未配对；原因尚未定位，不能直接归因于距离。
2. 主机 `GET /api/settings/audio` 返回 `cyberAudioBaseUrl: null`、`tokenConfigured: false`，
   语音为 `enabled: false`，输入与输出都未选择。

首次烧录与整包升级时尚未提交、推送或创建 PR；下文保留该时间点的源码和部署证据，
不代表当前 Git 交付状态。没有保存或猜测 CyberAudio 凭据，语音保持关闭。

## 实现边界

- 产品名称是 CyberController；M5Stack StickS3 为设备适配器。
- 复用 CyberOS `a29e363d4b84ca70ac251c790c5a04a1cbbe05bf` 的 audiod、CyberAudio 客户端、
  任务关联及系统语音会话；主机工作树为 `codex/bluetooth-voice-agent-channel`，含未提交变更。
- 标准 ATVV 1.0、16 kHz 单声道、高半字节优先 IMA-ADPCM 上行；没有新增 HID、Wi-Fi 或 A2DP。
- 独立播放 GATT 服务执行完整段接收、序号/长度/CRC 校验，再解码播放；不边收边播。
- BLE 输出使用 `ble-playback:` ID，必须匹配选中的 ATVV 输入；没有主机音箱回退。
- 转换与 BLE 收发位于非 root 的 audiod；凭据只交给 API。无新语音服务、会话模型或数据库迁移。
- 按住 A 立即停止旧播报，但不取消已经执行的 Agent；迟到结果不转送新设备。
- 旧播放 ID 的取消被忽略，迟到的 ATVV 关麦不清除新一轮开麦请求。
- 配对要求显式模式、屏幕验证码、MITM、Secure Connections 和 bonding；非配对模式的新验证码请求直接断连。

两端规范统一位于 CyberOS 的 `docs/src/content/docs/reference/ble-playback.md`，包含 UUID、
字节序、参考向量、ATT 512 字节属性上限、流控及所有期限。

## 软件证据

| 项目 | 本次结果 | 不能替代的验证 |
| --- | --- | --- |
| 固定依赖固件构建、烧录 | 成功；上传哈希验证通过 | 真实 BLE 配对和声学验收 |
| 固件原生 C++ 测试 | ASAN/UBSAN、`-Wall -Wextra -Werror` 通过 | 无线丢包、设备扬声器完成通知 |
| Rust workspace | Linux arm64 共 510 个测试通过，其中 audiod 15 个 | 实机 ATVV 收音、BLE 播放 |
| Vitest | 248 个文件、1616 个测试通过 | 20 轮真实对话 |
| 文档测试 | 7 个通过，完整站点构建及内部链接检查通过 | 实机操作 |
| 类型与静态检查 | 完整 `npm run typecheck`、`npm run lint` 通过 | 外部服务连通和推理 |
| 完整 Debian 构建 | arm64 包、运行时 smoke、包文件生成通过 | 设备闭环 |

测试覆盖独立 IMA/CRC 向量、ATVV 协商/同步/分片/结束顺序、录音边界、MTU 23/247/517、
ATT 属性最多 512 字节、8 片流控、完整确认、提前完成拒绝、校验失败、取消、断连清理、
接收超时及 `millis()` 回绕、分配失败、WAV 实际转码、输入输出匹配、迟到合成不改投、
BLE 270 秒与原 ALSA 135 秒客户端期限。设置页面、数据库及原协调器包含在全量回归中。

macOS 并行测试曾有一次原有 stale Unix socket 测试失败；该文件未改动。
随后独立运行、串行 audiod 全套及 Linux 全量测试均通过。包构建曾因 GitHub 下载超时失败；
最后使用 72 个逐文件 SHA-256 与既有清单完全一致的原始 Linux 源码缓存，正常完成 DKMS 打包。
没有修改产品源码来绕过这些门禁。

## StickS3 USB 硬件证据

最后一次固件验证时间：2026-11-04 08:55（Asia/Shanghai）。

- 实测 Flash 8 MiB、PSRAM 8 MiB；首次烧录前已读取完整 8 MiB Flash 并保存 SHA-256。
  原始 dump 可能含密钥，仅保留在私有、Git 忽略目录中，权限 0600；未擦除 NVS。
- `SPEAKER_TEST`：一秒 440 Hz、10% 低音量固定音频；扬声器驱动完成事件为 1139 ms。
- 随后的 `MIC_TEST`：得到 64044 字节 WAV，16 kHz、单声道、16 bit、32000 样本、两秒。
  峰值 3040、去直流 RMS 884.72、均值 0.19、削波样本 0。
- 播放切换到采集后，采集完成返回 Disconnected，空闲 PSRAM 恢复为 8384592 字节。
- 上述为 USB 诊断录音，不是 ATVV 无线录音；非零波形不证明语言清晰或首尾音完整。
- `advertising=1`、绑定数 0。08:43 的 CyberMachine 扫描确认开启，设备列表最多 6 项（可能包含缓存），
  但没有发现目标 StickS3。扫描已恢复为关闭；没有添加中继、白名单或临时生产语音服务。
- 没有人工确认实际出声、声压、录音可懂度或真实按键手感。

最终已烧录 `firmware.bin` SHA-256：

```text
ad76c4dbb90a37ec2a2cd7648ff0bb8d4baa68f9a18452dd1fd1e4c876f10102
```

固件固定 PlatformIO 6.1.19、pioarduino 55.03.35 / Arduino ESP32 3.3.5、M5Unified 0.2.29、
M5GFX 0.2.36、NimBLE-Arduino 2.5.2，板型 `esp32-s3-devkitc1-n8r8`。

## 11:08 Mac BLE 扫描复核

用户反馈 MacBook Pro 也未找到设备后，在这台 Mac 上用 CoreBluetooth 直接扫描，
发现正在广播的 StickS3。扫描没有连接或配对设备，也没有修改固件或删除系统绑定。

- 广播 Local Name：`CyberController StickS3`。
- 广播服务：`AB5E0001-5A21-4F05-BC7D-AF01F617B664`，与标准 ATVV UUID 一致。
- 可连接标记为 true；本次显示的目标信号为 −41 至 −37 dBm，共收到目标广播 83 次。
- `system_profiler` 保存的同一地址 `14:C1:9F:D5:4B:1D` 仍名为 `Claude-shinryu-4B1C`，
  类型是旧固件的 Gamepad；CoreBluetooth 同时返回该缓存名和正确的新广播名。
- 固件串口仍为 `advertising=1`、绑定数 0，烧录哈希未变。

因此，已经证实当前固件的 BLE 无线广播可被 Mac 接收；不能再将 Mac 系统蓝牙面板
未显示新名称当作固件未广播的证据。系统设置与通用 BLE 扫描结果需要区分，旧缓存名也需要区分。
检查新广播名应使用 BLE 扫描工具；若要清除旧记录，只处理该地址对应的旧名称。
这次发现仍未证明 CyberMachine 蓝牙可达、配对完成、ATVV 收音或 BLE 播放成功。

原始扫描日志保存在 Git 忽略的 `.context/mac-ble-scan.log`；扫描器退出后已停止扫描。

随后复核 CyberMachine：HCI 追踪确认实际开启了主动 LE 扫描，收到其他设备的 10 条
Advertising Report，未收到 StickS3；另外执行持续 20 秒的 LE-only 扫描也未收到目标。
两个探测均结束并恢复扫描状态，没有配对其他设备。Mac 已收到目标广播，因此接下来需检查
StickS3 与 CyberMachine 所在位置及主机接收链路，不能只根据 Mac 系统面板判定固件故障。

## CyberMachine 整包升级证据

- 安装并验收完成：2026-11-04 08:57:10（Asia/Shanghai）。包版本 2.2.9，源为上述基线加未提交变更。
- 新包 SHA-256：`70eb5b81b0acc553dbf4cf319798f2572bf1f0ba2b5dd95929ee05d6797dd252`。
- 不是组件替换。已核对旧包与安装中的构建元信息，以及旧 audiod / API 实际文件哈希。
- 已保存旧包、SQLite 一致性备份、state/home、终端 home 和各 unit 的启用/运行状态。
- 备份目录：`/cyberos/deploy/backups/sticks3-20261104T005158Z-PEP5lV`。
- 部署记录：`/cyberos/deploy/work.sticks3-jX0V8T/deploy-report.json`。
- 完成运行时激活和至少 30 秒稳定性门禁，再恢复并核对原有 unit 状态；设置表 SHA-256 前后相同。
- SQLite quick check 为 `ok`，`dpkg --verify cyberos` 无差异，systemd 无失败 unit。
- API、worker 和 audiod 均 active，重启计数 0；audiod 实际进程用户为 `cyberos`。
- 小米 `MI RC` 仍可枚举，10 个原有显式主机输出仍存在；语音 daemon 为 `disabled`、无录音、无播放。
- CyberAudio 地址、Token、输入输出选择仍为空；没有自动开启语音或选择扬声器。

包内 `builtAt` 受 Debian `SOURCE_DATE_EPOCH` 控制，不能当作此次实际部署时间；部署时间与包身份
以本节记录、部署日志和 SHA-256 为准。私有日志与测试产物位于两个仓库各自忽略的验证目录。

## 剩余验收与继续条件

先确认 StickS3 与 CyberMachine 蓝牙可达，显式进入配对并完成验证码流程；再在主机 Audio
设置中填写实际 CyberAudio 地址和 Token，选择同一 StickS3 的输入和内置扬声器。首次实测先调低音量。

- [ ] 配对、绑定跨重启、标准 ATVV 协商及按住/松开，得到可听且首尾完整的无线录音。
- [ ] 不依赖 ASR/Agent 的固定音频 BLE 下发：完整接收、CRC、流控、扬声器完成确认。
- [ ] 真实 BLE 取消、断连、超时、损坏包和离线输出；确认缓冲清空且不转向主机音箱。
- [ ] 至少 20 轮真实 ASR → Agent → TTS → BLE → 设备播放对话。
- [ ] 再次录音打断旧播报但不取消 Agent，空转写、繁忙、审批、重启、设备切换与输出离线。
- [ ] 小米真实 ATVV、普通 HID 和现有主机输出的物理回归。
- [ ] 分别记录 ASR、Agent、TTS、BLE 下发和设备播放耗时，并人工确认实际收音和出声。

现有回合记录保存 ASR/Agent/首段 TTS 耗时；audiod 按播放 ID 记录 BLE 下发与设备完成事件耗时。
这些是后续验收的观测入口，不是本次已经测得的真实闭环延迟。未执行上述验收，不得标记为通过。
