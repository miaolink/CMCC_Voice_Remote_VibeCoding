#include "key_learn.h"
#include "sd_log.h"

#include <Preferences.h>
#include <SD.h>

static const RemoteKeyDef kDefs[kRemoteKeyCount] = {
    {"up", "上"},       {"down", "下"},     {"left", "左"},      {"right", "右"},
    {"ok", "确定"},     {"home", "首页"},   {"back", "返回"},    {"voice", "语音"},
    {"m", "M"},         {"vol_up", "音量+"}, {"vol_down", "音量-"}, {"ch_up", "频道+"},
    {"ch_down", "频道-"}, {"menu", "菜单"}, {"mute", "静音"},    {"settings", "设置"},
    {"tv", "TV"},       {"source", "信源"}, {"power", "电源"},   {"live", "直播"},
    {"switch", "切换"},
};

static KeyMapping gMap[kRemoteKeyCount];
static HidReportSample gRing[kReportRing];
static int gRingHead = 0;
static int gRingCount = 0;

static bool gLearnActive = false;
static bool gArmed = false;
static bool gWaitRelease = false;
static bool gLearnOneShot = false;
static int gStep = 0;
static uint32_t gWaitReleaseMs = 0;
static Preferences gPrefs;

static void learnAdvanceAfterRelease() {
    if (gLearnOneShot) {
        keyLearnSave();
        gLearnActive = false;
        gArmed = false;
        gWaitRelease = false;
        gLearnOneShot = false;
        slog("learn one-shot done [%d] %s", gStep, kDefs[gStep].zh);
        return;
    }
    if (gStep + 1 < kRemoteKeyCount) {
        gStep++;
        gArmed = true;
        gWaitRelease = false;
        slog("learn next [%d] %s — 请按键", gStep, kDefs[gStep].zh);
    } else {
        keyLearnSave();
        gLearnActive = false;
        gArmed = false;
        gWaitRelease = false;
        slog("learn complete %d/%d", kRemoteKeyCount, kRemoteKeyCount);
    }
}

const RemoteKeyDef *remoteKeyDefs() { return kDefs; }

static bool isZero(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i)
        if (data[i]) return false;
    return true;
}

static String toHex(const uint8_t *data, size_t len) {
    String s;
    s.reserve(len * 3);
    for (size_t i = 0; i < len; ++i) {
        if (i) s += ' ';
        char b[4];
        snprintf(b, sizeof(b), "%02X", data[i]);
        s += b;
    }
    return s;
}

static bool sameReport(const KeyMapping &m, const uint8_t *data, size_t len) {
    if (!m.learned || !data || !len) return false;
    // Allow 16 vs 20 (etc.) when the shorter side matches and the rest is zero-pad
    size_t n = m.len < len ? m.len : len;
    if (memcmp(m.data, data, n) != 0) return false;
    for (size_t i = n; i < m.len; ++i)
        if (m.data[i]) return false;
    for (size_t i = n; i < len; ++i)
        if (data[i]) return false;
    return true;
}

// CMCC voice/mic key family (ignore trailing pad / len variance)
bool keyLearnLooksLikeVoice(const uint8_t *data, size_t len) {
    static const uint8_t kVoice[] = {0x82, 0x03, 0x01, 0x00, 0x01, 0x00, 0x01};
    if (!data || len < sizeof(kVoice)) return false;
    return memcmp(data, kVoice, sizeof(kVoice)) == 0;
}

static bool parseHexByte(const char *p, uint8_t &out) {
    auto h = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int a = h(p[0]), b = h(p[1]);
    if (a < 0 || b < 0) return false;
    out = (uint8_t)((a << 4) | b);
    return true;
}

static bool parseHexList(const String &hex, uint8_t *out, uint8_t &len) {
    len = 0;
    for (int i = 0; i + 1 < (int)hex.length() && len < kMaxReportBytes;) {
        while (i < (int)hex.length() && (hex[i] == ' ' || hex[i] == ',')) i++;
        if (i + 1 >= (int)hex.length()) break;
        uint8_t b;
        if (!parseHexByte(hex.c_str() + i, b)) return false;
        out[len++] = b;
        i += 2;
    }
    return len > 0;
}

bool keyLearnSetUsb(int index, UsbOutKind kind, uint16_t code, uint8_t mods, const char *label) {
    if (index < 0 || index >= kRemoteKeyCount) return false;
    gMap[index].usbKind = kind;
    gMap[index].usbCode = code;
    gMap[index].usbMods = mods;
    gMap[index].usbLabel[0] = 0;
    if (label && label[0]) {
        strncpy(gMap[index].usbLabel, label, sizeof(gMap[index].usbLabel) - 1);
        gMap[index].usbLabel[sizeof(gMap[index].usbLabel) - 1] = 0;
    }
    return true;
}

static bool parseOneKeyToken(const String &tok, UsbOutKind &kind, uint16_t &code) {
    String s = tok;
    s.trim();
    if (!s.length()) return false;

    struct Item {
        const char *name;
        UsbOutKind kind;
        uint16_t code;
    };
    static const Item kItems[] = {
        {"ArrowUp", UsbOutKind::Keyboard, 0xDA},
        {"ArrowDown", UsbOutKind::Keyboard, 0xD9},
        {"ArrowLeft", UsbOutKind::Keyboard, 0xD8},
        {"ArrowRight", UsbOutKind::Keyboard, 0xD7},
        {"Enter", UsbOutKind::Keyboard, 0xB0},
        {"Escape", UsbOutKind::Keyboard, 0xB1},
        {"Esc", UsbOutKind::Keyboard, 0xB1},
        {"Backspace", UsbOutKind::Keyboard, 0xB2},
        {"Tab", UsbOutKind::Keyboard, 0xB3},
        {"PageUp", UsbOutKind::Keyboard, 0xD3},
        {"PageDown", UsbOutKind::Keyboard, 0xD6},
        {"Home", UsbOutKind::Keyboard, 0xD2},
        {"End", UsbOutKind::Keyboard, 0xD5},
        {"Insert", UsbOutKind::Keyboard, 0xD1},
        {"Delete", UsbOutKind::Keyboard, 0xD4},
        {"Menu", UsbOutKind::Keyboard, 0xED},
        {"F1", UsbOutKind::Keyboard, 0xC2},  {"F2", UsbOutKind::Keyboard, 0xC3},
        {"F3", UsbOutKind::Keyboard, 0xC4},  {"F4", UsbOutKind::Keyboard, 0xC5},
        {"F5", UsbOutKind::Keyboard, 0xC6},  {"F6", UsbOutKind::Keyboard, 0xC7},
        {"F7", UsbOutKind::Keyboard, 0xC8},  {"F8", UsbOutKind::Keyboard, 0xC9},
        {"F9", UsbOutKind::Keyboard, 0xCA},  {"F10", UsbOutKind::Keyboard, 0xCB},
        {"F11", UsbOutKind::Keyboard, 0xCC}, {"F12", UsbOutKind::Keyboard, 0xCD},
        {"Space", UsbOutKind::Keyboard, ' '},
        {"Vol+", UsbOutKind::Consumer, 0x00E9},
        {"Vol-", UsbOutKind::Consumer, 0x00EA},
        {"VolumeUp", UsbOutKind::Consumer, 0x00E9},
        {"VolumeDown", UsbOutKind::Consumer, 0x00EA},
        {"Mute", UsbOutKind::Consumer, 0x00E2},
        {"BrowserHome", UsbOutKind::Consumer, 0x0223},
        {"BrowserBack", UsbOutKind::Consumer, 0x0224},
        {"AcHome", UsbOutKind::Consumer, 0x0223},
        {"AcBack", UsbOutKind::Consumer, 0x0224},
        // Do NOT use Consumer 0x0183 — Windows treats it as "open Media Player"
        {"Settings", UsbOutKind::Keyboard, 0xCB}, // F10
        {"WheelUp", UsbOutKind::MouseWheel, 1},
        {"WheelDown", UsbOutKind::MouseWheel, (uint16_t)(int16_t)-1},
        {"Wheel+1", UsbOutKind::MouseWheel, 1},
        {"Wheel-1", UsbOutKind::MouseWheel, (uint16_t)(int16_t)-1},
        {"ScrollUp", UsbOutKind::MouseWheel, 1},
        {"ScrollDown", UsbOutKind::MouseWheel, (uint16_t)(int16_t)-1},
    };
    for (auto &it : kItems) {
        if (s.equalsIgnoreCase(it.name)) {
            kind = it.kind;
            code = it.code;
            return true;
        }
    }
    if (s.length() == 1) {
        kind = UsbOutKind::Keyboard;
        char ch = s[0];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        code = (uint16_t)(uint8_t)ch;
        return true;
    }
    return false;
}

static uint8_t modFromToken(const String &tok) {
    if (tok.equalsIgnoreCase("Ctrl") || tok.equalsIgnoreCase("Control") || tok.equalsIgnoreCase("Ctl"))
        return kUsbModCtrl;
    if (tok.equalsIgnoreCase("Shift")) return kUsbModShift;
    if (tok.equalsIgnoreCase("Alt") || tok.equalsIgnoreCase("Option") || tok.equalsIgnoreCase("LAlt") ||
        tok.equalsIgnoreCase("LeftAlt"))
        return kUsbModAlt;
    if (tok.equalsIgnoreCase("Meta") || tok.equalsIgnoreCase("Win") || tok.equalsIgnoreCase("Gui") ||
        tok.equalsIgnoreCase("Cmd") || tok.equalsIgnoreCase("Command"))
        return kUsbModGui;
    return 0;
}

bool keyLearnParseUsbLabel(const char *label, UsbOutKind &kind, uint16_t &code, uint8_t &mods) {
    kind = UsbOutKind::None;
    code = 0;
    mods = 0;
    if (!label || !label[0]) return false;
    String s = label;
    s.trim();
    s.replace(" ", "");

    if (s.startsWith("kb:") || s.startsWith("KB:")) {
        kind = UsbOutKind::Keyboard;
        code = (uint16_t)strtoul(s.c_str() + 3, nullptr, 0);
        return true;
    }
    if (s.startsWith("cc:") || s.startsWith("CC:")) {
        kind = UsbOutKind::Consumer;
        code = (uint16_t)strtoul(s.c_str() + 3, nullptr, 0);
        return true;
    }
    if (s.startsWith("wheel:") || s.startsWith("Wheel:") || s.startsWith("wh:")) {
        kind = UsbOutKind::MouseWheel;
        int v = (int)strtol(strchr(s.c_str(), ':') + 1, nullptr, 0);
        if (v == 0) v = 1;
        code = (uint16_t)(int16_t)v;
        return true;
    }

    // Exact name first (Vol+ / Wheel+1 must not be split on '+')
    if (parseOneKeyToken(s, kind, code)) {
        mods = 0;
        return true;
    }

    // Chord: Ctrl+Shift+0 / Ctrl+Escape
    uint8_t m = 0;
    int start = 0;
    String keyTok;
    while (start <= (int)s.length()) {
        int plus = s.indexOf('+', start);
        String part = plus < 0 ? s.substring(start) : s.substring(start, plus);
        part.trim();
        if (part.length()) {
            uint8_t bit = modFromToken(part);
            if (bit) m |= bit;
            else keyTok = part; // last non-mod token wins as key
        }
        if (plus < 0) break;
        start = plus + 1;
    }

    if (!keyTok.length()) return false;
    if (!parseOneKeyToken(keyTok, kind, code)) return false;
    if ((kind == UsbOutKind::Consumer || kind == UsbOutKind::MouseWheel) && m) return false;
    mods = m;
    return true;
}

static void installDefaultUsb(int i) {
    // Prefer VolumeUp/Down (no '+') so JSON/chord parsing never mangles Vol+
    // voice(7): no USB HID — reserved for PTT / open-mic only
    static const char *kUsb[kRemoteKeyCount] = {
        "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "Enter", "Home", "Escape", "",
        "Ctrl+Shift+B", "VolumeUp", "VolumeDown", "WheelUp", "WheelDown", "Menu", "Ctrl+0", "F10",
        "F11", "Ctrl+Shift+V", "Ctrl+Shift+C", "Ctrl+Shift+X", "Ctrl+Shift+Z",
    };
    if (!kUsb[i] || !kUsb[i][0]) {
        keyLearnSetUsb(i, UsbOutKind::None, 0, 0, "");
        return;
    }
    UsbOutKind kind;
    uint16_t code;
    uint8_t mods = 0;
    if (keyLearnParseUsbLabel(kUsb[i], kind, code, mods)) keyLearnSetUsb(i, kind, code, mods, kUsb[i]);
}

static void migrateUsbDefaults() {
    // Old built-in labels → user-requested mappings
    struct Mig {
        int i;
        const char *from;
        const char *to;
    };
    static const Mig kMig[] = {
        {5, "AcHome", "Home"},
        {6, "AcBack", "Escape"},
        {9, "Vol+", "VolumeUp"},
        {9, "Vol-", "VolumeUp"}, // broken parse used to store Vol- on both
        {10, "Vol-", "VolumeDown"},
        {10, "Vol+", "VolumeDown"},
        {11, "PageUp", "WheelUp"},
        {12, "PageDown", "WheelDown"},
        {14, "Mute", "Ctrl+0"},
        {15, "Settings", "F10"}, // 0x0183 opened Windows Media Player
        {8, "m", "Ctrl+Shift+B"},
        {8, "M", "Ctrl+Shift+B"},
    };
    bool changed = false;
    // Force M / TV / 信源 / 电源 / 直播 / 切换 → user chords
    {
        struct Force {
            int i;
            const char *to;
        };
        static const Force kForce[] = {
            {8, "Ctrl+Shift+B"},
            {16, "F11"},
            {17, "Ctrl+Shift+V"},
            {18, "Ctrl+Shift+C"},
            {19, "Ctrl+Shift+X"},
            {20, "Ctrl+Shift+Z"},
        };
        for (auto &f : kForce) {
            if (!String(gMap[f.i].usbLabel).equalsIgnoreCase(f.to)) {
                UsbOutKind kind;
                uint16_t code;
                uint8_t mods = 0;
                if (keyLearnParseUsbLabel(f.to, kind, code, mods)) {
                    keyLearnSetUsb(f.i, kind, code, mods, f.to);
                    changed = true;
                }
            }
        }
    }
    // Voice key: clear any old USB binding (Alt+b / F8 / …) — PTT only
    {
        String vl = gMap[7].usbLabel;
        if (gMap[7].usbKind != UsbOutKind::None || vl.length()) {
            if (vl.equalsIgnoreCase("Alt+b") || vl.equalsIgnoreCase("Alt+B") ||
                vl.equalsIgnoreCase("F8") || vl.equalsIgnoreCase("LAlt+B") ||
                vl.equalsIgnoreCase("LeftAlt+B") || vl.length() > 0) {
                keyLearnSetUsb(7, UsbOutKind::None, 0, 0, "");
                changed = true;
            }
        }
    }
    for (auto &m : kMig) {
        if (String(gMap[m.i].usbLabel).equalsIgnoreCase(m.from)) {
            UsbOutKind kind;
            uint16_t code;
            uint8_t mods = 0;
            if (keyLearnParseUsbLabel(m.to, kind, code, mods)) {
                keyLearnSetUsb(m.i, kind, code, mods, m.to);
                changed = true;
            }
        }
    }
    // Force-correct volume codes even if labels were mangled to the same Vol-
    {
        UsbOutKind k;
        uint16_t c;
        uint8_t mo = 0;
        if (gMap[9].usbKind == UsbOutKind::Consumer && gMap[9].usbCode == 0x00EA) {
            if (keyLearnParseUsbLabel("VolumeUp", k, c, mo)) {
                keyLearnSetUsb(9, k, c, mo, "VolumeUp");
                changed = true;
            }
        }
        if (gMap[10].usbKind == UsbOutKind::Consumer && gMap[10].usbCode == 0x00E9) {
            if (keyLearnParseUsbLabel("VolumeDown", k, c, mo)) {
                keyLearnSetUsb(10, k, c, mo, "VolumeDown");
                changed = true;
            }
        }
        if (gMap[9].usbKind == UsbOutKind::None || !gMap[9].usbLabel[0]) {
            if (keyLearnParseUsbLabel("VolumeUp", k, c, mo)) {
                keyLearnSetUsb(9, k, c, mo, "VolumeUp");
                changed = true;
            }
        }
        if (gMap[10].usbKind == UsbOutKind::None || !gMap[10].usbLabel[0]) {
            if (keyLearnParseUsbLabel("VolumeDown", k, c, mo)) {
                keyLearnSetUsb(10, k, c, mo, "VolumeDown");
                changed = true;
            }
        }
    }
    if (changed) keyLearnSave();
}

void keyLearnInstallDefaults() {
    static const char *kHex[kRemoteKeyCount] = {
        "00 00 52 00 00 00 00 00", "00 00 51 00 00 00 00 00", "00 00 50 00 00 00 00 00",
        "00 00 4F 00 00 00 00 00", "41 00", "23 02", "24 02",
        "82 03 01 00 01 00 01 00 00 00 00 00 00 00 00 00", "F0 00", "E9 00", "EA 00",
        "00 00 4B 00 00 00 00 00", "00 00 4E 00 00 00 00 00", "00 00 65 00 00 00 00 00", "E2 00",
        "EB 00",
        "82 13 01 00 01 00 00 00 00 00 00 00 00 00 00 00", // tv
        "82 14 01 00 01 00 00 00 00 00 00 00 00 00 00 00", // source 信源
        "00 00 66 00 00 00 00 00",                         // power 电源
        "82 1A 01 00 01 00 01 00 00 00 00 00 00 00 00 00", // live 直播
        "49 46 4C 59 16 00 01 00 49 46 00 00 80 15 04 00", // switch 切换
    };
    for (int i = 0; i < kRemoteKeyCount; ++i) {
        uint8_t buf[kMaxReportBytes];
        uint8_t len = 0;
        if (kHex[i] && kHex[i][0] && parseHexList(String(kHex[i]), buf, len))
            keyLearnSetMap(i, buf, len);
        installDefaultUsb(i);
    }
    keyLearnSave();
    slog("keymap defaults installed %d/%d", kRemoteKeyCount, kRemoteKeyCount);
}

void keyLearnBegin() {
    gPrefs.begin("keymap", false);
    if (!keyLearnLoad()) {
        keyLearnInstallDefaults();
    } else {
        // Fill missing BLE HEX for tv..switch from built-in samples
        static const char *kHexFill[5] = {
            "82 13 01 00 01 00 00 00 00 00 00 00 00 00 00 00",
            "82 14 01 00 01 00 00 00 00 00 00 00 00 00 00 00",
            "00 00 66 00 00 00 00 00",
            "82 1A 01 00 01 00 01 00 00 00 00 00 00 00 00 00",
            "49 46 4C 59 16 00 01 00 49 46 00 00 80 15 04 00",
        };
        bool hexFilled = false;
        for (int i = 0; i < 5; ++i) {
            int idx = 16 + i;
            if (gMap[idx].learned) continue;
            uint8_t buf[kMaxReportBytes];
            uint8_t len = 0;
            if (parseHexList(String(kHexFill[i]), buf, len) && keyLearnSetMap(idx, buf, len))
                hexFilled = true;
        }
        for (int i = 0; i < kRemoteKeyCount; ++i) {
            if (gMap[i].usbKind == UsbOutKind::None) installDefaultUsb(i);
        }
        migrateUsbDefaults();
        if (hexFilled) keyLearnSave();
    }
}

void keyLearnOnHidReport(const uint8_t *data, size_t len) {
    if (!data || len == 0) return;
    if (len > kMaxReportBytes) len = kMaxReportBytes;

    HidReportSample &s = gRing[gRingHead];
    s.ms = millis();
    s.len = (uint8_t)len;
    memcpy(s.data, data, len);
    if (len < kMaxReportBytes) memset(s.data + len, 0, kMaxReportBytes - len);
    s.isZero = isZero(data, len);
    gRingHead = (gRingHead + 1) % kReportRing;
    if (gRingCount < kReportRing) gRingCount++;

    if (!gLearnActive) return;

    if (gWaitRelease) {
        // CMCC 部分键不发全0：全0或不同报告算松开；超时由 keyLearnPoll 处理
        if (sameReport(gMap[gStep], data, len)) return; // 仍按住/重复同一报告
        const bool pendingPress = !s.isZero;
        learnAdvanceAfterRelease();
        if (pendingPress && gLearnActive && gArmed) {
            keyLearnSetMap(gStep, data, len);
            slog("learn[%d] %s/%s = %s", gStep, kDefs[gStep].id, kDefs[gStep].zh,
                 toHex(data, len).c_str());
            gArmed = false;
            gWaitRelease = true;
            gWaitReleaseMs = millis();
        }
        return;
    }

    if (gArmed && !s.isZero && gStep >= 0 && gStep < kRemoteKeyCount) {
        keyLearnSetMap(gStep, data, len);
        slog("learn[%d] %s/%s = %s", gStep, kDefs[gStep].id, kDefs[gStep].zh, toHex(data, len).c_str());
        gArmed = false;
        gWaitRelease = true;
        gWaitReleaseMs = millis();
    }
}

void keyLearnPoll() {
    if (!gLearnActive || !gWaitRelease) return;
    if ((millis() - gWaitReleaseMs) < 700) return;
    slog("learn release timeout [%d] %s", gStep, kDefs[gStep].zh);
    learnAdvanceAfterRelease();
}

int keyLearnRecentCount() { return gRingCount; }

bool keyLearnRecentAt(int newestIndex, HidReportSample &out) {
    if (newestIndex < 0 || newestIndex >= gRingCount) return false;
    int idx = gRingHead - 1 - newestIndex;
    while (idx < 0) idx += kReportRing;
    out = gRing[idx % kReportRing];
    return true;
}

void keyLearnStart(int step, bool oneShot) {
    if (step < 0) step = 0;
    if (step >= kRemoteKeyCount) step = kRemoteKeyCount - 1;
    gStep = step;
    gLearnActive = true;
    gArmed = true;
    gWaitRelease = false;
    gLearnOneShot = oneShot;
    gWaitReleaseMs = 0;
    slog("learn start step=%d %s%s", gStep, kDefs[gStep].zh, oneShot ? " (one)" : "");
}

void keyLearnSetStep(int step) {
    if (step < 0 || step >= kRemoteKeyCount) return;
    gStep = step;
    gWaitRelease = false;
    gWaitReleaseMs = 0;
    if (gLearnActive) gArmed = true;
}

void keyLearnSkip() {
    if (!gLearnActive) return;
    if (gWaitRelease) {
        learnAdvanceAfterRelease();
        return;
    }
    // 未录则跳过当前，进入下一键（oneShot 则停止）
    learnAdvanceAfterRelease();
}

int keyLearnStep() { return gStep; }
bool keyLearnActive() { return gLearnActive; }
bool keyLearnArmed() { return gArmed; }
bool keyLearnWaitingRelease() { return gWaitRelease; }
bool keyLearnOneShot() { return gLearnOneShot; }

void keyLearnStop() {
    gLearnActive = false;
    gArmed = false;
    gWaitRelease = false;
    gLearnOneShot = false;
    slog("learn stop at %d", gStep);
}

const KeyMapping *keyLearnGetMap(int index) {
    if (index < 0 || index >= kRemoteKeyCount) return nullptr;
    return &gMap[index];
}

bool keyLearnSetMap(int index, const uint8_t *data, size_t len) {
    if (index < 0 || index >= kRemoteKeyCount || !data || len == 0 || len > kMaxReportBytes) return false;
    gMap[index].learned = true;
    gMap[index].len = (uint8_t)len;
    memcpy(gMap[index].data, data, len);
    if (len < kMaxReportBytes) memset(gMap[index].data + len, 0, kMaxReportBytes - len);
    return true;
}

void keyLearnClearAll() {
    for (int i = 0; i < kRemoteKeyCount; ++i) gMap[i] = KeyMapping{};
    gPrefs.clear();
    if (sdMounted()) SD.remove("/logs/keymap.json");
}

static size_t significantLen(const uint8_t *data, size_t len) {
    while (len > 0 && data[len - 1] == 0) len--;
    return len;
}

int keyLearnMatch(const uint8_t *data, size_t len) {
    if (!data || !len || isZero(data, len)) return -1;

    // Voice/mic first — never let short consumer maps (Settings/Mute) steal it
    if (keyLearnLooksLikeVoice(data, len)) return 7;
    for (int i = 0; i < kRemoteKeyCount; ++i) {
        if (i == 7 && gMap[i].learned && sameReport(gMap[i], data, len)) return 7;
    }

    // Exact / pad-tolerant
    for (int i = 0; i < kRemoteKeyCount; ++i) {
        if (sameReport(gMap[i], data, len)) return i;
    }

    // Longest significant prefix wins (avoid EB 00 / E2 00 false hits on longer frames)
    size_t inSig = significantLen(data, len);
    int best = -1;
    size_t bestSig = 0;
    for (int i = 0; i < kRemoteKeyCount; ++i) {
        if (!gMap[i].learned) continue;
        size_t mapSig = significantLen(gMap[i].data, gMap[i].len);
        if (mapSig == 0 || inSig == 0) continue;
        if (mapSig <= len && memcmp(gMap[i].data, data, mapSig) == 0) {
            if (mapSig > bestSig) {
                bestSig = mapSig;
                best = i;
            }
            continue;
        }
        if (inSig <= gMap[i].len && memcmp(gMap[i].data, data, inSig) == 0 &&
            significantLen(gMap[i].data, inSig) == inSig) {
            bool restZero = true;
            for (size_t k = inSig; k < gMap[i].len; ++k) {
                if (gMap[i].data[k]) {
                    restZero = false;
                    break;
                }
            }
            if (restZero && inSig > bestSig) {
                bestSig = inSig;
                best = i;
            }
        }
    }
    return best;
}

String keyLearnMapJson() {
    String j = "{\"keys\":[";
    for (int i = 0; i < kRemoteKeyCount; ++i) {
        if (i) j += ',';
        j += "{\"i\":";
        j += String(i);
        j += ",\"id\":\"";
        j += kDefs[i].id;
        j += "\",\"zh\":\"";
        j += kDefs[i].zh;
        j += "\",\"learned\":";
        j += gMap[i].learned ? "true" : "false";
        j += ",\"hex\":\"";
        if (gMap[i].learned) j += toHex(gMap[i].data, gMap[i].len);
        j += "\",\"len\":";
        j += String((int)gMap[i].len);
        j += ",\"usb_kind\":\"";
        j += gMap[i].usbKind == UsbOutKind::Consumer     ? "cc"
             : gMap[i].usbKind == UsbOutKind::MouseWheel ? "wh"
             : (gMap[i].usbKind == UsbOutKind::Keyboard  ? "kb" : "");
        j += "\",\"usb_code\":";
        j += String(gMap[i].usbCode);
        j += ",\"usb_mods\":";
        j += String((int)gMap[i].usbMods);
        j += ",\"usb_label\":\"";
        j += gMap[i].usbLabel;
        j += "\"}";
    }
    j += "],\"step\":";
    j += String(gStep);
    j += ",\"active\":";
    j += gLearnActive ? "true" : "false";
    j += ",\"armed\":";
    j += gArmed ? "true" : "false";
    j += ",\"wait_release\":";
    j += gWaitRelease ? "true" : "false";
    j += ",\"one_shot\":";
    j += gLearnOneShot ? "true" : "false";
    j += "}";
    return j;
}

bool keyLearnSave() {
    for (int i = 0; i < kRemoteKeyCount; ++i) {
        String k = String("k") + i;
        if (gMap[i].learned) gPrefs.putString(k.c_str(), toHex(gMap[i].data, gMap[i].len));
        else gPrefs.remove(k.c_str());
        String u = String("u") + i;
        if (gMap[i].usbKind != UsbOutKind::None && gMap[i].usbLabel[0]) {
            gPrefs.putString(u.c_str(), gMap[i].usbLabel);
        } else if (gMap[i].usbKind != UsbOutKind::None) {
            String v;
            if (gMap[i].usbKind == UsbOutKind::Consumer) v = "cc:0x";
            else if (gMap[i].usbKind == UsbOutKind::MouseWheel) v = "wheel:";
            else v = "kb:0x";
            if (gMap[i].usbKind == UsbOutKind::MouseWheel)
                v += String((int)(int16_t)gMap[i].usbCode);
            else
                v += String(gMap[i].usbCode, HEX);
            gPrefs.putString(u.c_str(), v);
        } else {
            gPrefs.remove(u.c_str());
        }
    }
    String json = keyLearnMapJson();
    if (sdMounted()) {
        File f = SD.open("/logs/keymap.json", FILE_WRITE);
        if (f) {
            f.print(json);
            f.close();
        }
    }
    int n = 0;
    for (int i = 0; i < kRemoteKeyCount; ++i)
        if (gMap[i].learned) n++;
    slog("keymap saved %d/%d", n, kRemoteKeyCount);
    return true;
}

bool keyLearnLoad() {
    int n = 0;
    for (int i = 0; i < kRemoteKeyCount; ++i) {
        gMap[i] = KeyMapping{};
        String k = String("k") + i;
        String hex = gPrefs.getString(k.c_str(), "");
        if (hex.length()) {
            uint8_t buf[kMaxReportBytes];
            uint8_t len = 0;
            if (parseHexList(hex, buf, len)) {
                keyLearnSetMap(i, buf, len);
                n++;
            }
        }
        String u = gPrefs.getString((String("u") + i).c_str(), "");
        if (u.length()) {
            UsbOutKind kind;
            uint16_t code;
            uint8_t mods = 0;
            if (keyLearnParseUsbLabel(u.c_str(), kind, code, mods))
                keyLearnSetUsb(i, kind, code, mods, u.c_str());
        }
    }
    slog("keymap loaded %d/%d", n, kRemoteKeyCount);
    return n > 0;
}

static String jsonField(const String &block, const char *key) {
    String pat = String("\"") + key + "\":\"";
    int a = block.indexOf(pat);
    if (a < 0) return "";
    a += pat.length();
    int b = block.indexOf('"', a);
    if (b < 0) return "";
    return block.substring(a, b);
}

bool keyLearnApplyJson(const String &json) {
    bool any = false;
    int pos = 0;
    while (true) {
        int iKey = json.indexOf("\"i\":", pos);
        if (iKey < 0) break;
        int idx = json.substring(iKey + 4).toInt();
        int next = json.indexOf("\"i\":", iKey + 4);
        String block = next < 0 ? json.substring(iKey) : json.substring(iKey, next);
        pos = iKey + 4;
        if (idx < 0 || idx >= kRemoteKeyCount) continue;

        String hex = jsonField(block, "hex");
        if (hex.length()) {
            uint8_t buf[kMaxReportBytes];
            uint8_t len = 0;
            if (parseHexList(hex, buf, len)) {
                keyLearnSetMap(idx, buf, len);
                any = true;
            }
        }
        String label = jsonField(block, "usb_label");
        if (!label.length()) {
            // try kind+code
            String kind = jsonField(block, "usb_kind");
            int cPos = block.indexOf("\"usb_code\":");
            if (kind.length() && cPos >= 0) {
                uint16_t code = (uint16_t)block.substring(cPos + 11).toInt();
                label = kind + ":0x" + String(code, HEX);
            }
        }
        if (label.length()) {
            UsbOutKind k;
            uint16_t code;
            uint8_t mods = 0;
            if (keyLearnParseUsbLabel(label.c_str(), k, code, mods)) {
                keyLearnSetUsb(idx, k, code, mods, label.c_str());
                any = true;
            }
        }
    }
    if (any) keyLearnSave();
    return any;
}