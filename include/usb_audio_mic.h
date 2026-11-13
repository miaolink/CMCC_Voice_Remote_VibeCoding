#pragma once
#include <Arduino.h>
#include <stdint.h>

// USB UAC microphone (16 kHz mono 16-bit). Always enumerated; silence when idle.
void usbAudioBegin();
bool usbAudioReady();
void usbAudioLoop();

// Push PCM samples (int16 mono). Returns samples accepted.
int usbAudioWritePcm(const int16_t *samples, int count);

// Host opened the mic streaming interface (alt setting > 0).
bool usbAudioHostCapturing();

void usbAudioSetPtt(bool pressed);
bool usbAudioPtt();
