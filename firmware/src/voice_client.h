// voice_client.h —— /ws/voice 与 /ws/tts-stream 双 WebSocket 客户端
#pragma once
#include <Arduino.h>
#include <WebSocketsClient.h>

namespace VoiceClient {
bool begin();                 // WiFi 就绪后调用：建立两条 wss 连接
void loop();                  // 主循环调用：收包/重连/发送待发文本

bool isUp();                  // voice 通道是否可用
bool startRecording();        // 开始上行（安装麦克风）
void pauseMic();              // 暂停上行（不补静音尾，用于播放前防回声）
void stopRecordingAndFlush(); // 停止上行并补静音尾巴，触发服务端 VAD 收句
void sendInterrupt();         // 打断 AI（服务端会 cancel LLM + 停 TTS）
void requestTts(const String &text);   // 请求合成播放回复
uint32_t msSinceLastVoiceActivity();   // 距最近一次 VAD 说话事件的毫秒数

// 供 main.cpp 录音循环直接取 socket 发二进制帧
namespace Detail {
WebSocketsClient &voiceSocket();
}
}
