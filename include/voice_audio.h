#pragma once
#include <Arduino.h>

// CMCC 0xFC audio: queue 20B GATT notifies → Serial "A <HEX>" for PC voice_bridge
void voiceAudioBegin();
void voiceAudioLoop(); // drain queue to CDC (call from main loop)

// Feed from BLE notify (safe: non-blocking enqueue)
void voiceAudioOnNotify(const uint8_t *data, size_t len);

// PTT boundary markers for PC: "# VOICE 1" / "# VOICE 0"
void voiceAudioMarkPtt(bool pressed);

bool voiceAudioStreaming();
uint32_t voiceAudioFrames();
uint32_t voiceAudioDropped();
