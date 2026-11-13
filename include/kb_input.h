#pragma once
#include <Arduino.h>

enum class UiKey : uint8_t {
    None = 0,
    Up,
    Down,
    Enter,
    Back,
    Scan,
};

void kbBegin();
UiKey kbPoll();