#pragma once
#include <string>

namespace efzda {

// Initialize logger to write EternalFighterZero.log into moduleDir
void init_logger(const std::wstring &moduleDir);
void shutdown_logger();
// Optional console window for live logs
void enable_console();

// Thread-safe logging helpers
void log(const char *fmt, ...);
void logw(const wchar_t *fmt, ...);

}
