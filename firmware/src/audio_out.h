// audio_out.h —— 扬声器输出：16k 固定输出 + 服务端音频重采样 + 软件音量
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace AudioOut {
bool begin();
void selfTest();                                // 开机自检音（验证扬声器通路）
// 流式接口：服务端 audio.start 后调用；内部自动重采样到 16k 输出
bool beginStream(uint32_t inputRate);
size_t feed(const uint8_t *mono16, size_t bytes);
void finishStream();
void stopPlayback();
void loop();
size_t buffered();
bool active();

void setVolume(int v);                          // 0~100（软件音量，平方曲线）
int  getVolume();
void playTone(uint32_t freqHz, uint32_t durationMs);   // 状态提示音
}
