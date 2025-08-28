// Real EFZ-backed provider implementation (replacing the stub)
#include "state/game_state_provider.h"

#include <windows.h>
#include <cstdint>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <cctype>
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

constexpr uintptr_t WIN_COUNT_BASE_OFFSET = 0xA02CC; // in EfzRevival.dll
constexpr uintptr_t P1_WIN_COUNT_OFFSET = 0x4C8;
constexpr uintptr_t P2_WIN_COUNT_OFFSET = 0x4CC;
constexpr uintptr_t P1_WIN_COUNT_SPECTATOR_OFFSET = 0x80; // fallback
constexpr uintptr_t P2_WIN_COUNT_SPECTATOR_OFFSET = 0x84; // fallback
// Nicknames (wide strings) relative to same base pointer
constexpr uintptr_t P1_NICKNAME_OFFSET = 0x3BE;
constexpr uintptr_t P2_NICKNAME_OFFSET = 0x43E;
constexpr uintptr_t P1_NICKNAME_SPECTATOR_OFFSET = 0x9A;
constexpr uintptr_t P2_NICKNAME_SPECTATOR_OFFSET = 0x11A;
// "Current player" index: 0 = P1, 1 = P2, relative to same base pointer
constexpr uintptr_t CURRENT_PLAYER_OFFSET = 0x2A8;

// From efz-training-mode
constexpr uintptr_t GAME_MODE_OFFSET = 0x1364; // byte in game state struct
constexpr uintptr_t REVIVAL_ONLINE_STATE_OFFSET = 0xA05D0; // int in EfzRevival.dll

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
    const wchar_t* allowed = L" -_.!?+#@$%&*()[]{}:;<>,'\"\\|/~^";
    std::wstring out;
    out.reserve(in.size());
    size_t count = 0;
    for (wchar_t c : in) {
        if (count >= maxLen) break;
        bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
        if (!ok) {
            for (const wchar_t* p = allowed; *p; ++p) { if (c == *p) { ok = true; break; } }
        }
        if (ok) { out.push_back(c); ++count; }
        else {
            if (out.empty() || out.back() != L'_') { out.push_back(L'_'); ++count; }
        }
    }
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
    uintptr_t basePtrAddr = revivalBase + WIN_COUNT_BASE_OFFSET;
    uintptr_t ptr = 0;
    if (!safe_read(reinterpret_cast<void*>(basePtrAddr), ptr)) return 0;
    return ptr;
}

static std::string read_nickname(uintptr_t revivalBase, uintptr_t primaryOff, uintptr_t spectatorOff) {
    uintptr_t ptr = read_revival_ptr(revivalBase);
    if (!ptr) return {};
    std::wstring w;
    // try primary (player) slot
    if (read_wide_string(reinterpret_cast<void*>(ptr + primaryOff), 20, w) && !w.empty()) {
        auto s = narrow(sanitize_nickname_w(w));
        if (!s.empty() && s != "Player" && s != "Player 1" && s != "Player 2") return s;
    }
    // fallback spectator mapping
    w.clear();
    if (read_wide_string(reinterpret_cast<void*>(ptr + spectatorOff), 20, w) && !w.empty()) {
        auto s = narrow(sanitize_nickname_w(w));
        if (!s.empty()) return s;
    }
    return {};
}

static int read_current_player_index(uintptr_t revivalBase) {
    uintptr_t ptr = read_revival_ptr(revivalBase);
    if (!ptr) return -1;
    int idx = -1;
    if (!safe_read(reinterpret_cast<void*>(ptr + CURRENT_PLAYER_OFFSET), idx)) return -1;
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
    // [revivalBase + WIN_COUNT_BASE_OFFSET] -> ptr
    uintptr_t winsBasePtrAddr = revivalBase + WIN_COUNT_BASE_OFFSET;
    uintptr_t winsBase = 0;
    if (!safe_read(reinterpret_cast<void*>(winsBasePtrAddr), winsBase) || winsBase == 0)
        return 0;

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
    int v = -1;
    if (!safe_read(reinterpret_cast<void*>(revivalBase + REVIVAL_ONLINE_STATE_OFFSET), v))
        return OnlineState::Unknown;
    efzda::log("[tick=%llu] ONLINE state raw=%d addr=%p", ticks(), v, (void*)(revivalBase + REVIVAL_ONLINE_STATE_OFFSET));
    switch (v) {
        case 0: return OnlineState::Netplay;
        case 1: return OnlineState::Spectating;
        case 2: return OnlineState::Offline;
        case 3: return OnlineState::Tournament;
        default: return OnlineState::Unknown;
    }
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
                    gs.state = "The true Eternal does exist here";
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
                gs.state = "The true Eternal does exist here";
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
        p1Wins = read_win_count(revivalBase, P1_WIN_COUNT_OFFSET, P1_WIN_COUNT_SPECTATOR_OFFSET);
        p2Wins = read_win_count(revivalBase, P2_WIN_COUNT_OFFSET, P2_WIN_COUNT_SPECTATOR_OFFSET);
        p1Nick = read_nickname(revivalBase, P1_NICKNAME_OFFSET, P1_NICKNAME_SPECTATOR_OFFSET);
        p2Nick = read_nickname(revivalBase, P2_NICKNAME_OFFSET, P2_NICKNAME_SPECTATOR_OFFSET);
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
            gs.state = "The true Eternal does exist here";
        } else if (haveScreen && s_screenCharSel >= 0 && screenIdx == (uint8_t)s_screenCharSel) {
            // Derive pretty mode locally for online-pending branch
            std::string pm = gmName ? gmName : "";
            if (pm == "Arcade" || pm == "Practice") pm += " Mode";
            if (pm.empty()) pm = "Game";
            gs.details = std::string("Playing in ") + pm;
            // ensure no icons here until selection
            gs.largeImageKey.clear(); gs.largeImageText.clear();
            gs.smallImageKey.clear(); gs.smallImageText.clear();
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
        gs.details = "Watching online match"; // spectator has no nickname in memory; omit parens
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
    } else {
        std::string st;
        st.reserve(64);
        st += "Against ";
        if (oppChar.empty() && onl == OnlineState::Netplay && !oppNick.empty()) {
            // Fallback requested: "Against the <nickname>"
            st += "the ";
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
