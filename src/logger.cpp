#include "logger.h"

#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <mutex>
#include <chrono>
#include <ios>
#include <iostream>

namespace efzda {
static std::wstring g_logPath;
static std::mutex g_logMutex;
static bool g_consoleEnabled = false;

static void write_line_locked(const std::string &line) {
    HANDLE hFile = CreateFileW(g_logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return;
    SetFilePointer(hFile, 0, nullptr, FILE_END);
    DWORD written = 0;
    WriteFile(hFile, line.c_str(), (DWORD)line.size(), &written, nullptr);
    WriteFile(hFile, "\r\n", 2, &written, nullptr);
    CloseHandle(hFile);
}

static std::string timestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return std::string(buf);
}

void init_logger(const std::wstring &moduleDir) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logPath = moduleDir + L"\\EfzRichPresence.log";
    // Probe writeability; if it fails, fallback to %TEMP%
    HANDLE hProbe = CreateFileW(g_logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hProbe == INVALID_HANDLE_VALUE) {
        wchar_t tmp[MAX_PATH];
        DWORD n = GetTempPathW(MAX_PATH, tmp);
        if (n > 0 && n < MAX_PATH) {
            g_logPath = std::wstring(tmp) + L"EfzRichPresence.log";
            HANDLE hAlt = CreateFileW(g_logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hAlt != INVALID_HANDLE_VALUE) CloseHandle(hAlt);
        }
        OutputDebugStringW(L"[EfzRichPresence] Logger fell back to %TEMP%\n");
    } else {
        CloseHandle(hProbe);
    }
    // Start file with session header
    write_line_locked("=== EfzRichPresence start: " + timestamp() + " ===");
    // Print resolved log path for diagnostics
    OutputDebugStringW((std::wstring(L"[EfzRichPresence] Logging to: ") + g_logPath + L"\n").c_str());
}

void shutdown_logger() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_logPath.empty())
    write_line_locked("=== EfzRichPresence stop: " + timestamp() + " ===");
}

void log(const char *fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    std::string line = "[" + timestamp() + "] " + buffer;
    write_line_locked(line);
    OutputDebugStringA((line + "\n").c_str());
    if (g_consoleEnabled) {
        std::fputs((line + "\n").c_str(), stdout);
        std::fflush(stdout);
    }
}

void logw(const wchar_t *fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    wchar_t wbuffer[2048];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(wbuffer, _TRUNCATE, fmt, args);
    va_end(args);

    int needed = WideCharToMultiByte(CP_UTF8, 0, wbuffer, -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(needed > 0 ? needed - 1 : 0, '\0');
    if (needed > 0)
        WideCharToMultiByte(CP_UTF8, 0, wbuffer, -1, utf8.data(), needed, nullptr, nullptr);

    std::string line = "[" + timestamp() + "] " + utf8;
    write_line_locked(line);
    OutputDebugStringA((line + "\n").c_str());
    if (g_consoleEnabled) {
        std::fputs((line + "\n").c_str(), stdout);
        std::fflush(stdout);
    }
}

void enable_console() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_consoleEnabled)
        return;
    bool consoleReady = false;

    // Prefer attaching to an existing parent console (e.g., if launched from a console)
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        consoleReady = true;
    } else {
        DWORD err = GetLastError();
        // ERROR_ACCESS_DENIED means we already have a console; treat as ready
        if (err == ERROR_ACCESS_DENIED) {
            consoleReady = true;
        } else {
            // Otherwise, allocate a new console
            if (AllocConsole()) {
                consoleReady = true;
            }
        }
    }

    if (consoleReady) {
        // Set UTF-8 code pages for proper output of extended characters
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);

        // Redirect standard streams to the console
        FILE* fp;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        freopen_s(&fp, "CONIN$",  "r", stdin);

    // Optional: ensure C++ iostreams are in sync with C stdio (set to false for perf)
    std::ios::sync_with_stdio(false);

    SetConsoleTitleW(L"EFZ Rich Presence Logs");
        g_consoleEnabled = true;

        std::fputs("[logger] Console enabled\n", stdout);
        std::fflush(stdout);
    }
}

} // namespace efzda
