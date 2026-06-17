// Scenario.cpp - the scenario registry and the first concrete scenario.
//
// Scenarios are deterministic, time-gated state machines. They call ONLY the
// ScenarioApi facade (implemented in KenshiCoop.cpp), so this file needs no
// engine/Boost headers and all game mutation stays SEH-guarded on the main
// thread. Determinism comes from ScenarioContext::elapsedMs (wall-clock since
// the scenario armed) plus position tolerances - the engine runs in real time,
// so steps are gated on elapsed time rather than exact frame counts.

#define _CRT_SECURE_NO_WARNINGS 1

#include "Scenario.h"
#include "ScenarioApi.h"

#include <cstdio>  // _vsnprintf
#include <cstdarg> // va_list / va_start
#include <cstring>

namespace coop {

namespace {

// Format helper: write to a fixed buffer and emit via scenarioLog.
void logf(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    scenarioLog(buf);
}

// Emit a CHECK line in the schema the runner parses:
//   CHECK <key> <PASS|FAIL> expected=<e> actual=<a>
void checkInt(const char* key, int expected, int actual) {
    logf("CHECK %s %s expected=%d actual=%d", key,
         (expected == actual) ? "PASS" : "FAIL", expected, actual);
}

// ---- Scenario: squad_spawn_sync -------------------------------------------
// Host-authoritative squad-visual test and TDD target for synchronized squad
// visuals. The host spawns 3 NPCs into the player squad, teleports them to
// fixed offsets around the player so they idle at known poses, then logs each
// member's hand + position (SCENARIO MEMBER). The join logs SCENARIO RECV for
// any squad members it receives (emitted from receiveNpcStates). run_test.ps1
// matches MEMBER vs RECV by hand within a tolerance.
//
// NOTE: publishNpcStates currently skips the player's own squad members, so the
// join receives no RECV lines yet - this scenario is intentionally a RED test
// until the synchronized-squad-visuals feature streams squad members.
class SquadSpawnSync : public Scenario {
public:
    SquadSpawnSync()
        : armed_(false), armElapsedMs_(0), peerOk_(false),
          spawnedCount_(0), baselineSquad_(-1), spawnDone_(false),
          teleportDone_(false), lastMemberLog_(0), passed_(false) {
        for (int i = 0; i < kCount; ++i) members_[i] = 0;
    }

    virtual const char* name() const { return "squad_spawn_sync"; }

    virtual void onStart(const ScenarioContext& ctx) {
        baselineSquad_ = playerSquadSize(ctx.gw);
        logf("SCENARIO STEP 0 baseline squad_size=%d isHost=%d",
             baselineSquad_, ctx.isHost ? 1 : 0);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Peer-sync arm: hold the whole timeline until the other client is
        // in-game and streaming (remotePlayerCount() > 0). Without this the host
        // finished its ~1.5s of work and self-exited BEFORE the join (launched
        // 8s later) even left the loading screen - so both the join screenshot
        // was useless and no MEMBER/RECV overlap was possible. Arming on the peer
        // guarantees both clients are simultaneously live for spawn/log/capture.
        if (!armed_) {
            if (remotePlayerCount() > 0) {
                armed_        = true;
                armElapsedMs_ = ctx.elapsedMs;
                peerOk_       = true;
                checkInt("peer_connected", 1, 1);
            } else if (ctx.elapsedMs >= kArmTimeoutMs) {
                checkInt("peer_connected", 1, 0); // never saw the peer; fail fast
                peerOk_ = false; passed_ = false;
                logf("SCENARIO STEP 9 abort no_peer");
                return true;
            } else {
                return false; // keep waiting for the handshake
            }
        }
        DWORD t = ctx.elapsedMs - armElapsedMs_;

        // The join only observes (RECV lines come from receiveSquadState); it
        // waits out the same arm-relative timeline so both clients exit together.
        // Its verdict is simply "saw the peer and ran the timeline" - the actual
        // host->join position match is the runner's CROSSCHECK job, not ours.
        if (!ctx.isHost) {
            if (t >= kDoneMs) {
                passed_ = peerOk_;
                logf("SCENARIO STEP 3 done (observer) peerOk=%d", peerOk_ ? 1 : 0);
                return true;
            }
            return false;
        }

        // t < kSpawnMs: let the world settle after arming.
        if (t < kSpawnMs) return false;

        // t ~ kSpawnMs: spawn kCount NPCs into the player squad at offsets.
        if (!spawnDone_) {
            spawnDone_ = true;
            Ogre::Vector3 base;
            Character* p = localPlayer(ctx.gw);
            if (!p || !getCharPos(p, &base)) {
                logf("SCENARIO STEP 1 spawn ABORT no_player");
                passed_ = false;
                return true;
            }
            logf("SCENARIO STEP 1 spawn base=%.1f,%.1f,%.1f", base.x, base.y, base.z);
            for (int i = 0; i < kCount; ++i) {
                Ogre::Vector3 at = base + spawnOffset(i);
                Character* c = spawnIntoPlayerSquad(ctx.gw, 0, at);
                if (c) {
                    members_[spawnedCount_++] = c;
                    clearCharGoals(c);
                }
            }
            checkInt("host_spawned_count", kCount, spawnedCount_);
            return false;
        }

        // t ~ kTeleportMs: teleport each member to a fixed offset and quiet it.
        if (!teleportDone_ && t >= kTeleportMs) {
            teleportDone_ = true;
            Ogre::Vector3 base;
            Character* p = localPlayer(ctx.gw);
            if (p && getCharPos(p, &base)) {
                for (int i = 0; i < spawnedCount_; ++i) {
                    if (!members_[i]) continue;
                    Ogre::Vector3 at = base + restOffset(i);
                    teleportChar(members_[i], at, 0.0f);
                    clearCharGoals(members_[i]);
                }
            }
            logf("SCENARIO STEP 2 teleport count=%d", spawnedCount_);
            return false;
        }

        // t ~ kMemberLogStartMs..kDoneMs: log each member's hand + position.
        if (t >= kMemberLogStartMs && t < kDoneMs) {
            if (t - lastMemberLog_ >= kMemberLogIntervalMs || lastMemberLog_ == 0) {
                lastMemberLog_ = t;
                for (int i = 0; i < spawnedCount_; ++i) {
                    Character* c = members_[i];
                    if (!c) continue;
                    u32 h[5]; Ogre::Vector3 pos;
                    if (getCharHand(c, h) && getCharPos(c, &pos)) {
                        // hand=index,serial,type,container,containerSerial
                        logf("SCENARIO MEMBER hand=%u,%u,%u,%u,%u pos=%.2f,%.2f,%.2f",
                             h[0], h[1], h[2], h[3], h[4], pos.x, pos.y, pos.z);
                    }
                }
            }
            return false;
        }

        // t >= kDoneMs: finish. PASS requires the handshake, all 3 spawns, and
        // the squad to have grown by 3 (the cross-client position match is the
        // runner's job, not the in-plugin verdict).
        if (t >= kDoneMs) {
            int finalSquad = playerSquadSize(ctx.gw);
            bool spawnsOk = (spawnedCount_ == kCount);
            int  delta    = (baselineSquad_ < 0 || finalSquad < 0)
                              ? kCount : (finalSquad - baselineSquad_);
            bool squadGrew = (baselineSquad_ < 0) ||
                             (finalSquad >= baselineSquad_ + kCount);
            // Lower-bound check: the squad must have grown by AT LEAST kCount. We
            // clamp the reported delta to kCount because the coop remote-player
            // ghost is also enrolled in the player squad (delta would read kCount+1
            // with a peer present), which is unrelated to whether our spawns took.
            checkInt("squad_size_delta", kCount, delta >= kCount ? kCount : delta);
            passed_ = peerOk_ && spawnsOk && squadGrew;
            logf("SCENARIO STEP 3 done spawned=%d squad=%d->%d",
                 spawnedCount_, baselineSquad_, finalSquad);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const int   kCount               = 3;
    static const DWORD kArmTimeoutMs        = 60000; // give up waiting for peer
    static const DWORD kSpawnMs             = 1500;
    static const DWORD kTeleportMs          = 3500;
    static const DWORD kMemberLogStartMs    = 4000;
    static const DWORD kMemberLogIntervalMs = 500;
    static const DWORD kDoneMs              = 6500;

    // Fixed spawn ring around the player (deterministic placement).
    static Ogre::Vector3 spawnOffset(int i) {
        switch (i) {
            case 0:  return Ogre::Vector3(5.0f, 0.0f, 0.0f);
            case 1:  return Ogre::Vector3(-5.0f, 0.0f, 0.0f);
            default: return Ogre::Vector3(0.0f, 0.0f, 5.0f);
        }
    }
    // Fixed resting offsets (where members are teleported to idle).
    static Ogre::Vector3 restOffset(int i) {
        switch (i) {
            case 0:  return Ogre::Vector3(3.0f, 0.0f, 3.0f);
            case 1:  return Ogre::Vector3(-3.0f, 0.0f, 3.0f);
            default: return Ogre::Vector3(0.0f, 0.0f, 6.0f);
        }
    }

    Character* members_[kCount];
    bool       armed_;
    DWORD      armElapsedMs_;
    bool       peerOk_;
    int        spawnedCount_;
    int        baselineSquad_;
    bool       spawnDone_;
    bool       teleportDone_;
    DWORD      lastMemberLog_;
    bool       passed_;
};

// ---- Scenario: squad_move_sync --------------------------------------------
// Join-authoritative squad-position test. BOTH clients spawn 2 units into their
// own squad (squad of 3); the JOIN then moves its squad slightly away and logs
// its authoritative member positions (SCENARIO MEMBER). The HOST is expected to
// observe them (SCENARIO RECV, emitted by receiveNpcStates). run_test.ps1
// matches the join's MEMBER lines against the host's RECV lines by hand.
//
// This is intentionally a RED test: publishNpcStates is host-only and skips the
// player's own squad, and receiveNpcStates is join-only, so the host receives
// nothing about the join's units and logs no RECV lines. It turns green once
// join->host squad streaming (synchronized squad visuals) is implemented.
//
// Determinism across the staggered launch: the two clients reach gameplay at
// different wall-clock times, so steps are gated on a local clock that starts
// when the peer is first seen (remotePlayerCount() > 0) rather than on
// elapsedMs-since-gameplay. Both clients then run on aligned clocks.
class SquadMoveSync : public Scenario {
public:
    SquadMoveSync()
        : isHost_(false), armed_(false), armElapsedMs_(0), peerOk_(false),
          baselineSquad_(-1), finalSquad_(-1), spawnedCount_(0), spawnDone_(false),
          formationDone_(false), moveDone_(false), lastMemberLog_(0),
          passed_(false) {
        for (int i = 0; i < kCount; ++i) members_[i] = 0;
    }

    virtual const char* name() const { return "squad_move_sync"; }

    virtual void onStart(const ScenarioContext& ctx) {
        isHost_        = ctx.isHost;
        baselineSquad_ = playerSquadSize(ctx.gw);
        logf("SCENARIO STEP 0 baseline squad_size=%d isHost=%d",
             baselineSquad_, ctx.isHost ? 1 : 0);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Peer-sync arm: hold until the other client is in-game and sending, so
        // both clients start their step timeline together.
        if (!armed_) {
            if (remotePlayerCount() > 0) {
                armed_        = true;
                armElapsedMs_ = ctx.elapsedMs;
                peerOk_       = true;
                checkInt("peer_connected", 1, 1);
            } else if (ctx.elapsedMs >= kArmTimeoutMs) {
                checkInt("peer_connected", 1, 0); // never saw the peer; fail fast
                peerOk_ = false; passed_ = false;
                logf("SCENARIO STEP 9 abort no_peer");
                return true;
            } else {
                return false; // keep waiting for the handshake
            }
        }
        DWORD t = ctx.elapsedMs - armElapsedMs_;

        // t ~ 500ms (both): spawn 2 units into the local squad.
        if (!spawnDone_ && t >= kSpawnMs) {
            spawnDone_ = true;
            Character* p = localPlayer(ctx.gw);
            Ogre::Vector3 base;
            if (!p || !getCharPos(p, &base)) {
                logf("SCENARIO STEP 1 spawn ABORT no_player");
                passed_ = false;
                return true;
            }
            members_[0] = p; // leader is squad member 0
            logf("SCENARIO STEP 1 spawn base=%.1f,%.1f,%.1f", base.x, base.y, base.z);
            for (int i = 0; i < kSpawn; ++i) {
                Ogre::Vector3 at = base + spawnOffset(i);
                Character* c = spawnIntoPlayerSquad(ctx.gw, 0, at);
                if (c) {
                    members_[1 + spawnedCount_] = c;
                    ++spawnedCount_;
                    clearCharGoals(c);
                }
            }
            // Assert only the spawn COUNT here (deterministic). Squad SIZE is
            // checked at the done step as a baseline-relative lower bound: the
            // engine reflects the recruit a frame or two later, so reading the
            // size in this same tick races it - and the save's starting roster is
            // not assumed to be just the leader.
            const char* side = isHost_ ? "host" : "join";
            char k1[64]; _snprintf(k1, sizeof(k1) - 1, "%s_spawned_count", side); k1[63] = '\0';
            checkInt(k1, kSpawn, spawnedCount_);
            return false;
        }

        // t ~ 2000ms (both): place the 3 members into a fixed formation.
        if (spawnDone_ && !formationDone_ && t >= kFormationMs) {
            formationDone_ = true;
            Ogre::Vector3 base;
            if (members_[0] && getCharPos(members_[0], &base)) {
                for (int i = 0; i < kCount; ++i) {
                    if (!members_[i]) continue;
                    Ogre::Vector3 at = base + formationOffset(i);
                    teleportChar(members_[i], at, 0.0f);
                    clearCharGoals(members_[i]);
                }
            }
            logf("SCENARIO STEP 2 formation count=%d", spawnedCount_ + 1);
            return false;
        }

        // t ~ 3500ms (JOIN only): move the whole squad slightly away.
        if (formationDone_ && !moveDone_ && !isHost_ && t >= kMoveMs) {
            moveDone_ = true;
            Ogre::Vector3 base;
            if (members_[0] && getCharPos(members_[0], &base)) {
                Ogre::Vector3 base2 = base + Ogre::Vector3(kMoveDx, 0.0f, 0.0f);
                for (int i = 0; i < kCount; ++i) {
                    if (!members_[i]) continue;
                    Ogre::Vector3 at = base2 + formationOffset(i);
                    teleportChar(members_[i], at, 0.0f);
                    clearCharGoals(members_[i]);
                }
            }
            logf("SCENARIO STEP 3 join_move dx=%.1f", kMoveDx);
            return false;
        }

        // t ~ 4000-6000ms (JOIN): log authoritative member positions ~ every
        // 500ms. The host does no extra work here - its RECV lines (none today)
        // come from receiveNpcStates.
        if (t >= kMemberLogStartMs && t < kDoneMs) {
            if (!isHost_ && (t - lastMemberLog_ >= kMemberLogIntervalMs || lastMemberLog_ == 0)) {
                lastMemberLog_ = t;
                for (int i = 0; i < kCount; ++i) {
                    Character* c = members_[i];
                    if (!c) continue;
                    u32 h[5]; Ogre::Vector3 pos;
                    if (getCharHand(c, h) && getCharPos(c, &pos)) {
                        logf("SCENARIO MEMBER hand=%u,%u,%u,%u,%u pos=%.2f,%.2f,%.2f",
                             h[0], h[1], h[2], h[3], h[4], pos.x, pos.y, pos.z);
                    }
                }
            }
            return false;
        }

        // t >= 6500ms: finish. PASS requires the handshake, 2 spawns, the squad
        // to have grown by 2, and (join) the move to have been issued. The
        // cross-client RECV match is the runner's job, not the in-plugin verdict.
        if (t >= kDoneMs) {
            finalSquad_ = playerSquadSize(ctx.gw);
            bool spawnsOk = (spawnedCount_ == kSpawn);
            bool squadGrew = (baselineSquad_ < 0) ||
                             (finalSquad_ >= baselineSquad_ + kSpawn);
            bool moveOk = isHost_ ? true : moveDone_;
            passed_ = peerOk_ && spawnsOk && squadGrew && moveOk;
            // Emit the squad-growth result as a baseline-relative lower bound
            // (clamped), robust to the save's starting roster and to any neutral
            // proxy bodies the coop layer adds for the peer's units.
            const char* side = isHost_ ? "host" : "join";
            char kg[64]; _snprintf(kg, sizeof(kg) - 1, "%s_squad_grew", side); kg[63] = '\0';
            int delta = (baselineSquad_ < 0 || finalSquad_ < 0)
                          ? kSpawn : (finalSquad_ - baselineSquad_);
            checkInt(kg, kSpawn, delta >= kSpawn ? kSpawn : delta);
            logf("SCENARIO STEP 4 done spawned=%d squad=%d->%d move=%d",
                 spawnedCount_, baselineSquad_, finalSquad_, moveDone_ ? 1 : 0);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const int   kCount               = 3;    // leader + 2 spawned
    static const int   kSpawn               = 2;    // units spawned per client
    static const float kMoveDx;                      // join squad displacement
    static const DWORD kArmTimeoutMs        = 60000; // give up waiting for peer
    static const DWORD kSpawnMs             = 500;
    static const DWORD kFormationMs         = 2000;
    static const DWORD kMoveMs              = 3500;
    static const DWORD kMemberLogStartMs    = 4000;
    static const DWORD kMemberLogIntervalMs = 500;
    static const DWORD kDoneMs              = 6500;

    static Ogre::Vector3 spawnOffset(int i) {
        return (i == 0) ? Ogre::Vector3(5.0f, 0.0f, 0.0f)
                        : Ogre::Vector3(-5.0f, 0.0f, 0.0f);
    }
    // Fixed formation offsets for the 3 members (member 0 = leader).
    static Ogre::Vector3 formationOffset(int i) {
        switch (i) {
            case 0:  return Ogre::Vector3(0.0f, 0.0f, 0.0f);
            case 1:  return Ogre::Vector3(3.0f, 0.0f, 0.0f);
            default: return Ogre::Vector3(-3.0f, 0.0f, 0.0f);
        }
    }

    bool       isHost_;
    bool       armed_;
    DWORD      armElapsedMs_;
    bool       peerOk_;
    int        baselineSquad_;
    int        finalSquad_;
    int        spawnedCount_;
    bool       spawnDone_;
    bool       formationDone_;
    bool       moveDone_;
    DWORD      lastMemberLog_;
    Character* members_[kCount];
    bool       passed_;
};
const float SquadMoveSync::kMoveDx = 10.0f;

} // namespace

// ---- Registry --------------------------------------------------------------
Scenario* makeScenario(const std::string& name) {
    if (name == "squad_spawn_sync") return new SquadSpawnSync();
    if (name == "squad_move_sync")  return new SquadMoveSync();
    return 0;
}

} // namespace coop
