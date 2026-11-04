# ESP32-C3 固件交接 · 0.5.17-handoff

交接日期：2026-11-04。正式固件版本 `0.5.17-handoff`，自检固件版本 `hwtest-2-handoff`。范围为 `firmware/ESP32C3_AI_Voice`、`firmware/ESP32C3_HwTest`、设备规格及本说明。电脑端模型、工具执行器、手机 App 和实验网关均不在此次提交中。

## 1. 接手时先知道这些

- 设备负责按键、PDM 录音、PCM 播放、Wi-Fi/WebSocket、BLE 管理和电源；ASR、LLM、TTS 在外部服务器执行，ESP32 不运行大模型。
- 当前仍为半双工：**按住说话，说完松开**。回复中再次按住可打断并录制新回合。轻点且录音不足 250 ms 时取消，不触发“没听清”。
- 芯片只能连接 2.4 GHz；电脑的上游网络可以为 5 GHz，但需另外提供主板能访问的 2.4 GHz 热点。固件更新不能增加 5 GHz 射频能力。
- 主库 V2 使用 ESP32-C3-WROOM-02-N4，V1 使用 MINI-1-H4X；GPIO 表沿用。历史实板记录来自 V1，不能据此认定 V2 已完成硬件验收。
- 本次整理增加维护修复并标为 0.5.17；历史实板成功记录对应 0.5.16 及之前版本。本次不烧录主板，不把自动测试当作本版实板验收。
- 交付状态为供维护者审查的试点源码，尚有电池采样、BLE 共存长稳及新板验收事项。

## 2. 代码模块与所有权

| 文件／模块 | 职责与维护要点 |
|---|---|
| `main.cpp` | `app_main` 初始化顺序与单主循环；先检查电源，再启用音频和无线；遥测避免在录放音实时路径打印 |
| `config.h`、`CMakeLists.txt` | 引脚、采样率、缓冲、超时、版本；两个版本字符串必须一致 |
| `audio_io.*`、`pdm_filter.*` | I2S 原始 PDM 接收、定点抽取、TX DMA 和功放时序；此次不改滤波算法 |
| `session.*` | IDLE / REC / WAIT / PLAY / ERR；物理按键优先、取消、重连、提示音隔离及试点回执 |
| `button.*` | 25 ms 去抖、一次性边沿事件；新按下清掉旧松开事件；原始低电平也算按键活动 |
| `cue.*`、`bubble.*`、`led.*` | 本地提示音、非阻塞等待音和 LED；新增可返回音频失败的短鸣；录音期间 LED 关闭 |
| `net_wifi.*` | NVS 凭据、指数退避、候选换网、可选的一次性编译配置迁移；换网成功才持久化 |
| `transport.*` | 原生非阻塞 TCP / TLS-PSK；等待 TCP connect 完成、TCP_NODELAY、统一写超时 |
| `net_ws.*` | 固定内存 WS 收发、客户端掩码、背压、断线原因；音频和文本缓冲分开 |
| `proto.*` | 有界设备协议解析，lab 操作编号与音量检查；整数溢出及非整数拒绝 |
| `ring.*` | 16 KB 静态播放环形缓冲；与原版相同，不组装整段回复 |
| `battery_adc.*` | 从原自检提取的 ADC 实现，正式／自检共用；16 次原始均值→eFuse 曲线校准→乘 2 |
| `power.*`、`power_policy.h` | USB 在线待机、电池空闲深睡与低压保护；策略和驱动分离，可做主机测试 |
| `device_settings.*` | NVS 私网服务地址与端口；编译期域名仍作默认值，运行期试点只接受规范 RFC1918 IPv4 |
| `ble_control.*` | 常驻 BLE 管理端点；回调只校验／排队／读快照，主循环持有音频、网络、NVS 修改 |
| `net_prov.*` | 保留旧小程序独立 BLE 配网，仅启动按住 3 秒进入；与常驻管理服务分离 |
| `ESP32C3_HwTest/hwtest_main.cpp` | 离线 L/S/B/M/P 自检，复用正式驱动与共享 ADC，不链接无线或真实凭据 |
| `tools/build.ps1` | Windows ASCII 路径镜像构建，指定 IDF 与工具目录，不自动烧录 |

## 3. 相对原版的行为变化

### 录音、提示音与重连

录音开始前立即关闭 GPIO8 LED；离线自检 M 项同样关闭。该绕行来自同板开关 LED 的对照现象，尚未证明电路根因，也不宣称适用于所有板型的通用噪声修复。

录音中丢弃旧提示音及过期 error，重复 ready 不重置正在进行的录音。断线时终止当前捕获并清理控制；若断线期间一直按住，重连后需先松开，再按住开始新回合，防止旧手势跨连接复用。上行零字节暂时拥塞可丢整帧；半帧已写入则必须关闭连接，避免 WS 边界错位。

默认输出增益为 Q8 200/256，较原 100/256 提升约 6 dB；麦克风增益保持 256/256。服务器等待上限为 60 秒，以容纳本地 TTS 启动；这只是失败上限，不是延迟保证。

### 电源与诊断

USB 在位保持网络待机，不再因空闲 20 分钟深睡；拔 USB 后重新开始电池空闲窗口。电池低于 3600 mV 连续 3 秒深睡，低压睡眠后需 3800 mV 或 USB 才恢复。纯电池 ADC 无效会停止运行；USB 下可运行但记录无效值。**不因测值异常而放宽保护阈值。**

`[BAT]` 输出 ADC 首值、均值、末值、范围、校准后电压及有效性；`[TLM]` 保留 RSSI、堆、丢帧和欠载。串口打开可能触发 USB 复位，观察前应确认串口工具 DTR/RTS 行为。

### 本轮维护审查补充

1. `test_ble_control.py` 移除用户目录／缓存哈希硬编码，支持 `--cjson-dir`、`CJSON_DIR` 和当前构建缓存发现。
2. BLE 候选换网启动后应用与普通开机相同的 `WIFI_SLEEP_OFF` 策略。
3. 协议整数解析检查 int32 边界，拒绝超长数字、小数和指数形式；增加回归。
4. 正式构建要求当前源码存在 `secrets.h`，且不沿用已经从源码删除的 `ble_gateway_private.h`；避免缓存悄悄保留旧凭据。
5. README 同步 USB 在线待机、常驻 BLE、录音灭灯以及 V1/V2 下载入口。

## 4. WS 协议与服务器兼容

基础链路不变：16 kHz / PCM16LE / 单声道，录音每 100 ms 发送 3200 字节 binary；文本控制为 JSON。服务器必须理解 turn_start / turn_end / abort，并发送 ready / audio_begin / binary / audio_end。播放预缓冲 9600 字节，ring 16384 字节；未知消息静默忽略。

hello 追加能力 `lab=1`、`mobile_voice=1`、`desktop_standby=1`、`mobile_interrupt=1`、`volume_control=1`，以及 `up_ms`、`dc`、`ud`。旧服务器忽略额外字段即可继续物理按键对话。设备文本发送最大 256 字节，RFC WS 控制帧仍最大 125 字节；设备号过长导致 hello 超界会明确失败并退避。

`dc`：0 开机、1 半帧写入、2 TCP 丢失、3 pong 超时、4 远端 close、5 其他本地关闭。`ud` 为累计上行丢包数，不是本次连接计数。

| 试点下行扩展 | 实际行为 |
|---|---|
| `lab_identify` + `id` | 空闲短鸣；成功写入 I2S 后 completed，不代表人工听到 |
| `lab_voice` + `id` | 空闲时进入 WAIT 并返回 running；外部服务器再执行手机音频／文字到回答音频的链路；播放排空后 completed |
| `lab_volume` + `id` + `volume` | 0～100 整数映射 Q8 增益；IDLE/WAIT/PLAY 可调，实体录音及按键活动时拒绝 |
| `lab_interrupt` + `id` | 发 abort、关闭播放、清 ring；完成停止后回执；实体录音优先，不强制抢占 |

`id` 为 32 位小写十六进制。上行 `lab_result` 含 id/status/code，终态为 completed/failed/rejected；保留最近四个终态去重，**没有跨复位的 exactly-once 保证**。结果未知的写操作不得自动重放。网络断线可能导致最终回执无法送达，客户端应表示 unknown，而非推断成功。

`lab_status` 含 state（IDLE/REC/WAIT/PLAY/ERR）、usb、mv、rssi、volume。状态或音量变化立即尝试上报，稳定态约 2 秒；网络传输和客户端刷新仍可能延迟，不是实时硬保证。固件不新增匿名 HTTP 服务；这些扩展须由外部鉴权网关承接。本次 PR 不包含该网关，不能仅靠烧录获得手机控制功能。

## 5. BLE 管理契约

- 常驻管理服务 UUID：`10624C9B-F2AA-4CA8-8594-9CD3CC78DB3F`；旧配网为 `10624C9A-F2AA-4CA8-8594-9CD3CC78DB3F`。设备名 `AIFIG_` 加 MAC 后三字节，两服务不能混用。
- Security 1 / 无 PoP；端点 prov-session=FF51、proto-ver=FF53、device-control=FF54。客户端须按 protocomm 连续加密流处理请求与响应，传输错误后重建安全会话。
- 管理消息为最大 384 字节的扁平 JSON。`o` 操作、`k` 为 64 位小写 hex 随机所有者凭据、写操作 `i` 为 32 位小写 hex。嵌套和数组拒绝。
- 首次 `auth` 后 60 秒内先松开、再按主板键确认；只有 NVS 保存成功才绑定。该手势被消耗，不录音。已有所有者时其他密钥拒绝，无自动接管／转让入口。
- `status` 只读；`identify`、`volume(n:0..100)`、`wifi(s:SSID,p:密码)`、`server(s:私网IP,n:端口)` 先 accepted，后以 `result(i)` 查询实际结果。四个终态去重，忙碌拒绝。音量可在播放中调整，其余设置要求空闲。
- 候选 Wi-Fi 在 35 秒内获得 IP 后保存 NVS；失败恢复旧配置。保存一组网络，不是多网漫游。
- `gateway` 仅向已授权会话提供可选联调网关地址和凭据；默认未注入时 credential 为空。可在本地 `ble_gateway_private.h` 定义 `BLE_GATEWAY_CREDENTIAL`，该头文件必须保密且忽略提交。它是试点引导机制，尚不是生产账号体系。
- BLE 所有者凭据、Wi-Fi 和运行期服务地址均存 NVS，当前不启用产品级安全启动、Flash/NVS 加密；不能把 Security 1 加物理确认描述为完整量产安全方案。

## 6. 依赖与复现

要求 ESP-IDF **6.1.0**、esp32c3、4 MB Flash、无 PSRAM。依赖锁定：network_provisioning 1.3.1、cjson 1.7.19~2、json_generator 2.0.0；保留 `dependencies.lock`。不把第三方源码复制进本次提交。

在仓库根目录 PowerShell 执行（替换工具路径）：

```powershell
Copy-Item firmware/ESP32C3_AI_Voice/secrets.h.example firmware/ESP32C3_AI_Voice/secrets.h
# 在本机编辑 secrets.h：Wi-Fi、服务地址、TLS 与设备身份；不要提交。
& firmware/ESP32C3_AI_Voice/tools/build.ps1 -IdfPath D:\esp\v6.1\esp-idf -ToolsPath D:\Espressif\tools
& firmware/ESP32C3_HwTest/tools/build.ps1 -IdfPath D:\esp\v6.1\esp-idf -ToolsPath D:\Espressif\tools
```

Windows 构建脚本在 TEMP 下创建 ASCII 路径镜像，包含私有配置时与原工程同等保密。可将 TEMP/TMP 指向有足够空间的英文路径。正式程序产物为 build-idf61/ai_figure.bin；自检为 build-idf61/hwtest-merged.bin。本次验证使用隔离源码和**虚拟凭据**，验证镜像不用于部署，也不随 Git 提交。

主机测试需要 Python 3、原生 g++/gcc（例如 MinGW），先将编译器加入 PATH：

```powershell
g++ -std=c++17 -O2 -Wall -Wextra -Werror -o host_tests.exe firmware/ESP32C3_AI_Voice/tests/host_tests.cpp firmware/ESP32C3_AI_Voice/pdm_filter.cpp
.\host_tests.exe
python firmware/ESP32C3_AI_Voice/tools/test_button.py
python firmware/ESP32C3_AI_Voice/tools/test_battery_adc.py
python firmware/ESP32C3_AI_Voice/tools/test_session_recovery.py
python firmware/ESP32C3_AI_Voice/tools/test_ble_control.py --cjson-dir <当前构建缓存中的espressif__cjson/cJSON目录>
```

BLE 测试使用锁定组件原版 cJSON.c/h，模拟 GPIO、无线、NVS；不访问串口、不读取真实私钥。Windows 下依赖安装及主机编译器是维护者前置环境，不是仓库附带的二进制工具。

## 7. 本次验证及相似度口径

结果与逐文件源码哈希见 [固件审查清单](firmware-review-2026-11-04.json)。对比基线是 2026-11-04 拉取的主库 master `cc74723`。同时验证两条提交分支中 firmware 树完全一致。

相似度衡量同一路径 C/C++ 源码的行级公共块：`2 × 公共行数 /（基线总行数 + 本版总行数）`，新增文件计入本版分母；另列同路径文件相似度和逐字节未改文件数。这是**与本仓库基线的改动范围指标**，不是互联网抄袭检测，也不证明代码正确。正确性另由代码审查、主机回归和实际构建检查。

本次结果为 **86.76%**：基线 2895 行、本版 3541 行、公共 2792 行；34 个同路径源文件中 12 个逐字节未改，新增 6 个 C/C++ 源文件。五套主机测试全部通过；正式 TLS 镜像 1,366,928 字节、明文镜像 1,367,088 字节，应用槽约剩 30.47%；自检应用 226,320 字节，自检槽约剩 78.42%。三套构建成功，未出现编译器 warning 行；SDK 的 CMake 配置警告不计作编译器 warning。镜像仅用于合成凭据构建校验，未发布为可烧录包。

本次检查覆盖：电源策略（含模拟 USB 48 小时和计时回绕）、PDM 分块连续性／通带／抗混叠、共享 ADC 原自检等价 64 组及错误隔离、按键去抖／旧松开、过期提示音和 ready、断线按键重置、短按取消、手机回合准入／完成／超时、控制去重／物理优先、真实 WS 零写入／半帧／文本上限、BLE 授权／离线控制／换网保存与恢复。构建检查正式 TLS 和局域网明文两条编译路径及自检镜像容量。

## 8. 尚未关闭的验收项

| 项目 | 已知证据与下一步 |
|---|---|
| 电池电压异常 | 历史 V1 自检和正式程序均出现明显偏低值；共享 ADC 仅消除实现差异，未修复电路／测量根因。需万用表核对 VBAT、GPIO1 分压、USB 检测，勿伪造电量百分比 |
| 持续闪灯与录音断线 | 历史有 Wi-Fi 仍在线而 WS 录音上行丢帧、重连的记录。现有发送期限和整帧丢弃策略不等同于根因关闭；需对应服务日志、dc/ud、RSSI 及热点拓扑做实板复测 |
| V2 新板 | 引脚兼容来自主库设计，录音、播放、BOOT/RESET、电源和 RF 均需 V2 实测 |
| 常驻 BLE 与 Wi-Fi/TLS | 主机和构建通过；手机实际配对、长写入、连续录放音、堆碎片与多次重连仍待长期实板测试 |
| USB 在线待机 | 策略模拟 48 小时通过，不能替代真实整板长期待机、供电压降和耗电测量 |
| TLS | TLS 编译路径验证；实际服务器 PSK 握手、长连与压力仍需部署环境验收 |
| 手机／网关 | 需另外部署有鉴权、结果 unknown 处理及去重的服务；本 PR 不交付手机 UI、DNS-SD 或网关 HTTP API |

## 9. 烧录、迁移与回滚

1. 确认板版本、USB 数据线、端口与供电。更新前备份整片 Flash 到私有位置；备份含所有者密钥和网络凭据，禁止上传。
2. 读回分区表和当前活动 OTA 槽，不能假定所有板都从 ota_0 启动。现有表：NVS 0x9000/0x6000，otadata 0xf000，ota_0 0x20000/0x1e0000，ota_1 0x200000/0x1e0000。
3. 已部署板只写确认后的活动应用槽并校验；不要为了更新应用使用整片擦除或重写 otadata。全新板才按构建 flash_args 布局安装 bootloader、分区表和应用。
4. 自检合并镜像从 0x0 写入，属于单独固件；会覆盖引导和分区布局，切换前备份，测完按正式布局恢复。不要把自检刷入当作普通应用槽升级。
5. 旧 NVS net/ssid、net/pass 保留；新增 control/owner、control/server。WIFI_PROFILE_REVISION 默认不启用；显式更改该值会一次性覆盖 Wi-Fi，版本不变时尊重后续 BLE 配网。回滚旧编译配置时也应检查该迁移开关。
6. V2 下载：USB 保持插入，按住 SW2/BOOT，轻按 SW1/RESET，1 秒后松开 BOOT。V1：TP3 为 BOOT、TP2 为 GND、TP1 为 EN，按对应自检包步骤进入下载。
7. 回滚优先写回同板备份的原活动应用槽，保持 NVS 和 otadata。回退旧版后新增 NVS 键通常被忽略，但常驻 BLE／手机控制能力可能消失，客户端必须依据 hello 能力降级。

## 10. 维护者验收顺序

先复现本说明的主机测试和两套构建，再在 USB 供电下核对 hello/ready。完成三次 10～15 秒按住说话、连续长回答播放、播放中打断和短按取消；对照每次 turn_start/turn_end、dc/ud 与声音。随后做断网重连并确认旧按住不会自动开始新回合。独立验收 BLE 首次实体授权、离线短鸣／音量、正确与错误 Wi-Fi 的保存／回退。最后以实测电压为前提验证纯电池深睡、唤醒及长稳；责任人由主库维护者指定。

此次提供源码与审查材料，未自动合并主库、未更新在用主板、未声称上述开放验收项已完成。
