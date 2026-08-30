// audio_in.h —— PDM 数字麦克风采集（GPIO2=CLK, GPIO3=DATA，16kHz 单声道）
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace AudioIn {
bool begin();
void end();
// 读一帧 16bit 单声道采样；返回采样数（0=无数据）
size_t read(int16_t *out, size_t maxSamples, uint32_t timeoutMs = 50);
}
