#include "voice_audio.h"
#include "voice_codec.h"
#include "usb_audio_mic.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

struct AudioFrame {
    uint8_t len = 0;
    uint8_t data[24] = {};
};

static QueueHandle_t gQ = nullptr;
static constexpr int kQDepth = 384;
static volatile uint32_t gFrames = 0;
static volatile uint32_t gDropped = 0;
static volatile bool gStreaming = false;
static uint32_t gLastMs = 0;
static bool gWasStreaming = false;
static int8_t gPttEvent = -1; // -1 none, 0 up, 1 down
static bool gCdcHex = true;   // debug bypass

void voiceAudioBegin() {
    if (!gQ) gQ = xQueueCreate(kQDepth, sizeof(AudioFrame));
    voiceCodecBegin();
}

bool voiceAudioStreaming() { return gStreaming; }
uint32_t voiceAudioFrames() { return gFrames; }
uint32_t voiceAudioDropped() { return gDropped; }

void voiceAudioMarkPtt(bool pressed) {
    gPttEvent = pressed ? 1 : 0;
    usbAudioSetPtt(pressed);
    if (pressed) {
        if (gQ) xQueueReset(gQ);
        voiceCodecReset();
        gFrames = 0;
        gDropped = 0;
    }
}

void voiceAudioOnNotify(const uint8_t *data, size_t len) {
    if (!data || len < 20 || len > sizeof(((AudioFrame *)0)->data)) return;
    if (data[3] != 0x00 || data[2] > 2) return;
    if (data[0] == 0x82 && data[1] == 0x03) return;

    gLastMs = millis();
    gStreaming = true;
    gFrames++;
    if (!gQ) {
        gDropped++;
        return;
    }
    AudioFrame f;
    f.len = (uint8_t)len;
    memcpy(f.data, data, len);
    if (xQueueSend(gQ, &f, 0) != pdTRUE) gDropped++;
}

void voiceAudioLoop() {
    if (!gQ) return;

    if (gPttEvent >= 0) {
        int8_t ve = gPttEvent;
        gPttEvent = -1;
        Serial.printf("# VOICE %d\n", (int)ve);
    }

    static const char HEXC[] = "0123456789ABCDEF";
    char buf[64];
    AudioFrame f;
    int16_t pcm[kVoiceFrameSamples];
    int budget = 64;
    while (budget-- > 0) {
        if (xQueueReceive(gQ, &f, 0) != pdTRUE) break;

        int ns = voiceCodecFeedNotify(f.data, f.len, pcm);
        if (ns > 0) {
            int wrote = 0;
            while (wrote < ns) {
                int n = usbAudioWritePcm(pcm + wrote, ns - wrote);
                if (n <= 0) break;
                wrote += n;
            }
        }

        if (gCdcHex) {
            size_t n = 2 + (size_t)f.len * 2 + 1;
            if (n >= sizeof(buf)) n = sizeof(buf) - 1;
            if (Serial.availableForWrite() >= (int)n) {
                size_t k = 0;
                buf[k++] = 'A';
                buf[k++] = ' ';
                for (size_t i = 0; i < f.len && k + 3 < sizeof(buf); ++i) {
                    buf[k++] = HEXC[(f.data[i] >> 4) & 0x0F];
                    buf[k++] = HEXC[f.data[i] & 0x0F];
                }
                buf[k++] = '\n';
                if (Serial.write((const uint8_t *)buf, k) != (int)k) gDropped++;
            }
        }
    }

    usbAudioLoop();

    if (gStreaming && (millis() - gLastMs) > 300) gStreaming = false;
    if (gStreaming != gWasStreaming) {
        if (gStreaming)
            Serial.printf("# A START n=%lu drop=%lu codec_ok=%lu\n", (unsigned long)gFrames,
                          (unsigned long)gDropped, (unsigned long)voiceCodecFramesOk());
        else
            Serial.println("# A END");
        gWasStreaming = gStreaming;
    }
}
