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

} // namespace

Scenario* makeScenario(const std::string& name) {
    if (name == "leader_move") return new LeaderMoveScenario();
    if (name == "npc_sync")    return new NpcSyncScenario();
    if (name == "craft_order") return new CraftOrderScenario();
    if (name == "down_order")  return new DownOrderScenario();
    return 0;
}

} // namespace coop
