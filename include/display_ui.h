#pragma once
#include <Arduino.h>
#include "keymap.h"

enum class UiScreen : uint8_t {
    Main = 0,
    Scanning,
    Devices,
    Status,
};

void uiBegin();
void uiBoot(const char *msg); // immediate boot splash (no wait for uiLoop)
void uiSetStatus(const char *line1, const char *line2 = "");
void uiSetLastKey(ArrowKey key);
void uiSetLastKeyName(const char *name); // any remote key label (中文/id)
void uiSetScreen(UiScreen screen);
void uiSetMenuCursor(int cursor);
void uiSetDeviceCount(int count);
void uiSetHint(const char *hint);
void uiInvalidate();
void uiLoop();