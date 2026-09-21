// display_ui.cpp —— 字幕系统：efont 中文 14pt、逐字打字机、自动换行、满屏上滚
//
// 防闪烁策略（借鉴小智固件 LVGL 的“局部重绘”思想，零额外显存/堆占用）：
//   · 每个可见行与上次屏幕内容逐行比对，没变的行一个字节都不发；
//   · 打字常态下新行文本是旧行的前缀延长 → 只把新增字符直接续画在旧像素之后，
//     不清屏、不重绘旧行，画面完全静止无闪烁；
//   · 结构性变化（换行滚动/新消息/光标增减/清空）才对本行或整区清底重绘，
//     属低频事件，不会形成频闪。
// 布局：顶栏 0~21px（状态+音量），字幕区 24~239px（约 12 行）
#include <Arduino.h>
#include <WiFi.h>
#include <LovyanGFX.hpp>
#include <driver/adc.h>
#include <time.h>
#include <vector>
#include "display_ui.h"
#include "config.h"

namespace DisplayUI {

class LGFX_Xingzhi : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI _bus;
public:
    LGFX_Xingzhi() {
        {
            auto cfg = _bus.config();
            cfg.spi_host = SPI3_HOST;
            cfg.spi_mode = 3;
            cfg.freq_write = 40000000;
            cfg.freq_read = 16000000;
            cfg.spi_3wire = false;
            cfg.use_lock = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk = DISPLAY_SCL;
            cfg.pin_mosi = DISPLAY_SDA;
            cfg.pin_miso = -1;
            cfg.pin_dc = DISPLAY_DC;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg = _panel.config();
            cfg.pin_cs = DISPLAY_CS;
            cfg.pin_rst = DISPLAY_RES;
            cfg.pin_busy = -1;
            cfg.panel_width = 240;
            cfg.panel_height = 240;
            cfg.memory_width = 240;
            cfg.memory_height = 240;
            cfg.invert = true;
            cfg.rgb_order = false;
            _panel.config(cfg);
        }
        setPanel(&_panel);
    }
};

static LGFX_Xingzhi s_lcd;
static bool s_ok = false;

// ---------- 状态 ----------
static String s_state = "BOOT";
static int s_vol = 80;
static uint32_t s_lastDraw = 0;
static bool s_dirtySub = true;      // 字幕区待刷新（差异路径内部再判每行是否真变了）
static bool s_dirtyBar = true;      // 顶栏待刷新

// ---- 顶栏附加显示（时间/WiFi/电量；音量仅调节时临时显示） ----
static uint32_t s_volShowUntil = 0; // 音量临时显示截止时刻（0=不显示）
static int s_lastHour = -1, s_lastMin = -1;
static bool s_timeValid = false;
static bool s_wifiConn = false;
static int s_battLevel = -1;        // 0~100，-1=未知
static bool s_battCharging = false;
static uint32_t s_lastBattMs = 0;
static uint32_t s_last1sMs = 0;
static bool s_adcInit = false;

// ---------- 字幕数据 ----------
static std::vector<String> s_lines;      // 已完成行
static std::vector<uint16_t> s_colors;   // 每行颜色
static String s_typingRest;              // 打字机剩余文本
static String s_curLine;                 // 当前正在打的一行
static uint16_t s_curColor = 0x07FF;
static uint32_t s_lastTypeMs = 0;
static bool s_streamMode = false;        // 流式回复进行中（assistant.delta 追加模式）
static String s_streamQueued;            // 已进入打字缓冲的累计文本（与全文对账用）

static constexpr int LINE_H = 18;        // efontCN_14 行高
static constexpr int SUB_Y = 24;
static constexpr int SUB_H = 216;        // 239-24
static constexpr int VISIBLE = SUB_H / LINE_H;   // 12 行
static constexpr size_t MAX_LINES = 80;
static constexpr int SUB_W = 232;        // 留边
static constexpr uint32_t TYPE_DRAW_MS = 70;    // 打字中刷新节流
static constexpr uint32_t IDLE_DRAW_MS = 200;   // 无打字时刷新节流

// ---------- 工具 ----------
static inline bool typingActive() {
    return s_curLine.length() || s_typingRest.length();
}

// 已绘制到屏幕的可见行快照（差异比对的基准）
struct Slot {
    String txt;
    uint16_t col;
    bool caret;
};
static Slot s_drawn[VISIBLE];

static void prepFont(lgfx::LGFXBase &g) {
    g.setFont(&fonts::efontCN_14);
}

// 度量环境固定在 s_lcd + efontCN_14，与实际绘制字体一致
static int textW(const String &s) {
    prepFont(s_lcd);
    return s_lcd.textWidth(s);
}

// 按像素宽度把文本切成若干行
static void wrapToLines(const String &text, uint16_t color,
                        std::vector<String> &out) {
    (void)color;
    String cur;
    int i = 0;
    while (i < (int)text.length()) {
        // 取一个 UTF-8 字符
        int len = 1;
        uint8_t c = text[i];
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        String ch = text.substring(i, i + len);
        i += len;

        if (ch == "\n") {
            out.push_back(cur.length() ? cur : " ");
            cur = "";
            continue;
        }
        String test = cur + ch;
        if (textW(test) > SUB_W && cur.length()) {
            out.push_back(cur);
            cur = ch;
        } else {
            cur = test;
        }
    }
    out.push_back(cur);
}

static void pushLines(const String &text, uint16_t color) {
    std::vector<String> wrapped;
    wrapToLines(text, color, wrapped);
    for (auto &l : wrapped) {
        s_lines.push_back(l);
        s_colors.push_back(color);
    }
    while (s_lines.size() > MAX_LINES) {
        s_lines.erase(s_lines.begin());
        s_colors.erase(s_colors.begin());
    }
}

bool begin() {
    if (s_ok) return true;
    s_ok = s_lcd.init();
    if (!s_ok) {
        Serial.println("[UI] 屏幕初始化失败");
        return false;
    }
    s_lcd.setRotation(0);
    s_lcd.fillScreen(TFT_BLACK);

    ledcSetup(7, 5000, 8);
    ledcAttachPin(DISPLAY_BACKLIGHT_PIN, 7);
    ledcWrite(7, 220);

    // 电池电压 ADC（ADC2_CH6=S3 GPIO17，卖家源码确认）+ 充电检测引脚
    pinMode(PIN_BAT_CHG, INPUT);
    s_adcInit = (adc2_config_channel_atten(ADC2_CHANNEL_6, ADC_ATTEN_DB_11) == ESP_OK);

    prepFont(s_lcd);
    Serial.println("[UI] 屏幕就绪 ST7789 240x240 (efont 中文 · 局部重绘防闪烁)");
    return true;
}

void setState(const char *state) { _setStateRaw(state); }
void _setStateRaw(const char *state) {
    if (s_state == state) return;
    s_state = state;
    s_dirtyBar = true;
    s_dirtySub = true;               // 状态影响聆听光标的显隐
}
void setInfo(const String &) {}           // 预留

void setVolume(int v) {
    v = (v < 0) ? 0 : (v > 100 ? 100 : v);
    if (s_vol == v) return;
    s_vol = v;
    s_dirtyBar = true;
    s_volShowUntil = millis() + 1500;   // 仅调节后 1.5s 内显示音量
}

// ---------- 顶栏后台信息（1s 周期） ----------

// 卖家源码标定的 ADC→电量映射（12bit 原始值）
static void pollBattery() {
    if (!s_adcInit) return;
    int v1 = 0, v2 = 0;
    esp_err_t e1 = adc2_get_raw(ADC2_CHANNEL_6, ADC_WIDTH_BIT_12, &v1);
    delay(5);
    esp_err_t e2 = adc2_get_raw(ADC2_CHANNEL_6, ADC_WIDTH_BIT_12, &v2);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        static bool reported = false;          // 只报一次，避免刷屏
        if (!reported) {
            reported = true;
            Serial.printf("[电量] ADC读取失败 err=%d/%d（WiFi占用ADC2则电量不可用）\n",
                          e1, e2);
        }
        return;
    }
    int adc = (v1 + v2) / 2;

    static const struct { uint16_t adc; uint8_t lvl; } L[] = {
        {1970, 0}, {2062, 20}, {2154, 40}, {2246, 60}, {2338, 80}, {2430, 100}
    };
    int lvl;
    if (adc <= L[0].adc) lvl = 0;
    else if (adc >= L[5].adc) lvl = 100;
    else {
        lvl = 0;
        for (int i = 0; i < 5; i++) {
            if (adc >= L[i].adc && adc < L[i + 1].adc) {
                float r = (float)(adc - L[i].adc) / (L[i + 1].adc - L[i].adc);
                lvl = L[i].lvl + (int)(r * (L[i + 1].lvl - L[i].lvl));
                break;
            }
        }
    }
    bool chg = digitalRead(PIN_BAT_CHG) == HIGH && lvl < 100;
    if (lvl != s_battLevel || chg != s_battCharging) s_dirtyBar = true;
    s_battLevel = lvl;
    s_battCharging = chg;
    Serial.printf("[电量] adc=%d level=%d%% charging=%d\n", adc, lvl, chg);
}

static void topBarHousekeeping() {
    uint32_t now = millis();
    if (s_volShowUntil && now > s_volShowUntil) {   // 音量临时条到期
        s_volShowUntil = 0;
        s_dirtyBar = true;
    }
    if (now - s_last1sMs < 1000) return;
    s_last1sMs = now;

    struct tm tmnow;
    bool tv = getLocalTime(&tmnow, 0);
    if (tv != s_timeValid ||
        (tv && (tmnow.tm_hour != s_lastHour || tmnow.tm_min != s_lastMin))) {
        s_timeValid = tv;
        if (tv) { s_lastHour = tmnow.tm_hour; s_lastMin = tmnow.tm_min; }
        s_dirtyBar = true;                          // 分钟变化才重绘
    }

    bool conn = (WiFi.status() == WL_CONNECTED);
    if (conn != s_wifiConn) { s_wifiConn = conn; s_dirtyBar = true; }

    if (s_lastBattMs == 0 || now - s_lastBattMs >= 30000) {
        s_lastBattMs = now;
        pollBattery();                              // 30s 一测，电量变化才重绘
    }
}

// ---------- 字幕 API ----------

void addUserLine(const String &text) {
    flushTyping();                        // 上一条 AI 若没打完，先补完
    pushLines("我:" + text, 0x07E0);
    s_dirtySub = true;
}

void beginReplyTypewriter(const String &text) {
    flushTyping();
    s_typingRest = text;
    s_curLine = "";
    s_curColor = 0x07FF;
    s_streamMode = false;
    s_streamQueued = "";
    s_dirtySub = true;
}

// 流式字幕：LLM 增量追加进打字缓冲，由 tickTypewriter 按固定节奏揭示，
// 字幕随 LLM 生成实时推进（LLM 快于揭示速度时缓冲排队，慢于时即时显示）
void beginReplyStream() {
    flushTyping();
    s_typingRest = "";
    s_curLine = "";
    s_curColor = 0x07FF;
    s_streamQueued = "";
    s_streamMode = true;
    s_dirtySub = true;
}

void appendReplyDelta(const String &delta) {
    if (!delta.length()) return;
    if (!s_streamMode) beginReplyStream();
    s_typingRest += delta;
    s_streamQueued += delta;
    s_dirtySub = true;
}

// 回复完成：以后端全文为准。与已排队的增量做前缀对账，只补齐差额；
// 没收到过增量（旧后端）或内容对不上时，整条按打字机重新显示
void setReplyFullText(const String &full) {
    if (s_streamMode && full.startsWith(s_streamQueued)) {
        if (full.length() > s_streamQueued.length()) {
            s_typingRest += full.substring(s_streamQueued.length());
        }
        s_streamMode = false;
        s_streamQueued = "";
        s_dirtySub = true;
        return;
    }
    beginReplyTypewriter(full);
}

void flushTyping() {
    s_streamMode = false;
    s_streamQueued = "";
    if (s_typingRest.length() || s_curLine.length()) {
        String rest = s_curLine + s_typingRest;
        pushLines(rest, s_curColor);
        s_typingRest = "";
        s_curLine = "";
        s_dirtySub = true;
    }
}

void clearChat() {
    s_lines.clear(); s_colors.clear();
    s_typingRest = ""; s_curLine = "";
    s_streamMode = false;
    s_streamQueued = "";
    s_dirtySub = true;
}

// ---------- 绘制（局部重绘） ----------

// 计算第 slot 个可见行的期望内容；返回该行是否存在
// 视图序列 = 历史行(尾部 VISIBLE 个) + 当前打字行；聆听光标跟随最后一行
static bool desiredRow(int slot, String &txt, uint16_t &col, bool &caret) {
    int total = (int)s_lines.size() + (typingActive() ? 1 : 0);
    if (total == 0) {                     // 空屏：仅 LISTEN 时首行画光标提示
        txt = ""; col = TFT_BLACK;
        caret = (s_state == "LISTEN") && (slot == 0);
        return caret;                     // 只有带光标的空行才算“存在”
    }

    int firstIdx = total > VISIBLE ? total - VISIBLE : 0;
    int absRow = firstIdx + slot;
    if (absRow >= total) return false;

    bool isTypingRow = typingActive() && absRow == total - 1;
    if (isTypingRow) {
        txt = s_curLine;
        col = s_curColor;
    } else {
        txt = s_lines[absRow];
        col = s_colors[absRow];
    }
    caret = (s_state == "LISTEN") && absRow == total - 1;
    return true;
}

static constexpr uint16_t CARET_COLOR = 0x47F4;   // #40FFA0 的合法 RGB565

// 把一行内容直接绘到屏幕对应行位置。
// 增量快路径：新文本是旧行的纯延长且与光标无关时，只续画新增字符，
// 不清底、不动旧像素 —— 打字期间画面除新增字外完全静止。
static void commitRow(int slot, const String &txt, uint16_t col, bool caret) {
    const int bandY = SUB_Y + slot * LINE_H;
    const Slot &d = s_drawn[slot];

    bool canExtend = !caret && !d.caret && d.col == col &&
                     d.txt.length() > 0 &&
                     txt.length() > d.txt.length() &&
                     txt.startsWith(d.txt);
    if (canExtend) {
        prepFont(s_lcd);
        int xoff = 4 + textW(d.txt);
        s_lcd.setTextColor(col, TFT_BLACK);       // 带底色绘制可覆盖残留像素
        s_lcd.setCursor(xoff, bandY + 2);
        s_lcd.print(txt.substring(d.txt.length()));
    } else {
        s_lcd.fillRect(0, bandY, 240, LINE_H, TFT_BLACK);
        if (txt.length()) {
            prepFont(s_lcd);
            s_lcd.setTextColor(col, TFT_BLACK);
            s_lcd.setCursor(4, bandY + 2);
            s_lcd.print(txt);
        }
        if (caret) {
            prepFont(s_lcd);
            s_lcd.setCursor(4 + textW(txt), bandY + 2);
            s_lcd.setTextColor(CARET_COLOR, TFT_BLACK);
            s_lcd.print("_");
        }
    }

    Slot &m = s_drawn[slot];
    m.txt = txt;
    m.col = col;
    m.caret = caret;
}

static void drawSubtitles() {
    for (int slot = 0; slot < VISIBLE; slot++) {
        String txt; uint16_t col = TFT_BLACK; bool caret = false;
        bool exist = desiredRow(slot, txt, col, caret);

        Slot &d = s_drawn[slot];
        bool same = exist && d.txt == txt && d.col == col && d.caret == caret;
        if (!exist) same = (d.txt.length() == 0 && !d.caret);
        if (same) continue;               // 内容没变 → 一个字节都不发

        if (exist)
            commitRow(slot, txt, col, caret);
        else {                            // 该行已空出（内容减少时），清成黑条
            if (d.txt.length() || d.caret)
                commitRow(slot, "", TFT_BLACK, false);
        }
    }
}

static void drawTopBar() {
    const uint16_t bg = 0x1202;   // 深绿黑（#104010 的合法 RGB565 换算）
    s_lcd.fillRect(0, 0, 240, 22, bg);
    s_lcd.setFont(&fonts::efontCN_12);
    // 合法 RGB565 值：原 0x40FFA0 等常量超出 16 位被截断成错误颜色
    uint16_t c = TFT_WHITE;
    if (s_state == "LISTEN") c = 0x67F4;          // 春绿
    else if (s_state == "PLAY") c = 0xFF0C;       // 琥珀黄
    else if (s_state == "OFFLINE") c = 0xF98C;    // 红
    s_lcd.setTextColor(c, bg);
    s_lcd.setCursor(6, 5);
    s_lcd.print(s_state);

    if (s_volShowUntil) {
        // 音量调节临时条（1.5s）
        s_lcd.setTextColor(TFT_CYAN, bg);
        char buf[12];
        snprintf(buf, sizeof(buf), "VOL %d%%", s_vol);
        s_lcd.setCursor(176, 5);
        s_lcd.print(buf);
    } else {
        // 时间（NTP 对时后显示，未同步显示 --:--）
        char tbuf[8];
        if (s_timeValid) snprintf(tbuf, sizeof(tbuf), "%02d:%02d", s_lastHour, s_lastMin);
        else snprintf(tbuf, sizeof(tbuf), "--:--");
        s_lcd.setTextColor(TFT_WHITE, bg);
        s_lcd.setCursor(150, 5);
        s_lcd.print(tbuf);
        // WiFi 信号柱（三格）
        uint16_t wc = s_wifiConn ? TFT_CYAN : 0x630C;
        s_lcd.fillRect(190, 10, 3, 5, wc);
        s_lcd.fillRect(195, 7, 3, 8, wc);
        s_lcd.fillRect(200, 4, 3, 11, wc);
        // 电量百分比（右对齐），充电中且未满时左侧画小闪电
        char bbuf[6];
        if (s_battLevel < 0) snprintf(bbuf, sizeof(bbuf), "--%%");
        else snprintf(bbuf, sizeof(bbuf), "%d%%", s_battLevel);
        int bw = s_lcd.textWidth(bbuf);
        s_lcd.setTextColor(TFT_WHITE, bg);
        s_lcd.setCursor(237 - bw, 5);
        s_lcd.print(bbuf);
        if (s_battCharging) {
            int bx = 237 - bw - 8;
            s_lcd.drawLine(bx + 2, 3, bx, 8, 0xFFE0);
            s_lcd.drawLine(bx, 8, bx + 3, 8, 0xFFE0);
            s_lcd.drawLine(bx + 3, 8, bx + 1, 13, 0xFFE0);
        }
    }
    s_lcd.setFont(&fonts::efontCN_14);
}

// ---------- 打字机推进 ----------

static void tickTypewriter() {
    if (!s_typingRest.length()) return;
    uint32_t now = millis();
    // 143ms/单位 ≈ 7 单位/秒，与 TTS 朗读速度大致同步（可读、不晃眼）：
    // 中文按"字"、英文按"词"（单词+紧连标点+词后空格）各算一个揭示单位
    if (now - s_lastTypeMs < 143) return;
    s_lastTypeMs = now;

    // 计算本拍的揭示单位
    const char *p = s_typingRest.c_str();
    size_t n = s_typingRest.length();
    size_t i = 0;
    while (i < n && p[i] == ' ') i++;              // 前导空格并入本单位
    if (i < n && (unsigned char)p[i] >= 0x21 && (unsigned char)p[i] <= 0x7E) {
        // 英文：连续 ASCII 可见字符（单词/数字+紧连标点）整体一次揭示
        while (i < n && (unsigned char)p[i] >= 0x21 && (unsigned char)p[i] <= 0x7E) i++;
        while (i < n && p[i] == ' ') i++;          // 词后空格一并带出
    } else {
        // 中文等非 ASCII：按 1 个 UTF-8 字符揭示
        uint8_t c = (unsigned char)p[i];
        int len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        i += len;
    }
    if (i > n) i = n;
    if (i == 0) i = 1;                             // 防御：保证每拍有推进

    String ch = s_typingRest.substring(0, i);
    s_typingRest.remove(0, i);

    String test = s_curLine + ch;
    if (textW(test) > SUB_W && s_curLine.length()) {
        pushLines(s_curLine, s_curColor);            // 结构性换行→差异路径多刷几行
        s_curLine = ch;
    } else {
        s_curLine = test;
    }
    s_dirtySub = true;
}

void tick() {
    if (!s_ok) return;

    tickTypewriter();
    topBarHousekeeping();               // 时间/WiFi/电量/音量条到期（内部标脏）

    if (!s_dirtySub && !s_dirtyBar) return;

    uint32_t ivl = typingActive() ? TYPE_DRAW_MS : IDLE_DRAW_MS;
    if (millis() - s_lastDraw < ivl) return;
    s_lastDraw = millis();

    if (s_dirtyBar) {
        s_dirtyBar = false;
        drawTopBar();
    }
    if (s_dirtySub) {
        s_dirtySub = false;
        drawSubtitles();
    }
}

} // namespace DisplayUI
