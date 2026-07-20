// ScenarioMedical.cpp - medical + stats scenarios (monolith split from
// Scenario.cpp, 2026-07-12): medic_order, limb_loss, stats_sync. Classes are
// TU-private (anonymous namespace); only makeMedicalScenario
// (ScenarioSupport.h) is exported.
// Must NOT: change any SCENARIO log string (oracle API, resources/CODE_MAP.md).

#include "ScenarioSupport.h"

namespace coop {
namespace {


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
class MedicOrderScenario : public TimedScenario {
public:
    MedicOrderScenario()
        : TimedScenario("medic_order", 0), recvCount_(0), lastLogMs_(0), lastHealMs_(0),
          haveOwn_(false), havePeer_(false), woundLogged_(false),
          healLogged_(false) {}

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
class LimbLossScenario : public TimedScenario {
public:
    LimbLossScenario()
        : TimedScenario("limb_loss", 0), recvCount_(0), lastLogMs_(0),
          haveOwn_(false), havePeer_(false), cutLogged_(false) {}

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
class StatsSyncScenario : public TimedScenario {
public:
    StatsSyncScenario()
        : TimedScenario("stats_sync", 0), recvCount_(0), lastLogMs_(0),
          haveOwn_(false), havePeer_(false), raiseLogged_(false) {}

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

    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveOwn_;
    bool          havePeer_;
    bool          raiseLogged_;
    unsigned int  ownHand_[5];
    unsigned int  peerHand_[5];
};

} // namespace

Scenario* makeMedicalScenario(const std::string& name) {
    if (name == "medic_order")  return new MedicOrderScenario();
    if (name == "limb_loss")    return new LimbLossScenario();
    if (name == "stats_sync")   return new StatsSyncScenario();
    return 0;
}

} // namespace coop
