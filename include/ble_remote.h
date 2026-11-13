#pragma once
#include <Arduino.h>
#include "keymap.h"

static constexpr int kMaxBleDevices = 12;
static constexpr const char *kDefaultCmccName = "CMCC_Voice_Remote";
static constexpr const char *kDefaultCmccAddr = "0c:f3:21:b2:99:8b";

struct BleDeviceInfo {
    String name;
    String addr;
    int rssi = 0;
};

using BleStatusCb = void (*)(const char *status, const char *detail);
using BleArrowCb = void (*)(ArrowKey key, bool pressed, const uint8_t *raw, size_t len);
using BleRawCb = void (*)(const uint8_t *raw, size_t len, bool isRelease);

void bleRemoteBegin(BleStatusCb onStatus, BleArrowCb onArrow, BleRawCb onRaw = nullptr);
void bleRemoteLoop();

void bleStartScan(uint32_t durationMs = 8000);
void bleStopScan();
bool bleIsScanning();
bool bleIsBusy();

int bleDeviceCount();
bool bleGetDevice(int index, BleDeviceInfo &out);
void bleClearDevices();

// Async requests (non-blocking for WiFi/UI). Actual work runs in bleRemoteLoop().
void bleRequestConnectIndex(int index);
void bleRequestConnectAddr(const char *addrStr, const char *nameHint = "");
void bleAutoPairCmcc(); // scan + pair default CMCC remote

// Legacy sync wrappers (prefer request* / autoPair)
bool bleConnectIndex(int index);
bool bleConnectAddr(const char *addrStr, const char *nameHint = "");
void bleDisconnect();

bool bleRemoteConnected();
const char *bleRemotePeerName();
const char *bleRemotePeerAddr();

bool bleLoadLastPeer(String &addr, String &name);
void bleSaveLastPeer(const char *addr, const char *name);
void bleClearLastPeer();

// Voice MIC probe helpers
String bleGattDumpJson();
int bleSubscribeAllNotifies(); // subscribe every notify/indicate char (HID + custom)
// Light path: only 0xFC (+ F8/F9/04) Input + FD12 notify — avoids ATT flood disconnect
int bleSubscribeVoiceReports();
String bleHidReportCatalogJson(); // Report Map + each 2A4D id/type
// Open/close mic: write 0x01/0x00 to HID Output report 0xFB; also try FD11
bool bleVoiceMicEnable(bool on, String &detail);