#define _CRT_SECURE_NO_WARNINGS 1

#include "Scenario.h"
#include "../CoopLog.h"
#include "../game/Engine.h"

#include <kenshi/GameWorld.h>
#include <kenshi/Character.h>

#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstdarg>

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

// Log one extended "SCENARIO VITALS" line for the body at hand h (readObjectHand
// layout: [type,container,containerSerial,index,serial]). The prefix
// "hand=i,s t=.. blood=.." is byte-compatible with the short form combat_kill
// emits (Test-DamageGuard anchors on it); the medical-model fields the player-
// combat/medic oracles need follow after. fl = per-limb flesh, bd = per-limb
// bandaging, order LA,RA,LL,RL (RobotLimbs::Limb). Protocol-16 fields append
// AFTER dead= (older regexes have no $ anchor, so they keep matching): pfl/pst
// = min flesh / min stun across the FULL anatomy (head/chest/stomach included),
// ls = the 4 LimbStates (255 = unknown). Silent no-op if unresolved.
void logVitalsLine(const unsigned int h[5], unsigned long t) {
    engine::MedicalRead mr;
    if (!engine::readMedicalByHand(h, &mr) || !mr.valid) return;
    float pfl = 1e9f, pst = 1e9f;
    for (unsigned int i = 0; i < mr.nParts && i < 12; ++i) {
        if (!mr.parts[i].used) continue;
        if (mr.parts[i].flesh     >= 0.0f && mr.parts[i].flesh     < pfl) pfl = mr.parts[i].flesh;
        if (mr.parts[i].fleshStun >= 0.0f && mr.parts[i].fleshStun < pst) pst = mr.parts[i].fleshStun;
    }
    if (pfl > 1e8f) pfl = -1.0f;
    if (pst > 1e8f) pst = -1.0f;
    char b[320];
    _snprintf(b, sizeof(b) - 1,
              "SCENARIO VITALS hand=%u,%u t=%lu blood=%.1f bleed=%.2f "
              "fl=%.1f,%.1f,%.1f,%.1f bd=%.1f,%.1f,%.1f,%.1f unc=%d dead=%d "
              "pfl=%.1f pst=%.1f ls=%u,%u,%u,%u",
              h[3], h[4], t, mr.blood, mr.bleedRate,
              mr.limbFlesh[0], mr.limbFlesh[1], mr.limbFlesh[2], mr.limbFlesh[3],
              mr.limbBand[0], mr.limbBand[1], mr.limbBand[2], mr.limbBand[3],
              mr.unconscious ? 1 : 0, mr.dead ? 1 : 0,
              pfl, pst,
              (unsigned)mr.limbState[0], (unsigned)mr.limbState[1],
              (unsigned)mr.limbState[2], (unsigned)mr.limbState[3]);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}

// ---- Squad-tab classification (shared by the player_* / medic scenarios) ----
// Mirrors CoopPresenceScenario's partition EXACTLY (and therefore the
// Replicator's): a member's tab identity is its hand CONTAINER, and a tab's
// rank is its position among the distinct containers sorted ascending. Host
// owns rank 0, join owns rank 1.
bool tabHandLess(const EntityState& a, const EntityState& b) {
    if (a.hType != b.hType) return a.hType < b.hType;
    if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
    if (a.hContainerSerial != b.hContainerSerial) return a.hContainerSerial < b.hContainerSerial;
    if (a.hIndex != b.hIndex) return a.hIndex < b.hIndex;
    return a.hSerial < b.hSerial;
}
bool tabCtnrLess(const EntityState& a, const EntityState& b) {
    if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
    return a.hContainerSerial < b.hContainerSerial;
}
bool tabCtnrEq(const EntityState& a, const EntityState& b) {
    return a.hContainer == b.hContainer && a.hContainerSerial == b.hContainerSerial;
}
// Rank of member i's squad tab among the distinct, sorted containers (-1 unknown).
int tabRankOf(const EntityState* sq, unsigned int n, unsigned int i) {
    const unsigned int MAXT = 32;
    EntityState distinct[MAXT]; unsigned int dn = 0;
    for (unsigned int a = 0; a < n; ++a) {
        bool seen = false;
        for (unsigned int b = 0; b < dn; ++b) if (tabCtnrEq(distinct[b], sq[a])) { seen = true; break; }
        if (!seen && dn < MAXT) distinct[dn++] = sq[a];
    }
    for (unsigned int a = 1; a < dn; ++a)
        for (unsigned int b = a; b > 0 && tabCtnrLess(distinct[b], distinct[b-1]); --b) {
            EntityState t = distinct[b]; distinct[b] = distinct[b-1]; distinct[b-1] = t;
        }
    for (unsigned int b = 0; b < dn; ++b) if (tabCtnrEq(distinct[b], sq[i])) return (int)b;
    return -1;
}
// The lowest-hand member of tab 'rank' ("that tab's leader" - the same stable
// pick coop_presence uses for its mover). Returns the index into sq, or -1.
int tabLeaderIdx(const EntityState* sq, unsigned int n, unsigned int rank) {
    int best = -1;
    for (unsigned int i = 0; i < n; ++i) {
        if (tabRankOf(sq, n, i) != (int)rank) continue;
        if (best < 0 || tabHandLess(sq[i], sq[best])) best = (int)i;
    }
    return best;
}
// Fill h[5] (readObjectHand layout) from a captured EntityState's hand fields.
void handFromEntity(const EntityState& e, unsigned int h[5]) {
    h[0] = e.hType; h[1] = e.hContainer; h[2] = e.hContainerSerial;
    h[3] = e.hIndex; h[4] = e.hSerial;
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

    // PIN the worker at LOCAL GAMEPLAY START (pre-arm), while the baked worker is
    // still parked at the prop and is unambiguously the nearest non-squad NPC -
    // waiting for peer-ready arming (a join-load, ~10-20 s) lets other world NPCs
    // wander past the prop and the worker patrol away (observed: "pin FAILED").
    // Then HOLD it at the fixture every pre-arm tick, exactly like the baseline
    // phase does, so it is still there when the armed clock begins.
    virtual void onGameplay(const ScenarioContext& ctx) {
        if (!ctx.isHost) return;
        pinWorker(ctx);
        if (haveWorker_) engine::holdWorkerAtFixture(ctx.gw, workerHand_);
    }

    virtual void onStart(const ScenarioContext& ctx) {
        // Covers immediate arming (peer already streaming / arm-timeout 0), where
        // no pre-arm tick ever ran.
        if (ctx.isHost) {
            pinWorker(ctx);
            if (!haveWorker_)
                coop::logLine("SCENARIO craft worker pin FAILED (no fixture/worker)");
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

    // Pin once; retried each pre-arm tick until it lands (the world may need a
    // few frames after load before the fixture/worker query resolves).
    void pinWorker(const ScenarioContext& ctx) {
        if (haveWorker_) return;
        haveWorker_ = engine::pickCraftWorker(ctx.gw, workerHand_, &task_);
        if (haveWorker_) {
            char b[128];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO craft worker pinned hand=%u,%u,%u,%u,%u task=%d",
                      workerHand_[3], workerHand_[4], workerHand_[0],
                      workerHand_[1], workerHand_[2], task_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
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

    // PIN the subject at local gameplay start (pre-arm) while it is still where
    // the save baked it, and HOLD it upright until the armed clock begins - the
    // peer-ready arming wait is long enough for it to wander otherwise.
    virtual void onGameplay(const ScenarioContext& ctx) {
        if (!ctx.isHost) return;
        pinSubject(ctx);
        if (haveSubject_) engine::holdSubjectUpright(ctx.gw, subjHand_);
    }

    virtual void onStart(const ScenarioContext& ctx) {
        if (ctx.isHost) {
            pinSubject(ctx); // covers immediate arming (no pre-arm tick ran)
            if (!haveSubject_)
                coop::logLine("SCENARIO down subject pin FAILED (no nearby NPC)");
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

    void pinSubject(const ScenarioContext& ctx) {
        if (haveSubject_) return;
        haveSubject_ = engine::pickDownSubject(ctx.gw, subjHand_);
        if (haveSubject_) {
            char b[128];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO down subject pinned hand=%u,%u,%u,%u,%u",
                      subjHand_[3], subjHand_[4], subjHand_[0],
                      subjHand_[1], subjHand_[2]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

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

    // Same pre-arm pin+hold as down_order (see there for rationale).
    virtual void onGameplay(const ScenarioContext& ctx) {
        if (!ctx.isHost) return;
        pinSubject(ctx);
        if (haveSubject_) engine::holdSubjectUpright(ctx.gw, subjHand_);
    }

    virtual void onStart(const ScenarioContext& ctx) {
        if (ctx.isHost) {
            pinSubject(ctx);
            if (!haveSubject_)
                coop::logLine("SCENARIO death subject pin FAILED (no nearby NPC)");
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

    void pinSubject(const ScenarioContext& ctx) {
        if (haveSubject_) return;
        haveSubject_ = engine::pickDownSubject(ctx.gw, subjHand_);
        if (haveSubject_) {
            char b[128];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO death subject pinned hand=%u,%u,%u,%u,%u",
                      subjHand_[3], subjHand_[4], subjHand_[0],
                      subjHand_[1], subjHand_[2]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

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

    // Pre-arm: pin both duelists while they are still at their baked spawn and
    // hold them peaceful until the armed clock begins (see craft_order rationale).
    virtual void onGameplay(const ScenarioContext& ctx) {
        if (!ctx.isHost) return;
        pinDuelists(ctx);
        if (havePins_) engine::holdDuelistsPeaceful(ctx.gw);
    }

    virtual void onStart(const ScenarioContext& ctx) {
        if (ctx.isHost) {
            pinDuelists(ctx);
            if (!havePins_)
                coop::logLine("SCENARIO duel pin FAILED (need two nearby non-squad NPCs)");
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

    void pinDuelists(const ScenarioContext& ctx) {
        if (havePins_) return;
        havePins_ = engine::pickDuelSubjects(ctx.gw, handA_, handB_);
        if (havePins_) {
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO duel subjects pinned A=%u,%u B=%u,%u",
                      handA_[3], handA_[4], handB_[3], handB_[4]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

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

    // Same pre-arm pin+hold as combat_order.
    virtual void onGameplay(const ScenarioContext& ctx) {
        if (!ctx.isHost) return;
        pinDuelists(ctx);
        if (havePins_) engine::holdDuelistsPeaceful(ctx.gw);
    }

    virtual void onStart(const ScenarioContext& ctx) {
        if (ctx.isHost) {
            pinDuelists(ctx);
            if (!havePins_)
                coop::logLine("SCENARIO duel pin FAILED (need two nearby non-squad NPCs)");
        }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (ctx.isHost && havePins_ && ctx.elapsedMs < ORDER_AT_MS) {
            engine::holdDuelistsPeaceful(ctx.gw);
        }
        if (ctx.isHost && havePins_ && ctx.elapsedMs >= ORDER_AT_MS) {
            if (!combatLogged_) {
                int n = engine::startDuel(ctx.gw);
                // Weaken B on the HOST so the real fight draws decisive blood in the
                // window (the damage_guard oracle needs a host-side blood drop as its
                // ground truth; the join's copy must stay flat - it takes no local
                // damage and blood never crosses the wire).
                bool w = engine::woundSubject(ctx.gw, handB_, 45.0f);
                char b[160];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO COMBAT issued orders=%d loser=B(%u,%u) wounded=%d",
                          n, handB_[3], handB_[4], w ? 1 : 0);
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
            // Damage-guard vitals. HOST: log the PINNED duelists' blood the whole
            // run (it knows the hands), so the series captures the full drop -
            // pre-fight baseline through the wound bias + real melee. JOIN: it
            // doesn't know the pins, so it logs any local copy that is fighting
            // or down (== the duelists, once intent replicates). The oracle
            // asserts the HOST victim's blood DROPPED while the JOIN's driven
            // copy stayed FLAT (guarded cosmetic fight). Both reads come off the
            // rendered body's medical state, not a field we wrote.
            for (unsigned int i = 0; i < n; ++i) {
                bool log;
                if (ctx.isHost) {
                    log = havePins_ &&
                          ((npcs[i].hIndex == handA_[3] && npcs[i].hSerial == handA_[4]) ||
                           (npcs[i].hIndex == handB_[3] && npcs[i].hSerial == handB_[4]));
                } else {
                    log = taskIsCombat(npcs[i].task) ||
                          ((npcs[i].bodyState & 7u) != 0);
                }
                if (!log) continue;
                unsigned int h[5] = { npcs[i].hType, npcs[i].hContainer,
                                      npcs[i].hContainerSerial, npcs[i].hIndex,
                                      npcs[i].hSerial };
                float blood = 0.0f;
                if (engine::readBloodByHand(h, &blood)) {
                    char b[128];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO VITALS hand=%u,%u t=%lu blood=%.1f",
                              npcs[i].hIndex, npcs[i].hSerial,
                              (unsigned long)ctx.elapsedMs, blood);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
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
    static const unsigned long KO_DELAY_MS      = 14000; // fight visibly before the takedown
                                                         // (long enough for real landed hits,
                                                         // which the damage_guard oracle needs)
    static const unsigned long HOST_DURATION_MS = 66000;
    static const unsigned long JOIN_DURATION_MS = 54000;
    static const unsigned int  MAX_LOG          = 40;

    void pinDuelists(const ScenarioContext& ctx) {
        if (havePins_) return;
        havePins_ = engine::pickDuelSubjects(ctx.gw, handA_, handB_);
        if (havePins_) {
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO duel subjects pinned A=%u,%u B=%u,%u",
                      handA_[3], handA_[4], handB_[3], handB_[4]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

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

// split_interest (step 5, dual-interest conformance): the players SPLIT UP and the
// shared world must keep streaming around BOTH of them. The HOST relocates its
// whole owned tab (rank 0) ~260 u away from the bar and holds it there; the JOIN's
// tab (rank 1) stays with the bar NPCs. Under the old single-host-leader interest
// sphere the bar leaves the host's capture radius and its NPCs stop streaming -
// exactly spike 16's degradation. With dual-interest (one sphere per tab leader)
// the host's SECOND sphere - centered on the join's tab-1 member, resolved locally
// from the shared save - keeps the bar streamed. Both sides log the standard NPC
// MEMBER/RECV series; the runner's SPLIT-INTEREST oracle checks that bar-anchored
// NPCs (near the logged bar anchor) still track AFTER the host moved away.
class SplitInterestScenario : public Scenario {
public:
    SplitInterestScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0),
          movedLogged_(false), haveBar_(false), bx_(0), by_(0), bz_(0) {}

    virtual const char* name() const { return "split_interest"; }

    virtual void onStart(const ScenarioContext& ctx) {
        // Bar anchor = the rank-1 tab leader's position (the member that STAYS).
        // Logged by both clients so the oracle can select bar-anchored NPCs.
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        for (unsigned int i = 0; i < n; ++i) {
            if (containerRankOf(sq, n, i) == 1) {
                haveBar_ = true; bx_ = sq[i].x; by_ = sq[i].y; bz_ = sq[i].z;
                break;
            }
        }
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SPLIT bar=%.2f,%.2f,%.2f have=%d",
                  bx_, by_, bz_, haveBar_ ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (!haveBar_)
            coop::logLine("SCENARIO SPLIT needs a 2-tab save (rank-1 member missing)");
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // HOST: from MOVE_AT_MS, hold every rank-0 member at the remote point.
        // Teleport-park (not walk) so the split is deterministic and immediate.
        if (ctx.isHost && haveBar_ && ctx.elapsedMs >= MOVE_AT_MS) {
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            unsigned int moved = 0;
            for (unsigned int i = 0; i < n; ++i) {
                if (containerRankOf(sq, n, i) != 0) continue;
                Character* c = engine::resolve(sq[i]);
                if (!c) continue;
                float rx = bx_ + SPLIT_DIST + (float)moved * 3.0f;
                float d2 = (sq[i].x - rx) * (sq[i].x - rx) + (sq[i].z - bz_) * (sq[i].z - bz_);
                if (d2 > 4.0f) engine::park(c, rx, by_, bz_, 0.0f);
                ++moved;
            }
            if (!movedLogged_ && moved > 0) {
                movedLogged_ = true;
                char b[128];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO SPLIT moved=%u to=%.2f,%.2f,%.2f",
                          moved, bx_ + SPLIT_DIST, by_, bz_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        // Standard NPC series on both sides (captureNpcs is dual-sphere now, so
        // the host's MEMBER set must keep covering the bar after the move).
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
            if (ctx.isHost) passed_ = haveBar_ && movedLogged_;
            else            passed_ = haveBar_ && (recvCount_ >= 1);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
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

    static const unsigned long MOVE_AT_MS       = 8000;  // baseline, then split
    static const unsigned long HOST_DURATION_MS = 60000; // outlive the join's window
    static const unsigned long JOIN_DURATION_MS = 46000;
    static const unsigned int  MAX_SQUAD        = 32;
    static const unsigned int  MAX_LOG          = 40;
    static const float         SPLIT_DIST;               // how far rank-0 relocates

    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          movedLogged_;
    bool          haveBar_;
    float         bx_, by_, bz_;
};
const float SplitInterestScenario::SPLIT_DIST = 260.0f;

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

// world_weapon_drop / world_armor_drop (Phase W2 oracle): CROSS-CLIENT conservation drop.
// The HOST owns the leader (tab 0) and DROPS a piece of its gear (the real action a player
// performs - for the armor variant an EQUIPPED piece, mirroring the 2026-07-07 session's
// "drag equipped pants to ground"); the conservation channel authors a DROP intent and the
// JOIN - which does NOT own the leader - must RELOCATE its own copy of that gear to the
// ground (Inventory::dropItem), NOT destroy it. The join asserts the gear both LEFT its
// leader's bag AND APPEARED as a free ground item (proving it crossed over by conservation,
// not by the inv-reconcile deleting an unreconstructable item). Roles split on isHost; both
// load the same save so they target the same leader. Parameterized on the gear category
// (2 = WEAPON, 3 = ARMOUR); both variants share the WDROP log contract and oracle.
class WorldGearDropScenario : public Scenario {
public:
    WorldGearDropScenario(const char* scenarioName, unsigned int gearCat)
        : passed_(false), have_(false), isHost_(false), baseType_(0), step_(0),
          invBase_(0), invAfter_(-1), grndAfter_(-1), invMin_(999), grndMax_(0),
          scenarioName_(scenarioName), gearCat_(gearCat) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        baseSid_[0] = '\0';
    }
    virtual const char* name() const { return scenarioName_; }

    virtual void onStart(const ScenarioContext& ctx) {
        isHost_ = ctx.isHost;
        have_ = findLeaderGear(ctx.gw, gearCat_, hand_, baseSid_, sizeof(baseSid_), &baseType_);
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
    // The squad LEADER (index 0 = tab 0, host-owned) and its first gear item of the
    // requested category (2 = WEAPON, 3 = ARMOUR), preferring an EQUIPPED piece.
    // Deterministic across clients (same save), so host (owner) drops it and join
    // (non-owner) mirrors.
    static bool findLeaderGear(GameWorld* gw, unsigned int gearCat, unsigned int out[5],
                               char* outSid, unsigned int outLen, unsigned int* outType) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ true, sq, MAX_SQUAD);
        if (n == 0) return false;
        unsigned int h[5] = { sq[0].hType, sq[0].hContainer, sq[0].hContainerSerial,
                              sq[0].hIndex, sq[0].hSerial };
        InvItemEntry it[INV_ITEMS_MAX];
        unsigned int cnt = engine::captureContainerContents(gw, h, it, INV_ITEMS_MAX, 0);
        for (unsigned int pass = 0; pass < 2; ++pass) {
            for (unsigned int i = 0; i < cnt; ++i) {
                if (it[i].itemType != gearCat) continue;
                if (pass == 0 && !it[i].equipped) continue; // prefer an equipped piece
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
    const char*  scenarioName_;
    unsigned int gearCat_;
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

// ===========================================================================
// SpikeScenario - generic investigative harness for the autonomous spike loop.
// The concrete probe is selected by the KENSHICOOP_SPIKE env var ("1".."50").
// Each probe emits "SPIKE <id> ..." evidence lines that run_spike.ps1 collects
// and the per-spike findings doc summarizes. It is DIAGNOSTIC: passed() means
// "the probe executed and produced evidence", not a cross-client sync gate.
//
// All spike-specific code lives HERE so it can be reverted in one place between
// batches (the harness baseline keeps only the dispatcher + the smoke probe).
// Add a probe by extending dispatchStart()/dispatchTick() with a new id branch.
// ===========================================================================
class SpikeScenario : public Scenario {
public:
    SpikeScenario()
        : passed_(false), passedSet_(false), started_(false), smokeDone_(false),
          lastLogMs_(0), durMs_(30000) {
        const char* id = std::getenv("KENSHICOOP_SPIKE");
        id_ = id ? id : "0";
    }
    virtual const char* name() const { return "spike"; }

    virtual void onStart(const ScenarioContext& ctx) {
        started_ = true;
        logSpike("start id=%s host=%d", id_.c_str(), ctx.isHost ? 1 : 0);
        dispatchStart(ctx);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Keep a MEMBER/RECV anchor flowing so the runner can time a screenshot
        // and never stalls on the missing-anchor wait.
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            Character* ld = engine::leader(ctx.gw);
            if (ld) logScenarioLine(ctx.isHost ? "MEMBER" : "RECV", ld);
            dispatchTick(ctx);
        }
        if (ctx.elapsedMs >= durMs_) {
            // Diagnostic: a probe "passes" if it executed without faulting and
            // emitted at least its start line. Concrete probes set passed_ when
            // their evidence is captured; default to true so a pure-enumeration
            // probe still reports a clean RESULT.
            if (!passedSet_) passed_ = true;
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    void logSpike(const char* fmt, ...) {
        char b[480];
        char f[512];
        _snprintf(f, sizeof(f) - 1, "SPIKE %s %s", id_.c_str(), fmt);
        f[sizeof(f) - 1] = '\0';
        va_list ap; va_start(ap, fmt);
        _vsnprintf(b, sizeof(b) - 1, f, ap);
        va_end(ap);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
    }
    void setPass(bool ok) { passed_ = ok; passedSet_ = true; }

    // ---- per-spike dispatch ------------------------------------------------
    void dispatchStart(const ScenarioContext& ctx) {
        // id "0" = smoke probe (baseline): prove the harness runs end to end.
        // Concrete spikes are added as additional id branches per batch.
        (void)ctx;
    }

    void dispatchTick(const ScenarioContext& ctx) {
        // Smoke probe: log the leader hand/pos + a rough nearby-NPC count once,
        // so the baseline build is verifiably exercisable before real probes land.
        if (!smokeDone_) {
            smokeDone_ = true;
            Character* ld = engine::leader(ctx.gw);
            unsigned int h[5]; float x = 0, y = 0, z = 0;
            if (ld && engine::readHand(ld, h) && engine::readPos(ld, &x, &y, &z)) {
                logSpike("smoke leader hand=%u,%u,%u,%u,%u pos=%.1f,%.1f,%.1f",
                         h[0], h[1], h[2], h[3], h[4], x, y, z);
            } else {
                logSpike("smoke leader UNRESOLVED");
            }
            setPass(true);
        }
    }

    std::string   id_;
    bool          passed_;
    bool          passedSet_;
    bool          started_;
    bool          smokeDone_;
    unsigned long lastLogMs_;
    unsigned long durMs_;
};

// player_combat (player-combat validation, phase 1): real combat damage TO
// player characters, both ownership directions, plus the players' own combat
// intent crossing. Save 'sync' (2-tab squad in a bar full of ARMED NPCs).
//
// Design pivots from two characterization runs:
//   * Unarmed ally-vs-ally player duels draw ZERO blood (block/stun sparring) -
//     players cannot be each other's damage vector in these saves.
//   * An ARMED bar NPC very much can (measured: mob took the host leader from
//     75.8 blood to -16 with limb flesh at 2.0 in ~25 s).
// So the ARMED NPC is the striker and the PLAYERS are the victims:
//   Window A: the host orders NPC1 to melee the JOIN's tab-1 leader. The NPC's
//     combat intent streams to the join, whose engine swings the NPC copy at the
//     join's REAL (owned) member - authoritative damage lands on the victim's
//     OWNER (the join), the exact authority rule under test. The join victim
//     fights back automatically, which streams ITS combat intent join->host.
//   Window B: NPC2 melees the HOST's tab-0 leader - the same mechanism with the
//     victim owned by the host (all-local control, join renders both copies).
class PlayerCombatScenario : public Scenario {
public:
    PlayerCombatScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0), lastOrderAMs_(0),
          lastOrderBMs_(0), haveOwn_(false), havePeer_(false), haveNpcA_(false),
          haveNpcB_(false), issuedA_(false), issuedB_(false),
          noFightA_(0), noFightB_(0),
          lastVicBloodA_(-1.0f), lastVicBloodB_(-1.0f) {}

    virtual const char* name() const { return "player_combat"; }

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        const unsigned int ownRank = ctx.isHost ? 0u : 1u;
        if (!haveOwn_ || !havePeer_) latchLeaders(ctx, ownRank);

        // All attack orders are HOST-side (world NPCs are host-owned; the first
        // characterization run showed a join-ordered NPC stops resolving on the
        // host once the join's combat branch detaches it).
        if (ctx.isHost && haveOwn_ && havePeer_) {
            // Window A: a striker -> the JOIN's leader. The order + fight are
            // host-local against the leader's DRIVEN copy; the striker's combat
            // intent streams and the join's engine runs the authoritative fight
            // on its OWNED member.
            if (ctx.elapsedMs >= A_AT_MS && ctx.elapsedMs < B_AT_MS) {
                driveWindow(ctx, "A", peerHand_, npcA_, haveNpcA_, issuedA_,
                            lastOrderAMs_, haveNpcB_ ? npcB_ : 0, noFightA_,
                            lastVicBloodA_);
                // RESERVE window B's striker now, while the pool is still clean:
                // by B time the whole bar is brawling and pickCombatVictim's
                // in-combat filter can find nobody (run 011932's B pick FAILED).
                if (haveNpcA_ && !haveNpcB_)
                    haveNpcB_ = engine::pickCombatVictim(ctx.gw, ownHand_,
                                                         npcA_, npcB_);
            }
            // Window B: a striker -> our OWN leader (host-owned victim,
            // all-local; the join renders both copies).
            if (ctx.elapsedMs >= B_AT_MS)
                driveWindow(ctx, "B", ownHand_, npcB_, haveNpcB_, issuedB_,
                            lastOrderBMs_, haveNpcA_ ? npcA_ : 0, noFightB_,
                            lastVicBloodB_);
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            logSeries(ctx, ownRank);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (issuedA_ && issuedB_ && haveNpcA_ && haveNpcB_)
                                 : (recvCount_ >= 1);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    // Shared (peer-ready-armed) timeline: NPC1 fights the join's leader 12-34s,
    // NPC2 fights the host's leader from 34s. The join outlives B by ~22s.
    static const unsigned long A_AT_MS          = 12000;
    static const unsigned long B_AT_MS          = 34000;
    static const unsigned long HOST_DURATION_MS = 64000;
    static const unsigned long JOIN_DURATION_MS = 58000;
    static const unsigned int  MAX_LOG          = 40;

    // Keep one live striker melee-ing 'vic', re-ordering every 2.5 s and
    // REPLACING the striker if the bar brawl KOs it (run 010631: window A's
    // striker was beaten unconscious by another bar NPC ~13 s in, silently
    // ending the window with 0.6 blood of damage done).
    void driveWindow(const ScenarioContext& ctx, const char* tag,
                     const unsigned int vic[5], unsigned int striker[5],
                     bool& haveStriker, bool& issued, unsigned long& lastMs,
                     const unsigned int* exclude, unsigned int& noFight,
                     float& lastVicBlood) {
        if (issued && ctx.elapsedMs - lastMs < 2500) return;
        lastMs = ctx.elapsedMs;
        // Replace the striker when it is DOWN (brawl KO, run 010631), stuck
        // fighting someone OTHER than our victim (a reserved striker dragged
        // into the brawl before its window - orderAttackByHand deliberately
        // no-ops on a fighting body, so it would never engage), or a DUD:
        //   * upright + free yet ignoring the attack goal for 3 straight order
        //     ticks (run 033318: window B's striker stood idle all window), or
        //   * fighting the right victim but drawing NO blood for 5 straight
        //     ticks (run 034659: an engaged striker landed 1.8 blood in 30 s -
        //     weak/unarmed; the gate needs >= 3).
        // A fresh pick is out-of-combat by pickCombatVictim's filter; a dud is
        // excluded from its own re-pick (the old one keeps brawling - added
        // pressure, no downside). If the pool is empty (whole bar brawling),
        // keep the old one - re-orders are safe no-ops.
        const unsigned int NOFIGHT_LIMIT  = 3;
        const unsigned int NOBLOOD_LIMIT  = 5;
        const float        DROP_PER_TICK  = 0.3f; // "real damage" floor per 2.5 s
        bool vicDropped = false;
        {
            engine::MedicalRead vm;
            if (engine::readMedicalByHand(vic, &vm) && vm.valid) {
                vicDropped = (lastVicBlood >= 0.0f &&
                              (lastVicBlood - vm.blood) >= DROP_PER_TICK);
                lastVicBlood = vm.blood;
            }
        }
        bool replace = !haveStriker;
        const unsigned int* excludeDud = 0;
        if (haveStriker) {
            engine::MedicalRead mr;
            if (!engine::readMedicalByHand(striker, &mr) || !mr.valid ||
                mr.unconscious || mr.dead) {
                replace = true;
            } else {
                Character* s = engine::resolveCharByHand(striker[3], striker[4],
                                                         striker[0], striker[1],
                                                         striker[2]);
                engine::CombatRead cr;
                bool fighting = s && engine::readCombat(s, &cr) && cr.inCombat;
                if (fighting && !(cr.hasTarget && cr.target[3] == vic[3] &&
                                  cr.target[4] == vic[4])) {
                    replace = true;
                } else if (vicDropped) {
                    noFight = 0; // striker is delivering - keep it
                } else if (issued &&
                           ++noFight >= (fighting ? NOBLOOD_LIMIT
                                                  : NOFIGHT_LIMIT)) {
                    replace = true;
                    excludeDud = striker; // don't hand the window back to the dud
                }
            }
        }
        if (replace) {
            unsigned int fresh[5];
            if (engine::pickCombatVictim(ctx.gw, ownHand_, exclude, fresh,
                                         excludeDud)) {
                bool replaced = haveStriker;
                memcpy(striker, fresh, sizeof(fresh));
                haveStriker = true;
                noFight = 0; // fresh striker gets a full engagement window
                if (replaced) {
                    char b[128];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO PCOMBAT %s restrike atk=%u,%u",
                              tag, striker[3], striker[4]);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            } else if (!haveStriker) {
                if (!issued) {
                    char b[96];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO PCOMBAT %s pick FAILED (no upright NPC)", tag);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    issued = true;
                }
                return;
            }
        }
        bool ok = engine::orderAttackByHand(ctx.gw, striker, vic);
        if (!issued) {
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO PCOMBAT %s issued atk=%u,%u vic=%u,%u ok=%d",
                      tag, striker[3], striker[4], vic[3], vic[4], ok ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            issued = true;
        }
    }

    void latchLeaders(const ScenarioContext& ctx, unsigned int ownRank) {
        EntityState sq[MAX_LOG];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_LOG);
        if (!haveOwn_) {
            int idx = tabLeaderIdx(sq, n, ownRank);
            if (idx >= 0) {
                handFromEntity(sq[idx], ownHand_);
                haveOwn_ = true;
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO PCOMBAT own rank=%u hand=%u,%u",
                          ownRank, ownHand_[3], ownHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        if (!havePeer_) {
            int idx = tabLeaderIdx(sq, n, ownRank == 0u ? 1u : 0u);
            if (idx >= 0) {
                handFromEntity(sq[idx], peerHand_);
                havePeer_ = true;
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO PCOMBAT peer rank=%u hand=%u,%u",
                          ownRank == 0u ? 1u : 0u, peerHand_[3], peerHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
    }

    void logSeries(const ScenarioContext& ctx, unsigned int ownRank) {
        // Squad members: MEMBER for our tab, RECV for the peer's, VITALS for all
        // (the players are the victims whose medical truth the oracle compares).
        EntityState sq[MAX_LOG];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_LOG);
        bool sawPeer = false;
        for (unsigned int i = 0; i < n; ++i) {
            int r = tabRankOf(sq, n, i);
            if (r < 0) continue;
            logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
            if ((unsigned int)r != ownRank) sawPeer = true;
            unsigned int h[5]; handFromEntity(sq[i], h);
            logVitalsLine(h, ctx.elapsedMs);
        }
        if (!ctx.isHost && sawPeer) ++recvCount_;
        // World NPCs (the strikers): host MEMBER, join RECV - the join's RECV
        // series for the striker hands is the intent-crossing signal.
        EntityState npcs[MAX_LOG];
        unsigned int nn = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
        for (unsigned int i = 0; i < nn; ++i)
            logScenarioEntity(ctx.isHost ? "MEMBER" : "RECV", npcs[i]);
    }

    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    unsigned long lastOrderAMs_;
    unsigned long lastOrderBMs_;
    bool          haveOwn_;
    bool          havePeer_;
    bool          haveNpcA_;
    bool          haveNpcB_;
    bool          issuedA_;
    bool          issuedB_;
    unsigned int  noFightA_;      // consecutive order ticks without the victim
    unsigned int  noFightB_;      // bleeding (dud detection, runs 033318/034659)
    float         lastVicBloodA_; // victim blood at the previous order tick
    float         lastVicBloodB_; // (-1 = not yet sampled)
    unsigned int  ownHand_[5];
    unsigned int  peerHand_[5];
    unsigned int  npcA_[5];
    unsigned int  npcB_[5];
};

// player_ko (player-combat validation, phase 1): players as VICTIMS, both
// directions. Save 'squad1'. Window A: the HOST knocks out its OWN tab-0 leader
// (scaffold KO, the down_order pattern - deterministic; real combat damage is
// player_combat's job), holds it down, then REVIVES it. The KO and revive edges
// must cross as reliable EVT_KNOCKOUT/EVT_REVIVE and the join's driven copy must
// lie down / stand up. Window B inverts it: the JOIN KOs + revives its OWN tab-1
// member and the host's driven copy must follow. Both sides log squad
// MEMBER/RECV + VITALS series.
class PlayerKoScenario : public Scenario {
public:
    PlayerKoScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0), lastAssertMs_(0),
          haveSubj_(false), downLogged_(false), reviveLogged_(false) {}

    virtual const char* name() const { return "player_ko"; }

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        const unsigned int  ownRank = ctx.isHost ? 0u : 1u;
        const unsigned long downAt  = ctx.isHost ? A_DOWN_AT_MS : B_DOWN_AT_MS;
        const unsigned long revAt   = ctx.isHost ? A_REV_AT_MS : B_REV_AT_MS;
        const char* tag             = ctx.isHost ? "A" : "B";

        if (!haveSubj_) latchSubject(ctx, ownRank);

        if (haveSubj_ && ctx.elapsedMs >= downAt && ctx.elapsedMs < revAt) {
            // KO our OWN member; re-assert on a throttle (tops the forced KO
            // timer) so it stays down for the whole window.
            if (!downLogged_ || ctx.elapsedMs - lastAssertMs_ >= 1500) {
                lastAssertMs_ = ctx.elapsedMs;
                bool ok = engine::orderDownSubject(ctx.gw, subjHand_);
                if (!downLogged_) {
                    char b[128];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO PKO %s down issued hand=%u,%u ok=%d",
                              tag, subjHand_[3], subjHand_[4], ok ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    downLogged_ = true;
                }
            }
        }
        if (haveSubj_ && ctx.elapsedMs >= revAt) {
            // Revive: clear the KO + restore blood so the body stands and the
            // owner publishes the upright edge (EVT_REVIVE). Re-issued once.
            if (!reviveLogged_ || (ctx.elapsedMs - lastAssertMs_ >= 1500 &&
                                   ctx.elapsedMs < revAt + 4000)) {
                lastAssertMs_ = ctx.elapsedMs;
                bool ok = engine::reviveSubject(ctx.gw, subjHand_);
                if (!reviveLogged_) {
                    char b[128];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO PKO %s revive issued hand=%u,%u ok=%d",
                              tag, subjHand_[3], subjHand_[4], ok ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    reviveLogged_ = true;
                }
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
                unsigned int h[5]; handFromEntity(sq[i], h);
                logVitalsLine(h, ctx.elapsedMs);
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (downLogged_ && reviveLogged_)
                                 : (downLogged_ && reviveLogged_ && recvCount_ >= 1);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    // Sequential windows on the peer-ready-armed shared timeline:
    // A (host victim): down 10-24s, revive at 24s. B (join victim): down 34-48s,
    // revive at 48s. The host outlives the join's whole window.
    static const unsigned long A_DOWN_AT_MS    = 10000;
    static const unsigned long A_REV_AT_MS     = 24000;
    static const unsigned long B_DOWN_AT_MS    = 34000;
    static const unsigned long B_REV_AT_MS     = 48000;
    static const unsigned long HOST_DURATION_MS = 70000;
    static const unsigned long JOIN_DURATION_MS = 60000;
    static const unsigned int  MAX_SQUAD        = 32;

    void latchSubject(const ScenarioContext& ctx, unsigned int ownRank) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int idx = tabLeaderIdx(sq, n, ownRank);
        if (idx < 0) return;
        handFromEntity(sq[idx], subjHand_);
        haveSubj_ = true;
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO PKO subject rank=%u hand=%u,%u",
                  ownRank, subjHand_[3], subjHand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    unsigned long lastAssertMs_;
    bool          haveSubj_;
    bool          downLogged_;
    bool          reviveLogged_;
    unsigned int  subjHand_[5];
};

// medic_order (medical replication validation): players HEAL each other, both
// directions. Save 'squad1'. Window A: the HOST wounds its OWN tab-0 leader
// (limb flesh + blood - the owner's authoritative medical truth), then the JOIN
// "bandages" its DRIVEN copy of that member (healSubjectBandage = the medical
// fields a real first-aid pass raises). Window B inverts the roles. The oracle
// compares the wounded member's VITALS series on both sides: the wound must
// reach the healer's copy, the treatment must reach the owner's body, and the
// two series must converge. (Phase-1 truth per spikes 21-23: NEITHER crosses -
// the healer's copy is pristine (nothing to bandage, n=0) and the owner never
// sees the treatment. The vitals-sync + treatment-forwarding features are what
// turn this green.)
class MedicOrderScenario : public Scenario {
public:
    MedicOrderScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0), lastHealMs_(0),
          haveOwn_(false), havePeer_(false), woundLogged_(false),
          healLogged_(false) {}

    virtual const char* name() const { return "medic_order"; }

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        const unsigned int  ownRank  = ctx.isHost ? 0u : 1u;
        const unsigned int  peerRank = ctx.isHost ? 1u : 0u;
        const unsigned long woundAt  = ctx.isHost ? A_WOUND_AT_MS : B_WOUND_AT_MS;
        const unsigned long healAt   = ctx.isHost ? B_HEAL_AT_MS : A_HEAL_AT_MS;
        const char* wTag             = ctx.isHost ? "A" : "B";
        const char* hTag             = ctx.isHost ? "B" : "A";

        if (!haveOwn_ || !havePeer_) latchSubjects(ctx, ownRank, peerRank);

        // WOUND our own member (we are its owner - authoritative medical truth).
        if (haveOwn_ && !woundLogged_ && ctx.elapsedMs >= woundAt) {
            bool ok = engine::woundSubjectLimbs(ctx.gw, ownHand_, 40.0f, 70.0f);
            char b[128];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO MEDIC %s wound issued hand=%u,%u ok=%d",
                      wTag, ownHand_[3], ownHand_[4], ok ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            woundLogged_ = true;
        }

        // HEAL the PEER's wounded member = bandage the DRIVEN copy we render.
        // Re-applied ~1 Hz for a few seconds (a real first-aid pass is also
        // incremental, and the phase-2 treatment detector samples).
        if (havePeer_ && ctx.elapsedMs >= healAt && ctx.elapsedMs < healUntil(healAt)) {
            if (ctx.elapsedMs - lastHealMs_ >= 1000) {
                lastHealMs_ = ctx.elapsedMs;
                int nb = engine::healSubjectBandage(ctx.gw, peerHand_);
                if (!healLogged_) {
                    char b[128];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO MEDIC %s heal issued hand=%u,%u n=%d",
                              hTag, peerHand_[3], peerHand_[4], nb);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    healLogged_ = true;
                }
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
                unsigned int h[5]; handFromEntity(sq[i], h);
                logVitalsLine(h, ctx.elapsedMs);
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (woundLogged_ && healLogged_)
                                 : (woundLogged_ && healLogged_ && recvCount_ >= 1);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    // Shared timeline: host wounds A at 10s, join heals A 20-27s; join wounds B
    // at 32s, host heals B 42-49s.
    static const unsigned long A_WOUND_AT_MS   = 10000;
    static const unsigned long A_HEAL_AT_MS    = 20000;
    static const unsigned long B_WOUND_AT_MS   = 32000;
    static const unsigned long B_HEAL_AT_MS    = 42000;
    static const unsigned long HEAL_WINDOW_MS  = 7000;
    static const unsigned long HOST_DURATION_MS = 62000;
    static const unsigned long JOIN_DURATION_MS = 56000;
    static const unsigned int  MAX_SQUAD        = 32;

    static unsigned long healUntil(unsigned long healAt) { return healAt + HEAL_WINDOW_MS; }

    void latchSubjects(const ScenarioContext& ctx, unsigned int ownRank,
                       unsigned int peerRank) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        if (!haveOwn_) {
            int idx = tabLeaderIdx(sq, n, ownRank);
            if (idx >= 0) {
                handFromEntity(sq[idx], ownHand_);
                haveOwn_ = true;
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO MEDIC own rank=%u hand=%u,%u",
                          ownRank, ownHand_[3], ownHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        if (!havePeer_) {
            int idx = tabLeaderIdx(sq, n, peerRank);
            if (idx >= 0) {
                handFromEntity(sq[idx], peerHand_);
                havePeer_ = true;
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO MEDIC peer rank=%u hand=%u,%u",
                          peerRank, peerHand_[3], peerHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
    }

    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    unsigned long lastHealMs_;
    bool          haveOwn_;
    bool          havePeer_;
    bool          woundLogged_;
    bool          healLogged_;
    unsigned int  ownHand_[5];
    unsigned int  peerHand_[5];
};

// limb_loss (protocol-16 limb replication validation): each client runs the
// engine's own amputate on ITS OWN tab leader (window A = host member LEFT_ARM
// at 12s, window B = join member RIGHT_ARM at 30s) - authoritative damage with
// the severed ground item, exactly what a real severing hit does. Both sides
// log VITALS (with the ls= LimbStates) for every squad member at 2 Hz plus a
// "SCENARIO LIMBITEMS" severed-ground-item count near each subject. The
// Test-LimbLoss oracle gates: the COPY side reaches the STUMP state within a
// latency budget (event or self-heal), and both sides converge on exactly one
// severed ground item per amputation (host-authoritative world-item channel;
// the join's local duplicate must be deduped).
class LimbLossScenario : public Scenario {
public:
    LimbLossScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0),
          haveOwn_(false), havePeer_(false), cutLogged_(false) {}

    virtual const char* name() const { return "limb_loss"; }

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        const unsigned int  ownRank = ctx.isHost ? 0u : 1u;
        const unsigned int  peerRank = ctx.isHost ? 1u : 0u;
        const unsigned long cutAt   = ctx.isHost ? A_CUT_AT_MS : B_CUT_AT_MS;
        const int           limb    = ctx.isHost ? 0 : 1; // A: LEFT_ARM, B: RIGHT_ARM
        const char*         wTag    = ctx.isHost ? "A" : "B";

        if (!haveOwn_ || !havePeer_) latchSubjects(ctx, ownRank, peerRank);

        // Amputate OUR OWN member (we are its owner - authoritative damage).
        if (haveOwn_ && !cutLogged_ && ctx.elapsedMs >= cutAt) {
            bool ok = engine::amputateSubjectLimb(ctx.gw, ownHand_, limb);
            char b[128];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO LIMB %s amputate issued hand=%u,%u limb=%d ok=%d",
                      wTag, ownHand_[3], ownHand_[4], limb, ok ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            cutLogged_ = true;
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
                unsigned int h[5]; handFromEntity(sq[i], h);
                logVitalsLine(h, ctx.elapsedMs);
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
            // Severed ground items near each subject (both sides count their
            // LOCAL world - convergence on exactly 1 per cut is the gate).
            if (haveOwn_) logLimbItems(ctx, ownHand_);
            if (havePeer_) logLimbItems(ctx, peerHand_);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? cutLogged_ : (cutLogged_ && recvCount_ >= 1);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long A_CUT_AT_MS      = 12000;
    static const unsigned long B_CUT_AT_MS      = 30000;
    static const unsigned long HOST_DURATION_MS = 52000;
    static const unsigned long JOIN_DURATION_MS = 46000;
    static const unsigned int  MAX_SQUAD        = 32;

    void logLimbItems(const ScenarioContext& ctx, const unsigned int h[5]) {
        int n = engine::countSeveredLimbsNear(ctx.gw, h, 20.0f);
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO LIMBITEMS hand=%u,%u t=%lu n=%d",
                  h[3], h[4], ctx.elapsedMs, n);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchSubjects(const ScenarioContext& ctx, unsigned int ownRank,
                       unsigned int peerRank) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        if (!haveOwn_) {
            int idx = tabLeaderIdx(sq, n, ownRank);
            if (idx >= 0) {
                handFromEntity(sq[idx], ownHand_);
                haveOwn_ = true;
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO LIMB own rank=%u hand=%u,%u",
                          ownRank, ownHand_[3], ownHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        if (!havePeer_) {
            int idx = tabLeaderIdx(sq, n, peerRank);
            if (idx >= 0) {
                handFromEntity(sq[idx], peerHand_);
                havePeer_ = true;
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO LIMB peer rank=%u hand=%u,%u",
                          peerRank, peerHand_[3], peerHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
    }

    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveOwn_;
    bool          havePeer_;
    bool          cutLogged_;
    unsigned int  ownHand_[5];
    unsigned int  peerHand_[5];
};

// stats_sync (protocol-17 character-stats replication validation): each client
// raises stats on ITS OWN tab leader via the raise-only scaffold (window A =
// host raises Strength+Stealth at 12s, window B = join raises Dexterity+
// Athletics at 30s) - the owner-side write the stats channel must carry to the
// peer's driven copy. Both sides log "SCENARIO STATS" series for BOTH leaders
// at 2 Hz; the Test-StatsSync oracle gates: each raised value crosses to the
// peer copy within a latency budget, stays sticky, and stats neither side
// touched do not drift.
class StatsSyncScenario : public Scenario {
public:
    StatsSyncScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0),
          haveOwn_(false), havePeer_(false), raiseLogged_(false) {}

    virtual const char* name() const { return "stats_sync"; }

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        const unsigned int  ownRank  = ctx.isHost ? 0u : 1u;
        const unsigned int  peerRank = ctx.isHost ? 1u : 0u;
        const unsigned long raiseAt  = ctx.isHost ? A_RAISE_AT_MS : B_RAISE_AT_MS;
        // StatsEnumerated: host raises STRENGTH(1)+STEALTH(16); join raises
        // DEXTERITY(18)+ATHLETICS(17). Disjoint so each direction is provable.
        const int   stat1 = ctx.isHost ? 1  : 18;
        const int   stat2 = ctx.isHost ? 16 : 17;
        const char* wTag  = ctx.isHost ? "A" : "B";

        if (!haveOwn_ || !havePeer_) latchSubjects(ctx, ownRank, peerRank);

        // Raise target: well above any squad1 save-load stat so the crossing
        // is an unambiguous step in the series (raise-only scaffold).
        const float RAISE_TO = 60.0f;

        // Raise OUR OWN leader's stats (we are its owner - authoritative write).
        if (haveOwn_ && !raiseLogged_ && ctx.elapsedMs >= raiseAt) {
            bool ok1 = engine::raiseSubjectStat(ctx.gw, ownHand_, stat1, RAISE_TO);
            bool ok2 = engine::raiseSubjectStat(ctx.gw, ownHand_, stat2, RAISE_TO);
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO STATRAISE %s hand=%u,%u stat%d=%.0f ok=%d stat%d=%.0f ok=%d",
                      wTag, ownHand_[3], ownHand_[4], stat1, RAISE_TO, ok1 ? 1 : 0,
                      stat2, RAISE_TO, ok2 ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            raiseLogged_ = true;
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
                unsigned int h[5]; handFromEntity(sq[i], h);
                logStatsLine(h, ctx.elapsedMs);
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? raiseLogged_ : (raiseLogged_ && recvCount_ >= 1);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long A_RAISE_AT_MS    = 12000;
    static const unsigned long B_RAISE_AT_MS    = 30000;
    static const unsigned long HOST_DURATION_MS = 52000;
    static const unsigned long JOIN_DURATION_MS = 46000;
    static const unsigned int  MAX_SQUAD        = 32;

    // One "SCENARIO STATS" line: the watched stats (STRENGTH/STEALTH raised by
    // A, DEXTERITY/ATHLETICS by B, TOUGHNESS as the untouched-drift control)
    // plus xp, read from the LOCAL CharStats of the body at h.
    void logStatsLine(const unsigned int h[5], unsigned long t) {
        engine::StatsRead sr;
        if (!engine::readStatsByHand(h, &sr) || !sr.valid) return;
        char b[200];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO STATS hand=%u,%u t=%lu str=%.2f stealth=%.2f dex=%.2f athl=%.2f tough=%.2f xp=%.0f",
                  h[3], h[4], t, sr.stats[1], sr.stats[16], sr.stats[18],
                  sr.stats[17], sr.stats[21], sr.xp);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchSubjects(const ScenarioContext& ctx, unsigned int ownRank,
                       unsigned int peerRank) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        if (!haveOwn_) {
            int idx = tabLeaderIdx(sq, n, ownRank);
            if (idx >= 0) {
                handFromEntity(sq[idx], ownHand_);
                haveOwn_ = true;
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO STATS own rank=%u hand=%u,%u",
                          ownRank, ownHand_[3], ownHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        if (!havePeer_) {
            int idx = tabLeaderIdx(sq, n, peerRank);
            if (idx >= 0) {
                handFromEntity(sq[idx], peerHand_);
                havePeer_ = true;
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO STATS peer rank=%u hand=%u,%u",
                          peerRank, peerHand_[3], peerHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
    }

    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveOwn_;
    bool          havePeer_;
    bool          raiseLogged_;
    unsigned int  ownHand_[5];
    unsigned int  peerHand_[5];
};

// carry_order (protocol-18 carried-body replication validation, save squad1:
// host tab = leader L0 + second member M2, join tab = leader L1 only). Three
// windows, covering own-tab and BOTH cross-tab directions:
//   A (own-tab, host authors):   host downs M2, L0 picks it up, walks a short
//                                leg, ragdoll-drops it. M2 stays KO'd.
//   B (cross-tab, join carrier): join's L1 picks up the still-down HOST-owned
//                                M2 (carrier owner != carried owner), walks,
//                                drops.
//   C (cross-tab, host carrier): join downs its own L1; host's L0 picks it
//                                up (the other ownership direction), walks,
//                                drops; join revives L1 at the end.
// Both sides log "SCENARIO CARRY" for all three bodies at 2 Hz (isCarrying +
// carried hand, isBeingCarried, position, bodyState). The Test-CarryOrder
// oracle gates per window: the pickup CROSSES (the peer's local pair enters
// the carried state within a latency budget), the carried copy TRACKS its
// carrier while carried (median carrier-carried distance small - the dragged/
// teleported-body artifact shows up as huge gaps), and the drop crosses (the
// peer's copy leaves the carried state within budget).
class CarryOrderScenario : public Scenario {
public:
    CarryOrderScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0),
          haveL0_(false), haveM2_(false), haveL1_(false),
          aDown_(false), aPick_(false), aWalk_(false), aDrop_(false),
          bPick_(false), bWalk_(false), bDrop_(false),
          cDown_(false), cPick_(false), cWalk_(false), cDrop_(false),
          cRevive_(false), lastHoldMs_(0) {}

    virtual const char* name() const { return "carry_order"; }

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveL0_ || !haveM2_ || !haveL1_) latchSubjects(ctx);

        // ---- Window A (host, own-tab): down M2, L0 carries it ----------------
        if (ctx.isHost && haveM2_ && !aDown_ && ctx.elapsedMs >= A_DOWN_AT_MS) {
            bool ok = engine::orderDownSubject(ctx.gw, m2Hand_);
            logAct("A down", m2Hand_, ok); aDown_ = true;
        }
        if (ctx.isHost && haveL0_ && haveM2_ && !aPick_ && ctx.elapsedMs >= A_PICK_AT_MS) {
            bool ok = engine::carrySubject(ctx.gw, l0Hand_, m2Hand_);
            logAct("A pickup", m2Hand_, ok); aPick_ = true;
        }
        if (ctx.isHost && haveL0_ && !aWalk_ && ctx.elapsedMs >= A_WALK_AT_MS) {
            aWalk_ = walkLeg(l0Hand_, "A");
        }
        if (ctx.isHost && haveL0_ && !aDrop_ && ctx.elapsedMs >= A_DROP_AT_MS) {
            bool ok = engine::dropSubject(ctx.gw, l0Hand_, /*ragdoll*/true);
            logAct("A drop", l0Hand_, ok); aDrop_ = true;
        }

        // Keep the KO'd subjects DOWN through their carry legs: the scaffold KO
        // (knockoutForceTimer 8s) expires mid-carry and the engine truthfully
        // stands the body back up, ending the carry at the drop (run 191905:
        // M2 revived as it was dropped, bs 2->0, so the oracle's still-down
        // gate read bs=0). Each subject's OWNER re-tops the timer every 2s
        // (holdDown = timer-only, no fresh knockout on the shoulder) while its
        // windows need the body down - owner-side, so the authoritative
        // medical stream carries the KO state to the peer.
        if (ctx.elapsedMs - lastHoldMs_ >= 2000) {
            lastHoldMs_ = ctx.elapsedMs;
            if (ctx.isHost && haveM2_ && aDown_ && ctx.elapsedMs < HOLD_M2_UNTIL_MS)
                holdSubject(m2Hand_);
            if (!ctx.isHost && haveL1_ && cDown_ && !cRevive_)
                holdSubject(l1Hand_);
        }

        // ---- Window B (join carrier, cross-tab): L1 carries the host's M2 ----
        if (!ctx.isHost && haveL1_ && haveM2_ && !bPick_ && ctx.elapsedMs >= B_PICK_AT_MS) {
            bool ok = engine::carrySubject(ctx.gw, l1Hand_, m2Hand_);
            logAct("B pickup", m2Hand_, ok); bPick_ = true;
        }
        if (!ctx.isHost && haveL1_ && !bWalk_ && ctx.elapsedMs >= B_WALK_AT_MS) {
            bWalk_ = walkLeg(l1Hand_, "B");
        }
        if (!ctx.isHost && haveL1_ && !bDrop_ && ctx.elapsedMs >= B_DROP_AT_MS) {
            bool ok = engine::dropSubject(ctx.gw, l1Hand_, /*ragdoll*/true);
            logAct("B drop", l1Hand_, ok); bDrop_ = true;
        }

        // ---- Window C (host carrier, cross-tab): L0 carries the join's L1 ----
        if (!ctx.isHost && haveL1_ && !cDown_ && ctx.elapsedMs >= C_DOWN_AT_MS) {
            bool ok = engine::orderDownSubject(ctx.gw, l1Hand_);
            logAct("C down", l1Hand_, ok); cDown_ = true;
        }
        if (ctx.isHost && haveL0_ && haveL1_ && !cPick_ && ctx.elapsedMs >= C_PICK_AT_MS) {
            bool ok = engine::carrySubject(ctx.gw, l0Hand_, l1Hand_);
            logAct("C pickup", l1Hand_, ok); cPick_ = true;
        }
        if (ctx.isHost && haveL0_ && !cWalk_ && ctx.elapsedMs >= C_WALK_AT_MS) {
            cWalk_ = walkLeg(l0Hand_, "C");
        }
        if (ctx.isHost && haveL0_ && !cDrop_ && ctx.elapsedMs >= C_DROP_AT_MS) {
            bool ok = engine::dropSubject(ctx.gw, l0Hand_, /*ragdoll*/true);
            logAct("C drop", l0Hand_, ok); cDrop_ = true;
        }
        if (!ctx.isHost && haveL1_ && !cRevive_ && ctx.elapsedMs >= C_REVIVE_AT_MS) {
            bool ok = engine::reviveSubject(ctx.gw, l1Hand_);
            logAct("C revive", l1Hand_, ok); cRevive_ = true;
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            const unsigned int ownRank = ctx.isHost ? 0u : 1u;
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
            if (haveL0_) logCarryLine(l0Hand_, ctx.elapsedMs);
            if (haveM2_) logCarryLine(m2Hand_, ctx.elapsedMs);
            if (haveL1_) logCarryLine(l1Hand_, ctx.elapsedMs);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (aDown_ && aPick_ && aDrop_ && cPick_ && cDrop_)
                                 : (bPick_ && bDrop_ && cDown_ && recvCount_ >= 1);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    // Shared timeline (peer-armed clock on both sides). Each carry leg is
    // pickup +2s walk +8s drop, with 6s of settle between windows.
    static const unsigned long A_DOWN_AT_MS    = 8000;
    static const unsigned long A_PICK_AT_MS    = 12000;
    static const unsigned long A_WALK_AT_MS    = 14000;
    static const unsigned long A_DROP_AT_MS    = 22000;
    static const unsigned long B_PICK_AT_MS    = 30000;
    static const unsigned long B_WALK_AT_MS    = 32000;
    static const unsigned long B_DROP_AT_MS    = 40000;
    static const unsigned long C_DOWN_AT_MS    = 46000;
    static const unsigned long C_PICK_AT_MS    = 50000;
    static const unsigned long C_WALK_AT_MS    = 52000;
    static const unsigned long C_DROP_AT_MS    = 60000;
    static const unsigned long C_REVIVE_AT_MS  = 64000;
    // Keep M2 KO'd through both its carry legs (A + B) plus post-drop settle;
    // after this it may wake naturally (nothing gates on it).
    static const unsigned long HOLD_M2_UNTIL_MS = 44000;
    static const unsigned long HOST_DURATION_MS = 72000;
    static const unsigned long JOIN_DURATION_MS = 68000;
    static const unsigned int  MAX_SQUAD        = 32;

    void logAct(const char* what, const unsigned int h[5], bool ok) {
        char b[144];
        _snprintf(b, sizeof(b) - 1, "SCENARIO CARRYACT %s hand=%u,%u ok=%d",
                  what, h[3], h[4], ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // Re-top the KO timer on our own KO'd subject (timer-only; never a fresh
    // knockout while the body rides a shoulder).
    void holdSubject(const unsigned int h[5]) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (c) engine::holdDown(c);
    }

    // Order the carrier to walk a short leg (+10u east of where it stands).
    // The carried body must FOLLOW via its local shoulder attach on both sides.
    bool walkLeg(const unsigned int h[5], const char* wTag) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (!c) return false;
        float x, y, z;
        if (!engine::readPos(c, &x, &y, &z)) return false;
        bool ok = engine::orderMoveTo(c, x + 10.0f, y, z);
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO CARRYACT %s walk hand=%u,%u ok=%d",
                  wTag, h[3], h[4], ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return true;
    }

    // One "SCENARIO CARRY" line: this body's LOCAL carry relationship + pose.
    void logCarryLine(const unsigned int h[5], unsigned long t) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (!c) return;
        engine::CarryRead cr;
        if (!engine::readCarry(c, &cr) || !cr.valid) return;
        float x = 0, y = 0, z = 0;
        engine::readPos(c, &x, &y, &z);
        unsigned short bs = engine::readBodyState(c);
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO CARRY hand=%u,%u t=%lu carrying=%d carried=%u,%u "
                  "beingCarried=%d pos=%.2f,%.2f,%.2f bs=%u",
                  h[3], h[4], t, cr.carrying ? 1 : 0,
                  cr.carried[3], cr.carried[4], cr.beingCarried ? 1 : 0,
                  x, y, z, (unsigned)bs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchSubjects(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        if (!haveL0_) {
            int idx = tabLeaderIdx(sq, n, 0);
            if (idx >= 0) {
                handFromEntity(sq[idx], l0Hand_);
                haveL0_ = true;
                logSubject("L0", l0Hand_);
            }
        }
        if (haveL0_ && !haveM2_) {
            // Host tab's SECOND member: the lowest hand of rank 0 that is not L0.
            int best = -1;
            for (unsigned int i = 0; i < n; ++i) {
                if (tabRankOf(sq, n, i) != 0) continue;
                unsigned int h[5]; handFromEntity(sq[i], h);
                if (h[3] == l0Hand_[3] && h[4] == l0Hand_[4]) continue;
                if (best < 0 || tabHandLess(sq[i], sq[best])) best = (int)i;
            }
            if (best >= 0) {
                handFromEntity(sq[best], m2Hand_);
                haveM2_ = true;
                logSubject("M2", m2Hand_);
            }
        }
        if (!haveL1_) {
            int idx = tabLeaderIdx(sq, n, 1);
            if (idx >= 0) {
                handFromEntity(sq[idx], l1Hand_);
                haveL1_ = true;
                logSubject("L1", l1Hand_);
            }
        }
    }

    void logSubject(const char* who, const unsigned int h[5]) {
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO CARRY %s hand=%u,%u", who, h[3], h[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveL0_, haveM2_, haveL1_;
    bool          aDown_, aPick_, aWalk_, aDrop_;
    bool          bPick_, bWalk_, bDrop_;
    bool          cDown_, cPick_, cWalk_, cDrop_, cRevive_;
    unsigned long lastHoldMs_;
    unsigned int  l0Hand_[5];
    unsigned int  m2Hand_[5];
    unsigned int  l1Hand_[5];
};

// npc_carry (protocol 18, world-NPC carrier extension): the 2026-07-07 remote
// session's third gap - a HOST-side world NPC hauling a downed player character
// never replicated to the join. One window: the host KO's its second member
// (M2), then DIRECTS the nearest world NPC to pick it up, walk a leg, and drop
// it (carrySubject/dropSubject run the same engine calls the NPC AI does). The
// NPC is host-owned world state, so the edge events now ride publishOwned's NPC
// extension and the join's replicator must execute the pickup on its LOCAL
// NPC copy. The join is a passive observer: it never knows a priori which NPC
// the host chose - it DETECTS the carrier from its own local world (the NPC
// copy whose readCarry.carried == M2's hand) and latches it, which is itself
// evidence the pickup applied locally. Both sides log the same "SCENARIO CARRY"
// series as carry_order; Test-NpcCarry gates pickup-crossed / tracks-carrier /
// drop-crossed on the M2 + NPC series.
class NpcCarryScenario : public Scenario {
public:
    NpcCarryScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0),
          haveL0_(false), haveM2_(false), haveNpc_(false),
          nDown_(false), nPick_(false), nWalk_(false), nDrop_(false),
          lastHoldMs_(0) {}

    virtual const char* name() const { return "npc_carry"; }

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveL0_ || !haveM2_) latchSubjects(ctx);

        // Host: pick the carrier - the world NPC nearest L0 (upright ones only;
        // a KO'd body can't carry). Latched once, logged as the NPC role line.
        if (ctx.isHost && haveL0_ && !haveNpc_ && ctx.elapsedMs >= NPC_LATCH_AT_MS)
            latchHostNpc(ctx);
        // Join: detect the carrier - the local NPC copy that reads carrying=M2.
        // Finding one IS the feature working (the join executed the pickup).
        if (!ctx.isHost && haveM2_ && !haveNpc_) detectJoinCarrier(ctx);

        // ---- Window N (host): down M2, NPC carries it, walks, drops ----------
        if (ctx.isHost && haveM2_ && !nDown_ && ctx.elapsedMs >= N_DOWN_AT_MS) {
            bool ok = engine::orderDownSubject(ctx.gw, m2Hand_);
            logAct("N down", m2Hand_, ok); nDown_ = true;
        }
        if (ctx.isHost && haveNpc_ && haveM2_ && !nPick_ && ctx.elapsedMs >= N_PICK_AT_MS) {
            bool ok = engine::carrySubject(ctx.gw, npcHand_, m2Hand_);
            logAct("N pickup", m2Hand_, ok); nPick_ = true;
        }
        if (ctx.isHost && haveNpc_ && !nWalk_ && ctx.elapsedMs >= N_WALK_AT_MS) {
            nWalk_ = walkLeg(npcHand_, "N");
        }
        if (ctx.isHost && haveNpc_ && !nDrop_ && ctx.elapsedMs >= N_DROP_AT_MS) {
            bool ok = engine::dropSubject(ctx.gw, npcHand_, /*ragdoll*/true);
            logAct("N drop", npcHand_, ok); nDrop_ = true;
        }

        // Keep M2 KO'd through the carry leg (the scaffold KO timer expires
        // mid-carry otherwise - the carry_order lesson). Host-owned, so the
        // host re-tops it; timer-only, never a fresh knockout on the shoulder.
        if (ctx.isHost && haveM2_ && nDown_ && ctx.elapsedMs < HOLD_M2_UNTIL_MS &&
            ctx.elapsedMs - lastHoldMs_ >= 2000) {
            lastHoldMs_ = ctx.elapsedMs;
            holdSubject(m2Hand_);
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            const unsigned int ownRank = ctx.isHost ? 0u : 1u;
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
            if (haveM2_)  logCarryLine(m2Hand_, ctx.elapsedMs);
            if (haveNpc_) logCarryLine(npcHand_, ctx.elapsedMs);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (haveNpc_ && nDown_ && nPick_ && nDrop_)
                                 : (recvCount_ >= 1);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long NPC_LATCH_AT_MS  = 6000;
    static const unsigned long N_DOWN_AT_MS     = 8000;
    static const unsigned long N_PICK_AT_MS     = 12000;
    static const unsigned long N_WALK_AT_MS     = 14000;
    static const unsigned long N_DROP_AT_MS     = 24000;
    static const unsigned long HOLD_M2_UNTIL_MS = 30000;
    static const unsigned long HOST_DURATION_MS = 40000;
    static const unsigned long JOIN_DURATION_MS = 36000;
    static const unsigned int  MAX_SQUAD        = 32;
    static const unsigned int  MAX_NPCS         = 96;

    void logAct(const char* what, const unsigned int h[5], bool ok) {
        char b[144];
        _snprintf(b, sizeof(b) - 1, "SCENARIO CARRYACT %s hand=%u,%u ok=%d",
                  what, h[3], h[4], ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void holdSubject(const unsigned int h[5]) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (c) engine::holdDown(c);
    }

    bool walkLeg(const unsigned int h[5], const char* wTag) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (!c) return false;
        float x, y, z;
        if (!engine::readPos(c, &x, &y, &z)) return false;
        bool ok = engine::orderMoveTo(c, x + 10.0f, y, z);
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO CARRYACT %s walk hand=%u,%u ok=%d",
                  wTag, h[3], h[4], ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return true;
    }

    void logCarryLine(const unsigned int h[5], unsigned long t) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (!c) return;
        engine::CarryRead cr;
        if (!engine::readCarry(c, &cr) || !cr.valid) return;
        float x = 0, y = 0, z = 0;
        engine::readPos(c, &x, &y, &z);
        unsigned short bs = engine::readBodyState(c);
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO CARRY hand=%u,%u t=%lu carrying=%d carried=%u,%u "
                  "beingCarried=%d pos=%.2f,%.2f,%.2f bs=%u",
                  h[3], h[4], t, cr.carrying ? 1 : 0,
                  cr.carried[3], cr.carried[4], cr.beingCarried ? 1 : 0,
                  x, y, z, (unsigned)bs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchSubjects(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        if (!haveL0_) {
            int idx = tabLeaderIdx(sq, n, 0);
            if (idx >= 0) {
                handFromEntity(sq[idx], l0Hand_);
                haveL0_ = true;
                logSubject("L0", l0Hand_);
            }
        }
        if (haveL0_ && !haveM2_) {
            int best = -1;
            for (unsigned int i = 0; i < n; ++i) {
                if (tabRankOf(sq, n, i) != 0) continue;
                unsigned int h[5]; handFromEntity(sq[i], h);
                if (h[3] == l0Hand_[3] && h[4] == l0Hand_[4]) continue;
                if (best < 0 || tabHandLess(sq[i], sq[best])) best = (int)i;
            }
            if (best >= 0) {
                handFromEntity(sq[best], m2Hand_);
                haveM2_ = true;
                logSubject("M2", m2Hand_);
            }
        }
    }

    // Host: nearest UPRIGHT world NPC to L0 becomes the directed carrier.
    void latchHostNpc(const ScenarioContext& ctx) {
        Character* l0 = engine::resolveCharByHand(l0Hand_[3], l0Hand_[4],
                                                  l0Hand_[0], l0Hand_[1], l0Hand_[2]);
        float lx, ly, lz;
        if (!l0 || !engine::readPos(l0, &lx, &ly, &lz)) return;
        static Character*  chars[MAX_NPCS]; // main-thread only
        static EntityState states[MAX_NPCS];
        unsigned int n = engine::listNpcs(ctx.gw, chars, states, MAX_NPCS);
        int best = -1; float bestD2 = 1e18f;
        for (unsigned int i = 0; i < n; ++i) {
            if (coop::bodyIsDown(states[i].bodyState) ||
                (states[i].bodyState & BODY_DEAD) != 0) continue;
            float dx = states[i].x - lx, dy = states[i].y - ly, dz = states[i].z - lz;
            float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < bestD2) { bestD2 = d2; best = (int)i; }
        }
        if (best < 0) return;
        npcHand_[0] = states[best].hType;
        npcHand_[1] = states[best].hContainer;
        npcHand_[2] = states[best].hContainerSerial;
        npcHand_[3] = states[best].hIndex;
        npcHand_[4] = states[best].hSerial;
        haveNpc_ = true;
        logSubject("NPC", npcHand_);
    }

    // Join: the carrier reveals itself - the local NPC copy carrying M2.
    void detectJoinCarrier(const ScenarioContext& ctx) {
        static Character*  chars[MAX_NPCS]; // main-thread only
        static EntityState states[MAX_NPCS];
        unsigned int n = engine::listNpcs(ctx.gw, chars, states, MAX_NPCS);
        for (unsigned int i = 0; i < n; ++i) {
            engine::CarryRead cr;
            if (!engine::readCarry(chars[i], &cr) || !cr.carrying) continue;
            if (cr.carried[3] != m2Hand_[3] || cr.carried[4] != m2Hand_[4]) continue;
            npcHand_[0] = states[i].hType;
            npcHand_[1] = states[i].hContainer;
            npcHand_[2] = states[i].hContainerSerial;
            npcHand_[3] = states[i].hIndex;
            npcHand_[4] = states[i].hSerial;
            haveNpc_ = true;
            logSubject("NPC", npcHand_);
            return;
        }
    }

    void logSubject(const char* who, const unsigned int h[5]) {
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO CARRY %s hand=%u,%u", who, h[3], h[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveL0_, haveM2_, haveNpc_;
    bool          nDown_, nPick_, nWalk_, nDrop_;
    unsigned long lastHoldMs_;
    unsigned int  l0Hand_[5];
    unsigned int  m2Hand_[5];
    unsigned int  npcHand_[5];
};

// bed_pose (protocol 19 phase 1, conscious bed use): USE_BED / USE_BED_ORDER
// have been on the reproducible-pose allowlist since the fixture-pose work but
// were never runtime-validated (spike 24 PARTIAL). Save 'bedcage1' bakes a Camp
// Bed + Prisoner Cage with save-stable hands next to the squad. The HOST orders
// its own leader (L0) to USE_BED_ORDER at the baked bed via the player-order
// path; the stream carries the committed bed task + fixture subject hand and
// the JOIN's driven L0 copy must commit the same pose at the same bed. Both
// sides log the standard MEMBER/RECV series (task= + pelvis=); Test-BedPose
// anchors on the "SCENARIO BED ORDER" marker and gates host-committed /
// join-committed / co-located.
class BedPoseScenario : public Scenario {
public:
    BedPoseScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0), haveL0_(false),
          orderLogged_(false), lastOrderMs_(0), orderOk_(false) {}

    virtual const char* name() const { return "bed_pose"; }

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveL0_) latchLeader(ctx);

        // HOST, order phase: send L0 to the baked bed. orderUseBed is GUARDED
        // (no-op once L0 is on a bed task), so the throttled re-issue recovers
        // a failed first order without ever standing the sleeper back up.
        if (ctx.isHost && haveL0_ && ctx.elapsedMs >= ORDER_AT_MS &&
            (!orderLogged_ || (!orderOk_ && ctx.elapsedMs - lastOrderMs_ >= 4000))) {
            lastOrderMs_ = ctx.elapsedMs;
            int ordered = 0, useBed = 0;
            orderOk_ = engine::orderUseBed(ctx.gw, l0Hand_, &ordered, &useBed);
            if (!orderLogged_) {
                char b[160];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO BED ORDER issued hand=%u,%u task=%d accept=%d,%d ok=%d",
                          l0Hand_[3], l0Hand_[4], ordered, ordered, useBed,
                          orderOk_ ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                orderLogged_ = true;
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            const unsigned int ownRank = ctx.isHost ? 0u : 1u;
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (orderLogged_ && orderOk_) : (recvCount_ >= 1);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long ORDER_AT_MS      = 14000; // join logs standing baseline first
    static const unsigned long HOST_DURATION_MS = 60000; // approach + in-bed observation
    static const unsigned long JOIN_DURATION_MS = 54000;
    static const unsigned int  MAX_SQUAD        = 32;

    void latchLeader(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int idx = tabLeaderIdx(sq, n, 0); // host tab's leader on BOTH sides
        if (idx >= 0) {
            handFromEntity(sq[idx], l0Hand_);
            haveL0_ = true;
            char b[96];
            _snprintf(b, sizeof(b) - 1, "SCENARIO BED L0 hand=%u,%u",
                      l0Hand_[3], l0Hand_[4]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveL0_;
    bool          orderLogged_;
    unsigned long lastOrderMs_;
    bool          orderOk_;
    unsigned int  l0Hand_[5];
};

// bed_put / cage_put (protocol 19 phase 3, unconscious placement): save
// 'bedcage1' again. Two sequential owner-side windows against the SAME baked
// fixture (one occupant at a time):
//   Window A (host own-tab):  KO M2 (host tab's second member), place it in
//     the fixture via the putSubjectInFurniture scaffold, hold it there, then
//     take it back out. The JOIN's driven M2 copy must mirror enter + exit.
//   Window B (join own-tab):  same shape with the join's leader L1 - the
//     reverse ownership direction over the identical machinery.
// The KO is held down by its OWNER's holdDown re-top every 2 s (timer-only,
// the carry_order lesson) so the scaffold KO can't expire mid-occupancy.
// Both sides log the standard MEMBER/RECV series plus a per-subject
// "SCENARIO FURN hand=i,s t=ms in=<0|1|2> furn=i,s pos=x,y,z bs=n" line at
// 2 Hz reading the LOCAL character (on the peer that is the driven copy -
// exactly what must have entered). Test-FurnPut gates on both windows.
class FurnPutScenario : public Scenario {
public:
    explicit FurnPutScenario(int kind)
        : kind_(kind), passed_(false), recvCount_(0), lastLogMs_(0),
          haveM2_(false), haveL1_(false), lastHoldMs_(0),
          aDown_(false), aPut_(false), aOut_(false),
          bDown_(false), bPut_(false), bOut_(false),
          aPutOk_(false), bPutOk_(false), lastPutMs_(0) {}

    virtual const char* name() const { return kind_ == 2 ? "cage_put" : "bed_put"; }

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveM2_ || !haveL1_) latchSubjects(ctx);

        // ---- Window A (host, own-tab): KO M2, put it in, take it out -------
        if (ctx.isHost && haveM2_ && !aDown_ && ctx.elapsedMs >= A_DOWN_AT_MS) {
            bool ok = engine::orderDownSubject(ctx.gw, m2Hand_);
            logAct("A down", m2Hand_, ok); aDown_ = true;
        }
        if (ctx.isHost && haveM2_ && ctx.elapsedMs >= A_PUT_AT_MS &&
            ctx.elapsedMs < A_OUT_AT_MS &&
            (!aPut_ || (!aPutOk_ && ctx.elapsedMs - lastPutMs_ >= 3000))) {
            // Throttled re-issue until the engine accepts (the bed_pose lesson:
            // never give up on the first frame's transient failure).
            lastPutMs_ = ctx.elapsedMs;
            aPutOk_ = engine::putSubjectInFurniture(ctx.gw, m2Hand_, kind_, true);
            if (!aPut_) { logAct("A put", m2Hand_, aPutOk_); aPut_ = true; }
        }
        if (ctx.isHost && haveM2_ && !aOut_ && ctx.elapsedMs >= A_OUT_AT_MS) {
            bool ok = engine::putSubjectInFurniture(ctx.gw, m2Hand_, kind_, false);
            logAct("A out", m2Hand_, ok); aOut_ = true;
        }

        // ---- Window B (join, own-tab): the same over L1 ---------------------
        if (!ctx.isHost && haveL1_ && !bDown_ && ctx.elapsedMs >= B_DOWN_AT_MS) {
            bool ok = engine::orderDownSubject(ctx.gw, l1Hand_);
            logAct("B down", l1Hand_, ok); bDown_ = true;
        }
        if (!ctx.isHost && haveL1_ && ctx.elapsedMs >= B_PUT_AT_MS &&
            ctx.elapsedMs < B_OUT_AT_MS &&
            (!bPut_ || (!bPutOk_ && ctx.elapsedMs - lastPutMs_ >= 3000))) {
            lastPutMs_ = ctx.elapsedMs;
            bPutOk_ = engine::putSubjectInFurniture(ctx.gw, l1Hand_, kind_, true);
            if (!bPut_) { logAct("B put", l1Hand_, bPutOk_); bPut_ = true; }
        }
        if (!ctx.isHost && haveL1_ && !bOut_ && ctx.elapsedMs >= B_OUT_AT_MS) {
            bool ok = engine::putSubjectInFurniture(ctx.gw, l1Hand_, kind_, false);
            logAct("B out", l1Hand_, ok); bOut_ = true;
        }

        // Owner-side KO hold (timer-only re-top) through each subject's window.
        if (ctx.elapsedMs - lastHoldMs_ >= 2000) {
            lastHoldMs_ = ctx.elapsedMs;
            if (ctx.isHost && haveM2_ && aDown_ && ctx.elapsedMs < A_HOLD_UNTIL_MS)
                holdSubject(m2Hand_);
            if (!ctx.isHost && haveL1_ && bDown_ && ctx.elapsedMs < B_HOLD_UNTIL_MS)
                holdSubject(l1Hand_);
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            const unsigned int ownRank = ctx.isHost ? 0u : 1u;
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
            if (haveM2_) logFurnLine(m2Hand_, ctx.elapsedMs);
            if (haveL1_) logFurnLine(l1Hand_, ctx.elapsedMs);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (aDown_ && aPut_ && aPutOk_ && aOut_)
                                 : (bDown_ && bPut_ && bPutOk_ && bOut_ && recvCount_ >= 1);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    // Shared timeline (peer-armed clock on both sides): KO settles 6 s before
    // the put; ~16 s in the furniture; 6 s of settle between the windows.
    static const unsigned long A_DOWN_AT_MS    = 8000;
    static const unsigned long A_PUT_AT_MS     = 14000;
    static const unsigned long A_OUT_AT_MS     = 30000;
    static const unsigned long A_HOLD_UNTIL_MS = 34000;
    static const unsigned long B_DOWN_AT_MS    = 36000;
    static const unsigned long B_PUT_AT_MS     = 42000;
    static const unsigned long B_OUT_AT_MS     = 58000;
    static const unsigned long B_HOLD_UNTIL_MS = 62000;
    static const unsigned long HOST_DURATION_MS = 70000;
    static const unsigned long JOIN_DURATION_MS = 66000;
    static const unsigned int  MAX_SQUAD        = 32;

    void logAct(const char* what, const unsigned int h[5], bool ok) {
        char b[144];
        _snprintf(b, sizeof(b) - 1, "SCENARIO FURNACT %s hand=%u,%u kind=%d ok=%d",
                  what, h[3], h[4], kind_, ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void holdSubject(const unsigned int h[5]) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (c) engine::holdDown(c);
    }

    // One "SCENARIO FURN" line: this body's LOCAL occupancy + position.
    void logFurnLine(const unsigned int h[5], unsigned long t) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (!c) return;
        engine::FurnitureRead fr;
        if (!engine::readFurniture(c, &fr) || !fr.valid) return;
        float x = 0, y = 0, z = 0;
        engine::readPos(c, &x, &y, &z);
        unsigned short bs = engine::readBodyState(c);
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO FURN hand=%u,%u t=%lu in=%d furn=%u,%u pos=%.2f,%.2f,%.2f bs=%u",
                  h[3], h[4], t, fr.kind, fr.furn[3], fr.furn[4], x, y, z,
                  (unsigned)bs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchSubjects(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        if (!haveM2_) {
            // Host tab's SECOND member (the lowest non-leader hand of rank 0).
            int lidx = tabLeaderIdx(sq, n, 0);
            if (lidx >= 0) {
                unsigned int lh[5]; handFromEntity(sq[lidx], lh);
                int best = -1;
                for (unsigned int i = 0; i < n; ++i) {
                    if (tabRankOf(sq, n, i) != 0) continue;
                    unsigned int h[5]; handFromEntity(sq[i], h);
                    if (h[3] == lh[3] && h[4] == lh[4]) continue;
                    if (best < 0 || tabHandLess(sq[i], sq[best])) best = (int)i;
                }
                if (best >= 0) {
                    handFromEntity(sq[best], m2Hand_);
                    haveM2_ = true;
                    logSubject("M2", m2Hand_);
                }
            }
        }
        if (!haveL1_) {
            int idx = tabLeaderIdx(sq, n, 1);
            if (idx >= 0) {
                handFromEntity(sq[idx], l1Hand_);
                haveL1_ = true;
                logSubject("L1", l1Hand_);
            }
        }
    }

    void logSubject(const char* who, const unsigned int h[5]) {
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO FURN %s hand=%u,%u kind=%d",
                  who, h[3], h[4], kind_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    int           kind_;
    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveM2_, haveL1_;
    unsigned long lastHoldMs_;
    bool          aDown_, aPut_, aOut_;
    bool          bDown_, bPut_, bOut_;
    bool          aPutOk_, bPutOk_;
    unsigned long lastPutMs_;
    unsigned int  m2Hand_[5];
    unsigned int  l1Hand_[5];
};

// sneak_probe (protocol 20 phase 0, host-side spike): does the engine's stealth
// detection fire against a DRIVEN copy, and is whoSeesMeSneaking safely
// readable? The HOST directly sets stealthMode on its driven copy of the
// join's leader (L1) near the bar NPCs (save 'sync'), then logs the copy's
// readStealth series at 2 Hz: mode / unseen / map size / top seer entries.
// The JOIN logs the same series off its OWN L1 (baseline: mode stays off
// locally - nothing streams stealth yet in this spike). Log-only; the oracle
// just gates "host saw detection entries while the mode was on".
class SneakProbeScenario : public Scenario {
public:
    SneakProbeScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0), haveL1_(false),
          onDone_(false), offDone_(false), onOk_(false), lastActMs_(0),
          sawSeer_(false) {}

    virtual const char* name() const { return "sneak_probe"; }

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveL1_) latchL1(ctx);

        // HOST: force stealth mode on the DRIVEN L1 copy (throttled re-issue
        // until the engine call lands), then release it near the end.
        if (ctx.isHost && haveL1_ && ctx.elapsedMs >= ON_AT_MS &&
            ctx.elapsedMs < OFF_AT_MS &&
            (!onDone_ || (!onOk_ && ctx.elapsedMs - lastActMs_ >= 3000))) {
            lastActMs_ = ctx.elapsedMs;
            onOk_ = engine::sneakSubject(ctx.gw, l1Hand_, true);
            if (!onDone_) { logAct("on", onOk_); onDone_ = true; }
        }
        if (ctx.isHost && haveL1_ && !offDone_ && ctx.elapsedMs >= OFF_AT_MS) {
            bool ok = engine::sneakSubject(ctx.gw, l1Hand_, false);
            logAct("off", ok); offDone_ = true;
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            const unsigned int ownRank = ctx.isHost ? 0u : 1u;
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
            if (haveL1_) logSneakLine(ctx.elapsedMs);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (onDone_ && onOk_ && sawSeer_)
                                 : (recvCount_ >= 1);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long ON_AT_MS         = 8000;
    static const unsigned long OFF_AT_MS        = 40000;
    static const unsigned long HOST_DURATION_MS = 50000;
    static const unsigned long JOIN_DURATION_MS = 46000;
    static const unsigned int  MAX_SQUAD        = 32;

    void logAct(const char* what, bool ok) {
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SNEAKACT %s hand=%u,%u ok=%d",
                  what, l1Hand_[3], l1Hand_[4], ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // One "SCENARIO SNEAKPROBE" line: L1's LOCAL stealth state + top seers.
    void logSneakLine(unsigned long t) {
        Character* c = engine::resolveCharByHand(l1Hand_[3], l1Hand_[4],
                                                 l1Hand_[0], l1Hand_[1], l1Hand_[2]);
        if (!c) return;
        engine::StealthRead sr;
        if (!engine::readStealth(c, &sr) || !sr.valid) {
            char b[96];
            _snprintf(b, sizeof(b) - 1, "SCENARIO SNEAKPROBE t=%lu readfail", t);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return;
        }
        if (sr.nSeers > 0) sawSeer_ = true;
        char b[288]; int off = _snprintf(b, sizeof(b) - 1,
            "SCENARIO SNEAKPROBE hand=%u,%u t=%lu mode=%d unseen=%u map=%u",
            l1Hand_[3], l1Hand_[4], t, sr.sneaking ? 1 : 0,
            (unsigned)sr.unseen, sr.mapSize);
        for (unsigned int i = 0; i < sr.nSeers && i < 4 && off > 0 &&
                                 off < (int)sizeof(b) - 48; ++i) {
            off += _snprintf(b + off, sizeof(b) - 1 - off,
                             " seer=%u,%u:%u:%.2f",
                             sr.seers[i].npc[3], sr.seers[i].npc[4],
                             (unsigned)sr.seers[i].see, sr.seers[i].prog);
        }
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchL1(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int idx = tabLeaderIdx(sq, n, 1); // join tab's leader on BOTH sides
        if (idx >= 0) {
            handFromEntity(sq[idx], l1Hand_);
            haveL1_ = true;
            char b[96];
            _snprintf(b, sizeof(b) - 1, "SCENARIO SNEAK L1 hand=%u,%u",
                      l1Hand_[3], l1Hand_[4]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveL1_;
    bool          onDone_, offDone_, onOk_;
    unsigned long lastActMs_;
    bool          sawSeer_;
    unsigned int  l1Hand_[5];
};

// sneak_pose (protocol 20 phase 4): stealth POSTURE crossing, both ownership
// directions. Window A: the HOST toggles stealth on its own leader (L0) ON at
// T+10 s, OFF at T+35 s - the join's driven copy must follow via the streamed
// BODY_SNEAK bit. Window B: the JOIN does the same with its leader (L1) at
// T+45/T+70 - the host's copy must follow. Both sides log a 2 Hz
// "SCENARIO SNEAK hand=i,s t=ms mode=0|1 bs=n" series for BOTH subjects; the
// oracle asserts the PEER's copy flips mode within budget on all four edges.
class SneakPoseScenario : public Scenario {
public:
    SneakPoseScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0),
          haveL0_(false), haveL1_(false),
          aOnDone_(false), aOffDone_(false), bOnDone_(false), bOffDone_(false) {}

    virtual const char* name() const { return "sneak_pose"; }

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveL0_ || !haveL1_) latchLeaders(ctx);

        // Window A: HOST toggles ITS OWN leader. Window B: JOIN toggles its own.
        if (ctx.isHost && haveL0_) {
            if (!aOnDone_ && ctx.elapsedMs >= A_ON_MS) {
                aOnDone_ = true; logAct("A-on", engine::sneakSubject(ctx.gw, l0Hand_, true), l0Hand_);
            }
            if (!aOffDone_ && ctx.elapsedMs >= A_OFF_MS) {
                aOffDone_ = true; logAct("A-off", engine::sneakSubject(ctx.gw, l0Hand_, false), l0Hand_);
            }
        }
        if (!ctx.isHost && haveL1_) {
            if (!bOnDone_ && ctx.elapsedMs >= B_ON_MS) {
                bOnDone_ = true; logAct("B-on", engine::sneakSubject(ctx.gw, l1Hand_, true), l1Hand_);
            }
            if (!bOffDone_ && ctx.elapsedMs >= B_OFF_MS) {
                bOffDone_ = true; logAct("B-off", engine::sneakSubject(ctx.gw, l1Hand_, false), l1Hand_);
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            const unsigned int ownRank = ctx.isHost ? 0u : 1u;
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
            if (haveL0_) logSneakLine(ctx.elapsedMs, l0Hand_);
            if (haveL1_) logSneakLine(ctx.elapsedMs, l1Hand_);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (aOnDone_ && aOffDone_) : (recvCount_ >= 1);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long A_ON_MS  = 10000;
    static const unsigned long A_OFF_MS = 35000;
    static const unsigned long B_ON_MS  = 45000;
    static const unsigned long B_OFF_MS = 70000;
    static const unsigned long HOST_DURATION_MS = 85000;
    static const unsigned long JOIN_DURATION_MS = 80000;
    static const unsigned int  MAX_SQUAD        = 32;

    void logAct(const char* what, bool ok, const unsigned int h[5]) {
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SNEAKACT %s hand=%u,%u ok=%d",
                  what, h[3], h[4], ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void logSneakLine(unsigned long t, const unsigned int h[5]) {
        Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
        if (!c) return;
        int mode = engine::readStealthMode(c);
        unsigned short bs = engine::readBodyState(c);
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SNEAK hand=%u,%u t=%lu mode=%d bs=%u",
                  h[3], h[4], t, mode, (unsigned)bs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchLeaders(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        if (!haveL0_) {
            int idx = tabLeaderIdx(sq, n, 0);
            if (idx >= 0) {
                handFromEntity(sq[idx], l0Hand_); haveL0_ = true;
                char b[96]; _snprintf(b, sizeof(b) - 1, "SCENARIO SNEAK L0 hand=%u,%u",
                                      l0Hand_[3], l0Hand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        if (!haveL1_) {
            int idx = tabLeaderIdx(sq, n, 1);
            if (idx >= 0) {
                handFromEntity(sq[idx], l1Hand_); haveL1_ = true;
                char b[96]; _snprintf(b, sizeof(b) - 1, "SCENARIO SNEAK L1 hand=%u,%u",
                                      l1Hand_[3], l1Hand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
    }

    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveL0_, haveL1_;
    bool          aOnDone_, aOffDone_, bOnDone_, bOffDone_;
    unsigned int  l0Hand_[5];
    unsigned int  l1Hand_[5];
};

// sneak_detect (protocol 20 phase 4): DETECTION-INDICATOR crossing. The JOIN
// toggles stealth on its own leader (L1) near the bar NPCs (save 'sync'). The
// host's world is the detection authority: its NPCs fill whoSeesMeSneaking on
// its DRIVEN copy of L1 (spike-proven), publishStealth streams it back, and
// the join replays it onto its OWN L1 - both sides log a 2 Hz "SCENARIO
// DETECT hand=i,s t=ms mode=d map=n see=..." series off their local L1. The
// oracle asserts the join accumulated entries while sneaking (via the
// feedback channel - "[sneak] DETECT RECV ... applied>=1" proves the path)
// and that they cleared after the sneak ended.
class SneakDetectScenario : public Scenario {
public:
    SneakDetectScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0), haveL1_(false),
          onDone_(false), offDone_(false) {}

    virtual const char* name() const { return "sneak_detect"; }

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveL1_) latchL1(ctx);

        // The JOIN owns the sneaker: it performs the toggles.
        if (!ctx.isHost && haveL1_) {
            if (!onDone_ && ctx.elapsedMs >= ON_AT_MS) {
                onDone_ = true;
                bool ok = engine::sneakSubject(ctx.gw, l1Hand_, true);
                logAct("on", ok);
            }
            if (!offDone_ && ctx.elapsedMs >= OFF_AT_MS) {
                offDone_ = true;
                bool ok = engine::sneakSubject(ctx.gw, l1Hand_, false);
                logAct("off", ok);
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            const unsigned int ownRank = ctx.isHost ? 0u : 1u;
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
            if (haveL1_) logDetectLine(ctx.elapsedMs);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? true : (recvCount_ >= 1 && onDone_ && offDone_);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long ON_AT_MS         = 10000;
    static const unsigned long OFF_AT_MS        = 45000;
    static const unsigned long HOST_DURATION_MS = 62000;
    static const unsigned long JOIN_DURATION_MS = 58000;
    static const unsigned int  MAX_SQUAD        = 32;

    void logAct(const char* what, bool ok) {
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SNEAKACT %s hand=%u,%u ok=%d",
                  what, l1Hand_[3], l1Hand_[4], ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void logDetectLine(unsigned long t) {
        Character* c = engine::resolveCharByHand(l1Hand_[3], l1Hand_[4],
                                                 l1Hand_[0], l1Hand_[1], l1Hand_[2]);
        if (!c) return;
        engine::StealthRead sr;
        if (!engine::readStealth(c, &sr) || !sr.valid) return;
        char b[288]; int off = _snprintf(b, sizeof(b) - 1,
            "SCENARIO DETECT hand=%u,%u t=%lu mode=%d unseen=%u map=%u",
            l1Hand_[3], l1Hand_[4], t, sr.sneaking ? 1 : 0,
            (unsigned)sr.unseen, sr.mapSize);
        for (unsigned int i = 0; i < sr.nSeers && i < 4 && off > 0 &&
                                 off < (int)sizeof(b) - 48; ++i) {
            off += _snprintf(b + off, sizeof(b) - 1 - off,
                             " seer=%u,%u:%u:%.2f",
                             sr.seers[i].npc[3], sr.seers[i].npc[4],
                             (unsigned)sr.seers[i].see, sr.seers[i].prog);
        }
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchL1(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int idx = tabLeaderIdx(sq, n, 1);
        if (idx >= 0) {
            handFromEntity(sq[idx], l1Hand_);
            haveL1_ = true;
            char b[96];
            _snprintf(b, sizeof(b) - 1, "SCENARIO SNEAK L1 hand=%u,%u",
                      l1Hand_[3], l1Hand_[4]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveL1_;
    bool          onDone_, offDone_;
    unsigned int  l1Hand_[5];
};

// speed_sync (consensus game-speed validation): both clients run the default-on
// speed-sync module; the scenario SIMULATES user speed clicks by writing the
// engine's speed directly (writeGameSpeed != the replicator's lastApplied ->
// detected as a user request, exactly like a real click). Save 'sync' (the bar
// with armed NPCs for the combat phase). Timeline (peer-ready armed), each step
// exercising one consensus rule:
//   T+10 s HOST clicks 3x -> DENIED (join still requests 1x; min holds 1x -
//          the "both must raise" rule; the host engine snaps back).
//   T+22 s JOIN clicks 3x -> requests now 3/3 -> both settle at 3x.
//   T+34 s JOIN clicks 1x -> min rule: EITHER can lower -> both drop to 1x.
//   T+44 s JOIN clicks 3x -> both back at 3x.
//   T+52 s HOST orders a bar NPC onto its OWN leader -> own-squad combat ->
//          the consensus cap demotes the effective speed to 1x mid-3x.
// Both sides log "SCENARIO SPEED t=<ms> mult=<f> paused=<n>" at ~2 Hz; the
// Test-SpeedSync oracle time-aligns the two series (CLOCKSYNC-corrected) and
// gates the transition count, the denied lone raise, each transition's follow
// latency, the match fraction, and the combat window sitting at 1x.
class SpeedSyncScenario : public Scenario {
public:
    SpeedSyncScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0), lastOrderMs_(0),
          haveOwn_(false), haveStriker_(false), hostClicked_(false),
          hostClicked1_(false), hostClicked3b_(false),
          joinClicked3a_(false), joinClicked1_(false), joinClicked3b_(false),
          combatIssued_(false) {}

    virtual const char* name() const { return "speed_sync"; }

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        const unsigned int ownRank = ctx.isHost ? 0u : 1u;
        if (!haveOwn_) latchLeader(ctx, ownRank);

        // Simulated user clicks. writeGameSpeed goes through the engine's own
        // (hooked) setters, so each call registers as captured USER INTENT -
        // the same path a real UI click takes. The middle legs exercise the
        // previously-impossible case: a click EQUAL to the current effective
        // (join lowers its stale 3x vote by clicking 1x while the effective is
        // already 1x), which the old state-diff detector could never see.
        if (ctx.isHost && !hostClicked_ && ctx.elapsedMs >= HOST_3X_AT_MS) {
            bool ok = engine::writeGameSpeed(ctx.gw, 3.0f, false);
            logClick("host", 3.0f, ok, "");
            hostClicked_ = true;
        }
        if (!ctx.isHost && !joinClicked3a_ && ctx.elapsedMs >= JOIN_3XA_AT_MS) {
            bool ok = engine::writeGameSpeed(ctx.gw, 3.0f, false);
            logClick("join", 3.0f, ok, "");
            joinClicked3a_ = true;
        }
        // Host lowers to 1x: effective drops to 1x but the JOIN's 3x vote is
        // now stale-high (the stuck-vote setup).
        if (ctx.isHost && !hostClicked1_ && ctx.elapsedMs >= HOST_1X_AT_MS) {
            bool ok = engine::writeGameSpeed(ctx.gw, 1.0f, false);
            logClick("host", 1.0f, ok, " tag=lower");
            hostClicked1_ = true;
        }
        // The SAME-VALUE click: join clicks 1x while the effective is already
        // 1x. Engine state doesn't change - only the intent hooks can see it.
        if (!ctx.isHost && !joinClicked1_ && ctx.elapsedMs >= JOIN_1X_SAME_AT_MS) {
            bool ok = engine::writeGameSpeed(ctx.gw, 1.0f, false);
            logClick("join", 1.0f, ok, " tag=samevalue");
            joinClicked1_ = true;
        }
        // Host re-raises to 3x: must be DENIED (join's vote is now 1x) - the
        // assertion that the same-value click actually landed as a vote.
        if (ctx.isHost && !hostClicked3b_ && ctx.elapsedMs >= HOST_3XB_AT_MS) {
            bool ok = engine::writeGameSpeed(ctx.gw, 3.0f, false);
            logClick("host", 3.0f, ok, " tag=reraise");
            hostClicked3b_ = true;
        }
        if (!ctx.isHost && !joinClicked3b_ && ctx.elapsedMs >= JOIN_3XB_AT_MS) {
            bool ok = engine::writeGameSpeed(ctx.gw, 3.0f, false);
            logClick("join", 3.0f, ok, " tag=raise2");
            joinClicked3b_ = true;
        }

        // Combat phase: a bar NPC onto the host's OWN leader, so the host's
        // own-squad combat flag trips and the consensus cap demotes to 1x.
        // Re-ordered every 2.5 s (safe no-op while fighting); a KO'd striker
        // is replaced (the player_combat restrike lesson).
        if (ctx.isHost && haveOwn_ && ctx.elapsedMs >= COMBAT_AT_MS &&
            (ctx.elapsedMs - lastOrderMs_ >= 2500 || lastOrderMs_ == 0)) {
            lastOrderMs_ = ctx.elapsedMs;
            bool pick = !haveStriker_;
            if (haveStriker_) {
                engine::MedicalRead mr;
                if (!engine::readMedicalByHand(striker_, &mr) || !mr.valid ||
                    mr.unconscious || mr.dead)
                    pick = true;
            }
            if (pick)
                haveStriker_ = engine::pickCombatVictim(ctx.gw, ownHand_,
                                                        haveStriker_ ? striker_ : 0,
                                                        striker_);
            if (haveStriker_) {
                bool ok = engine::orderAttackByHand(ctx.gw, striker_, ownHand_);
                if (!combatIssued_) {
                    char b[128];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO SPEEDSYNC combat issued atk=%u,%u vic=%u,%u ok=%d",
                              striker_[3], striker_[4], ownHand_[3], ownHand_[4],
                              ok ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    combatIssued_ = true;
                }
            } else if (!combatIssued_) {
                coop::logLine("SCENARIO SPEEDSYNC combat pick FAILED (no upright NPC)");
                combatIssued_ = true;
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            // The SPEED series the oracle compares across the two clients.
            float mult = 0.0f; bool paused = false;
            if (engine::readGameSpeed(ctx.gw, &mult, &paused)) {
                char b[96];
                _snprintf(b, sizeof(b) - 1, "SCENARIO SPEED t=%lu mult=%.2f paused=%d",
                          ctx.elapsedMs, mult, paused ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            // Squad MEMBER/RECV series (harness anchors + advisory transform
            // oracles; also proves the 3x window doesn't break the stream).
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            bool sawPeer = false;
            for (unsigned int i = 0; i < n; ++i) {
                int r = tabRankOf(sq, n, i);
                if (r < 0) continue;
                logScenarioEntity(((unsigned int)r == ownRank) ? "MEMBER" : "RECV", sq[i]);
                if ((unsigned int)r != ownRank) sawPeer = true;
            }
            if (!ctx.isHost && sawPeer) ++recvCount_;
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (hostClicked_ && hostClicked1_ &&
                                    hostClicked3b_ && combatIssued_)
                                 : (joinClicked3a_ && joinClicked1_ &&
                                    joinClicked3b_ && recvCount_ >= 1);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    // Shared (peer-ready-armed) timeline; the combat window runs 62 s -> end.
    static const unsigned long HOST_3X_AT_MS      = 10000;
    static const unsigned long JOIN_3XA_AT_MS     = 22000;
    static const unsigned long HOST_1X_AT_MS      = 30000;
    static const unsigned long JOIN_1X_SAME_AT_MS = 38000;
    static const unsigned long HOST_3XB_AT_MS     = 46000;
    static const unsigned long JOIN_3XB_AT_MS     = 54000;
    static const unsigned long COMBAT_AT_MS       = 62000;
    static const unsigned long HOST_DURATION_MS   = 85000;
    static const unsigned long JOIN_DURATION_MS   = 78000;
    static const unsigned int  MAX_SQUAD          = 32;

    static void logClick(const char* who, float mult, bool ok, const char* tag) {
        char b[112];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SPEEDSYNC %s click mult=%.1f ok=%d%s",
                  who, mult, ok ? 1 : 0, tag);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void latchLeader(const ScenarioContext& ctx, unsigned int ownRank) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int idx = tabLeaderIdx(sq, n, ownRank);
        if (idx < 0) return;
        handFromEntity(sq[idx], ownHand_);
        haveOwn_ = true;
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SPEEDSYNC own rank=%u hand=%u,%u",
                  ownRank, ownHand_[3], ownHand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    unsigned long lastOrderMs_;
    bool          haveOwn_;
    bool          haveStriker_;
    bool          hostClicked_;
    bool          hostClicked1_;
    bool          hostClicked3b_;
    bool          joinClicked3a_;
    bool          joinClicked1_;
    bool          joinClicked3b_;
    bool          combatIssued_;
    unsigned int  ownHand_[5];
    unsigned int  striker_[5];
};

// speed_probe (vote-decoupling phase-0 spike, HOST-side, log-only): prove the
// three claims the decoupled design rests on, with speedSync forced OFF so the
// replicator can't fight the probe:
//   1. QUIET writes (setFrameSpeedMultiplier + guarded userPause) drive the
//      sim multiplier and it STICKS - nothing re-syncs it from the buttons.
//   2. QUIET writes leave the UI speed buttons untouched (the buttons=...
//      series stays constant across quiet acts), while a LOUD writeGameSpeed
//      (a simulated real click) moves them.
//   3. The intent hooks capture the loud click as a vote (INTENT line) and
//      stay silent for quiet writes.
// The join idles and logs its own series (harness anchor only).
class SpeedProbeScenario : public Scenario {
public:
    SpeedProbeScenario()
        : passed_(false), lastLogMs_(0), quiet3_(false), loud2_(false),
          quiet1_(false), quietPause_(false), quietResume_(false),
          actsOk_(true) {}

    virtual const char* name() const { return "speed_probe"; }

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        // Drain captured intent every tick (both clients): quiet acts must NOT
        // produce INTENT lines; the loud click at 16 s must.
        float im = 0.0f; bool ip = false;
        while (engine::consumeSpeedIntent(ctx.gw, &im, &ip)) {
            char b[96];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO SPEEDPROBE INTENT t=%lu mult=%.2f paused=%d",
                      ctx.elapsedMs, im, ip ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        if (ctx.isHost) {
            if (!quiet3_ && ctx.elapsedMs >= QUIET3_AT_MS) {
                quiet3_ = true;
                act("quiet3", engine::writeGameSpeedQuiet(ctx.gw, 3.0f, false), ctx);
            }
            if (!loud2_ && ctx.elapsedMs >= LOUD2_AT_MS) {
                loud2_ = true;
                act("loud2", engine::writeGameSpeed(ctx.gw, 2.0f, false), ctx);
            }
            if (!quiet1_ && ctx.elapsedMs >= QUIET1_AT_MS) {
                quiet1_ = true;
                act("quiet1", engine::writeGameSpeedQuiet(ctx.gw, 1.0f, false), ctx);
            }
            if (!quietPause_ && ctx.elapsedMs >= QPAUSE_AT_MS) {
                quietPause_ = true;
                act("quietpause", engine::writeGameSpeedQuiet(ctx.gw, 1.0f, true), ctx);
            }
            if (!quietResume_ && ctx.elapsedMs >= QRESUME_AT_MS) {
                quietResume_ = true;
                act("quietresume", engine::writeGameSpeedQuiet(ctx.gw, 1.0f, false), ctx);
            }
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            float mult = 0.0f; bool paused = false;
            char btn[16]; btn[0] = '\0';
            int nBtn = engine::readSpeedButtons(btn, sizeof(btn));
            if (engine::readGameSpeed(ctx.gw, &mult, &paused)) {
                char b[128];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO SPEEDPROBE t=%lu mult=%.2f paused=%d nbtn=%d buttons=%s",
                          ctx.elapsedMs, mult, paused ? 1 : 0, nBtn,
                          (nBtn > 0) ? btn : "?");
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost
                ? (quiet3_ && loud2_ && quiet1_ && quietPause_ &&
                   quietResume_ && actsOk_)
                : true; // join is a passive anchor; the oracle judges the host series
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long QUIET3_AT_MS     = 8000;
    static const unsigned long LOUD2_AT_MS      = 16000;
    static const unsigned long QUIET1_AT_MS     = 24000;
    static const unsigned long QPAUSE_AT_MS     = 32000;
    static const unsigned long QRESUME_AT_MS    = 40000;
    static const unsigned long HOST_DURATION_MS = 48000;
    static const unsigned long JOIN_DURATION_MS = 44000;

    void act(const char* what, bool ok, const ScenarioContext& ctx) {
        if (!ok) actsOk_ = false;
        char b[112];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SPEEDPROBE act=%s t=%lu ok=%d",
                  what, ctx.elapsedMs, ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    bool          passed_;
    unsigned long lastLogMs_;
    bool          quiet3_;
    bool          loud2_;
    bool          quiet1_;
    bool          quietPause_;
    bool          quietResume_;
    bool          actsOk_;
};

// combat_crowd (waiting-attacker stance validation): the HOST orders a CROWD of
// bar NPCs (up to 5) onto its own tab leader. Kenshi's AttackSlotManager grants
// only 1-2 of them an active attack slot; the rest hold the WAITING stance
// (circle/wait/hesitate sword states) - the exact case that used to teleport and
// AI-reset on the join (the 1.5 s clearGoals re-issue loop against slot-queued
// copies). The host streams TASK_COMBAT_WAIT for them (protocol 15); the join
// leaves their copies menacing in the ring. Both sides log SCENARIO MEMBER/RECV
// for every captured NPC at ~2 Hz. Test-CombatCrowd gates, per crowd hand: the
// join's [combat] order re-issue count (loop dead), the join's [combat] snap
// teleport count (artifact gone), host-vs-join position tracking, and - from the
// host MEMBER task series - that BOTH stances (active 0xFE00 / waiting 0xFE01)
// actually streamed (else the run proves nothing).
class CombatCrowdScenario : public Scenario {
public:
    CombatCrowdScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0), lastOrderMs_(0),
          haveOwn_(false), nStrikers_(0), nSeen_(0), issuedLogged_(false) {}

    virtual const char* name() const { return "combat_crowd"; }

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        const unsigned int ownRank = ctx.isHost ? 0u : 1u;
        if (!haveOwn_) latchLeader(ctx, ownRank);

        // HOST: pick the crowd once the window opens, then keep every striker
        // ordered on a throttle (orderAttackByHand no-ops while one is already
        // fighting, so the re-order never thrashes the host AI).
        if (ctx.isHost && haveOwn_ && ctx.elapsedMs >= COMBAT_AT_MS &&
            (ctx.elapsedMs - lastOrderMs_ >= 2500 || lastOrderMs_ == 0)) {
            lastOrderMs_ = ctx.elapsedMs;
            if (nStrikers_ == 0) pickCrowd(ctx);
            for (unsigned int i = 0; i < nStrikers_; ++i)
                engine::orderAttackByHand(ctx.gw, striker_[i], ownHand_);
            if (!issuedLogged_ && nStrikers_ > 0) {
                issuedLogged_ = true;
                char b[112];
                _snprintf(b, sizeof(b) - 1, "SCENARIO CROWD issued n=%u vic=%u,%u",
                          nStrikers_, ownHand_[3], ownHand_[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        // Both sides: the NPC position/task series the oracle compares.
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState npcs[MAX_LOG];
            unsigned int n = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
            const char* kind = ctx.isHost ? "MEMBER" : "RECV";
            for (unsigned int i = 0; i < n; ++i) logScenarioEntity(kind, npcs[i]);
            if (!ctx.isHost && n > 0) ++recvCount_;
            // JOIN: a driven combat copy leaves the interest capture when the
            // replicator detaches it into its own platoon (the capture's query
            // no longer enumerates it), which blinds the tracking series for
            // exactly the bodies under test. Remember every NPC hand ever
            // captured and keep logging the missing ones by direct resolve.
            if (!ctx.isHost) {
                for (unsigned int i = 0; i < n; ++i) rememberSeen(npcs[i]);
                for (unsigned int s = 0; s < nSeen_; ++s) {
                    bool inCap = false;
                    for (unsigned int i = 0; i < n; ++i)
                        if (npcs[i].hIndex == seen_[s].hIndex &&
                            npcs[i].hSerial == seen_[s].hSerial) { inCap = true; break; }
                    if (inCap) continue;
                    Character* c = engine::resolve(seen_[s]);
                    if (!c) continue;
                    EntityState e = seen_[s]; // original (host-frame) hand key
                    float x, y, z;
                    if (!engine::readPos(c, &x, &y, &z)) continue;
                    e.x = x; e.y = y; e.z = z;
                    e.task = TASK_NONE;
                    e.bodyState = engine::readBodyState(c);
                    logScenarioEntity("RECV", e);
                }
            }
            // HOST: raw combat-read diagnostic per striker (why did the stance
            // stream flicker?) - sword state + engaged flags + target presence.
            if (ctx.isHost) {
                for (unsigned int i = 0; i < nStrikers_; ++i) {
                    engine::CombatRead cr;
                    if (!engine::readCombatByHand(striker_[i], &cr)) continue;
                    char b[144];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO CROWDSTATE hand=%u,%u sw=%d mode=%d fight=%d "
                              "tgt=%d wait=%d",
                              striker_[i][3], striker_[i][4], cr.swordState,
                              cr.modeActive ? 1 : 0, cr.inCombat ? 1 : 0,
                              cr.hasTarget ? 1 : 0, cr.waiting ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? (nStrikers_ >= MIN_CROWD) : (recvCount_ >= 1);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    // Shared (peer-ready-armed) timeline; the crowd window runs 10 s -> end.
    static const unsigned long COMBAT_AT_MS     = 10000;
    static const unsigned long HOST_DURATION_MS = 75000;
    static const unsigned long JOIN_DURATION_MS = 68000;
    static const unsigned int  MAX_LOG      = 24;
    static const unsigned int  MAX_CROWD    = 5;
    static const unsigned int  MIN_CROWD    = 3;
    static const unsigned int  MAX_SQUAD    = 32;
    static const unsigned int  MAX_REMEMBER = 48;

    void latchLeader(const ScenarioContext& ctx, unsigned int ownRank) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int idx = tabLeaderIdx(sq, n, ownRank);
        if (idx < 0) return;
        handFromEntity(sq[idx], ownHand_);
        haveOwn_ = true;
        char b[112];
        _snprintf(b, sizeof(b) - 1, "SCENARIO CROWD own rank=%u hand=%u,%u",
                  ownRank, ownHand_[3], ownHand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void rememberSeen(const EntityState& e) {
        for (unsigned int s = 0; s < nSeen_; ++s)
            if (seen_[s].hIndex == e.hIndex && seen_[s].hSerial == e.hSerial) return;
        if (nSeen_ < MAX_REMEMBER) seen_[nSeen_++] = e;
    }

    // The crowd: the nearest upright, not-yet-fighting bar NPCs (the captured
    // set is already interest-sorted around the leaders).
    void pickCrowd(const ScenarioContext& ctx) {
        EntityState npcs[MAX_LOG];
        unsigned int n = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
        for (unsigned int i = 0; i < n && nStrikers_ < MAX_CROWD; ++i) {
            if (npcs[i].bodyState != 0) continue;           // down/dead/ragdoll
            if (taskIsCombat(npcs[i].task)) continue;       // already brawling
            handFromEntity(npcs[i], striker_[nStrikers_]);
            char b[96];
            _snprintf(b, sizeof(b) - 1, "SCENARIO CROWD striker=%u,%u",
                      striker_[nStrikers_][3], striker_[nStrikers_][4]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            ++nStrikers_;
        }
        if (nStrikers_ < MIN_CROWD)
            coop::logLine("SCENARIO CROWD pick FAILED (too few upright NPCs)");
    }

    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    unsigned long lastOrderMs_;
    bool          haveOwn_;
    unsigned int  nStrikers_;
    unsigned int  nSeen_;
    bool          issuedLogged_;
    unsigned int  ownHand_[5];
    unsigned int  striker_[MAX_CROWD][5];
    EntityState   seen_[MAX_REMEMBER]; // JOIN: hands ever captured (resolve fallback)
};

// spawn_probe / spawn_sync (runtime NPC spawn sync, protocol 21).
//
// Both scenarios run the SAME script; what differs is the plugin config:
//   * spawn_probe - spawnSync FORCED OFF (Config): baselines the two failure
//     modes with evidence. The join must log "[spawn] unresolved hand=..."
//     for the host's runtime spawns (failure mode 1: host-only enemies), and
//     the join's own local spawns must (or must not - that is the finding)
//     draw "[authority] suppress" lines (failure mode 2: join-only enemies).
//   * spawn_sync - spawnSync ON (default): the join must instead log
//     "[spawn] proxy BOUND" for the host's runtime spawns and the PROXY
//     position series must track the host's MEMBER series per hand.
//
// Script (host):  t=6s   spawn 4 runtime NPCs near the leader (leg=near)
//                 t=32s  teleport-park the rank-0 tab 600u away (the user's
//                        "roam far then get ambushed" reproduction)
//                 t=36s  spawn 4 more runtime NPCs there (leg=far)
// Script (join):  t=20s  spawn 4 runtime NPCs LOCALLY (the suppression leg)
//                        + log per-second JVIS visibility for those hands
// Both sides: MEMBER/RECV NPC series (npc_sync pattern) + 1 Hz CENSUS counts.
class SpawnSyncScenario : public Scenario {
public:
    SpawnSyncScenario(bool probe)
        : probe_(probe), passed_(false), lastLogMs_(0), lastCensusMs_(0),
          lastJvisMs_(0), nearSpawned_(false), farMoved_(false),
          farSpawned_(false), joinSpawned_(false),
          nNear_(0), nFar_(0), nJoin_(0),
          haveAnchor_(false), ax_(0), ay_(0), az_(0) {}

    virtual const char* name() const { return probe_ ? "spawn_probe" : "spawn_sync"; }

    virtual void onStart(const ScenarioContext& ctx) {
        Character* ld = engine::leader(ctx.gw);
        if (ld && engine::readPos(ld, &ax_, &ay_, &az_)) haveAnchor_ = true;
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SPAWNPROBE anchor=%.1f,%.1f,%.1f have=%d probe=%d",
                  ax_, ay_, az_, haveAnchor_ ? 1 : 0, probe_ ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Standard NPC series (npc_sync pattern): the host's spawned squad
        // appears in its MEMBER capture set; whether it appears in the join's
        // world at all is exactly what the oracles judge.
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState npcs[MAX_LOG];
            unsigned int n = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
            const char* kind = ctx.isHost ? "MEMBER" : "RECV";
            for (unsigned int i = 0; i < n; ++i) logScenarioEntity(kind, npcs[i]);
        }
        // 1 Hz world-NPC census on both sides: the coarse "does the join's
        // world hold as many bodies as the host's" spawn-frequency telemetry.
        if (ctx.elapsedMs - lastCensusMs_ >= 1000 || lastCensusMs_ == 0) {
            lastCensusMs_ = ctx.elapsedMs;
            static Character*  cc[MAX_CENSUS];
            static EntityState cs[MAX_CENSUS];
            unsigned int n = engine::listNpcs(ctx.gw, cc, cs, MAX_CENSUS);
            char b[96];
            _snprintf(b, sizeof(b) - 1, "SCENARIO CENSUS t=%lu n=%u", ctx.elapsedMs, n);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        if (ctx.isHost) {
            // Leg 1 (near): runtime squad beside the co-located leaders.
            if (!nearSpawned_ && ctx.elapsedMs >= NEAR_SPAWN_AT_MS) {
                nearSpawned_ = true;
                nNear_ = engine::spawnRuntimeSquad(ctx.gw, SQUAD_N, nearHands_);
                logSpawns("near", nearHands_, nNear_);
            }
            // Leg 2 (far): the user's reproduction - roam far, then meet a
            // runtime squad there. Teleport-park the whole rank-0 tab (the
            // split_interest pattern) so the move is deterministic.
            if (!farMoved_ && ctx.elapsedMs >= FAR_MOVE_AT_MS) {
                farMoved_ = true;
                parkRank0Far(ctx);
            }
            if (farMoved_ && !farSpawned_ && ctx.elapsedMs >= FAR_SPAWN_AT_MS) {
                farSpawned_ = true;
                nFar_ = engine::spawnRuntimeSquad(ctx.gw, SQUAD_N, farHands_);
                logSpawns("far", farHands_, nFar_);
            }
        } else {
            // Suppression leg: the JOIN's own engine spawns a runtime squad
            // (the join-only-enemies failure mode). enforceHostAuthority
            // should hide these within ~1 s; the [authority] suppress lines
            // (keyed hand=index,serial) are the oracle's anchor.
            if (!joinSpawned_ && ctx.elapsedMs >= JOIN_SPAWN_AT_MS) {
                joinSpawned_ = true;
                nJoin_ = engine::spawnRuntimeSquad(ctx.gw, SQUAD_N, joinHands_);
                logSpawns("join", joinHands_, nJoin_);
            }
            // Per-second visibility trace for the join-local spawns: do the
            // hands still resolve, and where are the bodies?
            if (joinSpawned_ && ctx.elapsedMs - lastJvisMs_ >= 1000) {
                lastJvisMs_ = ctx.elapsedMs;
                for (unsigned int i = 0; i < nJoin_; ++i) {
                    Character* c = engine::resolveCharByHand(
                        joinHands_[i][3], joinHands_[i][4], joinHands_[i][0],
                        joinHands_[i][1], joinHands_[i][2]);
                    float x = 0, y = 0, z = 0;
                    bool havePos = c && engine::readPos(c, &x, &y, &z);
                    char b[144];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO JVIS hand=%u,%u t=%lu res=%d pos=%.2f,%.2f,%.2f",
                              joinHands_[i][3], joinHands_[i][4], ctx.elapsedMs,
                              c ? 1 : 0, x, y, z);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    (void)havePos;
                }
            }
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            // The scenario verdict only asserts the SCRIPT ran (spawns
            // happened); the sync mechanism itself is judged by the
            // Test-SpawnProbe / Test-SpawnSync oracles over the log evidence.
            if (ctx.isHost) passed_ = (nNear_ > 0) && (nFar_ > 0);
            else            passed_ = (nJoin_ > 0);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    // One "SCENARIO SPAWN leg=<leg> hand=t,c,cs,i,s" line per spawned body.
    // Hand order matches readObjectHand (and the replicator's [spawn] logs),
    // so the oracle keys host spawns and join telemetry identically.
    void logSpawns(const char* leg, unsigned int (*hands)[5], unsigned int n) {
        for (unsigned int i = 0; i < n; ++i) {
            char b[144];
            _snprintf(b, sizeof(b) - 1, "SCENARIO SPAWN leg=%s hand=%u,%u,%u,%u,%u",
                      leg, hands[i][0], hands[i][1], hands[i][2], hands[i][3],
                      hands[i][4]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SPAWNED leg=%s n=%u", leg, n);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void parkRank0Far(const ScenarioContext& ctx) {
        if (!haveAnchor_) return;
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        unsigned int moved = 0;
        for (unsigned int i = 0; i < n; ++i) {
            if (tabRankOf(sq, n, i) != 0) continue;
            Character* c = engine::resolve(sq[i]);
            if (!c) continue;
            engine::park(c, ax_ + FAR_DIST + (float)moved * 3.0f, ay_, az_, 0.0f);
            ++moved;
        }
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SPAWNFAR moved=%u to=%.1f,%.1f,%.1f",
                  moved, ax_ + FAR_DIST, ay_, az_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    static const unsigned long NEAR_SPAWN_AT_MS = 6000;
    static const unsigned long JOIN_SPAWN_AT_MS = 20000;
    static const unsigned long FAR_MOVE_AT_MS   = 32000;
    static const unsigned long FAR_SPAWN_AT_MS  = 36000;
    static const unsigned long JOIN_DURATION_MS = 56000;
    static const unsigned long HOST_DURATION_MS = 70000;
    static const unsigned int  SQUAD_N          = 4;
    static const unsigned int  MAX_LOG          = 40;
    static const unsigned int  MAX_CENSUS       = 128;
    static const unsigned int  MAX_SQUAD        = 32;
    static const float         FAR_DIST; // rank-0 relocation distance (units)

    bool          probe_;
    bool          passed_;
    unsigned long lastLogMs_;
    unsigned long lastCensusMs_;
    unsigned long lastJvisMs_;
    bool          nearSpawned_, farMoved_, farSpawned_, joinSpawned_;
    unsigned int  nNear_, nFar_, nJoin_;
    unsigned int  nearHands_[4][5];
    unsigned int  farHands_[4][5];
    unsigned int  joinHands_[4][5];
    bool          haveAnchor_;
    float         ax_, ay_, az_;
};
const float SpawnSyncScenario::FAR_DIST = 600.0f;

// shop_probe (protocol 22 phase 0, probe tier): money + vendor-trading evidence.
//
// Kenshi facts under test (spikes 28-30): the wallet is per-Platoon (Ownerships::
// money - no global player wallet), vendors are ShopTrader RootObjects with
// save-stable hands, and Inventory::buyItem mutates vendor stock + wallet LOCALLY
// on one client only. Nothing about money is on the wire today.
//
// Script:
//   * both sides, 1 Hz: enumerate nearby vendors ("SCENARIO VENDOR hand=..
//     money=.. stock=.. thand=..") and read every squad tab's wallet
//     ("SCENARIO WALLET rank=.. money=..") - the divergence series.
//   * host t=10s / join t=22s: each side (1) SETS its OWNED tab's wallet to a
//     side-distinct sentinel via Ownerships::setMoney (host rank0=5000, join
//     rank1=7000) - validates the 1b apply primitive AND, on the peer's WALLET
//     series, decisively answers "does any wallet state cross today"; then
//     (2) attempts ONE programmatic Inventory::buyItem against the nearest
//     stocked vendor ([shop] BUY-BEFORE/AFTER evidence). Vendor inventories
//     are lazy (built on shop-open), so the stock is forced first.
// The verdict only asserts the script ran (wallet series + the scripted action
// logged on each side); what crossed is judged/recorded by Test-ShopProbe.
// The SAME script also runs as "money_sync" (probe=false): with the protocol
// 22 wallet channel LEFT ON, the sentinel writes must CROSS - each side's
// WALLET series must converge to the peer's sentinel. Test-MoneySync gates on
// that convergence (the shop_probe run gates only on the evidence existing).
class ShopProbeScenario : public Scenario {
public:
    explicit ShopProbeScenario(bool probe)
        : probe_(probe), passed_(false), lastEvidenceMs_(0), actDone_(false),
          buyRes_(-9), walletReads_(0), sawVendor_(false) {}

    virtual const char* name() const { return probe_ ? "shop_probe" : "money_sync"; }

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SHOPPROBE start host=%d probe=%d",
                  ctx.isHost ? 1 : 0, probe_ ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // 1 Hz evidence: vendor census + per-tab wallet series (both sides).
        // The money_sync leg skips the vendor census (wallet-only gate).
        if (ctx.elapsedMs - lastEvidenceMs_ >= 1000 || lastEvidenceMs_ == 0) {
            lastEvidenceMs_ = ctx.elapsedMs;
            if (probe_) logVendors(ctx);
            logWallets(ctx);
        }
        // One scripted action window per side, staggered so the logs separate
        // the host crossing window from the join one. Each side (1) RAISES the
        // wallet of the tab it OWNS via Ownerships::setMoney - the decisive
        // "does money cross today" divergence lever plus the 1b apply-primitive
        // validation - then (2) attempts the programmatic vendor purchase
        // (records the vendor-stock findings).
        unsigned long actAt = ctx.isHost ? HOST_ACT_AT_MS : JOIN_ACT_AT_MS;
        if (!actDone_ && ctx.elapsedMs >= actAt) {
            actDone_ = true;
            doWalletSet(ctx);
            if (probe_) doBuy(ctx);
        }
        if (ctx.elapsedMs >= DURATION_MS) {
            // Script-ran gate only: wallets were readable and both scripted
            // actions were logged (any result - the RESULTS are findings).
            passed_ = (walletReads_ > 0) && actDone_;
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    void logVendors(const ScenarioContext& ctx) {
        engine::VendorRead v[MAX_VENDORS];
        unsigned int n = engine::listVendorsNear(ctx.gw, v, MAX_VENDORS, VENDOR_RADIUS);
        for (unsigned int i = 0; i < n; ++i) {
            // Keyed by the trader CHARACTER's save-stable hand (thand) - the
            // ShopTrader wrapper's own serial is runtime-minted and differs
            // per client/run (run 103018 finding), so thand is what the oracle
            // uses to match vendors across the two logs.
            char b[288];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO VENDOR hand=%u,%u,%u,%u,%u money=%d stock=%d qty=%d "
                      "src=%d thand=%u,%u,%u,%u,%u sid='%s' t=%lu",
                      v[i].hand[0], v[i].hand[1], v[i].hand[2], v[i].hand[3],
                      v[i].hand[4], v[i].money, v[i].stock, v[i].qty, v[i].src,
                      v[i].traderHand[0], v[i].traderHand[1], v[i].traderHand[2],
                      v[i].traderHand[3], v[i].traderHand[4], v[i].sid, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (n > 0) sawVendor_ = true;
        char c[64];
        _snprintf(c, sizeof(c) - 1, "SCENARIO VENDORS n=%u t=%lu", n, ctx.elapsedMs);
        c[sizeof(c) - 1] = '\0'; coop::logLine(c);
    }

    // One WALLET line per distinct squad tab (keyed by RANK, the cross-client
    // stable tab identity): the money series the oracle diffs host-vs-join.
    void logWallets(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        for (unsigned int rank = 0; rank < 4; ++rank) {
            int li = tabLeaderIdx(sq, n, rank);
            if (li < 0) continue;
            unsigned int h[5];
            handFromEntity(sq[li], h);
            int money = -1;
            if (engine::readWalletByHand(h, &money)) ++walletReads_;
            char b[96];
            _snprintf(b, sizeof(b) - 1, "SCENARIO WALLET rank=%u money=%d t=%lu",
                      rank, money, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    // Wallet-write leg: set the OWNED tab's wallet to a side-distinct sentinel
    // via the engine accessor (host 5000, join 7000). Proves writeWallet works
    // (the 1b apply primitive) and, on the peer's WALLET series, whether ANY
    // wallet state crosses today (expected: it does not - the 1b gap evidence).
    void doWalletSet(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        unsigned int rank = ctx.isHost ? 0u : 1u;
        int li = tabLeaderIdx(sq, n, rank);
        if (li < 0) { li = tabLeaderIdx(sq, n, 0u); rank = 0u; }
        int before = -1, after = -1, ok = 0;
        int target = ctx.isHost ? 5000 : 7000;
        if (li >= 0) {
            unsigned int h[5];
            handFromEntity(sq[li], h);
            engine::readWalletByHand(h, &before);
            ok = engine::writeWalletByHand(h, target) ? 1 : 0;
            engine::readWalletByHand(h, &after);
        }
        char b[144];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO WALLETSET who=%s rank=%u target=%d ok=%d before=%d after=%d t=%lu",
                  ctx.isHost ? "host" : "join", rank, target, ok, before, after,
                  ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void doBuy(const ScenarioContext& ctx) {
        // Vendor inventories are LAZY (built when the trade UI first opens - run
        // 101952: every enumerated vendor read stock=-1), so force the engine's
        // own stock build on each candidate until one holds items, then re-read.
        engine::VendorRead v[MAX_VENDORS];
        unsigned int nv = engine::listVendorsNear(ctx.gw, v, MAX_VENDORS, VENDOR_RADIUS);
        int pick = -1;
        for (unsigned int i = 0; i < nv; ++i) {
            if (v[i].stock <= 0) {
                int r = engine::ensureVendorStock(ctx.gw, v[i].hand);
                char eb[112];
                _snprintf(eb, sizeof(eb) - 1, "SCENARIO SHOPSTOCK vendor=%u,%u ensure=%d",
                          v[i].hand[3], v[i].hand[4], r);
                eb[sizeof(eb) - 1] = '\0'; coop::logLine(eb);
            }
        }
        nv = engine::listVendorsNear(ctx.gw, v, MAX_VENDORS, VENDOR_RADIUS);
        for (unsigned int i = 0; i < nv; ++i)
            if (v[i].stock > 0) { pick = (int)i; break; }
        // Buyer = the tab THIS side owns (host rank 0, join rank 1; fall back to
        // rank 0 when the save has a single tab).
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int li = tabLeaderIdx(sq, n, ctx.isHost ? 0u : 1u);
        if (li < 0) li = tabLeaderIdx(sq, n, 0u);
        char sid[48]; sid[0] = '\0';
        if (pick < 0 || li < 0) {
            buyRes_ = -2; // no vendor / no buyer - recorded, judged by the oracle
        } else {
            unsigned int bh[5];
            handFromEntity(sq[li], bh);
            buyRes_ = engine::probeVendorBuy(ctx.gw, v[pick].hand, bh, sid, sizeof(sid));
        }
        char b[176];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO SHOPBUY who=%s res=%d sid='%s' vendor=%u,%u t=%lu",
                  ctx.isHost ? "host" : "join", buyRes_, sid,
                  pick >= 0 ? v[pick].hand[3] : 0u, pick >= 0 ? v[pick].hand[4] : 0u,
                  ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    static const unsigned long HOST_ACT_AT_MS = 10000;
    static const unsigned long JOIN_ACT_AT_MS = 22000;
    static const unsigned long DURATION_MS    = 40000;
    static const unsigned int  MAX_VENDORS    = 8;
    static const unsigned int  MAX_SQUAD      = 32;
    static const float         VENDOR_RADIUS;

    bool          probe_;
    bool          passed_;
    unsigned long lastEvidenceMs_;
    bool          actDone_;
    int           buyRes_;
    unsigned int  walletReads_;
    bool          sawVendor_;
};
const float ShopProbeScenario::VENDOR_RADIUS = 100.0f;

// vendor_trade (protocol 22 phase 1c): the buyer-side purchase COMPOSITE gate.
//
// A real Inventory::buyItem is unreachable in automation (vendor inventories
// are lazy and the test save's SHOP_TRADER_CLASS objects carry no bound trader
// - shop_probe runs 103018-104036), so the scenario performs the exact buyer-
// side mutations ONE purchase makes - a wallet debit and the bought item
// landing in the buyer's personal inventory, same tick - on the tab each side
// OWNS, and gates that BOTH effects converge on the peer through the two
// existing channels (PKT_MONEY + the bidirectional inventory snapshots). The
// VENDOR-side mutation (stock shrink, register cash) intentionally stays local
// for now: the engine regenerates vendor stock per client anyway, and the
// [shop] BUY-LOCAL detour is gathering the field evidence for that mirror.
//
// Script (mirrors money_sync's stagger):
//   * both sides, 1 Hz: every tab's WALLET line + a TINV line (count + content
//     hash) for every tab leader's personal container.
//   * host t=6s: seed its rank-0 wallet to 5000; t=10s: TRADE - one test item
//     into the rank-0 leader's inventory + wallet -= 250 (-> 4750).
//   * join t=18s/t=22s: same on its rank-1 tab with 7000 -> 6750.
class VendorTradeScenario : public Scenario {
public:
    VendorTradeScenario()
        : passed_(false), lastEvidenceMs_(0), seeded_(false), traded_(false),
          tradeOk_(false), walletReads_(0) {}

    virtual const char* name() const { return "vendor_trade"; }

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO VENDORTRADE start host=%d",
                  ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (ctx.elapsedMs - lastEvidenceMs_ >= 1000 || lastEvidenceMs_ == 0) {
            lastEvidenceMs_ = ctx.elapsedMs;
            logSeries(ctx);
        }
        unsigned long seedAt  = ctx.isHost ? HOST_SEED_AT_MS  : JOIN_SEED_AT_MS;
        unsigned long tradeAt = ctx.isHost ? HOST_TRADE_AT_MS : JOIN_TRADE_AT_MS;
        if (!seeded_ && ctx.elapsedMs >= seedAt)  { seeded_ = true; doSeed(ctx); }
        if (!traded_ && ctx.elapsedMs >= tradeAt) { traded_ = true; doTrade(ctx); }
        if (ctx.elapsedMs >= DURATION_MS) {
            passed_ = (walletReads_ > 0) && traded_ && tradeOk_;
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    // One WALLET + one TINV line per distinct squad tab (keyed by rank).
    void logSeries(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        for (unsigned int rank = 0; rank < 4; ++rank) {
            int li = tabLeaderIdx(sq, n, rank);
            if (li < 0) continue;
            unsigned int h[5];
            handFromEntity(sq[li], h);
            int money = -1;
            if (engine::readWalletByHand(h, &money)) ++walletReads_;
            char b[96];
            _snprintf(b, sizeof(b) - 1, "SCENARIO WALLET rank=%u money=%d t=%lu",
                      rank, money, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            InvItemEntry items[INV_ITEMS_MAX];
            unsigned int hash = 0;
            unsigned int cnt = engine::captureContainerContents(
                ctx.gw, h, items, INV_ITEMS_MAX, &hash);
            char c[112];
            _snprintf(c, sizeof(c) - 1, "SCENARIO TINV rank=%u count=%u hash=%u t=%lu",
                      rank, cnt, hash, ctx.elapsedMs);
            c[sizeof(c) - 1] = '\0'; coop::logLine(c);
        }
    }

    // The tab this side owns: host rank 0, join rank 1 (leader fallback).
    bool ownLeaderHand(const ScenarioContext& ctx, unsigned int h[5],
                       unsigned int* outRank) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        unsigned int rank = ctx.isHost ? 0u : 1u;
        int li = tabLeaderIdx(sq, n, rank);
        if (li < 0) { li = tabLeaderIdx(sq, n, 0u); rank = 0u; }
        if (li < 0) return false;
        handFromEntity(sq[li], h);
        if (outRank) *outRank = rank;
        return true;
    }

    void doSeed(const ScenarioContext& ctx) {
        unsigned int h[5]; unsigned int rank = 0;
        int ok = 0, target = ctx.isHost ? SEED_HOST : SEED_JOIN;
        if (ownLeaderHand(ctx, h, &rank))
            ok = engine::writeWalletByHand(h, target) ? 1 : 0;
        char b[112];
        _snprintf(b, sizeof(b) - 1, "SCENARIO TRADESEED who=%s rank=%u target=%d ok=%d t=%lu",
                  ctx.isHost ? "host" : "join", rank, target, ok, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void doTrade(const ScenarioContext& ctx) {
        unsigned int h[5]; unsigned int rank = 0;
        int w0 = -1, w1 = -1, added = 0;
        unsigned int cnt0 = 0, cnt1 = 0, hash = 0;
        char sid[48]; sid[0] = '\0';
        if (ownLeaderHand(ctx, h, &rank)) {
            InvItemEntry items[INV_ITEMS_MAX];
            engine::readWalletByHand(h, &w0);
            cnt0 = engine::captureContainerContents(ctx.gw, h, items, INV_ITEMS_MAX, &hash);
            // The two buyer-side mutations of one purchase, same tick.
            added = engine::addTestItemsToContainer(ctx.gw, h, 1, sid, sizeof(sid));
            if (w0 >= PRICE) engine::writeWalletByHand(h, w0 - PRICE);
            engine::readWalletByHand(h, &w1);
            cnt1 = engine::captureContainerContents(ctx.gw, h, items, INV_ITEMS_MAX, &hash);
        }
        tradeOk_ = (added > 0) && (w1 >= 0) && (w0 - w1 == PRICE);
        char b[208];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO TRADE who=%s rank=%u ok=%d sid='%s' price=%d "
                  "wBefore=%d wAfter=%d cntBefore=%u cntAfter=%u t=%lu",
                  ctx.isHost ? "host" : "join", rank, tradeOk_ ? 1 : 0, sid, PRICE,
                  w0, w1, cnt0, cnt1, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    static const unsigned long HOST_SEED_AT_MS  = 6000;
    static const unsigned long HOST_TRADE_AT_MS = 10000;
    static const unsigned long JOIN_SEED_AT_MS  = 18000;
    static const unsigned long JOIN_TRADE_AT_MS = 22000;
    static const unsigned long DURATION_MS      = 40000;
    static const unsigned int  MAX_SQUAD        = 32;
    static const int           SEED_HOST        = 5000;
    static const int           SEED_JOIN        = 7000;
    static const int           PRICE            = 250;

    bool          passed_;
    unsigned long lastEvidenceMs_;
    bool          seeded_;
    bool          traded_;
    bool          tradeOk_;
    unsigned int  walletReads_;
};

// recruit_probe (protocol 23 phase 0, probe tier): mid-session recruitment
// evidence. No recruit sync exists - a recruit exists on the recruiting client
// only - and the DESIGN questions are identity-shaped:
//   * does PlayerInterface::recruit work programmatically on both sides?
//   * what happens to the subject's HAND - the container MUST change (it moves
//     into a player platoon); do index/serial survive (peer could re-key) or
//     is the identity fully broken?
//   * does the recruit land in an EXISTING tab (rank stable) or mint a NEW
//     platoon (the sorted-container rank partition could RESHUFFLE mid-session
//     - the ownership hazard)?
//   * what does the PEER see? For the BAKED leg its copy of the subject still
//     stands (now unstreamed by the recruiter -> authority suppression?); the
//     recruiter streams an unresolvable new hand (spawn-sync REQ/proxy?). The
//     oracle cross-references the [spawn]/[authority] logs for both.
// Script: 1 Hz TABS census (distinct sorted containers + squad size) on both
// sides; host t=10s recruits the nearest BAKED world NPC, t=14s a RUNTIME
// spawn; join the same at t=22s/26s. Verdict gates only that the script ran
// (both legs logged + census series present); everything else is FINDINGs.
//
// The same script doubles as recruit_sync (probe=false, full tier): recruit
// sync stays ON (protocol 23) and every leg must actually SUCCEED locally
// (res=1); the Test-RecruitSync oracle then gates the cross-machine half from
// the logs - the peer re-keyed its local body to each recruited hand (baked
// legs, no duplicate proxy) or minted one via the bidirectional describe
// channel (runtime legs), and tracked it (SCENARIO PROXY series).
class RecruitProbeScenario : public Scenario {
public:
    explicit RecruitProbeScenario(bool probe)
        : probe_(probe), passed_(false), lastEvidenceMs_(0),
          bakedDone_(false), runtimeDone_(false),
          bakedRes_(-9), runtimeRes_(-9), tabsLogged_(0) {}

    virtual const char* name() const { return probe_ ? "recruit_probe" : "recruit_sync"; }

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO RECRUITPROBE start host=%d",
                  ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (ctx.elapsedMs - lastEvidenceMs_ >= 1000 || lastEvidenceMs_ == 0) {
            lastEvidenceMs_ = ctx.elapsedMs;
            logTabs(ctx);
        }
        unsigned long bakedAt   = ctx.isHost ? HOST_ACT_AT_MS : JOIN_ACT_AT_MS;
        unsigned long runtimeAt = bakedAt + 4000;
        if (!bakedDone_ && ctx.elapsedMs >= bakedAt) {
            bakedDone_ = true;
            bakedRes_ = doRecruit(ctx, /*runtime=*/false);
        }
        if (!runtimeDone_ && ctx.elapsedMs >= runtimeAt) {
            runtimeDone_ = true;
            runtimeRes_ = doRecruit(ctx, /*runtime=*/true);
        }
        if (ctx.elapsedMs >= DURATION_MS) {
            passed_ = bakedDone_ && runtimeDone_ && (tabsLogged_ > 0);
            // The gated variant requires the recruits to have actually happened
            // (the probe only requires the script to have run - failure IS data).
            if (!probe_) passed_ = passed_ && (bakedRes_ == 1) && (runtimeRes_ == 1);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    // Distinct sorted squad-tab containers + squad size: the rank-partition
    // census whose REORDERING mid-series is the ownership-reshuffle finding.
    void logTabs(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        std::vector<std::pair<unsigned int, unsigned int> > ctnrs;
        for (unsigned int i = 0; i < n; ++i)
            ctnrs.push_back(std::make_pair(sq[i].hContainer, sq[i].hContainerSerial));
        std::sort(ctnrs.begin(), ctnrs.end());
        ctnrs.erase(std::unique(ctnrs.begin(), ctnrs.end()), ctnrs.end());
        char list[128]; list[0] = '\0';
        unsigned int used = 0;
        for (unsigned int i = 0; i < ctnrs.size() && used + 24 < sizeof(list); ++i) {
            used += (unsigned int)_snprintf(list + used, sizeof(list) - used - 1,
                                            "%s%u:%u", i ? "|" : "",
                                            ctnrs[i].first, ctnrs[i].second);
        }
        list[sizeof(list) - 1] = '\0';
        char b[208];
        _snprintf(b, sizeof(b) - 1, "SCENARIO TABS n=%u squad=%u list=%s t=%lu",
                  (unsigned int)ctnrs.size(), n, list, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        ++tabsLogged_;
    }

    int doRecruit(const ScenarioContext& ctx, bool runtime) {
        unsigned int hb[5], ha[5];
        int res = engine::probeRecruit(ctx.gw, runtime, hb, ha);
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO RECRUIT who=%s leg=%s res=%d "
                  "before=%u,%u,%u,%u,%u after=%u,%u,%u,%u,%u t=%lu",
                  ctx.isHost ? "host" : "join", runtime ? "runtime" : "baked", res,
                  hb[0], hb[1], hb[2], hb[3], hb[4],
                  ha[0], ha[1], ha[2], ha[3], ha[4], ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return res;
    }

    static const unsigned long HOST_ACT_AT_MS = 10000;
    static const unsigned long JOIN_ACT_AT_MS = 22000;
    static const unsigned long DURATION_MS    = 40000;
    static const unsigned int  MAX_SQUAD      = 48;

    bool          probe_;
    bool          passed_;
    unsigned long lastEvidenceMs_;
    bool          bakedDone_;
    bool          runtimeDone_;
    int           bakedRes_;
    int           runtimeRes_;
    unsigned int  tabsLogged_;
};

Scenario* makeScenario(const std::string& name) {
    if (name == "spike")        return new SpikeScenario();
    if (name == "leader_move")  return new LeaderMoveScenario();
    if (name == "coop_presence") return new CoopPresenceScenario();
    if (name == "split_interest") return new SplitInterestScenario();
    if (name == "npc_sync")     return new NpcSyncScenario();
    if (name == "craft_order")  return new CraftOrderScenario();
    if (name == "down_order")   return new DownOrderScenario();
    if (name == "death_order")  return new DeathOrderScenario();
    if (name == "combat_probe") return new CombatProbeScenario();
    if (name == "combat_order") return new CombatOrderScenario();
    if (name == "combat_kill")  return new CombatKillScenario();
    if (name == "player_combat") return new PlayerCombatScenario();
    if (name == "player_ko")    return new PlayerKoScenario();
    if (name == "medic_order")  return new MedicOrderScenario();
    if (name == "limb_loss")    return new LimbLossScenario();
    if (name == "stats_sync")   return new StatsSyncScenario();
    if (name == "carry_order")  return new CarryOrderScenario();
    if (name == "speed_sync")   return new SpeedSyncScenario();
    if (name == "speed_probe")  return new SpeedProbeScenario();
    if (name == "combat_crowd") return new CombatCrowdScenario();
    if (name == "inv_order")    return new InventorySyncScenario();
    if (name == "inv_bidir")    return new InventoryBidirScenario();
    if (name == "inv_equip")    return new InventoryEquipScenario(/*reequip=*/false);
    if (name == "inv_reequip")  return new InventoryEquipScenario(/*reequip=*/true);
    if (name == "inv_wpnseq")   return new WeaponSeqScenario();
    if (name == "inv_addequip") return new InventoryAddEquipScenario();
    if (name == "drop_probe")   return new DropProbeScenario();
    if (name == "world_item_sync") return new WorldItemSyncScenario();
    if (name == "wpn_relocate") return new WeaponRelocateScenario();
    if (name == "npc_carry")    return new NpcCarryScenario();
    if (name == "world_weapon_drop") return new WorldGearDropScenario("world_weapon_drop", 2);
    if (name == "world_armor_drop")  return new WorldGearDropScenario("world_armor_drop", 3);
    if (name == "bed_pose")     return new BedPoseScenario();
    if (name == "bed_put")      return new FurnPutScenario(1);
    if (name == "cage_put")     return new FurnPutScenario(2);
    if (name == "sneak_probe")  return new SneakProbeScenario();
    if (name == "sneak_pose")   return new SneakPoseScenario();
    if (name == "sneak_detect") return new SneakDetectScenario();
    if (name == "spawn_probe")  return new SpawnSyncScenario(/*probe=*/true);
    if (name == "spawn_sync")   return new SpawnSyncScenario(/*probe=*/false);
    if (name == "shop_probe")   return new ShopProbeScenario(/*probe=*/true);
    if (name == "money_sync")   return new ShopProbeScenario(/*probe=*/false);
    if (name == "vendor_trade") return new VendorTradeScenario();
    if (name == "recruit_probe") return new RecruitProbeScenario(true);
    if (name == "recruit_sync")  return new RecruitProbeScenario(false);
    return 0;
}

} // namespace coop
