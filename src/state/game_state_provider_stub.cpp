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

// Map display character name to Discord small image asset key (Dev Portal)
static std::string map_char_to_small_icon_key(const std::string& displayName) {
    // Normalize to a tight lowercase key with no spaces/punct
    std::string k; k.reserve(displayName.size());
    for (unsigned char c : displayName) {
        if (std::isalnum(c)) k.push_back((char)std::tolower(c));
    }
    // Handle Nayuki variants by explicit tag
    if (k.find("nayukisleepy") != std::string::npos || k == "neyuki") return "90px-efz_neyuki_icon"; // Sleepy
    if (k.find("nayukiawake") != std::string::npos || k == "nayuki") return "90px-efz_nayuki_icon";   // Awake

    // Doppel vs Rumi
    if (k.find("doppelnanase") != std::string::npos || k == "doppel") return "90px-efz_doppel_icon";
    if (k.find("nanase") != std::string::npos || k.find("rumi") != std::string::npos) return "90px-efz_rumi_icon";

    // Straightforward first-name matches (and some surnames)
    if (k.find("akane") != std::string::npos) return "90px-efz_akane_icon";
    if (k.find("akiko") != std::string::npos) return "90px-efz_akiko_icon";
    if (k.find("ayu")   != std::string::npos) return "90px-efz_ayu_icon";
    if (k.find("ikumi") != std::string::npos) return "90px-efz_ikumi_icon";
    if (k.find("kanna") != std::string::npos) return "90px-efz_kanna_icon_-_copy";
    if (k.find("kano")  != std::string::npos) return "90px-efz_kano_icon";
    if (k.find("kaori") != std::string::npos) return "90px-efz_kaori_icon";
    if (k.find("mai")   != std::string::npos) return "90px-efz_mai_icon";
    if (k.find("makoto")!= std::string::npos) return "90px-efz_makoto_icon";
    if (k.find("mayu")  != std::string::npos) return "90px-efz_mayu_icon";
    if (k.find("minagi")!= std::string::npos) return "90px-efz_minagi_icon";
    if (k.find("mio")   != std::string::npos) return "90px-efz_mio_icon";
    if (k.find("misaki")!= std::string::npos) return "90px-efz_misaki_icon";
    if (k.find("mishio")!= std::string::npos) return "90px-efz_mishio_icon";
    if (k.find("misuzu")!= std::string::npos) return "90px-efz_misuzu_icon";
    if (k.find("nagamori") != std::string::npos) return "90px-efz_mizuka_icon";
    if (k.find("sayuri")!= std::string::npos) return "90px-efz_sayuri_icon";
    if (k.find("shiori")!= std::string::npos) return "90px-efz_shiori_icon";
    if (k.find("mizuka")!= std::string::npos || k.find("mizukab") != std::string::npos) return "90px-efz_unknown_icon";

    // Unknown fallback
    if (k == "unknown") return "90px-efz_unknown_icon";
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
        std::string mode = gmName ? gmName : "";
        if (mode == "Arcade" || mode == "Practice") mode += " Mode";
        if (mode.empty()) mode = "Game";

        if (inMatch) {
            gs.details = std::string("Playing in ") + mode;
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
            // Menu or pre-select
            if (gmName && std::string(gmName) == "Arcade")
                gs.details = "Main Menu";
            else if (gmName)
                gs.details = std::string("Playing in ") + mode;
            else
                gs.details = "In Menus";
            gs.state.clear();
            // Use main icon on the Main Menu
            if (gs.details == "Main Menu") {
                gs.largeImageKey = "efz_icon";
                gs.largeImageText = "Main Menu";
                gs.smallImageKey.clear();
                gs.smallImageText.clear();
            } else {
                gs.largeImageKey.clear(); gs.largeImageText.clear();
                gs.smallImageKey.clear(); gs.smallImageText.clear();
            }
        }
        log("GSPoll#%lu: offline -> details='%s' state='%s'", s_poll, gs.details.c_str(), gs.state.c_str());
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

    // ONLINE formatting (ignore gmName which often reads VS Human)
    std::string selfNick = (selfIdx == 0 ? p1Nick : selfIdx == 1 ? p2Nick : std::string());
    std::string oppNick = (selfIdx == 0 ? p2Nick : selfIdx == 1 ? p1Nick : std::string());

    // details: Playing/Watching online match (selfNick if known)
    if (onl == OnlineState::Spectating) {
        gs.details = "Watching online match"; // spectator has no nickname in memory; omit parens
    } else if (onl == OnlineState::Tournament) {
        gs.details = std::string("Playing tournament match") + (selfNick.empty() ? "" : (" (" + selfNick + ")"));
    } else {
        gs.details = std::string("Playing online match") + (selfNick.empty() ? "" : (" (" + selfNick + ")"));
    }

    // state: Prefer opponent character; if missing in Netplay and we have nickname, use "Against the <nickname>"
    const std::string& oppChar = (selfIdx == 1 ? p1 : p2); // if self is P2, opponent is P1; else default P2
    int ourWins = (selfIdx == 1 ? p2Wins : p1Wins);
    int theirWins = (selfIdx == 1 ? p1Wins : p2Wins);
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
    return gs;
}

} // namespace efzda
