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

static bool btnPressed(int pin) { return digitalRead(pin) == LOW; }

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

static void enterListening(bool withBeep = false) {
    if (!VoiceClient::isUp()) return;
    // 重进聆听不再提示音（原 1320Hz 短音在超时重听/重连重听时会不定期出现）
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
    configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");   // 顶栏时钟（UTC+8）

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

        if (AudioOut::buffered() > 0) {          // 回复到达 → 暂停收音防回声
            VoiceClient::pauseMic();
            s_state = ST_PLAYING;
            syncUi();
            Serial.println("[状态] AI 回复播放中...（按一下打断）");
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

    case ST_PLAYING:
        if (pressed && !s_prevPressed) {         // 打断 → 立即继续聆听
            delay(30);
            if (btnPressed(PIN_BTN_TALK)) {
                VoiceClient::sendInterrupt();
                DisplayUI::flushTyping();        // 字幕立即补完
                while (btnPressed(PIN_BTN_TALK)) { delay(10); }
                s_prevPressed = btnPressed(PIN_BTN_TALK);
                enterListening(false);
                break;
            }
        }
        if (!AudioOut::active()) {               // 播完自动继续聆听
            if (s_emptySince == 0) s_emptySince = millis();
            if (millis() - s_emptySince > 400) {
                s_emptySince = 0;
                enterListening(false);
            }
        } else {
            s_emptySince = 0;
        }
        break;
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
