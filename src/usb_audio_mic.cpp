#include "usb_audio_mic.h"
#include "voice_codec.h"

#include <string.h>
#include "USB.h"

#if __has_include("USBAudioCard.h")
#include "USBAudioCard.h"
#define CMCC_HAS_UAC 1
#else
#define CMCC_HAS_UAC 0
#endif

static bool gReady = false;
static bool gPtt = false;
static volatile bool gHostCap = false;

#if CMCC_HAS_UAC
static USBAudioCard *gUac = nullptr;

static void onAudioEvent(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    auto *ev = (arduino_usb_audio_card_event_data_t *)data;
    if (id == ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT && ev) {
        if (ev->interface_enable.interface == UAC_INTERFACE_MIC) {
            gHostCap = ev->interface_enable.enable;
            Serial.printf("[UAC] mic host %s\n", gHostCap ? "ON" : "OFF");
        }
    }
}
#endif

void usbAudioBegin() {
    if (gReady) return;
#if CMCC_HAS_UAC
#if !defined(CONFIG_TINYUSB_AUDIO_ENABLED) || !CONFIG_TINYUSB_AUDIO_ENABLED
    Serial.println("[UAC] TinyUSB Audio disabled in board config");
    return;
#else
    // Mic-only, 16 kHz / 16-bit (matches G.722.1 decode)
    static USBAudioCard uac(16000, UAC_BPS_16, UAC_SPK_NONE, UAC_MIC_MONO);
    gUac = &uac;
    if (!gUac->begin()) {
        Serial.println("[UAC] begin failed");
        gUac = nullptr;
        return;
    }
    gUac->onEvent(onAudioEvent);
    gReady = true;
    Serial.println("[UAC] mic 16kHz mono ENABLED");
#endif
#else
    Serial.println("[UAC] USBAudioCard.h missing — rebuild with Arduino-ESP32 3.3+");
#endif
}

bool usbAudioReady() { return gReady; }
bool usbAudioHostCapturing() { return gHostCap; }
void usbAudioSetPtt(bool pressed) { gPtt = pressed; }
bool usbAudioPtt() { return gPtt; }

int usbAudioWritePcm(const int16_t *samples, int count) {
#if CMCC_HAS_UAC
    if (!gReady || !gUac || !samples || count <= 0) return 0;
    if (!gHostCap) return 0;
    uint16_t n = gUac->write(samples, (uint16_t)(count * (int)sizeof(int16_t)));
    return (int)(n / sizeof(int16_t));
#else
    (void)samples;
    (void)count;
    return 0;
#endif
}

void usbAudioLoop() {
#if CMCC_HAS_UAC
    if (!gReady || !gUac || !gHostCap) return;
    // Keep ISO IN fed with silence when no PTT so Windows capture stays alive
    if (!gPtt) {
        static int16_t silence[kVoiceFrameSamples];
        static bool z = false;
        if (!z) {
            memset(silence, 0, sizeof(silence));
            z = true;
        }
        // small chunks ~1ms @ 16k = 16 samples
        const int chunk = 16;
        for (int i = 0; i < 8; ++i) gUac->write(silence, (uint16_t)(chunk * 2));
    }
#endif
}
