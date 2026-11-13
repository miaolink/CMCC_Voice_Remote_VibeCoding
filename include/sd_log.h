#pragma once
#include <Arduino.h>

bool sdLogBegin();
void sdLogLoop(); // flush periodically
bool sdMounted();
const char *sdRunPath();
const char *sdDebugPath();

// Always mirrors to Serial. Writes to SD when mounted.
void slog(const char *fmt, ...);
void dlog(const char *fmt, ...);
void slogLine(const char *line);
void dlogLine(const char *line);

// Force flush now
void sdLogFlush();