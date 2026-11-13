#pragma once
#include <Arduino.h>

static constexpr int kRemoteKeyCount = 21;
static constexpr int kMaxReportBytes = 16;
static constexpr int kReportRing = 24;

struct RemoteKeyDef {
    const char *id;
    const char *zh;
};

struct HidReportSample {
    uint32_t ms = 0;
    uint8_t len = 0;
    uint8_t data[kMaxReportBytes] = {};
    bool isZero = true;
};

enum class UsbOutKind : uint8_t { None = 0, Keyboard = 1, Consumer = 2, MouseWheel = 3 };

// usbMods bits for Keyboard chords
static constexpr uint8_t kUsbModCtrl = 1;
static constexpr uint8_t kUsbModShift = 2;
static constexpr uint8_t kUsbModAlt = 4;
static constexpr uint8_t kUsbModGui = 8;

struct KeyMapping {
    bool learned = false;
    uint8_t len = 0;
    uint8_t data[kMaxReportBytes] = {};
    UsbOutKind usbKind = UsbOutKind::None;
    uint16_t usbCode = 0;
    uint8_t usbMods = 0; // Ctrl/Shift/Alt/Gui
    char usbLabel[40] = {}; // e.g. ArrowUp, Escape, Ctrl+0
};

const RemoteKeyDef *remoteKeyDefs();

void keyLearnBegin();
void keyLearnInstallDefaults();
void keyLearnOnHidReport(const uint8_t *data, size_t len);
void keyLearnPoll(); // timeout leave wait_release when remote sends no zero

int keyLearnRecentCount();
bool keyLearnRecentAt(int newestIndex, HidReportSample &out);

void keyLearnStart(int step = 0, bool oneShot = false);
void keyLearnSetStep(int step);
void keyLearnSkip(); // force leave wait_release / go next (or stop if oneShot)
int keyLearnStep();
bool keyLearnActive();
void keyLearnStop();
bool keyLearnArmed();
bool keyLearnWaitingRelease();
bool keyLearnOneShot();

const KeyMapping *keyLearnGetMap(int index);
bool keyLearnSetMap(int index, const uint8_t *data, size_t len);
bool keyLearnSetUsb(int index, UsbOutKind kind, uint16_t code, uint8_t mods, const char *label);
void keyLearnClearAll();
bool keyLearnSave();
bool keyLearnLoad();
String keyLearnMapJson();
bool keyLearnApplyJson(const String &json);

int keyLearnMatch(const uint8_t *data, size_t len);
bool keyLearnLooksLikeVoice(const uint8_t *data, size_t len);

// Resolve label (supports Ctrl+0 / Shift+Escape) -> kind+code+mods
bool keyLearnParseUsbLabel(const char *label, UsbOutKind &kind, uint16_t &code, uint8_t &mods);
