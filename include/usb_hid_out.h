#pragma once
#include "keymap.h"

void usbHidBegin();
bool usbHidReady();

// Legacy arrow helpers
void usbHidPressArrow(ArrowKey key);
void usbHidReleaseArrow();
ArrowKey usbHidCurrentArrow();

// Full remote key (0..15) -> USB keyboard/consumer
void usbHidPressRemote(int keyIndex);
void usbHidReleaseRemote();
int usbHidCurrentRemote();