// Native-test stand-in for the real ESP32 core's <Preferences.h>. Only
// resolves here because `c++ ... -Itest_native ...` puts THIS directory
// ahead of the real core's include path -- arduino-cli's actual firmware
// build never sees this directory, so the real Preferences.h is always
// what ships to the board. See wifi_stubs.h for the actual stub
// implementation (both fake headers point at the same file, since the
// class definitions there don't need separating).
#pragma once
#include "wifi_stubs.h"
