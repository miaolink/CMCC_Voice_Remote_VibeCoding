#include "voice_probe.h"
#include "sd_log.h"

#include <SD.h>

static constexpr int kRing = 180;
static constexpr int kMaxBytes = 48;

struct Evt {
    uint32_t ms = 0;
    uint16_t seq = 0;
    uint8_t len = 0;
    uint8_t data[kMaxBytes] = {};
    char uuid[28] = {};
};

static Evt gRing[kRing];
static int gHead = 0;
static int gCount = 0;
static bool gActive = false;
static uint32_t gStartMs = 0;
static uint32_t gStopMs = 0;
static uint32_t gPkt = 0;
static uint32_t gBytes = 0;
static uint16_t gSeq = 0;
static uint32_t gWinStart = 0;
static uint32_t gWinPkt = 0;
static float gPps = 0;
static uint16_t gLenHist[49]; // count by length 0..48
static uint32_t gLastMs = 0;
static uint32_t gFcLike = 0; // len==20 with idx 0/1/2 (CMCC 0xFC audio chunk)

void voiceProbeBegin() {
    gActive = false;
    gCount = 0;
    gHead = 0;
}

bool voiceProbeActive() { return gActive; }

void voiceProbeStart() {
    gActive = true;
    gStartMs = millis();
    gStopMs = 0;
    gPkt = 0;
    gBytes = 0;
    gSeq = 0;
    gCount = 0;
    gHead = 0;
    gWinStart = gStartMs;
    gWinPkt = 0;
    gPps = 0;
    gLastMs = 0;
    gFcLike = 0;
    memset(gLenHist, 0, sizeof(gLenHist));
    slog("voice probe START");
}

void voiceProbeStop() {
    if (!gActive) return;
    gActive = false;
    gStopMs = millis();
    slog("voice probe STOP pkts=%u bytes=%u dur=%ums", (unsigned)gPkt, (unsigned)gBytes,
         (unsigned)(gStopMs - gStartMs));

    if (sdMounted()) {
        File f = SD.open("/logs/voice_probe.log", FILE_APPEND);
        if (f) {
            f.printf("\n=== probe %lu-%lu pkts=%u bytes=%u pps~%.1f ===\n", (unsigned long)gStartMs,
                     (unsigned long)gStopMs, (unsigned)gPkt, (unsigned)gBytes, (double)gPps);
            int n = gCount < 80 ? gCount : 80;
            for (int i = n - 1; i >= 0; --i) {
                int idx = gHead - 1 - i;
                while (idx < 0) idx += kRing;
                Evt &e = gRing[idx % kRing];
                f.printf("t=%lu seq=%u uuid=%s len=%u ", (unsigned long)e.ms, (unsigned)e.seq, e.uuid,
                         (unsigned)e.len);
                for (uint8_t b = 0; b < e.len; ++b) f.printf("%02X", e.data[b]);
                f.println();
            }
            f.close();
        }
    }
}

void voiceProbeOnNotify(const char *uuid, const uint8_t *data, size_t len) {
    if (!gActive || !data || !len) return;
    if (len > kMaxBytes) len = kMaxBytes;

    uint32_t now = millis();
    Evt &e = gRing[gHead];
    e.ms = now;
    e.seq = ++gSeq;
    e.len = (uint8_t)len;
    memcpy(e.data, data, len);
    e.uuid[0] = 0;
    if (uuid && uuid[0]) {
        strncpy(e.uuid, uuid, sizeof(e.uuid) - 1);
        e.uuid[sizeof(e.uuid) - 1] = 0;
    }
    gHead = (gHead + 1) % kRing;
    if (gCount < kRing) gCount++;

    gPkt++;
    gBytes += (uint32_t)len;
    gLastMs = now;
    if (len < 49) gLenHist[len]++;
    // CMCC 0xFC audio: 20B = seqLE(2)+idx(0..2)+0x00+payload16
    // Exclude learned voice-key report (82 03 01 00 …) which is also 20B padded
    if (len == 20 && data[3] == 0x00 && data[2] <= 2 && !(data[0] == 0x82 && data[1] == 0x03))
        gFcLike++;

    gWinPkt++;
    if (now - gWinStart >= 1000) {
        gPps = (float)gWinPkt * 1000.0f / (float)(now - gWinStart);
        gWinStart = now;
        gWinPkt = 0;
    }
}

static String toHex(const uint8_t *data, size_t len) {
    String s;
    s.reserve(len * 3);
    for (size_t i = 0; i < len; ++i) {
        if (i) s += ' ';
        char b[4];
        snprintf(b, sizeof(b), "%02X", data[i]);
        s += b;
    }
    return s;
}

String voiceProbeStatusJson() {
    uint32_t now = millis();
    uint32_t dur = gActive ? (now - gStartMs) : (gStopMs > gStartMs ? gStopMs - gStartMs : 0);
    float avgLen = gPkt ? (float)gBytes / (float)gPkt : 0;
    // Heuristic: CMCC 0xFC ~50pkt/s of 20B, or sustained high pps
    bool likely = (gFcLike >= 30 && gPps >= 10.0f) || (gPps >= 20.0f && avgLen >= 16.0f && gPkt >= 40);

    String j = "{";
    j += "\"active\":";
    j += gActive ? "true" : "false";
    j += ",\"pkt\":";
    j += String(gPkt);
    j += ",\"bytes\":";
    j += String(gBytes);
    j += ",\"dur_ms\":";
    j += String(dur);
    j += ",\"pps\":";
    j += String(gPps, 1);
    j += ",\"avg_len\":";
    j += String(avgLen, 1);
    j += ",\"ring\":";
    j += String(gCount);
    j += ",\"last_ms\":";
    j += String(gLastMs);
    j += ",\"likely_audio\":";
    j += likely ? "true" : "false";
    j += ",\"hint\":\"";
    if (!gPkt)
        j += "请按住遥控器 MIC/语音键 2~5 秒";
    else if (likely)
        j += "检测到疑似 0xFC 音频流";
    else if (gPps < 3.0f)
        j += "多为按键报告；先点开麦再按住语音键";
    else
        j += "有连续包，继续按住 MIC 观察 pps / fc_like";
    j += "\",\"fc_like\":";
    j += String(gFcLike);
    j += ",\"len_hist\":{";
    bool first = true;
    for (int i = 0; i < 49; ++i) {
        if (!gLenHist[i]) continue;
        if (!first) j += ',';
        first = false;
        j += "\"";
        j += String(i);
        j += "\":";
        j += String(gLenHist[i]);
    }
    j += "}}";
    return j;
}

String voiceProbeEventsJson(int n) {
    if (n < 1) n = 1;
    if (n > gCount) n = gCount;
    if (n > 80) n = 80;

    String j = "{\"events\":[";
    for (int i = 0; i < n; ++i) {
        if (i) j += ',';
        int idx = gHead - 1 - i;
        while (idx < 0) idx += kRing;
        Evt &e = gRing[idx % kRing];
        j += "{\"seq\":";
        j += String(e.seq);
        j += ",\"ms\":";
        j += String(e.ms);
        j += ",\"uuid\":\"";
        j += e.uuid;
        j += "\",\"len\":";
        j += String(e.len);
        j += ",\"hex\":\"";
        j += toHex(e.data, e.len);
        j += "\"}";
    }
    j += "]}";
    return j;
}
