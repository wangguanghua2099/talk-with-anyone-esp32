// audio_out.cpp —— 环形缓冲 + 24k→16k 线性重采样 + 软件音量(平方曲线) + 32bit 单声道输出
// 输出规格与卖家源码一致：32bit、左槽、样本<<16；输出时钟锁定 16kHz（鹿小班板载规格）
#include <Arduino.h>
#include <driver/i2s.h>
#include "config.h"
#include "audio_out.h"

namespace AudioOut {

constexpr size_t RING_SIZE = 512 * 1024;          // 512KB PSRAM 环形缓冲（mono16）
static uint8_t *s_ring = nullptr;
static volatile size_t s_head = 0, s_tail = 0;
static volatile bool s_streaming = false;
static volatile bool s_doneFlag = false;

static int s_volume = 80;                         // 软件音量 0~100
static int s_inRate = 24000;                      // 本次流的服务端采样率
// 重采样状态（输入率→16k 线性插值）
static float   s_rFrac = 0.0f;                    // 相位 [0,1)
static int16_t s_rPrev = 0;                       // 上一个输入样本

bool begin() {
    if (s_ring) return true;
    s_ring = (uint8_t *)ps_malloc(RING_SIZE);
    if (!s_ring) s_ring = (uint8_t *)malloc(RING_SIZE);
    if (!s_ring) return false;

    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = SPK_SAMPLE_RATE;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 8;
    cfg.dma_buf_len = 512;
    cfg.tx_desc_auto_clear = true;

    i2s_pin_config_t pins = {};
    pins.mck_io_num = I2S_PIN_NO_CHANGE;
    pins.bck_io_num = PIN_SPK_BCLK;
    pins.ws_io_num = PIN_SPK_LRCK;
    pins.data_out_num = PIN_SPK_DOUT;
    pins.data_in_num = I2S_PIN_NO_CHANGE;

    if (i2s_driver_install(I2S_NUM_1, &cfg, 0, nullptr) != ESP_OK) return false;
    if (i2s_set_pin(I2S_NUM_1, &pins) != ESP_OK) {
        i2s_driver_uninstall(I2S_NUM_1);
        return false;
    }
    i2s_zero_dma_buffer(I2S_NUM_1);
    Serial.println("[AudioOut] I2S1 扬声器就绪 (16k/32bit/mono)");
    return true;
}

void selfTest() {
    Serial.println("[AudioOut] 扬声器自检：应听到两短音");
    playTone(1200, 150);
    delay(120);
    playTone(1600, 150);
    Serial.println("[AudioOut] 自检完毕");
}

// 环形缓冲内按服务端字节序（小端 PCM16）读取一个样本
inline int16_t rdSample(size_t pos) {
    return (int16_t)((uint16_t)s_ring[pos % RING_SIZE] |
                     ((uint16_t)s_ring[(pos + 1) % RING_SIZE] << 8));
}

size_t buffered() { return (s_head - s_tail + RING_SIZE) % RING_SIZE; }

bool active() { return s_streaming || buffered() > 0; }

bool beginStream(uint32_t inputRate) {
    if (!s_ring) return false;
    s_inRate = (inputRate >= 8000 && inputRate <= 48000) ? (int)inputRate : 24000;
    s_head = s_tail = 0;
    s_rFrac = 0.0f;
    s_rPrev = 0;
    s_doneFlag = false;
    s_streaming = true;
    Serial.printf("[AudioOut] 新音频流 输入%dHz→输出%dk\n",
                  s_inRate, SPK_SAMPLE_RATE / 1000);
    return true;
}

void setVolume(int v) {
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    s_volume = v;
}

int getVolume() { return s_volume; }

inline int32_t applyVolume(int16_t v) {
    float factor = (s_volume / 100.0f) * (s_volume / 100.0f);   // 平方曲线
    return (int32_t)((float)v * factor * 65536.0f);
}

// 追加服务端 mono16 数据；采样率与输出一致时直通，否则线性插值重采样
size_t feed(const uint8_t *mono16, size_t bytes) {
    if (!s_ring || !mono16 || !bytes || !s_streaming) return 0;
    size_t nIn = bytes / sizeof(int16_t);
    const int16_t *in = (const int16_t *)mono16;

    // 直通：服务端已按设备输出率重采样
    if (s_inRate == SPK_SAMPLE_RATE) {
        size_t space = (s_tail - s_head - 1 + RING_SIZE) % RING_SIZE;
        size_t n = (bytes < space) ? bytes : space;
        for (size_t i = 0; i < n; i++) {
            s_ring[s_head] = mono16[i];
            s_head = (s_head + 1) % RING_SIZE;
        }
        return n;
    }

    const float ratio = (float)s_inRate / (float)SPK_SAMPLE_RATE;

    for (size_t i = 0; i < nIn; i++) {
        int16_t s_cur = in[i];
        float t = s_rFrac;
        bool full = false;
        while (t < 1.0f) {
            int32_t out = (int32_t)(s_rPrev + (int32_t)((s_cur - s_rPrev) * t));
            size_t space = (s_tail - s_head - 1 + RING_SIZE) % RING_SIZE;
            if (space < 2) { full = true; break; }
            s_ring[s_head] = (uint8_t)(out & 0xFF);
            s_ring[(s_head + 1) % RING_SIZE] = (uint8_t)((out >> 8) & 0xFF);
            s_head = (s_head + 2) % RING_SIZE;
            t += ratio;
        }
        s_rFrac = (t >= 1.0f) ? (t - 1.0f) : t;
        s_rPrev = s_cur;
        if (full) break;                          // 满则放弃剩余（正常不发生）
    }
    return nIn * sizeof(int16_t);
}

void finishStream() { s_doneFlag = true; }

void stopPlayback() {
    s_streaming = false;
    s_doneFlag = false;
    s_head = s_tail = 0;
    s_rFrac = 0.0f;
    s_rPrev = 0;
    i2s_zero_dma_buffer(I2S_NUM_1);
}

void loop() {
    if (!s_ring || !s_streaming) return;

    static bool started = false;
    if (!started) {
        if (s_doneFlag || buffered() >= PLAY_WATERMARK_BYTES) started = true;
        else return;
        // 首次起播：打印环形缓冲读出的前 6 个样本与 sum256，用于与服务端对账
        // （服务端音频为小端序 PCM16，必须低字节在前地取值）
        {
            int16_t probe[6];
            size_t t = s_tail;
            for (int i = 0; i < 6; i++) {
                probe[i] = rdSample(t);
                t = (t + 2) % RING_SIZE;
            }
            long sum = 0;
            size_t tt = s_tail;
            for (int i = 0; i < 256; i++) {
                int16_t v = rdSample(tt);
                tt = (tt + 2) % RING_SIZE;
                sum += v;
            }
            Serial.printf("[TTS][CHK-dev-ring] v=%d,%d,%d,%d sum256=%ld\n",
                          probe[0], probe[1], probe[2], probe[3], sum);
        }
    }

    // mono16 → 音量缩放 → 32bit 左槽；单次最多 128 采样
    // 注意：服务端 PCM 为小端序，必须低字节在前拼接，否则高低字节颠倒=持续白噪声
    static int32_t out[128];
    size_t availSamples = buffered() / sizeof(int16_t);
    if (availSamples == 0) {
        if (s_doneFlag) {
            started = false;
            s_streaming = false;
            Serial.println("[AudioOut] 播放完成");
        }
        return;
    }
    size_t n = (availSamples < 128) ? availSamples : 128;
    for (size_t i = 0; i < n; i++) {
        int16_t v = rdSample(s_tail);
        s_tail = (s_tail + 2) % RING_SIZE;
        out[i] = applyVolume(v);
    }
    size_t written = 0;
    esp_err_t err = i2s_write(I2S_NUM_1, out, n * sizeof(int32_t),
                              &written, pdMS_TO_TICKS(100));
    static uint32_t s_totalWr = 0;
    s_totalWr += written;
    if (err != ESP_OK)
        Serial.printf("[AudioOut] 写入异常 err=%d\n", err);
    static uint32_t s_lastLog = 0;
    if (s_totalWr > 0 && millis() - s_lastLog > 3000) {
        s_lastLog = millis();
        Serial.printf("[AudioOut] 已输出 %luKB\n", (unsigned long)(s_totalWr / 1024));
    }
}

void playTone(uint32_t freqHz, uint32_t durationMs) {
    const uint32_t rate = SPK_SAMPLE_RATE;
    size_t n = rate * durationMs / 1000;
    static int32_t out[256];
    for (size_t i = 0; i < n; i += 128) {
        size_t k = (n - i < 128) ? (n - i) : 128;
        for (size_t j = 0; j < k; j++) {
            float t = (float)(i + j) / rate;
            float env = 1.0f;
            size_t idx = i + j;
            if (idx < 160) env = idx / 160.0f;
            if (idx > n - 160) env = (n - idx) / 160.0f;
            float smp = sinf(2 * 3.14159265f * freqHz * t) * 9000 * env;
            out[j] = applyVolume((int16_t)smp);
        }
        size_t written = 0;
        i2s_write(I2S_NUM_1, out, k * sizeof(int32_t), &written, pdMS_TO_TICKS(150));
    }
    i2s_zero_dma_buffer(I2S_NUM_1);
}

} // namespace AudioOut
