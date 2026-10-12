// Stub. See firmware/test/stubs/README.md.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

// Consumes its arguments rather than discarding them, so the module's log calls
// are still counted as uses, and carries the printf format attribute so a
// mismatched format string is a compile error here rather than garbage on a
// serial console during an incident.
void treksafe_stub_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define LOG_INFO(...)  treksafe_stub_log(__VA_ARGS__)
#define LOG_WARN(...)  treksafe_stub_log(__VA_ARGS__)
#define LOG_ERROR(...) treksafe_stub_log(__VA_ARGS__)

// Arduino surface used by the module.
#define LOW 0
#define HIGH 1
#define INPUT_PULLUP 2
#define INPUT_PULLDOWN 3
#define OUTPUT 4
void pinMode(int pin, int mode);
int digitalRead(int pin);
void digitalWrite(int pin, int value);
uint32_t millis();
