#pragma once
#include <string>

namespace efzda {

struct GameState {
    std::string details; // e.g., character vs character, score
    std::string state;   // e.g., In Menus, Online, Training
    bool operator==(const GameState &o) const { return details == o.details && state == o.state; }
    bool operator!=(const GameState &o) const { return !(*this == o); }
};

class GameStateProvider {
public:
    // Returns current game state snapshot
    GameState get();
};

}
