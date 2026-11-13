#include <Arduino.h>

#include "ble_remote.h"
#include "display_ui.h"
#include "kb_input.h"
#include "key_learn.h"
#include "sd_log.h"
#include "usb_hid_out.h"
#include "usb_audio_mic.h"
#include "voice_audio.h"
#include "voice_probe.h"
#include "wifi_web.h"

static constexpr int kMainCount = 6;

static UiScreen screen = UiScreen::Main;
static int cursor = 0;
static uint32_t lastUiRefresh = 0;

static void showMain() {
    screen = UiScreen::Main;
    if (cursor < 0) cursor = 0;
    if (cursor >= kMainCount) cursor = kMainCount - 1;
    uiSetScreen(screen);
    uiSetMenuCursor(cursor);
    uiSetHint(";/. Enter `  s=scan");
}

static void showDevices() {
    screen = UiScreen::Devices;
    cursor = 0;
    uiSetDeviceCount(bleDeviceCount());
    uiSetScreen(screen);
    uiSetMenuCursor(cursor);
}

static void showStatus() {
    screen = UiScreen::Status;
    uiSetScreen(screen);
}

static void startScan() {
    screen = UiScreen::Scanning;
    cursor = 0;
    uiSetScreen(screen);
    uiSetDeviceCount(0);
    uiSetStatus("scanning", "8s");
    bleStartScan(8000);
}

static void doAutoPair() {
    uiSetStatus("autopair", kDefaultCmccName);
    bleAutoPairCmcc();
    showStatus();
}

static void doRunHid() {
    if (usbHidReady()) {
        uiSetStatus("HID already ON", "COM+keyboard");
        showStatus();
        return;
    }
    uiSetStatus("starting HID", "COM may blink");
    uiInvalidate();
    uiLoop();
    delay(50);
    if (!usbAudioReady()) usbAudioBegin();
    usbHidBegin();
    uiSetStatus("HID ON", "arrows->PC");
    showStatus();
}

static void activateMain() {
    switch (cursor) {
        case 0:
            doAutoPair();
            break;
        case 1:
            doRunHid();
            break;
        case 2:
            startScan();
            break;
        case 3:
            showDevices();
            break;
        case 4:
            bleDisconnect();
            uiSetStatus("disconnected", "");
            break;
        case 5:
            showStatus();
            break;
        default:
            break;
    }
}

static void onBleStatus(const char *status, const char *detail) {
    uiSetStatus(status, detail);
    uiSetDeviceCount(bleDeviceCount());
    slog("ble %s %s", status ? status : "", detail ? detail : "");
}

static void onBleRaw(const uint8_t *raw, size_t len, bool isRelease) {
    if (isRelease) {
        if (usbHidReady()) usbHidReleaseRemote();
        uiSetLastKeyName("-");
        return;
    }
    // Voice/mic: PTT only — never USB HID (Settings/0x0183 used to open Media Player)
    if (keyLearnLooksLikeVoice(raw, len)) {
        uiSetLastKeyName("语音");
        uiSetStatus("key", "语音/PTT");
        return;
    }
    int idx = keyLearnMatch(raw, len);
    if (idx < 0) {
        dlog("ui unmatched len=%u", (unsigned)len);
        return;
    }
    const RemoteKeyDef *defs = remoteKeyDefs();
    uiSetLastKeyName(defs[idx].zh);
    uiSetStatus("key", defs[idx].zh);
    dlog("remote[%d] %s hid=%d", idx, defs[idx].id, (int)usbHidReady());
    if (defs[idx].id && strcmp(defs[idx].id, "voice") == 0) return;
    if (idx == 15) {
        // Extra guard: never emit Consumer Settings/0x0183
        const KeyMapping *m = keyLearnGetMap(15);
        if (m && m->usbKind == UsbOutKind::Consumer && m->usbCode == 0x0183) return;
    }
    if (usbHidReady()) usbHidPressRemote(idx);
    else uiSetStatus("key(no HID)", defs[idx].zh);
}

static void onBleArrow(ArrowKey key, bool pressed, const uint8_t * /*raw*/, size_t /*len*/) {
    // When learned map exists, onBleRaw handles output.
    if (keyLearnGetMap(0) && keyLearnGetMap(0)->learned) return;
    if (pressed) {
        uiSetLastKey(key);
        if (usbHidReady()) usbHidPressArrow(key);
    } else {
        if (usbHidReady()) usbHidReleaseRemote();
        uiSetLastKey(ArrowKey::None);
    }
}

static void handleKey(UiKey key) {
    if (key == UiKey::None) return;

    switch (screen) {
        case UiScreen::Main:
            if (key == UiKey::Up) {
                cursor = (cursor + kMainCount - 1) % kMainCount;
                uiSetMenuCursor(cursor);
            } else if (key == UiKey::Down) {
                cursor = (cursor + 1) % kMainCount;
                uiSetMenuCursor(cursor);
            } else if (key == UiKey::Enter) {
                activateMain();
            } else if (key == UiKey::Scan) {
                startScan();
            }
            break;

        case UiScreen::Scanning:
            if (key == UiKey::Back) {
                bleStopScan();
                showMain();
            } else if (key == UiKey::Enter || key == UiKey::Scan) {
                bleStopScan();
                showDevices();
            }
            break;

        case UiScreen::Devices: {
            int n = bleDeviceCount();
            if (key == UiKey::Back) {
                showMain();
            } else if (n <= 0) {
                if (key == UiKey::Scan || key == UiKey::Enter) startScan();
            } else if (key == UiKey::Up) {
                cursor = (cursor + n - 1) % n;
                uiSetMenuCursor(cursor);
            } else if (key == UiKey::Down) {
                cursor = (cursor + 1) % n;
                uiSetMenuCursor(cursor);
            } else if (key == UiKey::Enter) {
                uiSetStatus("pairing", "...");
                showStatus();
                bleRequestConnectIndex(cursor);
            } else if (key == UiKey::Scan) {
                startScan();
            }
            break;
        }

        case UiScreen::Status:
            if (key == UiKey::Back || key == UiKey::Enter) {
                showMain();
            } else if (key == UiKey::Scan) {
                doAutoPair();
            }
            break;
    }
}

void setup() {
    Serial.begin(115200);
    delay(50);
    Serial.println();
    Serial.println("=== CMCC BLE Bridge + WiFi Web ===");

    uiBegin(); // screen on immediately
    uiBoot("1/5 keyboard");
    kbBegin();

    uiBoot("2/5 SD card");
    sdLogBegin();
    slog("=== bridge boot ===");

    uiBoot(sdMounted() ? "3/5 keymap (SD ok)" : "3/5 keymap (no SD)");
    keyLearnBegin();
    voiceProbeBegin();
    voiceAudioBegin();

    uiBoot("3b/5 USB UAC+HID");
    usbAudioBegin();
    usbHidBegin();

    uiBoot("4/5 BLE stack");
    bleRemoteBegin(onBleStatus, onBleArrow, onBleRaw);

    uiBoot("5/5 WiFi STA");
    wifiWebBegin();
    slog("wifi mode=%s (async STA)", wifiModeText());

    cursor = 0;
    uiSetStatus("ready", "joining wifi...");
    showMain();
    uiLoop();
    sdLogFlush();
}

void loop() {
    bleRemoteLoop();
    voiceAudioLoop();
    usbAudioLoop();
    keyLearnPoll();
    wifiWebLoop();
    sdLogLoop();
    handleKey(kbPoll());

    if (millis() - lastUiRefresh > 500) {
        lastUiRefresh = millis();
        uiSetDeviceCount(bleDeviceCount());
        uiInvalidate();
    }

    uiLoop();
    delay(2);
}