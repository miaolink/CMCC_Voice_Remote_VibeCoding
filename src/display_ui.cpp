#include "display_ui.h"
#include "ble_remote.h"
#include "sd_log.h"
#include "usb_hid_out.h"
#include "wifi_web.h"

#include <Arduino_GFX_Library.h>

static Arduino_DataBus *bus = nullptr;
static Arduino_GFX *gfx = nullptr;

static String line1 = "boot";
static String line2 = "";
static String lastKey = "-";
static String hint = ";/. Enter ` s";
static UiScreen screen = UiScreen::Main;
static int cursor = 0;
static int deviceCount = 0;
static bool dirty = true;

static const char *mainItems[] = {
    "1 Auto pair CMCC",
    "2 Run USB-HID",
    "3 Scan remotes",
    "4 Device list",
    "5 Disconnect",
    "6 WiFi / Web",
};
static constexpr int kMainCount = 6;

void uiBegin() {
    bus = new Arduino_ESP32SPI(34 /*DC*/, 37 /*CS*/, 36 /*SCK*/, 35 /*MOSI*/, GFX_NOT_DEFINED /*MISO*/, FSPI);
    gfx = new Arduino_ST7789(bus, 33 /*RST*/, 1 /*rotation*/, true /*IPS*/, 135, 240, 52, 40, 53, 40);
    pinMode(38, OUTPUT);
    digitalWrite(38, HIGH);
    gfx->begin();
    gfx->setTextSize(1);
    uiBoot("booting...");
}

void uiBoot(const char *msg) {
    if (!gfx) return;
    gfx->fillScreen(BLACK);
    gfx->setCursor(4, 6);
    gfx->setTextColor(CYAN);
    gfx->println("CMCC BLE Bridge");
    gfx->setCursor(4, 24);
    gfx->setTextColor(YELLOW);
    gfx->println(msg ? msg : "");
    gfx->setCursor(4, 48);
    gfx->setTextColor(DARKGREY);
    gfx->println("please wait");
    dirty = true;
}

void uiSetStatus(const char *a, const char *b) {
    line1 = a ? a : "";
    line2 = b ? b : "";
    dirty = true;
}

void uiSetLastKey(ArrowKey key) {
    lastKey = arrowKeyName(key);
    dirty = true;
}

void uiSetLastKeyName(const char *name) {
    lastKey = name && name[0] ? name : "-";
    dirty = true;
}

void uiSetScreen(UiScreen s) {
    screen = s;
    dirty = true;
}

void uiSetMenuCursor(int c) {
    cursor = c;
    dirty = true;
}

void uiSetDeviceCount(int count) {
    deviceCount = count;
    dirty = true;
}

void uiSetHint(const char *h) {
    hint = h ? h : "";
    dirty = true;
}

void uiInvalidate() { dirty = true; }

static void drawHeader(const char *title) {
    gfx->setCursor(4, 2);
    gfx->setTextColor(CYAN);
    gfx->print("CMCC ");
    gfx->setTextColor(usbHidReady() ? GREEN : DARKGREY);
    gfx->print(usbHidReady() ? "HID:ON" : "HID:OFF");
    gfx->setCursor(4, 14);
    gfx->setTextColor(WHITE);
    gfx->println(title);
}

static void drawMain() {
    drawHeader("MENU");
    for (int i = 0; i < kMainCount; ++i) {
        int y = 26 + i * 13;
        gfx->setCursor(4, y);
        if (i == cursor) {
            gfx->setTextColor(BLACK, YELLOW);
            gfx->print(">");
            gfx->print(mainItems[i]);
        } else {
            gfx->setTextColor(GREEN);
            gfx->print(" ");
            gfx->print(mainItems[i]);
        }
    }
    gfx->setCursor(4, 104);
    gfx->setTextColor(YELLOW);
    gfx->print("Key:");
    gfx->print(lastKey);
    gfx->setCursor(4, 116);
    gfx->setTextColor(DARKGREY);
    gfx->print(line1);
    if (line2.length()) {
        gfx->print(" ");
        gfx->print(line2);
    }
}

static void drawScanning() {
    drawHeader("SCANNING");
    gfx->setCursor(4, 36);
    gfx->setTextColor(YELLOW);
    gfx->println(bleIsScanning() ? "Searching BLE..." : "Done");
    gfx->setCursor(4, 54);
    gfx->setTextColor(GREEN);
    gfx->printf("Found: %d", deviceCount);
    gfx->setCursor(4, 72);
    gfx->setTextColor(WHITE);
    gfx->println(line1);
    gfx->setCursor(4, 86);
    gfx->println(line2);
    gfx->setCursor(4, 104);
    gfx->setTextColor(DARKGREY);
    gfx->println("Enter=list  `=menu");
}

static void drawDevices() {
    drawHeader("DEVICES");
    if (deviceCount <= 0) {
        gfx->setCursor(4, 40);
        gfx->setTextColor(YELLOW);
        gfx->println("No devices");
        gfx->setCursor(4, 56);
        gfx->setTextColor(DARKGREY);
        gfx->println("s or menu->Scan");
        return;
    }

    int window = 5;
    int start = 0;
    if (cursor >= window) start = cursor - window + 1;
    if (start < 0) start = 0;

    for (int i = 0; i < window; ++i) {
        int idx = start + i;
        if (idx >= deviceCount) break;
        BleDeviceInfo d;
        if (!bleGetDevice(idx, d)) continue;

        int y = 30 + i * 16;
        gfx->setCursor(4, y);
        if (idx == cursor) {
            gfx->setTextColor(BLACK, YELLOW);
        } else {
            gfx->setTextColor(GREEN);
        }

        String label = d.name;
        if (label.length() > 16) label = label.substring(0, 16);
        gfx->printf("%c%d %s", idx == cursor ? '>' : ' ', idx + 1, label.c_str());

        gfx->setCursor(170, y);
        gfx->setTextColor(idx == cursor ? BLACK : DARKGREY, idx == cursor ? YELLOW : BLACK);
        gfx->printf("%d", d.rssi);
    }

    gfx->setCursor(4, 112);
    gfx->setTextColor(DARKGREY);
    gfx->print("Enter=pair  `=back");
}

static void drawStatus() {
    drawHeader("WIFI/WEB");
    gfx->setCursor(4, 30);
    gfx->setTextColor(GREEN);
    gfx->printf("%s HID:%s", wifiModeText(), usbHidReady() ? "ON" : "OFF");
    gfx->setCursor(4, 44);
    gfx->setTextColor(CYAN);
    gfx->print("AP ");
    gfx->println(wifiApIp());
    gfx->setCursor(4, 58);
    gfx->setTextColor(WHITE);
    gfx->print("STA ");
    gfx->println(wifiStaIp());
    gfx->setCursor(4, 72);
    gfx->setTextColor(YELLOW);
    gfx->printf("BLE:%s %s", bleRemoteConnected() ? "ON" : "OFF", bleRemotePeerName());
    gfx->setCursor(4, 86);
    gfx->setTextColor(DARKGREY);
    gfx->printf("SD:%s", sdMounted() ? "OK /logs" : "FAIL");
    gfx->setCursor(4, 100);
    gfx->println(wifiStaConnected() ? "STA ok" : (wifiApUp() ? "AP on" : "wifi..."));
    gfx->setCursor(4, 114);
    gfx->print("Key:");
    gfx->print(lastKey);
}

void uiLoop() {
    if (!dirty || !gfx) return;
    dirty = false;
    gfx->fillScreen(BLACK);

    switch (screen) {
        case UiScreen::Main:
            drawMain();
            break;
        case UiScreen::Scanning:
            drawScanning();
            break;
        case UiScreen::Devices:
            drawDevices();
            break;
        case UiScreen::Status:
            drawStatus();
            break;
    }
}