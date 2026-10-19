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

    struct Decision {
        int shift = 0;              // ticks to hold the input back (0 = fire now)
        bool onTimeViable = true;   // the input was fine as pressed
        bool timedOut = false;      // ran out of time/budget while searching
        bool realChanged = false;   // a real player was disturbed by the look-ahead (and restored)
        int steps = 0;
        double ms = 0.0;
        // Debug mode only: predicted positions per tick, one list per moved player.
        std::vector<CCPoint> predicted[2];  // with the chosen timing
        std::vector<CCPoint> onTime[2];     // as pressed (only when that was doomed)
    };

    bool isFake(PlayerObject* player);

    // Called by the PlayLayer hooks.
    void setup(PlayLayer* layer);
    void teardown();
    // Returns true if the death belonged to a hidden player (and must not reach the game).
    bool onDestroyPlayer(PlayerObject* player);

    // Decide how many ticks to hold back an input that changes the button to `down`.
    // `players` are the real players that this input moves (two in one-key dual mode).
    Decision decide(GJBaseGameLayer* layer, std::vector<PlayerObject*> const& players, bool heldNow, bool down);

    // Debug mode: green = predicted path with the chosen timing, red = as pressed.
    void drawDebug(Decision const& decision);
    void clearDebug();
}
