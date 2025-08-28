#pragma once
#include <string>

namespace efzda {

// Compile-time logging switch. Define EFZDA_ENABLE_LOGGING=1 in your build or
// before including this header to enable logging; otherwise logging is disabled.
#ifndef EFZDA_ENABLE_LOGGING
#define EFZDA_ENABLE_LOGGING 1
#endif

// Initialize logger to write EternalFighterZero.log into moduleDir
void init_logger(const std::wstring &moduleDir);
void shutdown_logger();
// Optional console window for live logs
void enable_console();

// Thread-safe logging helpers
void log(const char *fmt, ...);
void logw(const wchar_t *fmt, ...);

}
