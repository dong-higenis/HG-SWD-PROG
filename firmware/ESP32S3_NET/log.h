#ifndef LOG_H_
#define LOG_H_

#include <Arduino.h>

// Events (state changes, errors) are always printed on USB Serial.
// Traces (per-frame / per-request detail) only after "log on" in the CLI.
extern volatile bool g_log_verbose;

#define LOGI(tag, fmt, ...) Serial.printf("[%7lu][" tag "] " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__)
#define LOGV(tag, fmt, ...)       \
  do                              \
  {                               \
    if (g_log_verbose)            \
    {                             \
      LOGI(tag, fmt, ##__VA_ARGS__); \
    }                             \
  } while (0)

#endif
