// audio_in.cpp —— PDM 麦克风采集（与卖家源码 NoAudioCodecSimplexPdm 对齐）
// PDM 模式：ws_io_num 引脚作时钟(CLK)，data_in_num 作数据(DATA)，
// 硬件解调输出 16bit；软件左移 MIC_GAIN_SHIFT 位补增益并限幅。
#include <Arduino.h>
#include <driver/i2s.h>
#include "config.h"
#include "audio_in.h"

namespace AudioIn {

static bool s_installed = false;

bool begin() {
    if (s_installed) return true;

    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
    cfg.sample_rate = MIC_SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;   // PDM 固定 16bit 输出
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 256;

    // PDM RX: ws 引脚 = 时钟，data_in = 数据
    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = I2S_PIN_NO_CHANGE;
    pins.ws_io_num = PIN_MIC_PDM_CLK;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num = PIN_MIC_PDM_DIN;

    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) {
        Serial.println("[AudioIn] PDM 驱动安装失败");
        return false;
    }
    if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
        i2s_driver_uninstall(I2S_NUM_0);
        Serial.println("[AudioIn] PDM 引脚设置失败");
        return false;
    }
    i2s_zero_dma_buffer(I2S_NUM_0);
    s_installed = true;
    Serial.println("[AudioIn] PDM 麦克风就绪 (CLK=IO2 DATA=IO3)");
    return true;
}

void end() {
    if (!s_installed) return;
    i2s_driver_uninstall(I2S_NUM_0);
    s_installed = false;
}

size_t read(int16_t *out, size_t maxSamples, uint32_t timeoutMs) {
    if (!s_installed || !out || !maxSamples) return 0;

    static int16_t raw[RECORD_CHUNK_SAMPLES];
    size_t want = (maxSamples < RECORD_CHUNK_SAMPLES) ? maxSamples : RECORD_CHUNK_SAMPLES;
    size_t got = 0;

    if (i2s_read(I2S_NUM_0, raw, want * sizeof(int16_t), &got,
                 pdMS_TO_TICKS(timeoutMs)) != ESP_OK) return 0;

    size_t n = got / sizeof(int16_t);
    for (size_t i = 0; i < n; i++) {
        int32_t v = (int32_t)raw[i] << MIC_GAIN_SHIFT;   // 增益
        if (v > INT16_MAX) v = INT16_MAX;
        else if (v < -INT16_MAX) v = -INT16_MAX;
        out[i] = (int16_t)v;
    }
    return n;
}

} // namespace AudioIn
