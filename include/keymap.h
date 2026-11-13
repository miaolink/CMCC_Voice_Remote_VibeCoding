#pragma once
#include <Arduino.h>

enum class ArrowKey : uint8_t {
    None = 0,
    Up,
    Down,
    Left,
    Right,
};

const char *arrowKeyName(ArrowKey key);

// Parse a BLE HID input report. Returns ArrowKey::None if not an arrow.
// Also used for release detection: all-zero / empty report => None (release).
ArrowKey parseArrowFromHidReport(const uint8_t *data, size_t len, bool *isRelease);