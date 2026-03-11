// Real EFZ-backed provider implementation (replacing the stub)
#include "state/game_state_provider.h"

#include <windows.h>
#include <cstdint>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <cctype>
#include <atomic>
#include <cwctype>
#include <type_traits>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include "logger.h"
#include "efz_netplay_state.h"

namespace efzda {

namespace {
// Offsets derived from efz_streaming
constexpr uintptr_t EFZ_BASE_OFFSET_P1 = 0x390104;
constexpr uintptr_t EFZ_BASE_OFFSET_P2 = 0x390108;
constexpr uintptr_t EFZ_BASE_OFFSET_GAME_STATE = 0x39010C;
// Global active-screen index (byte_790148) from efz.exe.c: absolute 0x00790148 -> base offset 0x390148
constexpr uintptr_t EFZ_GLOBAL_SCREEN_INDEX_OFFSET = 0x390148; // byte: current UI state index
constexpr uintptr_t CHARACTER_NAME_OFFSET = 0x94;

// EfzRevival version-aware RVAs
// 1.02e/f/g: wins-base ptr RVA=0x00A02CC, online-state RVA=0x00A05D0
// 1.02h:     wins-base ptr RVA=0x00A02EC, online-state RVA=0x00A05F0
// 1.02i:     wins-base ptr RVA=0x00A15F8, online-state RVA=0x00A15FC
constexpr uintptr_t P1_WIN_COUNT_OFFSET_1_02h = 0x4C8;
constexpr uintptr_t P2_WIN_COUNT_OFFSET_1_02h = 0x4CC;
constexpr uintptr_t P1_WIN_COUNT_OFFSET_1_02i = 0x4D0; // from CE table (netplay)
constexpr uintptr_t P2_WIN_COUNT_OFFSET_1_02i = 0x4D4; // from CE table (netplay)
// Tournament mode win counters (observed on 1.02h!!!): base+0x2FC and base+0x300
constexpr uintptr_t P1_TOURN_WIN_COUNT_OFFSET_1_02h = 0x2FC;
constexpr uintptr_t P2_TOURN_WIN_COUNT_OFFSET_1_02h = 0x300;
constexpr uintptr_t P1_TOURN_WIN_COUNT_OFFSET_1_02i = 0x304; // from CE table provided
constexpr uintptr_t P2_TOURN_WIN_COUNT_OFFSET_1_02i = 0x308; // from CE table provided
constexpr uintptr_t P1_WIN_COUNT_SPECTATOR_OFFSET = 0x80; // fallback
constexpr uintptr_t P2_WIN_COUNT_SPECTATOR_OFFSET = 0x84; // fallback
// Nicknames (wide strings) relative to same base pointer
constexpr uintptr_t P1_NICKNAME_OFFSET_1_02h = 0x3BE;
constexpr uintptr_t P2_NICKNAME_OFFSET_1_02h = 0x43E;
constexpr uintptr_t P1_NICKNAME_OFFSET_1_02i = 0x3C6; // from CE table (netplay)
constexpr uintptr_t P2_NICKNAME_OFFSET_1_02i = 0x446; // from CE table (netplay)
constexpr uintptr_t P1_NICKNAME_SPECTATOR_OFFSET = 0x9A;
constexpr uintptr_t P2_NICKNAME_SPECTATOR_OFFSET = 0x11A;
// "Current player" index: 0 = P1, 1 = P2, relative to same base pointer
constexpr uintptr_t CURRENT_PLAYER_OFFSET_1_02h = 0x2A8; // verified for 1.02h!!!
constexpr uintptr_t CURRENT_PLAYER_OFFSET_1_02i = 0x2B0; // verified from 1.02i decomp (this+688)

// From efz-training-mode
constexpr uintptr_t GAME_MODE_OFFSET = 0x1364; // byte in game state struct
// Choose actual offsets at runtime based on EFZ window title ("-Revival- 1.02e/h/i").

static inline unsigned long long ticks() { return GetTickCount64(); }
// Silent memory read (no logging), for probing purposes
static bool read_bytes_no_log(const void* addr, void* buffer, size_t size) {
    SIZE_T read = 0;
    if (!addr || !buffer || size == 0) return false;
    return ReadProcessMemory(GetCurrentProcess(), addr, buffer, size, &read) && read == size;
}

// Hex dump helper used by safe_read logging
static std::string hex_bytes(const void* data, size_t size, size_t maxOut = 16) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data);
    size_t n = size;
    if (n > maxOut) n = maxOut; // cap to avoid gigantic lines
    std::string out;
    out.reserve(n * 3);
    char buf[4];
    for (size_t i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof(buf), "%02X", (unsigned)p[i]);
        out.append(buf);
        if (i + 1 < n) out.push_back(' ');
    }
    if (n < size) out.append(" ...");
    return out;
}

// Generic safe_read template must be visible before first use
template <typename T>
bool safe_read(const void* addr, T& out) {
    SIZE_T read = 0;
    if (!addr) return false;
    bool ok = (ReadProcessMemory(GetCurrentProcess(), addr, &out, sizeof(T), &read) && read == sizeof(T));
    if (ok) {
        // Log success with address, size, and bytes (and value for integrals)
        std::string bytes = hex_bytes(&out, sizeof(T));
        if constexpr (std::is_integral<T>::value || std::is_pointer<T>::value) {
            efzda::log("[tick=%llu] READ ok @%p size=%zu bytes=[%s] value=0x%llX", ticks(), addr, sizeof(T), bytes.c_str(), (unsigned long long)(uintptr_t)out);
        } else {
            efzda::log("[tick=%llu] READ ok @%p size=%zu bytes=[%s]", ticks(), addr, sizeof(T), bytes.c_str());
        }
        return true;
    } else {
        DWORD err = GetLastError();
        efzda::log("[tick=%llu] READ fail @%p size=%zu read=%zu err=%lu", ticks(), addr, sizeof(T), (size_t)read, (unsigned long)err);
        return false;
    }
}

bool safe_read_bytes(const void* addr, void* buffer, size_t size) {
    SIZE_T read = 0;
    if (!addr || !buffer || size == 0) return false;
    bool ok = (ReadProcessMemory(GetCurrentProcess(), addr, buffer, size, &read) && read == size);
    if (ok) {
        std::string bytes = hex_bytes(buffer, size);
        efzda::log("[tick=%llu] READBYTES ok @%p size=%zu bytes=[%s]", ticks(), addr, size, bytes.c_str());
        return true;
    } else {
        DWORD err = GetLastError();
        efzda::log("[tick=%llu] READBYTES fail @%p size=%zu read=%zu err=%lu", ticks(), addr, size, (size_t)read, (unsigned long)err);
        return false;
    }
}

// --- EfzRevival version detection and RVA selection ---
enum class EfzRevivalVersion : int { Unknown = 0, Vanilla, Revival102e, Revival102f, Revival102g, Revival102h, Revival102i, Other };

// PE TimeDateStamp values from decompilation/release inventory (used by netplay mod too)
constexpr DWORD REVIVAL_TS_102E = 0x5EA876B0;
constexpr DWORD REVIVAL_TS_102F = 0x5F8C58A3;
constexpr DWORD REVIVAL_TS_102G = 0x6240CE73;
constexpr DWORD REVIVAL_TS_102H = 0x62929371;
constexpr DWORD REVIVAL_TS_102I = 0x63BF27EA;

static EfzRevivalVersion DetectEfzRevivalVersionByTimestamp() {
    HMODULE revival = GetModuleHandleW(L"EfzRevival.dll");
    if (!revival) return EfzRevivalVersion::Unknown;

    auto base = reinterpret_cast<const unsigned char*>(revival);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return EfzRevivalVersion::Unknown;
    if (dos->e_lfanew <= 0) return EfzRevivalVersion::Unknown;

    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) return EfzRevivalVersion::Unknown;

    switch (nt->FileHeader.TimeDateStamp) {
        case REVIVAL_TS_102E: return EfzRevivalVersion::Revival102e;
        case REVIVAL_TS_102F: return EfzRevivalVersion::Revival102f;
        case REVIVAL_TS_102G: return EfzRevivalVersion::Revival102g;
        case REVIVAL_TS_102H: return EfzRevivalVersion::Revival102h;
        case REVIVAL_TS_102I: return EfzRevivalVersion::Revival102i;
        default: return EfzRevivalVersion::Unknown;
    }
}

static BOOL CALLBACK EnumWindowsProcFindSelf(HWND hwnd, LPARAM lParam) {
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
        *reinterpret_cast<HWND*>(lParam) = hwnd;
        return FALSE; // stop
    }
    return TRUE; // continue
}

static HWND FindMainWindowForSelf() {
    HWND found = nullptr;
    EnumWindows(EnumWindowsProcFindSelf, reinterpret_cast<LPARAM>(&found));
    return found;
}

static EfzRevivalVersion DetectEfzRevivalVersion() {
    static std::atomic<int> s_cached{-1}; // -1 = unresolved, else EfzRevivalVersion
    int v = s_cached.load(std::memory_order_acquire);
    if (v != -1) return static_cast<EfzRevivalVersion>(v);
    // Prefer module timestamp (stable across title/localization changes).
    EfzRevivalVersion byTs = DetectEfzRevivalVersionByTimestamp();
    if (byTs != EfzRevivalVersion::Unknown) {
        s_cached.store(static_cast<int>(byTs), std::memory_order_release);
        return byTs;
    }
    HWND hwnd = FindMainWindowForSelf();
    if (!hwnd) {
        return EfzRevivalVersion::Unknown; // keep unresolved to retry
    }
    wchar_t titleW[256] = {};
    if (GetWindowTextW(hwnd, titleW, _countof(titleW)) <= 0) {
        return EfzRevivalVersion::Unknown; // retry later
    }
    // narrow to lower
    int need = WideCharToMultiByte(CP_UTF8, 0, titleW, -1, nullptr, 0, nullptr, nullptr);
    std::string t(need > 0 ? need - 1 : 0, '\0');
    if (need > 0) WideCharToMultiByte(CP_UTF8, 0, titleW, -1, t.data(), need, nullptr, nullptr);
    std::string lower = t; std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    EfzRevivalVersion ver = EfzRevivalVersion::Vanilla;
    if (lower.find("-revival-") != std::string::npos) {
        if (lower.find("1.02e") != std::string::npos) ver = EfzRevivalVersion::Revival102e;
        else if (lower.find("1.02f") != std::string::npos) ver = EfzRevivalVersion::Revival102f;
        else if (lower.find("1.02g") != std::string::npos) ver = EfzRevivalVersion::Revival102g;
        else if (lower.find("1.02h") != std::string::npos) ver = EfzRevivalVersion::Revival102h;
        else if (lower.find("1.02i") != std::string::npos) ver = EfzRevivalVersion::Revival102i;
        else ver = EfzRevivalVersion::Other;
    } else {
        ver = EfzRevivalVersion::Vanilla;
    }
    s_cached.store(static_cast<int>(ver), std::memory_order_release);
    return ver;
}

static uintptr_t RevivalWinsBaseRva() {
    // Optional override for diagnostics: EFZDA_WINS_BASE_RVA (hex or decimal)
    wchar_t buf[32]; DWORD n = GetEnvironmentVariableW(L"EFZDA_WINS_BASE_RVA", buf, _countof(buf));
    if (n > 0) {
        wchar_t* end = nullptr; unsigned long v = wcstoul(buf, &end, 0);
        if (v > 0 && v < 0x1000000) return (uintptr_t)v;
    }
    EfzRevivalVersion v = DetectEfzRevivalVersion();
    switch (v) {
        case EfzRevivalVersion::Revival102e: return 0x00A02CC;
        case EfzRevivalVersion::Revival102f: return 0x00A02CC;
        case EfzRevivalVersion::Revival102g: return 0x00A02CC;
        case EfzRevivalVersion::Revival102h: return 0x00A02EC;
        case EfzRevivalVersion::Revival102i: return 0x00A15F8;
        default: return 0; // Vanilla/Other/Unknown
    }
}

static uintptr_t RevivalOnlineStateRva() {
    // Optional override for diagnostics: EFZDA_ONLINE_STATE_RVA (hex or decimal)
    wchar_t buf[32]; DWORD n = GetEnvironmentVariableW(L"EFZDA_ONLINE_STATE_RVA", buf, _countof(buf));
    if (n > 0) {
        wchar_t* end = nullptr; unsigned long v = wcstoul(buf, &end, 0);
        if (v > 0 && v < 0x1000000) return (uintptr_t)v;
    }
    EfzRevivalVersion v = DetectEfzRevivalVersion();
    switch (v) {
        case EfzRevivalVersion::Revival102e: return 0x00A05D0;
        case EfzRevivalVersion::Revival102f: return 0x00A05D0;
        case EfzRevivalVersion::Revival102g: return 0x00A05D0;
        case EfzRevivalVersion::Revival102h: return 0x00A05F0;
        case EfzRevivalVersion::Revival102i: return 0x00A15FC;
        default: return 0; // Vanilla/Other/Unknown
    }
}

// New pointer-chain based online-state: EfzRevival.dll+0x26A4 -> ptr; byte at ptr+offset
static uintptr_t RevivalOnlineStatePtrRva() {
    // Optional override: EFZDA_ONLINE_STATE_PTR_RVA
    wchar_t buf[32]; DWORD n = GetEnvironmentVariableW(L"EFZDA_ONLINE_STATE_PTR_RVA", buf, _countof(buf));
    if (n > 0) {
        wchar_t* end = nullptr; unsigned long v = wcstoul(buf, &end, 0);
        if (v > 0 && v < 0x1000000) return (uintptr_t)v;
    }
    return 0x000026A4; // from user's CE table
}

static uintptr_t RevivalOnlineStateOffsetPrimary() {
    // Optional override: EFZDA_ONLINE_STATE_OFFSET
    wchar_t buf[32]; DWORD n = GetEnvironmentVariableW(L"EFZDA_ONLINE_STATE_OFFSET", buf, _countof(buf));
    if (n > 0) {
        wchar_t* end = nullptr; unsigned long v = wcstoul(buf, &end, 0);
        if (v > 0 && v < 0x10000) return (uintptr_t)v;
    }
    EfzRevivalVersion v = DetectEfzRevivalVersion();
    // 1.02i uses +0x37C, 1.02h/e use +0x370
    if (v == EfzRevivalVersion::Revival102i) return 0x37C;
    // Default to h/e
    return 0x370;
}

static uintptr_t RevivalOnlineStateOffsetAlternate() {
    // If primary is 0x37C (i), alt is 0x370; otherwise 0x37C
    uintptr_t p = RevivalOnlineStateOffsetPrimary();
    return (p == 0x37C) ? 0x370 : 0x37C;
}

// Version-aware current-player offset with optional environment override
static uintptr_t CurrentPlayerOffset() {
    // Optional override: EFZDA_CURRENT_PLAYER_OFFSET (hex or decimal)
    wchar_t buf[32]; DWORD n = GetEnvironmentVariableW(L"EFZDA_CURRENT_PLAYER_OFFSET", buf, _countof(buf));
    if (n > 0) {
        wchar_t* end = nullptr; unsigned long v = wcstoul(buf, &end, 0);
        if (v > 0 && v < 0x10000) return (uintptr_t)v;
    }
    EfzRevivalVersion v = DetectEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102i) return CURRENT_PLAYER_OFFSET_1_02i;
    // default to 1.02e/f/g/h layout for others (same observed layout for these fields)
    return CURRENT_PLAYER_OFFSET_1_02h;
}

// Version-aware NETPLAY win offsets (primary), with optional environment overrides
static uintptr_t NetP1WinOffset() {
    wchar_t buf[32]; DWORD n = GetEnvironmentVariableW(L"EFZDA_NET_P1_WIN_OFFSET", buf, _countof(buf));
    if (n > 0) { wchar_t* end=nullptr; unsigned long v=wcstoul(buf,&end,0); if (v>0 && v<0x10000) return (uintptr_t)v; }
    EfzRevivalVersion v = DetectEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102i) return P1_WIN_COUNT_OFFSET_1_02i;
    return P1_WIN_COUNT_OFFSET_1_02h;
}
static uintptr_t NetP2WinOffset() {
    wchar_t buf[32]; DWORD n = GetEnvironmentVariableW(L"EFZDA_NET_P2_WIN_OFFSET", buf, _countof(buf));
    if (n > 0) { wchar_t* end=nullptr; unsigned long v=wcstoul(buf,&end,0); if (v>0 && v<0x10000) return (uintptr_t)v; }
    EfzRevivalVersion v = DetectEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102i) return P2_WIN_COUNT_OFFSET_1_02i;
    return P2_WIN_COUNT_OFFSET_1_02h;
}

// Version-aware nickname offsets (primary), with optional environment overrides
static uintptr_t NickP1Offset() {
    wchar_t buf[32]; DWORD n = GetEnvironmentVariableW(L"EFZDA_P1_NICK_OFFSET", buf, _countof(buf));
    if (n > 0) { wchar_t* end=nullptr; unsigned long v=wcstoul(buf,&end,0); if (v>0 && v<0x10000) return (uintptr_t)v; }
    EfzRevivalVersion v = DetectEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102i) return P1_NICKNAME_OFFSET_1_02i;
    return P1_NICKNAME_OFFSET_1_02h;
}
static uintptr_t NickP2Offset() {
    wchar_t buf[32]; DWORD n = GetEnvironmentVariableW(L"EFZDA_P2_NICK_OFFSET", buf, _countof(buf));
    if (n > 0) { wchar_t* end=nullptr; unsigned long v=wcstoul(buf,&end,0); if (v>0 && v<0x10000) return (uintptr_t)v; }
    EfzRevivalVersion v = DetectEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102i) return P2_NICKNAME_OFFSET_1_02i;
    return P2_NICKNAME_OFFSET_1_02h;
}

// Version-aware tournament win offsets with optional environment overrides
static uintptr_t TournP1WinOffset() {
    wchar_t buf[32]; DWORD n = GetEnvironmentVariableW(L"EFZDA_TOURN_P1_WIN_OFFSET", buf, _countof(buf));
    if (n > 0) {
        wchar_t* end = nullptr; unsigned long v = wcstoul(buf, &end, 0);
        if (v > 0 && v < 0x10000) return (uintptr_t)v;
    }
    EfzRevivalVersion v = DetectEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102i) return P1_TOURN_WIN_COUNT_OFFSET_1_02i;
    // default for 1.02e/h and others
    return P1_TOURN_WIN_COUNT_OFFSET_1_02h;
}

static uintptr_t TournP2WinOffset() {
    wchar_t buf[32]; DWORD n = GetEnvironmentVariableW(L"EFZDA_TOURN_P2_WIN_OFFSET", buf, _countof(buf));
    if (n > 0) {
        wchar_t* end = nullptr; unsigned long v = wcstoul(buf, &end, 0);
        if (v > 0 && v < 0x10000) return (uintptr_t)v;
    }
    EfzRevivalVersion v = DetectEfzRevivalVersion();
    if (v == EfzRevivalVersion::Revival102i) return P2_TOURN_WIN_COUNT_OFFSET_1_02i;
    return P2_TOURN_WIN_COUNT_OFFSET_1_02h;
}

static uintptr_t get_game_state_ptr(uintptr_t efzBase) {
    if (!efzBase) return 0;
    uintptr_t gameStatePtr = 0;
    if (!safe_read(reinterpret_cast<void*>(efzBase + EFZ_BASE_OFFSET_GAME_STATE), gameStatePtr)) return 0;
    return gameStatePtr;
}

static void probe_game_state_region(uintptr_t gameStatePtr) {
    if (!gameStatePtr) return;
    // Dump a small window around GAME_MODE_OFFSET to help identify scene/menu flags
    const size_t start = (GAME_MODE_OFFSET > 0x80) ? (GAME_MODE_OFFSET - 0x80) : 0;
    const size_t span = 0x140; // 320 bytes window
    unsigned char buf[0x200] = {};
    size_t toRead = span < sizeof(buf) ? span : sizeof(buf);
    if (!read_bytes_no_log(reinterpret_cast<void*>(gameStatePtr + start), buf, toRead)) return;
    // Print 16 bytes per line
    for (size_t i = 0; i < toRead; i += 16) {
        char line[128];
        int n = std::snprintf(line, sizeof(line), "[probe] gs+0x%04zX:", start + i);
        std::string out(line, line + (n > 0 ? (size_t)n : 0));
        for (size_t j = 0; j < 16 && i + j < toRead; ++j) {
            char b[4];
            std::snprintf(b, sizeof(b), " %02X", buf[i + j]);
            out += b;
        }
        efzda::log("%s", out.c_str());
    }
}

// Optional scene-based detection (offline only). Configure via environment:
// EFZDA_SCENE_OFFSET   = hex offset within game state struct (e.g., 0x135C)
// EFZDA_SCENE_MAINMENU = integer value for main menu scene
// EFZDA_SCENE_CHARSEL  = integer value for character select scene
static bool s_sceneCfgInit = false;
static uintptr_t s_sceneOffset = 0;
static int s_sceneMainMenu = -1;
static int s_sceneCharSel = -1;
// Prefer global screen index by default; can be disabled via EFZDA_USE_SCREEN_INDEX=0
static bool s_useGlobalScreen = true; // prefer global screen index over scene byte in game-state
// Defaults from Cheat Engine observations: 0=Title,1=CharSel,2=Loading,3=InGame,5=Win,6=Settings,8=Replay menu
static int s_screenTitle = 0;
static int s_screenCharSel = 1;
static int s_screenLoading = 2;
static int s_screenInGame = 3;
static int s_screenWin = 5;
static int s_screenSettings = 6;
static int s_screenReplayMenu = 8;

static void maybe_init_scene_cfg() {
    if (s_sceneCfgInit) return;
    s_sceneCfgInit = true;
    wchar_t buf[32];
    DWORD n;
    // Toggle to use global active-screen index (byte_790148) — default is ON; set to 0 to disable
    n = GetEnvironmentVariableW(L"EFZDA_USE_SCREEN_INDEX", buf, _countof(buf));
    if (n > 0) { s_useGlobalScreen = (wcstol(buf, nullptr, 0) != 0); }
    n = GetEnvironmentVariableW(L"EFZDA_SCENE_OFFSET", buf, _countof(buf));
    if (n > 0) {
        wchar_t* end = nullptr;
        unsigned long v = wcstoul(buf, &end, 0); // accepts 0x..
        if (v > 0 && v < 0x10000) s_sceneOffset = (uintptr_t)v;
    }
    n = GetEnvironmentVariableW(L"EFZDA_SCENE_MAINMENU", buf, _countof(buf));
    if (n > 0) {
        s_sceneMainMenu = (int)wcstol(buf, nullptr, 0);
    }
    n = GetEnvironmentVariableW(L"EFZDA_SCENE_CHARSEL", buf, _countof(buf));
    if (n > 0) {
        s_sceneCharSel = (int)wcstol(buf, nullptr, 0);
    }
    // Optional explicit mappings for the global screen index
    n = GetEnvironmentVariableW(L"EFZDA_SCREEN_TITLE", buf, _countof(buf));
    if (n > 0) s_screenTitle = (int)wcstol(buf, nullptr, 0);
    n = GetEnvironmentVariableW(L"EFZDA_SCREEN_CHARSEL", buf, _countof(buf));
    if (n > 0) s_screenCharSel = (int)wcstol(buf, nullptr, 0);
    n = GetEnvironmentVariableW(L"EFZDA_SCREEN_LOADING", buf, _countof(buf));
    if (n > 0) s_screenLoading = (int)wcstol(buf, nullptr, 0);
    n = GetEnvironmentVariableW(L"EFZDA_SCREEN_INGAME", buf, _countof(buf));
    if (n > 0) s_screenInGame = (int)wcstol(buf, nullptr, 0);
    n = GetEnvironmentVariableW(L"EFZDA_SCREEN_WIN", buf, _countof(buf));
    if (n > 0) s_screenWin = (int)wcstol(buf, nullptr, 0);
    n = GetEnvironmentVariableW(L"EFZDA_SCREEN_SETTINGS", buf, _countof(buf));
    if (n > 0) s_screenSettings = (int)wcstol(buf, nullptr, 0);
    n = GetEnvironmentVariableW(L"EFZDA_SCREEN_REPLAY_MENU", buf, _countof(buf));
    if (n > 0) s_screenReplayMenu = (int)wcstol(buf, nullptr, 0);
}

static bool read_scene_value(uintptr_t efzBase, uint8_t& outVal) {
    maybe_init_scene_cfg();
    if (!s_sceneOffset) return false;
    uintptr_t gsp = get_game_state_ptr(efzBase);
    if (!gsp) return false;
    return safe_read(reinterpret_cast<void*>(gsp + s_sceneOffset), outVal);
}

// Read the global active-screen index (not part of game-state struct), if enabled
static bool read_screen_index(uintptr_t efzBase, uint8_t& outVal) {
    maybe_init_scene_cfg();
    if (!s_useGlobalScreen || !efzBase) return false;
    uint8_t v = 0xFF;
    if (!safe_read(reinterpret_cast<void*>(efzBase + EFZ_GLOBAL_SCREEN_INDEX_OFFSET), v)) return false;
    outVal = v;
    static uint8_t s_lastLogged = 0xFF;
    if (outVal != s_lastLogged) {
        s_lastLogged = outVal;
        efzda::log("[tick=%llu] SCREEN index addr=%p val=%u", ticks(), (void*)(efzBase + EFZ_GLOBAL_SCREEN_INDEX_OFFSET), (unsigned)outVal);
    }
    return true;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w;
    w.resize(static_cast<size_t>(len ? len - 1 : 0));
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], len);
    return w;
}

static std::string sanitize_ascii(const char* buf, size_t maxLen) {
    // Ensure null-terminated, strip non-printables
    size_t n = 0;
    for (; n < maxLen && buf[n]; ++n) {}
    std::string s(buf, buf + n);
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7F; // control chars
    }), s.end());
    return s;
}

// Narrow a UTF-16 string to UTF-8
static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(need > 0 ? need - 1 : 0, '\0');
    if (need > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), need, nullptr, nullptr);
    return s;
}

// Simple nickname sanitization similar to efz_streaming
static std::wstring sanitize_nickname_w(const std::wstring& in, size_t maxLen = 20) {
    if (in.empty()) return L"";
    std::wstring out;
    out.reserve(in.size());
    size_t count = 0;
    bool lastWasSpace = false;
    for (wchar_t c : in) {
        if (c == L'\0') break;
        if (count >= maxLen) break;
        // Drop control characters; normalize common whitespace to a single space
        if (iswcntrl(c)) continue;
        if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
        if (iswspace(c)) {
            if (lastWasSpace) continue; // collapse consecutive spaces
            lastWasSpace = true;
            out.push_back(L' ');
            ++count;
            continue;
        }
        lastWasSpace = false;
        // Keep all remaining printable Unicode characters as-is (supports Japanese, etc.)
        out.push_back(c);
        ++count;
    }
    // Trim trailing space if present
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

static bool read_wide_string(void* addr, size_t maxChars, std::wstring& out) {
    if (!addr || maxChars == 0) return false;
    SIZE_T bytes = 0;
    std::wstring tmp;
    tmp.resize(maxChars);
    bool ok = (ReadProcessMemory(GetCurrentProcess(), addr, tmp.data(), maxChars * sizeof(wchar_t), &bytes) && bytes == maxChars * sizeof(wchar_t));
    if (!ok) {
        DWORD err = GetLastError();
        efzda::log("[tick=%llu] READWIDE fail @%p chars=%zu read=%zu err=%lu", ticks(), addr, maxChars, (size_t)bytes, (unsigned long)err);
        return false;
    }
    // trim at first null
    size_t n = 0; while (n < tmp.size() && tmp[n] != L'\0') ++n;
    tmp.resize(n);
    out = tmp;
    efzda::log("[tick=%llu] READWIDE ok @%p chars=%zu", ticks(), addr, n);
    return true;
}

static uintptr_t read_revival_ptr(uintptr_t revivalBase) {
    if (!revivalBase) return 0;
    uintptr_t rva = RevivalWinsBaseRva();
    if (!rva) return 0;
    uintptr_t basePtrAddr = revivalBase + rva;
    uintptr_t ptr = 0;
    if (!safe_read(reinterpret_cast<void*>(basePtrAddr), ptr)) return 0;
    return ptr;
}

static std::string read_nickname(uintptr_t revivalBase, uintptr_t primaryOff, uintptr_t spectatorOff) {
    uintptr_t ptr = read_revival_ptr(revivalBase);
    if (!ptr) return {};
    std::wstring w;
    // try primary (player) slot
    if (read_wide_string(reinterpret_cast<void*>(ptr + primaryOff), 26, w) && !w.empty()) {
        auto s = narrow(sanitize_nickname_w(w));
        if (!s.empty() && s != "Player" && s != "Player 1" && s != "Player 2") return s;
    }
    // fallback spectator mapping
    w.clear();
    if (read_wide_string(reinterpret_cast<void*>(ptr + spectatorOff), 26, w) && !w.empty()) {
        auto s = narrow(sanitize_nickname_w(w));
        if (!s.empty()) return s;
    }
    return {};
}

static int read_current_player_index(uintptr_t revivalBase) {
    uintptr_t ptr = read_revival_ptr(revivalBase);
    if (!ptr) return -1;
    int idx = -1;
    static bool s_logged = false;
    uintptr_t off = CurrentPlayerOffset();
    if (!s_logged) {
        s_logged = true;
        efzda::log("[tick=%llu] CURRENT_PLAYER offset=0x%lX (ver=%d)", ticks(), (unsigned long)off, (int)DetectEfzRevivalVersion());
    }
    if (!safe_read(reinterpret_cast<void*>(ptr + off), idx)) return -1;
    if (idx == 0 || idx == 1) return idx;
    return -1;
}

// Title-case helper
static std::string title_case(const std::string& s) {
    if (s.empty()) return s;
    std::string out = s;
    out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    for (size_t i = 1; i < out.size(); ++i) {
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    }
    return out;
}

// Build a stable key by lowercasing and stripping non-alphanumerics (so "nayuki(b)" -> "nayukib")
static std::string make_key(const std::string& s) {
    std::string k;
    k.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c)) k.push_back(static_cast<char>(std::tolower(c)));
    }
    return k;
}

// Special-case display names per EFZ conventions and notes
static std::string normalize_display_name(const std::string& rawLower) {
    static const std::unordered_map<std::string, std::string> overrides = {
        // ONE
        {"nagamori", "Mizuka Nagamori"},
        {"mizuka", "Unknown"},               // boss version reads as mizuka
        {"mizukab", "Unknown"},              // playable version reads as mizukab
        {"nanase", "Rumi Nanase"},
        {"exnanase", "Doppel Nanase"},
        {"akane", "Akane Satomura"},
        {"misaki", "Misaki Kawana"},
        {"mayu", "Mayu Shiina"},
        {"mio", "Mio Kouzuki"},
        {"ayu", "Ayu Tsukimiya"},
    // Nayuki variants per request
        {"nayuki", "Nayuki(Sleepy)"},
        {"nayukib", "Nayuki(Awake)"},
         {"neyuki", "Nayuki(Sleepy)"},
         {"akiko", "Akiko Minase"},
        {"makoto", "Makoto Sawatari"},
        {"shiori", "Shiori Misaka"},
        {"kaori", "Kaori Misaka"},
        {"mai", "Mai Kawasumi"},
        {"sayuri", "Sayuri Kurata"},
        {"minagi", "Minagi Tohno"},        
        {"kano", "Kano Kirishima"},
        {"misuzu", "Misuzu Kamio"},
        {"kanna", "Kanna"},                  
        {"ikumi", "Ikumi Amasawa"},      
        {"mishio", "Mishio Amano"}          
    };
    auto it = overrides.find(make_key(rawLower));
    if (it != overrides.end()) return it->second;
    return title_case(rawLower);
}

static std::string read_character_name(uintptr_t base, uintptr_t baseOffset) {
    // base is efz.exe module base; [base + baseOffset] -> ptr, then [ptr + CHARACTER_NAME_OFFSET] -> 12-byte ASCII name
    uintptr_t* pSlot = reinterpret_cast<uintptr_t*>(base + baseOffset);
    uintptr_t charStruct = 0;
    if (!safe_read(pSlot, charStruct) || charStruct == 0)
        return {};

    char raw[12] = {};
    if (!safe_read_bytes(reinterpret_cast<void*>(charStruct + CHARACTER_NAME_OFFSET), raw, sizeof(raw)))
        return {};
    std::string s = sanitize_ascii(raw, sizeof(raw));
    efzda::log("[tick=%llu] CHAR name raw='%s' sanitized='%s' base=%p slot=%p charStruct=%p nameAddr=%p", ticks(), s.c_str(), s.c_str(), (void*)base, (void*)pSlot, (void*)charStruct, (void*)(charStruct + CHARACTER_NAME_OFFSET));
    // Raw is typically lower-case; normalize for display
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    // Validate raw against known EFZ character identifiers to avoid sticky/garbage names
    static const std::unordered_set<std::string> kAllowedRaw = {
        "akane","akiko","ayu","doppel","exnanase","nanase","ikumi","kanna","kano","kaori","mai","makoto","mayu","minagi","mio","misaki","mishio","misuzu","nagamori","nayuki","nayukib","mizuka","mizukab","sayuri","shiori"
    };
    if (lower.empty() || lower.size() < 3 || lower.size() > 12 || kAllowedRaw.find(lower) == kAllowedRaw.end()) {
        efzda::log("[tick=%llu] CHAR name rejected as invalid/raw='%s'", ticks(), lower.c_str());
        return {};
    }
    auto disp = normalize_display_name(lower);
    efzda::log("[tick=%llu] CHAR name display='%s'", ticks(), disp.c_str());
    return disp;
}

static int read_win_count(uintptr_t revivalBase, uintptr_t offsetPrimary, uintptr_t offsetSpectator) {
    if (!revivalBase) return 0;
    // Use the version-aware RevivalWinsBaseRva via read_revival_ptr
    uintptr_t winsBase = read_revival_ptr(revivalBase);
    if (!winsBase) return 0;

    int val = 0;
    if (safe_read(reinterpret_cast<void*>(winsBase + offsetPrimary), val)) {
        if (val >= 0 && val <= 99)
            return val;
    }
    // fallback to spectator offsets
    val = 0;
    if (safe_read(reinterpret_cast<void*>(winsBase + offsetSpectator), val)) {
        if (val >= 0 && val <= 99)
            return val;
    }
    efzda::log("[tick=%llu] WINS invalid/zero at base=%p primaryOff=0x%lX spectOff=0x%lX", ticks(), (void*)winsBase, (unsigned long)offsetPrimary, (unsigned long)offsetSpectator);
    return 0;
}

static uint8_t read_game_mode(uintptr_t efzBase) {
    if (!efzBase) return 0xFF;
    uintptr_t gameStatePtr = 0;
    if (!safe_read(reinterpret_cast<void*>(efzBase + EFZ_BASE_OFFSET_GAME_STATE), gameStatePtr) || !gameStatePtr)
        return 0xFF;
    uint8_t raw = 0xFF;
    safe_read(reinterpret_cast<void*>(gameStatePtr + GAME_MODE_OFFSET), raw);
    efzda::log("[tick=%llu] GAMEMODE base=%p gameStatePtr=%p addr=%p raw=%u", ticks(), (void*)efzBase, (void*)gameStatePtr, (void*)(gameStatePtr + GAME_MODE_OFFSET), (unsigned)raw);
    return raw;
}

static const char* game_mode_name(uint8_t raw) {
    switch (raw) {
        case 0: return "Arcade";
        case 1: return "Practice";
        case 3: return "VS CPU";
        case 4: return "VS Human";
        case 5: return "Replay";
        case 6: return "Auto-Replay";
        default: return nullptr;
    }
}

enum class OnlineState : int { Netplay = 0, Spectating = 1, Offline = 2, Tournament = 3, Unknown = -1 };

static OnlineState read_online_state(uintptr_t revivalBase) {
    if (!revivalBase) return OnlineState::Unknown;
    auto normalize = [](uint8_t x) -> OnlineState {
        switch (x) {
            case 0: return OnlineState::Netplay;
            case 1: return OnlineState::Spectating;
            case 2: return OnlineState::Offline;
            case 3: return OnlineState::Tournament;
            default: return OnlineState::Unknown;
        }
    };

    // Primary: pointer chain EfzRevival.dll+0x26A4 -> ptr; read byte at ptr+offset
    uintptr_t ptrRva = RevivalOnlineStatePtrRva();
    uintptr_t basePtr = 0;
    if (ptrRva && safe_read(reinterpret_cast<void*>(revivalBase + ptrRva), basePtr) && basePtr != 0) {
        uintptr_t off1 = RevivalOnlineStateOffsetPrimary();
        uint8_t v1 = 0xFF;
        if (safe_read(reinterpret_cast<void*>(basePtr + off1), v1)) {
            OnlineState s1 = normalize(v1);
            efzda::log("[tick=%llu] ONLINE(ptr) raw=%u basePtr=%p off=0x%lX", ticks(), (unsigned)v1, (void*)basePtr, (unsigned long)off1);
            if (s1 != OnlineState::Unknown) return s1;
            // 1.02i sometimes stores flags in higher bits; try low 2 bits
            if (DetectEfzRevivalVersion() == EfzRevivalVersion::Revival102i) {
                uint8_t m = (uint8_t)(v1 & 0x03);
                OnlineState sm = normalize(m);
                if (sm != OnlineState::Unknown) {
                    efzda::log("[tick=%llu] ONLINE(ptr) masked low2 raw=%u -> %u", ticks(), (unsigned)v1, (unsigned)m);
                    return sm;
                }
            }
        }
        // Try alternate offset (0x370 vs 0x37C)
        uintptr_t off2 = RevivalOnlineStateOffsetAlternate();
        uint8_t v2 = 0xFF;
        if (safe_read(reinterpret_cast<void*>(basePtr + off2), v2)) {
            OnlineState s2 = normalize(v2);
            efzda::log("[tick=%llu] ONLINE(ptr-alt) raw=%u basePtr=%p off=0x%lX", ticks(), (unsigned)v2, (void*)basePtr, (unsigned long)off2);
            if (s2 != OnlineState::Unknown) return s2;
            if (DetectEfzRevivalVersion() == EfzRevivalVersion::Revival102i) {
                uint8_t m2 = (uint8_t)(v2 & 0x03);
                OnlineState sm2 = normalize(m2);
                if (sm2 != OnlineState::Unknown) {
                    efzda::log("[tick=%llu] ONLINE(ptr-alt) masked low2 raw=%u -> %u", ticks(), (unsigned)v2, (unsigned)m2);
                    return sm2;
                }
            }
        }
    } else {
        efzda::log("[tick=%llu] ONLINE ptr base read failed ptrRva=0x%lX", ticks(), (unsigned long)ptrRva);
    }

    // Fallback: direct RVA locations used previously
    uintptr_t rva = RevivalOnlineStateRva();
    if (rva) {
        uint8_t v = 0xFF;
        if (safe_read(reinterpret_cast<void*>(revivalBase + rva), v)) {
            OnlineState s = normalize(v);
            efzda::log("[tick=%llu] ONLINE(direct) raw=%u addr=%p", ticks(), (unsigned)v, (void*)(revivalBase + rva));
            if (s != OnlineState::Unknown) return s;
        }
    }
    return OnlineState::Unknown;
}

static const char* online_state_name(OnlineState s) {
    switch (s) {
        case OnlineState::Netplay: return "Netplay";
        case OnlineState::Spectating: return "Spectating";
        case OnlineState::Offline: return "Offline";
        case OnlineState::Tournament: return "Tournament";
        default: return nullptr;
    }
}

// ---- efz_netplay_mod exported state (shared memory / DLL export) ----
constexpr uint32_t EFZ_NETPLAY_STATE_LEGACY_MIN_V1_SIZE = 204u; // pre-lastUpdateTick legacy layout

struct NetplayExportState {
    bool valid = false;
    bool fromSharedMemory = false;
    bool fromDllExport = false;
    bool hasCapabilityFlags = false;
    bool hasActivityPhase = false;
    bool hasEndReason = false;
    bool hasCharSelectContext = false;
    bool hasMatchContext = false;
    uint32_t version = 0;
    uint32_t structSize = 0;
    uint32_t lastUpdateTick = 0;
    uint32_t capabilityFlags = 0;
    uint32_t stateSeq = 0;
    uint32_t sessionId = 0;
    uint32_t setId = 0;
    int32_t sessionMode = 0;
    int32_t sessionPhase = 0;
    int32_t localSide = -1;
    int32_t p1Wins = 0;
    int32_t p2Wins = 0;
    int32_t matchCounter = 0;
    int32_t pingMs = -1;
    int32_t rollbackFrames = -1;
    bool inNetplayMenu = false;
    uint8_t netplayMenuScreen = 0;
    uint8_t netplayMenuDetail = EFZ_MENU_DETAIL_NONE;
    bool inNetplayCharacterSelect = false;
    bool inNetplayMatch = false;
    uint8_t activityPhase = EFZ_ACTIVITY_IDLE;
    uint8_t endReason = EFZ_END_NONE;
    uint8_t p1CharId = 0xFF;
    uint8_t p2CharId = 0xFF;
    bool p1Locked = false;
    bool p2Locked = false;
    uint8_t localCursorCharId = 0xFF;
    uint8_t stageId = 0xFF;
    uint8_t roundIndex = 0xFF;
    bool isRoundActive = false;
    uint16_t roundTimerFrames = 0xFFFF;
    std::string localNickname;
    std::string p1Name;
    std::string p2Name;
    std::string revivalVersion;
};

// v2-v4 layout used before the menu ABI refresh in v6. The v6 public header is
// now the default, so older export layouts are decoded through this compat
// struct instead of relying on a single memcpy path.
struct EFZNetplayStateCompatV4 {
    uint32_t magic;
    uint32_t version;
    uint32_t structSize;
    uint32_t lastUpdateTick;
    int32_t sessionMode;
    int32_t sessionPhase;
    int32_t localSide;
    int32_t p1Wins;
    int32_t p2Wins;
    int32_t matchCounter;
    char localNickname[32];
    char p1Name[64];
    char p2Name[64];
    int32_t pingMs;
    int32_t rollbackFrames;
    uint8_t inNetplayMenu;
    uint8_t netplayMenuScreen;
    uint8_t pad0[2];
    char revivalVersion[16];
    uint8_t inNetplayCharacterSelect;
    uint8_t inNetplayMatch;
    uint8_t pad1[2];
    uint32_t capabilityFlags;
    uint32_t stateSeq;
    uint32_t sessionId;
    uint32_t setId;
    uint8_t activityPhase;
    uint8_t endReason;
    uint8_t pad2[2];
    uint8_t p1CharId;
    uint8_t p2CharId;
    uint8_t p1Locked;
    uint8_t p2Locked;
    uint8_t localCursorCharId;
    uint8_t pad3[3];
    uint8_t stageId;
    uint8_t roundIndex;
    uint8_t isRoundActive;
    uint8_t pad4;
    uint16_t roundTimerFrames;
    uint8_t pad5[2];
};

static HANDLE s_npMapHandle = nullptr;
static const unsigned char* s_npMapView = nullptr;
static ULONGLONG s_npLastOpenAttempt = 0;

static void close_netplay_state_map() {
    if (s_npMapView) {
        UnmapViewOfFile(s_npMapView);
        s_npMapView = nullptr;
    }
    if (s_npMapHandle) {
        CloseHandle(s_npMapHandle);
        s_npMapHandle = nullptr;
    }
}

static HMODULE find_netplay_mod() {
    HMODULE mod = GetModuleHandleA("efz_netplay_mod");
    if (!mod) mod = GetModuleHandleA("efz_netplay_mod.dll");
    return mod;
}

static bool ensure_netplay_state_map_open() {
    if (s_npMapView) return true;
    ULONGLONG now = GetTickCount64();
    if (now - s_npLastOpenAttempt < 1000ULL) return false;
    s_npLastOpenAttempt = now;

    HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, EFZ_NETPLAY_STATE_SHM_NAME);
    if (!hMap) return false;
    void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!view) {
        CloseHandle(hMap);
        return false;
    }
    s_npMapHandle = hMap;
    s_npMapView = reinterpret_cast<const unsigned char*>(view);
    return true;
}

static bool read_u32(const unsigned char* p, size_t off, uint32_t& out) {
    if (!p) return false;
    std::memcpy(&out, p + off, sizeof(out));
    return true;
}

static bool read_i32_from_struct(const unsigned char* base, uint32_t structSize, size_t off, int32_t& out) {
    if (!base || off + sizeof(int32_t) > structSize) return false;
    std::memcpy(&out, base + off, sizeof(out));
    return true;
}

static std::string sanitize_cstr(const char* p, size_t maxLen) {
    if (!p || maxLen == 0) return {};
    size_t len = 0;
    while (len < maxLen && p[len] != '\0') ++len;
    std::string s(p, p + len);
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) { return c < 0x20; }), s.end());
    return s;
}

static std::string read_cstr_from_struct(const unsigned char* base, uint32_t structSize, size_t off, size_t maxLen) {
    if (!base || off >= structSize || maxLen == 0) return {};
    size_t n = static_cast<size_t>(structSize) - off;
    if (n > maxLen) n = maxLen;
    return sanitize_cstr(reinterpret_cast<const char*>(base + off), n);
}

static uint8_t normalize_legacy_menu_screen(uint8_t legacyScreen) {
    switch (legacyScreen) {
        case 0: return EFZ_MENU_MAIN;
        case 1: return EFZ_MENU_HOST;
        case 2: return EFZ_MENU_JOIN;
        case 3: return EFZ_MENU_OPTIONS; // old Nickname menu
        case 4: return EFZ_MENU_LOBBY;
        default: return legacyScreen;
    }
}

static bool parse_netplay_export_state_v6(const unsigned char* base, uint32_t structSize, uint32_t version32, NetplayExportState& out) {
    EFZNetplayState typed{};
    const size_t toCopy = (std::min)(static_cast<size_t>(structSize), sizeof(EFZNetplayState));
    std::memcpy(&typed, base, toCopy);

    out.version = version32;
    out.structSize = structSize;
    out.lastUpdateTick = typed.lastUpdateTick;
    out.sessionMode = typed.sessionMode;
    out.sessionPhase = typed.sessionPhase;
    out.localSide = typed.localSide;
    out.p1Wins = typed.p1Wins;
    out.p2Wins = typed.p2Wins;
    out.matchCounter = typed.matchCounter;
    out.localNickname = sanitize_cstr(typed.localNickname, sizeof(typed.localNickname));
    out.p1Name = sanitize_cstr(typed.p1Name, sizeof(typed.p1Name));
    out.p2Name = sanitize_cstr(typed.p2Name, sizeof(typed.p2Name));
    out.pingMs = typed.pingMs;
    out.rollbackFrames = typed.rollbackFrames;

    if (structSize >= offsetof(EFZNetplayState, inNetplayMenu) + sizeof(typed.inNetplayMenu)) {
        out.inNetplayMenu = (typed.inNetplayMenu != 0);
    }
    if (structSize >= offsetof(EFZNetplayState, netplayMenuScreen) + sizeof(typed.netplayMenuScreen)) {
        out.netplayMenuScreen = typed.netplayMenuScreen;
    }
    if (structSize >= offsetof(EFZNetplayState, netplayMenuDetail) + sizeof(typed.netplayMenuDetail)) {
        out.netplayMenuDetail = typed.netplayMenuDetail;
    }
    if (structSize >= offsetof(EFZNetplayState, revivalVersion) + sizeof(typed.revivalVersion)) {
        out.revivalVersion = sanitize_cstr(typed.revivalVersion, sizeof(typed.revivalVersion));
    }
    if (structSize >= offsetof(EFZNetplayState, inNetplayCharacterSelect) + sizeof(typed.inNetplayCharacterSelect)) {
        out.inNetplayCharacterSelect = (typed.inNetplayCharacterSelect != 0);
    }
    if (structSize >= offsetof(EFZNetplayState, inNetplayMatch) + sizeof(typed.inNetplayMatch)) {
        out.inNetplayMatch = (typed.inNetplayMatch != 0);
    }
    if (structSize >= offsetof(EFZNetplayState, capabilityFlags) + sizeof(typed.capabilityFlags)) {
        out.capabilityFlags = typed.capabilityFlags;
        out.hasCapabilityFlags = true;
    }
    if (structSize >= offsetof(EFZNetplayState, stateSeq) + sizeof(typed.stateSeq)) {
        out.stateSeq = typed.stateSeq;
    }
    if (structSize >= offsetof(EFZNetplayState, sessionId) + sizeof(typed.sessionId)) {
        out.sessionId = typed.sessionId;
    }
    if (structSize >= offsetof(EFZNetplayState, setId) + sizeof(typed.setId)) {
        out.setId = typed.setId;
    }
    if (structSize >= offsetof(EFZNetplayState, activityPhase) + sizeof(typed.activityPhase)) {
        out.activityPhase = typed.activityPhase;
        out.hasActivityPhase = true;
    }
    if (structSize >= offsetof(EFZNetplayState, endReason) + sizeof(typed.endReason)) {
        out.endReason = typed.endReason;
        out.hasEndReason = true;
    }
    if (structSize >= offsetof(EFZNetplayState, localCursorCharId) + sizeof(typed.localCursorCharId)) {
        out.p1CharId = typed.p1CharId;
        out.p2CharId = typed.p2CharId;
        out.p1Locked = (typed.p1Locked != 0);
        out.p2Locked = (typed.p2Locked != 0);
        out.localCursorCharId = typed.localCursorCharId;
        out.hasCharSelectContext = true;
    }
    if (structSize >= offsetof(EFZNetplayState, roundTimerFrames) + sizeof(typed.roundTimerFrames)) {
        out.stageId = typed.stageId;
        out.roundIndex = typed.roundIndex;
        out.isRoundActive = (typed.isRoundActive != 0);
        out.roundTimerFrames = typed.roundTimerFrames;
        out.hasMatchContext = true;
    }
    out.valid = true;
    return true;
}

static bool parse_netplay_export_state_v2_to_v4(const unsigned char* base, uint32_t structSize, uint32_t version32, NetplayExportState& out) {
    EFZNetplayStateCompatV4 typed{};
    const size_t toCopy = (std::min)(static_cast<size_t>(structSize), sizeof(EFZNetplayStateCompatV4));
    std::memcpy(&typed, base, toCopy);

    out.version = version32;
    out.structSize = structSize;
    out.lastUpdateTick = typed.lastUpdateTick;
    out.sessionMode = typed.sessionMode;
    out.sessionPhase = typed.sessionPhase;
    out.localSide = typed.localSide;
    out.p1Wins = typed.p1Wins;
    out.p2Wins = typed.p2Wins;
    out.matchCounter = typed.matchCounter;
    out.localNickname = sanitize_cstr(typed.localNickname, sizeof(typed.localNickname));
    out.p1Name = sanitize_cstr(typed.p1Name, sizeof(typed.p1Name));
    out.p2Name = sanitize_cstr(typed.p2Name, sizeof(typed.p2Name));
    out.pingMs = typed.pingMs;
    out.rollbackFrames = typed.rollbackFrames;

    if (structSize >= offsetof(EFZNetplayStateCompatV4, inNetplayMenu) + sizeof(typed.inNetplayMenu)) {
        out.inNetplayMenu = (typed.inNetplayMenu != 0);
    }
    if (structSize >= offsetof(EFZNetplayStateCompatV4, netplayMenuScreen) + sizeof(typed.netplayMenuScreen)) {
        out.netplayMenuScreen = normalize_legacy_menu_screen(typed.netplayMenuScreen);
    }
    if (structSize >= offsetof(EFZNetplayStateCompatV4, revivalVersion) + sizeof(typed.revivalVersion)) {
        out.revivalVersion = sanitize_cstr(typed.revivalVersion, sizeof(typed.revivalVersion));
    }
    if (structSize >= offsetof(EFZNetplayStateCompatV4, inNetplayCharacterSelect) + sizeof(typed.inNetplayCharacterSelect)) {
        out.inNetplayCharacterSelect = (typed.inNetplayCharacterSelect != 0);
    }
    if (structSize >= offsetof(EFZNetplayStateCompatV4, inNetplayMatch) + sizeof(typed.inNetplayMatch)) {
        out.inNetplayMatch = (typed.inNetplayMatch != 0);
    }
    if (structSize >= offsetof(EFZNetplayStateCompatV4, capabilityFlags) + sizeof(typed.capabilityFlags)) {
        out.capabilityFlags = typed.capabilityFlags;
        out.hasCapabilityFlags = true;
    }
    if (structSize >= offsetof(EFZNetplayStateCompatV4, stateSeq) + sizeof(typed.stateSeq)) {
        out.stateSeq = typed.stateSeq;
    }
    if (structSize >= offsetof(EFZNetplayStateCompatV4, sessionId) + sizeof(typed.sessionId)) {
        out.sessionId = typed.sessionId;
    }
    if (structSize >= offsetof(EFZNetplayStateCompatV4, setId) + sizeof(typed.setId)) {
        out.setId = typed.setId;
    }
    if (structSize >= offsetof(EFZNetplayStateCompatV4, activityPhase) + sizeof(typed.activityPhase)) {
        out.activityPhase = typed.activityPhase;
        out.hasActivityPhase = true;
    }
    if (structSize >= offsetof(EFZNetplayStateCompatV4, endReason) + sizeof(typed.endReason)) {
        out.endReason = typed.endReason;
        out.hasEndReason = true;
    }
    if (structSize >= offsetof(EFZNetplayStateCompatV4, localCursorCharId) + sizeof(typed.localCursorCharId)) {
        out.p1CharId = typed.p1CharId;
        out.p2CharId = typed.p2CharId;
        out.p1Locked = (typed.p1Locked != 0);
        out.p2Locked = (typed.p2Locked != 0);
        out.localCursorCharId = typed.localCursorCharId;
        out.hasCharSelectContext = true;
    }
    if (structSize >= offsetof(EFZNetplayStateCompatV4, roundTimerFrames) + sizeof(typed.roundTimerFrames)) {
        out.stageId = typed.stageId;
        out.roundIndex = typed.roundIndex;
        out.isRoundActive = (typed.isRoundActive != 0);
        out.roundTimerFrames = typed.roundTimerFrames;
        out.hasMatchContext = true;
    }
    out.valid = true;
    return true;
}

static bool parse_netplay_export_state_v1_legacy(const unsigned char* base, uint32_t structSize, uint32_t version32, NetplayExportState& out) {
    // Legacy layout from NETPLAY_STATE_EXPORT.md before lastUpdateTick field:
    // header (12), then sessionMode at +12.
    if (structSize < EFZ_NETPLAY_STATE_LEGACY_MIN_V1_SIZE) return false;

    out.version = version32;
    out.structSize = structSize;
    if (!read_i32_from_struct(base, structSize, 12, out.sessionMode)) return false;
    if (!read_i32_from_struct(base, structSize, 16, out.sessionPhase)) return false;
    if (!read_i32_from_struct(base, structSize, 20, out.localSide)) return false;
    if (!read_i32_from_struct(base, structSize, 24, out.p1Wins)) return false;
    if (!read_i32_from_struct(base, structSize, 28, out.p2Wins)) return false;
    if (!read_i32_from_struct(base, structSize, 32, out.matchCounter)) return false;
    out.localNickname = read_cstr_from_struct(base, structSize, 36, 32);
    out.p1Name = read_cstr_from_struct(base, structSize, 68, 64);
    out.p2Name = read_cstr_from_struct(base, structSize, 132, 64);
    (void)read_i32_from_struct(base, structSize, 196, out.pingMs);
    (void)read_i32_from_struct(base, structSize, 200, out.rollbackFrames);
    if (204 < structSize) out.inNetplayMenu = (base[204] != 0);
    if (205 < structSize) out.netplayMenuScreen = normalize_legacy_menu_screen(base[205]);
    if (206 < structSize) out.revivalVersion = read_cstr_from_struct(base, structSize, 206, 16);
    out.valid = true;
    return true;
}

static bool parse_netplay_export_state(const void* statePtr, NetplayExportState& out) {
    out = NetplayExportState{};
    if (!statePtr) return false;

    const unsigned char* base = reinterpret_cast<const unsigned char*>(statePtr);
    uint32_t magic = 0;
    uint32_t version32 = 0;
    uint32_t structSize = 0;
    if (!read_u32(base, 0, magic)) return false;
    if (magic != EFZ_NETPLAY_STATE_MAGIC) return false;
    if (!read_u32(base, 4, version32)) return false;
    if (!read_u32(base, 8, structSize)) return false;
    if (structSize < EFZ_NETPLAY_STATE_LEGACY_MIN_V1_SIZE || structSize > 4096) return false;

    // v6 changed the layout in-place (larger localNickname and menu detail).
    // Keep explicit parsers for both the refreshed ABI and the older v2-v4 ABI.
    if (version32 >= 6) {
        if (structSize >= offsetof(EFZNetplayState, rollbackFrames) + sizeof(int32_t)) {
            if (parse_netplay_export_state_v6(base, structSize, version32, out)) return true;
        }
    } else {
        if (structSize >= offsetof(EFZNetplayStateCompatV4, rollbackFrames) + sizeof(int32_t)) {
            if (parse_netplay_export_state_v2_to_v4(base, structSize, version32, out)) return true;
        }
    }
    return parse_netplay_export_state_v1_legacy(base, structSize, version32, out);
}

using NetplayGetStateFn = const void* (__cdecl *)(void);

static bool read_netplay_export_state(NetplayExportState& out) {
    // Preferred path: named shared memory.
    if (ensure_netplay_state_map_open()) {
        if (parse_netplay_export_state(s_npMapView, out)) {
            out.fromSharedMemory = true;
            return true;
        }
    }

    // Fallback path: exported function.
    HMODULE mod = find_netplay_mod();
    if (mod) {
        auto fn = reinterpret_cast<NetplayGetStateFn>(GetProcAddress(mod, "EFZNetplay_GetState"));
        if (fn) {
            const void* p = fn();
            if (parse_netplay_export_state(p, out)) {
                out.fromDllExport = true;
                return true;
            }
        }
    } else {
        // If netplay mod is gone, close stale map handle so we can re-open cleanly later.
        close_netplay_state_map();
    }

    return false;
}

static const char* netplay_menu_screen_name(uint8_t id) {
    switch (id) {
        case EFZ_MENU_MAIN: return "Main menu";
        case EFZ_MENU_HOST: return "Host menu";
        case EFZ_MENU_JOIN: return "Join menu";
        case EFZ_MENU_PLAYER_ROOMS: return "Player Rooms";
        case EFZ_MENU_OPTIONS: return "Options";
        case EFZ_MENU_LOBBY: return "Lobby";
        case EFZ_MENU_BATTLE_LOG: return "Battle Log";
        default: return "Netplay menu";
    }
}

static const char* netplay_menu_detail_name(uint8_t screen, uint8_t detail) {
    switch (screen) {
        case EFZ_MENU_PLAYER_ROOMS:
            switch (detail) {
                case EFZ_MENU_DETAIL_PLAYER_ROOMS_ROOT: return "Rooms";
                case EFZ_MENU_DETAIL_PLAYER_ROOMS_JOIN: return "Join";
                case EFZ_MENU_DETAIL_PLAYER_ROOMS_CREATE: return "Create";
                default: return nullptr;
            }
        case EFZ_MENU_OPTIONS:
            switch (detail) {
                case EFZ_MENU_DETAIL_OPTIONS_ROOT: return "Categories";
                case EFZ_MENU_DETAIL_OPTIONS_CATEGORY: return "Category";
                case EFZ_MENU_DETAIL_OPTIONS_EDIT: return "Editing";
                case EFZ_MENU_DETAIL_OPTIONS_MODAL: return "Modal";
                default: return nullptr;
            }
        case EFZ_MENU_BATTLE_LOG:
            switch (detail) {
                case EFZ_MENU_DETAIL_BATTLE_LOG_SUMMARY_PROFILE: return "Profile summary";
                case EFZ_MENU_DETAIL_BATTLE_LOG_SUMMARY_FULL: return "Full summary";
                case EFZ_MENU_DETAIL_BATTLE_LOG_SUMMARY_SEARCH: return "Search summary";
                case EFZ_MENU_DETAIL_BATTLE_LOG_BROWSER: return "Browser";
                case EFZ_MENU_DETAIL_BATTLE_LOG_FILTERS: return "Filters";
                case EFZ_MENU_DETAIL_BATTLE_LOG_SET_DETAILS: return "Set details";
                default: return nullptr;
            }
        default:
            return nullptr;
    }
}

static std::string format_netplay_menu_state(const NetplayExportState& state) {
    std::string out = netplay_menu_screen_name(state.netplayMenuScreen);
    if (const char* detail = netplay_menu_detail_name(state.netplayMenuScreen, state.netplayMenuDetail)) {
        out += " - ";
        out += detail;
    }
    return out;
}

static const char* netplay_phase_name(int32_t phase) {
    switch (phase) {
        case EFZ_PHASE_IDLE: return "Idle";
        case EFZ_PHASE_CONNECTING: return "Connecting";
        case EFZ_PHASE_DELAY_SETUP: return "Delay setup";
        case EFZ_PHASE_CONNECTED: return "Connected";
        case EFZ_PHASE_FAILED: return "Failed";
        case EFZ_PHASE_SESSION_ENDED: return "Session ended";
        default: return "Unknown";
    }
}

static const char* netplay_mode_name(int32_t mode) {
    switch (mode) {
        case EFZ_SESSION_NONE: return "None";
        case EFZ_SESSION_HOSTING: return "Hosting";
        case EFZ_SESSION_JOINING: return "Joining";
        case EFZ_SESSION_SPECTATING: return "Spectating";
        case EFZ_SESSION_TOURNAMENT: return "Tournament";
        default: return "Unknown";
    }
}

static const char* netplay_activity_name(uint8_t activity) {
    switch (activity) {
        case EFZ_ACTIVITY_IDLE: return "Idle";
        case EFZ_ACTIVITY_MENU: return "Menu";
        case EFZ_ACTIVITY_CONNECTING: return "Connecting";
        case EFZ_ACTIVITY_DELAY_SETUP: return "Delay setup";
        case EFZ_ACTIVITY_CHAR_SELECT: return "Character select";
        case EFZ_ACTIVITY_LOADING: return "Loading";
        case EFZ_ACTIVITY_MATCH: return "Match";
        case EFZ_ACTIVITY_RESULTS: return "Results";
        default: return "Unknown";
    }
}

static const char* netplay_end_reason_name(uint8_t reason) {
    switch (reason) {
        case EFZ_END_NONE: return "None";
        case EFZ_END_GRACEFUL: return "Graceful";
        case EFZ_END_DISCONNECT: return "Disconnected";
        case EFZ_END_CANCELLED: return "Cancelled";
        case EFZ_END_CONNECT_FAILED: return "Connect failed";
        case EFZ_END_PEER_PROCESS_DIED: return "Peer process ended";
        case EFZ_END_UNKNOWN: return "Unknown";
        default: return "Unknown";
    }
}

// Map display character name to Discord small image asset key (Dev Portal)
static std::string map_char_to_small_icon_key(const std::string& displayName) {
    // Lowercase copy
    std::string s = displayName;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    // Extract first token (up to space or '(')
    std::string first;
    for (char c : s) {
        if (c == ' ' || c == '(') break;
        if ((c >= 'a' && c <= 'z')) first.push_back(c);
    }
    // Flags for Nayuki variants
    bool isSleepy = s.find("(sleepy)") != std::string::npos;

    // Exact first-name mapping only
    if (first == "nayuki") return isSleepy ? "90px-efz_neyuki_icon" : "90px-efz_nayuki_icon";
    if (first == "doppel") return "90px-efz_doppel_icon";
    if (first == "rumi" || first == "nanase") return "90px-efz_rumi_icon";
    if (first == "akane") return "90px-efz_akane_icon";
    if (first == "akiko") return "90px-efz_akiko_icon";
    if (first == "ayu")   return "90px-efz_ayu_icon";
    if (first == "ikumi") return "90px-efz_ikumi_icon";
    if (first == "kanna") return "90px-efz_kanna_icon_-_copy";
    if (first == "kano")  return "90px-efz_kano_icon";
    if (first == "kaori") return "90px-efz_kaori_icon";
    if (first == "mai")   return "90px-efz_mai_icon";
    if (first == "makoto")return "90px-efz_makoto_icon";
    if (first == "mayu")  return "90px-efz_mayu_icon";
    if (first == "minagi")return "90px-efz_minagi_icon";
    if (first == "mio")   return "90px-efz_mio_icon";
    if (first == "misaki")return "90px-efz_misaki_icon";
    if (first == "mishio")return "90px-efz_mishio_icon";
    if (first == "misuzu")return "90px-efz_misuzu_icon";
    if (first == "mizuka")return "90px-efz_mizuka_icon"; // Mizuka Nagamori (not Unknown)
    if (first == "sayuri")return "90px-efz_sayuri_icon";
    if (first == "shiori")return "90px-efz_shiori_icon";
    if (first == "unknown") return "90px-efz_unknown_icon";

    // Specific reads that map to the Unknown character
    if (s == "unknown") return "90px-efz_unknown_icon";
    if (s.find("mizukab") != std::string::npos || s == "mizuka") return "90px-efz_unknown_icon";

    return {};
}

// For now large image uses same asset namespace as small when available.
static std::string map_char_to_large_image_key(const std::string& displayName) {
    // If you upload separate large assets, adjust mapping here.
    auto key = map_char_to_small_icon_key(displayName); // reuse icons if no large art
    return key; // no generic unknown fallback; unknown is a real character
}

} // namespace

GameState GameStateProvider::get() {
    GameState gs{};
    static unsigned long s_poll = 0;
    ++s_poll;
    // Sticky state across polls
    static uint8_t s_lastGmRaw = 0xFF;
    static std::string s_lastP1Name;
    static std::string s_lastP2Name;
    static uint8_t s_lastScreenIdx = 0xFF; // track screen transitions (Title/Charsel/etc.)
    // We now update presence immediately on mode change and update each character as soon as it becomes available (no global suppression)
    // Simple debounced spawn heuristic inspired by efz-training-mode
    static int s_spawnedFrames = 0;
    static int s_unspawnedFrames = 0;
    static bool s_waitOnlineNicknames = false;    // online reported but no nicknames yet
    // Export-side nickname cache to survive transient empty frames from netplay mod.
    static uint32_t s_exportNickSessionId = 0;
    static std::string s_exportP1NickCache;
    static std::string s_exportP2NickCache;

    // Determine module bases
    uintptr_t efzBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); // main module (efz.exe) in same process
    HMODULE revivalMod = GetModuleHandleW(L"EfzRevival.dll");
    uintptr_t revivalBase = reinterpret_cast<uintptr_t>(revivalMod);
    HMODULE netplayMod = find_netplay_mod();
    static bool s_lastNetplayModLoaded = false;
    bool netplayModLoaded = (netplayMod != nullptr);
    if (netplayModLoaded != s_lastNetplayModLoaded) {
        s_lastNetplayModLoaded = netplayModLoaded;
        log("GSPoll#%lu: efz_netplay_mod %s", s_poll, netplayModLoaded ? "detected" : "not detected");
        if (!netplayModLoaded) close_netplay_state_map();
    }
    // Allow disabling all EfzRevival usage via environment for debugging
    wchar_t disableEnv[8];
    if (GetEnvironmentVariableW(L"EFZDA_DISABLE_REVIVAL", disableEnv, _countof(disableEnv)) > 0) {
        revivalBase = 0;
    }

    log("GSPoll#%lu: efzBase=%p revivalBase=%p", s_poll, reinterpret_cast<void*>(efzBase), reinterpret_cast<void*>(revivalBase));

    if (!efzBase) {
        gs.details = "Idle";
        gs.state = "In Menus";
        return gs;
    }

    // Read current screen index early to detect transitions (e.g., 1->0 means back to Title)
    uint8_t topScreenIdx = 0xFF; bool haveTopScreen = read_screen_index(efzBase, topScreenIdx);
    if (haveTopScreen && topScreenIdx != s_lastScreenIdx) {
        // On entering Title/Main Menu or Character Select, clear stale names and spawn debounce immediately
        if (topScreenIdx == (uint8_t)s_screenTitle || topScreenIdx == (uint8_t)s_screenCharSel) {
            s_lastP1Name.clear(); s_lastP2Name.clear();
            s_spawnedFrames = 0; s_unspawnedFrames = 0;
        }
        s_lastScreenIdx = topScreenIdx;
    }

    std::string p1 = read_character_name(efzBase, EFZ_BASE_OFFSET_P1);
    std::string p2 = read_character_name(efzBase, EFZ_BASE_OFFSET_P2);
    // Also read raw character pointers to detect spawn state independently of name parsing
    uintptr_t p1Ptr = 0, p2Ptr = 0;
    safe_read(reinterpret_cast<void*>(efzBase + EFZ_BASE_OFFSET_P1), p1Ptr);
    safe_read(reinterpret_cast<void*>(efzBase + EFZ_BASE_OFFSET_P2), p2Ptr);
    bool rawSpawned = (p1Ptr != 0) && (p2Ptr != 0);
    if (rawSpawned) {
        int inc = s_spawnedFrames + 1; s_spawnedFrames = (inc > 60 ? 60 : inc); s_unspawnedFrames = 0;
    } else {
        int inc = s_unspawnedFrames + 1; s_unspawnedFrames = (inc > 60 ? 60 : inc); s_spawnedFrames = 0;
    }
    bool spawnedDebounced = (s_spawnedFrames >= 3);
    if (p1.size() > 32) p1.resize(32);
    if (p2.size() > 32) p2.resize(32);
    if (p1.empty() || p2.empty()) {
        log("GSPoll#%lu: char names p1='%s' p2='%s' (one or both empty)", s_poll, p1.c_str(), p2.c_str());
    } else {
        log("GSPoll#%lu: char names p1='%s' p2='%s'", s_poll, p1.c_str(), p2.c_str());
    }

    // Read game mode and online state
    uint8_t gmRaw = read_game_mode(efzBase);
    const char* gmName = game_mode_name(gmRaw);
    OnlineState onl = read_online_state(revivalBase);
    NetplayExportState np{};
    bool haveNetplayExport = read_netplay_export_state(np);
    static bool s_lastNetplayExport = false;
    static bool s_lastNetplayExportShared = false;
    static std::string s_lastNetplayRevivalVersion;
    if (haveNetplayExport != s_lastNetplayExport ||
        (haveNetplayExport && np.fromSharedMemory != s_lastNetplayExportShared)) {
        s_lastNetplayExport = haveNetplayExport;
        s_lastNetplayExportShared = haveNetplayExport ? np.fromSharedMemory : false;
        if (haveNetplayExport) {
            log("GSPoll#%lu: netplay state export active (source=%s ver=%u size=%u)",
                s_poll,
                np.fromSharedMemory ? "shared-memory" : "dll-export",
                (unsigned)np.version,
                (unsigned)np.structSize);
        } else {
            log("GSPoll#%lu: netplay state export unavailable", s_poll);
        }
    }
    if (haveNetplayExport && np.revivalVersion != s_lastNetplayRevivalVersion) {
        s_lastNetplayRevivalVersion = np.revivalVersion;
        log("GSPoll#%lu: netplay export revivalVersion='%s'",
            s_poll, s_lastNetplayRevivalVersion.c_str());
    }
    static bool s_npTransitionKnown = false;
    static int32_t s_npLastMode = 0;
    static int32_t s_npLastPhase = 0;
    static uint8_t s_npLastActivity = 0;
    static uint8_t s_npLastEndReason = 0;
    static bool s_npLastMenu = false;
    static uint8_t s_npLastMenuScreen = 0;
    static uint8_t s_npLastMenuDetail = EFZ_MENU_DETAIL_NONE;
    static bool s_npLastCharSelect = false;
    static bool s_npLastMatch = false;
    static uint32_t s_npLastSessionId = 0;
    static uint32_t s_npLastSetId = 0;
    static bool s_npSeqKnown = false;
    static uint32_t s_npLastSeqObserved = 0;
    static ULONGLONG s_npLastSeqChangeAt = 0;
    static bool s_npSeqWasStale = false;
    bool npStateLikelyStale = false;
    if (haveNetplayExport) {
        ULONGLONG nowTick = GetTickCount64();
        if (!s_npSeqKnown || np.stateSeq != s_npLastSeqObserved) {
            s_npSeqKnown = true;
            s_npLastSeqObserved = np.stateSeq;
            s_npLastSeqChangeAt = nowTick;
            if (s_npSeqWasStale) {
                log("GSPoll#%lu: netplay export state resumed (seq=%u)", s_poll, (unsigned)np.stateSeq);
            }
            s_npSeqWasStale = false;
        } else if (s_npSeqKnown && (nowTick - s_npLastSeqChangeAt) > 1500ULL) {
            npStateLikelyStale = true;
            if (!s_npSeqWasStale) {
                log("GSPoll#%lu: netplay export state appears stale (seq=%u unchanged for %llums)",
                    s_poll, (unsigned)np.stateSeq, (unsigned long long)(nowTick - s_npLastSeqChangeAt));
                s_npSeqWasStale = true;
            }
        }
        if (!s_npTransitionKnown) {
            log("NPTransition: init mode=%s phase=%s activity=%s end=%s menu=%d/%u/%u charsel=%d match=%d sid=%u set=%u",
                netplay_mode_name(np.sessionMode),
                netplay_phase_name(np.sessionPhase),
                netplay_activity_name(np.activityPhase),
                netplay_end_reason_name(np.endReason),
                np.inNetplayMenu ? 1 : 0,
                (unsigned)np.netplayMenuScreen,
                (unsigned)np.netplayMenuDetail,
                np.inNetplayCharacterSelect ? 1 : 0,
                np.inNetplayMatch ? 1 : 0,
                (unsigned)np.sessionId,
                (unsigned)np.setId);
            s_npTransitionKnown = true;
        } else {
            bool changed =
                np.sessionMode != s_npLastMode ||
                np.sessionPhase != s_npLastPhase ||
                np.activityPhase != s_npLastActivity ||
                np.endReason != s_npLastEndReason ||
                np.inNetplayMenu != s_npLastMenu ||
                np.netplayMenuScreen != s_npLastMenuScreen ||
                np.netplayMenuDetail != s_npLastMenuDetail ||
                np.inNetplayCharacterSelect != s_npLastCharSelect ||
                np.inNetplayMatch != s_npLastMatch ||
                np.sessionId != s_npLastSessionId ||
                np.setId != s_npLastSetId;
            if (changed) {
                log("NPTransition: mode %s -> %s | phase %s -> %s | activity %s -> %s | end %s -> %s | menu %d/%u/%u -> %d/%u/%u | charsel %d -> %d | match %d -> %d | sid %u -> %u | set %u -> %u",
                    netplay_mode_name(s_npLastMode), netplay_mode_name(np.sessionMode),
                    netplay_phase_name(s_npLastPhase), netplay_phase_name(np.sessionPhase),
                    netplay_activity_name(s_npLastActivity), netplay_activity_name(np.activityPhase),
                    netplay_end_reason_name(s_npLastEndReason), netplay_end_reason_name(np.endReason),
                    s_npLastMenu ? 1 : 0, (unsigned)s_npLastMenuScreen, (unsigned)s_npLastMenuDetail,
                    np.inNetplayMenu ? 1 : 0, (unsigned)np.netplayMenuScreen, (unsigned)np.netplayMenuDetail,
                    s_npLastCharSelect ? 1 : 0, np.inNetplayCharacterSelect ? 1 : 0,
                    s_npLastMatch ? 1 : 0, np.inNetplayMatch ? 1 : 0,
                    (unsigned)s_npLastSessionId, (unsigned)np.sessionId,
                    (unsigned)s_npLastSetId, (unsigned)np.setId);
            }
        }
        s_npLastMode = np.sessionMode;
        s_npLastPhase = np.sessionPhase;
        s_npLastActivity = np.activityPhase;
        s_npLastEndReason = np.endReason;
        s_npLastMenu = np.inNetplayMenu;
        s_npLastMenuScreen = np.netplayMenuScreen;
        s_npLastMenuDetail = np.netplayMenuDetail;
        s_npLastCharSelect = np.inNetplayCharacterSelect;
        s_npLastMatch = np.inNetplayMatch;
        s_npLastSessionId = np.sessionId;
        s_npLastSetId = np.setId;
    } else if (s_npTransitionKnown) {
        log("NPTransition: export lost; clearing transition baseline");
        s_npTransitionKnown = false;
        s_npSeqKnown = false;
        s_npSeqWasStale = false;
        s_npLastMenuDetail = EFZ_MENU_DETAIL_NONE;
    }
    if (haveNetplayExport && np.sessionMode != EFZ_SESSION_NONE) {
        if (np.sessionId != 0 && np.sessionId != s_exportNickSessionId) {
            s_exportNickSessionId = np.sessionId;
            s_exportP1NickCache.clear();
            s_exportP2NickCache.clear();
        }
        if (!np.p1Name.empty()) s_exportP1NickCache = np.p1Name;
        if (!np.p2Name.empty()) s_exportP2NickCache = np.p2Name;
    } else {
        s_exportNickSessionId = 0;
        s_exportP1NickCache.clear();
        s_exportP2NickCache.clear();
    }

    bool exportIdleNoFlow =
        haveNetplayExport &&
        np.sessionMode != EFZ_SESSION_NONE &&
        np.sessionPhase == EFZ_PHASE_IDLE &&
        !np.inNetplayMenu &&
        !np.inNetplayCharacterSelect &&
        !np.inNetplayMatch &&
        (!np.hasActivityPhase || np.activityPhase == EFZ_ACTIVITY_IDLE);

    // Prefer netplay export role when available; it distinguishes hosting/joining.
    if (haveNetplayExport) {
        if (exportIdleNoFlow) {
            // Session metadata can linger as HOST/JOIN after menu/session exit.
            // Treat idle/no-flow as inactive to avoid sticky "Hosting" presence.
            onl = OnlineState::Offline;
        } else {
            switch (np.sessionMode) {
                case EFZ_SESSION_HOSTING:
                case EFZ_SESSION_JOINING:
                    onl = OnlineState::Netplay;
                    break;
                case EFZ_SESSION_SPECTATING:
                    onl = OnlineState::Spectating;
                    break;
                case EFZ_SESSION_TOURNAMENT:
                    onl = OnlineState::Tournament;
                    break;
                default:
                    break;
            }
        }
    }
    // Optional probe: EFZDA_MENU_PROBE=1 dumps a window of the game state struct for reverse engineering
    static bool s_probeChecked = false;
    static bool s_probeEnabled = false;
    if (!s_probeChecked) {
        wchar_t env[4];
        s_probeEnabled = GetEnvironmentVariableW(L"EFZDA_MENU_PROBE", env, _countof(env)) > 0;
        s_probeChecked = true;
    }
    if (s_probeEnabled && (onl == OnlineState::Offline || onl == OnlineState::Unknown)) {
        if (uintptr_t gsp = get_game_state_ptr(efzBase)) probe_game_state_region(gsp);
    }
    const char* onlName = online_state_name(onl);
    if (haveNetplayExport) {
        log("GSPoll#%lu: gameModeRaw=%u gameMode='%s' onlineState='%s' netplay(mode=%d phase=%d side=%d menu=%d/%u/%u cs=%d match=%d act=%u:%s end=%u:%s caps=0x%X seq=%u sid=%u set=%u)",
            s_poll,
            (unsigned)gmRaw,
            gmName ? gmName : "?",
            onlName ? onlName : "?",
            np.sessionMode,
            np.sessionPhase,
            np.localSide,
            np.inNetplayMenu ? 1 : 0,
            (unsigned)np.netplayMenuScreen,
            (unsigned)np.netplayMenuDetail,
            np.inNetplayCharacterSelect ? 1 : 0,
            np.inNetplayMatch ? 1 : 0,
            (unsigned)np.activityPhase,
            netplay_activity_name(np.activityPhase),
            (unsigned)np.endReason,
            netplay_end_reason_name(np.endReason),
            (unsigned)np.capabilityFlags,
            (unsigned)np.stateSeq,
            (unsigned)np.sessionId,
            (unsigned)np.setId);
    } else {
        log("GSPoll#%lu: gameModeRaw=%u gameMode='%s' onlineState='%s'",
            s_poll, (unsigned)gmRaw, gmName ? gmName : "?", onlName ? onlName : "?");
    }
    // If the EFZ game mode changed (offline/unknown), treat it as a transition from main menu to a pre-match flow (char-select)
    bool justChangedMode = false;
    if ((onl == OnlineState::Offline || onl == OnlineState::Unknown) && s_lastGmRaw != gmRaw) {
        justChangedMode = true;
        // Clear last-seen names and reset spawn debounce so we don't carry stale characters/icons
        s_lastP1Name.clear();
        s_lastP2Name.clear();
        s_spawnedFrames = 0;
        s_unspawnedFrames = 0;
        log("GSPoll#%lu: detected game mode change -> entering char-select flow", s_poll);
    }

    bool inMatch = !p1.empty() && !p2.empty();

    bool isOnTitleScreen = false;
    if (haveTopScreen && s_screenTitle >= 0) {
        isOnTitleScreen = (topScreenIdx == (uint8_t)s_screenTitle);
    }
    bool inNetplayMenuState = haveNetplayExport && np.inNetplayMenu;
    bool inNetplayConnectingState = false;
    bool inNetplayDelaySetupState = false;
    bool inNetplayCharacterSelectState = haveNetplayExport && np.inNetplayCharacterSelect;
    bool inNetplayLoadingState = false;
    bool inNetplayMatchState = haveNetplayExport && np.inNetplayMatch;
    bool inNetplayResultsState = false;
    bool useActivityPhase =
        haveNetplayExport &&
        np.hasActivityPhase &&
        (!np.hasCapabilityFlags ||
         (np.capabilityFlags & EFZ_CAP_ACTIVITY) != 0 ||
         np.activityPhase != EFZ_ACTIVITY_IDLE);
    if (useActivityPhase) {
        inNetplayMenuState = false;
        inNetplayConnectingState = false;
        inNetplayDelaySetupState = false;
        inNetplayCharacterSelectState = false;
        inNetplayLoadingState = false;
        inNetplayMatchState = false;
        inNetplayResultsState = false;
        switch (np.activityPhase) {
            case EFZ_ACTIVITY_MENU: inNetplayMenuState = true; break;
            case EFZ_ACTIVITY_CONNECTING: inNetplayConnectingState = true; break;
            case EFZ_ACTIVITY_DELAY_SETUP: inNetplayDelaySetupState = true; break;
            case EFZ_ACTIVITY_CHAR_SELECT: inNetplayCharacterSelectState = true; break;
            case EFZ_ACTIVITY_LOADING: inNetplayLoadingState = true; break;
            case EFZ_ACTIVITY_MATCH: inNetplayMatchState = true; break;
            case EFZ_ACTIVITY_RESULTS: inNetplayResultsState = true; break;
            default: break;
        }
    }
    if (inNetplayMenuState &&
        (inNetplayCharacterSelectState || inNetplayLoadingState || inNetplayMatchState || inNetplayResultsState)) {
        log("GSPoll#%lu: suppress netplay-menu state (explicit netplay activity flags)", s_poll);
        inNetplayMenuState = false;
    }
    bool hasActiveNetplaySession = haveNetplayExport && np.sessionMode != EFZ_SESSION_NONE;
    bool localScreenContradictsMenu =
        haveTopScreen &&
        s_screenTitle >= 0 &&
        topScreenIdx != (uint8_t)s_screenTitle;
    bool localVsFlow = (gmRaw == 4 || gmRaw == 5);
    auto mapLocalFlowFromContext = [&]() {
        bool mapped = false;
        if (haveTopScreen) {
            if (s_screenCharSel >= 0 && topScreenIdx == (uint8_t)s_screenCharSel) {
                inNetplayCharacterSelectState = true;
                mapped = true;
            } else if (s_screenLoading >= 0 && topScreenIdx == (uint8_t)s_screenLoading) {
                inNetplayLoadingState = true;
                mapped = true;
            } else if (s_screenInGame >= 0 && topScreenIdx == (uint8_t)s_screenInGame) {
                inNetplayMatchState = true;
                mapped = true;
            }
        }
        if (!mapped && (inMatch || spawnedDebounced)) {
            inNetplayMatchState = true;
            mapped = true;
        }
        if (!mapped && hasActiveNetplaySession && localVsFlow) {
            inNetplayCharacterSelectState = true;
            mapped = true;
        }
        return mapped;
    };
    // Hard override for known bad exports: session is active and local flow is
    // clearly not title/menu even though activity/menu flags still say "Menu".
    if (inNetplayMenuState &&
        hasActiveNetplaySession &&
        (localScreenContradictsMenu || (np.sessionPhase == EFZ_PHASE_CONNECTED && localVsFlow))) {
        log("GSPoll#%lu: suppress netplay-menu state (hard override: session=%d phase=%d screen=%u title=%d gmRaw=%u)",
            s_poll,
            np.sessionMode,
            np.sessionPhase,
            haveTopScreen ? (unsigned)topScreenIdx : 0xFFu,
            s_screenTitle,
            (unsigned)gmRaw);
        inNetplayMenuState = false;
        if (!mapLocalFlowFromContext() && np.sessionPhase == EFZ_PHASE_CONNECTED) {
            inNetplayLoadingState = true;
        }
    }
    // The menu overlay should only exist on title screen. If local game state clearly
    // indicates gameplay/charselect, treat menu/activity=Menu as stale and prefer local evidence.
    bool gameplayLikeContext =
        inMatch ||
        spawnedDebounced ||
        (haveTopScreen && !isOnTitleScreen);
    if (inNetplayMenuState &&
        gameplayLikeContext &&
        (np.sessionPhase == EFZ_PHASE_CONNECTED || npStateLikelyStale)) {
        log("GSPoll#%lu: suppress netplay-menu state (local context contradicts menu, screen=%u title=%d connected=%d stale=%d)",
            s_poll,
            haveTopScreen ? (unsigned)topScreenIdx : 0xFFu,
            s_screenTitle,
            (np.sessionPhase == EFZ_PHASE_CONNECTED) ? 1 : 0,
            npStateLikelyStale ? 1 : 0);
        inNetplayMenuState = false;
        if (!mapLocalFlowFromContext() && np.sessionPhase == EFZ_PHASE_CONNECTED) {
            inNetplayLoadingState = true;
        }
    }
    if (!useActivityPhase) {
        // The menu overlay lives on top of title screen. If we're no longer on title,
        // or match entities are already spawned during connected phase, treat menu flag
        // as stale and continue into normal online/match presence handling.
        if (inNetplayMenuState && haveTopScreen && !isOnTitleScreen) {
            log("GSPoll#%lu: suppress netplay-menu state (screen=%u not title=%d)",
                s_poll, (unsigned)topScreenIdx, s_screenTitle);
            inNetplayMenuState = false;
        }
        if (inNetplayMenuState && np.sessionPhase == EFZ_PHASE_CONNECTED) {
            // Connected alone is not enough: after a match, the session may remain
            // connected while the user is back in the netplay menu.
            // Suppress menu only when context still looks like active gameplay/handoff.
            if (gameplayLikeContext) {
                log("GSPoll#%lu: suppress netplay-menu state (connected gameplay context)", s_poll);
                inNetplayMenuState = false;
            }
        }
    }

    if (exportIdleNoFlow) {
        // Session metadata can linger as Hosting/Joining after leaving the netplay flow.
        // Force local netplay-flow flags inactive so we fall back to normal menu/offline mapping.
        inNetplayMenuState = false;
        inNetplayConnectingState = false;
        inNetplayDelaySetupState = false;
        inNetplayCharacterSelectState = false;
        inNetplayLoadingState = false;
        inNetplayMatchState = false;
        inNetplayResultsState = false;
    }

    bool exportSnapshotResolved = false;
    int exportSelfIdx = -1;
    int exportP1Wins = 0;
    int exportP2Wins = 0;
    std::string exportP1Nick;
    std::string exportP2Nick;
    auto ensureExportSnapshot = [&]() {
        if (exportSnapshotResolved) return;
        exportSnapshotResolved = true;
        if (!haveNetplayExport || np.sessionMode == EFZ_SESSION_NONE) return;

        auto clampScore = [](int v) { return (v >= 0 && v <= 99) ? v : 0; };

        exportSelfIdx = np.localSide;
        if (exportSelfIdx != 0 && exportSelfIdx != 1) {
            if (np.sessionMode == EFZ_SESSION_HOSTING) exportSelfIdx = 0;
            else if (np.sessionMode == EFZ_SESSION_JOINING) exportSelfIdx = 1;
        }

        exportP1Wins = np.p1Wins;
        exportP2Wins = np.p2Wins;
        exportP1Nick = !np.p1Name.empty() ? np.p1Name : s_exportP1NickCache;
        exportP2Nick = !np.p2Name.empty() ? np.p2Name : s_exportP2NickCache;

        if (exportSelfIdx == 0 && exportP1Nick.empty() && !np.localNickname.empty()) exportP1Nick = np.localNickname;
        if (exportSelfIdx == 1 && exportP2Nick.empty() && !np.localNickname.empty()) exportP2Nick = np.localNickname;
        if (exportSelfIdx == -1 && exportP1Nick.empty() && !np.localNickname.empty() && np.sessionMode == EFZ_SESSION_HOSTING) exportP1Nick = np.localNickname;
        if (exportSelfIdx == -1 && exportP2Nick.empty() && !np.localNickname.empty() && np.sessionMode == EFZ_SESSION_JOINING) exportP2Nick = np.localNickname;

        if (!revivalBase) {
            exportP1Wins = clampScore(exportP1Wins);
            exportP2Wins = clampScore(exportP2Wins);
            return;
        }

        int revivalSelfIdx = read_current_player_index(revivalBase);
        if ((exportSelfIdx != 0 && exportSelfIdx != 1) && (revivalSelfIdx == 0 || revivalSelfIdx == 1)) {
            exportSelfIdx = revivalSelfIdx;
        }

        if (exportP1Nick.empty()) {
            exportP1Nick = read_nickname(revivalBase, NickP1Offset(), P1_NICKNAME_SPECTATOR_OFFSET);
        }
        if (exportP2Nick.empty()) {
            exportP2Nick = read_nickname(revivalBase, NickP2Offset(), P2_NICKNAME_SPECTATOR_OFFSET);
        }

        bool inActiveFlowContext =
            inNetplayCharacterSelectState ||
            inNetplayLoadingState ||
            inNetplayMatchState ||
            inNetplayResultsState ||
            np.sessionPhase == EFZ_PHASE_CONNECTED;

        bool needWinsFallback =
            (exportP1Wins < 0 || exportP1Wins > 99 || exportP2Wins < 0 || exportP2Wins > 99) ||
            ((exportP1Wins == 0 && exportP2Wins == 0) && inActiveFlowContext);
        if (needWinsFallback) {
            int revP1Wins = 0;
            int revP2Wins = 0;
            if (np.sessionMode == EFZ_SESSION_TOURNAMENT || onl == OnlineState::Tournament) {
                revP1Wins = read_win_count(revivalBase, TournP1WinOffset(), P1_WIN_COUNT_SPECTATOR_OFFSET);
                revP2Wins = read_win_count(revivalBase, TournP2WinOffset(), P2_WIN_COUNT_SPECTATOR_OFFSET);
            } else {
                revP1Wins = read_win_count(revivalBase, NetP1WinOffset(), P1_WIN_COUNT_SPECTATOR_OFFSET);
                revP2Wins = read_win_count(revivalBase, NetP2WinOffset(), P2_WIN_COUNT_SPECTATOR_OFFSET);
            }
            if ((exportP1Wins < 0 || exportP1Wins > 99 || exportP2Wins < 0 || exportP2Wins > 99) ||
                ((exportP1Wins == 0 && exportP2Wins == 0) && (revP1Wins > 0 || revP2Wins > 0))) {
                exportP1Wins = revP1Wins;
                exportP2Wins = revP2Wins;
            }
        }

        exportP1Wins = clampScore(exportP1Wins);
        exportP2Wins = clampScore(exportP2Wins);
    };

    // Dedicated netplay-menu state from efz_netplay_mod export.
    if (inNetplayMenuState) {
        gs.details = "In Netplay Menu";
        std::string state = format_netplay_menu_state(np);
        if (np.sessionPhase != EFZ_PHASE_IDLE &&
            np.sessionPhase != EFZ_PHASE_CONNECTED) {
            state += " (" + std::string(netplay_phase_name(np.sessionPhase)) + ")";
        }
        gs.state = state;
        gs.largeImageKey = "efz_icon";
        gs.largeImageText = "Netplay Menu";
        gs.smallImageKey.clear();
        gs.smallImageText.clear();
        s_lastP1Name = p1; s_lastP2Name = p2; s_lastGmRaw = gmRaw;
        log("GSPoll#%lu: netplay-menu -> details='%s' state='%s'", s_poll, gs.details.c_str(), gs.state.c_str());
        return gs;
    }

    // Connecting/negotiation/error phases should be explicit statuses.
    bool phaseNeedsStatus =
        np.sessionPhase == EFZ_PHASE_CONNECTING ||
        np.sessionPhase == EFZ_PHASE_DELAY_SETUP ||
        np.sessionPhase == EFZ_PHASE_FAILED ||
        np.sessionPhase == EFZ_PHASE_SESSION_ENDED;
    if (haveNetplayExport &&
        np.sessionMode != EFZ_SESSION_NONE &&
        !exportIdleNoFlow &&
        np.sessionPhase != EFZ_PHASE_IDLE &&
        (phaseNeedsStatus || inNetplayConnectingState || inNetplayDelaySetupState)) {
        ensureExportSnapshot();
        int mode = np.sessionMode;
        if (mode == EFZ_SESSION_HOSTING) gs.details = np.localNickname.empty() ? "Hosting" : ("Hosting(" + np.localNickname + ")");
        else if (mode == EFZ_SESSION_JOINING) gs.details = "Playing online match";
        else if (mode == EFZ_SESSION_SPECTATING) gs.details = "Watching online match";
        else if (mode == EFZ_SESSION_TOURNAMENT) gs.details = "Playing tournament match";
        else gs.details = "Online";

        if (inNetplayConnectingState) {
            gs.state = "Connecting...";
        } else if (inNetplayDelaySetupState) {
            gs.state = "Setting input delay...";
        } else {
            switch (np.sessionPhase) {
                case EFZ_PHASE_CONNECTING: gs.state = "Connecting..."; break;
                case EFZ_PHASE_DELAY_SETUP: gs.state = "Setting input delay..."; break;
                case EFZ_PHASE_FAILED: gs.state = "Connection failed"; break;
                case EFZ_PHASE_SESSION_ENDED: gs.state = "Session ended"; break;
                default: gs.state = "Synchronizing..."; break;
            }
        }
        if (np.hasEndReason &&
            np.endReason != EFZ_END_NONE &&
            (np.sessionPhase == EFZ_PHASE_FAILED || np.sessionPhase == EFZ_PHASE_SESSION_ENDED)) {
            gs.state += " (" + std::string(netplay_end_reason_name(np.endReason)) + ")";
        }
        if (exportP1Wins >= 0 && exportP2Wins >= 0 && exportP1Wins <= 99 && exportP2Wins <= 99) {
            gs.state += " (" + std::to_string(exportP1Wins) + "-" + std::to_string(exportP2Wins) + ")";
        }
        gs.largeImageKey = "210px-efzlogo";
        gs.largeImageText = "Online Match";
        gs.smallImageKey.clear();
        gs.smallImageText.clear();
        s_lastP1Name = p1; s_lastP2Name = p2; s_lastGmRaw = gmRaw;
        log("GSPoll#%lu: netplay-phase -> details='%s' state='%s'", s_poll, gs.details.c_str(), gs.state.c_str());
        return gs;
    }

    // Explicit online character-select state from netplay export (v3/v4).
    if (inNetplayCharacterSelectState) {
        ensureExportSnapshot();
        int side = exportSelfIdx;
        if (side != 0 && side != 1) {
            if (np.sessionMode == EFZ_SESSION_HOSTING) side = 0;
            else if (np.sessionMode == EFZ_SESSION_JOINING) side = 1;
        }
        std::string p1Nick = exportP1Nick;
        std::string p2Nick = exportP2Nick;
        if (side == 0 && p1Nick.empty() && !np.localNickname.empty()) p1Nick = np.localNickname;
        if (side == 1 && p2Nick.empty() && !np.localNickname.empty()) p2Nick = np.localNickname;
        const std::string selfNick = (side == 0 ? p1Nick : (side == 1 ? p2Nick : std::string()));
        const std::string oppNick = (side == 0 ? p2Nick : (side == 1 ? p1Nick : std::string()));
        const std::string& ourChar = (side == 1 ? p2 : p1);
        const std::string& oppChar = (side == 1 ? p1 : p2);

        if (onl == OnlineState::Tournament) {
            gs.details = "Playing tournament match";
        } else if (onl == OnlineState::Spectating) {
            gs.details = "Watching online match";
        } else {
            gs.details = selfNick.empty() ? "Playing online match" : ("Playing online match (" + selfNick + ")");
        }
        int ourWins = (side == 1 ? exportP2Wins : exportP1Wins);
        int theirWins = (side == 1 ? exportP1Wins : exportP2Wins);
        bool haveScore = (ourWins >= 0 && theirWins >= 0 && ourWins <= 99 && theirWins <= 99);
        if (!oppChar.empty() || !oppNick.empty()) {
            gs.state = "Against ";
            if (!oppChar.empty()) {
                gs.state += oppChar;
                if (!oppNick.empty()) gs.state += " (" + oppNick + ")";
            } else {
                gs.state += oppNick;
            }
        } else {
            gs.state = "Character select";
        }
        if (haveScore) {
            gs.state += " (" + std::to_string(ourWins) + "-" + std::to_string(theirWins) + ")";
        }

        if (!ourChar.empty()) {
            std::string kL = map_char_to_large_image_key(ourChar);
            if (!kL.empty()) { gs.largeImageKey = kL; gs.largeImageText = ourChar; }
            else { gs.largeImageKey = "210px-efzlogo"; gs.largeImageText = "Online Match"; }
        } else {
            gs.largeImageKey = "210px-efzlogo";
            gs.largeImageText = "Online Match";
        }
        if (!oppChar.empty()) {
            std::string kS = map_char_to_small_icon_key(oppChar);
            if (!kS.empty()) { gs.smallImageKey = kS; gs.smallImageText = std::string("Against ") + oppChar; }
            else { gs.smallImageKey.clear(); gs.smallImageText.clear(); }
        } else {
            gs.smallImageKey.clear();
            gs.smallImageText.clear();
        }
        s_lastP1Name = p1; s_lastP2Name = p2; s_lastGmRaw = gmRaw;
        log("GSPoll#%lu: netplay-charselect -> details='%s' state='%s'", s_poll, gs.details.c_str(), gs.state.c_str());
        return gs;
    }

    if (inNetplayLoadingState) {
        ensureExportSnapshot();
        std::string selfNick = (exportSelfIdx == 0 ? exportP1Nick : (exportSelfIdx == 1 ? exportP2Nick : std::string()));
        if (onl == OnlineState::Tournament) gs.details = "Playing tournament match";
        else if (onl == OnlineState::Spectating) gs.details = "Watching online match";
        else gs.details = selfNick.empty() ? "Playing online match" : ("Playing online match (" + selfNick + ")");
        gs.state = "Loading match";
        if (exportP1Wins >= 0 && exportP2Wins >= 0 && exportP1Wins <= 99 && exportP2Wins <= 99) {
            int left = exportP1Wins;
            int right = exportP2Wins;
            if (onl != OnlineState::Spectating && exportSelfIdx == 1) {
                left = exportP2Wins;
                right = exportP1Wins;
            }
            gs.state += " (" + std::to_string(left) + "-" + std::to_string(right) + ")";
        }
        gs.largeImageKey = "210px-efzlogo";
        gs.largeImageText = "Online Match";
        gs.smallImageKey.clear();
        gs.smallImageText.clear();
        s_lastP1Name = p1; s_lastP2Name = p2; s_lastGmRaw = gmRaw;
        log("GSPoll#%lu: netplay-loading -> details='%s' state='%s'", s_poll, gs.details.c_str(), gs.state.c_str());
        return gs;
    }

    if (inNetplayResultsState) {
        ensureExportSnapshot();
        std::string selfNick = (exportSelfIdx == 0 ? exportP1Nick : (exportSelfIdx == 1 ? exportP2Nick : std::string()));
        if (onl == OnlineState::Tournament) gs.details = "Playing tournament match";
        else if (onl == OnlineState::Spectating) gs.details = "Watching online match";
        else gs.details = selfNick.empty() ? "Playing online match" : ("Playing online match (" + selfNick + ")");
        gs.state = "Results";
        if (exportP1Wins >= 0 && exportP2Wins >= 0 && exportP1Wins <= 99 && exportP2Wins <= 99) {
            int left = exportP1Wins;
            int right = exportP2Wins;
            if (onl != OnlineState::Spectating && exportSelfIdx == 1) {
                left = exportP2Wins;
                right = exportP1Wins;
            }
            gs.state += " (" + std::to_string(left) + "-" + std::to_string(right) + ")";
        }
        gs.largeImageKey = "210px-efzlogo";
        gs.largeImageText = "Online Match";
        gs.smallImageKey.clear();
        gs.smallImageText.clear();
        s_lastP1Name = p1; s_lastP2Name = p2; s_lastGmRaw = gmRaw;
        log("GSPoll#%lu: netplay-results -> details='%s' state='%s'", s_poll, gs.details.c_str(), gs.state.c_str());
        return gs;
    }

    // No offline suppression: we will show available characters incrementally like netplay

    // Prefer EFZ game mode in offline/unknown online contexts
    const bool isReplay = (gmName && (std::string(gmName) == "Replay" || std::string(gmName) == "Auto-Replay"));
    if (onl == OnlineState::Offline || onl == OnlineState::Unknown) {
        // No constant label; details/state reflect activity directly
    if (isReplay) {
            gs.details = "Watching replay";
            gs.state = inMatch ? (p1 + " vs " + p2) : std::string("Loading replay");
            // Large: our character (P1), Small: opponent (P2)
            if (!p1.empty()) {
                std::string kL = map_char_to_large_image_key(p1);
                if (!kL.empty()) { gs.largeImageKey = kL; gs.largeImageText = p1; }
            }
            // Small icon: opponent (P2) when available
            if (!p2.empty()) {
                std::string key = map_char_to_small_icon_key(p2);
                if (!key.empty()) { gs.smallImageKey = key; gs.smallImageText = std::string("Against ") + p2; }
            }
            log("GSPoll#%lu: offline replay -> details='%s' state='%s'", s_poll, gs.details.c_str(), gs.state.c_str());
            return gs;
        }

    // Not replay: Playing in <Mode> (P1)
    // Keep raw mode (as read) for logic, and a prettified label for display
    std::string rawMode = gmName ? gmName : "";
    std::string prettyMode = rawMode;
    if (prettyMode == "Arcade" || prettyMode == "Practice") prettyMode += " Mode";
    if (prettyMode.empty()) prettyMode = "Game";

        if (inMatch) {
            gs.details = std::string("Playing in ") + prettyMode;
            // Always show current P1 when known; update incrementally
            gs.state = std::string("As ") + p1; // P1 perspective
            // Large: our character (P1), Small: opponent (P2)
            if (!p1.empty()) {
                std::string kL = map_char_to_large_image_key(p1);
                if (!kL.empty()) { gs.largeImageKey = kL; gs.largeImageText = p1; }
            }
            // Small icon: opponent (P2) when available
            if (!p2.empty()) {
                std::string key = map_char_to_small_icon_key(p2);
                if (!key.empty()) { gs.smallImageKey = key; gs.smallImageText = std::string("Against ") + p2; }
            }
        } else {
        // Menus or pre-select: prefer deterministic screen-index mapping, else fallback
        uint8_t screenIdx = 0xFF;
        bool haveScreen = false;
        if (haveTopScreen) { screenIdx = topScreenIdx; haveScreen = true; }
        else { haveScreen = read_screen_index(efzBase, screenIdx); }
            if (haveScreen) {
                if (s_screenTitle >= 0 && screenIdx == (uint8_t)s_screenTitle) {
                    gs.details = "Main Menu";
                    gs.state.clear();
                    gs.largeImageKey = "efz_icon"; gs.largeImageText = "Main Menu";
                    gs.state = "The true Eternal does exists here";
                    gs.smallImageKey.clear(); gs.smallImageText.clear();
                    log("GSPoll#%lu: offline(screen=%u) -> details='%s' state='%s'", s_poll, (unsigned)screenIdx, gs.details.c_str(), gs.state.c_str());
            if (haveScreen) s_lastScreenIdx = screenIdx;
            s_lastP1Name = p1; s_lastP2Name = p2; s_lastGmRaw = gmRaw; return gs;
                }
                if (s_screenSettings >= 0 && screenIdx == (uint8_t)s_screenSettings) {
                    gs.details = "Options";
            gs.state.clear();
                    gs.largeImageKey = "efz_icon"; gs.largeImageText = "Options";
                    gs.smallImageKey.clear(); gs.smallImageText.clear();
                    log("GSPoll#%lu: offline(screen=%u) -> details='%s' state='%s'", s_poll, (unsigned)screenIdx, gs.details.c_str(), gs.state.c_str());
            if (haveScreen) s_lastScreenIdx = screenIdx;
            s_lastP1Name = p1; s_lastP2Name = p2; s_lastGmRaw = gmRaw; return gs;
                }
                if (s_screenReplayMenu >= 0 && screenIdx == (uint8_t)s_screenReplayMenu) {
            gs.details = "Replay Selection";
            gs.state = "Selecting replay";
            // Clear icons to avoid leftovers
            gs.largeImageKey.clear(); gs.largeImageText.clear();
            gs.smallImageKey.clear(); gs.smallImageText.clear();
                    log("GSPoll#%lu: offline(screen=%u) -> details='%s' state='%s'", s_poll, (unsigned)screenIdx, gs.details.c_str(), gs.state.c_str());
            if (haveScreen) s_lastScreenIdx = screenIdx;
            s_lastP1Name = p1; s_lastP2Name = p2; s_lastGmRaw = gmRaw; return gs;
                }
                if (s_screenCharSel >= 0 && screenIdx == (uint8_t)s_screenCharSel) {
                    // Char-select: show current mode as activity; no icons until selection happens
                    gs.details = std::string("Playing in ") + prettyMode;
                    // Show picks incrementally if available, but don't rely on them
            gs.state.clear();
            // Clear icons first to avoid stale assets
            gs.largeImageKey.clear(); gs.largeImageText.clear();
            gs.smallImageKey.clear(); gs.smallImageText.clear();
                    if (!p1.empty()) {
                        gs.state = std::string("As ") + p1;
                        std::string kL = map_char_to_large_image_key(p1);
                        if (!kL.empty()) { gs.largeImageKey = kL; gs.largeImageText = p1; }
                    }
                    if (!p2.empty()) {
                        std::string key = map_char_to_small_icon_key(p2);
                        if (!key.empty()) { gs.smallImageKey = key; gs.smallImageText = std::string("Against ") + p2; }
                        if (gs.state.empty() && !p1.empty()) {
                            // If we somehow have P2 first, still show incremental
                            gs.state = std::string("As ") + p1;
                        }
                    }
                    log("GSPoll#%lu: offline(screen=%u) -> details='%s' state='%s'", s_poll, (unsigned)screenIdx, gs.details.c_str(), gs.state.c_str());
            if (haveScreen) s_lastScreenIdx = screenIdx;
            s_lastP1Name = p1; s_lastP2Name = p2; s_lastGmRaw = gmRaw; return gs;
                }
                if (s_screenLoading >= 0 && screenIdx == (uint8_t)s_screenLoading) {
            gs.details = std::string("Loading") + (prettyMode.empty() ? "" : (" - " + prettyMode));
            gs.state = "Loading";
            // Clear icons during loading to avoid stale display
            gs.largeImageKey.clear(); gs.largeImageText.clear();
            gs.smallImageKey.clear(); gs.smallImageText.clear();
                    log("GSPoll#%lu: offline(screen=%u) -> details='%s' state='%s'", s_poll, (unsigned)screenIdx, gs.details.c_str(), gs.state.c_str());
            if (haveScreen) s_lastScreenIdx = screenIdx;
            s_lastP1Name = p1; s_lastP2Name = p2; s_lastGmRaw = gmRaw; return gs;
                }
                if (s_screenInGame >= 0 && screenIdx == (uint8_t)s_screenInGame) {
                    // Treat as in-match even if names haven't populated yet
                    gs.details = std::string("Playing in ") + prettyMode;
            // Clear icons then set incrementally
            gs.largeImageKey.clear(); gs.largeImageText.clear();
            gs.smallImageKey.clear(); gs.smallImageText.clear();
            if (!p1.empty()) {
                        gs.state = std::string("As ") + p1;
                        std::string kL = map_char_to_large_image_key(p1);
                        if (!kL.empty()) { gs.largeImageKey = kL; gs.largeImageText = p1; }
                    }
                    if (!p2.empty()) {
                        std::string key = map_char_to_small_icon_key(p2);
                        if (!key.empty()) { gs.smallImageKey = key; gs.smallImageText = std::string("Against ") + p2; }
                        if (gs.state.empty() && !p1.empty()) {
                            gs.state = std::string("As ") + p1;
                        }
                    }
                    log("GSPoll#%lu: offline(screen=%u) -> details='%s' state='%s'", s_poll, (unsigned)screenIdx, gs.details.c_str(), gs.state.c_str());
            if (haveScreen) s_lastScreenIdx = screenIdx;
            s_lastP1Name = p1; s_lastP2Name = p2; s_lastGmRaw = gmRaw; return gs;
                }
                // Unknown screen value: fall back below
            }

            // Fallback (no screen index): use scene/heuristics
            uint8_t sceneVal = 0xFF; bool haveScene = read_scene_value(efzBase, sceneVal);
            bool isCharSel = false;
            if (haveScene && s_sceneCharSel >= 0 && sceneVal == (uint8_t)s_sceneCharSel) isCharSel = true;
            else if (gmName && (rawMode == "Arcade" || rawMode == "Practice" || rawMode == "VS CPU" || rawMode == "VS Human") && !spawnedDebounced) isCharSel = true;
            else if (justChangedMode) isCharSel = true;

            bool isMainMenu = false;
            if (haveScene && s_sceneMainMenu >= 0 && sceneVal == (uint8_t)s_sceneMainMenu) isMainMenu = true;
            else if (!isCharSel && !spawnedDebounced) isMainMenu = true;

            if (isMainMenu) {
                gs.details = "Main Menu";
                gs.largeImageKey = "efz_icon"; gs.largeImageText = "Main Menu";
                gs.state = "The true Eternal does exists here";
            } else if (isCharSel) {
                gs.details = std::string("Character Select") + (prettyMode.empty() ? "" : (" - " + prettyMode));
            } else if (gmName) {
                gs.details = std::string("Playing in ") + prettyMode;
            } else {
                gs.details = "In Menus";
            }
            // Incremental icons in fallback
            if (!p1.empty()) { std::string kL = map_char_to_large_image_key(p1); if (!kL.empty()) { gs.largeImageKey = kL; gs.largeImageText = p1; } }
            if (!p2.empty()) { std::string key = map_char_to_small_icon_key(p2); if (!key.empty()) { gs.smallImageKey = key; gs.smallImageText = std::string("Against ") + p2; } }
        }
        log("GSPoll#%lu: offline -> details='%s' state='%s'", s_poll, gs.details.c_str(), gs.state.c_str());
    // update last-seen names and mode before returning
    s_lastP1Name = p1; s_lastP2Name = p2; s_lastGmRaw = gmRaw;
    return gs;
    }

    // Only attempt EfzRevival reads when online/spectating/tournament
    int p1Wins = 0, p2Wins = 0;
    std::string p1Nick, p2Nick;
    int selfIdx = -1; // 0=P1, 1=P2, -1 unknown
    if (haveNetplayExport && np.sessionMode != EFZ_SESSION_NONE) {
        ensureExportSnapshot();
        p1Wins = exportP1Wins;
        p2Wins = exportP2Wins;
        p1Nick = exportP1Nick;
        p2Nick = exportP2Nick;
        selfIdx = exportSelfIdx;
        log("GSPoll#%lu: using netplay export for scores/nicknames (mode=%d phase=%d side=%d p1=%d p2=%d)",
            s_poll, np.sessionMode, np.sessionPhase, selfIdx, p1Wins, p2Wins);
    } else if (onl == OnlineState::Netplay || onl == OnlineState::Spectating || onl == OnlineState::Tournament) {
        if (onl == OnlineState::Tournament) {
            // 1.02i appears to store tournament counters differently; prefer plausible pair between standard and tournament.
            EfzRevivalVersion ver = DetectEfzRevivalVersion();
            if (ver == EfzRevivalVersion::Revival102i) {
                int p1Std = read_win_count(revivalBase, NetP1WinOffset(), P1_WIN_COUNT_SPECTATOR_OFFSET);
                int p2Std = read_win_count(revivalBase, NetP2WinOffset(), P2_WIN_COUNT_SPECTATOR_OFFSET);
                int p1T = read_win_count(revivalBase, TournP1WinOffset(), P1_WIN_COUNT_SPECTATOR_OFFSET);
                int p2T = read_win_count(revivalBase, TournP2WinOffset(), P2_WIN_COUNT_SPECTATOR_OFFSET);
                auto plausible = [](int a, int b) { return (a >= 0 && b >= 0 && a <= 9 && b <= 9); };
                bool stdOK = plausible(p1Std, p2Std);
                bool tOK = plausible(p1T, p2T);
                bool isCharSel = false; // detect via global screen already sampled above
                {
                    uint8_t tmp = 0xFF;
                    if (read_screen_index(efzBase, tmp)) isCharSel = (tmp == (uint8_t)s_screenCharSel);
                }
                if (stdOK && !tOK) { p1Wins = p1Std; p2Wins = p2Std; efzda::log("[tick=%llu] WINS(1.02i): choose STANDARD std=%d-%d tourn=%d-%d", ticks(), p1Std, p2Std, p1T, p2T); }
                else if (!stdOK && tOK) { p1Wins = p1T; p2Wins = p2T; efzda::log("[tick=%llu] WINS(1.02i): choose TOURNAMENT std=%d-%d tourn=%d-%d", ticks(), p1Std, p2Std, p1T, p2T); }
                else if (stdOK && tOK) {
                    // Prefer standard at character select, tournament during match
                    if (isCharSel) { p1Wins = p1Std; p2Wins = p2Std; }
                    else { p1Wins = p1T; p2Wins = p2T; }
                    efzda::log("[tick=%llu] WINS(1.02i): both plausible, chose %s std=%d-%d tourn=%d-%d", ticks(), isCharSel ? "STANDARD" : "TOURNAMENT", p1Std, p2Std, p1T, p2T);
                } else {
                    // Neither looks right — default to standard to avoid outliers (e.g., 21-0)
                    p1Wins = p1Std; p2Wins = p2Std;
                    efzda::log("[tick=%llu] WINS(1.02i): neither plausible, default STANDARD std=%d-%d tourn=%d-%d", ticks(), p1Std, p2Std, p1T, p2T);
                }
            } else {
                // 1.02e/h: tournament offsets stable
                p1Wins = read_win_count(revivalBase, TournP1WinOffset(), P1_WIN_COUNT_SPECTATOR_OFFSET);
                p2Wins = read_win_count(revivalBase, TournP2WinOffset(), P2_WIN_COUNT_SPECTATOR_OFFSET);
            }
        } else {
            // Netplay/Spectating: use version-aware primary counters.
            p1Wins = read_win_count(revivalBase, NetP1WinOffset(), P1_WIN_COUNT_SPECTATOR_OFFSET);
            p2Wins = read_win_count(revivalBase, NetP2WinOffset(), P2_WIN_COUNT_SPECTATOR_OFFSET);
            // Optional opt-in fallback via env if needed for diagnostics:
            // EFZDA_ALLOW_TOURNAMENT_FALLBACK=1 will re-enable probing tournament counters when both are zero.
            if (p1Wins == 0 && p2Wins == 0) {
                static bool s_tfChecked = false;
                static bool s_allowTournFallback = false;
                if (!s_tfChecked) {
                    wchar_t env[8];
                    s_allowTournFallback = (GetEnvironmentVariableW(L"EFZDA_ALLOW_TOURNAMENT_FALLBACK", env, _countof(env)) > 0) && (wcstol(env, nullptr, 0) != 0);
                    s_tfChecked = true;
                }
                if (s_allowTournFallback) {
                    int t1 = read_win_count(revivalBase, TournP1WinOffset(), P1_WIN_COUNT_SPECTATOR_OFFSET);
                    int t2 = read_win_count(revivalBase, TournP2WinOffset(), P2_WIN_COUNT_SPECTATOR_OFFSET);
                    if ((t1 > 0 || t2 > 0) && t1 <= 99 && t2 <= 99) {
                        efzda::log("[tick=%llu] WINS fallback to tournament offsets (env-enabled): p1=%d p2=%d", ticks(), t1, t2);
                        p1Wins = t1; p2Wins = t2;
                    }
                }
            }
        }
        p1Nick = read_nickname(revivalBase, NickP1Offset(), P1_NICKNAME_SPECTATOR_OFFSET);
        p2Nick = read_nickname(revivalBase, NickP2Offset(), P2_NICKNAME_SPECTATOR_OFFSET);
        selfIdx = read_current_player_index(revivalBase);
    }
    if (p1Wins < 0 || p1Wins > 99) p1Wins = 0;
    if (p2Wins < 0 || p2Wins > 99) p2Wins = 0;
    log("GSPoll#%lu: wins p1=%d p2=%d nicks p1='%s' p2='%s' selfIdx=%d", s_poll, p1Wins, p2Wins, p1Nick.c_str(), p2Nick.c_str(), selfIdx);

    // Online nickname monitoring: if reported online but both nicknames are missing, keep monitoring
    if (onl == OnlineState::Netplay || onl == OnlineState::Spectating || onl == OnlineState::Tournament) {
        bool haveAnyNick = !p1Nick.empty() || !p2Nick.empty();
        if (haveNetplayExport) {
            if (inNetplayMatchState) {
                // During active match, avoid falling back to generic menu text
                // just because nicknames have not populated yet.
                s_waitOnlineNicknames = false;
            } else {
            // When export is present, only wait on nicknames once a session is actually connected.
            s_waitOnlineNicknames =
                !haveAnyNick &&
                np.sessionPhase == EFZ_PHASE_CONNECTED;
            }
        } else {
            s_waitOnlineNicknames = !haveAnyNick;
        }
    } else {
        s_waitOnlineNicknames = false;
    }

    // ONLINE formatting (ignore gmName which often reads VS Human)
    std::string selfNick = (selfIdx == 0 ? p1Nick : selfIdx == 1 ? p2Nick : std::string());
    std::string oppNick = (selfIdx == 0 ? p2Nick : selfIdx == 1 ? p1Nick : std::string());
    if (selfNick.empty() && haveNetplayExport && !np.localNickname.empty()) {
        selfNick = np.localNickname;
    }

    // details: Playing/Watching online match (selfNick if known)
    if (s_waitOnlineNicknames) {
        // Mirror the offline menu mapping using the screen index, to avoid relying on characters
        uint8_t screenIdx = 0xFF;
        bool haveScreen = read_screen_index(efzBase, screenIdx);
        if (haveScreen && s_screenTitle >= 0 && screenIdx == (uint8_t)s_screenTitle) {
            gs.details = "Main Menu";
            gs.largeImageKey = "efz_icon";
            gs.largeImageText = "Main Menu";
            gs.state = "The true Eternal does exists here";
        } else if (haveScreen && s_screenCharSel >= 0 && screenIdx == (uint8_t)s_screenCharSel) {
            // Derive pretty mode locally for online-pending branch
            std::string pm = gmName ? gmName : "";
            if (pm == "Arcade" || pm == "Practice") pm += " Mode";
            if (pm.empty()) pm = "Game";
            gs.details = std::string("Playing in ") + pm;
            // ensure no icons here until selection
            gs.largeImageKey.clear(); gs.largeImageText.clear();
            gs.smallImageKey.clear(); gs.smallImageText.clear();
            // Show neutral scoreboard at char-select even without nicknames
            gs.state = std::string("Score (") + std::to_string(p1Wins) + "-" + std::to_string(p2Wins) + ")";
        } else if (haveScreen && s_screenLoading >= 0 && screenIdx == (uint8_t)s_screenLoading) {
            gs.details = "Loading";
            gs.state = "Loading";
            gs.largeImageKey.clear(); gs.largeImageText.clear();
            gs.smallImageKey.clear(); gs.smallImageText.clear();
        } else if (haveScreen && s_screenSettings >= 0 && screenIdx == (uint8_t)s_screenSettings) {
            gs.details = "Options"; gs.state.clear();
            gs.largeImageKey = "efz_icon"; gs.largeImageText = "Options";
            gs.smallImageKey.clear(); gs.smallImageText.clear();
        } else if (haveScreen && s_screenReplayMenu >= 0 && screenIdx == (uint8_t)s_screenReplayMenu) {
            gs.details = "Replay Selection"; gs.state = "Selecting replay";
        } else {
            gs.details = "In Menus";
        }
        // update last-seen names and mode and return
        s_lastP1Name = p1; s_lastP2Name = p2; s_lastGmRaw = gmRaw;
        log("GSPoll#%lu: online pending nicknames -> details='%s' state='%s'", s_poll, gs.details.c_str(), gs.state.c_str());
        return gs;
    } else if (onl == OnlineState::Spectating) {
        // Spectating: format like replay with nicknames and characters
        gs.details = "Watching online match";
        auto makeSide = [](const std::string& nick, const std::string& chr, const char* fallbackLabel) {
            if (!nick.empty() && !chr.empty()) return nick + " (" + chr + ")";
            if (!nick.empty()) return nick;
            if (!chr.empty()) return chr;
            return std::string(fallbackLabel);
        };
        std::string left = makeSide(p1Nick, p1, "P1");
        std::string right = makeSide(p2Nick, p2, "P2");
        gs.state = left + " vs " + right + " (" + std::to_string(p1Wins) + "-" + std::to_string(p2Wins) + ")";
        // Icons: mirror replay — large=P1 char, small=P2 char
        if (!p1.empty()) {
            std::string kL = map_char_to_large_image_key(p1);
            if (!kL.empty()) { gs.largeImageKey = kL; gs.largeImageText = p1; }
        }
        if (!p2.empty()) {
            std::string kS = map_char_to_small_icon_key(p2);
            if (!kS.empty()) { gs.smallImageKey = kS; gs.smallImageText = p2; }
        }
        log("GSPoll#%lu: spectating -> details='%s' state='%s'", s_poll, gs.details.c_str(), gs.state.c_str());
        s_lastP1Name = p1; s_lastP2Name = p2; s_lastGmRaw = gmRaw;
        return gs;
    } else if (onl == OnlineState::Tournament) {
        gs.details = std::string("Playing tournament match") + (selfNick.empty() ? "" : (" (" + selfNick + ")"));
    } else {
        bool liveOnlineBattleContext =
            inNetplayMatchState ||
            inMatch ||
            spawnedDebounced ||
            (haveTopScreen && s_screenInGame >= 0 && topScreenIdx == (uint8_t)s_screenInGame);
        bool hostPreMatchContext =
            haveNetplayExport &&
            np.sessionMode == EFZ_SESSION_HOSTING &&
            !liveOnlineBattleContext &&
            !inNetplayLoadingState;
        if (hostPreMatchContext) {
            gs.details = selfNick.empty() ? "Hosting" : ("Hosting(" + selfNick + ")");
        } else {
            // Match/live context should mirror vanilla wording for both host and join.
            gs.details = std::string("Playing online match") + (selfNick.empty() ? "" : (" (" + selfNick + ")"));
        }
    }

    // state: Prefer opponent character; if missing but nickname exists, use "Against the <nickname>"; otherwise show waiting message
    const std::string& oppChar = (selfIdx == 1 ? p1 : p2); // if self is P2, opponent is P1; else default P2
    int ourWins = (selfIdx == 1 ? p2Wins : p1Wins);
    int theirWins = (selfIdx == 1 ? p1Wins : p2Wins);
    if (oppChar.empty() && oppNick.empty()) {
        gs.state = "Waiting for the opponent...";
        // Append score even while waiting/at character select
        gs.state += " (" + std::to_string(ourWins) + "-" + std::to_string(theirWins) + ")";
    } else {
        std::string st;
        st.reserve(64);
        st += "Against ";
        if (oppChar.empty() && onl == OnlineState::Netplay && !oppNick.empty()) {
            // Fallback requested: "Against the <nickname>"
            //st += "the ";
            st += oppNick;
        } else {
            st += oppChar.empty() ? std::string("undefined") : oppChar;
            if (!oppNick.empty()) {
                st += " ("; st += oppNick; st += ")";
            }
        }
        // Always show current score, including 0-0 at match start
        st += " (" + std::to_string(ourWins) + "-" + std::to_string(theirWins) + ")";
        gs.state = st;
    }

    std::string oppForIcon = oppChar;
    // If we fell back to nickname and not char, try not to set icon
    if (!oppForIcon.empty()) {
        std::string key = map_char_to_small_icon_key(oppForIcon);
        if (!key.empty()) {
            gs.smallImageKey = key;
            gs.smallImageText = std::string("Against ") + oppForIcon; // tooltip shows the opponent
        }
    }
    // Set large image to our character (based on selfIdx and p1/p2)
    const std::string& ourChar = (selfIdx == 1 ? p2 : p1);
    if (!ourChar.empty()) {
        std::string kL = map_char_to_large_image_key(ourChar);
        if (!kL.empty()) { gs.largeImageKey = kL; gs.largeImageText = ourChar; }
    } else {
        // Pre-pick (no character yet): use the generic EFZ logo as large image
        gs.largeImageKey = "210px-efzlogo";
        gs.largeImageText = "Online Match";
    }
    log("GSPoll#%lu: online -> details='%s' state='%s'", s_poll, gs.details.c_str(), gs.state.c_str());
    // update last-seen names and mode before returning
    s_lastP1Name = p1; s_lastP2Name = p2; s_lastGmRaw = gmRaw;
    return gs;
}

} // namespace efzda
