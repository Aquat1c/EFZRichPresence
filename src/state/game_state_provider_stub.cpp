// Real EFZ-backed provider implementation (replacing the stub)
#include "state/game_state_provider.h"

#include <windows.h>
#include <cstdint>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <cctype>
#include <type_traits>
#include "logger.h"

namespace efzda {

namespace {
// Offsets derived from efz_streaming
constexpr uintptr_t EFZ_BASE_OFFSET_P1 = 0x390104;
constexpr uintptr_t EFZ_BASE_OFFSET_P2 = 0x390108;
constexpr uintptr_t EFZ_BASE_OFFSET_GAME_STATE = 0x39010C;
constexpr uintptr_t CHARACTER_NAME_OFFSET = 0x94;

constexpr uintptr_t WIN_COUNT_BASE_OFFSET = 0xA02CC; // in EfzRevival.dll
constexpr uintptr_t P1_WIN_COUNT_OFFSET = 0x4C8;
constexpr uintptr_t P2_WIN_COUNT_OFFSET = 0x4CC;
constexpr uintptr_t P1_WIN_COUNT_SPECTATOR_OFFSET = 0x80; // fallback
constexpr uintptr_t P2_WIN_COUNT_SPECTATOR_OFFSET = 0x84; // fallback

// From efz-training-mode
constexpr uintptr_t GAME_MODE_OFFSET = 0x1364; // byte in game state struct
constexpr uintptr_t REVIVAL_ONLINE_STATE_OFFSET = 0xA05D0; // int in EfzRevival.dll

static inline unsigned long long ticks() { return GetTickCount64(); }

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
        {"mizuka", "UNKNOWN"},               // boss version
        {"mizukab", "UNKNOWN"},              // playable version
        {"nanase", "Rumi Nanase"},
        {"exnanase", "Doppel Nanase"},
        {"akane", "Akane Satomura"},
        {"misaki", "Misaki Kawana"},
        {"mayu", "Mayu Shiina"},
        {"mio", "Mio Kouzuki"},
        {"ayu", "Ayu Tsukimiya"},
        {"nayuki", "Nayuki Minase (Asleep)"},
        {"nayukib", "Nayuki Minase (Awake)"},
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

} // namespace

GameState GameStateProvider::get() {
    GameState gs{};
    static unsigned long s_poll = 0;
    ++s_poll;

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

    std::string p1 = read_character_name(efzBase, EFZ_BASE_OFFSET_P1);
    std::string p2 = read_character_name(efzBase, EFZ_BASE_OFFSET_P2);
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
    const char* onlName = online_state_name(onl);
    log("GSPoll#%lu: gameModeRaw=%u gameMode='%s' onlineState='%s'", s_poll, (unsigned)gmRaw, gmName ? gmName : "?", onlName ? onlName : "?");

    bool inMatch = !p1.empty() && !p2.empty();
    if (!inMatch) {
        gs.details = "Idle";
        // Special-case: Arcade with no characters selected means Main Menu
        if (gmName && std::string(gmName) == "Arcade") {
            gs.state = "Main Menu";
            return gs;
        }

        std::string st;
        // If in Replay modes, prefer that label over online/offline
        if (gmName && (std::string(gmName) == "Replay" || std::string(gmName) == "Auto-Replay")) {
            st = gmName;
        } else {
            if (onlName) {
                st += onlName; // Netplay/Spectating/Offline/Tournament
            } else {
                st += "Offline"; // Default safe fallback
            }
            if (gmName) {
                st += " • ";
                st += gmName;
            }
        }
    gs.state = st;
    log("GSPoll#%lu: not in match -> details='%s' state='%s'", s_poll, gs.details.c_str(), gs.state.c_str());
        return gs;
    }

    // Only attempt EfzRevival win reads when online/spectating/tournament
    int p1Wins = 0, p2Wins = 0;
    if (onl == OnlineState::Netplay || onl == OnlineState::Spectating || onl == OnlineState::Tournament) {
        p1Wins = read_win_count(revivalBase, P1_WIN_COUNT_OFFSET, P1_WIN_COUNT_SPECTATOR_OFFSET);
        p2Wins = read_win_count(revivalBase, P2_WIN_COUNT_OFFSET, P2_WIN_COUNT_SPECTATOR_OFFSET);
    }
    if (p1Wins < 0 || p1Wins > 99) p1Wins = 0;
    if (p2Wins < 0 || p2Wins > 99) p2Wins = 0;
    log("GSPoll#%lu: wins p1=%d p2=%d", s_poll, p1Wins, p2Wins);

    // Build strings
    gs.details = p1 + " vs " + p2;
    if (p1Wins > 0 || p2Wins > 0) {
        gs.details += " (" + std::to_string(p1Wins) + "-" + std::to_string(p2Wins) + ")";
    }

    {
        std::string st;
        // If in Replay modes, override label regardless of online state
        if (gmName && (std::string(gmName) == "Replay" || std::string(gmName) == "Auto-Replay")) {
            st = gmName;
        } else {
            switch (onl) {
                case OnlineState::Netplay: st = "Online Match"; break;
                case OnlineState::Spectating: st = "Spectating"; break;
                case OnlineState::Offline: st = "Offline Match"; break;
                case OnlineState::Tournament: st = "Tournament Match"; break;
                default: st = "Match"; break;
            }
            if (gmName) {
                st += " • ";
                st += gmName;
            }
        }
    gs.state = st;
    log("GSPoll#%lu: in match -> details='%s' state='%s'", s_poll, gs.details.c_str(), gs.state.c_str());
    }
    return gs;
}

} // namespace efzda
