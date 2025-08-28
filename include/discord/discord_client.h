#pragma once
#include <string>

namespace efzda {

class DiscordClient {
public:
    bool init(const std::string &appId);
    void updatePresence(const std::string &details, const std::string &state,
                        const std::string &smallImageKey = std::string(),
                        const std::string &smallImageText = std::string(),
                        const std::string &largeImageKey = std::string(),
                        const std::string &largeImageText = std::string());
    // Run Discord callbacks; call periodically from a loop.
    void poll();
    void clearPresence();
    void shutdown();
};

}
