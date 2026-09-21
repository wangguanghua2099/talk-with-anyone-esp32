// voice_client.cpp —— talk-with-anyone 语音协议实现（流式版）
// 上行: /ws/voice 二进制帧 = PCM16@16k 单声道
// 控制: {"type":"session.start"|"interrupt"|"client_stats"} 文本帧
// 下行(新协议，随 LLM 生成边生成边推):
//        assistant.delta 文字增量 → 字幕流式打字
//        audio.start/chunk(b64)/audio.done → 扬声器边收边播（不等全文合成）
// 兜底: 整轮未收到音频时，把全文发 /ws/tts-stream 合成（旧行为，兼容旧后端）
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

// ---------- 本轮流式回复状态 ----------
static bool s_roundActive = false;      // assistant.delta 流进行中
static bool s_roundFinalized = false;   // 本轮已收尾（completed/打断/出错），迟到内容丢弃
static bool s_receivedAudio = false;    // 本轮是否收到过 voice WS 推送的音频
static bool s_voiceAudioOn = false;     // voice WS 音频流已 start、尚未 done
static uint32_t s_llmFirstTokenMs = 0;  // LLM 首个文字增量时刻（延迟统计）
static bool s_statSent = false;         // 本轮"首字→出声"延迟是否已上报

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

static void resetRound() {
    s_roundActive = false;
    s_roundFinalized = false;
    s_receivedAudio = false;
    s_voiceAudioOn = false;
    s_llmFirstTokenMs = 0;
    s_statSent = false;
}

// 本轮被打断/出错：立即补完已收到的字幕，之后迟到的增量/结果全部丢弃
static void finalizeRoundPartial() {
    if (s_roundActive && !s_roundFinalized) {
        DisplayUI::flushTyping();
    }
    s_roundActive = false;
    s_roundFinalized = true;
}

// 解码 audio.chunk 的 base64 并喂入扬声器（voice WS 与 /ws/tts-stream 共用）。
// 首块时上报 "LLM首字→出声" 延迟（服务端日志展示，对齐 web/手机端）
static void feedAudioChunk(JsonDocument &doc) {
    const char *b64 = doc["data"] | "";
    size_t b64len = strlen(b64);
    if (!b64len) return;
    size_t need = b64len * 3 / 4 + 8;
    uint8_t *pcm = (uint8_t *)malloc(need);
    if (!pcm) return;
    size_t olen = 0;
    if (mbedtls_base64_decode(pcm, need, &olen,
                              (const uint8_t *)b64, b64len) == 0 && olen) {
        s_chunkCnt++;
        s_chunkBytes += olen;
        if (s_chunkCnt == 1) {
            int16_t *s16 = (int16_t *)pcm;
            long sum = 0;
            for (int i = 0; i < 256 && i < (int)(olen / 2); i++) sum += s16[i];
            Serial.printf("[TTS][t=%lu][CHK-dev-decode] v=%d,%d,%d,%d sum256=%ld len=%u\n",
                          (unsigned long)millis(),
                          s16[0], s16[1], s16[2], s16[3], sum, (unsigned)olen);
        }
        AudioOut::feed(pcm, olen);
        if (!s_statSent && s_llmFirstTokenMs) {
            s_statSent = true;
            uint32_t lat = millis() - s_llmFirstTokenMs;
            Serial.printf("[Voice][t=%lu] LLM首token→首音频块: %lu ms\n",
                          (unsigned long)millis(), (unsigned long)lat);
            JsonDocument st;
            st["type"] = "client_stats";
            st["llm_first_token_to_audio_ms"] = (int)lat;
            String out;
            serializeJson(st, out);
            s_voice.sendTXT(out);
        }
        if (s_chunkCnt % 50 == 0)
            Serial.printf("[TTS] 已收%lu块/%luKB\n",
                          (unsigned long)s_chunkCnt,
                          (unsigned long)(s_chunkBytes / 1024));
    }
    free(pcm);
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
            // 新一轮开始：清掉上一轮残留的播放与流式状态
            if (s_voiceAudioOn) AudioOut::stopPlayback();
            resetRound();
            String txt = doc["text"] | "";
            Serial.printf("[ASR][t=%lu] %s\n", (unsigned long)millis(), txt.c_str());
            DisplayUI::addUserLine(txt);          // 字幕：用户说的话
            // 语句已识别完毕：立即暂停上行。等到出声之间麦克风无事可做
            // （服务端 VAD 已判定语句结束，此后上行只有噪声帧），持续上行
            // 会与下行音频在链路上争抢带宽，推高首包出声延迟
            pauseMic();
        } else if (!strcmp(t, "assistant.delta")) {
            if (s_roundFinalized) break;          // 已被打断收尾，丢弃迟到增量
            String delta = doc["text"] | "";
            if (!delta.length()) break;
            if (!s_roundActive) {
                s_roundActive = true;
                s_llmFirstTokenMs = millis();
                Serial.printf("[Voice][t=%lu] LLM 开始流式输出\n",
                              (unsigned long)millis());
            }
            DisplayUI::appendReplyDelta(delta);   // 字幕随 LLM 实时打字
        } else if (!strcmp(t, "audio.start")) {
            if (s_roundFinalized) break;          // 已被打断的旧回复，整条丢弃
            // 回复音频随 LLM 生成边推边播（不等全文）
            s_receivedAudio = true;
            s_voiceAudioOn = true;
            s_chunkCnt = 0; s_chunkBytes = 0;
            AudioOut::beginStream((uint32_t)(doc["sample_rate"] | 24000));
        } else if (!strcmp(t, "audio.chunk")) {
            if (s_roundFinalized) break;          // 旧回复的迟到音频块
            feedAudioChunk(doc);
        } else if (!strcmp(t, "audio.done")) {
            if (s_voiceAudioOn) {
                s_voiceAudioOn = false;
                AudioOut::finishStream();
                Serial.printf("[Voice][t=%lu] 回复音频推送完毕，播完自动继续聆听\n",
                              (unsigned long)millis());
            }
        } else if (!strcmp(t, "assistant.completed")) {
            if (s_roundFinalized) break;          // 本轮已被打断，丢弃迟到结果
            s_roundFinalized = true;
            s_roundActive = false;
            String reply = doc["text"] | "";
            if (reply.length()) {
                Serial.printf("[Voice] AI回复完成(%d字)\n", reply.length());
                DisplayUI::setReplyFullText(reply);   // 字幕对账补齐
                if (!s_receivedAudio) {
                    // 整轮没收到过音频（服务器关朗读/合成失败/旧后端）
                    // → 回退旧行为：把全文送 /ws/tts-stream 合成播放
                    requestTts(reply);
                }
            }
        } else if (!strcmp(t, "interrupt.ack")) {
            // 服务端确认取消生成；字幕定格已收到的部分
            finalizeRoundPartial();
        } else if (!strcmp(t, "error") || !strcmp(t, "asr.error") ||
                   !strcmp(t, "assistant.error")) {
            Serial.printf("[Voice] 错误: %s\n",
                          doc["message"] | doc["error"] | "");
            if (!strcmp(t, "assistant.error")) finalizeRoundPartial();
            // 回复出错：恢复收音，避免停在"聆听中"却没录音（asr.result 时已暂停）
            if (!s_recording && isUp()) startRecording();
        }
        break;
    }
    default:
        break;
    }
}

// ---------- tts 通道事件（兜底路径：整段文字合成） ----------

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
            feedAudioChunk(doc);
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
    finalizeRoundPartial();   // 本轮立即收尾，服务器取消生效前的迟到增量不再显示
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
