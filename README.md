# Talk With Anyone · ESP32 小音箱前端

[English](README.en.md) | 简体中文

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

为本地语音聊天服务端 **[talk-with-anyone](https://github.com/wangguanghua2099/talk-with-anyone)**
配套的 ESP32-S3 语音聊天音箱固件。按一下按键（或开机自动）进入对话：

> 说话 → 云端/本地识别 → AI 回复朗读 → 屏幕同步显示中文字幕 → 自动继续聆听

数据全程只在你的电脑与音箱之间流转，不经过任何第三方服务器。

## 功能特性

- **全自动连续对话**：联网后自动进入聆听，说完即答，答完继续听，无需反复按键
- **中文字幕**：1.54 寸 240×240 彩屏，用户（绿）/ AI（青）双色字幕，打字机逐字显示，
  自动换行、满屏上滚，保留最近 80 行历史
- **防闪烁显示**：逐行差异比对 + 增量续画的局部重绘，打字时只有新增字符在动，
  不清屏、不整屏重绘，长时间注视不刺眼（零额外显存分配）
- **按键操作**：BOOT 键 = 打断播放 / 进入或退出对话；音量 +/- 实体键，±10 步进，
  掉电保存
- **状态顶栏**：对话状态（LISTEN/PLAY/IDLE/OFFLINE 四色）、NTP 对时的时间、
  WiFi 信号三格、电池电量与充电指示
- **音质处理**：服务端音频线性插值重采样到 16k 输出；音量平方曲线 + 输入预增益 +
  软限幅，中低音量下也有充足响度且不破音

## 硬件

开发与验证基于 **鹿小班第三代 · 小智 1.54 寸屏 WiFi 版**（ESP32-S3，8MB Flash + 8MB PSRAM）：

| 部件 | 规格 | 引脚 |
|------|------|------|
| 麦克风 | PDM 数字麦 | CLK=GPIO2, DATA=GPIO3 |
| 功放 | NS4168 I2S | DOUT=7, BCLK=15, LRCK=16 |
| 屏幕 | ST7789 240×240 SPI | MOSI=10, SCLK=9, DC=8, CS=14, RST=18, 背光=13 |
| 按键 | BOOT / 音量+ / 音量− | GPIO0 / GPIO39 / GPIO40 |
| 电池 | 电压 ADC2_CH6（GPIO17），充电检测 GPIO38 | |

全部引脚集中在 [`firmware/src/config.h`](firmware/src/config.h)，
换板子只需改这一个文件（外加 `display_ui.cpp` 里的屏幕驱动类）。

## 快速开始

### 前置要求

- 已安装 [PlatformIO](https://platformio.org/)（VSCode 插件或 CLI 均可）
- 一台运行 [talk-with-anyone](https://github.com/wangguanghua2099/talk-with-anyone)
  服务端的电脑（默认端口 7862，详见其仓库说明）
- 音箱与该电脑在同一局域网

### 步骤

1. **配置私密信息**（WiFi 密码、服务端 IP 不会进入仓库）：

   ```bash
   cd firmware/src
   cp config_private.example.h config_private.h
   ```

   编辑 `config_private.h`，填入：
   - `WIFI_SSID` / `WIFI_PASSWORD`：音箱要连的 WiFi（需与服务端同一网络，热点也可以）
   - `SERVER_HOST`：运行服务端的电脑局域网 IPv4（Windows 用 `ipconfig` 查看；
     建议在路由器后台给电脑绑定静态 IP）

2. **编译烧录**：

   ```bash
   cd firmware
   pio run                # 编译
   pio run -t upload      # 烧录（USB 连接音箱）
   pio device monitor     # 查看串口日志（115200）
   ```

3. **开始对话**：开机后音箱自动联网并进入聆听状态，直接说话即可。
   - AI 播放中按一下 BOOT 键：打断当前回复并继续聆听
   - 聆听中按一下 BOOT 键：退出对话，进入待机（待机下再按重新开始）
   - 音量 +/−：实时调节并保存，屏幕顶栏短暂显示

## 工作原理

```
麦克风 PDM ──PCM16@16k──▶ WebSocket /ws/voice ──▶ 服务端 ASR ─▶ LLM
        （按键打断时发送 interrupt 控制帧）              │
屏幕字幕   ◀── asr.result / assistant.completed 文本事件 ┘
扬声器     ◀── WebSocket /ws/tts-stream ◀── PCM16 音频流
                （环形缓冲 + 线性插值重采样 → I2S 32bit 输出）
```

| 模块 | 文件 | 职责 |
|------|------|------|
| 状态机 | [`main.cpp`](firmware/src/main.cpp) | 待机/聆听/播放三态流转、按键、音量 |
| 协议 | [`voice_client.cpp`](firmware/src/voice_client.cpp) | 双 WebSocket 通道、自动重连、interrupt |
| 采集 | [`audio_in.cpp`](firmware/src/audio_in.cpp) | PDM 麦克风 16k 单声道 |
| 播放 | [`audio_out.cpp`](firmware/src/audio_out.cpp) | 环形缓冲、重采样、音量曲线 |
| 字幕 | [`display_ui.cpp`](firmware/src/display_ui.cpp) | 局部重绘、打字机、顶栏 |

## 常见问题

- **说话没有反应**：串口看 `[麦克风] 峰值`。说话时应到 10000+；若一直接近 0，
  调 `config.h` 的 `MIC_GAIN_SHIFT`（无声加大）或检查 PDM 接线。
- **音量太小/太大**：调 `SPK_INPUT_GAIN`（默认 4.0，配合软限幅防破音）。
- **连不上服务端**：确认音箱与电脑在同一网段、电脑防火墙放行 7862 端口、
  `SERVER_HOST` 是电脑当前 IP（路由器 DHCP 可能会变，建议绑定静态 IP）。
- **时间显示 `--:--`**：NTP 尚未同步，检查路由器是否放行 UDP 123（NTP）出站。

## 已知限制

- 语音打断（说话即打断）在本硬件上暂未实现，打断请使用 BOOT 按键。
- 屏幕驱动基于 LovyanGFX 的 ST7789 通用面板配置，其他尺寸屏幕需自行适配。

## 致谢

- [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) —— 小智 AI 生态与本硬件的来源项目
- [LovyanGFX](https://github.com/lovyan03/LovyanGFX)、
  [WebSockets](https://github.com/Links2004/arduinoWebSockets)、
  [ArduinoJson](https://github.com/bblanchon/ArduinoJson)
- [talk-with-anyone](https://github.com/wangguanghua2099/talk-with-anyone) —— 配套服务端

## 许可证

[MIT](LICENSE)
