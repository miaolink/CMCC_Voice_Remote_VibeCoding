#pragma once
#include <Arduino.h>
#include <stdint.h>

// CMCC 0xFC → 16 kHz mono PCM (G.722.1 16 kbps)
static constexpr int kVoiceSampleRate = 16000;
static constexpr int kVoiceFrameSamples = 320; // 20 ms
static constexpr int kVoiceCodecBytes = 40;    // 16 kbps / 50

void voiceCodecBegin();
void voiceCodecReset();

// Feed one 20B GATT notify (seqLE + idx + 00 + payload16).
// When a full frame is decoded, writes up to kVoiceFrameSamples into outPcm.
// Returns number of PCM samples produced (0 or 320).
int voiceCodecFeedNotify(const uint8_t *data20, size_t len, int16_t *outPcm);

uint32_t voiceCodecFramesOk();
uint32_t voiceCodecFramesGap();
