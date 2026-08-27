// main.cpp —— Talk With Anyone ESP32 前端（鹿小班第三代 · 最终整合版）
// 自动聆听对话：说话→识别→AI 回复播放→继续聆听；播放时暂停收音防回声。
// 按键：BOOT(GPIO0)=开始聆听/打断；音量+/- =GPIO39/40，音量显示在屏幕并可调。
// 屏幕：状态(LISTEN/PLAY/IDLE/OFFLINE)、音量条、IP、运行时长。
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include "config.h"
#include "audio_in.h"
#include "audio_out.h"
#include "voice_client.h"
#include "display_ui.h"

enum State { ST_IDLE, ST_LISTENING, ST_PLAYING };
static State s_state = ST_IDLE;
static bool s_prevPressed = false;
static bool s_prevVolUp = false, s_prevVolDn = false;
static uint32_t s_pressStart = 0;
static uint32_t s_emptySince = 0;
static uint32_t s_cooldownUntil = 0;

// ---- 语音打断（播放期间能量门限检测）状态 ----
static uint32_t s_playStartMs = 0;      // 本次播放起始时刻（0=待初始化）
static uint32_t s_bargeWinMs = 0;       // 当前统计窗起点
static uint64_t s_bargeMicAbs = 0;      // 窗内麦克风|样本|累计
static uint32_t s_bargeMicN = 0;        // 窗内麦克风样本数
static uint64_t s_bargeOutPrev = 0;     // 上窗末的输出能量计数
static float s_bargeK = 0.6f;           // 回声通路增益估计（自适应，跨会话保留）
static uint32_t s_bargeHitMs = 0;       // 连续命中时长

static bool btnPressed(int pin) { return digitalRead(pin) == LOW; }

// 丢弃麦克风 DMA 里已缓冲的回声尾巴，避免旧回声触发服务端 VAD 误识别
static void drainMic() {
    static int16_t tmp[RECORD_CHUNK_SAMPLES];
    for (int i = 0; i < 4; i++) {
        AudioIn::read(tmp, RECORD_CHUNK_SAMPLES, 0);
        delay(8);
    }
}

static const char *stateName(State s) {
    switch (s) {
    case ST_LISTENING: return "LISTEN";
    case ST_PLAYING:   return "PLAY";
    default:           return VoiceClient::isUp() ? "IDLE" : "OFFLINE";
    }
}

static void syncUi() {
    DisplayUI::setState(stateName(s_state));
    DisplayUI::setVolume(AudioOut::getVolume());
}

static void enterListening(bool withBeep = true) {
    if (!VoiceClient::isUp()) return;
    if (withBeep) AudioOut::playTone(1320, 120);
    if (!VoiceClient::startRecording()) return;
    s_state = ST_LISTENING;
    s_pressStart = millis();
    syncUi();
    Serial.println("[状态] 聆听中，请说话");
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== TWA · 鹿小班三代 ESP32 前端 ===");

    pinMode(PIN_BTN_TALK, INPUT_PULLUP);
    pinMode(PIN_BTN_VOLUP, INPUT_PULLUP);
    pinMode(PIN_BTN_VOLDN, INPUT_PULLUP);

    Preferences prefs;
    prefs.begin("twa", false);
    int vol = prefs.getInt("volume", 80);

    DisplayUI::begin();
    DisplayUI::setVolume(vol);
    DisplayUI::setState("BOOT");

    AudioOut::begin();
    AudioOut::setVolume(vol);
    AudioOut::selfTest();               // 开机两短音：验证扬声器通路

    Serial.printf("连接 WiFi: %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\n[WiFi] 已连接, IP: %s\n", WiFi.localIP().toString().c_str());
    DisplayUI::setInfo(WiFi.localIP().toString());

    Serial.printf("[MEM] 建连前 heap=%u maxblk=%u psramFree=%u\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getFreePsram());
    VoiceClient::begin();
    syncUi();
    Serial.println("就绪。通道建立后自动进入聆听。");
}

void loop() {
    VoiceClient::loop();
    AudioOut::loop();
    DisplayUI::tick();

    bool pressed = btnPressed(PIN_BTN_TALK);

    // ---- 音量键（按下沿触发，±10）----
    bool up = btnPressed(PIN_BTN_VOLUP), dn = btnPressed(PIN_BTN_VOLDN);
    if (up && !s_prevVolUp) {
        delay(20);
        if (btnPressed(PIN_BTN_VOLUP)) {
            int v = AudioOut::getVolume() + 10;
            AudioOut::setVolume(v > 100 ? 100 : v);
            DisplayUI::setVolume(AudioOut::getVolume());
            Serial.printf("[音量] %d%%\n", AudioOut::getVolume());
            AudioOut::playTone(1800, 60);          // 调节反馈音
            Preferences p; p.begin("twa", false);
            p.putInt("volume", AudioOut::getVolume()); p.end();
            if (s_state == ST_PLAYING && s_playStartMs)
                s_playStartMs = millis();          // 音量突变重置起播静默期，防误触发打断
        }
    }
    if (dn && !s_prevVolDn) {
        delay(20);
        if (btnPressed(PIN_BTN_VOLDN)) {
            int v = AudioOut::getVolume() - 10;
            AudioOut::setVolume(v < 0 ? 0 : v);
            DisplayUI::setVolume(AudioOut::getVolume());
            Serial.printf("[音量] %d%%\n", AudioOut::getVolume());
            AudioOut::playTone(900, 60);
            Preferences p; p.begin("twa", false);
            p.putInt("volume", AudioOut::getVolume()); p.end();
            if (s_state == ST_PLAYING && s_playStartMs)
                s_playStartMs = millis();
        }
    }
    s_prevVolUp = up;
    s_prevVolDn = dn;

    // ---- 主按键状态机 ----
    switch (s_state) {
    case ST_IDLE:
        if (AUTO_LISTEN_ON_BOOT && VoiceClient::isUp() &&
            millis() > 5000 && millis() > s_cooldownUntil) {
            enterListening();
            break;
        }
        if (pressed && !s_prevPressed) {
            delay(30);
            if (btnPressed(PIN_BTN_TALK)) enterListening();
        }
        break;

    case ST_LISTENING: {
        if (millis() - s_pressStart > 150) {
            static int16_t buf[RECORD_CHUNK_SAMPLES];
            size_t n = AudioIn::read(buf, RECORD_CHUNK_SAMPLES);
            if (n) {
                VoiceClient::Detail::voiceSocket().sendBIN(
                    (uint8_t *)buf, n * sizeof(int16_t));
                static uint16_t peak = 0;
                for (size_t i = 0; i < n; i++) {
                    int16_t a = buf[i] < 0 ? -buf[i] : buf[i];
                    if (a > peak) peak = a;
                }
                static uint32_t lastLvl = 0;
                if (millis() - lastLvl > 2000) {
                    lastLvl = millis();
                    Serial.printf("[麦克风] 峰值=%d\n", peak);
                    peak = 0;
                }
            }
        }

        if (AudioOut::buffered() > 0) {          // 回复到达 → 进入播放
            s_state = ST_PLAYING;                // 麦克风保持收音（供语音打断检测）
            s_playStartMs = 0;                   // 由 PLAYING 分支首帧初始化
            syncUi();
            Serial.println("[状态] AI 回复播放中...（说话或按键可打断）");
            break;
        }

        if (pressed && !s_prevPressed) {
            delay(30);
            if (btnPressed(PIN_BTN_TALK)) {
                VoiceClient::pauseMic();
                DisplayUI::flushTyping();
                AudioOut::playTone(660, 150);
                s_cooldownUntil = millis() + 8000;
                s_state = ST_IDLE;
                syncUi();
                Serial.println("[状态] 手动退出聆听");
                break;
            }
        }

        if (VoiceClient::msSinceLastVoiceActivity() > LISTEN_TIMEOUT_MS &&
            millis() - s_pressStart > LISTEN_TIMEOUT_MS) {
            VoiceClient::pauseMic();
            s_cooldownUntil = millis() + 8000;
            s_state = ST_IDLE;
            syncUi();
            Serial.println("[状态] 聆听超时，待机");
        }
        break;
    }

    case ST_PLAYING: {
        // ---- 播放中持续收音（只测能量不发送，回声不会到达服务端 VAD）----
        if (s_playStartMs == 0) {                // 本次播放首帧：初始化统计窗
            s_playStartMs = millis();
            s_bargeWinMs = millis();
            s_bargeMicAbs = 0; s_bargeMicN = 0;
            s_bargeOutPrev = AudioOut::absOutAccum();
            s_bargeHitMs = 0;
        }
        {
            static int16_t drain[RECORD_CHUNK_SAMPLES];
            size_t n = AudioIn::read(drain, RECORD_CHUNK_SAMPLES, 0);
            for (size_t i = 0; i < n; i++) {
                int16_t a = drain[i] < 0 ? -drain[i] : drain[i];
                s_bargeMicAbs += a;
            }
            s_bargeMicN += n;
        }

#if BARGE_IN_ENABLE
        if (millis() - s_bargeWinMs >= 50) {      // 50ms 统计窗
            uint64_t outAcc = AudioOut::absOutAccum();
            uint64_t outWin = outAcc - s_bargeOutPrev;
            s_bargeOutPrev = outAcc;
            float micAvg = s_bargeMicN ? (float)s_bargeMicAbs / s_bargeMicN : 0.0f;
            float outAvg = (float)outWin / (SPK_SAMPLE_RATE * 0.05f);  // v域每样本均值

            bool cond = false;
            if (micAvg > BARGE_IN_SPEECH_FLOOR) {
                if (outAvg < BARGE_IN_OUT_FLOOR) {
                    cond = true;                  // 播放间隙：麦克风有声即人声
                } else {
                    // 回声增益自适应：下降快、上升慢，用户说话不抬高门限
                    float r = micAvg / outAvg;
                    if (r < s_bargeK) s_bargeK = s_bargeK * 0.7f + r * 0.3f;
                    else if (r < s_bargeK * 2.0f) s_bargeK = s_bargeK * 0.95f + r * 0.05f;
                    if (micAvg > s_bargeK * outAvg * BARGE_IN_GATE_RATIO + BARGE_IN_SLACK)
                        cond = true;
                }
            }
            s_bargeHitMs = cond ? s_bargeHitMs + 50 : 0;

            static uint32_t s_lastDiag = 0;       // 每2s打印一次，便于调参
            if (millis() - s_lastDiag > 2000) {
                s_lastDiag = millis();
                Serial.printf("[打断监测] mic=%.0f out=%.0f k=%.2f hit=%lums\n",
                              micAvg, outAvg, s_bargeK, (unsigned long)s_bargeHitMs);
            }

            s_bargeMicAbs = 0; s_bargeMicN = 0;
            s_bargeWinMs = millis();

            if (s_bargeHitMs >= BARGE_IN_SUSTAIN_MS &&
                millis() - s_playStartMs > BARGE_IN_START_DELAY_MS) {
                Serial.printf("[打断] 检测到用户说话，打断播放 (mic=%.0f out=%.0f k=%.2f)\n",
                              micAvg, outAvg, s_bargeK);
                VoiceClient::sendInterrupt();
                DisplayUI::flushTyping();
                AudioOut::stopPlayback();
                s_playStartMs = 0;
                drainMic();
                enterListening(false);
                break;
            }
        }
#endif // BARGE_IN_ENABLE

        if (pressed && !s_prevPressed) {         // 按键打断 → 立即继续聆听
            delay(30);
            if (btnPressed(PIN_BTN_TALK)) {
                VoiceClient::sendInterrupt();
                DisplayUI::flushTyping();        // 字幕立即补完
                while (btnPressed(PIN_BTN_TALK)) { delay(10); }
                s_prevPressed = btnPressed(PIN_BTN_TALK);
                s_playStartMs = 0;
                drainMic();
                enterListening(false);
                break;
            }
        }
        if (!AudioOut::active()) {               // 播完自动继续聆听
            if (s_emptySince == 0) s_emptySince = millis();
            if (millis() - s_emptySince > 400) {
                s_emptySince = 0;
                s_playStartMs = 0;
                drainMic();
                enterListening(false);
            }
        } else {
            s_emptySince = 0;
        }
        break;
    }
    }

    // 心跳
    static uint32_t s_lastBeat = 0;
    if (millis() - s_lastBeat > 30000) {
        s_lastBeat = millis();
        Serial.printf("[心跳] 运行%lus 状态=%d 服务器=%s heap=%u\n",
                      millis() / 1000, s_state,
                      VoiceClient::isUp() ? "在线" : "离线",
                      ESP.getFreeHeap());
    }

    s_prevPressed = pressed;
    delay(1);
}
