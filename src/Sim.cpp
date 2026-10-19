#include "Sim.hpp"

#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>

#include <algorithm>
#include <chrono>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace {
    using Clock = std::chrono::steady_clock;

    constexpr int kMaxShift = 16;
    // Hidden players per real player: one saved state per tick of the "do nothing" path
    // (0..kMaxShift), then a runner, a landing save and a follow-up runner.
    constexpr int kRunner = kMaxShift + 1;
    constexpr int kLanding = kMaxShift + 2;
    constexpr int kFollow = kMaxShift + 3;
    constexpr int kPoolSize = kMaxShift + 4;

    std::vector<Ref<PlayerObject>> g_pools[2];
    std::unordered_set<int> g_poolIDs;
    Ref<CCDrawNode> g_debugDraw;

    bool g_dead = false;   // a hidden player died this tick
    bool g_stop = false;   // the hidden player hit something we don't simulate (portal, end, ...)

    // Activation flags of objects a hidden player touched, restored after every run.
    struct SavedFlags {
        bool p1;
        bool p2;
    };
    std::unordered_map<EffectGameObject*, SavedFlags> g_touched;

    void remember(EffectGameObject* obj) {
        if (!obj || g_touched.contains(obj)) return;
        g_touched.emplace(obj, SavedFlags { obj->m_activatedByPlayer1, obj->m_activatedByPlayer2 });
    }

    enum class Outcome { Died, DiedEarly, Survived, Unknown };
    enum class Step { Ok, Dead, Stop, OutOfTime };

    // Toggle-back offsets tried after the assumed hold (-1 = never). Order matters for the
    // early exit: the most natural continuation goes first. Cube/ball/spider barely care
    // when you let go, robot does a little, fly modes a lot.
    constexpr int kPressFly[] = { 0, -1, 6, 12, 24, 48 };
    constexpr int kReleaseFly[] = { -1, 0, 6, 12, 24, 48 };
    constexpr int kPressRobot[] = { 0, -1, 6, 12 };
    constexpr int kPressGround[] = { 0, -1 };
    constexpr int kReleaseGround[] = { -1, 0 };

    // After landing, "can I still get out of here?": stand still for kStandTicks, or jump at
    // one of these offsets.
    constexpr int kStandTicks = 20;
    constexpr int kEscapeJumps[] = { 0, 3, 6, 10, 14, 20 };
    constexpr int kEscapeMax = 150;
    constexpr int kGroundSettleMax = 240;  // stop looking for a landing after 1s
    constexpr int kSafestSpan = 4;         // Safest mode scores this many ticks past the first safe one

    bool isFlyMode(PlayerObject* p) {
        return p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing;
    }

    void setButton(PlayerObject* p, bool down) {
        if (down) p->pushButton(PlayerButton::Jump);
        else p->releaseButton(PlayerButton::Jump);
    }

    // The bits of a real player we check after simulating, to catch any leak into the real game.
    struct RealSnapshot {
        PlayerObject* player;
        CCPoint pos;
        double yVel;
        bool onGround;
        bool upsideDown;
        bool dead;

        explicit RealSnapshot(PlayerObject* p)
          : player(p), pos(p->getPosition()), yVel(p->m_yVelocity),
            onGround(p->m_isOnGround), upsideDown(p->m_isUpsideDown), dead(p->m_isDead) {}

        // Puts the player back if anything moved. Returns true if it had to.
        bool restoreIfChanged() const {
            auto now = player->getPosition();
            bool changed = now.x != pos.x || now.y != pos.y || player->m_yVelocity != yVel
                || player->m_isOnGround != onGround || player->m_isUpsideDown != upsideDown
                || player->m_isDead != dead;
            if (changed) {
                player->setPosition(pos);
                player->m_yVelocity = yVel;
                player->m_isOnGround = onGround;
                player->m_isDead = dead;
            }
            return changed;
        }
    };

    // Make `dst` an exact stand-in for `src` (a real player or another hidden one).
    void copyState(PlayerObject* dst, PlayerObject* src, bool held) {
        // spawnFromPlayer copies attributes; keep the hidden player's own object ID so its
        // orb/pad activations never get mixed up with (or erased as) the real player's.
        int ownID = dst->m_uniqueID;
        dst->spawnFromPlayer(src, false);
        dst->m_uniqueID = ownID;

        dst->toggleVisibility(false);
        dst->setVisible(false);
        dst->m_playEffects = false;
        dst->m_gravityMod = src->m_gravityMod;
        dst->m_isOnGround = src->m_isOnGround;
        dst->m_isOnGround2 = src->m_isOnGround2;
        dst->m_isSecondPlayer = src->m_isSecondPlayer;
        dst->m_isDead = false;
        dst->m_touchingRings->removeAllObjects();
        dst->m_touchedRings = src->m_touchedRings;
        dst->resetCollisionLog(true);
        if (held) dst->pushButton(PlayerButton::Jump);
        // Holding is not the same as a fresh click: keep the source's buffered-jump state.
        dst->m_jumpBuffered = src->m_jumpBuffered;
        dst->m_stateRingJump = src->m_stateRingJump;
        dst->m_stateRingJump2 = src->m_stateRingJump2;
        dst->m_ringJumpRelated = src->m_ringJumpRelated;
    }

    void restoreWorld(GJBaseGameLayer* layer) {
        for (auto& [obj, flags] : g_touched) {
            obj->m_activatedByPlayer1 = flags.p1;
            obj->m_activatedByPlayer2 = flags.p2;
        }
        g_touched.clear();

        auto& activated = layer->m_gameState.m_activatedObjectIDs;
        for (auto it = activated.begin(); it != activated.end();) {
            if (g_poolIDs.contains(it->first.second)) it = activated.erase(it);
            else ++it;
        }
    }

    void quiet(PlayerObject* p) {
        p->stopParticles();
        if (p->m_isDashing) p->stopDashing();
    }

    struct PlayerSim {
        PlayerObject* real;
        std::vector<Ref<PlayerObject>>* pool;
        int baseValid = 0;               // saved states 0..baseValid-1 are usable
        std::vector<CCPoint> basePath;   // positions of the "do nothing" path (debug)

        PlayerObject* at(int i) const { return (*pool)[i].data(); }
    };

    struct Evaluator {
        GJBaseGameLayer* layer;
        std::vector<PlayerSim> players;
        bool heldNow;
        bool down;
        int budget;
        Clock::time_point deadline;
        int steps = 0;
        bool timedOut = false;
        float dt = .25f;

        bool outOfTime() {
            if (budget <= 0) timedOut = true;
            else if ((steps & 31) == 0 && Clock::now() > deadline) timedOut = true;
            return timedOut;
        }

        // One physics tick, in GD's order: collisions, then input, then movement.
        Step tick(PlayerObject* p, int input, std::vector<CCPoint>* path) {
            --budget;
            ++steps;
            if (outOfTime()) return Step::OutOfTime;
            g_dead = false;
            g_stop = false;
            p->resetCollisionLog(false);
            layer->checkCollisions(p, dt, false);
            if (g_dead) return Step::Dead;
            if (g_stop) return Step::Stop;
            if (input >= 0) setButton(p, input == 1);
            p->update(dt);
            p->updateSpecial(dt);
            p->updateRotation(dt);
            if (path) path->push_back(p->getPosition());
            return Step::Ok;
        }

        std::span<int const> branches() const {
            auto p = players.front().real;
            if (isFlyMode(p)) {
                if (down) return kPressFly;
                return kReleaseFly;
            }
            if (!down) return kReleaseGround;
            if (p->m_isRobot) return kPressRobot;
            return kPressGround;
        }

        // Save the "do nothing" path once: state after t ticks without the input, t = 0..maxShift.
        void buildBase(PlayerSim& ps, int maxShift) {
            auto runner = ps.at(kRunner);
            copyState(ps.at(0), ps.real, heldNow);
            copyState(runner, ps.real, heldNow);
            ps.baseValid = 1;
            for (int t = 1; t <= maxShift; ++t) {
                if (tick(runner, -1, &ps.basePath) != Step::Ok) break;
                copyState(ps.at(t), runner, heldNow);
                ps.baseValid = t + 1;
            }
            quiet(runner);
            restoreWorld(layer);
        }

        // From a landing: is there still a way out (stand still a bit, or jump soon)?
        bool canEscape(PlayerSim& ps, bool held) {
            auto landing = ps.at(kLanding);
            auto runner = ps.at(kFollow);
            copyState(landing, ps.at(kRunner), held);

            auto attempt = [&](int jumpAt) -> bool {
                copyState(runner, landing, held);
                bool pressed = held;
                for (int t = 0; t <= kEscapeMax; ++t) {
                    int input = -1;
                    if (jumpAt < 0) {
                        if (t >= kStandTicks) return true;
                    }
                    else {
                        if (held && t == 0) {
                            input = 0;
                            pressed = false;
                        }
                        if (t == jumpAt + (held ? 1 : 0)) {
                            input = 1;
                            pressed = true;
                        }
                        if (pressed && t == jumpAt + (held ? 1 : 0) + ibConfig().assumedHold) {
                            input = 0;
                            pressed = false;
                        }
                        if (t > jumpAt + ibConfig().assumedHold + 1 && runner->m_isOnGround) return true;
                    }
                    auto step = tick(runner, input, nullptr);
                    if (step == Step::Dead) return false;
                    if (step == Step::OutOfTime) return false;
                    if (step == Step::Stop) return true;
                }
                return true;
            };

            bool ok = attempt(-1);
            for (int j : kEscapeJumps) {
                if (ok || timedOut) break;
                ok = attempt(j);
            }
            quiet(runner);
            return ok;
        }

        // One player, one timing (delay), one continuation (branch).
        Outcome runCandidate(PlayerSim& ps, int delay, int toggleAfter, std::vector<CCPoint>* path) {
            auto const& cfg = ibConfig();
            if (delay >= ps.baseValid) return Outcome::DiedEarly;  // dead before the input lands

            auto runner = ps.at(kRunner);
            copyState(runner, ps.at(delay), heldNow);
            if (path) path->assign(ps.basePath.begin(), ps.basePath.begin() + std::min<size_t>(delay, ps.basePath.size()));

            int keepFor = cfg.assumedHold;
            int toggleAt = toggleAfter >= 0 ? keepFor + toggleAfter : -1;
            int settleStart = toggleAt >= 0 ? toggleAt : keepFor;
            int maxT = settleStart + std::max(cfg.flyWindow, kGroundSettleMax);
            bool held = down;

            Outcome result = Outcome::Survived;
            for (int t = 0; t <= maxT; ++t) {
                int input = -1;
                if (t == 0) input = down ? 1 : 0;
                if (t == toggleAt) {
                    input = down ? 0 : 1;
                    held = !down;
                }

                auto step = tick(runner, input, path);
                if (step == Step::OutOfTime) {
                    result = Outcome::Unknown;
                    break;
                }
                if (step == Step::Dead) {
                    result = t <= keepFor ? Outcome::DiedEarly : Outcome::Died;
                    break;
                }
                if (step == Step::Stop) break;

                if (t >= settleStart) {
                    if (isFlyMode(runner)) {
                        if (t - settleStart >= cfg.flyWindow) break;
                    }
                    else if (t > settleStart && runner->m_isOnGround) {
                        // Landed. Only safe if you can still get out from here.
                        if (!canEscape(ps, held)) result = timedOut ? Outcome::Unknown : Outcome::Died;
                        break;
                    }
                }
            }
            quiet(runner);
            restoreWorld(layer);
            return result;
        }

        // A continuation counts only if every moved player survives it (one key moves both in dual).
        Outcome runShared(int delay, int toggleAfter, std::vector<CCPoint>* paths = nullptr) {
            Outcome worst = Outcome::Survived;
            for (size_t i = 0; i < players.size(); ++i) {
                auto out = runCandidate(players[i], delay, toggleAfter, paths ? &paths[i] : nullptr);
                if (out == Outcome::Died || out == Outcome::DiedEarly) return out;
                if (out == Outcome::Unknown) worst = Outcome::Unknown;
            }
            return worst;
        }

        // Is there any way to keep going with this timing? Unknown (out of time) counts as
        // safe only for the on-time input, so we never add delay on a guess.
        bool viable(int delay) {
            for (int b : branches()) {
                auto out = runShared(delay, b);
                if (out == Outcome::Survived) return true;
                if (out == Outcome::Unknown) return delay == 0;
                // Died during the input or assumed hold: every continuation shares that.
                if (out == Outcome::DiedEarly) return false;
            }
            return false;
        }

        // How forgiving is this timing: number of continuations that survive.
        int score(int delay) {
            int count = 0;
            for (int b : branches()) {
                if (timedOut) break;
                auto out = runShared(delay, b);
                if (out == Outcome::Survived) ++count;
                if (out == Outcome::DiedEarly) break;
            }
            return count;
        }

        int choose() {
            auto const& cfg = ibConfig();
            int first = -1;
            for (int d = 1; d <= cfg.bufferTicks && !timedOut; ++d) {
                if (viable(d)) {
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
            for (int d = first; d <= last && !timedOut; ++d) {
                scores[d] = score(d);
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

        // Debug: record the first surviving continuation of this timing (or the first one if none).
        void record(int delay, std::vector<CCPoint>* paths) {
            budget = std::max(budget, 4000);
            timedOut = false;
            deadline = Clock::now() + std::chrono::milliseconds(50);
            for (int b : branches()) {
                if (runShared(delay, b, paths) == Outcome::Survived) return;
            }
            runShared(delay, branches().front(), paths);
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
        return player && std::any_of(std::begin(g_pools), std::end(g_pools), [&](auto const& pool) {
                return std::any_of(pool.begin(), pool.end(), [&](auto const& p) { return p.data() == player; });
            });
    }

    void setup(PlayLayer* layer) {
        teardown();
        for (auto& pool : g_pools) {
            for (int i = 0; i < kPoolSize; ++i) {
                auto p = PlayerObject::create(1, 1, layer, layer, true);
                p->setPosition({ 0.f, 105.f });
                p->setVisible(false);
                layer->m_objectLayer->addChild(p);
                pool.push_back(p);
                g_poolIDs.insert(p->m_uniqueID);
            }
        }
        // Never treat a real player's ID as ours (we erase our IDs' orb/pad activations).
        if (layer->m_player1) g_poolIDs.erase(layer->m_player1->m_uniqueID);
        if (layer->m_player2) g_poolIDs.erase(layer->m_player2->m_uniqueID);
        if (ibConfig().debug) {
            log::info("[IB] created {} hidden players, {} distinct IDs", kPoolSize * 2, g_poolIDs.size());
        }

        auto draw = CCDrawNode::create();
        draw->setID("debug-paths"_spr);
        draw->m_bUseArea = false;  // otherwise segments are culled on some levels
        layer->m_objectLayer->addChild(draw, 9999);
        g_debugDraw = draw;
    }

    void teardown() {
        for (auto& pool : g_pools) {
            for (auto& p : pool) {
                if (p) p->removeFromParent();
            }
            pool.clear();
        }
        g_poolIDs.clear();
        if (g_debugDraw) g_debugDraw->removeFromParent();
        g_debugDraw = nullptr;
        g_touched.clear();
    }

    bool onDestroyPlayer(PlayerObject* player) {
        if (!active || !isFake(player)) return false;
        g_dead = true;
        return true;
    }

    Decision decide(GJBaseGameLayer* layer, std::vector<PlayerObject*> const& players, bool heldNow, bool down) {
        auto const& cfg = ibConfig();
        Decision result;
        if (players.empty() || players.size() > 2) return result;

        auto start = Clock::now();
        Evaluator ev { layer, {}, heldNow, down, cfg.simBudget, start + std::chrono::milliseconds(cfg.timeLimitMs) };
        ev.dt = ibRuntime().lastDt;
        std::vector<RealSnapshot> snapshots;
        for (auto real : players) {
            auto& pool = g_pools[real == layer->m_player2 ? 1 : 0];
            if (!real || real->m_isDead || static_cast<int>(pool.size()) != kPoolSize) return result;
            ev.players.push_back(PlayerSim { real, &pool });
            snapshots.emplace_back(real);
        }

        {
            ActiveGuard guard;

            int maxShift = std::min(cfg.bufferTicks, kMaxShift);
            for (auto& ps : ev.players) ev.buildBase(ps, maxShift);

            // On time and survivable: never touch it (keeps spam / straight-fly exactly normal).
            result.onTimeViable = ev.viable(0);
            if (!result.onTimeViable) result.shift = ev.choose();
            result.steps = ev.steps;
            result.timedOut = ev.timedOut;

            if (cfg.debug) {
                if (!result.onTimeViable) ev.record(0, result.onTime);
                ev.record(result.shift, result.predicted);
            }
        }

        for (auto const& snap : snapshots) {
            if (snap.restoreIfChanged()) result.realChanged = true;
        }
        result.ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        return result;
    }

    void drawDebug(Decision const& decision) {
        if (!g_debugDraw) return;
        g_debugDraw->clear();
        auto line = [](std::vector<CCPoint> const& path, ccColor4F color) {
            for (size_t i = 1; i < path.size(); ++i) {
                g_debugDraw->drawSegment(path[i - 1], path[i], .8f, color);
            }
        };
        for (auto const& path : decision.onTime) line(path, { 1.f, .2f, .2f, 1.f });
        for (auto const& path : decision.predicted) line(path, { .2f, 1.f, .3f, 1.f });
    }

    void clearDebug() {
        if (g_debugDraw) g_debugDraw->clear();
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
