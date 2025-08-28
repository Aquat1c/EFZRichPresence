#pragma once
#include <string>

namespace efzda {

struct GameState {
    std::string details; // e.g., character vs character, score
    std::string state;   // e.g., In Menus, Online, Training
    // Optional Discord assets
    std::string largeImageKey;  // Main image (our character)
    std::string largeImageText; // Tooltip for main image
    // Small image (overlay in a small circle)
    std::string smallImageKey;  // Dev Portal asset key, e.g., "90px-efz_akiko_icon"
    std::string smallImageText; // Tooltip, e.g., opponent character name
    bool operator==(const GameState &o) const {
        return details == o.details && state == o.state &&
               largeImageKey == o.largeImageKey && largeImageText == o.largeImageText &&
               smallImageKey == o.smallImageKey && smallImageText == o.smallImageText;
    }
    bool operator!=(const GameState &o) const { return !(*this == o); }
};

class GameStateProvider {
public:
    // Returns current game state snapshot
    GameState get();
};

}
