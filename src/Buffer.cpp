#include "State.hpp"
#include "Sim.hpp"
#include "UI.hpp"

#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

namespace {
    bool g_bypass = false;      // our own re-injected inputs skip the buffer
    constexpr size_t kMaxQueue = 16;

    void fire(GJBaseGameLayer* layer, Channel& ch, bool down, bool isPlayer1) {
        g_bypass = true;
        layer->handleButton(down, static_cast<int>(PlayerButton::Jump), isPlayer1);
        g_bypass = false;
        ch.held = down;
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

        int shift = 0;
        if (down != ch.held) {
            auto players = movedPlayers(pl, isPlayer1);
            if (!players.empty()) shift = sim::decideShift(this, players, ch.held, down);
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
        if (pl && static_cast<GJBaseGameLayer*>(pl) == this) {
            auto& rt = ibRuntime();
            rt.tick++;
            if (ibConfig().enabled) rt.usedThisAttempt = true;

            for (int i = 0; i < 2; ++i) {
                auto& ch = rt.channels[i];
                while (!ch.queue.empty() && ch.queue.front().fireTick <= rt.tick) {
                    auto input = ch.queue.front();
                    ch.queue.pop_front();
                    fire(this, ch, input.down, i == 0);
                }
                if (ch.queue.empty()) ch.lastShift = 0;
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(IBPlayLayer, PlayLayer) {
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
        ui::addIndicator(this);
        announceState(false);
        return true;
    }

    void resetLevel() {
        settleChannels(this);
        PlayLayer::resetLevel();
        ibRuntime().usedThisAttempt = ibConfig().enabled;
    }

    void onQuit() {
        ibRuntime().clearQueues();
        sim::teardown();
        PlayLayer::onQuit();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (sim::onDestroyPlayer(player)) return;
        if (sim::active) return;
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
