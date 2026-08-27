// display_ui.h —— 屏幕 UI：顶部状态栏 + 中文字幕区（打字机效果、自动滚动）
#pragma once
#include <Arduino.h>

namespace DisplayUI {
bool begin();
void setState(const char *state);        // IDLE/LISTEN/PLAY/OFFLINE（顶栏，带颜色）
void setVolume(int v);                   // 0~100，顶栏显示
void setInfo(const String &line);        // 保留接口（当前顶栏不含此行）
void tick();                             // 主循环调用：打字机推进 + 节流重绘

// 字幕接口
void addUserLine(const String &text);    // 用户说的话（绿色），立即整行显示
void beginReplyTypewriter(const String &text); // AI 回复：逐字显示（与播放同步观感）
void flushTyping();                      // 立即显示剩余未打的字（打断时调用）
void clearChat();                        // 清空字幕

// 内部使用
void _setStateRaw(const char *state);
}
