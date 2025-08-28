// Discord Rich Presence via native IPC (named pipe) compatible with newer Discord clients
#include "discord/discord_client.h"
#include "logger.h"
#include <windows.h>
#include <string>
#include <vector>
#include <algorithm>
#include <objbase.h>
#include <rpc.h>
#pragma comment(lib, "Rpcrt4.lib")

namespace efzda {

static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static std::string g_appId;

static std::string escape_json(const std::string& in) {
    std::string out; out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

static bool write_frame(uint32_t op, const std::string& json) {
    if (g_pipe == INVALID_HANDLE_VALUE) return false;
    struct Header { uint32_t op; uint32_t len; } hdr{ op, static_cast<uint32_t>(json.size()) };
    DWORD written = 0;
    if (!WriteFile(g_pipe, &hdr, sizeof(hdr), &written, nullptr) || written != sizeof(hdr))
        return false;
    if (!WriteFile(g_pipe, json.data(), (DWORD)json.size(), &written, nullptr) || written != json.size())
        return false;
    return true;
}

static bool connect_pipe() {
    // Try discord-ipc-0..9
    for (int i = 0; i < 10; ++i) {
        wchar_t name[64];
        swprintf_s(name, L"\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            g_pipe = h;
            return true;
        }
    }
    return false;
}

static std::string new_nonce() {
    UUID uuid{};
    if (UuidCreate(&uuid) != RPC_S_OK && UuidCreateSequential(&uuid) != RPC_S_OK) {
        // Fallback: timestamp-based
        return std::to_string(GetTickCount64());
    }
    RPC_CSTR sz = nullptr;
    if (UuidToStringA(&uuid, &sz) == RPC_S_OK && sz) {
        std::string s(reinterpret_cast<char*>(sz));
        RpcStringFreeA(&sz);
        // remove braces if present
        s.erase(std::remove(s.begin(), s.end(), '{'), s.end());
        s.erase(std::remove(s.begin(), s.end(), '}'), s.end());
        return s;
    }
    return std::to_string(GetTickCount64());
}

bool DiscordClient::init(const std::string &appId) {
    if (appId.empty()) {
        log("Discord IPC: No App ID; Rich Presence disabled.");
        return false;
    }
    g_appId = appId;
    if (!connect_pipe()) {
        log("Discord IPC: Could not connect to Discord named pipe.");
        return false;
    }
    // Handshake (OP 0)
    std::string hs = std::string("{\"v\": 1, \"client_id\": \"") + g_appId + "\"}";
    if (!write_frame(0, hs)) {
        log("Discord IPC: Handshake write failed.");
        CloseHandle(g_pipe); g_pipe = INVALID_HANDLE_VALUE; return false;
    }
    log("Discord IPC: Initialized (AppID=%s)", g_appId.c_str());
    return true;
}

void DiscordClient::updatePresence(const std::string &details, const std::string &state) {
    if (g_pipe == INVALID_HANDLE_VALUE) return;
    std::string nonce = new_nonce();
    // Note: The app name in Discord developer portal is the primary label. We also set assets.large_text as a visible hover text.
    std::string json = std::string("{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":")
        + std::to_string(GetCurrentProcessId()) + ",\"activity\":{\"details\":\"" + escape_json(details) +
        "\",\"state\":\"" + escape_json(state) +
        "\",\"assets\":{\"large_text\":\"Eternal Fighter Zero\"}}} ,\"nonce\":\"" + nonce + "\"}";
    if (!write_frame(1, json)) {
        log("Discord IPC: SET_ACTIVITY write failed; attempting reconnect");
        CloseHandle(g_pipe); g_pipe = INVALID_HANDLE_VALUE; connect_pipe();
    }
}

void DiscordClient::poll() {
    // Optional: could read responses; we keep it as a no-op
}

void DiscordClient::clearPresence() {
    if (g_pipe == INVALID_HANDLE_VALUE) return;
    std::string nonce = new_nonce();
    std::string json = std::string("{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":")
        + std::to_string(GetCurrentProcessId()) + ",\"activity\":null},\"nonce\":\"" + nonce + "\"}";
    write_frame(1, json);
}

void DiscordClient::shutdown() {
    if (g_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
    }
}

} // namespace efzda
