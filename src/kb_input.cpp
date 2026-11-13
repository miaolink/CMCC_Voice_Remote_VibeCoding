#include "kb_input.h"

#include <Adafruit_TCA8418.h>
#include <Wire.h>

static constexpr int kSda = 8;
static constexpr int kScl = 9;
static constexpr uint8_t kAddr = 0x34;

// Cardputer ADV value_first map (same as Launcher Keyboard.h)
static const char kMap[4][14] = {
    {'`', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', (char)0x2a},
    {(char)0x2b, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\\'},
    {(char)0xff, (char)0x81, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', (char)0x28},
    {(char)0x80, (char)0x00, (char)0x82, 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', ' '},
};

static Adafruit_TCA8418 tca;
static bool ready = false;

static void mapRaw(uint8_t keyvalue, uint8_t &row, uint8_t &col) {
    const uint8_t u = keyvalue % 10;
    const uint8_t t = keyvalue / 10;
    if (u >= 1 && u <= 8 && t <= 6) {
        const uint8_t u0 = u - 1;
        row = u0 & 0x03;
        col = (t << 1) | (u0 >> 2);
    } else {
        row = 0xFF;
        col = 0xFF;
    }
}

static UiKey mapChar(char c) {
    if (c == ';') return UiKey::Up;
    if (c == '.') return UiKey::Down;
    if (c == (char)0x28 || c == ' ') return UiKey::Enter;
    if (c == '`' || c == (char)0x2a) return UiKey::Back;
    if (c == 's') return UiKey::Scan;
    return UiKey::None;
}

void kbBegin() {
    pinMode(5, OUTPUT);
    digitalWrite(5, HIGH);

    Wire.begin(kSda, kScl);
    delay(50);
    ready = tca.begin(kAddr, &Wire);
    if (!ready) {
        Serial.println("[KB] TCA8418 init failed");
        return;
    }
    tca.matrix(7, 8);
    tca.flush();
    tca.enableInterrupts();
    Serial.println("[KB] ready  ;=up .=down Enter=OK `=back s=scan");
}

UiKey kbPoll() {
    if (!ready) return UiKey::None;

    while (tca.available() > 0) {
        int keyEvent = tca.getEvent();
        bool pressed = (keyEvent & 0x80) != 0;
        uint8_t value = keyEvent & 0x7F;
        if (!pressed) continue;

        uint8_t row, col;
        mapRaw(value, row, col);
        if (row >= 4 || col >= 14) continue;

        char c = kMap[row][col];
        UiKey uk = mapChar(c);
        if (uk != UiKey::None) {
            Serial.printf("[KB] %c -> %d\n", (c >= 32 && c < 127) ? c : '?', (int)uk);
            // clear IRQ flag
            tca.writeRegister(TCA8418_REG_INT_STAT, 1);
            return uk;
        }
    }
    tca.writeRegister(TCA8418_REG_INT_STAT, 1);
    return UiKey::None;
}