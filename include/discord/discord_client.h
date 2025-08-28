#pragma once
#include <string>

namespace efzda {

class DiscordClient {
public:
    bool init(const std::string &appId);
    void updatePresence(const std::string &details, const std::string &state);
    // Run Discord callbacks; call periodically from a loop.
    void poll();
    void clearPresence();
    void shutdown();
};

}
