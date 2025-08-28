#include "config.h"
#include "logger.h"

#include <windows.h>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace efzda {

// Optional embedded default App ID; can be provided via compile definition EFZDA_DEFAULT_APP_ID
#ifdef EFZDA_DEFAULT_APP_ID
static constexpr const char* EMBEDDED_APP_ID = EFZDA_DEFAULT_APP_ID;
#else
static constexpr const char* EMBEDDED_APP_ID = "";
#endif

static std::string trim(const std::string &s) {
    auto start = s.find_first_not_of(" \t\r\n");
    auto end = s.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start, end - start + 1);
}

Config load_config(const std::wstring &moduleDir) {
    Config cfg{};
    std::wstring path = moduleDir + L"\\discord_app_id.txt";
    std::ifstream in(path);
    if (in) {
        std::stringstream ss;
        ss << in.rdbuf();
        cfg.discordAppId = trim(ss.str());
        if (cfg.discordAppId.empty()) {
            if (EMBEDDED_APP_ID[0] != '\0') {
                cfg.discordAppId = EMBEDDED_APP_ID;
                efzda::log("Config: discord_app_id.txt is empty; using embedded App ID.");
            } else {
                efzda::log("Config: discord_app_id.txt is empty; Discord will be disabled.");
            }
        } else {
            efzda::log("Config: loaded Discord App ID: %s", cfg.discordAppId.c_str());
        }
    } else {
        if (EMBEDDED_APP_ID[0] != '\0') {
            cfg.discordAppId = EMBEDDED_APP_ID;
            efzda::log("Config: discord_app_id.txt not found; using embedded App ID.");
        } else {
            efzda::log("Config: discord_app_id.txt not found; Discord will be disabled.");
        }
    }
    return cfg;
}

} // namespace efzda
