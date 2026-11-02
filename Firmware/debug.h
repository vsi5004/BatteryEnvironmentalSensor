#pragma once
#include <Arduino.h>
#include "config.h"

// At DEBUG=0 these are empty inline functions — the compiler eliminates all
// call sites, arguments, and format strings from the binary entirely.
// Include this header in every .cpp/.ino file that needs logging.
// Format strings must include '\n' explicitly (no auto-append).
#if DEBUG
  template<typename... Args>
  static inline void LOG(const char* fmt, Args... args) { Serial.printf(fmt, args...); }
  static inline void LOG_FLUSH() { Serial.flush(); }
#else
  template<typename... Args>
  static inline void LOG(const char*, Args...) {}
  static inline void LOG_FLUSH() {}
#endif
