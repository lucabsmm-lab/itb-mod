#pragma once

#include "State.hpp"
#include <vector>

// Look-ahead simulation.
//
// We keep two hidden PlayerObjects (one per real player). To test a timing we copy the
// real player into a hidden one and step it forward with the game's own physics and
// collision code. While that runs, hooks in Sim.cpp block anything that would leak into
// the real level (triggers, coins, effects, portals that change level state, death).
namespace sim {
    // True while a hidden player is being stepped.
    extern bool active;

    bool isFake(PlayerObject* player);

    // Called by the PlayLayer hooks.
    void setup(PlayLayer* layer);
    void teardown();
    // Returns true if the death belonged to a hidden player (and must not reach the game).
    bool onDestroyPlayer(PlayerObject* player);

    // How many ticks (0 = none) to hold back an input that changes the button to `down`.
    // `players` are the real players that this input moves (two in one-key dual mode).
    int decideShift(GJBaseGameLayer* layer, std::vector<PlayerObject*> const& players, bool heldNow, bool down);
}
