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

    // Future clicks the look-ahead may plan after your input (at orbs and on the ground).
    constexpr int kMaxDepth = 3;

    // Hidden players per real player: a fly-mode runner, then a runner + save per planning depth.
    // Every option starts from the real player (copies of mid-air copies drift by a tick).
    constexpr int kRunner = 0;
    constexpr int kPoolSize = 1 + 2 * (kMaxDepth + 1);
    constexpr int slotRunner(int depth) { return 1 + 2 * depth; }
    constexpr int slotSave(int depth) { return 2 + 2 * depth; }

    std::vector<Ref<PlayerObject>> g_pools[2];
    std::unordered_set<int> g_poolIDs;
    Ref<CCDrawNode> g_debugDraw;

    bool g_dead = false;                 // a hidden player died this tick
    bool g_stop = false;                 // it hit something we don't simulate (portal, end, ...)
    RingObject* g_ringTouched = nullptr; // it overlapped this orb this tick
    bool g_realDeath = false;            // a real player died this attempt

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

    // Fly modes (ship / UFO / wave / swing): toggle-back offsets tried after the assumed
    // hold (-1 = never). The most natural continuation goes first for the early exit.
    constexpr int kPressFly[] = { 0, -1, 6, 12, 24, 48 };
    constexpr int kReleaseFly[] = { -1, 0, 6, 12, 24, 48 };
    // Ground modes (cube / ball / robot / spider) plan future clicks instead.
    constexpr int kGroundPlan[] = { 0 };

    constexpr int kTapHold = 4;            // a tap is held this long (robot uses the assumed hold)
    constexpr int kStandTicks = 20;        // standing this long on ground = fine
    constexpr int kGroundJumps[] = { 0, 4, 8, 12 };  // planned jumps after landing (ticks on ground)
    constexpr int kRingClicks[] = { 0, 3, 6 };  // planned clicks on an orb, ticks after first touching it
    constexpr int kContinueHorizon = 180;  // surviving this long without dying = fine
    constexpr int kSafestSpan = 4;         // Safest mode looks this many ticks past the first safe one

    bool isFlyMode(PlayerObject* p) {
        return p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing;
    }

    void setButton(PlayerObject* p, bool down) {
        if (down) p->pushButton(PlayerButton::Jump);
        else p->releaseButton(PlayerButton::Jump);
    }

    // 0 cube, 1 ship, 2 ball, 3 UFO, 4 wave, 5 robot, 6 spider, 7 swing
    int modeOf(PlayerObject* p) {
        if (p->m_isShip) return 1;
        if (p->m_isBall) return 2;
        if (p->m_isBird) return 3;
        if (p->m_isDart) return 4;
        if (p->m_isRobot) return 5;
        if (p->m_isSpider) return 6;
        if (p->m_isSwing) return 7;
        return 0;
    }

    void setMode(PlayerObject* p, int mode, bool enable) {
        switch (mode) {
            case 1: p->toggleFlyMode(enable, true); break;
            case 2: p->toggleRollMode(enable, true); break;
            case 3: p->toggleBirdMode(enable, true); break;
            case 4: p->toggleDartMode(enable, true); break;
            case 5: p->toggleRobotMode(enable, true); break;
            case 6: p->toggleSpiderMode(enable, true); break;
            case 7: p->toggleSwingMode(enable, true); break;
            default: break;
        }
    }

    // The bits of a real player we check after simulating, to catch any leak into the real game
    // (in dual mode GD switches both players at a portal, so a hidden player could flip yours).
    struct RealSnapshot {
        PlayerObject* player;
        CCPoint pos;
        double yVel;
        bool onGround;
        bool upsideDown;
        bool dead;
        int mode;
        float size;
        float speed;

        explicit RealSnapshot(PlayerObject* p)
          : player(p), pos(p->getPosition()), yVel(p->m_yVelocity),
            onGround(p->m_isOnGround), upsideDown(p->m_isUpsideDown), dead(p->m_isDead),
            mode(modeOf(p)), size(p->m_vehicleSize), speed(p->m_playerSpeed) {}

        // Puts the player back if anything changed. Returns true if it had to.
        bool restoreIfChanged() const {
            bool changed = false;
            if (modeOf(player) != mode) {
                setMode(player, modeOf(player), false);
                setMode(player, mode, true);
                changed = true;
            }
            if (player->m_vehicleSize != size) {
                player->togglePlayerScale(size < 1.f, true);
                changed = true;
            }
            if (player->m_isUpsideDown != upsideDown) {
                player->flipGravity(upsideDown, true);
                changed = true;
            }
            if (player->m_playerSpeed != speed) {
                player->m_playerSpeed = speed;
                changed = true;
            }
            auto now = player->getPosition();
            if (now.x != pos.x || now.y != pos.y || player->m_yVelocity != yVel
                || player->m_isOnGround != onGround || player->m_isDead != dead) {
                player->setPosition(pos);
                player->m_yVelocity = yVel;
                player->m_isOnGround = onGround;
                player->m_isDead = dead;
                changed = true;
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
        // Keep the collision memory (what it stood on / last touched): wiping it makes a copy
        // taken right at a landing jump one tick late.
        dst->m_collisionLogTop->removeAllObjects();
        dst->m_collisionLogBottom->removeAllObjects();
        dst->m_collisionLogLeft->removeAllObjects();
        dst->m_collisionLogRight->removeAllObjects();
        dst->m_lastCollisionBottom = src->m_lastCollisionBottom;
        dst->m_lastCollisionTop = src->m_lastCollisionTop;
        dst->m_lastCollisionLeft = src->m_lastCollisionLeft;
        dst->m_lastCollisionRight = src->m_lastCollisionRight;
        dst->m_unk50C = src->m_unk50C;
        dst->m_unk510 = src->m_unk510;
        dst->m_maybeLastGroundObject = src->m_maybeLastGroundObject;
        dst->m_preLastGroundObject = src->m_preLastGroundObject;
        dst->m_lastGroundObject = src->m_lastGroundObject;
        dst->m_collidedObject = src->m_collidedObject;
        dst->m_currentSlope2 = src->m_currentSlope2;
        dst->m_slopeAngle = src->m_slopeAngle;
        dst->m_slopeStartTime = src->m_slopeStartTime;
        // Testing "what if you let go now" while the source is still holding.
        auto holding = src->m_holdingButtons.find(1);
        bool letGo = !held && holding != src->m_holdingButtons.end() && holding->second;
        if (held) dst->pushButton(PlayerButton::Jump);
        else if (letGo) dst->releaseButton(PlayerButton::Jump);
        // Holding is not the same as a fresh click: keep the source's buffered-jump state.
        dst->m_jumpBuffered = src->m_jumpBuffered && !letGo;
        dst->m_stateRingJump = src->m_stateRingJump;
        dst->m_stateRingJump2 = src->m_stateRingJump2;
        dst->m_ringJumpRelated = src->m_ringJumpRelated;
    }

    // Everything level-wide a hidden player can change: portals move GD's camera and portal
    // bounds (kept in m_gameState), dying sets "a player died" (which turns on "show hitboxes on
    // death"). Saved before every look-ahead and put back after it, so hidden players can go
    // through portals without the real level noticing.
    struct WorldSnapshot;
    WorldSnapshot* g_world = nullptr;
    bool g_worldDirty = false;  // a hidden player went through a portal since the last restore

    struct WorldSnapshot {
        GJBaseGameLayer* layer;
        GJGameState state;
        bool playerDied;
        bool debugDraw;
        float cameraFlip;
        float cameraWidthOffset;
        float cameraHeightOffset;
        float cameraUnzoomedHeightOffset;
        float targetCameraHeightOffset;
        bool calculateTargetHeightOffset;
        GameObject* p1Block;
        GameObject* p2Block;
        WorldSnapshot* previous;

        explicit WorldSnapshot(GJBaseGameLayer* l)
          : layer(l), state(l->m_gameState), playerDied(l->m_playerDied), debugDraw(l->m_isDebugDrawEnabled),
            cameraFlip(l->m_cameraFlip), cameraWidthOffset(l->m_cameraWidthOffset),
            cameraHeightOffset(l->m_cameraHeightOffset), cameraUnzoomedHeightOffset(l->m_cameraUnzoomedHeightOffset),
            targetCameraHeightOffset(l->m_targetCameraHeightOffset),
            calculateTargetHeightOffset(l->m_calculateTargetHeightOffset),
            p1Block(l->m_player1CollisionBlock), p2Block(l->m_player2CollisionBlock), previous(g_world) {
            g_world = this;
        }

        void restore() {
            if (g_worldDirty) {
                layer->m_gameState = state;
                g_worldDirty = false;
            }
            layer->m_playerDied = playerDied;
            layer->m_isDebugDrawEnabled = debugDraw;
            layer->m_cameraFlip = cameraFlip;
            layer->m_cameraWidthOffset = cameraWidthOffset;
            layer->m_cameraHeightOffset = cameraHeightOffset;
            layer->m_cameraUnzoomedHeightOffset = cameraUnzoomedHeightOffset;
            layer->m_targetCameraHeightOffset = targetCameraHeightOffset;
            layer->m_calculateTargetHeightOffset = calculateTargetHeightOffset;
            layer->m_player1CollisionBlock = p1Block;
            layer->m_player2CollisionBlock = p2Block;
        }

        ~WorldSnapshot() {
            restore();
            g_world = previous;
        }
    };

    void restoreFlags() {
        for (auto& [obj, flags] : g_touched) {
            obj->m_activatedByPlayer1 = flags.p1;
            obj->m_activatedByPlayer2 = flags.p2;
        }
        g_touched.clear();
    }

    // Forget orb/pad activations made by these hidden-player IDs.
    void forgetActivations(GJBaseGameLayer* layer, std::unordered_set<int> const& ids) {
        auto& activated = layer->m_gameState.m_activatedObjectIDs;
        for (auto it = activated.begin(); it != activated.end();) {
            if (ids.contains(it->first.second)) it = activated.erase(it);
            else ++it;
        }
    }

    void restoreWorld(GJBaseGameLayer* layer) {
        restoreFlags();
        if (g_world) g_world->restore();
        forgetActivations(layer, g_poolIDs);
    }

    void quiet(PlayerObject* p) {
        p->stopParticles();
        if (p->m_isDashing) p->stopDashing();
    }

    struct PlayerSim {
        PlayerObject* real;
        std::vector<Ref<PlayerObject>>* pool;

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
        // GD only marks "on ground" while checking collisions (movement clears it again), so
        // landings must be read right after the collision check of each tick.
        bool grounded = false;

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
            g_ringTouched = nullptr;
            // Only orbs overlapped this tick can be clicked; the collision check re-adds them.
            p->m_touchingRings->removeAllObjects();
            p->resetCollisionLog(false);
            layer->checkCollisions(p, dt, false);
            if (g_dead) return Step::Dead;
            if (g_stop) return Step::Stop;
            grounded = p->m_isOnGround;
            if (input >= 0) setButton(p, input == 1);
            p->update(dt);
            p->updateSpecial(dt);
            p->updateRotation(dt);
            if (path) path->push_back(p->getPosition());
            return Step::Ok;
        }

        bool flyMode() const {
            return isFlyMode(players.front().real);
        }

        // How long a click is held: robot jumps higher the longer you hold, the rest only care
        // that you clicked.
        int clickHold() const {
            return players.front().real->m_isRobot ? ibConfig().assumedHold : kTapHold;
        }

        std::span<int const> branches() const {
            if (!flyMode()) return kGroundPlan;
            if (down) return kPressFly;
            return kReleaseFly;
        }

        // Undo what deeper planning levels left behind, so the next attempt starts clean.
        void cleanDeeper(PlayerSim& ps, int depth) {
            std::unordered_set<int> ids;
            for (int d = 0; d < depth; ++d) {
                ids.insert(ps.at(slotRunner(d))->m_uniqueID);
                ids.insert(ps.at(slotSave(d))->m_uniqueID);
            }
            restoreFlags();
            forgetActivations(layer, ids);
        }

        // Ground modes: from `src`, can you keep going if you click like a player would?
        // At every orb you overlap and every landing we try "click here" (up to `depth` more
        // clicks deep). With no clicks left, reaching the next chance to click alive is enough.
        // `holdTicks`: how long the starting click is held (-1 = a planned tap).
        bool canContinue(PlayerSim& ps, int depth, PlayerObject* src, bool srcHeld, bool startClick, std::vector<CCPoint>* path, int waitTicks = 0, int holdTicks = -1) {
            auto runner = ps.at(slotRunner(depth));
            auto save = ps.at(slotSave(depth));
            copyState(runner, src, srcHeld);

            std::vector<CCPoint> local;
            auto* trace = path ? &local : nullptr;
            auto succeed = [&](std::vector<CCPoint> const* tail) {
                if (path) {
                    path->insert(path->end(), local.begin(), local.end());
                    if (tail) path->insert(path->end(), tail->begin(), tail->end());
                }
                quiet(runner);
                return true;
            };
            auto tryClick = [&]() -> bool {
                copyState(save, runner, false);
                std::vector<CCPoint> child;
                bool ok = canContinue(ps, depth - 1, save, false, true, path ? &child : nullptr);
                cleanDeeper(ps, depth);
                if (ok) return succeed(&child);
                return false;
            };

            std::vector<std::pair<RingObject*, int>> ringSeen;  // orb, tick of first touch (-1 = not a new chance)

            // Held back: the button stays as it is for `waitTicks` first.
            for (int t = 0; t < waitTicks; ++t) {
                auto step = tick(runner, -1, trace);
                if (step == Step::Dead || step == Step::OutOfTime) {
                    quiet(runner);
                    return false;
                }
                if (step == Step::Stop) return succeed(nullptr);
                // Orbs passed while waiting belong to this input, not to a later click.
                if (g_ringTouched) ringSeen.emplace_back(g_ringTouched, -1);
            }

            int hold = holdTicks >= 0 ? holdTicks : clickHold();
            int clickEnd = startClick ? hold : 0;
            int groundStreak = 0;
            int groundSince = -1;

            for (int t = 0; t <= kContinueHorizon; ++t) {
                int input = -1;
                if (t == 0) {
                    if (startClick) input = 1;
                    else if (srcHeld) input = 0;
                }
                if (startClick && t == hold) input = 0;

                auto step = tick(runner, input, trace);
                if (step == Step::Dead || step == Step::OutOfTime) {
                    quiet(runner);
                    return false;
                }
                if (step == Step::Stop) return succeed(nullptr);
                if (t <= clickEnd) {
                    // The orb this click was for is not "the next click".
                    if (g_ringTouched) ringSeen.emplace_back(g_ringTouched, -1);
                    continue;
                }

                // Landed (two ticks in a row, so an orb flip touching a surface doesn't count).
                groundStreak = grounded ? groundStreak + 1 : 0;
                if (groundStreak >= 2) {
                    if (groundSince < 0) groundSince = t;
                    int onFor = t - groundSince;
                    if (depth == 0) return succeed(nullptr);  // back on the ground, your click again
                    if (onFor >= kStandTicks) return succeed(nullptr);
                    if (std::find(std::begin(kGroundJumps), std::end(kGroundJumps), onFor) != std::end(kGroundJumps)) {
                        if (tryClick()) return true;
                        if (timedOut) return false;
                    }
                }
                else if (!grounded) {
                    groundSince = -1;
                }

                // Overlapping an orb: this is a chance to click.
                auto ring = g_ringTouched;
                if (ring) {
                    auto it = std::find_if(ringSeen.begin(), ringSeen.end(), [&](auto const& e) { return e.first == ring; });
                    int since = 0;
                    if (it == ringSeen.end()) ringSeen.emplace_back(ring, t);
                    else if (it->second < 0) continue;  // ignored (already clicked)
                    else since = t - it->second;
                    if (std::find(std::begin(kRingClicks), std::end(kRingClicks), since) != std::end(kRingClicks)) {
                        if (depth == 0) return succeed(nullptr);  // reached the next click
                        if (tryClick()) return true;
                        if (timedOut) return false;
                    }
                }
            }
            return succeed(nullptr);
        }

        // Ground modes: your input is the first click of the plan, so the next orb or landing can
        // be planned for straight away (dense orb chains don't leave a 12-tick gap).
        Outcome runGroundCandidate(PlayerSim& ps, int delay, std::vector<CCPoint>* path) {
            if (path) path->clear();
            Outcome result = Outcome::Survived;
            // Your press is held for the assumed hold: holding into an orb or a landing fires it,
            // exactly like in the real game.
            int hold = std::max(ibConfig().assumedHold, kTapHold);
            if (!canContinue(ps, kMaxDepth, ps.real, heldNow, down, path, delay, hold)) {
                result = timedOut ? Outcome::Unknown : Outcome::Died;
            }
            restoreWorld(layer);
            return result;
        }

        // Fly modes: one timing (delay), one continuation (toggle-back offset).
        Outcome runFlyCandidate(PlayerSim& ps, int delay, int toggleAfter, std::vector<CCPoint>* path) {
            auto const& cfg = ibConfig();
            auto runner = ps.at(kRunner);
            copyState(runner, ps.real, heldNow);
            if (path) path->clear();
            for (int t = 0; t < delay; ++t) {
                auto step = tick(runner, -1, path);
                if (step == Step::Dead || step == Step::Stop || step == Step::OutOfTime) {
                    quiet(runner);
                    restoreWorld(layer);
                    if (step == Step::Dead) return Outcome::DiedEarly;
                    return step == Step::Stop ? Outcome::Survived : Outcome::Unknown;
                }
            }

            int keepFor = cfg.assumedHold;
            int toggleAt = toggleAfter >= 0 ? keepFor + toggleAfter : -1;
            int settleStart = toggleAt >= 0 ? toggleAt : keepFor;
            int maxT = settleStart + cfg.flyWindow;

            Outcome result = Outcome::Survived;
            for (int t = 0; t <= maxT; ++t) {
                int input = -1;
                if (t == 0) input = down ? 1 : 0;
                if (t == toggleAt) input = down ? 0 : 1;

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
            }
            quiet(runner);
            restoreWorld(layer);
            return result;
        }

        // A plan counts only if every moved player survives it (one key moves both in dual).
        Outcome runShared(int delay, int branch, std::vector<CCPoint>* paths = nullptr) {
            Outcome worst = Outcome::Survived;
            for (size_t i = 0; i < players.size(); ++i) {
                auto* path = paths ? &paths[i] : nullptr;
                auto out = flyMode()
                    ? runFlyCandidate(players[i], delay, branch, path)
                    : runGroundCandidate(players[i], delay, path);
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

        // Debug: record the first surviving plan for this timing (or the first one if none).
        void record(int delay, std::vector<CCPoint>* paths) {
            budget = std::max(budget, 8000);
            timedOut = false;
            deadline = Clock::now() + std::chrono::milliseconds(50);
            for (int b : branches()) {
                for (size_t i = 0; i < players.size(); ++i) paths[i].clear();
                if (runShared(delay, b, paths) == Outcome::Survived) return;
            }
            for (size_t i = 0; i < players.size(); ++i) paths[i].clear();
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
        if (!active || !isFake(player)) {
            if (!active) g_realDeath = true;
            return false;
        }
        g_dead = true;
        return true;
    }

    void onAttemptStart() {
        g_realDeath = false;
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
            WorldSnapshot world(layer);
            ActiveGuard guard;

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

    bool landingSoon(GJBaseGameLayer* layer, std::vector<PlayerObject*> const& players) {
        // A few ticks of warning: the auto-jump happens on the touchdown tick itself.
        constexpr int kLookTicks = 4;
        bool soon = false;
        {
            WorldSnapshot world(layer);
            ActiveGuard guard;
            Evaluator ev { layer, {}, true, true, 64, Clock::now() + std::chrono::milliseconds(2) };
            ev.dt = ibRuntime().lastDt;
            for (auto real : players) {
                if (!real || real->m_isDead || real->m_isOnGround || isFlyMode(real)) continue;
                auto& pool = g_pools[real == layer->m_player2 ? 1 : 0];
                if (static_cast<int>(pool.size()) != kPoolSize) continue;
                RealSnapshot snap(real);
                auto runner = pool[kRunner].data();
                copyState(runner, real, true);
                for (int t = 0; t < kLookTicks && !soon; ++t) {
                    if (ev.tick(runner, -1, nullptr) != Step::Ok) break;
                    if (ev.grounded) soon = true;
                }
                quiet(runner);
                restoreWorld(layer);
                snap.restoreIfChanged();
                if (soon) break;
            }
        }
        return soon;
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
        // Doomed with nothing in range to save it: the red line is the whole story.
        if (!decision.onTimeViable && decision.shift == 0) return;
        for (auto const& path : decision.predicted) line(path, { .2f, 1.f, .3f, 1.f });
    }

    void clearDebug() {
        if (g_debugDraw) g_debugDraw->clear();
    }
}

// ---------------------------------------------------------------------------
// Side-effect guards while simulating
// ---------------------------------------------------------------------------

// All guards run before any other mod's hooks, so other mods (Mega Hack, hitbox / death
// features, ...) never see the hidden look-ahead players or their deaths.
#define IB_RUN_FIRST \
    static void onModify(auto& self) { \
        for (auto hook : self.getAllHooks()) hook->setPriority(Priority::First); \
    }

class $modify(IBSimGameLayer, GJBaseGameLayer) {
    IB_RUN_FIRST
    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (sim::active) remember(object);
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void bumpPlayer(PlayerObject* player, EffectGameObject* object) {
        if (sim::active) remember(object);
        GJBaseGameLayer::bumpPlayer(player, object);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* object) {
        if (sim::active) {
            remember(object);
            g_ringTouched = object;
        }
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

    // Gamemode and teleport portals: the hidden player goes through them for real; what they
    // change level-wide (camera, portal bounds) is rolled back after the look-ahead.
    // In dual mode GD switches both players at a portal, so there we stop instead.
    bool portalAllowed() {
        if (!sim::active) return true;
        if (m_gameState.m_isDualMode) {
            g_stop = true;
            return false;
        }
        g_worldDirty = true;
        return true;
    }

    void playerWillSwitchMode(PlayerObject* player, GameObject* object) {
        if (!portalAllowed()) return;
        GJBaseGameLayer::playerWillSwitchMode(player, object);
    }

    void switchToRobotMode(PlayerObject* player, GameObject* object, bool noPortal) {
        if (!portalAllowed()) return;
        GJBaseGameLayer::switchToRobotMode(player, object, noPortal);
    }

    void switchToRollMode(PlayerObject* player, GameObject* object, bool noPortal) {
        if (!portalAllowed()) return;
        GJBaseGameLayer::switchToRollMode(player, object, noPortal);
    }

    void switchToSpiderMode(PlayerObject* player, GameObject* object, bool noPortal) {
        if (!portalAllowed()) return;
        GJBaseGameLayer::switchToSpiderMode(player, object, noPortal);
    }

    void teleportPlayer(TeleportPortalObject* object, PlayerObject* player) {
        if (!portalAllowed()) return;
        GJBaseGameLayer::teleportPlayer(object, player);
    }

    // Would add or remove the real second player: never simulated.
    void toggleDualMode(GameObject* object, bool dual, PlayerObject* player, bool noEffects) {
        if (sim::active) {
            g_stop = true;
            return;
        }
        GJBaseGameLayer::toggleDualMode(object, dual, player, noEffects);
    }

    // Purely visual for the level (mirror, ground animations, camera shake, particles).
    void toggleFlipped(bool flip, bool noEffects) {
        if (sim::active) return;
        GJBaseGameLayer::toggleFlipped(flip, noEffects);
    }

    void updateDualGround(PlayerObject* player, int mode, bool instant, float duration) {
        if (sim::active) return;
        GJBaseGameLayer::updateDualGround(player, mode, instant, duration);
    }

    void animateInDualGroundNew(GameObject* object, float height, bool instant, float duration) {
        if (sim::active) return;
        GJBaseGameLayer::animateInDualGroundNew(object, height, instant, duration);
    }

    void playExitDualEffect(PlayerObject* player) {
        if (sim::active) return;
        GJBaseGameLayer::playExitDualEffect(player);
    }

    void checkCameraLimitAfterTeleport(PlayerObject* player, float yOffset) {
        if (sim::active) return;
        GJBaseGameLayer::checkCameraLimitAfterTeleport(player, yOffset);
    }

    void shakeCamera(float duration, float strength, float interval) {
        if (sim::active) return;
        GJBaseGameLayer::shakeCamera(duration, strength, interval);
    }

    void spawnParticleTrigger(int particleID, cocos2d::CCPoint position, float rotation, float scale) {
        if (sim::active) return;
        GJBaseGameLayer::spawnParticleTrigger(particleID, position, rotation, scale);
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

    void updateDebugDraw() {
        if (sim::active) return;
        // Failsafe for "show hitboxes on death": if GD thinks a player died but neither real
        // player is dead, that came from a hidden look-ahead player. Turn it back off.
        if (auto pl = PlayLayer::get(); pl && static_cast<GJBaseGameLayer*>(pl) == this && m_playerDied && !g_realDeath) {
            bool realDeath = (m_player1 && m_player1->m_isDead) || (m_player2 && m_player2->m_isDead);
            if (!realDeath) m_playerDied = false;
        }
        GJBaseGameLayer::updateDebugDraw();
    }
};

class $modify(IBSimPlayer, PlayerObject) {
    IB_RUN_FIRST
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
    IB_RUN_FIRST
    void playShineEffect() {
        if (sim::active) return;
        GameObject::playShineEffect();
    }
};

class $modify(IBSimStreak, HardStreak) {
    IB_RUN_FIRST
    void addPoint(cocos2d::CCPoint point) {
        if (sim::active) return;
        HardStreak::addPoint(point);
    }
};
