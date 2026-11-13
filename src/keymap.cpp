#include "keymap.h"

const char *arrowKeyName(ArrowKey key) {
    switch (key) {
        case ArrowKey::Up: return "UP";
        case ArrowKey::Down: return "DOWN";
        case ArrowKey::Left: return "LEFT";
        case ArrowKey::Right: return "RIGHT";
        default: return "-";
    }
}

static bool isAllZero(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (data[i] != 0) return false;
    }
    return true;
}

static ArrowKey fromKeyboardUsage(uint8_t usage) {
    switch (usage) {
        case 0x52: return ArrowKey::Up;
        case 0x51: return ArrowKey::Down;
        case 0x50: return ArrowKey::Left;
        case 0x4F: return ArrowKey::Right;
        default: return ArrowKey::None;
    }
}

// USB HID Consumer Control (0x0C) + Generic Desktop D-pad
static ArrowKey fromConsumerUsage(uint16_t usage) {
    switch (usage) {
        case 0x0042: // Menu Up
        case 0x0090: // D-pad Up
            return ArrowKey::Up;
        case 0x0043: // Menu Down
        case 0x0091: // D-pad Down
            return ArrowKey::Down;
        case 0x0044: // Menu Left
        case 0x0093: // D-pad Left
            return ArrowKey::Left;
        case 0x0045: // Menu Right
        case 0x0092: // D-pad Right
            return ArrowKey::Right;
        default:
            return ArrowKey::None;
    }
}

static ArrowKey fromBitfield(const uint8_t *data, size_t len) {
    // Some remotes: 1-2 byte bitmasks for directions
    if (len == 1) {
        if (data[0] & 0x01) return ArrowKey::Up;
        if (data[0] & 0x02) return ArrowKey::Down;
        if (data[0] & 0x04) return ArrowKey::Left;
        if (data[0] & 0x08) return ArrowKey::Right;
    }
    if (len == 2 && data[0] == 0) {
        if (data[1] & 0x01) return ArrowKey::Up;
        if (data[1] & 0x02) return ArrowKey::Down;
        if (data[1] & 0x04) return ArrowKey::Left;
        if (data[1] & 0x08) return ArrowKey::Right;
    }
    return ArrowKey::None;
}

ArrowKey parseArrowFromHidReport(const uint8_t *data, size_t len, bool *isRelease) {
    if (isRelease) *isRelease = false;
    if (!data || len == 0) {
        if (isRelease) *isRelease = true;
        return ArrowKey::None;
    }
    if (isAllZero(data, len)) {
        if (isRelease) *isRelease = true;
        return ArrowKey::None;
    }

    // Boot keyboard: [modifier][reserved][key1..key6]
    if (len >= 3 && len <= 8) {
        for (size_t i = 2; i < len; ++i) {
            ArrowKey k = fromKeyboardUsage(data[i]);
            if (k != ArrowKey::None) return k;
        }
        ArrowKey k1 = fromKeyboardUsage(data[1]);
        if (k1 != ArrowKey::None) return k1;
    }

    // Report-ID prefixed keyboard: [id][mod][res][keys...]
    if (len >= 4 && len <= 9) {
        for (size_t i = 3; i < len; ++i) {
            ArrowKey k = fromKeyboardUsage(data[i]);
            if (k != ArrowKey::None) return k;
        }
    }

    if (len == 1) {
        ArrowKey k = fromKeyboardUsage(data[0]);
        if (k != ArrowKey::None) return k;
        k = fromBitfield(data, len);
        if (k != ArrowKey::None) return k;
    }

    // Consumer control: [lo][hi] or [id][lo][hi]
    if (len == 2) {
        uint16_t u = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
        ArrowKey k = fromConsumerUsage(u);
        if (k != ArrowKey::None) return k;
        k = fromBitfield(data, len);
        if (k != ArrowKey::None) return k;
        k = fromKeyboardUsage(data[0]);
        if (k != ArrowKey::None) return k;
        k = fromKeyboardUsage(data[1]);
        if (k != ArrowKey::None) return k;
    }
    if (len == 3) {
        uint16_t u = (uint16_t)data[1] | ((uint16_t)data[2] << 8);
        ArrowKey k = fromConsumerUsage(u);
        if (k != ArrowKey::None) return k;
        // also try [lo][hi] ignoring id at end
        u = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
        k = fromConsumerUsage(u);
        if (k != ArrowKey::None) return k;
    }

    ArrowKey kb = ArrowKey::None;
    for (size_t i = 0; i < len; ++i) {
        kb = fromKeyboardUsage(data[i]);
        if (kb != ArrowKey::None) return kb;
    }

    // Scan 16-bit LE usages anywhere in report
    for (size_t i = 0; i + 1 < len; ++i) {
        uint16_t u = (uint16_t)data[i] | ((uint16_t)data[i + 1] << 8);
        ArrowKey k = fromConsumerUsage(u);
        if (k != ArrowKey::None) return k;
    }

    return fromBitfield(data, len);
}