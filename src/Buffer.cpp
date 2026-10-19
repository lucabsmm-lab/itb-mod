#include "State.hpp"
#include "Sim.hpp"
#include "UI.hpp"

#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <cmath>

namespace {
    bool g_bypass = false;      // our own re-injected inputs skip the buffer
    constexpr size_t kMaxQueue = 16;

    // Debug mode: compares the real run, tick by tick, with what the look-ahead predicted.
    // GD applies an input to the *next* tick's movement, so predicted[0] lines up with the
    // real position one tick after the decision.
    struct DriftTrack {
        bool active = false;
        int startTick = 0;
        int channel = 0;
        int shift = 0;
        int length = 0;
        std::vector<PlayerObject*> players;
        std::vector<CCPoint> predicted[2];
        float maxErr[3] = { 0.f, 0.f, 0.f };  // real vs predicted one tick behind / same / ahead
        int firstBad = -1;
        int compared = 0;
    };
    DriftTrack g_drift;

    void finishDrift(char const* why) {
        if (!g_drift.active) return;
        g_drift.active = false;
        auto const& e = g_drift.maxErr;
        char const* verdict = "on time";
        if (e[1] > 2.f && e[0] < e[1] && e[0] <= e[2]) verdict = "LANDED 1 TICK LATE";
        else if (e[1] > 2.f && e[2] < e[1] && e[2] < e[0]) verdict = "LANDED 1 TICK EARLY";
        log::info(
            "[IB] drift check ended ({}): {} ticks compared | max error {:.2f} units (first tick off by more than 2: {}) | if 1 tick late {:.2f}, if 1 tick early {:.2f} -> {}",
            why, g_drift.compared, e[1], g_drift.firstBad, e[0], e[2], verdict
        );
    }

    void startDrift(sim::Decision const& decision, std::vector<PlayerObject*> const& players, int channel) {
        finishDrift("next input");
        g_drift = DriftTrack {};
        g_drift.active = true;
        g_drift.startTick = ibRuntime().tick;
        g_drift.channel = channel;
        g_drift.shift = decision.shift;
        g_drift.length = decision.shift + ibConfig().assumedHold;
        g_drift.players = players;
        for (int i = 0; i < 2; ++i) g_drift.predicted[i] = decision.predicted[i];
    }

    void stepDrift() {
        if (!g_drift.active) return;
        int index = ibRuntime().tick - g_drift.startTick - 1;
        if (index < 0) return;
        for (size_t p = 0; p < g_drift.players.size() && p < 2; ++p) {
            auto const& path = g_drift.predicted[p];
            if (index >= static_cast<int>(path.size())) continue;
            auto real = g_drift.players[p]->getPosition();
            for (int o = -1; o <= 1; ++o) {
                int i = std::clamp(index + o, 0, static_cast<int>(path.size()) - 1);
                float err = std::hypot(real.x - path[i].x, real.y - path[i].y);
                g_drift.maxErr[o + 1] = std::max(g_drift.maxErr[o + 1], err);
                if (o == 0 && err > 2.f && g_drift.firstBad < 0) g_drift.firstBad = index;
            }
        }
        g_drift.compared = index + 1;
        if (index >= g_drift.length) finishDrift("covered delay + assumed hold");
    }

    void logDecision(sim::Decision const& d, bool down, bool isPlayer1, size_t playerCount) {
        log::info(
            "[IB] {} (P{} key){}: as pressed {} -> hold back {} ticks | {} sim ticks in {:.2f} ms (dt {:.4f}){}{}",
            down ? "press" : "release", isPlayer1 ? 1 : 2, playerCount == 2 ? " (dual, both players)" : "",
            d.onTimeViable ? "survives" : "DOOMED", d.shift, d.steps, d.ms, ibRuntime().lastDt,
            d.timedOut ? " | TIME LIMIT HIT" : "",
            d.realChanged ? " | WARNING: real player was disturbed by the look-ahead (restored)" : ""
        );
    }

    void fire(GJBaseGameLayer* layer, Channel& ch, bool down, bool isPlayer1) {
        g_bypass = true;
        layer->handleButton(down, static_cast<int>(PlayerButton::Jump), isPlayer1);
        g_bypass = false;
        ch.held = down;
    }

    // Releases held-back inputs whose tick has come. `where` is only for the debug log.
    void fireDue(GJBaseGameLayer* layer, char const* where) {
        auto& rt = ibRuntime();
        for (int i = 0; i < 2; ++i) {
            auto& ch = rt.channels[i];
            while (!ch.queue.empty() && ch.queue.front().fireTick <= rt.tick) {
                auto input = ch.queue.front();
                ch.queue.pop_front();
                if (ibConfig().debug) {
                    log::info("[IB] released held-back {} at tick {} (due {}) via {}",
                        input.down ? "press" : "release", rt.tick, input.fireTick, where);
                }
                fire(layer, ch, input.down, i == 0);
            }
            if (ch.queue.empty()) ch.lastShift = 0;
        }
    }

    void flush(GJBaseGameLayer* layer, Channel& ch, bool isPlayer1) {
        while (!ch.queue.empty()) {
            auto input = ch.queue.front();
            ch.queue.pop_front();
            fire(layer, ch, input.down, isPlayer1);
        }
        ch.lastShift = 0;
    }

    bool bufferingAllowed(PlayLayer* pl) {
        return ibConfig().enabled
            && !pl->m_isPlatformer
            && !pl->m_hasCompletedLevel
            && pl->m_player1 && !pl->m_player1->m_isDead;
    }

    // Which real players a jump input on this side moves.
    std::vector<PlayerObject*> movedPlayers(PlayLayer* pl, bool isPlayer1) {
        bool dual = pl->m_gameState.m_isDualMode && pl->m_player2;
        bool twoPlayer = pl->m_levelSettings && pl->m_levelSettings->m_twoPlayerMode;
        if (!dual) {
            if (twoPlayer && !isPlayer1) return {};
            return { pl->m_player1 };
        }
        if (twoPlayer) return { isPlayer1 ? pl->m_player1 : pl->m_player2 };
        return { pl->m_player1, pl->m_player2 };  // one key drives both
    }

    // Before a respawn, make sure the game's button state matches the real key.
    void settleChannels(GJBaseGameLayer* layer) {
        auto& rt = ibRuntime();
        for (int i = 0; i < 2; ++i) {
            auto& ch = rt.channels[i];
            ch.queue.clear();
            ch.lastShift = 0;
            if (ch.held != ch.physical) fire(layer, ch, ch.physical, i == 0);
        }
    }

    // Holding through a landing makes GD jump again the moment you touch down. That jump is not
    // a click, so treat it like one: if it would doom you, let go for you and press again on the
    // first tick that survives (you keep holding the real button the whole time).
    void checkHeldLandings(GJBaseGameLayer* layer, PlayLayer* pl) {
        if (!bufferingAllowed(pl)) return;
        auto& rt = ibRuntime();
        for (int i = 0; i < 2; ++i) {
            auto& ch = rt.channels[i];
            if (!ch.held || !ch.physical || !ch.queue.empty()) continue;
            if (rt.tick - ch.holdCheckTick < 3) continue;  // one check per landing

            auto players = movedPlayers(pl, i == 0);
            if (players.empty() || !sim::landingSoon(layer, players)) continue;
            ch.holdCheckTick = rt.tick;

            // "Let go now, press again after N ticks"; N = 0 is the same as holding through.
            auto decision = sim::decide(layer, players, false, true);
            if (ibConfig().debug) {
                log::info("[IB] holding into a landing (P{} key): auto-jump {} -> {}",
                    i == 0 ? 1 : 2, decision.onTimeViable ? "survives" : "DOOMED",
                    decision.onTimeViable || decision.shift == 0
                        ? std::string("left alone")
                        : fmt::format("let go and re-press in {} ticks", decision.shift));
                sim::drawDebug(decision);
                startDrift(decision, players, i);
            }
            if (decision.onTimeViable || decision.shift <= 0) continue;

            fire(layer, ch, false, i == 0);
            // Letting go must also cancel a buffered jump, or GD still jumps on touchdown.
            for (auto p : players) p->m_jumpBuffered = false;
            ch.queue.push_back({ true, rt.tick + decision.shift });
            ch.lastShift = decision.shift;
            rt.flashTimer = .35f;
            rt.lastBufferedBy = decision.shift;
        }
    }

    // Safe mode: run progress-saving code as if this were a start-pos test run.
    struct SafeModeGuard {
        PlayLayer* layer;
        bool active;
        bool oldTestMode = false;

        explicit SafeModeGuard(PlayLayer* pl)
          : layer(pl), active(ibConfig().safeMode && ibRuntime().usedThisAttempt) {
            if (active) {
                oldTestMode = layer->m_isTestMode;
                layer->m_isTestMode = true;
            }
        }
        ~SafeModeGuard() {
            if (active) layer->m_isTestMode = oldTestMode;
        }
    };
}

class $modify(IBGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        auto pl = PlayLayer::get();
        if (g_bypass || sim::active || button != static_cast<int>(PlayerButton::Jump)
            || !pl || static_cast<GJBaseGameLayer*>(pl) != this) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        auto& rt = ibRuntime();
        auto& ch = rt.channels[isPlayer1 ? 0 : 1];
        ch.physical = down;

        if (!bufferingAllowed(pl)) {
            flush(this, ch, isPlayer1);
            fire(this, ch, down, isPlayer1);
            return;
        }

        // Something is already waiting: keep the order and the same spacing you pressed with,
        // so a quick tap keeps its length and spam stays in rhythm.
        if (!ch.queue.empty()) {
            int fireTick = std::max(rt.tick + ch.lastShift, ch.queue.back().fireTick + 1);
            ch.queue.push_back({ down, fireTick });
            if (ch.queue.size() > kMaxQueue) flush(this, ch, isPlayer1);
            return;
        }

        if (g_drift.active && g_drift.channel == (isPlayer1 ? 0 : 1)) finishDrift("next input");

        int shift = 0;
        if (down != ch.held) {
            auto players = movedPlayers(pl, isPlayer1);
            if (!players.empty()) {
                auto decision = sim::decide(this, players, ch.held, down);
                shift = decision.shift;
                if (ibConfig().debug) {
                    logDecision(decision, down, isPlayer1, players.size());
                    sim::drawDebug(decision);
                    startDrift(decision, players, isPlayer1 ? 0 : 1);
                }
                else if (decision.realChanged) {
                    log::warn("[IB] look-ahead disturbed the real player (restored)");
                }
            }
        }

        if (shift <= 0) {
            fire(this, ch, down, isPlayer1);
            return;
        }

        ch.queue.push_back({ down, rt.tick + shift });
        ch.lastShift = shift;
        rt.flashTimer = .35f;
        rt.lastBufferedBy = shift;
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        auto pl = PlayLayer::get();
        bool ours = pl && static_cast<GJBaseGameLayer*>(pl) == this;
        if (ours) {
            auto& rt = ibRuntime();
            rt.tick++;
            if (ibConfig().enabled) rt.usedThisAttempt = true;
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);

        if (ours) {
            // Normally already released inside processQueuedButtons; this only catches a
            // tick where GD skipped that step.
            fireDue(this, "end of tick (fallback)");
            stepDrift();
        }
    }

    // GD hands real clicks to handleButton from here, so held-back inputs go in at the same
    // point: they then move the player on exactly the tick the look-ahead simulated.
    void processQueuedButtons(float dt, bool clearInputQueue) {
        auto pl = PlayLayer::get();
        if (pl && static_cast<GJBaseGameLayer*>(pl) == this) {
            fireDue(this, "input queue");
            checkHeldLandings(this, pl);
        }
        GJBaseGameLayer::processQueuedButtons(dt, clearInputQueue);
    }
};

class $modify(IBPlayLayer, PlayLayer) {
    // Hidden look-ahead players must be caught here before any other mod sees them "die".
    static void onModify(auto& self) {
        for (auto name : { "PlayLayer::destroyPlayer", "PlayLayer::levelComplete",
                           "PlayLayer::playEndAnimationToPos", "PlayLayer::checkpointActivated" }) {
            (void)self.setHookPriority(name, Priority::First);
        }
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        auto& rt = ibRuntime();
        rt.clearQueues();
        for (auto& ch : rt.channels) {
            ch.held = false;
            ch.physical = false;
        }
        rt.tick = 0;
        rt.usedThisAttempt = ibConfig().enabled;

        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        sim::setup(this);
        sim::onAttemptStart();
        ui::addIndicator(this);
        announceState(false);
        return true;
    }

    void resetLevel() {
        finishDrift("level reset");
        sim::clearDebug();
        settleChannels(this);
        PlayLayer::resetLevel();
        sim::onAttemptStart();
        ibRuntime().usedThisAttempt = ibConfig().enabled;
    }

    void onQuit() {
        g_drift = DriftTrack {};
        ibRuntime().clearQueues();
        sim::teardown();
        PlayLayer::onQuit();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (sim::onDestroyPlayer(player)) return;
        if (sim::active) return;
        if (ibConfig().debug && g_drift.active) {
            log::info("[IB] real player died {} ticks after the last decision (held back {} ticks)",
                ibRuntime().tick - g_drift.startTick, g_drift.shift);
            finishDrift("death");
        }
        SafeModeGuard guard(this);
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        if (sim::active) return;
        SafeModeGuard guard(this);
        PlayLayer::levelComplete();
    }

    void playEndAnimationToPos(cocos2d::CCPoint position) {
        if (sim::active) return;
        PlayLayer::playEndAnimationToPos(position);
    }

    void checkpointActivated(CheckpointGameObject* object) {
        if (sim::active) return;
        PlayLayer::checkpointActivated(object);
    }
};
