#pragma once
#include <string>

namespace efzda {
struct Config {
    std::string discordAppId;
};

Config load_config(const std::wstring &moduleDir);
}
