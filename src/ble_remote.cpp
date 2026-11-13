#include "ble_remote.h"
#include "key_learn.h"
#include "sd_log.h"
#include "voice_audio.h"
#include "voice_probe.h"

#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>

static constexpr uint16_t kHidService = 0x1812;
static constexpr uint16_t kHidReport = 0x2A4D;
static constexpr uint16_t kHidBootKbInput = 0x2A22;
static constexpr uint16_t kHidReportMap = 0x2A4B;
static constexpr uint16_t kHidReportRef = 0x2908;

static BleStatusCb gStatusCb = nullptr;
static BleArrowCb gArrowCb = nullptr;
static BleRawCb gRawCb = nullptr;
static NimBLEClient *gClient = nullptr;
static NimBLEAddress gPeerAddr;
static uint8_t gPeerType = BLE_ADDR_PUBLIC;
static String gPeerName;
static String gPeerAddrStr;
static bool gConnected = false;
static bool gAutoReconnect = false;
static bool gScanning = false;
static bool gBusy = false;
static uint32_t gLastReconnectMs = 0;
static ArrowKey gHeld = ArrowKey::None;

static BleDeviceInfo gDevices[kMaxBleDevices];
static int gDeviceCount = 0;
static Preferences gPrefs;

enum class PendingAction : uint8_t { None = 0, Connect, Autopair };
static PendingAction gPending = PendingAction::None;
static String gPendAddr;
static String gPendName;
static uint8_t gPendType = BLE_ADDR_PUBLIC;
static bool gAutopairActive = false;
static uint32_t gAutopairDeadline = 0;

static void setStatus(const char *a, const char *b = "") {
    Serial.printf("[BLE] %s %s\n", a, b);
    dlog("ble %s %s", a ? a : "", b ? b : "");
    if (gStatusCb) gStatusCb(a, b);
}

static void dumpHex(uint8_t *data, size_t len) {
    char hex[96];
    size_t pos = 0;
    hex[0] = 0;
    for (size_t i = 0; i < len && pos + 3 < sizeof(hex); ++i) {
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X ", data[i]);
    }
    Serial.printf("[BLE] report: %s\n", hex);
    dlog("hid report len=%u %s", (unsigned)len, hex);
}

static int findDevice(const String &addr) {
    for (int i = 0; i < gDeviceCount; ++i) {
        if (gDevices[i].addr.equalsIgnoreCase(addr)) return i;
    }
    return -1;
}

static bool isCmccTarget(const String &name, const String &addr) {
    if (addr.equalsIgnoreCase(kDefaultCmccAddr)) return true;
    if (name.indexOf(kDefaultCmccName) >= 0) return true;
    return false;
}

static void addOrUpdateDevice(NimBLEAdvertisedDevice *adv) {
    String addr = adv->getAddress().toString().c_str();
    String name = adv->haveName() ? String(adv->getName().c_str()) : String("(no name)");
    int rssi = adv->getRSSI();

    int idx = findDevice(addr);
    if (idx >= 0) {
        if (adv->haveName()) gDevices[idx].name = name;
        gDevices[idx].rssi = rssi;
        return;
    }
    if (gDeviceCount >= kMaxBleDevices) return;

    gDevices[gDeviceCount].name = name;
    gDevices[gDeviceCount].addr = addr;
    gDevices[gDeviceCount].rssi = rssi;
    gDeviceCount++;
    Serial.printf("[BLE] found[%d] %s rssi=%d %s\n", gDeviceCount - 1, name.c_str(), rssi, addr.c_str());
}

// PTT: open mic only after voice-key notify; defer ATT write out of notifyCB
static volatile bool gMicWantOpen = false;
static volatile bool gMicWantClose = false;
static bool gMicIsOpen = false;
static bool gVoiceKeyHeld = false;
static uint32_t gMicOpenAfterMs = 0; // delay open so voice-key ATT settles
static NimBLERemoteCharacteristic *gFbOutChr = nullptr;

static bool isVoiceKeyIndex(int idx) {
    if (idx < 0) return false;
    const RemoteKeyDef *defs = remoteKeyDefs();
    return defs && defs[idx].id && strcmp(defs[idx].id, "voice") == 0;
}

static void notifyCB(NimBLERemoteCharacteristic *pChar, uint8_t *pData, size_t length, bool /*isNotify*/) {
    const char *uuid = pChar ? pChar->getUUID().toString().c_str() : "";
    // Skip Serial hex during probe/audio stream (CDC bandwidth for "A <HEX>")
    if (!voiceProbeActive() && !voiceAudioStreaming()) {
        dumpHex(pData, length);
        if (uuid && uuid[0]) dlog("from char %s", uuid);
    }
    voiceProbeOnNotify(uuid, pData, length);
    voiceAudioOnNotify(pData, length);
    keyLearnOnHidReport(pData, length);

    bool isRelease = false;
    ArrowKey key = parseArrowFromHidReport(pData, length, &isRelease);

    // Host-side PTT: voice key down → open 0xFB; release/zero → close
    if (isRelease) {
        if (gVoiceKeyHeld || gMicIsOpen) {
            gVoiceKeyHeld = false;
            gMicWantClose = true;
            voiceAudioMarkPtt(false);
        }
    } else {
        int idx = keyLearnMatch(pData, length);
        if (isVoiceKeyIndex(idx) || keyLearnLooksLikeVoice(pData, length)) {
            if (!gVoiceKeyHeld) voiceAudioMarkPtt(true);
            gVoiceKeyHeld = true;
            // Defer open ~80ms: immediate catalog/FB write was disconnecting the link
            if (!gMicIsOpen && !gMicWantOpen) {
                gMicOpenAfterMs = millis() + 80;
                gMicWantOpen = true;
            }
        }
    }

    if (gRawCb) gRawCb(pData, length, isRelease);

    if (isRelease) {
        if (gHeld != ArrowKey::None && gArrowCb) gArrowCb(gHeld, false, pData, length);
        gHeld = ArrowKey::None;
        return;
    }
    if (key == ArrowKey::None) {
        // May still be a learned non-arrow key (handled via gRawCb / keyLearnMatch)
        return;
    }

    if (gHeld != ArrowKey::None && gHeld != key && gArrowCb) {
        gArrowCb(gHeld, false, pData, length);
    }
    gHeld = key;
    if (gArrowCb) gArrowCb(key, true, pData, length);
}

class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient *pClient) override {
        gConnected = true;
        setStatus("connected", gPeerName.c_str());
        // Community voice bridge: itvl≈32 (40ms) keeps 0xFC coverage; tighter intervals drop audio
        pClient->updateConnParams(24, 40, 0, 400);
    }

    void onDisconnect(NimBLEClient * /*pClient*/) override {
        gConnected = false;
        gHeld = ArrowKey::None;
        gMicWantOpen = false;
        gMicWantClose = false;
        gMicIsOpen = false;
        gVoiceKeyHeld = false;
        gMicOpenAfterMs = 0;
        gFbOutChr = nullptr;
        setStatus("disconnected", gAutoReconnect ? "will retry" : "");
        gLastReconnectMs = millis();
    }

    void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
        if (!desc) return;
        dlog("auth complete enc=%d bonded=%d", (int)desc->sec_state.encrypted, (int)desc->sec_state.bonded);
        setStatus(desc->sec_state.encrypted ? "secured" : "auth weak", gPeerName.c_str());
    }
};

static ClientCallbacks gClientCbs;

class AdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice *advertisedDevice) override {
        bool named = advertisedDevice->haveName();
        bool hid = advertisedDevice->isAdvertisingService(NimBLEUUID(kHidService));
        if (!named && !hid) return;
        addOrUpdateDevice(advertisedDevice);

        if (!gAutopairActive) return;
        String addr = advertisedDevice->getAddress().toString().c_str();
        String name = advertisedDevice->haveName() ? String(advertisedDevice->getName().c_str()) : String("");
        if (!isCmccTarget(name, addr)) return;

        gPendAddr = addr;
        gPendName = name.length() ? name : String(kDefaultCmccName);
        gPendType = advertisedDevice->getAddressType();
        gPending = PendingAction::Connect;
        gAutopairActive = false;
        setStatus("CMCC found", gPendName.c_str());
        NimBLEDevice::getScan()->stop();
        gScanning = false;
    }
};

static int catalogHidReports(); // defined below — warmed after HID subscribe

static bool subscribeHidReports(NimBLEClient *client) {
    NimBLERemoteService *hid = client->getService(NimBLEUUID(kHidService));
    if (!hid) {
        setStatus("no HID svc", "0x1812");
        return false;
    }

    int subscribed = 0;
    std::vector<NimBLERemoteCharacteristic *> *chars = hid->getCharacteristics(true);
    if (!chars) {
        setStatus("no chars", "HID");
        return false;
    }

    for (auto *c : *chars) {
        if (!c) continue;
        if (c->getUUID().equals(NimBLEUUID(kHidReportMap))) continue;

        bool isReport = c->getUUID().equals(NimBLEUUID(kHidReport)) ||
                        c->getUUID().equals(NimBLEUUID(kHidBootKbInput));
        if (!(isReport || c->canNotify() || c->canIndicate())) continue;

        bool prefer = isReport;
        NimBLERemoteDescriptor *ref = c->getDescriptor(NimBLEUUID(kHidReportRef));
        if (ref) {
            std::string v = ref->readValue();
            if (v.size() >= 2) {
                uint8_t rtype = (uint8_t)v[1];
                if (rtype == 1) prefer = true;
                if (rtype == 2 || rtype == 3) continue;
            }
        }
        if (!prefer && !(c->canNotify() || c->canIndicate())) continue;

        if (c->canNotify()) {
            if (c->subscribe(true, notifyCB)) {
                dlog("notify %s", c->getUUID().toString().c_str());
                subscribed++;
            }
        } else if (c->canIndicate()) {
            if (c->subscribe(false, notifyCB)) {
                dlog("indicate %s", c->getUUID().toString().c_str());
                subscribed++;
            }
        }
    }

    setStatus(subscribed ? "HID ready" : "no reports", String(subscribed).c_str());
    // Do NOT catalogHidReports() here — reading Report Map/refs right after subscribe
    // was killing Input notifies (no key prompts until reconnect mess).
    return subscribed > 0;
}

static bool doConnect(const NimBLEAddress &addr, const String &name, uint8_t addrType) {
    if (gScanning || NimBLEDevice::getScan()->isScanning()) {
        bleStopScan();
        delay(50);
    }

    WiFi.setSleep(true);

    gPeerAddr = addr;
    gPeerType = addrType;
    gPeerName = name.length() ? name : String(addr.toString().c_str());
    gPeerAddrStr = addr.toString().c_str();
    gAutoReconnect = true;
    setStatus("connecting", gPeerName.c_str());

    if (gClient) {
        if (gClient->isConnected()) {
            gClient->disconnect();
            delay(150);
        }
    } else {
        gClient = NimBLEDevice::createClient();
        gClient->setClientCallbacks(&gClientCbs, false);
        gClient->setConnectTimeout(12);
    }

    NimBLEAddress useAddr(gPeerAddrStr.c_str(), addrType);
    if (!gClient->connect(useAddr, false)) {
        uint8_t alt = (addrType == BLE_ADDR_PUBLIC) ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
        NimBLEAddress altAddr(gPeerAddrStr.c_str(), alt);
        dlog("connect retry type=%u", alt);
        if (!gClient->connect(altAddr, false)) {
            setStatus("connect fail", gPeerAddrStr.c_str());
            return false;
        }
        gPeerType = alt;
        gPeerAddr = altAddr;
    }

    delay(100);
    bool secured = gClient->secureConnection();
    dlog("secureConnection=%d", (int)secured);
    if (!secured) setStatus("secure fail", "continue");

    gClient->getServices(true);
    if (!subscribeHidReports(gClient)) {
        gClient->disconnect();
        return false;
    }

    bleSaveLastPeer(gPeerAddrStr.c_str(), gPeerName.c_str());
    gPrefs.putUChar("atype", gPeerType);
    gPrefs.putBool("autorec", true); // remember to reconnect after reboot
    slog("ble bonded peer saved %s", gPeerAddrStr.c_str());
    return true;
}

static void queueConnect(const String &addr, const String &name, uint8_t type) {
    gPendAddr = addr;
    gPendName = name;
    gPendType = type;
    gPending = PendingAction::Connect;
    setStatus("queue connect", name.c_str());
}

void bleRemoteBegin(BleStatusCb onStatus, BleArrowCb onArrow, BleRawCb onRaw) {
    gStatusCb = onStatus;
    gArrowCb = onArrow;
    gRawCb = onRaw;
    gPrefs.begin("cmccble", false);

    // Seed default CMCC remote if empty
    if (gPrefs.getString("addr", "").length() == 0) {
        bleSaveLastPeer(kDefaultCmccAddr, kDefaultCmccName);
        gPrefs.putBool("autorec", true);
    }

    NimBLEDevice::init("ADV-CMCC-Bridge");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    // bond=true so pairing keys persist across reboot (NimBLE NVS)
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(new AdvertisedDeviceCallbacks(), false);
    scan->setActiveScan(true);
    scan->setInterval(80);
    scan->setWindow(60);

    // Resume last paired remote after reboot (unless user manually disconnected)
    String addr, name;
    bleLoadLastPeer(addr, name);
    uint8_t atype = gPrefs.getUChar("atype", BLE_ADDR_PUBLIC);
    bool want = gPrefs.getBool("autorec", true);
    gPeerAddrStr = addr;
    gPeerName = name;
    gPeerType = atype;
    if (want && addr.length()) {
        gAutoReconnect = true;
        gLastReconnectMs = millis() - 5000; // first loop reconnect ASAP
        queueConnect(addr, name, atype);
        setStatus("resume peer", name.c_str());
        slog("ble resume %s %s type=%u", name.c_str(), addr.c_str(), (unsigned)atype);
    } else {
        setStatus("ready", "no auto-resume");
    }
}

void bleRemoteLoop() {
    NimBLEScan *scan = NimBLEDevice::getScan();
    if (gScanning && !scan->isScanning()) {
        gScanning = false;
        char buf[24];
        snprintf(buf, sizeof(buf), "%d found", gDeviceCount);
        setStatus("scan done", buf);

        if (gAutopairActive) {
            gAutopairActive = false;
            // Prefer scanned CMCC; else fixed MAC
            int hit = findDevice(String(kDefaultCmccAddr));
            if (hit < 0) {
                for (int i = 0; i < gDeviceCount; ++i) {
                    if (gDevices[i].name.indexOf(kDefaultCmccName) >= 0) {
                        hit = i;
                        break;
                    }
                }
            }
            if (hit >= 0) {
                gPendAddr = gDevices[hit].addr;
                gPendName = gDevices[hit].name;
            } else {
                gPendAddr = kDefaultCmccAddr;
                gPendName = kDefaultCmccName;
            }
            gPendType = gPrefs.getUChar("atype", BLE_ADDR_PUBLIC);
            gPending = PendingAction::Connect;
            setStatus("autopair->conn", gPendAddr.c_str());
        }
    }

    if (gAutopairActive && millis() > gAutopairDeadline) {
        bleStopScan();
    }

    if (gPending == PendingAction::Connect && !gBusy && !gConnected) {
        gPending = PendingAction::None;
        gBusy = true;
        // Look up type from scan cache if possible
        NimBLEScanResults results = NimBLEDevice::getScan()->getResults();
        uint8_t type = gPendType;
        for (int i = 0; i < results.getCount(); ++i) {
            NimBLEAdvertisedDevice dadv = results.getDevice(i);
            String a = dadv.getAddress().toString().c_str();
            if (a.equalsIgnoreCase(gPendAddr)) {
                type = dadv.getAddressType();
                doConnect(dadv.getAddress(), gPendName, type);
                gBusy = false;
                return;
            }
        }
        doConnect(NimBLEAddress(gPendAddr.c_str(), type), gPendName, type);
        gBusy = false;
        return;
    }

    if (gAutoReconnect && !gConnected && !gScanning && !gBusy && gPending == PendingAction::None &&
        gPeerAddrStr.length()) {
        if (millis() - gLastReconnectMs > 4000) {
            gLastReconnectMs = millis();
            queueConnect(gPeerAddrStr, gPeerName, gPeerType);
        }
    }

    // Deferred PTT mic open/close (never write ATT inside notifyCB)
    if (gConnected && gMicWantOpen && !gMicIsOpen && !gBusy) {
        if (gMicOpenAfterMs && (int32_t)(millis() - gMicOpenAfterMs) < 0) {
            // wait
        } else if (!gVoiceKeyHeld) {
            // released before deferred open
            gMicWantOpen = false;
            gMicOpenAfterMs = 0;
        } else {
            gMicWantOpen = false;
            gMicOpenAfterMs = 0;
            String d;
            if (bleVoiceMicEnable(true, d)) {
                gMicIsOpen = true;
                setStatus("mic PTT on", d.c_str());
            } else {
                slog("mic PTT open fail %s", d.c_str());
            }
        }
    }
    if (gMicWantClose && gMicIsOpen && !gBusy) {
        gMicWantClose = false;
        String d;
        bleVoiceMicEnable(false, d);
        gMicIsOpen = false;
        setStatus("mic PTT off", d.c_str());
    }
    if (!gConnected) {
        gMicWantOpen = false;
        gMicWantClose = false;
        gMicIsOpen = false;
        gVoiceKeyHeld = false;
        gMicOpenAfterMs = 0;
        gFbOutChr = nullptr;
    }
}

void bleStartScan(uint32_t durationMs) {
    if (gConnected) {
        setStatus("busy", "disconnect first");
        return;
    }
    if (!gAutopairActive) gAutoReconnect = false;
    bleClearDevices();
    NimBLEScan *scan = NimBLEDevice::getScan();
    if (scan->isScanning()) scan->stop();
    setStatus("scanning", "...");
    gScanning = true;
    uint32_t sec = (durationMs + 999) / 1000;
    if (sec < 1) sec = 1;
    if (sec > 30) sec = 30;
    scan->start(sec, nullptr, false);
}

void bleStopScan() {
    NimBLEScan *scan = NimBLEDevice::getScan();
    if (scan->isScanning()) scan->stop();
    gScanning = false;
    char buf[24];
    snprintf(buf, sizeof(buf), "%d found", gDeviceCount);
    setStatus("scan stop", buf);
}

bool bleIsScanning() { return gScanning; }
bool bleIsBusy() { return gBusy || gPending != PendingAction::None || gAutopairActive; }

int bleDeviceCount() { return gDeviceCount; }

bool bleGetDevice(int index, BleDeviceInfo &out) {
    if (index < 0 || index >= gDeviceCount) return false;
    out = gDevices[index];
    return true;
}

void bleClearDevices() { gDeviceCount = 0; }

void bleRequestConnectIndex(int index) {
    BleDeviceInfo d;
    if (!bleGetDevice(index, d)) {
        setStatus("bad index", String(index).c_str());
        return;
    }
    gAutoReconnect = false;
    queueConnect(d.addr, d.name, gPrefs.getUChar("atype", BLE_ADDR_PUBLIC));
}

void bleRequestConnectAddr(const char *addrStr, const char *nameHint) {
    if (!addrStr || !addrStr[0]) return;
    gAutoReconnect = false;
    uint8_t t = gPrefs.getUChar("atype", BLE_ADDR_PUBLIC);
    queueConnect(String(addrStr), nameHint ? String(nameHint) : String(addrStr), t);
}

void bleAutoPairCmcc() {
    if (gConnected && String(bleRemotePeerAddr()).equalsIgnoreCase(kDefaultCmccAddr)) {
        setStatus("already paired", kDefaultCmccName);
        return;
    }
    if (gConnected) bleDisconnect();

    bleSaveLastPeer(kDefaultCmccAddr, kDefaultCmccName);
    gPendAddr = kDefaultCmccAddr;
    gPendName = kDefaultCmccName;
    gPendType = gPrefs.getUChar("atype", BLE_ADDR_PUBLIC);
    gAutopairActive = true;
    gAutopairDeadline = millis() + 12000;
    gAutoReconnect = false;
    setStatus("autopair", kDefaultCmccName);
    slog("autopair start %s %s", kDefaultCmccName, kDefaultCmccAddr);
    bleStartScan(10000);
}

bool bleConnectIndex(int index) {
    bleRequestConnectIndex(index);
    return true;
}

bool bleConnectAddr(const char *addrStr, const char *nameHint) {
    bleRequestConnectAddr(addrStr, nameHint);
    return true;
}

void bleDisconnect() {
    gAutoReconnect = false;
    gAutopairActive = false;
    gPending = PendingAction::None;
    gPrefs.putBool("autorec", false); // manual disconnect: do not resume on reboot
    if (gClient && (gConnected || gClient->isConnected())) {
        gClient->disconnect();
    }
    gConnected = false;
    gHeld = ArrowKey::None;
    setStatus("disconnected", "manual");
}

bool bleRemoteConnected() { return gConnected; }
const char *bleRemotePeerName() { return gPeerName.c_str(); }
const char *bleRemotePeerAddr() { return gPeerAddrStr.c_str(); }

bool bleLoadLastPeer(String &addr, String &name) {
    addr = gPrefs.getString("addr", kDefaultCmccAddr);
    name = gPrefs.getString("name", kDefaultCmccName);
    return addr.length() > 0;
}

void bleSaveLastPeer(const char *addr, const char *name) {
    gPrefs.putString("addr", addr ? addr : "");
    gPrefs.putString("name", name ? name : "");
}

void bleClearLastPeer() {
    gPrefs.remove("addr");
    gPrefs.remove("name");
    gPrefs.remove("atype");
    gPrefs.putBool("autorec", false);
}

String bleGattDumpJson() {
    String j = "{\"connected\":";
    j += gConnected ? "true" : "false";
    j += ",\"peer\":\"";
    j += gPeerName;
    j += "\",\"addr\":\"";
    j += gPeerAddrStr;
    j += "\",\"services\":[";
    if (!gClient || !gConnected) {
        j += "]}";
        return j;
    }
    std::vector<NimBLERemoteService *> *svcs = gClient->getServices(true);
    if (!svcs) {
        j += "]}";
        return j;
    }
    bool firstSvc = true;
    for (auto *svc : *svcs) {
        if (!svc) continue;
        if (!firstSvc) j += ',';
        firstSvc = false;
        j += "{\"uuid\":\"";
        j += svc->getUUID().toString().c_str();
        j += "\",\"chars\":[";
        std::vector<NimBLERemoteCharacteristic *> *chars = svc->getCharacteristics(true);
        bool firstCh = true;
        if (chars) {
            for (auto *c : *chars) {
                if (!c) continue;
                if (!firstCh) j += ',';
                firstCh = false;
                j += "{\"uuid\":\"";
                j += c->getUUID().toString().c_str();
                j += "\",\"notify\":";
                j += c->canNotify() ? "true" : "false";
                j += ",\"indicate\":";
                j += c->canIndicate() ? "true" : "false";
                j += ",\"read\":";
                j += c->canRead() ? "true" : "false";
                j += ",\"write\":";
                j += (c->canWrite() || c->canWriteNoResponse()) ? "true" : "false";
                j += "}";
            }
        }
        j += "]}";
    }
    j += "]}";
    return j;
}

int bleSubscribeAllNotifies() {
    if (!gClient || !gConnected) return 0;
    int n = 0;
    std::vector<NimBLERemoteService *> *svcs = gClient->getServices(true);
    if (!svcs) return 0;
    for (auto *svc : *svcs) {
        if (!svc) continue;
        std::vector<NimBLERemoteCharacteristic *> *chars = svc->getCharacteristics(true);
        if (!chars) continue;
        for (auto *c : *chars) {
            if (!c) continue;
            if (c->canNotify()) {
                if (c->subscribe(true, notifyCB)) {
                    n++;
                    delay(40); // avoid ATT flood / disconnect
                }
            } else if (c->canIndicate()) {
                if (c->subscribe(false, notifyCB)) {
                    n++;
                    delay(40);
                }
            }
        }
    }
    slog("subscribe-all notifies=%d", n);
    setStatus("sub all", String(n).c_str());
    return n;
}

struct HidReportSlot {
    uint8_t id = 0;
    uint8_t type = 0; // 1=Input 2=Output 3=Feature
    uint16_t handle = 0;
    bool notify = false;
    bool write = false;
    NimBLERemoteCharacteristic *chr = nullptr;
};

static constexpr int kMaxHidSlots = 24;
static HidReportSlot gHidSlots[kMaxHidSlots];
static int gHidSlotCount = 0;
static uint8_t gReportMap[512];
static int gReportMapLen = 0;
static bool gHasFc = false;
static bool gHasFb = false;

static int catalogHidReports() {
    gHidSlotCount = 0;
    gReportMapLen = 0;
    gHasFc = false;
    gHasFb = false;
    if (!gClient || !gConnected) return 0;
    // Only called on demand (mic open / Report Map page) — never right after subscribe

    NimBLERemoteService *hid = gClient->getService(NimBLEUUID(kHidService));
    if (!hid) return 0;

    NimBLERemoteCharacteristic *mapCh = hid->getCharacteristic(NimBLEUUID(kHidReportMap));
    if (mapCh && mapCh->canRead()) {
        std::string v = mapCh->readValue();
        gReportMapLen = (int)v.size();
        if (gReportMapLen > (int)sizeof(gReportMap)) gReportMapLen = sizeof(gReportMap);
        if (gReportMapLen > 0) memcpy(gReportMap, v.data(), gReportMapLen);
        // scan for report id bytes after 0x85
        for (int i = 0; i + 1 < gReportMapLen; ++i) {
            if (gReportMap[i] == 0x85) {
                if (gReportMap[i + 1] == 0xFC) gHasFc = true;
                if (gReportMap[i + 1] == 0xFB) gHasFb = true;
            }
        }
    }

    std::vector<NimBLERemoteCharacteristic *> *chars = hid->getCharacteristics(true);
    if (!chars) return 0;
    for (auto *c : *chars) {
        if (!c || !c->getUUID().equals(NimBLEUUID(kHidReport))) continue;
        if (gHidSlotCount >= kMaxHidSlots) break;
        HidReportSlot &s = gHidSlots[gHidSlotCount];
        s = HidReportSlot{};
        s.chr = c;
        s.handle = c->getHandle();
        s.notify = c->canNotify();
        s.write = c->canWrite() || c->canWriteNoResponse();
        NimBLERemoteDescriptor *ref = c->getDescriptor(NimBLEUUID(kHidReportRef));
        if (ref) {
            std::string v = ref->readValue();
            if (v.size() >= 2) {
                s.id = (uint8_t)v[0];
                s.type = (uint8_t)v[1];
            }
        }
        if (s.id == 0xFC) gHasFc = true;
        if (s.id == 0xFB) gHasFb = true;
        gHidSlotCount++;
    }
    slog("hid catalog slots=%d map=%d FC=%d FB=%d", gHidSlotCount, gReportMapLen, (int)gHasFc,
         (int)gHasFb);
    return gHidSlotCount;
}

int bleSubscribeVoiceReports() {
    if (!gClient || !gConnected) return 0;
    catalogHidReports();
    int n = 0;
    // Prefer audio-sized Input reports: FC (known), also F8/F9/04 from this remote's map
    for (int i = 0; i < gHidSlotCount; ++i) {
        HidReportSlot &s = gHidSlots[i];
        if (!s.chr || !s.notify) continue;
        if (s.type != 1 && s.type != 0) continue;
        if (s.id != 0xFC && s.id != 0xF8 && s.id != 0xF9 && s.id != 0x04) continue;
        if (s.chr->subscribe(true, notifyCB)) {
            n++;
            delay(30);
            slog("voice sub id=0x%02X h=0x%04X", s.id, (unsigned)s.handle);
        }
    }
    NimBLERemoteService *svc = gClient->getService(NimBLEUUID((uint16_t)0xFD10));
    if (svc) {
        NimBLERemoteCharacteristic *n12 = svc->getCharacteristic(NimBLEUUID((uint16_t)0xFD12));
        if (n12 && n12->canNotify()) {
            if (n12->subscribe(true, notifyCB)) {
                n++;
                delay(30);
                slog("voice sub FD12");
            }
        }
    }
    setStatus("voice sub", String(n).c_str());
    return n;
}

String bleHidReportCatalogJson() {
    catalogHidReports();
    String j = "{\"connected\":";
    j += gConnected ? "true" : "false";
    j += ",\"map_len\":";
    j += String(gReportMapLen);
    j += ",\"has_fc\":";
    j += gHasFc ? "true" : "false";
    j += ",\"has_fb\":";
    j += gHasFb ? "true" : "false";
    j += ",\"map_hex\":\"";
    for (int i = 0; i < gReportMapLen && i < 256; ++i) {
        char b[4];
        snprintf(b, sizeof(b), "%02X", gReportMap[i]);
        j += b;
        if (i + 1 < gReportMapLen && i + 1 < 256) j += ' ';
    }
    if (gReportMapLen > 256) j += "...";
    j += "\",\"reports\":[";
    for (int i = 0; i < gHidSlotCount; ++i) {
        if (i) j += ',';
        HidReportSlot &s = gHidSlots[i];
        j += "{\"id\":";
        j += String(s.id);
        j += ",\"id_hex\":\"";
        char hx[8];
        snprintf(hx, sizeof(hx), "%02X", s.id);
        j += hx;
        j += "\",\"type\":";
        j += String(s.type);
        j += ",\"type_name\":\"";
        j += s.type == 1 ? "Input" : (s.type == 2 ? "Output" : (s.type == 3 ? "Feature" : "?"));
        j += "\",\"handle\":";
        j += String(s.handle);
        j += ",\"notify\":";
        j += s.notify ? "true" : "false";
        j += ",\"write\":";
        j += s.write ? "true" : "false";
        j += "}";
    }
    j += "]}";
    return j;
}

static bool findFbOutputChar() {
    if (gFbOutChr) return true;
    if (!gClient || !gConnected) return false;
    NimBLERemoteService *hid = gClient->getService(NimBLEUUID(kHidService));
    if (!hid) return false;
    std::vector<NimBLERemoteCharacteristic *> *chars = hid->getCharacteristics(false);
    if (!chars) return false;
    for (auto *c : *chars) {
        if (!c || !c->getUUID().equals(NimBLEUUID(kHidReport))) continue;
        if (!(c->canWrite() || c->canWriteNoResponse())) continue;
        NimBLERemoteDescriptor *ref = c->getDescriptor(NimBLEUUID(kHidReportRef));
        if (!ref) continue;
        std::string v = ref->readValue();
        if (v.size() < 2) continue;
        uint8_t id = (uint8_t)v[0];
        uint8_t typ = (uint8_t)v[1];
        if (id == 0xFB && (typ == 2 || typ == 3 || typ == 0)) {
            gFbOutChr = c;
            return true;
        }
    }
    return false;
}

bool bleVoiceMicEnable(bool on, String &detail) {
    detail = "";
    if (!gClient || !gConnected) {
        detail = "not connected";
        return false;
    }
    // Light path only: find Output 0xFB and write 1 byte. No Report Map, no FD11
    // (full catalog + FD11 right after voice key was disconnecting the remote).
    if (!findFbOutputChar() || !gFbOutChr) {
        detail = "no FB output char";
        return false;
    }

    uint8_t b = on ? 0x01 : 0x00;
    // Prefer WoR to keep notify path alive; fall back to with-response
    bool withResp = !gFbOutChr->canWriteNoResponse() && gFbOutChr->canWrite();
    uint32_t t0 = millis();
    gFbOutChr->writeValue(&b, 1, withResp);
    uint32_t dt = millis() - t0;
    char buf[96];
    snprintf(buf, sizeof(buf), "FB write %02X h=0x%04X resp=%u %lums", b,
             (unsigned)gFbOutChr->getHandle(), (unsigned)withResp, (unsigned long)dt);
    detail = buf;
    slog("voice mic %s", buf);
    setStatus(on ? "mic open" : "mic close", detail.c_str());
    return true;
}