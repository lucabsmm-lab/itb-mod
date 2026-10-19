#include "Sim.hpp"

#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>

#include <unordered_map>

namespace {
    Ref<PlayerObject> g_fakes[2];

    bool g_dead = false;   // a hidden player died this branch
    bool g_stop = false;   // the branch hit something we don't simulate (portal, end, ...)

    // Activation flags of objects the hidden player touched, restored after each branch.
    struct SavedFlags {
        bool p1;
        bool p2;
    };
    std::unordered_map<EffectGameObject*, SavedFlags> g_touched;

    void remember(EffectGameObject* obj) {
        if (!obj || g_touched.contains(obj)) return;
        g_touched.emplace(obj, SavedFlags { obj->m_activatedByPlayer1, obj->m_activatedByPlayer2 });
    }

    enum class Outcome { Died, Survived, Unknown };

    // Plan for one simulated future of a single input.
    //  ticks [0, delay)                  : button stays as it is now
    //  tick  delay                       : the input happens
    //  ticks (delay, delay + keepFor]    : you keep it (assumed hold / assumed release)
    //  tick  delay + keepFor + toggleAfter: you flip the button back (toggleAfter < 0 = never)
    // After the last change we require survival until control is regained:
    //  cube/ball/robot/spider -> until touching ground again, fly modes -> `flyWindow` ticks.
    struct Plan {
        int delay;
        bool down;
        int keepFor;
        int toggleAfter;
    };

    // Toggle-back offsets tried for every candidate. Order matters for the early exit:
    // the most natural continuation goes first.
    constexpr int kPressBranches[] = { 0, -1, 6, 12, 24, 48 };
    constexpr int kReleaseBranches[] = { -1, 0, 6, 12, 24, 48 };
    constexpr int kBranchCount = 6;
    constexpr int kGroundSettleMax = 240;  // stop looking for a landing after 1s
    constexpr int kSafestSpan = 6;         // Safest mode scores this many ticks past the first safe one

    bool isFlyMode(PlayerObject* p) {
        return p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing;
    }

    void setButton(PlayerObject* p, bool down) {
        if (down) p->pushButton(PlayerButton::Jump);
        else p->releaseButton(PlayerButton::Jump);
    }

    void prepare(PlayerObject* fake, PlayerObject* real, bool held) {
        fake->spawnFromPlayer(real, false);
        fake->toggleVisibility(false);
        fake->setVisible(false);
        fake->m_playEffects = false;
        fake->m_gravityMod = real->m_gravityMod;
        fake->m_isOnGround = real->m_isOnGround;
        fake->m_isOnGround2 = real->m_isOnGround2;
        fake->m_isSecondPlayer = real->m_isSecondPlayer;
        fake->m_touchingRings->removeAllObjects();
        fake->m_touchedRings = real->m_touchedRings;
        fake->resetCollisionLog(true);
        if (held) fake->pushButton(PlayerButton::Jump);
        // Holding is not the same as a fresh click: keep the real buffered-jump state.
        fake->m_jumpBuffered = real->m_jumpBuffered;
        fake->m_stateRingJump = real->m_stateRingJump;
        fake->m_stateRingJump2 = real->m_stateRingJump2;
        fake->m_ringJumpRelated = real->m_ringJumpRelated;
    }

    void restoreWorld(GJBaseGameLayer* layer, PlayerObject* fake) {
        for (auto& [obj, flags] : g_touched) {
            obj->m_activatedByPlayer1 = flags.p1;
            obj->m_activatedByPlayer2 = flags.p2;
        }
        g_touched.clear();

        auto& activated = layer->m_gameState.m_activatedObjectIDs;
        int fakeID = fake->m_uniqueID;
        for (auto it = activated.begin(); it != activated.end();) {
            if (it->first.second == fakeID) it = activated.erase(it);
            else ++it;
        }

        fake->stopParticles();
        if (fake->m_isDashing) fake->stopDashing();
    }

    Outcome runBranch(GJBaseGameLayer* layer, PlayerObject* fake, PlayerObject* real, bool heldNow, Plan const& plan, int& budget) {
        auto const& cfg = ibConfig();
        g_dead = false;
        g_stop = false;

        prepare(fake, real, heldNow);

        float dt = ibRuntime().lastDt;
        int decisionEnd = plan.delay + plan.keepFor;
        int toggleAt = plan.toggleAfter >= 0 ? decisionEnd + plan.toggleAfter : -1;
        int settleStart = toggleAt >= 0 ? toggleAt : decisionEnd;
        int maxT = settleStart + std::max(cfg.flyWindow, kGroundSettleMax);

        Outcome result = Outcome::Survived;
        for (int t = 0; t <= maxT; ++t) {
            if (--budget < 0) {
                result = Outcome::Unknown;
                break;
            }

            fake->resetCollisionLog(false);
            layer->checkCollisions(fake, dt, false);
            if (g_dead) {
                result = Outcome::Died;
                break;
            }
            if (g_stop) break;

            if (t > settleStart) {
                int since = t - settleStart;
                if (isFlyMode(fake)) {
                    if (since >= cfg.flyWindow) break;
                }
                else if (fake->m_isOnGround) {
                    // Landed: you are back in control, the next click is yours.
                    break;
                }
            }

            if (t == plan.delay) setButton(fake, plan.down);
            if (t == toggleAt) setButton(fake, !plan.down);

            fake->update(dt);
            fake->updateSpecial(dt);
            fake->updateRotation(dt);
        }

        restoreWorld(layer, fake);
        return result;
    }

    struct Evaluator {
        GJBaseGameLayer* layer;
        std::vector<std::pair<PlayerObject*, PlayerObject*>> pairs;  // (real, fake)
        bool heldNow;
        bool down;
        int budget;

        int branchOffset(int i) const {
            return down ? kPressBranches[i] : kReleaseBranches[i];
        }

        // A branch counts only if every moved player survives it (one key moves both in dual).
        Outcome runShared(int delay, int branch) {
            Plan plan { delay, down, ibConfig().assumedHold, branchOffset(branch) };
            Outcome worst = Outcome::Survived;
            for (auto& [real, fake] : pairs) {
                auto out = runBranch(layer, fake, real, heldNow, plan, budget);
                if (out == Outcome::Died) return Outcome::Died;
                if (out == Outcome::Unknown) worst = Outcome::Unknown;
            }
            return worst;
        }

        // Is there any way to continue after this timing? Unknown (budget ran out) counts
        // as safe only for the on-time input, so we never add delay on a guess.
        bool viable(int delay) {
            for (int b = 0; b < kBranchCount; ++b) {
                auto out = runShared(delay, b);
                if (out == Outcome::Survived) return true;
                if (out == Outcome::Unknown) return delay == 0;
            }
            return false;
        }

        // How forgiving is this timing: number of continuations that survive.
        int score(int delay) {
            int count = 0;
            for (int b = 0; b < kBranchCount; ++b) {
                if (runShared(delay, b) == Outcome::Survived) ++count;
                if (budget <= 0) break;
            }
            return count;
        }
    };

    struct ActiveGuard {
        ActiveGuard() { sim::active = true; }
        ~ActiveGuard() { sim::active = false; }
    };
}

namespace sim {
    bool active = false;

    bool isFake(PlayerObject* player) {
        return player && (player == g_fakes[0].data() || player == g_fakes[1].data());
    }

    void setup(PlayLayer* layer) {
        teardown();
        for (auto& fake : g_fakes) {
            auto p = PlayerObject::create(1, 1, layer, layer, true);
            p->setPosition({ 0.f, 105.f });
            p->setVisible(false);
            layer->m_objectLayer->addChild(p);
            fake = p;
        }
    }

    void teardown() {
        for (auto& fake : g_fakes) {
            if (fake) fake->removeFromParent();
            fake = nullptr;
        }
        g_touched.clear();
    }

    bool onDestroyPlayer(PlayerObject* player) {
        if (!active || !isFake(player)) return false;
        g_dead = true;
        return true;
    }

    int decideShift(GJBaseGameLayer* layer, std::vector<PlayerObject*> const& players, bool heldNow, bool down) {
        auto const& cfg = ibConfig();
        if (players.empty() || players.size() > 2) return 0;

        Evaluator ev { layer, {}, heldNow, down, cfg.simBudget };
        for (size_t i = 0; i < players.size(); ++i) {
            auto real = players[i];
            auto fake = g_fakes[real == layer->m_player2 ? 1 : 0].data();
            if (!real || !fake || real->m_isDead) return 0;
            ev.pairs.emplace_back(real, fake);
        }

        ActiveGuard guard;

        // On time and survivable: never touch it (keeps spam / straight-fly exactly normal).
        if (ev.viable(0)) return 0;

        int first = -1;
        for (int d = 1; d <= cfg.bufferTicks && ev.budget > 0; ++d) {
            if (ev.viable(d)) {
                first = d;
                break;
            }
        }
        if (first < 0) return 0;  // nothing in range saves it: act normally
        if (!cfg.safest) return first;

        // Path of least resistance: the timing that leaves the most ways to keep going.
        // Ties go to the earliest tick that still has a safe neighbour (1 tick margin).
        int last = std::min(cfg.bufferTicks, first + kSafestSpan);
        std::vector<int> scores(last + 2, -1);
        int best = 0;
        for (int d = first; d <= last && ev.budget > 0; ++d) {
            scores[d] = ev.score(d);
            best = std::max(best, scores[d]);
        }
        if (best <= 0) return first;
        int pick = -1;
        for (int d = first; d <= last; ++d) {
            if (scores[d] != best) continue;
            if (scores[d + 1] == best) return d;
            if (pick < 0) pick = d;
        }
        return pick > 0 ? pick : first;
    }
}

// ---------------------------------------------------------------------------
// Side-effect guards while simulating
// ---------------------------------------------------------------------------

class $modify(IBSimGameLayer, GJBaseGameLayer) {
    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (sim::active) remember(object);
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void bumpPlayer(PlayerObject* player, EffectGameObject* object) {
        if (sim::active) remember(object);
        GJBaseGameLayer::bumpPlayer(player, object);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* object) {
        if (sim::active) remember(object);
        GJBaseGameLayer::playerTouchedRing(player, object);
    }

    void playerTouchedTrigger(PlayerObject* player, EffectGameObject* object) {
        if (sim::active) return;
        GJBaseGameLayer::playerTouchedTrigger(player, object);
    }

    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        if (sim::active) {
            if (sim::isFake(player)) player->flipGravity(flip, true);
            return;
        }
        GJBaseGameLayer::flipGravity(player, flip, noEffects);
    }

    // Gamemode / dual / mirror / teleport portals change level-wide state (camera, ground).
    // We don't simulate past them: the branch ends there as "survived".
    void playerWillSwitchMode(PlayerObject* player, GameObject* object) {
        if (sim::active) {
            g_stop = true;
            return;
        }
        GJBaseGameLayer::playerWillSwitchMode(player, object);
    }

    void switchToRobotMode(PlayerObject* player, GameObject* object, bool noPortal) {
        if (sim::active) {
            g_stop = true;
            return;
        }
        GJBaseGameLayer::switchToRobotMode(player, object, noPortal);
    }

    void switchToRollMode(PlayerObject* player, GameObject* object, bool noPortal) {
        if (sim::active) {
            g_stop = true;
            return;
        }
        GJBaseGameLayer::switchToRollMode(player, object, noPortal);
    }

    void toggleDualMode(GameObject* object, bool dual, PlayerObject* player, bool noEffects) {
        if (sim::active) {
            g_stop = true;
            return;
        }
        GJBaseGameLayer::toggleDualMode(object, dual, player, noEffects);
    }

    void teleportPlayer(TeleportPortalObject* object, PlayerObject* player) {
        if (sim::active) {
            g_stop = true;
            return;
        }
        GJBaseGameLayer::teleportPlayer(object, player);
    }

    void toggleFlipped(bool flip, bool noEffects) {
        if (sim::active) {
            g_stop = true;
            return;
        }
        GJBaseGameLayer::toggleFlipped(flip, noEffects);
    }

    void updateDualGround(PlayerObject* player, int mode, bool instant, float duration) {
        if (sim::active) return;
        GJBaseGameLayer::updateDualGround(player, mode, instant, duration);
    }

    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (sim::active) return;
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    void spawnGroup(int group, bool ordered, double delay, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
        if (sim::active) return;
        GJBaseGameLayer::spawnGroup(group, ordered, delay, remapKeys, triggerID, controlID);
    }

    void toggleGroupTriggered(int group, bool activate, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
        if (sim::active) return;
        GJBaseGameLayer::toggleGroupTriggered(group, activate, remapKeys, triggerID, controlID);
    }

    void pickupItem(EffectGameObject* object) {
        if (sim::active) return;
        GJBaseGameLayer::pickupItem(object);
    }

    void destroyObject(GameObject* object) {
        if (sim::active) return;
        GJBaseGameLayer::destroyObject(object);
    }

    void objectsCollided(int blockAID, int blockBID) {
        if (sim::active) return;
        GJBaseGameLayer::objectsCollided(blockAID, blockBID);
    }
};

class $modify(IBSimPlayer, PlayerObject) {
    void update(float dt) {
        if (!sim::active) {
            if (auto pl = PlayLayer::get(); pl && (this == pl->m_player1 || this == pl->m_player2)) {
                ibRuntime().lastDt = dt;
            }
        }
        PlayerObject::update(dt);
    }

    void playSpiderDashEffect(cocos2d::CCPoint from, cocos2d::CCPoint to) {
        if (sim::active) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void incrementJumps() {
        if (sim::active) return;
        PlayerObject::incrementJumps();
    }
};

class $modify(IBSimObject, GameObject) {
    void playShineEffect() {
        if (sim::active) return;
        GameObject::playShineEffect();
    }
};

class $modify(IBSimStreak, HardStreak) {
    void addPoint(cocos2d::CCPoint point) {
        if (sim::active) return;
        HardStreak::addPoint(point);
    }
};
