// ScenarioNpc.cpp - world-NPC streaming + spawn scenarios (monolith split
// from Scenario.cpp, 2026-07-12): npc_sync, craft_order, down_order,
// death_order, spawn_probe/spawn_sync, npc_census, spawn_far. Classes are
// TU-private (anonymous namespace); only makeNpcScenario (ScenarioSupport.h)
// is exported.
// Must NOT: change any SCENARIO log string (oracle API, resources/CODE_MAP.md).

#include "ScenarioSupport.h"

namespace coop {
namespace {


// npc_sync (Stage 4): the HOST streams nearby world NPCs (host-authoritative);
// the JOIN resolves each by hand and drives it (walk-drive while moving, park +
// AI-quiet at rest). Neither side scripts the NPCs - they do their own bar AI -
// so this just enumerates the same shared-save NPCs around each (co-located)
// leader and logs MEMBER (host, authoritative) / RECV (join, driven copy) per
// hand. The runner cross-checks positions per hand (ratio-based: stationary
// sitters match tightly, the occasional patroller may lag by interp/catch-up).
class NpcSyncScenario : public TimedScenario {
public:
    NpcSyncScenario() : TimedScenario("npc_sync", 0), recvCount_(0), lastLogMs_(0) {}

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

private:
    // 24/44 s -> 62/82 s (2026-07-10, same reasoning as leader_move): the
    // join's clock catch-up slews its sim at up to 2x for the first ~35-40 s,
    // and NPCs walk-driven from a 1x stream while the local sim runs 2x track
    // measurably worse (npc_track medians 5-8 u vs the 3 u tolerance, join
    // NPCs visibly fast-forwarded). Extending the window past convergence
    // makes the per-NPC MEDIANS the crosscheck judges steady-state-dominated.
    static const unsigned long HOST_DURATION_MS = 82000; // outlive the join's window
    static const unsigned long JOIN_DURATION_MS = 62000;
    static const unsigned int  MAX_LOG          = 40;     // cap NPCs logged per tick
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
class CraftOrderScenario : public TimedScenario {
public:
    CraftOrderScenario()
        : TimedScenario("craft_order", 0), recvCount_(0), lastLogMs_(0), orderLogged_(false),
          haveWorker_(false), task_(0), lastOrderMs_(0) {}

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
class DownOrderScenario : public TimedScenario {
public:
    DownOrderScenario()
        : TimedScenario("down_order", 0), recvCount_(0), lastLogMs_(0), downLogged_(false),
          haveSubject_(false), lastDownMs_(0) {}

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
class DeathOrderScenario : public TimedScenario {
public:
    DeathOrderScenario()
        : TimedScenario("death_order", 0), recvCount_(0), lastLogMs_(0), deathLogged_(false),
          haveSubject_(false), lastKillMs_(0) {}

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

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          deathLogged_;
    bool          haveSubject_;
    unsigned int  subjHand_[5];
    unsigned long lastKillMs_;
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
class SpawnSyncScenario : public TimedScenario {
public:
    SpawnSyncScenario(bool probe)
        : TimedScenario(probe ? "spawn_probe" : "spawn_sync", 0),
          probe_(probe), lastLogMs_(0), lastCensusMs_(0),
          lastJvisMs_(0), nearSpawned_(false), farMoved_(false),
          farSpawned_(false), joinSpawned_(false),
          nNear_(0), nFar_(0), nJoin_(0),
          haveAnchor_(false), ax_(0), ay_(0), az_(0) {}

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

// npc_census (protocol 36): wide-radius ghost culling. The 2026-07-09 field
// report: the join saw NPCs the host didn't have until they wandered within
// the ~200 u stream bubble - existence culling only reached as far as the
// positional stream. The census channel (host 1 Hz wide-radius hand list +
// the join's wide suppression pass) must now cull a join-only ghost at
// census range, far beyond the bubble.
//
// Script (join): t=10s spawn 4 runtime NPCs locally, then PARK them ~600 u
//                from the anchor - outside the 200 u stream bubble, inside
//                the 2000 u census radius. The near-pass can never judge
//                them; only the census pass can ("[census] cull" lines).
//                + per-second GVIS resolve/position trace for those hands.
// Script (host): idle (its census stream is the mechanism under test;
//                "[census] sent" lines prove the channel is live).
// The Test-NpcCensus oracle gates: census flowing both ends, every ghost
// hand culled, and no mass-suppression of legitimate census NPCs.
class NpcCensusScenario : public TimedScenario {
public:
    NpcCensusScenario()
        : TimedScenario("npc_census", 0), spawned_(false), lastGvisMs_(0), nGhost_(0),
          haveAnchor_(false), ax_(0), ay_(0), az_(0) {}

    virtual void onStart(const ScenarioContext& ctx) {
        Character* ld = engine::leader(ctx.gw);
        if (ld && engine::readPos(ld, &ax_, &ay_, &az_)) haveAnchor_ = true;
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO NPCCENSUS anchor=%.1f,%.1f,%.1f have=%d",
                  ax_, ay_, az_, haveAnchor_ ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!ctx.isHost) {
            if (!spawned_ && ctx.elapsedMs >= SPAWN_AT_MS) {
                spawned_ = true;
                nGhost_ = engine::spawnRuntimeSquad(ctx.gw, GHOST_N, ghostHands_);
                // Park each ghost far from the anchor: beyond the stream
                // bubble (so the near suppression pass can never reach it),
                // inside the census radius (so the wide pass must).
                unsigned int parked = 0;
                for (unsigned int i = 0; i < nGhost_; ++i) {
                    Character* c = engine::resolveCharByHand(
                        ghostHands_[i][3], ghostHands_[i][4], ghostHands_[i][0],
                        ghostHands_[i][1], ghostHands_[i][2]);
                    if (!c) continue;
                    engine::park(c, ax_ + GHOST_DIST + (float)i * 3.0f, ay_, az_, 0.0f);
                    ++parked;
                }
                for (unsigned int i = 0; i < nGhost_; ++i) {
                    char b[144];
                    _snprintf(b, sizeof(b) - 1, "SCENARIO SPAWN leg=ghost hand=%u,%u,%u,%u,%u",
                              ghostHands_[i][0], ghostHands_[i][1], ghostHands_[i][2],
                              ghostHands_[i][3], ghostHands_[i][4]);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
                char b[112];
                _snprintf(b, sizeof(b) - 1, "SCENARIO SPAWNED leg=ghost n=%u parked=%u dist=%.0f",
                          nGhost_, parked, GHOST_DIST);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            // Per-second resolve/position trace for the ghosts (the cull is
            // hide+freeze, so the hand still resolves - the [census] cull
            // lines carry the verdict; this series is the diagnostic).
            if (spawned_ && ctx.elapsedMs - lastGvisMs_ >= 1000) {
                lastGvisMs_ = ctx.elapsedMs;
                for (unsigned int i = 0; i < nGhost_; ++i) {
                    Character* c = engine::resolveCharByHand(
                        ghostHands_[i][3], ghostHands_[i][4], ghostHands_[i][0],
                        ghostHands_[i][1], ghostHands_[i][2]);
                    float x = 0, y = 0, z = 0;
                    if (c) engine::readPos(c, &x, &y, &z);
                    char b[144];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO GVIS hand=%u,%u t=%lu res=%d pos=%.2f,%.2f,%.2f",
                              ghostHands_[i][3], ghostHands_[i][4], ctx.elapsedMs,
                              c ? 1 : 0, x, y, z);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
        }
        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            passed_ = ctx.isHost ? true : (nGhost_ > 0);
            return true;
        }
        return false;
    }

private:
    static const unsigned long SPAWN_AT_MS     = 10000;
    static const unsigned long JOIN_DURATION_MS = 45000;
    static const unsigned long HOST_DURATION_MS = 55000;
    static const unsigned int  GHOST_N          = 4;
    static const float         GHOST_DIST; // ghost park distance (units)

    bool          spawned_;
    unsigned long lastGvisMs_;
    unsigned int  nGhost_;
    unsigned int  ghostHands_[GHOST_N][5];
    bool          haveAnchor_;
    float         ax_, ay_, az_;
};
const float NpcCensusScenario::GHOST_DIST = 600.0f;

// spawn_far (2026-07-11 "NPCs spawn on top of the join player" fix): census-
// range proxy minting. Host runtime spawns used to reach the join only via
// the ~200 u stream bubble + the 250 u spawn-REQ proximity gate, so a raid
// walking in from afar materialized at arm's length. With the census-missing
// scan + reply-side mint gate (KENSHICOOP_SPAWN_MINT_RADIUS, 600 u default)
// the join must mint the proxies while the squad is still FAR out and let
// them walk in. (spawn_sync's far leg teleports the PLAYERS to the spawn, so
// it never exercises approach-from-afar.)
//
// Script (host): t=6s spawn 4 runtime NPCs, PARK them ~620 u from the anchor
//                (just outside the mint radius, so the far-defer/retry path
//                is exercised too), then order them to WALK back toward the
//                anchor (re-issued every 5 s until close).
// Script (join): idle; the replicator's [spawn] census-missing / REQ / INFO
//                deferred (far) / proxy BOUND lines plus the ~2 Hz SCENARIO
//                PROXY series are the evidence.
// Both sides log their leader anchor ("SCENARIO FARBIND anchor=..") and the
// standard MEMBER/RECV NPC series. Test-SpawnFarBind gates: every far hand
// binds exactly once (no duplicate mints) and while still >= 400 u from the
// join anchor (no more on-top materialization).
class SpawnFarScenario : public TimedScenario {
public:
    SpawnFarScenario()
        : TimedScenario("spawn_far", 0), lastLogMs_(0), lastOrderMs_(0),
          spawned_(false), nFar_(0),
          haveAnchor_(false), ax_(0), ay_(0), az_(0) {}

    virtual void onStart(const ScenarioContext& ctx) {
        Character* ld = engine::leader(ctx.gw);
        if (ld && engine::readPos(ld, &ax_, &ay_, &az_)) haveAnchor_ = true;
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO FARBIND anchor=%.1f,%.1f,%.1f have=%d",
                  ax_, ay_, az_, haveAnchor_ ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Standard NPC series: the host's MEMBER set picks the squad up when
        // it re-enters the capture bubble; the join's RECV set shows the
        // proxies once driven.
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            EntityState npcs[MAX_LOG];
            unsigned int n = engine::captureNpcs(ctx.gw, npcs, MAX_LOG);
            const char* kind = ctx.isHost ? "MEMBER" : "RECV";
            for (unsigned int i = 0; i < n; ++i) logScenarioEntity(kind, npcs[i]);
        }

        if (ctx.isHost) {
            if (!spawned_ && haveAnchor_ && ctx.elapsedMs >= SPAWN_AT_MS) {
                spawned_ = true;
                nFar_ = engine::spawnRuntimeSquad(ctx.gw, SQUAD_N, farHands_);
                unsigned int parked = 0;
                for (unsigned int i = 0; i < nFar_; ++i) {
                    Character* c = engine::resolveCharByHand(
                        farHands_[i][3], farHands_[i][4], farHands_[i][0],
                        farHands_[i][1], farHands_[i][2]);
                    if (!c) continue;
                    engine::park(c, ax_ + SPAWN_DIST + (float)i * 4.0f, ay_, az_, 0.0f);
                    ++parked;
                }
                for (unsigned int i = 0; i < nFar_; ++i) {
                    char b[144];
                    _snprintf(b, sizeof(b) - 1, "SCENARIO SPAWN leg=far hand=%u,%u,%u,%u,%u",
                              farHands_[i][0], farHands_[i][1], farHands_[i][2],
                              farHands_[i][3], farHands_[i][4]);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
                char b[128];
                _snprintf(b, sizeof(b) - 1, "SCENARIO SPAWNED leg=far n=%u parked=%u dist=%.0f",
                          nFar_, parked, SPAWN_DIST);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            // March the squad toward the anchor; re-issue on a slow cadence
            // (the walk order holds between issues; per-frame re-issue would
            // path-restart-stutter, and the squad is detached from town AI).
            if (spawned_ && haveAnchor_ &&
                (ctx.elapsedMs - lastOrderMs_) >= ORDER_EVERY_MS) {
                lastOrderMs_ = ctx.elapsedMs;
                for (unsigned int i = 0; i < nFar_; ++i) {
                    Character* c = engine::resolveCharByHand(
                        farHands_[i][3], farHands_[i][4], farHands_[i][0],
                        farHands_[i][1], farHands_[i][2]);
                    if (!c) continue;
                    float x = 0, y = 0, z = 0;
                    if (!engine::readPos(c, &x, &y, &z)) continue;
                    float dx = x - ax_, dy = y - ay_, dz = z - az_;
                    if (dx * dx + dy * dy + dz * dz < ARRIVE_DIST * ARRIVE_DIST)
                        continue; // arrived: let it idle beside the players
                    engine::walkTo(c, ax_ + (float)i * 2.0f, ay_, az_, WALK_SPEED);
                }
            }
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            // Script-ran verdict only; the bind evidence is judged by the
            // Test-SpawnFarBind oracle over the paired logs.
            passed_ = ctx.isHost ? (nFar_ > 0) : true;
            return true;
        }
        return false;
    }

private:
    static const unsigned long SPAWN_AT_MS      = 6000;
    static const unsigned long ORDER_EVERY_MS   = 5000;
    // Walk-in budget: 620 u at 14 u/s ~ 44 s + 6 s spawn + mint latency.
    // The harness kill deadline is KillGraceSec (90 s loopback) from the
    // post-screenshot mark (~arm + 5 s), so the host must self-exit < ~95 s
    // after arm or it gets force-killed before logging RESULT.
    static const unsigned long JOIN_DURATION_MS = 75000;
    static const unsigned long HOST_DURATION_MS = 85000;
    static const unsigned int  SQUAD_N          = 4;
    static const unsigned int  MAX_LOG          = 40;
    static const float         SPAWN_DIST;  // park distance from anchor (units)
    static const float         WALK_SPEED;  // commanded approach speed (u/s)
    static const float         ARRIVE_DIST; // stop re-ordering inside this

    unsigned long lastLogMs_;
    unsigned long lastOrderMs_;
    bool          spawned_;
    unsigned int  nFar_;
    unsigned int  farHands_[SQUAD_N][5];
    bool          haveAnchor_;
    float         ax_, ay_, az_;
};

const float SpawnFarScenario::SPAWN_DIST  = 620.0f;
const float SpawnFarScenario::WALK_SPEED  = 14.0f;
const float SpawnFarScenario::ARRIVE_DIST = 30.0f;

// world_parity: full-roster cross-client parity soak on a dense save (camp).
// Nothing is scripted - both sides run their normal sims while the
// replicator's auditRows dumps (SCENARIO WORLD/WNPC, 5 s cadence, carrying
// the task=/pelvis=/mv= parity fields and the cls=pc player rows) feed
// Test-WorldParity, which tier-judges every host row against the join:
// PCs hard position gate, near tier existence+pos+task, census band
// existence+pos. The scenario verdict is script-ran only; parity is the
// oracle's job. Host outlives the join (stale streams would poison the
// tail samples, same reasoning as npc_sync).
class WorldParityScenario : public Scenario {
public:
    // Name-parameterized so the same passive-soak body backs the gated
    // world_parity parity run, the ungated jail_probe diagnostic spike, and the
    // long-play jail_soak (spike 58). world_parity/jail_probe use the tuned
    // ~160-180 s window; jail_soak runs ~10 min so the guard put-to-work
    // cage<->pole cycle and census drift actually accumulate.
    explicit WorldParityScenario(const char* name) : name_(name), passed_(false) {
        if (std::string(name) == "jail_soak") {
            hostDur_ = 570000; joinDur_ = 560000;   // long-play soak (~9.5 min)
        } else {
            hostDur_ = 180000; joinDur_ = 160000;   // parity/spike window
        }
    }

    virtual const char* name() const { return name_; }

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        unsigned long dur = ctx.isHost ? hostDur_ : joinDur_;
        if (ctx.elapsedMs >= dur) { passed_ = true; return true; }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    // ~160 s of joined observation: long enough past the join's clock
    // catch-up (~40 s) that steady-state dominates the 5 s dump series
    // (~24 aligned samples). jail_soak overrides these in the ctor.
    unsigned long hostDur_;
    unsigned long joinDur_;
    const char* name_;
    bool passed_;
};

} // namespace

Scenario* makeNpcScenario(const std::string& name) {
    if (name == "npc_sync")     return new NpcSyncScenario();
    if (name == "craft_order")  return new CraftOrderScenario();
    if (name == "down_order")   return new DownOrderScenario();
    if (name == "death_order")  return new DeathOrderScenario();
    if (name == "spawn_probe")  return new SpawnSyncScenario(/*probe=*/true);
    if (name == "spawn_sync")   return new SpawnSyncScenario(/*probe=*/false);
    if (name == "npc_census")   return new NpcCensusScenario();
    if (name == "spawn_far")    return new SpawnFarScenario();
    if (name == "world_parity") return new WorldParityScenario("world_parity");
    if (name == "jail_probe")   return new WorldParityScenario("jail_probe");
    if (name == "jail_soak")    return new WorldParityScenario("jail_soak");
    return 0;
}

} // namespace coop
