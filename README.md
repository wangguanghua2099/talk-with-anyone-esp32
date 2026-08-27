# Talk With Anyone · ESP32 小音箱前端

[English](README.en.md) | 简体中文

为 [talk-with-anyone](https://github.com/wangguanghua2099) 本地 TTS 语音聊天服务端配套的
ESP32-S3 语音聊天音箱固件：说话 → 识别 → AI 回复朗读 → 屏幕同步显示中文字幕，
全自动连续对话，无需反复按键。

## 功能

- **全自动语音对话**：开机联网后自动进入聆听，说完即答，答完继续听
- **中文字幕**：1.54 寸 240×240 彩屏，用户/AI 双色字幕，打字机逐字显示
- **防闪烁显示**：逐行差异比对 + 增量续画，打字时只有新增字符区域在动，
  不清屏、不整屏重绘，长时间注视不刺眼（零额外显存分配）
- **按键操作**：BOOT 键 = 打断/重新进入对话；音量 +/- 实体键，音量掉电保存
- **状态指示**：顶栏实时显示 LISTEN / PLAY / IDLE / OFFLINE 与音量

## 硬件

开发与验证基于 **鹿小班（小智同款）1.54 寸屏 WiFi 版**（ESP32-S3，8MB Flash / 8MB PSRAM）：

| 部件 | 规格 | 引脚 |
|------|------|------|
| 麦克风 | PDM 数字麦 | CLK=GPIO2, DATA=GPIO3 |
| 功放 | NS4168 I2S | DOUT=7, BCLK=15, LRCK=16 |
| 屏幕 | ST7789 240×240 SPI | MOSI=10, SCLK=9, DC=8, CS=14, RST=18, 背光=13 |
| 按键 | BOOT=GPIO0，音量+=39，音量-=40 | |

全部引脚定义集中在 [`firmware/src/config.h`](firmware/src/config.h)，换板子改这一个文件即可。

## 快速开始

1. **准备服务端**：先在本机或局域网内跑起 talk-with-anyone 服务端（默认端口 7862）。

2. **配置私密信息**（WiFi 密码、服务端 IP 不会进入仓库）：

   ```bash
   cd firmware/src
   cp config_private.example.h config_private.h
   ```

   编辑 `config_private.h`，填入：
   - `WIFI_SSID` / `WIFI_PASSWORD`：音箱要连的 WiFi（需与服务端同一网络）
   - `SERVER_HOST`：运行服务端的电脑局域网 IPv4（Windows 用 `ipconfig` 查看；
     建议在路由器后台给电脑绑定静态 IP）

3. **编译烧录**（需已安装 [PlatformIO](https://platformio.org/)）：

   ```bash
   cd firmware
   pio run                # 编译
   pio run -t upload      # 烧录（USB 连接音箱）
   pio device monitor     # 查看串口日志（115200）
   ```

4. 开机后音箱自动联网、自动进入聆听状态，直接说话即可。

## 工作原理

```
麦克风 PDM ──PCM16@16k──▶ /ws/voice ──▶ 服务端 ASR + LLM
                                      ◀── asr.result / assistant.completed（字幕）
屏幕字幕   ◀── 文本                    │
扬声器     ◀── /ws/tts-stream ◀── PCM16 音频流（环形缓冲 + 重采样）
```

- 上行：PCM16 单声道 16kHz 二进制帧
- 下行：TTS 音频流 + 文本事件，字幕与朗读同步推进
- 详见 [`firmware/src/`](firmware/src/)：`main.cpp`（状态机）、`voice_client.cpp`（协议）、
  `audio_in/out.cpp`（音频）、`display_ui.cpp`（字幕显示）

## 目录说明

```
firmware/            PlatformIO 工程（本项目主体）
  src/               固件源码
  platformio.ini     构建配置
config_private.h     私密配置（自行创建，不入库）
```

## 许可证

[MIT](LICENSE)
