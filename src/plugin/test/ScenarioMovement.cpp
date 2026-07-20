// ScenarioMovement.cpp - squad movement + presence scenarios (monolith split
// from Scenario.cpp, 2026-07-12): leader_move, fast_march, coop_presence,
// travel_parity, split_interest. Classes are TU-private (anonymous
// namespace); only makeMovementScenario (ScenarioSupport.h) is exported.
// Must NOT: change any SCENARIO log string (oracle API, resources/CODE_MAP.md).

#include "ScenarioSupport.h"

namespace coop {
namespace {


// leader_move (Stage 1): the HOST orders its squad leader to walk to a nearby
// destination and streams its transform; the JOIN drives its local copy of that
// same (shared-save) leader to the received transform. Host logs MEMBER, join
// logs RECV; the runner cross-checks them within tolerance.
class LeaderMoveScenario : public TimedScenario {
public:
    LeaderMoveScenario()
        : TimedScenario("leader_move", 500),
          started_(false), recvCount_(0),
          haveStart_(false), sx_(0), sy_(0), sz_(0) {}

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
        if (evidenceDue(ctx.elapsedMs)) {
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

private:
    // 24s -> 62s (2026-07-10): the join's session-start clock catch-up slews
    // its sim at up to 2x for the first ~35-40 s, and the smoothness oracle
    // now EXCLUDES frames scored during the slew (they measured the transient,
    // not the interp pipeline). The window must extend well past convergence
    // so the gate still scores a real steady-state sample (>= 200 frames).
    static const unsigned long DURATION_MS = 62000;
    static const unsigned long SETTLE_MS   = 8000;  // final halt window (fair cross-check + converge)
    static const unsigned long LEG_MS      = 6000;  // oscillation half-period (sparse reversals)
    static const float         LEG;                 // straight-walk leg length (units)
    bool          started_;
    unsigned int  recvCount_;
    bool          haveStart_;
    float         sx_, sy_, sz_;
};

const float LeaderMoveScenario::LEG = 14.0f;

// fast_march (2026-07-11 rubber-banding validation): leader_move at 5x game
// speed. Speed consensus is min(host, join), so BOTH sides vote 5x through the
// loud simulated-click path (writeGameSpeed - the intent hooks capture it as a
// user request, exactly like the manual-session repro). The host marches its
// leader in oscillating legs; the join drives its copy from the stream. The
// verdict rides the join's [interp] counters via the snap_rate oracle: before
// the velocity-aware snap gate, 5x wall-clock velocities turned the fixed 8 u
// hard-snap gate into a per-sample teleport (~35 snaps/s measured 2026-07-11).
class FastMarchScenario : public Scenario {
public:
    FastMarchScenario()
        : passed_(false), recvCount_(0), lastLogMs_(0), lastVoteMs_(0),
          haveStart_(false), sx_(0), sy_(0), sz_(0) {}

    virtual const char* name() const { return "fast_march"; }

    virtual void onStart(const ScenarioContext& ctx) {
        engine::writeGameSpeed(ctx.gw, 5.0f, false); // our 5x vote (both sides)
        if (ctx.isHost) {
            Character* ld = engine::leader(ctx.gw);
            if (ld && engine::readPos(ld, &sx_, &sy_, &sz_)) {
                haveStart_ = true;
                engine::orderMoveTo(ld, sx_ + LEG, sy_, sz_ + LEG);
            }
        }
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO FASTMARCH vote=5.0 haveStart=%d",
                  haveStart_ ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        bool settling = ctx.elapsedMs >= DURATION_MS - SETTLE_MS;
        // Re-vote 5x every 5 s (an engine event may have reset our request);
        // in the settle window vote back to 1x so the session ends sane.
        if (ctx.elapsedMs - lastVoteMs_ >= 5000 || lastVoteMs_ == 0) {
            lastVoteMs_ = ctx.elapsedMs;
            engine::writeGameSpeed(ctx.gw, settling ? 1.0f : 5.0f, false);
        }

        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            Character* ld = engine::leader(ctx.gw);
            if (ctx.isHost) {
                if (haveStart_ && ld) {
                    if (settling) {
                        engine::orderMoveTo(ld, sx_, sy_, sz_);
                    } else {
                        // Time-based oscillation: at 5x the leader covers a leg
                        // in ~1 s and rests until the flip - bursts of genuine
                        // 5x sprinting are exactly the old snap-storm repro.
                        bool legB = ((ctx.elapsedMs / LEG_MS) % 2) != 0;
                        engine::orderMoveTo(ld, legB ? sx_ + LEG : sx_, sy_,
                                                legB ? sz_ + LEG : sz_);
                    }
                }
                logScenarioLine("MEMBER", ld);
            } else {
                if (logScenarioLine("RECV", ld)) ++recvCount_;
            }
        }

        if (ctx.elapsedMs >= DURATION_MS) {
            engine::writeGameSpeed(ctx.gw, 1.0f, false); // leave the world at 1x
            passed_ = ctx.isHost ? (engine::leader(ctx.gw) != 0)
                                 : (recvCount_ >= 1);
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long DURATION_MS = 62000; // outlast the clock slew
    static const unsigned long SETTLE_MS   = 8000;  // final 1x halt window
    static const unsigned long LEG_MS      = 4000;  // oscillation half-period
    static const float         LEG;                 // leg length (units)
    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    unsigned long lastVoteMs_;
    bool          haveStart_;
    float         sx_, sy_, sz_;
};

const float FastMarchScenario::LEG = 25.0f;

// coop_presence (Phase 3.5, BIDIRECTIONAL presence - the keystone two-player test):
// both clients MOVE their OWNED squad member (chosen by save-stable hand-rank, the
// same ordering the Replicator partitions on: host owns rank 0, join owns rank 1 -
// leader-first) and stream it, while driving + observing the PEER's owned member.
// Each side logs MEMBER for its OWN member (authoritative truth it streams) and RECV
// for the PEER's member (the local driven copy), so the runner cross-checks BOTH
// directions by hand: host MEMBER(rank0) vs join RECV(rank0), and join MEMBER(rank1)
// vs host RECV(rank1). Proves each player's character is present + correctly placed
// on the other client. Requires a shared save with >=2 controllable squad members.
class CoopPresenceScenario : public TimedScenario {
public:
    CoopPresenceScenario()
        : TimedScenario("coop_presence", 500), recvCount_(0),
          haveStart_(false), sx_(0), sy_(0), sz_(0) {}

    virtual void onStart(const ScenarioContext&) {}

    virtual bool onTick(const ScenarioContext& ctx) {
        const unsigned int ownRank  = ctx.isHost ? 0u : 1u; // our squad-tab rank
        const unsigned int peerRank = ctx.isHost ? 1u : 0u; // the peer's squad-tab rank

        if (evidenceDue(ctx.elapsedMs)) {
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
    unsigned int  recvCount_;
    bool          haveStart_;
    float         sx_, sy_, sz_;
};
const float CoopPresenceScenario::LEG = 12.0f;

// travel_parity (2026-07-11 field report, "yellow packs while roaming"): the
// JOIN's player character travels FAR from the start while the HOST's PC
// follows - the roaming direction no automated test exercised (every mover so
// far was host-side, but in free play it is the JOIN that wanders and drags
// the interest/census coverage with it). The join TELEPORT-HOPS its OWN
// rank-1 tab leader across the map (the split_interest engine::park
// precedent): HOPS legs of HOP u with a HOP_DWELL_MS dwell each, ~60,000 u
// total - every hop lands entirely OUTSIDE the previous 2000 u census
// bubble, so existence coverage must rebuild from nothing at each stop
// (zone streaming, census re-centering, mint/cull churn - a compressed
// cross-map trek). The host follows its LOCAL driven copy of the join
// leader: teleport catch-up (park) when the gap exceeds FOLLOW_SNAP,
// orderMoveTo otherwise, logging "SCENARIO FOLLOW self=.. peer=.. gap=.."
// for the follow-quality gate.
// While the pair travels, BOTH sides dump a 5 s worldstate (SCENARIO WORLD /
// WNPC rows - the host from its census walk with cls=host, the join from the
// existence audit with each NPC's authority class; enabled via
// Replicator::setAuditRows when this scenario is armed) so Test-TravelParity
// can measure join-only ghosts under zone streaming + census re-centering,
// exactly the free-play failure mode.
class TravelParityScenario : public TimedScenario {
public:
    TravelParityScenario()
        : TimedScenario("travel_parity", 1000), recvCount_(0), hopsDone_(0),
          haveAnchor_(false), ax_(0), ay_(0), az_(0) {}

    virtual void onStart(const ScenarioContext& ctx) {
        // Anchor = the MOVER's start: the join's rank-1 tab leader (both
        // clients resolve it locally from the shared save).
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int mv = tabLeaderIdx(sq, n, 1);
        if (mv >= 0) {
            haveAnchor_ = true;
            ax_ = sq[mv].x; ay_ = sq[mv].y; az_ = sq[mv].z;
        }
        char b[160];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO TRAVEL anchor=%.1f,%.1f,%.1f have=%d hop=%.0f hops=%u dwell=%lums",
                  ax_, ay_, az_, haveAnchor_ ? 1 : 0, HOP,
                  (unsigned)HOPS, HOP_DWELL_MS);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (!haveAnchor_)
            coop::logLine("SCENARIO TRAVEL needs a 2-tab save (rank-1 member missing)");
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;

        if (haveAnchor_ && evidenceDue(ctx.elapsedMs)) {
            EntityState sq[MAX_SQUAD];
            unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
            int mv = tabLeaderIdx(sq, n, 1); // the join's mover
            int fl = tabLeaderIdx(sq, n, 0); // the host's follower

            if (!ctx.isHost) {
                // JOIN: hop our own tab leader one HOP further out every
                // HOP_DWELL_MS (park = halt + teleport; the dwell gives zone
                // streaming + census/mint a re-coverage window at each stop,
                // and a short walk order after the park re-grounds the body
                // and keeps it a live, moving subject rather than a statue).
                unsigned int wantHops = (unsigned int)(ctx.elapsedMs / HOP_DWELL_MS);
                if (wantHops > HOPS) wantHops = HOPS;
                if (mv >= 0) {
                    Character* c = engine::resolve(sq[mv]);
                    if (c) {
                        if (wantHops > hopsDone_) {
                            hopsDone_ = wantHops;
                            float hx = ax_ + (float)hopsDone_ * HOP;
                            engine::park(c, hx, sq[mv].y, az_, 0.0f);
                            char b[96];
                            _snprintf(b, sizeof(b) - 1,
                                      "SCENARIO HOP n=%u to=%.0f,%.0f,%.0f",
                                      hopsDone_, hx, sq[mv].y, az_);
                            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                        } else {
                            // Walk a short leg inside the dwell (re-grounds
                            // the parked body; a genuinely moving mover).
                            float hx = ax_ + (float)hopsDone_ * HOP;
                            bool legB = ((ctx.elapsedMs / 3000) % 2) != 0;
                            engine::orderMoveTo(c, hx + (legB ? 15.0f : 0.0f),
                                                sq[mv].y, az_);
                        }
                    }
                    logScenarioEntity("MEMBER", sq[mv]);
                }
                if (fl >= 0) { logScenarioEntity("RECV", sq[fl]); ++recvCount_; }
            } else {
                // HOST: chase the join leader's LOCAL driven copy - the same
                // body free-play players follow on screen. A hop opens a
                // multi-thousand-unit gap no walk can close: teleport
                // catch-up past FOLLOW_SNAP, walk inside it, stand inside
                // FOLLOW_STOP (don't shove the driven copy around).
                if (mv >= 0 && fl >= 0) {
                    float dx = sq[mv].x - sq[fl].x, dz = sq[mv].z - sq[fl].z;
                    float gap = (float)sqrt((double)(dx * dx + dz * dz));
                    Character* c = engine::resolve(sq[fl]);
                    if (c && gap > FOLLOW_SNAP) {
                        engine::park(c, sq[mv].x - FOLLOW_STOP, sq[mv].y,
                                     sq[mv].z, 0.0f);
                    } else if (c && gap > FOLLOW_STOP) {
                        float f = (gap - FOLLOW_STOP) / gap;
                        engine::orderMoveTo(c, sq[fl].x + dx * f, sq[mv].y,
                                                sq[fl].z + dz * f);
                    }
                    char b[160];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO FOLLOW self=%.1f,%.1f,%.1f peer=%.1f,%.1f,%.1f gap=%.1f",
                              sq[fl].x, sq[fl].y, sq[fl].z,
                              sq[mv].x, sq[mv].y, sq[mv].z, gap);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    logScenarioEntity("MEMBER", sq[fl]);
                    logScenarioEntity("RECV", sq[mv]); ++recvCount_;
                }
            }
        }

        if (ctx.elapsedMs >= dur) {
            passed_ = haveAnchor_ && recvCount_ >= 1;
            return true;
        }
        return false;
    }

private:
    // Long windows: the manifest entry raises the runner's self-exit backstop
    // (Seconds=220) and kill grace (KillGraceSec=190) for this scenario, so
    // the 160 s host window survives. 15 hops x 4000 u = 60,000 u in ~135 s
    // of hop cadence, then a dwell at the far point.
    static const unsigned long JOIN_DURATION_MS = 150000; // hops + far dwell
    static const unsigned long HOST_DURATION_MS = 160000; // outlive the join
    static const unsigned long HOP_DWELL_MS     = 9000;   // per-stop coverage window
    static const unsigned int  HOPS             = 15;     // total legs
    static const unsigned int  MAX_SQUAD        = 32;
    static const float         HOP;         // leg length (units)
    static const float         FOLLOW_STOP; // stop short of the peer (units)
    static const float         FOLLOW_SNAP; // teleport catch-up past this gap
    unsigned int  recvCount_;
    unsigned int  hopsDone_;
    bool          haveAnchor_;
    float         ax_, ay_, az_;
};
const float TravelParityScenario::HOP         = 4000.0f;
const float TravelParityScenario::FOLLOW_STOP = 12.0f;
const float TravelParityScenario::FOLLOW_SNAP = 150.0f;

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
class SplitInterestScenario : public TimedScenario {
public:
    SplitInterestScenario()
        : TimedScenario("split_interest", 500), recvCount_(0),
          movedLogged_(false), haveBar_(false), bx_(0), by_(0), bz_(0) {}

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
        if (evidenceDue(ctx.elapsedMs)) {
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

    unsigned int  recvCount_;
    bool          movedLogged_;
    bool          haveBar_;
    float         bx_, by_, bz_;
};

const float SplitInterestScenario::SPLIT_DIST = 260.0f;

// camp_approach (Phase 2 crash hardening SOAK): reproduce the town/bandit-camp
// approach crash conditions - mint/zone churn on approach plus a real peer drop
// - and prove the survivor cleans up without a fault. Run on the 'camp' save
// (a prison camp, many NPCs). There is NO deterministic repro of the original
// crash, so this is a stress/soak, not a strict-parity gate; Test-CampApproach
// verifies the FIX MECHANISMS from the flushed plugin log.
//
// Timeline (clock from ARM = peer-ready):
//   JOIN teleport-hops its leader across the camp region every HOP_DWELL_MS
//     (park + short walk leg), forcing zone streaming + census/mint bursts as
//     the coverage bubble re-centers - the "approach" churn. It is the machine
//     the crash breadcrumb ("2026-07-11 join crash") blames, so it is the
//     SURVIVOR we harden.
//   HOST holds near its start and self-exits FIRST at HOST_DURATION_MS. That
//     TerminateProcess closes the socket, so the JOIN's transport enqueues a
//     REAL 'handshake: peer left' - firing clearPeerReplicationState (B1) on the
//     survivor mid-churn (no new harness/transport code needed; travel_parity
//     already proves asymmetric self-exit produces a genuine peer-left edge).
//   JOIN keeps running ~JOIN-HOST ms after the drop, so its post-leave cleanup
//     ('[leave] cleared proxies=N') and any stale-drive attempt are captured
//     while it is still hopping (drive path exercised against a just-cleared map).
//
// Test-CampApproach gates (from host.log/join.log): both reach a SCENARIO RESULT
// line (no crash / no truncated log); the join logs 'handshake: peer left' ->
// '[leave] cleared proxies='; no '[drive] STALE' hand is driven after it was
// unbound and no '[drive]' fires after the leave; proxy count returns toward 0.
class CampApproachScenario : public TimedScenario {
public:
    CampApproachScenario()
        : TimedScenario("camp_approach", 1000), hopsDone_(0),
          haveAnchor_(false), ax_(0), ay_(0), az_(0) {}

    virtual void onStart(const ScenarioContext& ctx) {
        // Anchor = this side's own squad leader (both clients resolve their own
        // leader locally; camp is not guaranteed to be a 2-tab save, so we do
        // NOT rely on a rank-1 tab the way travel_parity does).
        Character* ld = engine::leader(ctx.gw);
        if (ld && engine::readPos(ld, &ax_, &ay_, &az_)) haveAnchor_ = true;
        char b[160];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO CAMP start host=%d anchor=%.1f,%.1f,%.1f have=%d "
                  "hop=%.0f hops=%u dwell=%lums hostDur=%lu joinDur=%lu",
                  ctx.isHost ? 1 : 0, ax_, ay_, az_, haveAnchor_ ? 1 : 0, HOP,
                  (unsigned)HOPS, HOP_DWELL_MS, HOST_DURATION_MS, JOIN_DURATION_MS);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (!haveAnchor_)
            coop::logLine("SCENARIO CAMP no leader resolved (empty squad?)");
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;

        if (haveAnchor_ && evidenceDue(ctx.elapsedMs)) {
            Character* ld = engine::leader(ctx.gw);
            if (!ctx.isHost) {
                // JOIN: hop the leader one HOP further out every HOP_DWELL_MS to
                // drive mint/zone churn; short walk legs inside the dwell keep it
                // a live, moving subject.
                unsigned int wantHops = (unsigned int)(ctx.elapsedMs / HOP_DWELL_MS);
                if (wantHops > HOPS) wantHops = HOPS;
                if (ld) {
                    if (wantHops > hopsDone_) {
                        hopsDone_ = wantHops;
                        float hx = ax_ + (float)hopsDone_ * HOP;
                        engine::park(ld, hx, ay_, az_, 0.0f);
                        char b[96];
                        _snprintf(b, sizeof(b) - 1,
                                  "SCENARIO CAMP hop n=%u to=%.0f,%.0f,%.0f",
                                  hopsDone_, hx, ay_, az_);
                        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    } else {
                        float hx = ax_ + (float)hopsDone_ * HOP;
                        bool legB = ((ctx.elapsedMs / 3000) % 2) != 0;
                        engine::orderMoveTo(ld, hx + (legB ? 15.0f : 0.0f), ay_, az_);
                    }
                }
            }
            // Both sides log their leader position (light telemetry; the real
            // gates read the plugin's [spawn]/[drive]/[leave] lines).
            if (ld) {
                float lx = 0, ly = 0, lz = 0;
                engine::readPos(ld, &lx, &ly, &lz);
                char b[128];
                _snprintf(b, sizeof(b) - 1,
                          "SCENARIO CAMP pos host=%d %.1f,%.1f,%.1f t=%lu",
                          ctx.isHost ? 1 : 0, lx, ly, lz, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        if (ctx.elapsedMs >= dur) {
            // Host: exiting first IS the peer-drop stimulus. Join: survived the
            // drop + churn (the cleanup/no-stale-drive verdict is the oracle's,
            // read from the flushed log).
            passed_ = haveAnchor_;
            return true;
        }
        return false;
    }

private:
    // Host exits FIRST (the peer drop); join outlives it by ~20 s to log the
    // post-leave cleanup while still hopping. Manifest raises Seconds/KillGrace
    // so the 150 s join window survives the runner backstop.
    // ~40 s gap so the transport reliably detects the host drop (ENet peer
    // timeout) and delivers 'peer left' to the join well before the join's own
    // self-exit - the survivor needs a wide window to log its post-leave cleanup.
    static const unsigned long HOST_DURATION_MS = 120000; // host drops here
    static const unsigned long JOIN_DURATION_MS = 160000; // join survives + logs cleanup
    static const unsigned long HOP_DWELL_MS     = 8000;   // per-stop coverage window
    static const unsigned int  HOPS             = 12;     // total legs
    static const float         HOP;                       // leg length (units)

    unsigned int  hopsDone_;
    bool          haveAnchor_;
    float         ax_, ay_, az_;
};
const float CampApproachScenario::HOP = 2800.0f;

} // namespace

Scenario* makeMovementScenario(const std::string& name) {
    if (name == "leader_move")  return new LeaderMoveScenario();
    if (name == "fast_march")   return new FastMarchScenario();
    if (name == "coop_presence") return new CoopPresenceScenario();
    if (name == "travel_parity") return new TravelParityScenario();
    if (name == "split_interest") return new SplitInterestScenario();
    if (name == "camp_approach") return new CampApproachScenario();
    return 0;
}

} // namespace coop
