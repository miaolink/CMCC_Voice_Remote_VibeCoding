#include "sd_log.h"

#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>

// Cardputer / ADV microSD (SPI, separate from TFT FSPI)
static constexpr int kSdCs = 12;
static constexpr int kSdSck = 40;
static constexpr int kSdMiso = 39;
static constexpr int kSdMosi = 14;
static constexpr int kSdHoldGpio = 5; // must stay HIGH for SD

static constexpr const char *kLogDir = "/logs";
static constexpr const char *kRunFile = "/logs/run.log";
static constexpr const char *kDbgFile = "/logs/debug.log";
static constexpr size_t kMaxFileBytes = 512 * 1024;
static constexpr uint32_t kFlushMs = 2000;

static SPIClass sdSPI(HSPI);
static bool mounted = false;
static File runFile;
static File dbgFile;
static uint32_t lastFlushMs = 0;
static char runPathBuf[24] = "/logs/run.log";
static char dbgPathBuf[24] = "/logs/debug.log";

static void stamp(char *buf, size_t n) {
    // millis-based stamp (no RTC required)
    uint32_t ms = millis();
    uint32_t s = ms / 1000;
    uint32_t m = s / 60;
    uint32_t h = m / 60;
    snprintf(buf, n, "%02u:%02u:%02u.%03u", (unsigned)(h % 100), (unsigned)(m % 60), (unsigned)(s % 60),
             (unsigned)(ms % 1000));
}

static void rotateIfHuge(const char *path, File &f) {
    if (!f) return;
    if ((size_t)f.size() < kMaxFileBytes) return;
    f.close();
    String bak = String(path) + ".old";
    SD.remove(bak);
    SD.rename(path, bak);
    f = SD.open(path, FILE_APPEND);
}

static void appendLine(File &f, const char *path, const char *level, const char *msg) {
    if (!mounted || !f) return;
    rotateIfHuge(path, f);
    if (!f) {
        f = SD.open(path, FILE_APPEND);
        if (!f) return;
    }
    char ts[24];
    stamp(ts, sizeof(ts));
    f.printf("[%s] %s %s\n", ts, level, msg);
}

bool sdLogBegin() {
    pinMode(kSdHoldGpio, OUTPUT);
    digitalWrite(kSdHoldGpio, HIGH);

    sdSPI.begin(kSdSck, kSdMiso, kSdMosi, kSdCs);
    delay(20);

    mounted = SD.begin(kSdCs, sdSPI, 25000000);
    if (!mounted) {
        // retry slower
        delay(50);
        mounted = SD.begin(kSdCs, sdSPI, 4000000);
    }

    if (!mounted) {
        Serial.println("[SD] mount FAILED");
        return false;
    }

    if (!SD.exists(kLogDir)) {
        SD.mkdir(kLogDir);
    }

    runFile = SD.open(kRunFile, FILE_APPEND);
    dbgFile = SD.open(kDbgFile, FILE_APPEND);
    if (!runFile || !dbgFile) {
        Serial.println("[SD] open log files FAILED");
        mounted = runFile || dbgFile;
    }

    uint64_t cardSize = SD.cardSize() / (1024 * 1024);
    Serial.printf("[SD] mounted OK size=%lluMB\n", (unsigned long long)cardSize);
    slog("boot SD ok size=%lluMB", (unsigned long long)cardSize);
    dlog("paths run=%s debug=%s", kRunFile, kDbgFile);
    sdLogFlush();
    return mounted;
}

void sdLogFlush() {
    if (runFile) runFile.flush();
    if (dbgFile) dbgFile.flush();
    lastFlushMs = millis();
}

void sdLogLoop() {
    if (!mounted) return;
    if (millis() - lastFlushMs >= kFlushMs) sdLogFlush();
}

bool sdMounted() { return mounted; }
const char *sdRunPath() { return runPathBuf; }
const char *sdDebugPath() { return dbgPathBuf; }

static void vlog(bool debug, const char *fmt, va_list ap) {
    char msg[256];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    Serial.print(debug ? "[D] " : "[R] ");
    Serial.println(msg);
    if (debug) appendLine(dbgFile, kDbgFile, "DBG", msg);
    else appendLine(runFile, kRunFile, "RUN", msg);
}

void slog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(false, fmt, ap);
    va_end(ap);
}

void dlog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(true, fmt, ap);
    va_end(ap);
}

void slogLine(const char *line) { slog("%s", line ? line : ""); }
void dlogLine(const char *line) { dlog("%s", line ? line : ""); }