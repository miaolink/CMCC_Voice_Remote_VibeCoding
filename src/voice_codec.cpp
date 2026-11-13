#include "voice_codec.h"

#include <string.h>

extern "C" {
#include "g722_1/g722_1.h"
}

static constexpr uint8_t kChunkData = 16;
static constexpr uint8_t kChunkHead = 4;
static constexpr uint16_t kXorKey = 0x0416;
static const uint8_t kIvDec[12] = {4, 16, 9, 8, 3, 11, 2, 6, 18, 7, 13, 17};

static g722_1_decode_state_t gDec;
static bool gDecOk = false;
static uint32_t gOk = 0;
static uint32_t gGap = 0;
static uint16_t gLastSeq = 0;
static bool gHaveLastSeq = false;

// seq -> up to 3x16B payloads
struct Slot {
    uint16_t seq = 0;
    uint8_t mask = 0;
    uint8_t part[3][kChunkData];
};
static constexpr int kSlotN = 8;
static Slot gSlots[kSlotN];

static void dewhite(const uint8_t frame48[48], uint8_t out40[40]) {
    uint16_t words[20];
    for (int i = 0; i < 20; ++i)
        words[i] = (uint16_t)frame48[i * 2] | ((uint16_t)frame48[i * 2 + 1] << 8);
    for (int g = 0; g < 4; ++g) {
        int a = kIvDec[3 * g], b = kIvDec[3 * g + 1], c = kIvDec[3 * g + 2];
        uint16_t oa = words[a], ob = words[b], oc = words[c];
        words[a] = oc;
        words[b] = oa;
        words[c] = ob;
    }
    for (int i = 0; i < 20; ++i) words[i] = (uint16_t)(words[i] ^ kXorKey);
    for (int i = 0; i < 20; ++i) {
        out40[i * 2] = (uint8_t)((words[i] >> 8) & 0xFF);
        out40[i * 2 + 1] = (uint8_t)(words[i] & 0xFF);
    }
}

static Slot *slotFor(uint16_t seq) {
    for (int i = 0; i < kSlotN; ++i)
        if (gSlots[i].mask && gSlots[i].seq == seq) return &gSlots[i];
    for (int i = 0; i < kSlotN; ++i)
        if (!gSlots[i].mask) {
            gSlots[i].seq = seq;
            gSlots[i].mask = 0;
            return &gSlots[i];
        }
    // drop oldest-ish: clear first
    gSlots[0].mask = 0;
    gSlots[0].seq = seq;
    return &gSlots[0];
}

void voiceCodecBegin() {
    memset(&gDec, 0, sizeof(gDec));
    gDecOk = g722_1_decode_init(&gDec, 16000, 16000) != nullptr;
    voiceCodecReset();
}

void voiceCodecReset() {
    memset(gSlots, 0, sizeof(gSlots));
    gOk = 0;
    gGap = 0;
    gHaveLastSeq = false;
    if (gDecOk) {
        // re-init decoder state for a new utterance
        g722_1_decode_init(&gDec, 16000, 16000);
    }
}

uint32_t voiceCodecFramesOk() { return gOk; }
uint32_t voiceCodecFramesGap() { return gGap; }

int voiceCodecFeedNotify(const uint8_t *data20, size_t len, int16_t *outPcm) {
    if (!gDecOk || !data20 || !outPcm || len < (size_t)(kChunkHead + kChunkData)) return 0;
    if (data20[3] != 0x00 || data20[2] > 2) return 0;
    if (data20[0] == 0x82 && data20[1] == 0x03) return 0; // voice key

    uint16_t seq = (uint16_t)data20[0] | ((uint16_t)data20[1] << 8);
    uint8_t idx = data20[2];
    if (idx == 0) {
        if (gHaveLastSeq && seq != (uint16_t)(gLastSeq + 1)) gGap++;
        gLastSeq = seq;
        gHaveLastSeq = true;
    }

    Slot *s = slotFor(seq);
    memcpy(s->part[idx], data20 + kChunkHead, kChunkData);
    s->mask = (uint8_t)(s->mask | (1u << idx));
    if (s->mask != 0x07) return 0;

    uint8_t frame48[48];
    memcpy(frame48 + 0, s->part[0], 16);
    memcpy(frame48 + 16, s->part[1], 16);
    memcpy(frame48 + 32, s->part[2], 16);
    s->mask = 0;

    uint8_t codec[40];
    dewhite(frame48, codec);
    int n = g722_1_decode(&gDec, outPcm, codec, kVoiceCodecBytes);
    if (n > 0) gOk++;
    return n > 0 ? n : 0;
}
