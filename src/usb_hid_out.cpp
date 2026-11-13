#include "usb_hid_out.h"
#include "key_learn.h"
#include "sd_log.h"

#include "USB.h"
#include "USBHIDConsumerControl.h"
#include "USBHIDKeyboard.h"
#include "USBHIDMouse.h"

#ifndef KEY_LEFT_CTRL
#define KEY_LEFT_CTRL 0x80
#define KEY_LEFT_SHIFT 0x81
#define KEY_LEFT_ALT 0x82
#define KEY_LEFT_GUI 0x83
#endif

static USBHIDKeyboard keyboard;
static USBHIDConsumerControl consumer;
static USBHIDMouse mouse;
static bool started = false;
static int currentRemote = -1;
static bool currentIsConsumer = false;
static bool currentIsWheel = false;
static uint8_t currentKb = 0;
static uint8_t currentMods = 0;
static ArrowKey currentArrow = ArrowKey::None;

static bool resolveOut(int idx, UsbOutKind &kind, uint16_t &code, uint8_t &mods) {
    mods = 0;
    const KeyMapping *m = keyLearnGetMap(idx);
    if (m && m->usbKind != UsbOutKind::None &&
        (m->usbCode != 0 || m->usbMods != 0 || m->usbKind == UsbOutKind::MouseWheel)) {
        kind = m->usbKind;
        code = m->usbCode;
        mods = m->usbMods;
        return true;
    }
    // voice(7) empty: no USB fallback — mic PTT only
    static const char *kUsb[kRemoteKeyCount] = {
        "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "Enter", "Home", "Escape", "",
        "Ctrl+Shift+B", "VolumeUp", "VolumeDown", "WheelUp", "WheelDown", "Menu", "Ctrl+0", "F10",
        "F11", "Ctrl+Shift+V", "Ctrl+Shift+C", "Ctrl+Shift+X", "Ctrl+Shift+Z",
    };
    if (!kUsb[idx] || !kUsb[idx][0]) return false;
    return keyLearnParseUsbLabel(kUsb[idx], kind, code, mods);
}

static void pressMods(uint8_t mods) {
    // Modifier-only press() updates internal report but does not send until a key is pressed.
    if (mods & kUsbModCtrl) keyboard.press(KEY_LEFT_CTRL);
    if (mods & kUsbModShift) keyboard.press(KEY_LEFT_SHIFT);
    if (mods & kUsbModAlt) keyboard.press(KEY_LEFT_ALT);
    if (mods & kUsbModGui) keyboard.press(KEY_LEFT_GUI);
}

static uint8_t normalizePrintableKey(uint8_t k) {
    // Avoid uppercase ASCII: Keyboard.press('B') forces Left-Shift and breaks Alt/Ctrl chords.
    if (k >= 'A' && k <= 'Z') return (uint8_t)(k - 'A' + 'a');
    return k;
}

void usbHidBegin() {
    if (started) return;
    keyboard.begin();
    consumer.begin();
    mouse.begin();
    USB.begin();
    started = true;
    Serial.println("[USB] HID keyboard+consumer+mouse ENABLED");
}

bool usbHidReady() { return started; }

void usbHidReleaseRemote() {
    if (!started || currentRemote < 0) return;
    if (currentIsWheel) {
        // wheel is pulse-only
    } else if (currentIsConsumer) {
        consumer.release();
    } else {
        keyboard.releaseAll();
    }
    dlog("usb release remote[%d]", currentRemote);
    currentRemote = -1;
    currentIsConsumer = false;
    currentIsWheel = false;
    currentKb = 0;
    currentMods = 0;
    currentArrow = ArrowKey::None;
}

void usbHidPressRemote(int keyIndex) {
    if (!started || keyIndex < 0 || keyIndex >= kRemoteKeyCount) return;
    if (currentRemote == keyIndex) return;
    usbHidReleaseRemote();

    UsbOutKind kind = UsbOutKind::None;
    uint16_t code = 0;
    uint8_t mods = 0;
    if (!resolveOut(keyIndex, kind, code, mods) || kind == UsbOutKind::None) return;

    currentRemote = keyIndex;
    currentMods = mods;
    currentIsWheel = false;
    currentIsConsumer = false;

    if (kind == UsbOutKind::Consumer) {
        currentIsConsumer = true;
        consumer.press(code);
    } else if (kind == UsbOutKind::MouseWheel) {
        currentIsWheel = true;
        int8_t delta = (int8_t)(int16_t)code;
        if (delta == 0) delta = 1;
        mouse.move(0, 0, delta);
    } else {
        currentKb = normalizePrintableKey((uint8_t)code);
        // Hold: mods first (latched in report), then key sends Alt/Ctrl+key together
        pressMods(mods);
        if (currentKb) keyboard.press(currentKb);
        Serial.printf("[USB] chord mods=0x%02X key=0x%02X ('%c')\n", (unsigned)mods, (unsigned)currentKb,
                      (currentKb >= 32 && currentKb < 127) ? (char)currentKb : '?');
    }
    const KeyMapping *m = keyLearnGetMap(keyIndex);
    const RemoteKeyDef *defs = remoteKeyDefs();
    Serial.printf("[USB] press %s -> %s\n", defs[keyIndex].zh, m && m->usbLabel[0] ? m->usbLabel : "?");
    dlog("usb press %s -> %s", defs[keyIndex].id, m && m->usbLabel[0] ? m->usbLabel : "?");

    switch (keyIndex) {
        case 0: currentArrow = ArrowKey::Up; break;
        case 1: currentArrow = ArrowKey::Down; break;
        case 2: currentArrow = ArrowKey::Left; break;
        case 3: currentArrow = ArrowKey::Right; break;
        default: currentArrow = ArrowKey::None; break;
    }
}

void usbHidPressArrow(ArrowKey key) {
    int idx = -1;
    switch (key) {
        case ArrowKey::Up: idx = 0; break;
        case ArrowKey::Down: idx = 1; break;
        case ArrowKey::Left: idx = 2; break;
        case ArrowKey::Right: idx = 3; break;
        default: return;
    }
    usbHidPressRemote(idx);
}

void usbHidReleaseArrow() { usbHidReleaseRemote(); }
ArrowKey usbHidCurrentArrow() { return currentArrow; }
int usbHidCurrentRemote() { return currentRemote; }
