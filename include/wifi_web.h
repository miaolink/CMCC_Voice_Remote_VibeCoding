#pragma once
#include <Arduino.h>

void wifiWebBegin();
void wifiWebLoop();

bool wifiStaConnected();
bool wifiApUp();
String wifiStaIp();
String wifiApIp();
String wifiStaSsid();
const char *wifiModeText();

void wifiSaveSta(const char *ssid, const char *pass);
void wifiLoadSta(String &ssid, String &pass);
void wifiClearSta();
void wifiReconnectSta();