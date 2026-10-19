#pragma once

#include <Geode/Geode.hpp>
#include <deque>

using namespace geode::prelude;

// Cached copy of the mod settings (read every tick, so we avoid setting lookups).
struct BufferConfig {
    bool enabled = true;
    int bufferTicks = 10;
    bool safest = true;        // timing style: true = Safest, false = Earliest
    bool safeMode = true;
    bool showIndicator = true;
    std::string indicatorPos = "Top Left";
    bool hotkeys = true;
    int assumedHold = 12;
    int flyWindow = 30;
    int simBudget = 12000;
};

BufferConfig& ibConfig();

// Setters that also write the Geode setting (so the settings menu stays in sync).
void setEnabled(bool on);
void setBufferTicks(int ticks);

// An input waiting to be released into the game.
struct PendingInput {
    bool down;
    int fireTick;
};

// One input channel = one "isPlayer1" side of GJBaseGameLayer::handleButton.
struct Channel {
    std::deque<PendingInput> queue;
    int lastShift = 0;
    bool held = false;      // what the game currently believes (after buffering)
    bool physical = false;  // what the player is actually pressing right now
};

struct BufferRuntime {
    int tick = 0;
    Channel channels[2];
    bool usedThisAttempt = false;
    float lastDt = 0.25f;  // GD 2.2 physics step (in 60fps units) at 240 TPS

    // Indicator feedback
    float bannerTimer = 0.f;     // > 0: show "ON"/"OFF" banner instead of ticks
    float flashTimer = 0.f;      // > 0: an input was just buffered
    int lastBufferedBy = 0;

    void clearQueues();
};

BufferRuntime& ibRuntime();

// Shows the ON/OFF banner on the indicator (and a notification outside levels).
void announceState(bool alsoNotify);
