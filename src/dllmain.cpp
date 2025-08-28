#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>

#include "version.h"
#include "logger.h"
#include "config.h"
#include "discord/discord_client.h"
#include "state/game_state_provider.h"

using namespace std::chrono_literals;

namespace {
std::atomic<bool> g_running{false};
std::thread g_worker;
HMODULE g_selfModule = nullptr;

std::wstring get_module_dir(HMODULE hMod) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(hMod, path, MAX_PATH);
    std::wstring full(path);
    size_t pos = full.find_last_of(L"/\\");
    if (pos != std::wstring::npos)
        return full.substr(0, pos);
    return L".";
}

// emergency_log_line removed (SEH path reverted)

void worker_main(HMODULE hMod) {
    // Initialize immediately

    auto moduleDir = get_module_dir(hMod);
    OutputDebugStringW(L"[EfzRichPresence] About to init_logger\n");
    efzda::init_logger(moduleDir);
    efzda::log("Stage: after init_logger");
    OutputDebugStringW(L"[EfzRichPresence] init_logger done\n");
    // Console disabled by default; opt-in via EFZDA_ENABLE_CONSOLE=1
    wchar_t envBuf[8];
    DWORD n = GetEnvironmentVariableW(L"EFZDA_ENABLE_CONSOLE", envBuf, _countof(envBuf));
    if (n > 0) {
        efzda::enable_console();
        efzda::log("Stage: console enabled (env)");
    } else {
        efzda::log("Stage: console skipped");
    }
    try {
        efzda::log("EfzRichPresence v%s starting...", EFZDA_VERSION);
    } catch (...) {
        OutputDebugStringW(L"[EfzRichPresence] log(starting) threw\n");
    }
    efzda::DiscordClient discord;
    bool discordReady = false;
    try {
        auto cfg = efzda::load_config(moduleDir);
        efzda::log("Stage: after load_config");
        OutputDebugStringW(L"[EfzRichPresence] Config loaded\n");
        discordReady = discord.init(cfg.discordAppId);
        efzda::log("Stage: after discord.init (%s)", discordReady ? "ok" : "fail");
        OutputDebugStringW(discordReady ? L"[EfzRichPresence] Discord init OK\n" : L"[EfzRichPresence] Discord init failed\n");
    } catch (...) {
        efzda::log("Stage: exception during config/discord init; continuing with Discord disabled");
    }

    efzda::GameStateProvider provider;
    efzda::GameState last{};
    efzda::log("Stage: entering poll loop");
    OutputDebugStringW(L"[EfzRichPresence] Entering poll loop\n");

    // Optional: clear-before-update to mitigate sticky presence in some clients
    bool clearBeforeUpdate = false;
    try {
        wchar_t cbu[8];
        clearBeforeUpdate = GetEnvironmentVariableW(L"EFZDA_CLEAR_BEFORE_UPDATE", cbu, _countof(cbu)) > 0;
    } catch (...) {}

    // Poll interval (ms). Default 500ms for responsiveness; override via EFZDA_POLL_MS
    unsigned int pollMs = 500;
    try {
        wchar_t pbuf[16];
        if (GetEnvironmentVariableW(L"EFZDA_POLL_MS", pbuf, _countof(pbuf)) > 0) {
            unsigned long v = wcstoul(pbuf, nullptr, 10);
            if (v >= 100 && v <= 5000) pollMs = static_cast<unsigned int>(v);
        }
    } catch (...) {}

    while (g_running.load(std::memory_order_relaxed)) {
        try {
            auto cur = provider.get();
            if (cur != last) {
                efzda::log("State change: details='%s' state='%s'", cur.details.c_str(), cur.state.c_str());
                if (discordReady) {
                    if (clearBeforeUpdate) {
                        discord.clearPresence();
                        // Tiny delay to let Discord register the clear
                        std::this_thread::sleep_for(50ms);
                    }
                    discord.updatePresence(cur.details, cur.state,
                                            cur.smallImageKey, cur.smallImageText,
                                            cur.largeImageKey, cur.largeImageText);
                }
                last = cur;
            }
            if (discordReady)
                discord.poll();
        } catch (...) {
            efzda::log("Worker loop caught unexpected exception; continuing");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
    }

    if (discordReady) {
        discord.clearPresence();
        discord.shutdown();
    }
    efzda::shutdown_logger();
    // Ensure the module reference acquired at attach is released on thread exit
    if (hMod) {
        FreeLibraryAndExitThread(hMod, 0);
    }
}

} // anonymous

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        OutputDebugStringW(L"[EfzRichPresence] DLL_PROCESS_ATTACH\n");
        g_running = true;
        try {
            // Pin the module to avoid being unloaded while worker is running
            HMODULE mod = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                   reinterpret_cast<LPCWSTR>(&DllMain), &mod)) {
                g_selfModule = mod;
            } else {
                g_selfModule = hModule; // fallback
            }
            // Start worker thread and immediately detach to avoid waiting in DllMain
            g_worker = std::thread(worker_main, g_selfModule);
            g_worker.detach();
        } catch (...) {
            OutputDebugStringW(L"[EfzRichPresence] Failed to start worker thread\n");
            g_running = false;
        }
        break;
    case DLL_PROCESS_DETACH:
        g_running = false;
        // Do NOT join here; waiting in DllMain can deadlock under the loader lock.
        // The detached worker will observe g_running=false and exit promptly.
        break;
    }
    return TRUE;
}
