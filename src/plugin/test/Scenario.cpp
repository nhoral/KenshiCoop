#define _CRT_SECURE_NO_WARNINGS 1

#include "Scenario.h"
#include "../CoopLog.h"
#include "../game/Engine.h"

#include <kenshi/GameWorld.h>
#include <kenshi/Character.h>

#include <cstdio>
#include <cmath>

namespace coop {
namespace {

// Log one "SCENARIO <kind> hand=i,s,t,c,cs pos=x,y,z" line for character 'c'.
// kind is "MEMBER" (authoritative) or "RECV" (observer). Returns false if the
// character's hand/pos couldn't be read.
bool logScenarioLine(const char* kind, Character* c) {
    if (!c) return false;
    unsigned int h[5];
    float x, y, z;
    if (!engine::readHand(c, h)) return false;
    if (!engine::readPos(c, &x, &y, &z)) return false;
    char b[160];
    _snprintf(b, sizeof(b) - 1,
              "SCENARIO %s hand=%u,%u,%u,%u,%u pos=%.2f,%.2f,%.2f",
              kind, h[0], h[1], h[2], h[3], h[4], x, y, z);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
    return true;
}

// Log a SCENARIO line straight from a captured EntityState. The hand field order
// (index,serial,type,container,containerSerial) MUST match logScenarioLine so the
// runner keys MEMBER and RECV lines identically.
void logScenarioEntity(const char* kind, const EntityState& e) {
    // task=<TaskType|65535> lets the runner's pose oracle compare the host's
    // current action against the join's reproduced one for seated NPCs.
    //
    // pelvis/crouch/idle are the AUTHORITATIVE pose oracle: read straight off the
    // rendered skeleton (pelvis = Bip01 height above ground; seated ~0.4-0.6 m,
    // standing ~0.9-1.1 m). We read them HERE (not in captureNpcs) by resolving the
    // hand back to the local Character* and calling readPoseState - this keeps pose
    // reading OFF the streaming-capture path (which broke host capture before). On
    // fault / unresolved, pelvis=-1 and the runner skips that sample.
    float pelvis = -1.0f; int crouch = -1, idle = -1, ptask = (int)e.task;
    Character* c = engine::resolve(e);
    // crouch=-2 is a diagnostic marker meaning the hand did NOT resolve to a local
    // Character here (so we never even called readPoseState) - distinct from -1
    // (resolved but the pose read returned nothing).
    if (c) engine::readPoseState(c, &pelvis, &idle, &crouch, &ptask);
    else   crouch = -2;
    char b[256];
    _snprintf(b, sizeof(b) - 1,
              "SCENARIO %s hand=%u,%u,%u,%u,%u pos=%.2f,%.2f,%.2f task=%u "
              "pelvis=%.2f crouch=%d idle=%d bs=%u",
              kind, e.hIndex, e.hSerial, e.hType, e.hContainer, e.hContainerSerial,
              e.x, e.y, e.z, (unsigned int)e.task, pelvis, crouch, idle,
              (unsigned int)e.bodyState);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}

// leader_move (Stage 1): the HOST orders its squad leader to walk to a nearby
// destination and streams its transform; the JOIN drives its local copy of that
// same (shared-save) leader to the received transform. Host logs MEMBER, join
// logs RECV; the runner cross-checks them within tolerance.
class LeaderMoveScenario : public Scenario {
public:
    LeaderMoveScenario()
        : started_(false), passed_(false), recvCount_(0),
          lastLogMs_(0), haveStart_(false), sx_(0), sy_(0), sz_(0) {}

    virtual const char* name() const { return "leader_move"; }

    virtual void onStart(const ScenarioContext& ctx) {
        started_ = true;
        if (ctx.isHost) {
            Character* ld = engine::leader(ctx.gw);
            if (ld && engine::readPos(ld, &sx_, &sy_, &sz_)) {
                haveStart_ = true;
                engine::orderMoveTo(ld, sx_ + LEG, sy_, sz_ + LEG);
            }
        }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Emit a MEMBER/RECV line ~2 Hz so the runner has positions to compare
        // and an anchor to time its screenshot.
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            Character* ld = engine::leader(ctx.gw);
            if (ctx.isHost) {
                // Oscillate between the start point and a far offset so the leader
                // keeps translating for the whole window (the later-loading join
                // then sees LIVE, sustained walking - the fair test for engine-
                // driven locomotion). Long legs + a long half-period keep straight
                // walking dominant and reversals rare (a reversal is a legitimate
                // stop/turn for engine locomotion, but we want them sparse). Then
                // SETTLE: return to the start and halt so the host streams a STILL
                // pose and the join converges for a fair cross-check.
                if (haveStart_ && ld) {
                    if (ctx.elapsedMs >= DURATION_MS - SETTLE_MS) {
                        engine::orderMoveTo(ld, sx_, sy_, sz_);
                    } else {
                        bool legB = ((ctx.elapsedMs / LEG_MS) % 2) != 0;
                        float tx = legB ? (sx_ + LEG) : sx_;
                        float tz = legB ? (sz_ + LEG) : sz_;
                        engine::orderMoveTo(ld, tx, sy_, tz);
                    }
                }
                logScenarioLine("MEMBER", ld);
            } else {
                if (logScenarioLine("RECV", ld)) ++recvCount_;
            }
        }

        if (ctx.elapsedMs >= DURATION_MS) {
            if (ctx.isHost) {
                // Authoritative side passes if its leader resolved (and, if we
                // had a start, ideally moved). Position match is the runner's job.
                passed_ = (engine::leader(ctx.gw) != 0);
            } else {
                passed_ = (recvCount_ >= 1); // observed + applied at least once
            }
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long DURATION_MS = 24000; // long enough to overlap the join's load
    static const unsigned long SETTLE_MS   = 8000;  // final halt window (fair cross-check + converge)
    static const unsigned long LEG_MS      = 6000;  // oscillation half-period (sparse reversals)
    static const float         LEG;                 // straight-walk leg length (units)
    bool          started_;
    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveStart_;
    float         sx_, sy_, sz_;
};

const float LeaderMoveScenario::LEG = 14.0f;

// npc_sync (Stage 4): the HOST streams nearby world NPCs (host-authoritative);
// the JOIN resolves each by hand and drives it (walk-drive while moving, park +
// AI-quiet at rest). Neither side scripts the NPCs - they do their own bar AI -
// so this just enumerates the same shared-save NPCs around each (co-located)
// leader and logs MEMBER (host, authoritative) / RECV (join, driven copy) per
// hand. The runner cross-checks positions per hand (ratio-based: stationary
// sitters match tightly, the occasional patroller may lag by interp/catch-up).
class NpcSyncScenario : public Scenario {
public:
    NpcSyncScenario() : passed_(false), recvCount_(0), lastLogMs_(0) {}

    virtual const char* name() const { return "npc_sync"; }

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState npcs[MAX_LOG];
            unsigned int n = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
            // Log EVERY captured NPC with a timestamp (the log line carries it). The
            // runner cross-checks TIME-ALIGNED samples (host MEMBER vs join RECV at
            // the nearest moment, both share the machine clock) - this is the only
            // correct way to compare autonomous movers across staggered clients;
            // latest-vs-latest measures unrelated moments and post-exit drift.
            const char* kind = ctx.isHost ? "MEMBER" : "RECV";
            for (unsigned int i = 0; i < n; ++i) logScenarioEntity(kind, npcs[i]);
            if (!ctx.isHost && n > 0) ++recvCount_;
        }

        // The host streams MUCH longer than the join runs, so it keeps feeding the
        // join's whole observation window (otherwise the host exits first, the
        // join's NPCs go stale, get released to local AI, and wander - which would
        // poison the cross-check). The join finishes first and reports the verdict.
        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            if (ctx.isHost) {
                EntityState npcs[MAX_LOG];
                passed_ = (engine::captureNpcs(ctx.gw, npcs, MAX_LOG) > 0); // streamed NPCs
            } else {
                passed_ = (recvCount_ >= 1); // resolved + drove at least one NPC
            }
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long HOST_DURATION_MS = 44000; // outlive the join's window
    static const unsigned long JOIN_DURATION_MS = 24000;
    static const unsigned int  MAX_LOG          = 40;     // cap NPCs logged per tick
    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
};

// craft_order (Stage 1, LIVE transition): unlike npc_sync (which validates boot-time
// state matching), this proves the runtime EVENT path. The worker starts UNTASKED
// (craft1 loaded WITHOUT host re-arm), the join observes it idle for a baseline
// window, then at ORDER_AT_MS the HOST issues the work order (engine::rearmCraftScene
// finds the baked fixture + nearest non-squad worker and goals it). The host then
// streams the new task; the join must transition its driven copy idle -> operating.
// Logs an "SCENARIO ORDER" marker (machine-timestamped) so the runner can split the
// join's per-hand task series into before/after and assert the live transition.
class CraftOrderScenario : public Scenario {
public:
    CraftOrderScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0), orderLogged_(false),
          haveWorker_(false), task_(0), lastOrderMs_(0) {}

    virtual const char* name() const { return "craft_order"; }

    virtual void onStart(const ScenarioContext& ctx) {
        // PIN the worker NOW (scene start), while the baked worker is still parked at
        // the prop and is unambiguously the nearest non-squad NPC. Picking "nearest
        // now" at order-time drifts: other world NPCs wander past the prop over the
        // baseline window, so the host would order the wrong body (observed). Pinning
        // by hand keeps host + join driving the SAME identity across the transition.
        if (ctx.isHost) {
            haveWorker_ = engine::pickCraftWorker(ctx.gw, workerHand_, &task_);
            if (haveWorker_) {
                char b[128];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO craft worker pinned hand=%u,%u,%u,%u,%u task=%d",
                          workerHand_[3], workerHand_[4], workerHand_[0],
                          workerHand_[1], workerHand_[2], task_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            } else {
                coop::logLine("SCENARIO craft worker pin FAILED (no fixture/worker)");
            }
        }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // HOST, baseline phase: keep the pinned worker genuinely UNTASKED at the prop.
        // Its faction AI otherwise patrols it out of the host's capture range, so when
        // ordered later it is too far to reach the work pose in the window. Holding it
        // parked at the prop is the faithful "untasked NPC standing by a prop" start.
        if (ctx.isHost && haveWorker_ && ctx.elapsedMs < ORDER_AT_MS) {
            engine::holdWorkerAtFixture(ctx.gw, workerHand_);
        }
        // HOST, order phase: hand the PINNED worker a work goal. orderCraftWorker is
        // GUARDED (no-ops once the worker is operating) so re-issuing on a throttle
        // recovers if its AI tries to drift, without thrashing pathing (the periodic
        // re-arm lesson). The marker is logged once, as the before/after split point.
        if (ctx.isHost && haveWorker_ && ctx.elapsedMs >= ORDER_AT_MS) {
            if (!orderLogged_ || ctx.elapsedMs - lastOrderMs_ >= 3000) {
                lastOrderMs_ = ctx.elapsedMs;
                bool ok = engine::orderCraftWorker(ctx.gw, workerHand_, task_);
                if (!orderLogged_) {
                    char b[128];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO ORDER issued task=%d ok=%d (craft live-order)",
                              task_, ok ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    orderLogged_ = true;
                }
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState npcs[MAX_LOG];
            unsigned int n = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
            const char* kind = ctx.isHost ? "MEMBER" : "RECV";
            for (unsigned int i = 0; i < n; ++i) logScenarioEntity(kind, npcs[i]);
            if (!ctx.isHost && n > 0) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            if (ctx.isHost) {
                EntityState npcs[MAX_LOG];
                passed_ = (engine::captureNpcs(ctx.gw, npcs, MAX_LOG) > 0);
            } else {
                passed_ = (recvCount_ >= 1);
            }
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long ORDER_AT_MS     = 18000; // issue after join logs idle baseline
    static const unsigned long HOST_DURATION_MS = 52000; // outlive the join (cover post-order)
    static const unsigned long JOIN_DURATION_MS = 34000; // baseline + post-order observation
    static const unsigned int  MAX_LOG          = 40;
    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          orderLogged_;
    bool          haveWorker_;
    unsigned int  workerHand_[5];
    int           task_;
    unsigned long lastOrderMs_;
};

// down_order (Stage 2, LIVE transition): the body-state analog of craft_order. The
// subject starts UPRIGHT (down1 loaded WITHOUT host re-arm, so nothing downs it), the
// join observes it standing for a baseline window, then at ORDER_AT_MS the HOST knocks
// it out (engine::orderDownSubject -> real knockout). The host then streams bodyState
// down; the join must transition its driven copy upright -> down. Logs a "SCENARIO
// DOWN" marker so the runner can split the join's per-hand bodyState series and assert
// the live transition (vs npc_sync, which only validates boot-time down state).
class DownOrderScenario : public Scenario {
public:
    DownOrderScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0), downLogged_(false),
          haveSubject_(false), lastDownMs_(0) {}

    virtual const char* name() const { return "down_order"; }

    virtual void onStart(const ScenarioContext& ctx) {
        // PIN the subject now (upright), so the host knocks out the SAME baked NPC the
        // join is already driving - re-picking "nearest now" at order-time could pick a
        // different body that the join has no upright baseline for.
        if (ctx.isHost) {
            haveSubject_ = engine::pickDownSubject(ctx.gw, subjHand_);
            if (haveSubject_) {
                char b[128];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO down subject pinned hand=%u,%u,%u,%u,%u",
                          subjHand_[3], subjHand_[4], subjHand_[0],
                          subjHand_[1], subjHand_[2]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            } else {
                coop::logLine("SCENARIO down subject pin FAILED (no nearby NPC)");
            }
        }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // HOST, baseline: keep the pinned subject UPRIGHT and in range (idle in place).
        if (ctx.isHost && haveSubject_ && ctx.elapsedMs < ORDER_AT_MS) {
            engine::holdSubjectUpright(ctx.gw, subjHand_);
        }
        // HOST, order phase: knock the PINNED subject out, re-issued on a throttle to
        // top up the KO timer so it stays down. The marker is the before/after split.
        if (ctx.isHost && haveSubject_ && ctx.elapsedMs >= ORDER_AT_MS) {
            if (!downLogged_ || ctx.elapsedMs - lastDownMs_ >= 3000) {
                lastDownMs_ = ctx.elapsedMs;
                bool ok = engine::orderDownSubject(ctx.gw, subjHand_);
                if (!downLogged_) {
                    char b[128];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO DOWN issued ok=%d (knockout live-order)",
                              ok ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    downLogged_ = true;
                }
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState npcs[MAX_LOG];
            unsigned int n = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
            const char* kind = ctx.isHost ? "MEMBER" : "RECV";
            for (unsigned int i = 0; i < n; ++i) logScenarioEntity(kind, npcs[i]);
            if (!ctx.isHost && n > 0) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            if (ctx.isHost) {
                EntityState npcs[MAX_LOG];
                passed_ = (engine::captureNpcs(ctx.gw, npcs, MAX_LOG) > 0);
            } else {
                passed_ = (recvCount_ >= 1);
            }
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long ORDER_AT_MS      = 18000;
    static const unsigned long HOST_DURATION_MS = 52000;
    static const unsigned long JOIN_DURATION_MS = 34000;
    static const unsigned int  MAX_LOG          = 40;
    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          downLogged_;
    bool          haveSubject_;
    unsigned int  subjHand_[5];
    unsigned long lastDownMs_;
};

// death_order (Stage 3 reliable-event proof): like down_order, but at ORDER_AT_MS the
// HOST KILLS the pinned subject (engine::killSubject -> Character::isDead()). The host
// detects the bodyState DEAD edge and emits a RELIABLE EVT_DEATH; the join must log a
// matching "[event] RECV ev=2" for that hand. The point is reliability: run this under
// -NetSimLossPct and the unreliable bodyState batches drop, but the death event still
// arrives (it rides the reliable channel). Reuses the down_order pin/hold baseline.
class DeathOrderScenario : public Scenario {
public:
    DeathOrderScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0), deathLogged_(false),
          haveSubject_(false), lastKillMs_(0) {}

    virtual const char* name() const { return "death_order"; }

    virtual void onStart(const ScenarioContext& ctx) {
        if (ctx.isHost) {
            haveSubject_ = engine::pickDownSubject(ctx.gw, subjHand_);
            if (haveSubject_) {
                char b[128];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO death subject pinned hand=%u,%u,%u,%u,%u",
                          subjHand_[3], subjHand_[4], subjHand_[0],
                          subjHand_[1], subjHand_[2]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            } else {
                coop::logLine("SCENARIO death subject pin FAILED (no nearby NPC)");
            }
        }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Baseline: hold the pinned subject upright + in range (clean "alive before").
        if (ctx.isHost && haveSubject_ && ctx.elapsedMs < ORDER_AT_MS) {
            engine::holdSubjectUpright(ctx.gw, subjHand_);
        }
        // Order phase: KILL the pinned subject, re-asserted on a throttle so it stays
        // dead. The first kill is the marker the runner splits the series on.
        if (ctx.isHost && haveSubject_ && ctx.elapsedMs >= ORDER_AT_MS) {
            if (!deathLogged_ || ctx.elapsedMs - lastKillMs_ >= 3000) {
                lastKillMs_ = ctx.elapsedMs;
                bool ok = engine::killSubject(ctx.gw, subjHand_);
                if (!deathLogged_) {
                    char b[128];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO DEATH issued ok=%d (kill live-order)", ok ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    deathLogged_ = true;
                }
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState npcs[MAX_LOG];
            unsigned int n = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
            const char* kind = ctx.isHost ? "MEMBER" : "RECV";
            for (unsigned int i = 0; i < n; ++i) logScenarioEntity(kind, npcs[i]);
            if (!ctx.isHost && n > 0) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            if (ctx.isHost) {
                EntityState npcs[MAX_LOG];
                passed_ = (engine::captureNpcs(ctx.gw, npcs, MAX_LOG) > 0);
            } else {
                passed_ = (recvCount_ >= 1);
            }
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long ORDER_AT_MS      = 18000;
    static const unsigned long HOST_DURATION_MS = 52000;
    static const unsigned long JOIN_DURATION_MS = 34000;
    static const unsigned int  MAX_LOG          = 40;
    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          deathLogged_;
    bool          haveSubject_;
    unsigned int  subjHand_[5];
    unsigned long lastKillMs_;
};

// combat_probe (Phase 3c, L5 READ probe): the HOST spawns two mutually-hostile
// non-squad NPCs in front of the leader and orders them to melee each other, then
// logs a "COMBAT ..." line per duelist each tick (inCombat/ranged/underMelee/
// fleeing/target). No wire, no apply - this validates that the combat-state
// primitives populate during a live fight before we replicate them. The duelists are
// runtime host spawns (not baked), so the join only logs whatever NPCs it has; the
// host log is the deliverable. Periodically re-arms disengaged duelists.
class CombatProbeScenario : public Scenario {
public:
    CombatProbeScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0), lastRearmMs_(0),
          haveDuel_(false) {}

    virtual const char* name() const { return "combat_probe"; }

    virtual void onStart(const ScenarioContext& ctx) {
        if (ctx.isHost) {
            haveDuel_ = engine::setupDuelScene(ctx.gw);
            // setupDuelScene now spawns PEACEFUL; the probe wants a live fight, so
            // trigger it immediately (the onTick re-arm keeps it going if they disengage).
            if (haveDuel_) engine::startDuel(ctx.gw);
            coop::logLine(haveDuel_ ? "SCENARIO duel spawned"
                                    : "SCENARIO duel spawn FAILED");
        }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // HOST: keep disengaged duelists fighting, and log their combat state.
        if (ctx.isHost && haveDuel_) {
            if (ctx.elapsedMs - lastRearmMs_ >= 2500) {
                lastRearmMs_ = ctx.elapsedMs;
                engine::rearmDuelScene(ctx.gw);
            }
            if (ctx.elapsedMs - lastLogMs_ >= 700 || lastLogMs_ == 0) {
                lastLogMs_ = ctx.elapsedMs;
                engine::logDuelCombat(ctx.gw);
            }
        }

        // Both: position log (anchors the screenshot + gives the join a RECV signal).
        if (!ctx.isHost) {
            EntityState npcs[MAX_LOG];
            unsigned int n = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
            for (unsigned int i = 0; i < n; ++i) logScenarioEntity("RECV", npcs[i]);
            if (n > 0) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? haveDuel_ : (recvCount_ >= 1);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long HOST_DURATION_MS = 40000;
    static const unsigned long JOIN_DURATION_MS = 28000;
    static const unsigned int  MAX_LOG          = 40;
    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    unsigned long lastRearmMs_;
    bool          haveDuel_;
};

// combat_order (Phase 3c, L5 LIVE transition): the payoff test. Both clients load the
// baked 'duel1' (two PEACEFUL, co-located non-squad NPCs present on both). The host
// pins them, HOLDS them peaceful for a baseline window, then at ORDER_AT_MS issues the
// LIVE attack order (startDuel). The host captures task==TASK_COMBAT_MELEE for them and
// streams it; the join reproduces the intent (orders its local copies to melee the same
// target) so its OWN copies enter combat. Because the join independently re-reads its
// copies via captureNpcs, the RECV series shows task flip NONE->65024 (combat) only
// AFTER the order - proving a live peaceful->fighting transition, not a baked load
// state. The runner splits the RECV series on "SCENARIO COMBAT issued".
class CombatOrderScenario : public Scenario {
public:
    CombatOrderScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0), combatLogged_(false),
          havePins_(false), lastRearmMs_(0) {}

    virtual const char* name() const { return "combat_order"; }

    virtual void onStart(const ScenarioContext& ctx) {
        if (ctx.isHost) {
            havePins_ = engine::pickDuelSubjects(ctx.gw, handA_, handB_);
            if (havePins_) {
                char b[160];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO duel subjects pinned A=%u,%u B=%u,%u",
                          handA_[3], handA_[4], handB_[3], handB_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            } else {
                coop::logLine("SCENARIO duel pin FAILED (need two nearby non-squad NPCs)");
            }
        }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Baseline: hold both duelists peaceful + in range (clean "not fighting before").
        if (ctx.isHost && havePins_ && ctx.elapsedMs < ORDER_AT_MS) {
            engine::holdDuelistsPeaceful(ctx.gw);
        }
        // Order phase: trigger the live fight once, then re-arm disengaged duelists on a
        // throttle so the fight is sustained for the whole post-order window.
        if (ctx.isHost && havePins_ && ctx.elapsedMs >= ORDER_AT_MS) {
            if (!combatLogged_) {
                int n = engine::startDuel(ctx.gw);
                char b[96];
                _snprintf(b, sizeof(b) - 1, "SCENARIO COMBAT issued orders=%d", n);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                combatLogged_ = true;
                lastRearmMs_ = ctx.elapsedMs;
            } else if (ctx.elapsedMs - lastRearmMs_ >= 2500) {
                lastRearmMs_ = ctx.elapsedMs;
                engine::rearmDuelScene(ctx.gw);
                engine::logDuelCombat(ctx.gw); // host-side combat ground truth
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState npcs[MAX_LOG];
            unsigned int n = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
            const char* kind = ctx.isHost ? "MEMBER" : "RECV";
            for (unsigned int i = 0; i < n; ++i) logScenarioEntity(kind, npcs[i]);
            if (!ctx.isHost && n > 0) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            if (ctx.isHost) {
                EntityState npcs[MAX_LOG];
                passed_ = havePins_ && (engine::captureNpcs(ctx.gw, npcs, MAX_LOG) > 0);
            } else {
                passed_ = (recvCount_ >= 1);
            }
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long ORDER_AT_MS      = 18000;
    static const unsigned long HOST_DURATION_MS = 52000;
    static const unsigned long JOIN_DURATION_MS = 34000;
    static const unsigned int  MAX_LOG          = 40;
    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          combatLogged_;
    bool          havePins_;
    unsigned int  handA_[5];
    unsigned int  handB_[5];
    unsigned long lastRearmMs_;
};

// combat_kill (Phase 3c, L5 OUTCOME): proves combat resolution is HOST-AUTHORITATIVE.
// Both clients load duel1 and run their OWN local fight, but the host decides who wins:
// at ORDER_AT_MS it triggers the duel AND weakens duelist B (woundSubject - lowers blood
// only, no scaffold collapse), so duelist A downs B via genuine melee. When the host's B
// goes down, the host emits a RELIABLE KO/death event STAMPED with A as the actor (combat
// attribution), and the join's body-state override forces ITS B down to match - even if
// the join's local sim had B winning. The runner asserts: the host sent a combat
// KO/death with a non-zero actor, the join received that exact event (hand + actor), and
// the join's victim is down afterwards. Reuses combat_order's pin/hold baseline.
class CombatKillScenario : public Scenario {
public:
    CombatKillScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0), combatLogged_(false),
          havePins_(false), lastRearmMs_(0), lastWoundMs_(0), koLogged_(false) {}

    virtual const char* name() const { return "combat_kill"; }

    virtual void onStart(const ScenarioContext& ctx) {
        if (ctx.isHost) {
            havePins_ = engine::pickDuelSubjects(ctx.gw, handA_, handB_);
            if (havePins_) {
                char b[160];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO duel subjects pinned A=%u,%u B=%u,%u",
                          handA_[3], handA_[4], handB_[3], handB_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            } else {
                coop::logLine("SCENARIO duel pin FAILED (need two nearby non-squad NPCs)");
            }
        }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (ctx.isHost && havePins_ && ctx.elapsedMs < ORDER_AT_MS) {
            engine::holdDuelistsPeaceful(ctx.gw);
        }
        if (ctx.isHost && havePins_ && ctx.elapsedMs >= ORDER_AT_MS) {
            if (!combatLogged_) {
                int n = engine::startDuel(ctx.gw);
                char b[128];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO COMBAT issued orders=%d loser=B(%u,%u)",
                          n, handB_[3], handB_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                combatLogged_ = true; lastRearmMs_ = ctx.elapsedMs;
            } else if (ctx.elapsedMs < (ORDER_AT_MS + KO_DELAY_MS)) {
                // Let them visibly trade blows for a few seconds; keep A on B.
                if (ctx.elapsedMs - lastRearmMs_ >= 2000) {
                    lastRearmMs_ = ctx.elapsedMs;
                    engine::rearmDuelScene(ctx.gw);
                    engine::logDuelCombat(ctx.gw);
                }
            } else {
                // Decisive takedown: while A is actively meleeing B (so the attacker map
                // names A), KO B. Two random NPCs won't reliably KO each other in-window,
                // so the takedown is enforced; the REPLICATION path (down edge -> reliable
                // event stamped with the attacker -> join forces B down) is what we test.
                // Re-assert on a throttle so B stays down (the KO timer needs topping).
                if (ctx.elapsedMs - lastWoundMs_ >= 1500) {
                    lastWoundMs_ = ctx.elapsedMs;
                    engine::orderDownSubject(ctx.gw, handB_); // knockDown B by hand
                    engine::logDuelCombat(ctx.gw);
                }
                if (!koLogged_) {
                    char b[128];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO KO enforced loser=B(%u,%u)",
                              handB_[3], handB_[4]);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    koLogged_ = true;
                }
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState npcs[MAX_LOG];
            unsigned int n = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
            const char* kind = ctx.isHost ? "MEMBER" : "RECV";
            for (unsigned int i = 0; i < n; ++i) logScenarioEntity(kind, npcs[i]);
            if (!ctx.isHost && n > 0) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            if (ctx.isHost) {
                EntityState npcs[MAX_LOG];
                passed_ = havePins_ && (engine::captureNpcs(ctx.gw, npcs, MAX_LOG) > 0);
            } else {
                passed_ = (recvCount_ >= 1);
            }
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long ORDER_AT_MS      = 14000;
    static const unsigned long KO_DELAY_MS      = 8000;  // fight visibly before the takedown
    static const unsigned long HOST_DURATION_MS = 60000;
    static const unsigned long JOIN_DURATION_MS = 48000;
    static const unsigned int  MAX_LOG          = 40;
    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          combatLogged_;
    bool          havePins_;
    unsigned int  handA_[5];
    unsigned int  handB_[5];
    unsigned long lastRearmMs_;
    unsigned long lastWoundMs_;
    bool          koLogged_;
};

// coop_presence (Phase 3.5, BIDIRECTIONAL presence - the keystone two-player test):
// both clients MOVE their OWNED squad member (chosen by save-stable hand-rank, the
// same ordering the Replicator partitions on: host owns rank 0, join owns rank 1 -
// leader-first) and stream it, while driving + observing the PEER's owned member.
// Each side logs MEMBER for its OWN member (authoritative truth it streams) and RECV
// for the PEER's member (the local driven copy), so the runner cross-checks BOTH
// directions by hand: host MEMBER(rank0) vs join RECV(rank0), and join MEMBER(rank1)
// vs host RECV(rank1). Proves each player's character is present + correctly placed
// on the other client. Requires a shared save with >=2 controllable squad members.
class CoopPresenceScenario : public Scenario {
public:
    CoopPresenceScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0),
          haveStart_(false), sx_(0), sy_(0), sz_(0) {}

    virtual const char* name() const { return "coop_presence"; }

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        const unsigned int ownRank  = ctx.isHost ? 0u : 1u; // our squad-tab rank
        const unsigned int peerRank = ctx.isHost ? 1u : 0u; // the peer's squad-tab rank

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, /*leaderOnly*/ false, sq, MAX_SQUAD);

            // Classify every shared-squad member by its SQUAD-TAB rank (same key the
            // Replicator partitions on: distinct hand-containers, sorted). Log MEMBER
            // for ALL members in OUR tab(s) (authoritative truth we stream) and RECV
            // for ALL members in the PEER's tab(s) (the bodies we drive). Pick the
            // lowest-hand owned member as the MOVER so the peer sees sustained motion.
            int leaderIdx = -1; bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int cr = containerRankOf(sq, n, i);
                if (cr < 0) continue;
                if ((unsigned int)cr == ownRank) {
                    logScenarioEntity("MEMBER", sq[i]);
                    if (leaderIdx < 0 || handLess(sq[i], sq[leaderIdx])) leaderIdx = (int)i;
                } else if ((unsigned int)cr == peerRank) {
                    logScenarioEntity("RECV", sq[i]); sawPeer = true;
                }
            }
            if (sawPeer) ++recvCount_;

            // Move our tab leader so the peer sees LIVE motion, then settle to the start
            // so both clients converge for a clean stationary cross-check in the
            // overlapping window (movement proves presence; the settle proves placement).
            if (leaderIdx >= 0) {
                Character* oc = engine::resolve(sq[leaderIdx]);
                if (oc) {
                    if (!haveStart_) {
                        sx_ = sq[leaderIdx].x; sy_ = sq[leaderIdx].y; sz_ = sq[leaderIdx].z;
                        haveStart_ = true;
                    }
                    if (ctx.elapsedMs < MOVE_MS) {
                        bool legB = ((ctx.elapsedMs / LEG_MS) % 2) != 0;
                        engine::orderMoveTo(oc, legB ? sx_ + LEG : sx_, sy_,
                                                legB ? sz_ + LEG : sz_);
                    } else {
                        engine::orderMoveTo(oc, sx_, sy_, sz_); // settle back to start
                    }
                }
            }
        }

        // The host outlives the join (keeps streaming through the join's whole window
        // so the join's RECV never goes stale); the join reports the verdict.
        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = haveStart_ && (recvCount_ >= 1);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    // Full hand order (used only to pick a stable per-tab "leader" mover).
    static bool handLess(const EntityState& a, const EntityState& b) {
        if (a.hType != b.hType) return a.hType < b.hType;
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        if (a.hContainerSerial != b.hContainerSerial) return a.hContainerSerial < b.hContainerSerial;
        if (a.hIndex != b.hIndex) return a.hIndex < b.hIndex;
        return a.hSerial < b.hSerial;
    }
    // Squad-tab identity = the hand CONTAINER (hContainer,hContainerSerial).
    static bool ctnrLess(const EntityState& a, const EntityState& b) {
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        return a.hContainerSerial < b.hContainerSerial;
    }
    static bool ctnrEq(const EntityState& a, const EntityState& b) {
        return a.hContainer == b.hContainer && a.hContainerSerial == b.hContainerSerial;
    }
    // Rank of member i's SQUAD TAB among the distinct, sorted containers (O(n^2),
    // squads are tiny). MUST match the Replicator's container-rank partition so the
    // scenario's "owned" set is exactly the set the Replicator streams as owned.
    static int containerRankOf(const EntityState* sq, unsigned int n, unsigned int i) {
        EntityState distinct[MAX_SQUAD]; unsigned int dn = 0;
        for (unsigned int a = 0; a < n; ++a) {
            bool seen = false;
            for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[a])) { seen = true; break; }
            if (!seen && dn < MAX_SQUAD) distinct[dn++] = sq[a];
        }
        for (unsigned int a = 1; a < dn; ++a)
            for (unsigned int b = a; b > 0 && ctnrLess(distinct[b], distinct[b-1]); --b) {
                EntityState t = distinct[b]; distinct[b] = distinct[b-1]; distinct[b-1] = t;
            }
        for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[i])) return (int)b;
        return -1;
    }

    static const unsigned long HOST_DURATION_MS = 44000; // outlive the join's window
    static const unsigned long JOIN_DURATION_MS = 24000;
    static const unsigned long MOVE_MS          = 12000; // oscillate, then settle
    static const unsigned long LEG_MS           = 4000;  // oscillation half-period
    static const unsigned int  MAX_SQUAD        = 32;
    static const float         LEG;
    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveStart_;
    float         sx_, sy_, sz_;
};
const float CoopPresenceScenario::LEG = 12.0f;

// Phase 4a: container-contents (inventory) replication. Both clients anchor on the
// SAME container (v1: the leader's own inventory - a save-stable hand that resolves
// cross-client). Each samples its LOCAL container's contents (count + order-
// independent content hash) every 500 ms; the host performs a LIVE add mid-run. The
// join must (a) observe a content CHANGE (>=2 distinct hashes - proving it wasn't a
// static loaded state) and (b) end with MORE items than its own baseline. The runner
// additionally cross-checks the host's and join's FINAL hashes match (same multiset).
class InventorySyncScenario : public Scenario {
public:
    InventorySyncScenario()
        : passed_(false), haveContainer_(false), added_(false), lastLogMs_(0),
          samples_(0), distinct_(0), firstCount_(0), lastCount_(0),
          firstHash_(0), lastHash_(0), prevHash_(0) {
        for (int i = 0; i < 5; ++i) cHand_[i] = 0;
    }

    virtual const char* name() const { return "inv_order"; }

    virtual void onStart(const ScenarioContext& ctx) {
        haveContainer_ = engine::pickInventoryContainer(ctx.gw, cHand_);
        char b[160];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO INV anchor have=%d hand=%u,%u,%u,%u,%u",
            haveContainer_ ? 1 : 0, cHand_[0], cHand_[1], cHand_[2], cHand_[3], cHand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (haveContainer_ && (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0)) {
            lastLogMs_ = ctx.elapsedMs;

            InvItemEntry items[INV_ITEMS_MAX];
            unsigned int hash = 0;
            unsigned int n = engine::captureContainerContents(
                ctx.gw, cHand_, items, INV_ITEMS_MAX, &hash);

            if (samples_ == 0) { firstCount_ = n; firstHash_ = hash; prevHash_ = hash; }
            else if (hash != prevHash_) { ++distinct_; prevHash_ = hash; }
            lastCount_ = n; lastHash_ = hash; ++samples_;

            char b[160];
            _snprintf(b, sizeof(b) - 1,
                "SCENARIO INV %s t=%lu count=%u hash=%u",
                ctx.isHost ? "MEMBER" : "RECV",
                (unsigned long)ctx.elapsedMs, n, hash);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);

            // Host performs the LIVE add once, after a short baseline window. The
            // content-change then rides the reliable snapshot channel to the join.
            if (ctx.isHost && !added_ && ctx.elapsedMs >= ADD_MS) {
                added_ = true;
                char sid[48]; sid[0] = '\0';
                int got = engine::addTestItemsToContainer(ctx.gw, cHand_, 1, sid, sizeof(sid));
                char m[200];
                _snprintf(m, sizeof(m) - 1,
                    "SCENARIO INV ADD added=%d sid='%s'", got, sid[0] ? sid : "(none)");
                m[sizeof(m) - 1] = '\0'; coop::logLine(m);
            }
        }

        // Host outlives the join so its stream/snapshot never goes stale mid-window.
        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            // Host: it performed the live add. Join: it ended holding synced (non-empty)
            // content. The launch stagger means the join may start sampling AFTER the
            // add already propagated (so it never sees the 0->1 edge itself); the
            // runner's INV-SYNC oracle is the authoritative cross-client proof
            // (host live-change + host/join final-hash match). distinct_ is advisory.
            if (ctx.isHost) passed_ = haveContainer_ && added_;
            else            passed_ = haveContainer_ && (lastCount_ > 0) && (lastHash_ != 0);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long HOST_DURATION_MS = 40000; // outlive the join's window
    static const unsigned long JOIN_DURATION_MS = 24000;
    static const unsigned long ADD_MS           = 8000;  // baseline, then add live

    bool          passed_;
    bool          haveContainer_;
    bool          added_;
    unsigned long lastLogMs_;
    unsigned int  cHand_[5];
    unsigned int  samples_;
    unsigned int  distinct_;   // count of content-hash changes observed
    unsigned int  firstCount_;
    unsigned int  lastCount_;
    unsigned int  firstHash_;
    unsigned int  lastHash_;
    unsigned int  prevHash_;
};

// inv_bidir (Phase 4a, BIDIRECTIONAL container-contents): each client mutates ONLY the
// inventory of a squad member it OWNS (host = tab-rank 0, join = tab-rank 1 - the same
// partition the Replicator streams on) and samples BOTH squad-tab containers every
// 500 ms, logging OWN (authoritative) and PEER (reconciled) lines keyed by rank. Each
// side runs an ADD-then-REMOVE sequence with a DISTINCT net delta (host -> +2, join ->
// +1) so the runner's per-rank convergence check is unambiguous and removals (not just
// adds) must propagate. The runner cross-checks, per rank, that the NON-authoring side
// converged to the author's FINAL contents - proving inventory flows both ways with no
// loss/dupe on the supported (owned-container) path. Requires a shared save with >=2
// squad tabs (rank 0 and rank 1).
class InventoryBidirScenario : public Scenario {
public:
    InventoryBidirScenario()
        : passed_(false), haveOwn_(false), added_(false), removed_(false),
          lastLogMs_(0), samples_(0), ownRank_(0),
          firstOwnCount_(0), lastOwnCount_(0), prevOwnHash_(0), distinctOwn_(0) {
        for (int i = 0; i < 5; ++i) ownHand_[i] = 0;
    }

    virtual const char* name() const { return "inv_bidir"; }

    virtual void onStart(const ScenarioContext& ctx) {
        ownRank_ = ctx.isHost ? 0u : 1u;
        haveOwn_ = resolveRankContainer(ctx.gw, ownRank_, ownHand_);
        char b[160];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO INVB anchor own_rank=%u have=%d hand=%u,%u,%u,%u,%u",
            ownRank_, haveOwn_ ? 1 : 0,
            ownHand_[0], ownHand_[1], ownHand_[2], ownHand_[3], ownHand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            // Sample BOTH squad-tab containers so each client logs its OWN container
            // (authoritative truth it streams) and the PEER's (the one it reconciles).
            for (unsigned int rank = 0; rank < 2; ++rank) {
                unsigned int cHand[5];
                if (!resolveRankContainer(ctx.gw, rank, cHand)) continue;
                InvItemEntry items[INV_ITEMS_MAX];
                unsigned int hash = 0;
                unsigned int n = engine::captureContainerContents(
                    ctx.gw, cHand, items, INV_ITEMS_MAX, &hash);
                const char* role = (rank == ownRank_) ? "OWN" : "PEER";
                if (rank == ownRank_) {
                    if (samples_ == 0) { firstOwnCount_ = n; prevOwnHash_ = hash; }
                    else if (hash != prevOwnHash_) { ++distinctOwn_; prevOwnHash_ = hash; }
                    lastOwnCount_ = n; ++samples_;
                }
                char b[160];
                _snprintf(b, sizeof(b) - 1,
                    "SCENARIO INVB r=%u %s t=%lu count=%u hash=%u",
                    rank, role, (unsigned long)ctx.elapsedMs, n, hash);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }

            // Mutate ONLY our OWN container: ADD a burst, then REMOVE part of it. The
            // remove forces the peer to converge DOWN (not just up), so a removal that
            // failed to propagate would leave the peer stuck above the author's count.
            if (haveOwn_) {
                int addN = ctx.isHost ? 3 : 2; // host ends +2 over baseline, join ends +1
                int remN = 1;
                if (!added_ && ctx.elapsedMs >= ADD_MS) {
                    added_ = true;
                    char sid[48]; sid[0] = '\0';
                    int got = engine::addTestItemsToContainer(ctx.gw, ownHand_, addN, sid, sizeof(sid));
                    char m[200];
                    _snprintf(m, sizeof(m) - 1,
                        "SCENARIO INVB ADD r=%u n=%d sid='%s'", ownRank_, got, sid[0] ? sid : "(none)");
                    m[sizeof(m) - 1] = '\0'; coop::logLine(m);
                }
                if (added_ && !removed_ && ctx.elapsedMs >= REM_MS) {
                    removed_ = true;
                    int got = engine::removeTestItemsFromContainer(ctx.gw, ownHand_, remN);
                    char m[160];
                    _snprintf(m, sizeof(m) - 1, "SCENARIO INVB REM r=%u n=%d", ownRank_, got);
                    m[sizeof(m) - 1] = '\0'; coop::logLine(m);
                }
            }
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            // In-plugin verdict only confirms the scenario EXECUTED (resolved its owned
            // container, sampled, and - on each side - performed its add+remove). The
            // runner's per-rank cross-client convergence check (Test-InventoryBidir) is
            // the authoritative no-loss/no-dupe gate.
            passed_ = haveOwn_ && (samples_ > 0) && added_ && removed_;
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    // Resolve the lowest-hand squad member whose squad-tab CONTAINER has the given rank
    // (distinct hand-containers, sorted - the SAME key the Replicator partitions on) and
    // write its object hand (== its personal-inventory container hand) to out.
    static bool resolveRankContainer(GameWorld* gw, unsigned int rank, unsigned int out[5]) {
        for (int i = 0; i < 5; ++i) out[i] = 0;
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, MAX_SQUAD);
        if (n == 0) return false;
        int best = -1;
        for (unsigned int i = 0; i < n; ++i) {
            int cr = containerRankOf(sq, n, i);
            if (cr < 0 || (unsigned int)cr != rank) continue;
            if (best < 0 || handLess(sq[i], sq[best])) best = (int)i;
        }
        if (best < 0) return false;
        out[0] = sq[best].hType; out[1] = sq[best].hContainer;
        out[2] = sq[best].hContainerSerial; out[3] = sq[best].hIndex; out[4] = sq[best].hSerial;
        return true;
    }
    static bool handLess(const EntityState& a, const EntityState& b) {
        if (a.hType != b.hType) return a.hType < b.hType;
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        if (a.hContainerSerial != b.hContainerSerial) return a.hContainerSerial < b.hContainerSerial;
        if (a.hIndex != b.hIndex) return a.hIndex < b.hIndex;
        return a.hSerial < b.hSerial;
    }
    static bool ctnrLess(const EntityState& a, const EntityState& b) {
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        return a.hContainerSerial < b.hContainerSerial;
    }
    static bool ctnrEq(const EntityState& a, const EntityState& b) {
        return a.hContainer == b.hContainer && a.hContainerSerial == b.hContainerSerial;
    }
    static int containerRankOf(const EntityState* sq, unsigned int n, unsigned int i) {
        EntityState distinct[MAX_SQUAD]; unsigned int dn = 0;
        for (unsigned int a = 0; a < n; ++a) {
            bool seen = false;
            for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[a])) { seen = true; break; }
            if (!seen && dn < MAX_SQUAD) distinct[dn++] = sq[a];
        }
        for (unsigned int a = 1; a < dn; ++a)
            for (unsigned int b = a; b > 0 && ctnrLess(distinct[b], distinct[b-1]); --b) {
                EntityState t = distinct[b]; distinct[b] = distinct[b-1]; distinct[b-1] = t;
            }
        for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[i])) return (int)b;
        return -1;
    }

    static const unsigned long HOST_DURATION_MS = 44000; // outlive the join's window
    static const unsigned long JOIN_DURATION_MS = 28000;
    static const unsigned long ADD_MS           = 8000;  // baseline, then add
    static const unsigned long REM_MS           = 14000; // then remove part of it

    static const unsigned int  MAX_SQUAD        = 32;

    bool          passed_;
    bool          haveOwn_;
    bool          added_;
    bool          removed_;
    unsigned long lastLogMs_;
    unsigned int  samples_;
    unsigned int  ownRank_;
    unsigned int  ownHand_[5];
    unsigned int  firstOwnCount_;
    unsigned int  lastOwnCount_;
    unsigned int  prevOwnHash_;
    unsigned int  distinctOwn_;
};

// inv_equip: EQUIPPED-gear (armour/weapon slot) sync. Each client owns one squad tab
// (host rank 0, join rank 1). On the geared member of its OWN tab it UNEQUIPS one REAL
// (save-loaded) worn item and leaves it off - the "drop/unequip armour" action. Because
// the peer loaded the same save, its local copy of that character starts wearing the
// SAME gear, so converging to "one fewer worn item" forces it to actively REMOVE its
// worn copy; a removal that failed to propagate (loose-only sync's blind spot, the bug
// the user hit) would leave the peer still wearing it - a permanent eq/hash mismatch.
// The snapshot now carries each item's equipped flag + slot. The runner cross-checks,
// per rank, that the author's worn count dropped and the NON-authoring side converged
// to the author's FINAL worn state (count + equipped-count + content hash). Fabricated
// re-equips don't persist in the engine, so the equip (up) path is intentionally out of
// scope here. Requires a shared save with >=2 squad tabs whose members wear gear.
class InventoryEquipScenario : public Scenario {
public:
    explicit InventoryEquipScenario(bool reequipMode = false)
        : passed_(false), haveOwn_(false), haveEq_(false), unequipped_(false),
          reequipped_(false), reequipMode_(reequipMode),
          lastLogMs_(0), samples_(0), ownRank_(0),
          baseEqCount_(0), baseType_(0), lastOwnEq_(0) {
        for (int i = 0; i < 5; ++i) ownHand_[i] = 0;
        for (int r = 0; r < 2; ++r) { rankHave_[r] = false; for (int i = 0; i < 5; ++i) rankHand_[r][i] = 0; }
        baseSid_[0] = '\0';
    }

    virtual const char* name() const { return reequipMode_ ? "inv_reequip" : "inv_equip"; }

    virtual void onStart(const ScenarioContext& ctx) {
        ownRank_ = ctx.isHost ? 0u : 1u;
        // Resolve+cache the geared member of BOTH tabs ONCE (the lead isn't always the
        // geared one). Caching keeps sampling locked to the same character even after we
        // unequip - so the OWN/PEER series track one member, not a shifting "first geared"
        // pick. Both clients share the save, so each rank resolves to the SAME member.
        for (unsigned int r = 0; r < 2; ++r)
            rankHave_[r] = resolveGearedRankContainer(ctx.gw, r, rankHand_[r]);
        haveOwn_ = rankHave_[ownRank_];
        if (haveOwn_) {
            for (int i = 0; i < 5; ++i) ownHand_[i] = rankHand_[ownRank_][i];
            haveEq_ = engine::findEquippedItemKey(ctx.gw, ownHand_, baseSid_, sizeof(baseSid_),
                                                  &baseType_, &baseEqCount_) != 0;
        }
        char b[220];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO INVE anchor own_rank=%u have=%d eq=%d baseEq=%d sid='%s' type=%u",
            ownRank_, haveOwn_ ? 1 : 0, haveEq_ ? 1 : 0, baseEqCount_,
            baseSid_[0] ? baseSid_ : "(none)", baseType_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            // Sample BOTH squad-tab containers: each client logs its OWN tab (the worn
            // state it streams) and the PEER's (the one it reconciles), with the count
            // of EQUIPPED items broken out so the runner can prove the slot - not just
            // a loose copy - converged.
            for (unsigned int rank = 0; rank < 2; ++rank) {
                if (!rankHave_[rank]) continue;
                InvItemEntry items[INV_ITEMS_MAX];
                unsigned int hash = 0;
                unsigned int n = engine::captureContainerContents(
                    ctx.gw, rankHand_[rank], items, INV_ITEMS_MAX, &hash);
                unsigned int eq = 0;
                for (unsigned int i = 0; i < n; ++i) if (items[i].equipped) ++eq;
                const char* role = (rank == ownRank_) ? "OWN" : "PEER";
                if (rank == ownRank_) { ++samples_; lastOwnEq_ = eq; }
                char b[180];
                _snprintf(b, sizeof(b) - 1,
                    "SCENARIO INVE r=%u %s t=%lu count=%u eq=%u hash=%u",
                    rank, role, (unsigned long)ctx.elapsedMs, n, eq, hash);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }

            // UNEQUIP one REAL worn item from our OWN geared member and leave it off
            // (the "drop/unequip armour" action). Because the peer loaded the SAME save,
            // its local copy of this character starts wearing the SAME gear; to converge
            // to "one fewer worn item" it must actively REMOVE its worn copy. A removal
            // that failed to propagate (the user's bug) would leave the peer still
            // wearing it - a permanent eq/hash mismatch. Fabricated re-equips don't
            // persist in the engine, so we deliberately test only this reliable path.
            if (haveOwn_ && haveEq_) {
                unsigned long unequipAt = reequipMode_ ? RE_UNEQUIP_MS : UNEQUIP_MS;
                if (!unequipped_ && ctx.elapsedMs >= unequipAt) {
                    unequipped_ = true;
                    // inv_equip DESTROYS the worn item (down path, ends reduced). inv_reequip
                    // MOVES it to loose (preserving identity) so it can be re-equipped below -
                    // the faithful "drag worn item into the bag, then back onto the body" cycle.
                    int got = reequipMode_
                        ? engine::unequipItemToLoose(ctx.gw, ownHand_, baseSid_, baseType_, 1)
                        : engine::removeEquippedItem(ctx.gw, ownHand_, baseSid_, baseType_, 1);
                    logStep(ctx, "UNEQUIP", got);
                }
                // RE-EQUIP (up path): equip the REAL loose item we just unequipped. Equipping
                // a real (not fabricated) item persists, so the slot fills back in - and the
                // observer, which down-moved its copy to loose when it saw the unequip, must
                // now UP-move it to converge. A broken up path leaves the observer's copy loose.
                if (reequipMode_ && unequipped_ && !reequipped_ && ctx.elapsedMs >= RE_REEQUIP_MS) {
                    reequipped_ = true;
                    int got = engine::reequipLooseItem(ctx.gw, ownHand_, baseSid_, baseType_, 1);
                    logStep(ctx, "REEQUIP", got);
                }
            }
        }

        unsigned long dur;
        if (reequipMode_) dur = ctx.isHost ? RE_HOST_DURATION_MS : RE_JOIN_DURATION_MS;
        else              dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            // In-plugin verdict confirms the scenario EXECUTED. For inv_equip: resolved a
            // geared member, sampled, unequipped. For inv_reequip: additionally re-equipped
            // AND the own worn count returned to its baseline peak (local proof the re-equip
            // of a REAL item PERSISTED - the d25 risk). The runner's per-rank cross-client
            // convergence check is the authoritative gate that the move replicated.
            if (reequipMode_)
                passed_ = haveOwn_ && haveEq_ && (samples_ > 0) && unequipped_ && reequipped_ &&
                          ((int)lastOwnEq_ >= baseEqCount_);
            else
                passed_ = haveOwn_ && haveEq_ && (samples_ > 0) && unequipped_;
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    void logStep(const ScenarioContext& ctx, const char* what, int got) {
        char m[200];
        _snprintf(m, sizeof(m) - 1, "SCENARIO INVE %s r=%u n=%d sid='%s'",
                  what, ownRank_, got, baseSid_[0] ? baseSid_ : "(none)");
        m[sizeof(m) - 1] = '\0'; coop::logLine(m);
        (void)ctx;
    }
    // Same squad-tab -> rank partitioning the Replicator and inv_bidir use, but among the
    // members of `rank`'s tab pick the LOWEST-HAND one that actually WEARS gear (eq >= 1).
    // Both clients share the save, so this resolves to the SAME member on each side - the
    // OWN/PEER comparison stays aligned. Falls back to false if no tab member wears gear.
    static bool resolveGearedRankContainer(GameWorld* gw, unsigned int rank, unsigned int out[5]) {
        for (int i = 0; i < 5; ++i) out[i] = 0;
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, MAX_SQUAD);
        if (n == 0) return false;
        // Index list of this rank's members, sorted ascending by hand (deterministic).
        unsigned int idx[MAX_SQUAD]; unsigned int m = 0;
        for (unsigned int i = 0; i < n; ++i) {
            int cr = containerRankOf(sq, n, i);
            if (cr >= 0 && (unsigned int)cr == rank && m < MAX_SQUAD) idx[m++] = i;
        }
        for (unsigned int a = 1; a < m; ++a)
            for (unsigned int b = a; b > 0 && handLess(sq[idx[b]], sq[idx[b-1]]); --b) {
                unsigned int t = idx[b]; idx[b] = idx[b-1]; idx[b-1] = t;
            }
        for (unsigned int a = 0; a < m; ++a) {
            unsigned int h[5] = { sq[idx[a]].hType, sq[idx[a]].hContainer,
                sq[idx[a]].hContainerSerial, sq[idx[a]].hIndex, sq[idx[a]].hSerial };
            char sid[48]; unsigned int ty = 0; int eq = 0;
            if (engine::findEquippedItemKey(gw, h, sid, sizeof(sid), &ty, &eq) && eq >= 1) {
                for (int k = 0; k < 5; ++k) out[k] = h[k];
                return true;
            }
        }
        return false;
    }
    static bool handLess(const EntityState& a, const EntityState& b) {
        if (a.hType != b.hType) return a.hType < b.hType;
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        if (a.hContainerSerial != b.hContainerSerial) return a.hContainerSerial < b.hContainerSerial;
        if (a.hIndex != b.hIndex) return a.hIndex < b.hIndex;
        return a.hSerial < b.hSerial;
    }
    static bool ctnrLess(const EntityState& a, const EntityState& b) {
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        return a.hContainerSerial < b.hContainerSerial;
    }
    static bool ctnrEq(const EntityState& a, const EntityState& b) {
        return a.hContainer == b.hContainer && a.hContainerSerial == b.hContainerSerial;
    }
    static int containerRankOf(const EntityState* sq, unsigned int n, unsigned int i) {
        EntityState distinct[MAX_SQUAD]; unsigned int dn = 0;
        for (unsigned int a = 0; a < n; ++a) {
            bool seen = false;
            for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[a])) { seen = true; break; }
            if (!seen && dn < MAX_SQUAD) distinct[dn++] = sq[a];
        }
        for (unsigned int a = 1; a < dn; ++a)
            for (unsigned int b = a; b > 0 && ctnrLess(distinct[b], distinct[b-1]); --b) {
                EntityState t = distinct[b]; distinct[b] = distinct[b-1]; distinct[b-1] = t;
            }
        for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[i])) return (int)b;
        return -1;
    }

    static const unsigned long HOST_DURATION_MS = 44000; // outlive the join's window
    static const unsigned long JOIN_DURATION_MS = 28000;
    static const unsigned long UNEQUIP_MS       = 8000;  // baseline, then unequip (leave off)

    // inv_reequip cycle (longer): the author UNEQUIPS then RE-EQUIPS, and both actions
    // must land while the OTHER client is alive+synced so the observer can witness the
    // dip+restore (the join only reaches gameplay ~12-20 s into the host's window, so the
    // cycle runs late; the host outlives the join's later own-cycle for the reverse check).
    static const unsigned long RE_HOST_DURATION_MS = 58000;
    static const unsigned long RE_JOIN_DURATION_MS = 42000;
    static const unsigned long RE_UNEQUIP_MS       = 22000; // after the peer is up
    static const unsigned long RE_REEQUIP_MS       = 32000; // hold the dip, then restore

    static const unsigned int  MAX_SQUAD        = 32;

    bool          passed_;
    bool          haveOwn_;
    bool          haveEq_;
    bool          unequipped_;
    bool          reequipped_;
    bool          reequipMode_;
    unsigned long lastLogMs_;
    unsigned int  samples_;
    unsigned int  ownRank_;
    unsigned int  ownHand_[5];
    int           baseEqCount_;
    char          baseSid_[48];
    unsigned int  baseType_;
    unsigned int  lastOwnEq_;
    bool          rankHave_[2];
    unsigned int  rankHand_[2][5];
};

} // namespace

// Local, single-client DIAGNOSTIC: reproduce the manual weapon-drag failure WITHOUT any
// UI by driving the reconcile (engine::applyContainerContents) directly through the exact
// snapshot sequence the join observed - start [weapon EQ + clothes EQ], then a snapshot
// with the weapon LOOSE only, then restore. dumpInventory after each step + the [recon]
// traces inside applyContainerContents show precisely which primitive loses the weapon.
// No network/invSync needed: the scenario's own applyContainerContents calls are the only
// inventory mutation, so the result is deterministic and reproducible from one instance.
class WeaponSeqScenario : public Scenario {
public:
    WeaponSeqScenario() : passed_(false), have_(false), nbase_(0), wIdx_(-1), step_(0) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
    }
    virtual const char* name() const { return "inv_wpnseq"; }

    virtual void onStart(const ScenarioContext& ctx) {
        have_ = firstGeared(ctx.gw, hand_);
        if (have_) {
            nbase_ = engine::captureContainerContents(ctx.gw, hand_, base_, INV_ITEMS_MAX, 0);
            for (unsigned int i = 0; i < nbase_; ++i)
                if (base_[i].equipped && wIdx_ < 0) wIdx_ = (int)i; // first worn entry = the weapon
        }
        char b[160];
        _snprintf(b, sizeof(b) - 1, "WSEQ start have=%d nbase=%u wIdx=%d wsid='%s' wtype=%u",
                  have_ ? 1 : 0, nbase_, wIdx_,
                  (wIdx_ >= 0) ? base_[wIdx_].stringID : "(none)",
                  (wIdx_ >= 0) ? base_[wIdx_].itemType : 0u);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (have_) { coop::logLine("WSEQ initial-state:"); engine::dumpInventory(ctx.gw, hand_); }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!have_ || wIdx_ < 0) { if (ctx.elapsedMs >= 8000) { passed_ = false; return true; } return false; }
        // S1 @ 9s: weapon LOOSE + clothes EQ (clean unequip-to-bag) -> move-down weapon.
        if (step_ == 0 && ctx.elapsedMs >= 9000) {
            step_ = 1;
            InvItemEntry s[INV_ITEMS_MAX]; unsigned int ns = 0;
            for (unsigned int i = 0; i < nbase_; ++i) {
                s[ns] = base_[i];
                if ((int)i == wIdx_) { s[ns].equipped = 0; s[ns].slot = 0; } // weapon loose
                ++ns;
            }
            coop::logLine("WSEQ apply S1=[weapon LOOSE + clothes EQ]");
            engine::applyContainerContents(ctx.gw, hand_, s, ns);
            coop::logLine("WSEQ after-S1:"); engine::dumpInventory(ctx.gw, hand_);
        }
        // S2 @ 12s: weapon FULLY GONE (cursor-held transient) -> destroys the weapon.
        if (step_ == 1 && ctx.elapsedMs >= 12000) {
            step_ = 2;
            InvItemEntry s[INV_ITEMS_MAX]; unsigned int ns = 0;
            for (unsigned int i = 0; i < nbase_; ++i)
                if ((int)i != wIdx_) s[ns++] = base_[i]; // everything EXCEPT the weapon
            coop::logLine("WSEQ apply S2=[weapon GONE]");
            engine::applyContainerContents(ctx.gw, hand_, ns ? s : 0, ns);
            coop::logLine("WSEQ after-S2:"); engine::dumpInventory(ctx.gw, hand_);
        }
        // S3 @ 15s: restore baseline (weapon EQ again) -> only CREATE-EQ available (fabricate).
        if (step_ == 2 && ctx.elapsedMs >= 15000) {
            step_ = 3;
            coop::logLine("WSEQ apply S3=[restore baseline EQ]");
            engine::applyContainerContents(ctx.gw, hand_, base_, nbase_);
            coop::logLine("WSEQ after-S3 (immediate):"); engine::dumpInventory(ctx.gw, hand_);
        }
        // @ 18s: re-dump - did the fabricated equipped weapon SURVIVE several ticks? (d25)
        if (step_ == 3 && ctx.elapsedMs >= 18000) {
            step_ = 4;
            coop::logLine("WSEQ after-S3 (3s later, persistence check):");
            engine::dumpInventory(ctx.gw, hand_);
        }
        if (ctx.elapsedMs >= 20000) { passed_ = (step_ == 4); return true; }
        return false;
    }
    virtual bool passed() const { return passed_; }

private:
    static const unsigned int MAX_SQUAD = 32;
    static bool firstGeared(GameWorld* gw, unsigned int out[5]) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, MAX_SQUAD);
        for (unsigned int i = 0; i < n; ++i) {
            unsigned int h[5] = { sq[i].hType, sq[i].hContainer, sq[i].hContainerSerial,
                                  sq[i].hIndex, sq[i].hSerial };
            char sid[48]; unsigned int ty = 0; int eq = 0;
            if (engine::findEquippedItemKey(gw, h, sid, sizeof(sid), &ty, &eq) && eq >= 1) {
                for (int k = 0; k < 5; ++k) out[k] = h[k];
                return true;
            }
        }
        return false;
    }
    bool         passed_;
    bool         have_;
    unsigned int hand_[5];
    InvItemEntry base_[INV_ITEMS_MAX];
    unsigned int nbase_;
    int          wIdx_;
    int          step_;
};

// inv_addequip (LOCAL, single-client, DETERMINISTIC): prove the fix for the "picked-up
// weapon auto-equips into the empty slot, flickers, then VANISHES" bug (d25). When the
// reconcile must ADD an EQUIPPED item the container has NO copy of (curEq=0, curLoose=0),
// the old code fell to fabricate-AND-equip, which the engine's equipment validation
// discards within a tick. applyContainerContents now creates the missing item LOOSE (which
// persists) and equips the now-real loose copy on a LATER reconcile pass - exactly how the
// host re-publishes the same worn snapshot every few seconds. This scenario scripts that
// sequence on one client with NO network: remove a worn item (so there is no copy), then
// re-apply the worn baseline repeatedly, and asserts the slot fills back in AND stays
// filled when we STOP re-applying (the persistence proof the old fabricate path failed).
class InventoryAddEquipScenario : public Scenario {
public:
    InventoryAddEquipScenario()
        : passed_(false), have_(false), nbase_(0), eIdx_(-1), baseType_(0), baseWorn_(0),
          step_(0), eqAfterCreate_(-1), eqAfterEquip_(-1), eqPersist_(-1) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        baseSid_[0] = '\0';
    }
    virtual const char* name() const { return "inv_addequip"; }

    virtual void onStart(const ScenarioContext& ctx) {
        have_ = firstGeared(ctx.gw, hand_);
        if (have_) {
            nbase_ = engine::captureContainerContents(ctx.gw, hand_, base_, INV_ITEMS_MAX, 0);
            // Prefer a NON-WEAPON worn item (armour). WEAPONS cannot currently be rebuilt by
            // the engine factory (createItem returns null for them), so the create-then-equip
            // path under test only applies to reconstructable gear; choosing armour isolates
            // the deferred-equip fix from that separate weapon-factory limitation.
            const unsigned int WEAPON_CAT = 2;
            for (unsigned int pass = 0; pass < 2 && eIdx_ < 0; ++pass)
                for (unsigned int i = 0; i < nbase_; ++i)
                    if (base_[i].equipped && (pass == 1 || base_[i].itemType != WEAPON_CAT)) {
                        eIdx_ = (int)i;
                        strncpy(baseSid_, base_[i].stringID, sizeof(baseSid_) - 1);
                        baseType_ = base_[i].itemType;
                        break;
                    }
            if (eIdx_ >= 0) baseWorn_ = wornCount(ctx.gw);
        }
        char b[200];
        _snprintf(b, sizeof(b) - 1,
            "ADDEQ start have=%d nbase=%u eIdx=%d sid='%s' type=%u baseWorn=%d",
            have_ ? 1 : 0, nbase_, eIdx_, baseSid_[0] ? baseSid_ : "(none)", baseType_, baseWorn_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (have_) { coop::logLine("ADDEQ initial:"); engine::dumpInventory(ctx.gw, hand_); }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!have_ || eIdx_ < 0) { if (ctx.elapsedMs >= 6000) { passed_ = false; return true; } return false; }
        // S0 @6s: REMOVE the worn item entirely (apply baseline minus it) -> no copy left.
        if (step_ == 0 && ctx.elapsedMs >= 6000) {
            step_ = 1;
            InvItemEntry s[INV_ITEMS_MAX]; unsigned int ns = 0;
            for (unsigned int i = 0; i < nbase_; ++i) if ((int)i != eIdx_) s[ns++] = base_[i];
            coop::logLine("ADDEQ apply S0=[worn item GONE]");
            engine::applyContainerContents(ctx.gw, hand_, ns ? s : 0, ns);
            coop::logLine("ADDEQ after-S0:"); engine::dumpInventory(ctx.gw, hand_);
            logEq(ctx, "S0-removed", wornCount(ctx.gw));
        }
        // S1 @9s: re-apply baseline (worn DESIRED, no copy present) -> CREATE-LOOSE (deferred);
        //         the worn count may legitimately still be 0 here (that is the whole point).
        if (step_ == 1 && ctx.elapsedMs >= 9000) {
            step_ = 2;
            coop::logLine("ADDEQ apply S1=[restore worn] (expect CREATE-LOOSE, deferred)");
            engine::applyContainerContents(ctx.gw, hand_, base_, nbase_);
            engine::dumpInventory(ctx.gw, hand_);
            eqAfterCreate_ = wornCount(ctx.gw);
            logEq(ctx, "after-create", eqAfterCreate_);
        }
        // S2 @12s: re-apply baseline AGAIN -> MOVE-UP equips the now-real loose copy persistently.
        if (step_ == 2 && ctx.elapsedMs >= 12000) {
            step_ = 3;
            coop::logLine("ADDEQ apply S2=[restore worn] (expect MOVE-UP equip)");
            engine::applyContainerContents(ctx.gw, hand_, base_, nbase_);
            engine::dumpInventory(ctx.gw, hand_);
            eqAfterEquip_ = wornCount(ctx.gw);
            logEq(ctx, "after-equip", eqAfterEquip_);
        }
        // @15s: persistence check WITHOUT re-applying - did the equip SURVIVE several ticks?
        if (step_ == 3 && ctx.elapsedMs >= 15000) {
            step_ = 4;
            coop::logLine("ADDEQ persistence check (no re-apply):");
            engine::dumpInventory(ctx.gw, hand_);
            eqPersist_ = wornCount(ctx.gw);
            logEq(ctx, "persist", eqPersist_);
        }
        if (ctx.elapsedMs >= 18000) {
            // PASS: after the second apply the worn copy returned to its baseline count AND it
            // persisted to the no-reapply check. eqAfterCreate may be 0 (deferred) - allowed.
            passed_ = have_ && (eIdx_ >= 0) && (baseWorn_ >= 1) &&
                      (eqAfterEquip_ >= baseWorn_) && (eqPersist_ >= baseWorn_);
            char b[140];
            _snprintf(b, sizeof(b) - 1,
                "ADDEQ verdict pass=%d baseWorn=%d create=%d equip=%d persist=%d",
                passed_ ? 1 : 0, baseWorn_, eqAfterCreate_, eqAfterEquip_, eqPersist_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return true;
        }
        return false;
    }
    virtual bool passed() const { return passed_; }

private:
    static const unsigned int MAX_SQUAD = 32;
    void logEq(const ScenarioContext& ctx, const char* what, int eq) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "ADDEQ eq-%s=%d (sid='%s')", what, eq, baseSid_[0] ? baseSid_ : "(none)");
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        (void)ctx;
    }
    int wornCount(GameWorld* gw) {
        InvItemEntry it[INV_ITEMS_MAX];
        unsigned int n = engine::captureContainerContents(gw, hand_, it, INV_ITEMS_MAX, 0);
        int c = 0;
        for (unsigned int i = 0; i < n; ++i)
            if (it[i].equipped && it[i].itemType == baseType_ && strcmp(it[i].stringID, baseSid_) == 0) ++c;
        return c;
    }
    static bool firstGeared(GameWorld* gw, unsigned int out[5]) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, MAX_SQUAD);
        for (unsigned int i = 0; i < n; ++i) {
            unsigned int h[5] = { sq[i].hType, sq[i].hContainer, sq[i].hContainerSerial,
                                  sq[i].hIndex, sq[i].hSerial };
            char sid[48]; unsigned int ty = 0; int eq = 0;
            if (engine::findEquippedItemKey(gw, h, sid, sizeof(sid), &ty, &eq) && eq >= 1) {
                for (int k = 0; k < 5; ++k) out[k] = h[k];
                return true;
            }
        }
        return false;
    }
    bool         passed_;
    bool         have_;
    unsigned int hand_[5];
    InvItemEntry base_[INV_ITEMS_MAX];
    unsigned int nbase_;
    int          eIdx_;
    char         baseSid_[48];
    unsigned int baseType_;
    int          baseWorn_;
    int          step_;
    int          eqAfterCreate_;
    int          eqAfterEquip_;
    int          eqPersist_;
};

// wpn_relocate (SPIKE for the conservation model): prove that a WEAPON - which the engine
// factory CANNOT fabricate (createItem returns null) - can still be moved bag -> ground ->
// bag by RELOCATING the REAL object, and that it PERSISTS at each step. This validates the
// "don't create, conserve & move" trade model: both clients already own the weapon (shared
// save), so a drop/pickup is a relocation of each side's real copy, never a create/destroy.
// Single-client + deterministic (no network): unequip the weapon to loose, DROP it (real
// ground item), confirm it survives ticks, then PICK IT UP by re-homing the real object.
class WeaponRelocateScenario : public Scenario {
public:
    WeaponRelocateScenario()
        : passed_(false), have_(false), baseType_(0), step_(0),
          invBase_(0), invAfterDrop_(-1), grndAfterDrop_(-1), grndPersist_(-1),
          invAfterPick_(-1), grndAfterPick_(-1), invPersist_(-1) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        baseSid_[0] = '\0';
    }
    virtual const char* name() const { return "wpn_relocate"; }

    virtual void onStart(const ScenarioContext& ctx) {
        have_ = findWeaponHolder(ctx.gw, hand_, baseSid_, sizeof(baseSid_), &baseType_);
        if (have_) invBase_ = invCount(ctx.gw);
        char b[200];
        _snprintf(b, sizeof(b) - 1, "RELOC start have=%d sid='%s' type=%u invBase=%d",
                  have_ ? 1 : 0, baseSid_[0] ? baseSid_ : "(none)", baseType_, invBase_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (have_) { coop::logLine("RELOC initial:"); engine::dumpInventory(ctx.gw, hand_); }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!have_ || !baseSid_[0]) { if (ctx.elapsedMs >= 6000) { passed_ = false; return true; } return false; }
        // S0 @6s: ensure the weapon is LOOSE (unequip the real object to the bag; preserves
        // identity). dropItemFromInventory drops loose items only.
        if (step_ == 0 && ctx.elapsedMs >= 6000) {
            step_ = 1;
            int un = engine::unequipItemToLoose(ctx.gw, hand_, baseSid_, baseType_, 1);
            logN(ctx, "unequip", un);
        }
        // S1 @9s: DROP the real weapon -> a free ground item (native Inventory::dropItem; no
        // createItem). Expect inv -1 and a free ground weapon to appear.
        if (step_ == 1 && ctx.elapsedMs >= 9000) {
            step_ = 2;
            int dr = engine::dropItemFromInventory(ctx.gw, hand_, baseSid_, baseType_, 1);
            invAfterDrop_  = invCount(ctx.gw);
            grndAfterDrop_ = engine::countFreeGroundItemsNear(ctx.gw, hand_, baseSid_, baseType_, radius());
            char b[120]; _snprintf(b, sizeof(b) - 1, "RELOC after-drop dropped=%d inv=%d ground=%d", dr, invAfterDrop_, grndAfterDrop_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // S1b @12s: the dropped weapon is a REAL persistent object - still on the ground
        // ticks later (a fabricated item would have vanished).
        if (step_ == 2 && ctx.elapsedMs >= 12000) {
            step_ = 3;
            grndPersist_ = engine::countFreeGroundItemsNear(ctx.gw, hand_, baseSid_, baseType_, radius());
            logN(ctx, "ground-persist", grndPersist_);
        }
        // S2 @15s: PICK IT UP by relocating the real ground object into the bag (no create).
        if (step_ == 3 && ctx.elapsedMs >= 15000) {
            step_ = 4;
            int pk = engine::pickupWorldItemIntoInventory(ctx.gw, hand_, baseSid_, baseType_, radius());
            invAfterPick_  = invCount(ctx.gw);
            grndAfterPick_ = engine::countFreeGroundItemsNear(ctx.gw, hand_, baseSid_, baseType_, radius());
            char b[120]; _snprintf(b, sizeof(b) - 1, "RELOC after-pickup picked=%d inv=%d ground=%d", pk, invAfterPick_, grndAfterPick_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            coop::logLine("RELOC after-pickup dump:"); engine::dumpInventory(ctx.gw, hand_);
        }
        // S2b @18s: the re-homed weapon persists in the bag (real object, no fabrication).
        if (step_ == 4 && ctx.elapsedMs >= 18000) {
            step_ = 5;
            invPersist_ = invCount(ctx.gw);
            logN(ctx, "inv-persist", invPersist_);
        }
        if (ctx.elapsedMs >= 21000) {
            bool dropOk    = (invAfterDrop_ >= 0) && (invAfterDrop_ <= invBase_ - 1) && (grndAfterDrop_ >= 1);
            bool dropHeld  = (grndPersist_ >= 1);
            bool pickOk    = (invAfterPick_ >= invBase_) && (grndAfterPick_ < grndAfterDrop_);
            bool pickHeld  = (invPersist_ >= invBase_);
            passed_ = have_ && (invBase_ >= 1) && dropOk && dropHeld && pickOk && pickHeld;
            char b[200];
            _snprintf(b, sizeof(b) - 1,
                "RELOC verdict pass=%d invBase=%d drop(inv=%d grnd=%d held=%d) pick(inv=%d grnd=%d persist=%d)",
                passed_ ? 1 : 0, invBase_, invAfterDrop_, grndAfterDrop_, grndPersist_,
                invAfterPick_, grndAfterPick_, invPersist_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return true;
        }
        return false;
    }
    virtual bool passed() const { return passed_; }

private:
    static const unsigned int MAX_SQUAD = 32;
    static float radius() { return 18.0f; } // ground search radius (drops land at feet)

    void logN(const ScenarioContext& ctx, const char* what, int n) {
        char b[100]; _snprintf(b, sizeof(b) - 1, "RELOC %s=%d sid='%s'", what, n, baseSid_[0] ? baseSid_ : "(none)");
        b[sizeof(b) - 1] = '\0'; coop::logLine(b); (void)ctx;
    }
    // Count copies of (baseSid_,baseType_) in the holder's inventory (loose + equipped).
    int invCount(GameWorld* gw) {
        InvItemEntry it[INV_ITEMS_MAX];
        unsigned int n = engine::captureContainerContents(gw, hand_, it, INV_ITEMS_MAX, 0);
        int c = 0;
        for (unsigned int i = 0; i < n; ++i)
            if (it[i].itemType == baseType_ && strcmp(it[i].stringID, baseSid_) == 0) ++c;
        return c;
    }
    // Scan squad members for one holding a WEAPON (type==2); prefer an equipped weapon.
    static bool findWeaponHolder(GameWorld* gw, unsigned int out[5], char* outSid,
                                 unsigned int outLen, unsigned int* outType) {
        const unsigned int WEAPON_CAT = 2;
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, MAX_SQUAD);
        for (unsigned int pass = 0; pass < 2; ++pass) { // pass0: equipped weapon, pass1: any
            for (unsigned int m = 0; m < n; ++m) {
                unsigned int h[5] = { sq[m].hType, sq[m].hContainer, sq[m].hContainerSerial,
                                      sq[m].hIndex, sq[m].hSerial };
                InvItemEntry it[INV_ITEMS_MAX];
                unsigned int cnt = engine::captureContainerContents(gw, h, it, INV_ITEMS_MAX, 0);
                for (unsigned int i = 0; i < cnt; ++i) {
                    if (it[i].itemType != WEAPON_CAT) continue;
                    if (pass == 0 && !it[i].equipped) continue;
                    for (int k = 0; k < 5; ++k) out[k] = h[k];
                    strncpy(outSid, it[i].stringID, outLen - 1); outSid[outLen - 1] = '\0';
                    if (outType) *outType = it[i].itemType;
                    return true;
                }
            }
        }
        return false;
    }

    bool         passed_;
    bool         have_;
    unsigned int hand_[5];
    char         baseSid_[48];
    unsigned int baseType_;
    int          step_;
    int          invBase_, invAfterDrop_, grndAfterDrop_, grndPersist_;
    int          invAfterPick_, grndAfterPick_, invPersist_;
};

// world_weapon_drop (Phase W2 oracle): CROSS-CLIENT conservation drop. The HOST owns the
// leader (tab 0) and DROPS its weapon (the real action a player performs); the conservation
// channel authors a DROP intent and the JOIN - which does NOT own the leader - must RELOCATE
// its own copy of that weapon to the ground (Inventory::dropItem), NOT destroy it. The join
// asserts the weapon both LEFT its leader's bag AND APPEARED as a free ground weapon (proving
// it crossed over by conservation, not by the inv-reconcile deleting an unreconstructable
// weapon). Roles split on isHost; both load the same save so they target the same leader.
class WorldWeaponDropScenario : public Scenario {
public:
    WorldWeaponDropScenario()
        : passed_(false), have_(false), isHost_(false), baseType_(0), step_(0),
          invBase_(0), invAfter_(-1), grndAfter_(-1), invMin_(999), grndMax_(0) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        baseSid_[0] = '\0';
    }
    virtual const char* name() const { return "world_weapon_drop"; }

    virtual void onStart(const ScenarioContext& ctx) {
        isHost_ = ctx.isHost;
        have_ = findLeaderWeapon(ctx.gw, hand_, baseSid_, sizeof(baseSid_), &baseType_);
        if (have_) {
            invBase_ = invCount(ctx.gw);
            invMin_  = invBase_;
            grndMax_ = engine::countFreeGroundItemsNear(ctx.gw, hand_, baseSid_, baseType_, radius());
        }
        char b[200];
        _snprintf(b, sizeof(b) - 1, "WDROP start role=%s have=%d sid='%s' type=%u invBase=%d",
                  isHost_ ? "host" : "join", have_ ? 1 : 0,
                  baseSid_[0] ? baseSid_ : "(none)", baseType_, invBase_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!have_ || !baseSid_[0]) { if (ctx.elapsedMs >= 6000) { passed_ = false; return true; } return false; }

        if (isHost_) {
            // ACTOR: drop the leader's weapon once (unequip to loose first if worn).
            if (step_ == 0 && ctx.elapsedMs >= 8000) {
                step_ = 1;
                int dr = engine::dropItemFromInventory(ctx.gw, hand_, baseSid_, baseType_, 1);
                if (dr == 0) {
                    int un = engine::unequipItemToLoose(ctx.gw, hand_, baseSid_, baseType_, 1);
                    if (un > 0) dr = engine::dropItemFromInventory(ctx.gw, hand_, baseSid_, baseType_, 1);
                }
                invAfter_  = invCount(ctx.gw);
                grndAfter_ = engine::countFreeGroundItemsNear(ctx.gw, hand_, baseSid_, baseType_, radius());
                char b[160]; _snprintf(b, sizeof(b) - 1,
                    "WDROP host-dropped n=%d inv=%d ground=%d", dr, invAfter_, grndAfter_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        } else {
            // OBSERVER: each tick, track the min bag count + max ground count for the weapon.
            // A successful conservation relocation shows the bag LOSE it AND the ground GAIN it.
            int inv  = invCount(ctx.gw);
            int grnd = engine::countFreeGroundItemsNear(ctx.gw, hand_, baseSid_, baseType_, radius());
            if (inv  < invMin_) invMin_  = inv;
            if (grnd > grndMax_) grndMax_ = grnd;
        }

        if (ctx.elapsedMs >= 22000) {
            if (isHost_) {
                bool dropped = (invAfter_ >= 0) && (invAfter_ <= invBase_ - 1) && (grndAfter_ >= 1);
                passed_ = have_ && (invBase_ >= 1) && dropped;
                char b[200]; _snprintf(b, sizeof(b) - 1,
                    "WDROP verdict role=host pass=%d sid='%s' invBase=%d invAfter=%d grndAfter=%d",
                    passed_ ? 1 : 0, baseSid_, invBase_, invAfter_, grndAfter_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            } else {
                bool leftBag   = (invMin_ <= invBase_ - 1);
                bool onGround  = (grndMax_ >= 1);
                passed_ = have_ && (invBase_ >= 1) && leftBag && onGround;
                char b[220]; _snprintf(b, sizeof(b) - 1,
                    "WDROP verdict role=join pass=%d sid='%s' invBase=%d invMin=%d grndMax=%d relocated=%d",
                    passed_ ? 1 : 0, baseSid_, invBase_, invMin_, grndMax_, (leftBag && onGround) ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            return true;
        }
        return false;
    }
    virtual bool passed() const { return passed_; }

private:
    static const unsigned int MAX_SQUAD = 32;
    static float radius() { return 18.0f; }

    int invCount(GameWorld* gw) {
        InvItemEntry it[INV_ITEMS_MAX];
        unsigned int n = engine::captureContainerContents(gw, hand_, it, INV_ITEMS_MAX, 0);
        int c = 0;
        for (unsigned int i = 0; i < n; ++i)
            if (it[i].itemType == baseType_ && strcmp(it[i].stringID, baseSid_) == 0) ++c;
        return c;
    }
    // The squad LEADER (index 0 = tab 0, host-owned) and its first WEAPON. Deterministic
    // across clients (same save), so host (owner) drops it and join (non-owner) mirrors.
    static bool findLeaderWeapon(GameWorld* gw, unsigned int out[5], char* outSid,
                                 unsigned int outLen, unsigned int* outType) {
        const unsigned int WEAPON_CAT = 2;
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ true, sq, MAX_SQUAD);
        if (n == 0) return false;
        unsigned int h[5] = { sq[0].hType, sq[0].hContainer, sq[0].hContainerSerial,
                              sq[0].hIndex, sq[0].hSerial };
        InvItemEntry it[INV_ITEMS_MAX];
        unsigned int cnt = engine::captureContainerContents(gw, h, it, INV_ITEMS_MAX, 0);
        for (unsigned int pass = 0; pass < 2; ++pass) {
            for (unsigned int i = 0; i < cnt; ++i) {
                if (it[i].itemType != WEAPON_CAT) continue;
                if (pass == 0 && !it[i].equipped) continue; // prefer an equipped weapon
                for (int k = 0; k < 5; ++k) out[k] = h[k];
                strncpy(outSid, it[i].stringID, outLen - 1); outSid[outLen - 1] = '\0';
                if (outType) *outType = it[i].itemType;
                return true;
            }
        }
        return false;
    }

    bool         passed_;
    bool         have_;
    bool         isHost_;
    unsigned int hand_[5];
    char         baseSid_[48];
    unsigned int baseType_;
    int          step_;
    int          invBase_, invAfter_, grndAfter_;
    int          invMin_, grndMax_;
};

// drop_probe (Phase W0, DIAGNOSTIC): characterize what a player DROP produces, with no
// protocol changes. The host seeds a known loose item into its leader's bag, enumerates
// nearby world items (WEAPON/ARMOUR/ITEM/CONTAINER) as a BEFORE baseline, drops the item,
// then re-enumerates as AFTER. The log IS the deliverable: it tells us the dropped
// object's itemType/hand/pos and whether getObjectsWithinSphere enumerates it (the
// `enumerated=` verdict) - the facts the W1 interest-scan + proxy design depends on. The
// join is a passive observer (the drop is host-authored; W0 has no world-item channel).
class DropProbeScenario : public Scenario {
public:
    DropProbeScenario()
        : passed_(false), have_(false), step_(0), seeded_(0), dropped_(0),
          nearBefore_(0), nearAfter_(0), dropType_(0) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        sid_[0] = '\0';
    }

    virtual const char* name() const { return "drop_probe"; }

    virtual void onStart(const ScenarioContext& ctx) {
        have_ = engine::pickInventoryContainer(ctx.gw, hand_);
        char b[160];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO DROP anchor host=%d have=%d hand=%u,%u,%u,%u,%u",
            ctx.isHost ? 1 : 0, have_ ? 1 : 0,
            hand_[0], hand_[1], hand_[2], hand_[3], hand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Only the host drives the probe; the join just lives out its window.
        if (!ctx.isHost) {
            if (ctx.elapsedMs >= JOIN_DURATION_MS) { passed_ = true; return true; }
            return false;
        }
        if (!have_) { if (ctx.elapsedMs >= 6000) { passed_ = false; return true; } return false; }

        // @4s: seed a known loose item and read back its real itemType from the bag.
        if (step_ == 0 && ctx.elapsedMs >= 4000) {
            step_ = 1;
            seeded_ = engine::addTestItemsToContainer(ctx.gw, hand_, 1, sid_, sizeof(sid_));
            InvItemEntry items[INV_ITEMS_MAX];
            unsigned int n = engine::captureContainerContents(ctx.gw, hand_, items, INV_ITEMS_MAX, 0);
            for (unsigned int i = 0; i < n; ++i)
                if (!items[i].equipped && strcmp(items[i].stringID, sid_) == 0) {
                    dropType_ = items[i].itemType; break;
                }
            char b[200];
            _snprintf(b, sizeof(b) - 1, "SCENARIO DROP SEEDED added=%d sid='%s' type=%u",
                      seeded_, sid_[0] ? sid_ : "(none)", dropType_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // @6s: BEFORE enumeration baseline.
        if (step_ == 1 && ctx.elapsedMs >= 6000) {
            step_ = 2;
            coop::logLine("SCENARIO DROP BEFORE-scan:");
            engine::dumpWorldItems(ctx.gw);
            nearBefore_ = engine::countWorldItemsNear(ctx.gw, 30.0f);
            char b[120];
            _snprintf(b, sizeof(b) - 1, "SCENARIO DROP BEFORE near=%d", nearBefore_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // @8s: perform the drop.
        if (step_ == 2 && ctx.elapsedMs >= 8000) {
            step_ = 3;
            dropped_ = engine::dropItemFromInventory(ctx.gw, hand_, sid_, dropType_, 1);
            char b[200];
            _snprintf(b, sizeof(b) - 1, "SCENARIO DROP DROPPED dropped=%d sid='%s' type=%u",
                      dropped_, sid_[0] ? sid_ : "(none)", dropType_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // @10s: AFTER enumeration (let the engine settle the new ground object a couple s).
        if (step_ == 3 && ctx.elapsedMs >= 10000) {
            step_ = 4;
            coop::logLine("SCENARIO DROP AFTER-scan:");
            engine::dumpWorldItems(ctx.gw);
            nearAfter_ = engine::countWorldItemsNear(ctx.gw, 30.0f);
            int enumerated = (nearAfter_ > nearBefore_) ? 1 : 0;
            char b[200];
            _snprintf(b, sizeof(b) - 1,
                "SCENARIO DROP RESULT dropped=%d before=%d after=%d enumerated=%d",
                dropped_, nearBefore_, nearAfter_, enumerated);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (ctx.elapsedMs >= HOST_DURATION_MS) { passed_ = (step_ == 4 && dropped_ > 0); return true; }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long HOST_DURATION_MS = 14000;
    static const unsigned long JOIN_DURATION_MS = 12000;
    bool         passed_;
    bool         have_;
    int          step_;
    int          seeded_;
    int          dropped_;
    int          nearBefore_;
    int          nearAfter_;
    unsigned int dropType_;
    unsigned int hand_[5];
    char         sid_[48];
};

// world_item_sync (Phase W1): host-authored ground-item visual sync. The HOST seeds a
// known item, DROPS it (a real free world item), then later DESPAWNS it. Both clients
// sample the interest sphere every 500 ms via captureWorldItems and log a machine-checkable
// "SCENARIO WI <HOST|JOIN> t=.. n=.. pos=.. hash=.." line (n = ground items seen; pos/hash
// from the first). The host's drop streams a netId-keyed snapshot to the join, which spawns
// a LOCAL proxy ground item (so the join's OWN captureWorldItems then enumerates it). The
// oracle asserts the join's observed item matches the host's pos (within tolerance) + CONTENT
// hash (exactly), then that the host's despawn culls the join's proxy cleanly (n -> 0 on both).
class WorldItemSyncScenario : public Scenario {
public:
    WorldItemSyncScenario()
        : passed_(false), have_(false), step_(0), lastLogMs_(0),
          seeded_(0), dropped_(0), despawned_(0), dropType_(0),
          peakN_(0), lastN_(0) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        sid_[0] = '\0';
    }

    virtual const char* name() const { return "world_item_sync"; }

    virtual void onStart(const ScenarioContext& ctx) {
        have_ = engine::pickInventoryContainer(ctx.gw, hand_);
        char b[160];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO WI anchor host=%d have=%d hand=%u,%u,%u,%u,%u",
            ctx.isHost ? 1 : 0, have_ ? 1 : 0,
            hand_[0], hand_[1], hand_[2], hand_[3], hand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Sample the interest sphere on BOTH clients every 500 ms.
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            engine::WorldItemRaw raw[16];
            unsigned int n = engine::captureWorldItems(ctx.gw, raw, 16, 60.0f);
            lastN_ = n; if (n > peakN_) peakN_ = n;
            float x = (n > 0) ? raw[0].x : 0.0f, y = (n > 0) ? raw[0].y : 0.0f, z = (n > 0) ? raw[0].z : 0.0f;
            unsigned int hash = (n > 0) ? raw[0].hash : 0u;
            char b[200];
            _snprintf(b, sizeof(b) - 1,
                "SCENARIO WI %s t=%lu n=%u pos=%.2f,%.2f,%.2f hash=%u",
                ctx.isHost ? "HOST" : "JOIN", (unsigned long)ctx.elapsedMs, n, x, y, z, hash);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        if (ctx.isHost && have_) {
            // @5s: seed a known item, read its real itemType, then DROP it to the ground.
            if (step_ == 0 && ctx.elapsedMs >= 5000) {
                step_ = 1;
                seeded_ = engine::addTestItemsToContainer(ctx.gw, hand_, 1, sid_, sizeof(sid_));
                InvItemEntry items[INV_ITEMS_MAX];
                unsigned int n = engine::captureContainerContents(ctx.gw, hand_, items, INV_ITEMS_MAX, 0);
                for (unsigned int i = 0; i < n; ++i)
                    if (!items[i].equipped && strcmp(items[i].stringID, sid_) == 0) { dropType_ = items[i].itemType; break; }
                dropped_ = engine::dropItemFromInventory(ctx.gw, hand_, sid_, dropType_, 1);
                char b[200];
                _snprintf(b, sizeof(b) - 1, "SCENARIO WI DROP seeded=%d dropped=%d sid='%s' type=%u",
                          seeded_, dropped_, sid_[0] ? sid_ : "(none)", dropType_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            // @16s: DESPAWN the dropped item so the host culls it and the join removes its proxy.
            if (step_ == 1 && ctx.elapsedMs >= 16000) {
                step_ = 2;
                despawned_ = engine::destroyWorldItemsNear(ctx.gw, 60.0f);
                char b[120];
                _snprintf(b, sizeof(b) - 1, "SCENARIO WI DESPAWN destroyed=%d", despawned_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            if (ctx.isHost) passed_ = have_ && (dropped_ > 0) && (despawned_ > 0);
            else            passed_ = true; // observer; the runner's WI-SYNC oracle is authoritative
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long HOST_DURATION_MS = 26000; // outlive the join's window
    static const unsigned long JOIN_DURATION_MS = 22000;
    bool          passed_;
    bool          have_;
    int           step_;
    unsigned long lastLogMs_;
    int           seeded_;
    int           dropped_;
    int           despawned_;
    unsigned int  dropType_;
    unsigned int  peakN_;
    unsigned int  lastN_;
    unsigned int  hand_[5];
    char          sid_[48];
};

Scenario* makeScenario(const std::string& name) {
    if (name == "leader_move")  return new LeaderMoveScenario();
    if (name == "coop_presence") return new CoopPresenceScenario();
    if (name == "npc_sync")     return new NpcSyncScenario();
    if (name == "craft_order")  return new CraftOrderScenario();
    if (name == "down_order")   return new DownOrderScenario();
    if (name == "death_order")  return new DeathOrderScenario();
    if (name == "combat_probe") return new CombatProbeScenario();
    if (name == "combat_order") return new CombatOrderScenario();
    if (name == "combat_kill")  return new CombatKillScenario();
    if (name == "inv_order")    return new InventorySyncScenario();
    if (name == "inv_bidir")    return new InventoryBidirScenario();
    if (name == "inv_equip")    return new InventoryEquipScenario(/*reequip=*/false);
    if (name == "inv_reequip")  return new InventoryEquipScenario(/*reequip=*/true);
    if (name == "inv_wpnseq")   return new WeaponSeqScenario();
    if (name == "inv_addequip") return new InventoryAddEquipScenario();
    if (name == "drop_probe")   return new DropProbeScenario();
    if (name == "world_item_sync") return new WorldItemSyncScenario();
    if (name == "wpn_relocate") return new WeaponRelocateScenario();
    if (name == "world_weapon_drop") return new WorldWeaponDropScenario();
    return 0;
}

} // namespace coop
