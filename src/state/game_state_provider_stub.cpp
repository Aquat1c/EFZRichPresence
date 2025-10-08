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
#include "logger.h"

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
// 1.02e: wins-base ptr RVA=0x00A02CC, online-state RVA=0x00A05D0
// 1.02h: wins-base ptr RVA=0x00A02EC, online-state RVA=0x00A05F0
// 1.02i: wins-base ptr RVA=0x00A15F8, online-state RVA=0x00A15FC
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
enum class EfzRevivalVersion : int { Unknown = 0, Vanilla, Revival102e, Revival102h, Revival102i, Other };

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
    // default to 1.02h layout for others (including 1.02e observed the same here)
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

    // Determine module bases
    uintptr_t efzBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); // main module (efz.exe) in same process
    HMODULE revivalMod = GetModuleHandleW(L"EfzRevival.dll");
    uintptr_t revivalBase = reinterpret_cast<uintptr_t>(revivalMod);
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
    log("GSPoll#%lu: gameModeRaw=%u gameMode='%s' onlineState='%s'", s_poll, (unsigned)gmRaw, gmName ? gmName : "?", onlName ? onlName : "?");
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
    if (onl == OnlineState::Netplay || onl == OnlineState::Spectating || onl == OnlineState::Tournament) {
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
        if (!haveAnyNick) {
            s_waitOnlineNicknames = true;
        } else {
            s_waitOnlineNicknames = false;
        }
    } else {
        s_waitOnlineNicknames = false;
    }

    // ONLINE formatting (ignore gmName which often reads VS Human)
    std::string selfNick = (selfIdx == 0 ? p1Nick : selfIdx == 1 ? p2Nick : std::string());
    std::string oppNick = (selfIdx == 0 ? p2Nick : selfIdx == 1 ? p1Nick : std::string());

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
        gs.details = std::string("Playing online match") + (selfNick.empty() ? "" : (" (" + selfNick + ")"));
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
