// Native-test stand-in for the real ESP32 core's <WiFi.h>. Only resolves
// here because `c++ ... -Itest_native ...` puts THIS directory ahead of
// the real core's include path -- arduino-cli's actual firmware build
// never sees this directory, so the real WiFi.h is always what ships to
// the board. See wifi_stubs.h for the actual stub implementation.
#pragma once
#include "wifi_stubs.h"
