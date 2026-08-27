// config.h —— 板级引脚(来自 xiaozhi-esp32 nologo/xingzhi-cube-1.54tft-wifi) 与连接配置
#pragma once

// ===== WiFi 与服务器地址（私密配置）=====
// WiFi 密码、电脑局域网 IP 因人而异：请复制 config_private.example.h 为
// config_private.h 后填入你自己的值。config_private.h 已被 .gitignore 排除，
// 真实密码不会提交到仓库。以下未在私有文件中定义的项使用占位符兜底。
#if __has_include("config_private.h")
#include "config_private.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID        "你的WiFi名称"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD    "你的WiFi密码"
#endif
#ifndef SERVER_HOST
#define SERVER_HOST      "192.168.1.100"   // 运行 talk-with-anyone 服务端的电脑 IP
#endif
#ifndef SERVER_PORT
#define SERVER_PORT      7862
#endif
#ifndef ACCESS_TOKEN
#define ACCESS_TOKEN     ""   // 服务端 config.json 的 access_token，未启用留空
#endif
// 提示：若上面仍为占位符值，固件能编译但无法联网，请先创建 config_private.h

// ===== 音频参数（与服务端约定一致，勿改）=====
#define MIC_SAMPLE_RATE      16000  // 上行 PCM16 单声道
#define SPK_SAMPLE_RATE      16000  // 下行（鹿小班同款 16k；服务端 audio.start 会覆盖）
#define MIC_SHIFT            14     // INMP441 32bit→16bit 右移位数：14=约4倍增益，无声改16
#define MIC_SLOT_LEFT        1      // 麦克风 L/R 接地=左声道；若录音全静音改成 0
#define SPK_INPUT_GAIN       4.0f   // 输出预增益：音量50%≈原来100%的响度
#define SPK_SOFTCLIP_K       16000  // 软限幅拐点（v域），峰值压缩防破音，上限32000

// ===== 电源（鹿小班第三代 · 卖家源码 luxiaoban-xiaozhi-1.54tft-wifi 确认）=====
#define PIN_BAT_CHG          38     // 充电检测：高电平=充电中
// 电池电压走 ADC2_CHANNEL_6（S3 上即 GPIO17），映射表见 display_ui.cpp

// ===== 板级引脚（鹿小班第三代 · 卖家源码 luxiaoban-xiaozhi-1.54tft-wifi 确认）=====
// 麦克风：PDM 数字麦！GPIO2=CLK, GPIO3=DATA（不是 I2S 麦，勿改回 4/5/6）
#define PIN_MIC_PDM_CLK  2
#define PIN_MIC_PDM_DIN  3
#define MIC_GAIN_SHIFT   5          // PDM 解调后左移增益（卖家源码同款）
// 功放 NS4168：标准 I2S，DOUT=7, BCLK=15, LRCK=16
#define PIN_SPK_DOUT  7
#define PIN_SPK_BCLK  15
#define PIN_SPK_LRCK  16
// 第三代实体按钮（卖家源码确认）：BOOT=GPIO0(对话/打断)，音量+=39，音量-=40
#define PIN_BTN_TALK  0
#define PIN_BTN_VOLUP 39
#define PIN_BTN_VOLDN 40

// ===== 屏幕（ST7789 240x240 SPI）=====
#define DISPLAY_SDA  10
#define DISPLAY_SCL  9
#define DISPLAY_DC   8
#define DISPLAY_CS   14
#define DISPLAY_RES  18
#define DISPLAY_BACKLIGHT_PIN 13

// ===== 行为参数 =====
#define AUTO_LISTEN_ON_BOOT  1          // B方案：服务器就绪后自动进入聆听，免按键
#define RECORD_CHUNK_SAMPLES  512          // 每次发送 512 采样 = 1024 字节 (~32ms)
#define RELEASE_SILENCE_MS    700         // 松开后补发静音，让服务端 VAD 判定语句结束
#define PLAY_WATERMARK_BYTES  (32 * 1024) // 缓冲到该水量才开始播放，降低卡顿
#define LISTEN_TIMEOUT_MS     (3 * 60 * 1000UL)  // 聆听状态无人说话自动退出（3分钟）

// ===== 语音打断（播放期间说话打断 AI 朗读）=====
// 原理：播放中麦克风只测能量不发送（回声不会到服务端 VAD）；
// 用输出能量做回声参考（kEcho 每次起播自动标定），麦克风能量显著高于
// "回声估计值"且持续一段时间判定为用户说话。
// 实测参考（鹿小班三代）：安静环境麦克风均值≈500~2000，用户说话≈10000+。
#define BARGE_IN_ENABLE         1        // 1=启用语音打断；0=仅按键打断
#define BARGE_IN_SPEECH_FLOOR   2000     // 麦克风均值下限（高于环境底，低于人声）
#define BARGE_IN_OUT_FLOOR      1200     // 输出峰保持低于此值=播放间隙
#define BARGE_IN_GATE_MIN       3500     // 触发门限下限（间隙模式/安静段兜底）
#define BARGE_IN_GATE_RATIO     3.0f     // 门限 = kEcho*输出峰保持*该倍数+余量
#define BARGE_IN_SLACK          300      // 门限绝对余量（v域）
#define BARGE_IN_SUSTAIN_MS     250      // 条件需持续命中该时长才触发
#define BARGE_IN_START_DELAY_MS 700      // 起播静默期，跳过起播瞬态与缓冲填充
