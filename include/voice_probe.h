#pragma once
#include <Arduino.h>

void voiceProbeBegin();
void voiceProbeStart();
void voiceProbeStop();
bool voiceProbeActive();

// Feed every BLE notify/indicate (HID or custom voice GATT)
void voiceProbeOnNotify(const char *uuid, const uint8_t *data, size_t len);

String voiceProbeStatusJson();
// newestFirst: 0 = newest. Returns up to n events.
String voiceProbeEventsJson(int n = 40);
