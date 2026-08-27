// voice_client.cpp —— talk-with-anyone 语音协议实现
// 上行: /ws/voice 二进制帧 = PCM16@16k 单声道
// 控制: {"type":"session.start"|"interrupt"} 文本帧
// 下行: /ws/tts-stream 发 {"text":...}，收 audio.start/chunk(b64)/audio.done
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include "config.h"
#include "voice_client.h"
#include "audio_in.h"
#include "audio_out.h"
#include "display_ui.h"

namespace VoiceClient {

static WebSocketsClient s_voice, s_tts;
static bool s_recording = false;
static volatile bool s_ttsPending = false;
static String s_pendingText;
static volatile uint32_t s_lastVadMs = 0;   // 最近一次 VAD 检测到说话的时刻
static uint32_t s_chunkCnt = 0, s_chunkBytes = 0;
static uint32_t s_lastConnectAttempt = 0;

// ---------- 内部工具 ----------

static String wsUrl(const char *path) {
    String url = path;
    if (ACCESS_TOKEN[0] != '\0') {
        url += "?token=";
        url += ACCESS_TOKEN;
    }
    return url;
}

static void sendSessionStart() {
    // 服务端收到后返回 session.ready；mode=chat 表示完整对话
    JsonDocument doc;
    doc["type"] = "session.start";
    doc["mode"] = "chat";
    String out;
    serializeJson(doc, out);
    s_voice.sendTXT(out);
    Serial.println("[Voice] 已发送 session.start");
}

// ---------- voice 通道事件 ----------

static void onVoiceEvent(WStype_t type, uint8_t *payload, size_t len) {
    switch (type) {
    case WStype_CONNECTED:
        Serial.println("[Voice] 已连接 /ws/voice");
        sendSessionStart();
        break;
    case WStype_DISCONNECTED:
        Serial.println("[Voice] /ws/voice 断开，等待自动重连");
        break;
    case WStype_TEXT: {
        JsonDocument doc;
        if (deserializeJson(doc, payload, len)) break;
        const char *t = doc["type"] | "";
        if (!strcmp(t, "vad.speaking")) {
            if (doc["speaking"].as<bool>()) s_lastVadMs = millis();
        } else if (!strcmp(t, "asr.result")) {
            String txt = doc["text"] | "";
            Serial.printf("[ASR] %s\n", txt.c_str());
            DisplayUI::addUserLine(txt);          // 字幕：用户说的话
        } else if (!strcmp(t, "assistant.completed")) {
            String reply = doc["text"] | "";
            Serial.printf("[Voice] 收到AI回复(%d字): %s\n", reply.length(), reply.c_str());
            DisplayUI::beginReplyTypewriter(reply);   // 字幕：逐字显示回复
            requestTts(reply);
        } else if (!strcmp(t, "error") || !strcmp(t, "asr.error") ||
                   !strcmp(t, "assistant.error")) {
            Serial.printf("[Voice] 错误: %s\n",
                          doc["message"] | doc["error"] | "");
        }
        break;
    }
    default:
        break;
    }
}

// ---------- tts 通道事件 ----------

static void onTtsEvent(WStype_t type, uint8_t *payload, size_t len) {
    switch (type) {
    case WStype_CONNECTED:
        Serial.println("[Voice] 已连接 /ws/tts-stream");
        break;
    case WStype_DISCONNECTED:
        Serial.println("[Voice] /ws/tts-stream 断开");
        break;
    case WStype_TEXT: {
        JsonDocument doc;
        if (deserializeJson(doc, payload, len)) break;
        const char *t = doc["type"] | "";
        if (!strcmp(t, "audio.start")) {
            s_chunkCnt = 0; s_chunkBytes = 0;
            AudioOut::beginStream((uint32_t)(doc["sample_rate"] | SPK_SAMPLE_RATE));
            Serial.printf("[TTS] 开始接收音频 rate=%d\n",
                          (int)(doc["sample_rate"] | SPK_SAMPLE_RATE));
        } else if (!strcmp(t, "audio.chunk")) {
            const char *b64 = doc["data"] | "";
            size_t b64len = strlen(b64);
            size_t need = b64len * 3 / 4 + 8;
            uint8_t *pcm = (uint8_t *)malloc(need);
            if (pcm) {
                size_t olen = 0;
                if (mbedtls_base64_decode(pcm, need, &olen,
                                          (const uint8_t *)b64, b64len) == 0) {
                    s_chunkCnt++;
                    s_chunkBytes += olen;
                    if (s_chunkCnt == 1) {
                        int16_t *s16 = (int16_t *)pcm;
                        long sum = 0;
                        for (int i = 0; i < 256 && i < (int)(olen / 2); i++) sum += s16[i];
                        Serial.printf("[TTS][CHK-dev-decode] v=%d,%d,%d,%d sum256=%ld len=%u\n",
                                      s16[0], s16[1], s16[2], s16[3], sum, (unsigned)olen);
                    }
                    AudioOut::feed(pcm, olen);
                    if (s_chunkCnt % 50 == 0)
                        Serial.printf("[TTS] 已收%lu块/%luKB\n",
                                      (unsigned long)s_chunkCnt,
                                      (unsigned long)(s_chunkBytes / 1024));
                }
                free(pcm);
            }
        } else if (!strcmp(t, "audio.done")) {
            AudioOut::finishStream();
            Serial.println("[TTS] 音频接收完毕");
        } else if (!strcmp(t, "error")) {
            AudioOut::stopPlayback();
            Serial.printf("[TTS] 错误: %s\n", doc["message"] | "");
        }
        break;
    }
    default:
        break;
    }
}

// ---------- 对外接口 ----------

bool begin() {
    String vp = wsUrl("/ws/voice");
    String tp = wsUrl("/ws/tts-stream");
    // 自签证书：不传 CA/指纹 = 跳过校验（仅限局域网自用）
    s_voice.beginSSL(SERVER_HOST, SERVER_PORT, vp.c_str(), nullptr, nullptr);
    s_tts.beginSSL(SERVER_HOST, SERVER_PORT, tp.c_str(), nullptr, nullptr);
    s_voice.onEvent(onVoiceEvent);
    s_tts.onEvent(onTtsEvent);
    s_voice.setReconnectInterval(5000);
    s_tts.setReconnectInterval(5000);
    return true;
}

void loop() {
    s_voice.loop();
    s_tts.loop();

    // 待发送的 TTS 请求（等 tts 通道可用）
    if (s_ttsPending && s_tts.isConnected()) {
        JsonDocument doc;
        doc["text"] = s_pendingText;
        doc["sample_rate"] = SPK_SAMPLE_RATE;   // 请求服务端重采样到设备输出率
        String out;
        serializeJson(doc, out);
        s_tts.sendTXT(out);
        s_ttsPending = false;
        Serial.println("[TTS] 已请求合成");
    }

    // WiFi 掉线重连
    static uint32_t lastWifiTry = 0;
    if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTry > 10000) {
        lastWifiTry = millis();
        Serial.println("[WiFi] 重连中...");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
}

bool isUp() { return s_voice.isConnected(); }

bool startRecording() {
    if (!isUp()) {
        Serial.println("[Voice] 未连接服务器，无法录音");
        return false;
    }
    if (!AudioIn::begin()) return false;
    s_recording = true;
    s_lastVadMs = millis();
    return true;
}

void pauseMic() {
    if (!s_recording) return;
    s_recording = false;
    AudioIn::end();
    Serial.println("[Voice] 收音暂停");
}

uint32_t msSinceLastVoiceActivity() {
    return s_lastVadMs ? millis() - s_lastVadMs : 0xFFFFFFFF;
}

void stopRecordingAndFlush() {
    if (!s_recording) return;
    s_recording = false;
    // 补 700ms 静音让服务端 VAD 判定语句结束并触发识别
    static int16_t silence[RECORD_CHUNK_SAMPLES] = {0};
    for (int i = 0; i < RELEASE_SILENCE_MS / 32; i++) {   // 每块 ~32ms
        s_voice.sendBIN((uint8_t *)silence, sizeof(silence));
        delay(1);
    }
    AudioIn::end();
    Serial.println("[Voice] 录音结束");
}

void sendInterrupt() {
    AudioOut::stopPlayback();
    if (isUp()) s_voice.sendTXT("{\"type\":\"interrupt\"}");
    Serial.println("[Voice] 已发送 interrupt");
}

void requestTts(const String &text) {
    if (!text.length()) return;
    s_pendingText = text;
    s_ttsPending = true;
}

namespace Detail {
WebSocketsClient &voiceSocket() { return s_voice; }
}

} // namespace VoiceClient
