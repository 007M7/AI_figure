# AI Figure 固件 · ESP-IDF 6.1.0

当前交接版本 **0.5.17-handoff**。V1 为 ESP32-C3-MINI-1-H4X；主库 V2 为 ESP32-C3-WROOM-02-N4，沿用相同 GPIO。硬件以 `../../hardware/DESIGN_CONSTRAINTS.md` 和对应版本原理图为准。历史实板证据来自 V1，V2 尚需独立验收。入口为 `main.cpp / app_main()`。WiFi、BLE 配网、NVS、TLS-PSK、GPIO、ADC 和 I2S 均使用原生 ESP-IDF 接口。

完整模块说明、WS/BLE 契约、测试命令、已知问题和回滚步骤见 [固件交接](../../docs/firmware-handoff-2026-11-04.md)。本 PR 不包含电脑端 ASR、LLM、TTS、手机 App 或网关实现。

## 接线

| 功能 | GPIO | 说明 |
|---|---:|---|
| PDM 麦克风 DATA / CLK | 0 / 6 | LMD2718T261-OA1，L/R 接 GND |
| 功放 BCLK / WS / DIN | 4 / 5 / 7 | NS4168，16 位 Philips I2S，左右复制同一单声道 |
| 功放 CTRL | 10 | 低电平关断，高电平选右声道 |
| 录音键 | 3 | 外部 100 kΩ 上拉，按下接 GND |
| LED | 8 | 低电平点亮 |
| 电池 ADC | 1 | R14 上端接 VBAT，1 MΩ / 1 MΩ，C19 100 nF |
| USB 检测 | 20 | VBUS_CHG → 68 kΩ → GPIO20 → 100 kΩ → GND，无内部上下拉 |
| USB Serial-JTAG | 18 / 19 | 烧录和日志；应用不使用 UART |

V1 电池检测网络名为 SYS_ADC，V2 为 VBAT_ADC，采样源均为 VBAT。STAT 不接主控，固件不显示充电状态。

## 电源策略

- 纯电池低于 **3.6 V 持续确认 3 秒**后深睡；低电压停机后按键唤醒，需达到 **3.8 V** 或检测到 USB 才运行。
- 检测到 USB 时允许低电池电压运行，包括只插 USB、不装电池；此时 VBAT 读数不能解释为电量。
- 深睡只启用 **GPIO3 按键唤醒**。USB 插入和定时器均不唤醒。
- 唤醒按键松开后进入启动流程，连接就绪后再次按住录音。按键、录音、等待回复和播放会更新活动时间；纯电池待机 **20 分钟**后再次深睡；USB 供电保持在线待机，拔 USB 后重新计算电池空闲窗口。
- 睡前停录音、功放、WebSocket、WiFi 和 BLE，保持功放关断、LED 熄灭、麦克风时钟低电平。等待录音键松开，再启用低电平唤醒，避免循环重启。
- GPIO1 使用 ADC1 通道 1、12 dB 衰减、eFuse 曲线校准，16 次平均后乘 2 得到 VBAT。启动时等待现有 RC 分压稳定。ADC 校准或读取失败时，纯电池运行停止；USB 下记录无效读数。

2026-10-26：电池采样已从离线自检P项提取到 `battery_adc.h/.cpp`，主程序 `power.cpp` 与自检共同调用。16次原始均值→校准→乘2的流程与原自检一致；64组主机对照和两份固件编译通过。`0.5.10-selftest-adc` 已实刷并联网：原自检同板读1114～1116mV，新主程序网络遥测1164～1368mV，异常仍存在，根因未确认；不以改板或新增电量计为前置。标准chat服务已恢复，设备不再使用旧echo实例。

上述电压是用户确认的**试板初值**，集中在 `config.h`。当前提供电池电压与低电压保护，没有把电压直接换算成未经标定的百分比。两路 LDO 的 EN 仍接 SYS，深睡不能切断整板电源；整板待机电流需测量。

## 音频和网络

ESP32-C3 支持原始 PDM 接收，没有硬件 PDM→PCM 转换。I2S0 的 RX 与 TX 分开申请：RX 原始 PDM 时钟 2.048 MHz，CIC3 /64 后用 63 抽头 FIR /2 转为 16 kHz PCM；TX 在播放时使用独立 I2S 时钟。停止录音和播放时停止相应时钟。

服务器协议是 16 kHz、单声道、PCM16，每 100 ms 一包，见 `../../AGENT.md` §3；播放预缓冲、打断、等待音效和重连逻辑见 `../../AGENT.md` §4、§5。麦克风增益 `MIC_GAIN_Q8=256` 是未做实板标定的初值。

普通启动提供独立的 BLE 离线管理服务（包括无 Wi-Fi 凭据时），首次授权需要实体键确认，支持状态、短鸣、音量、换网和私网服务地址。旧小程序配网继续使用 `espressif/network_provisioning 1.3.1` 的原 UUID 与 Security 1 / 无 PoP，只在启动时按住键 3 秒显式进入。配网成功保存到原 NVS `net/ssid`、`net/pass` 后重启。TLS-PSK 配置仍从本地 `secrets.h` 读取。BLE 与 Wi-Fi/TLS 共存的长稳和手机实机验收尚未完成。

## 构建和烧录

本机 IDF：`C:\esp\v6.1\esp-idf`；工具：`C:\Espressif\tools`。CMake 和组件清单检查 SDK 版本为 6.1.0，目标为 esp32c3。

项目包含中文路径，Windows 工具链会在生成链接规格文件时失败。提供的脚本将源文件复制到本机 TEMP 下的英文路径构建，成功后将产物复制到 `build-idf61/`。临时源文件包含本地 `secrets.h`，应和本地工程一样保管；不上传、不提交。脚本不烧录设备。

在当前固件目录运行：

```powershell
.\tools\build.ps1
```

有需要可传入 `-IdfPath`、`-ToolsPath`。保持现有 `secrets.h`；首次配置可从 `secrets.h.example` 复制并填写。

确认端口和目标板后，在 `build-idf61` 中执行（将 COMx 替换为实际端口）：

```powershell
& 'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe' -m esptool --chip esp32c3 -p COMx -b 460800 --before default-reset --after hard-reset write-flash '@flash_args'
```

4 MB Flash 使用自定义双 OTA 分区，每槽 0x1E0000。NVS 从 0x9000 开始；程序不会自动擦除 NVS。

## 验证与试板

本机 ESP-IDF 6.1.0 完整编译、链接、镜像生成与分区容量检查通过。纯策略 / 音频测试见 `tests/host_tests.cpp`，覆盖低电压连续确认、USB 插拔、恢复门槛、20 分钟超时、计时回绕、PDM 分块连续性、直流去除、1 kHz 通带和 10 kHz 抗混叠。测试不替代实板验收。

在 Visual Studio 开发者终端中，可从当前固件目录执行：

```text
cl /nologo /std:c++17 /EHsc /O2 /utf-8 /Fe:host_tests.exe tests/host_tests.cpp pdm_filter.cpp
host_tests.exe
```

尚待实板验证：录音声压与增益、播放打断、BLE 配网/WSS 长连接、负载压降下的低电压阈值、按键深睡唤醒与整板深睡电流。确认 USB 插入不会从深睡唤醒，纯电池 20 分钟无活动会深睡，USB 在线待机不会因空闲深睡。
