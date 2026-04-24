#pragma once
#include <Arduino.h>

// arduino-esp32 3.x + PSRAM + BLE regression: after startBt(), UART0's
// tx_mux becomes NULL and any Serial call asserts. This flag gates all
// post-setup Serial output. Setup sets it to false right before startBt,
// from that point LOG_* are silent no-ops.
extern volatile bool g_uartAlive;

#define LOGF(...)    do { if (g_uartAlive) Serial.printf(__VA_ARGS__); } while (0)
#define LOGLN(s)     do { if (g_uartAlive) Serial.println(s); } while (0)
#define LOGLN_()     do { if (g_uartAlive) Serial.println();  } while (0)
#define LOGPRINT(s)  do { if (g_uartAlive) Serial.print(s);   } while (0)
#define LOGWRITE(p,n) ((g_uartAlive) ? Serial.write((const uint8_t*)(p), (size_t)(n)) : 0)

// Diagnostic output that bypasses the broken arduino UART driver by
// going straight to ROM's UART0 TX (ets_printf writes bytes directly
// to hardware without any FreeRTOS or driver state). Safe to call from
// any context, any core, before/during/after BLE init. Slower than
// Serial (no TX buffer), but always works.
extern "C" int ets_printf(const char* fmt, ...);
#define ROMLOGF(...)  ets_printf(__VA_ARGS__)
