// ScenarioBuildings.cpp - door/building/production scenarios (monolith split
// from Scenario.cpp, 2026-07-12): door_probe/door_sync, build_probe/
// build_sync, bdoor_probe/bdoor_sync, prod_probe/prod_sync, research_probe/
// research_sync, store_probe/store_sync. Classes are TU-private (anonymous
// namespace); only makeBuildingScenario (ScenarioSupport.h) is exported.
// Must NOT: change any SCENARIO log string (oracle API, resources/CODE_MAP.md).

#include "ScenarioSupport.h"

namespace coop {
namespace {


// door_probe (protocol 26 phase 0, probe tier; doorSync forced OFF) /
// door_sync (probe=false, full tier; sync ON). Door/gate open+lock state on
// BAKED buildings is per-client - one player walks through a gate the other
// sees closed; door state feeds pathfinding, AI access, base defense. The
// probe's DESIGN questions:
//   * are baked-door hands cross-client stable (census intersection - the
//     furniture/bed precedent says yes; the wire identity rides on it)?
//   * a sentinel toggle through the engine's own openDoor/closeDoor - does
//     the write stick locally, and does anything cross (expected: no)?
//   * do organic changes appear in the series (NPCs using doors)?
// Script: 1 Hz DOOR census (hand + state per door within ~100m of the
// interest centers) on both sides; host t=12s toggles the FIRST door in
// serial order, join t=24s toggles the SECOND (distinct doors - two
// independent crossing legs for Test-DoorSync to pair). The probe gates only
// that the script ran; the sync variant also requires the local write ok.
class DoorProbeScenario : public TimedScenario {
public:
    explicit DoorProbeScenario(bool probe)
        : TimedScenario(probe ? "door_probe" : "door_sync", /*evidenceMs=*/1000),
          probe_(probe), wrote_(false), writeOk_(false), censusLogged_(0) {
        memset(sentinelHand_, 0, sizeof(sentinelHand_));
    }

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO DOORPROBE start host=%d",
                  ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (evidenceDue(ctx.elapsedMs))
            logCensus(ctx);
        unsigned long writeAt = ctx.isHost ? HOST_WRITE_AT_MS : JOIN_WRITE_AT_MS;
        if (!wrote_ && ctx.elapsedMs >= writeAt) {
            wrote_ = true;
            doSentinel(ctx);
        }
        if (ctx.elapsedMs >= DURATION_MS) {
            passed_ = wrote_ && (censusLogged_ > 0);
            // The gated variant requires the sentinel write to have stuck
            // (the probe only requires the script to have run).
            if (!probe_) passed_ = passed_ && writeOk_;
            return true;
        }
        return false;
    }

private:
    void logCensus(const ScenarioContext& ctx) {
        engine::DoorRead rows[MAX_DOORS];
        unsigned int n = engine::enumDoorsNear(ctx.gw, 100.0f, rows, MAX_DOORS);
        char c[96];
        _snprintf(c, sizeof(c) - 1, "SCENARIO DOORCOUNT n=%u t=%lu", n, ctx.elapsedMs);
        c[sizeof(c) - 1] = '\0'; coop::logLine(c);
        for (unsigned int i = 0; i < n && i < MAX_LOG_ROWS; ++i) {
            const engine::DoorRead& r = rows[i];
            char b[240];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO DOOR hand=%u.%u.%u.%u.%u open=%d locked=%d hasLock=%d "
                      "state=%d gate=%d name='%s' pos=(%.0f,%.0f,%.0f) t=%lu",
                      r.hand[0], r.hand[1], r.hand[2], r.hand[3], r.hand[4],
                      r.open, r.locked, r.hasLock, r.state, r.gate, r.name,
                      r.x, r.y, r.z, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (n > 0) ++censusLogged_;
    }

    // Deterministic cross-client pick: doors sorted by serial ascending; the
    // host toggles the FIRST, the join the SECOND (distinct doors when the
    // census has two - two independent crossing legs).
    bool pickSentinel(const ScenarioContext& ctx, unsigned int outHand[5]) {
        engine::DoorRead rows[MAX_DOORS];
        unsigned int n = engine::enumDoorsNear(ctx.gw, 100.0f, rows, MAX_DOORS);
        if (n == 0) return false;
        // Selection sort indices by (serial, index) ascending - tiny n.
        unsigned int order[MAX_DOORS];
        for (unsigned int i = 0; i < n; ++i) order[i] = i;
        for (unsigned int i = 0; i + 1 < n; ++i)
            for (unsigned int j = i + 1; j < n; ++j) {
                const engine::DoorRead& a = rows[order[i]];
                const engine::DoorRead& b = rows[order[j]];
                if (b.hand[4] < a.hand[4] ||
                    (b.hand[4] == a.hand[4] && b.hand[3] < a.hand[3])) {
                    unsigned int t = order[i]; order[i] = order[j]; order[j] = t;
                }
            }
        unsigned int idx = ctx.isHost ? 0u : (n > 1 ? 1u : 0u);
        memcpy(outHand, rows[order[idx]].hand, sizeof(unsigned int) * 5);
        return true;
    }

    void doSentinel(const ScenarioContext& ctx) {
        int ok = 0, before = -1, after = -1, want = -1;
        if (pickSentinel(ctx, sentinelHand_)) {
            engine::DoorRead cur;
            if (engine::readDoorByHand(sentinelHand_, &cur)) {
                before = cur.open;
                want = cur.open ? 0 : 1; // toggle
                engine::DoorRead post;
                ok = engine::writeDoorByHand(sentinelHand_, want, /*lock untouched*/ -1,
                                             &post) ? 1 : 0;
                after = post.open;
            }
        }
        writeOk_ = (ok == 1) && (after == want);
        char b[208];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO DOORWRITE who=%s hand=%u.%u.%u.%u.%u want=%d ok=%d "
                  "before=%d after=%d t=%lu",
                  ctx.isHost ? "host" : "join",
                  sentinelHand_[0], sentinelHand_[1], sentinelHand_[2],
                  sentinelHand_[3], sentinelHand_[4],
                  want, ok, before, after, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    static const unsigned long HOST_WRITE_AT_MS = 12000;
    static const unsigned long JOIN_WRITE_AT_MS = 24000;
    static const unsigned long DURATION_MS      = 40000;
    static const unsigned int  MAX_DOORS        = 64;
    static const unsigned int  MAX_LOG_ROWS     = 12;

    bool          probe_;
    bool          wrote_;
    bool          writeOk_;
    unsigned int  censusLogged_;
    unsigned int  sentinelHand_[5];
};

// build_probe (protocol 27 phase 0, probe tier; buildSync forced OFF) /
// build_sync (probe=false, full tier; sync ON). Player-PLACED buildings are
// runtime objects (host-only hands - the protocol-21 identity problem for
// structures): a building one player places does not exist at all for the
// other, and construction progress has no channel. The probe's DESIGN
// questions:
//   * does the raw createBuilding factory call succeed where the scenario
//     runs (the UI's placementVerification enforces town rules - does the
//     factory bypass them)? -> BUILDPLACE ok=0/1 answers it either way.
//   * is the minted site enumerable + readable by its local hand (census
//     shows it with progress<1), and do the hands DIFFER across clients
//     (expected: yes - runtime mint order; the wire must key by placer)?
//   * does setConstructionProgress work as the progress lever, and does the
//     engine self-complete at >= 1.0 (scaffold off natively)?
// Script: 1 Hz SITE census (construction sites within ~100m); host t=10s
// places a small template leader-relative (side -4), join t=22s (side +4 -
// distinct spots); each side then ramps its OWN site's progress +0.25 every
// 3 s until complete. The probe gates only that the script ran (a REFUSED
// placement is a finding, not a failure); the sync variant also requires
// the local place + ramp-to-complete to have worked.
class BuildProbeScenario : public TimedScenario {
public:
    explicit BuildProbeScenario(bool probe)
        : TimedScenario(probe ? "build_probe" : "build_sync", /*evidenceMs=*/1000),
          probe_(probe), censusLogged_(0),
          placed_(false), placeOk_(false), rampStep_(0), rampDoneOk_(false),
          nextRampMs_(0) {
        memset(ownHand_, 0, sizeof(ownHand_));
        ownSid_[0] = '\0';
    }

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO BUILDPROBE start host=%d",
                  ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (evidenceDue(ctx.elapsedMs))
            logCensus(ctx);
        unsigned long placeAt = ctx.isHost ? HOST_PLACE_AT_MS : JOIN_PLACE_AT_MS;
        if (!placed_ && ctx.elapsedMs >= placeAt) {
            placed_ = true;
            doPlace(ctx);
        }
        // Ramp until complete; cap the steps so an unknown progress scale (a
        // probe finding, not a failure) keeps the log bounded.
        if (placeOk_ && !rampDoneOk_ && rampStep_ < MAX_RAMP_STEPS &&
            ctx.elapsedMs >= nextRampMs_)
            doRampStep(ctx);
        if (ctx.elapsedMs >= DURATION_MS) {
            passed_ = placed_ && (censusLogged_ > 0);
            // The gated variant requires the whole local leg to have worked
            // (the probe only requires the script to have run and logged).
            if (!probe_) passed_ = passed_ && placeOk_ && rampDoneOk_;
            return true;
        }
        return false;
    }

private:
    void logCensus(const ScenarioContext& ctx) {
        engine::BuildRead rows[MAX_SITES];
        unsigned int n = engine::enumSitesNear(ctx.gw, 100.0f, rows, MAX_SITES);
        char c[96];
        _snprintf(c, sizeof(c) - 1, "SCENARIO BUILDCOUNT n=%u t=%lu", n, ctx.elapsedMs);
        c[sizeof(c) - 1] = '\0'; coop::logLine(c);
        for (unsigned int i = 0; i < n && i < MAX_LOG_ROWS; ++i) {
            const engine::BuildRead& r = rows[i];
            char b[288];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO BUILDSITE hand=%u.%u.%u.%u.%u sid='%s' prog=%.3f "
                      "complete=%d name='%s' pos=(%.0f,%.0f,%.0f) t=%lu",
                      r.hand[0], r.hand[1], r.hand[2], r.hand[3], r.hand[4],
                      r.sid, r.progress, r.complete, r.name, r.x, r.y, r.z,
                      ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (n > 0) ++censusLogged_;
    }

    void doPlace(const ScenarioContext& ctx) {
        float x = 0, y = 0, z = 0, yaw = 0;
        // Distinct spots: host builds 8m ahead-left of the leader, join
        // ahead-right (both clients anchor on the same shared-save leader).
        int rc = engine::probePlaceBuilding(ctx.gw, 8.0f,
                                            ctx.isHost ? -4.0f : 4.0f,
                                            /*wantDoor*/false,
                                            ownHand_, ownSid_, sizeof(ownSid_),
                                            &x, &y, &z, &yaw);
        placeOk_ = (rc == 1) && (ownHand_[4] != 0 || ownHand_[3] != 0);
        if (placeOk_) nextRampMs_ = ctx.elapsedMs + RAMP_STEP_MS;
        char b[256];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO BUILDPLACE who=%s rc=%d ok=%d sid='%s' "
                  "hand=%u.%u.%u.%u.%u pos=(%.1f,%.1f,%.1f) yaw=%.2f t=%lu",
                  ctx.isHost ? "host" : "join", rc, placeOk_ ? 1 : 0, ownSid_,
                  ownHand_[0], ownHand_[1], ownHand_[2], ownHand_[3], ownHand_[4],
                  x, y, z, yaw, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void doRampStep(const ScenarioContext& ctx) {
        ++rampStep_;
        nextRampMs_ = ctx.elapsedMs + RAMP_STEP_MS;
        float want = 0.25f * (float)rampStep_;
        if (want > 1.0f) want = 1.0f;
        engine::BuildRead post;
        int ok = engine::writeBuildProgressByHand(ownHand_, want, &post) ? 1 : 0;
        if (ok && post.complete) rampDoneOk_ = true;
        char b[192];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO BUILDPROG who=%s step=%u write=%.2f ok=%d prog=%.3f "
                  "complete=%d t=%lu",
                  ctx.isHost ? "host" : "join", rampStep_, want, ok,
                  post.progress, post.complete, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    static const unsigned long HOST_PLACE_AT_MS = 10000;
    static const unsigned long JOIN_PLACE_AT_MS = 22000;
    static const unsigned long RAMP_STEP_MS     = 3000;
    static const unsigned long DURATION_MS      = 45000;
    static const unsigned int  MAX_SITES        = 32;
    static const unsigned int  MAX_LOG_ROWS     = 8;
    static const unsigned int  MAX_RAMP_STEPS   = 8;

    bool          probe_;
    unsigned int  censusLogged_;
    bool          placed_;
    bool          placeOk_;
    unsigned int  rampStep_;
    bool          rampDoneOk_;
    unsigned long nextRampMs_;
    unsigned int  ownHand_[5];
    char          ownSid_[48];
};

// bdoor_probe (protocol 28 phase 0, probe tier; bdoorSync forced OFF, the
// protocol-27 mint channel deliberately ON) / bdoor_sync (probe=false, full
// tier; both ON). Doors on PLACED buildings have runtime hands, so the
// protocol-26 door channel skips them - one player opens their shack door,
// the other's proxy stays shut - and a dismantled/destroyed placed building
// leaves a ghost proxy on the peer. The probe's DESIGN questions:
//   * does a minted-then-completed building actually have DoorStuff
//     children, and is the parent->doors index order usable as the wire
//     identity (BDOOR census: parentHand + doorIndex per door)?
//   * does the polite openDoor/closeDoor lever work on a runtime door?
//   * does GameWorld::destroy cleanly remove a placed building locally, and
//     does the peer's proxy SURVIVE it (the ghost finding = the removal gap)?
// Script: place a SHACK (host t=8s side -4, join t=14s side +4), ramp
// progress +0.25/3s to self-complete, 1 Hz census of nearby doors with
// their parent link, toggle OWN shack's door #0 (host t=27s, join t=34s),
// host DESTROYS its shack t=42s; 55s duration. The probe gates the local
// legs only (place + >=1 door + toggle stuck + destroy worked); crossing
// is the sync oracle's job.
class BdoorProbeScenario : public TimedScenario {
public:
    explicit BdoorProbeScenario(bool probe)
        : TimedScenario(probe ? "bdoor_probe" : "bdoor_sync", /*evidenceMs=*/1000),
          probe_(probe), censusLogged_(0),
          placed_(false), placeOk_(false), rampStep_(0), rampDoneOk_(false),
          nextRampMs_(0), doorSeen_(false), toggled_(false), toggleOk_(false),
          destroyed_(false), destroyOk_(false) {
        memset(ownHand_, 0, sizeof(ownHand_));
        ownSid_[0] = '\0';
    }

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO BDOORPROBE start host=%d",
                  ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (evidenceDue(ctx.elapsedMs))
            logCensus(ctx);
        unsigned long placeAt = ctx.isHost ? HOST_PLACE_AT_MS : JOIN_PLACE_AT_MS;
        if (!placed_ && ctx.elapsedMs >= placeAt) {
            placed_ = true;
            doPlace(ctx);
        }
        if (placeOk_ && !rampDoneOk_ && rampStep_ < MAX_RAMP_STEPS &&
            ctx.elapsedMs >= nextRampMs_)
            doRampStep(ctx);
        unsigned long toggleAt = ctx.isHost ? HOST_TOGGLE_AT_MS : JOIN_TOGGLE_AT_MS;
        if (!toggled_ && rampDoneOk_ && ctx.elapsedMs >= toggleAt) {
            toggled_ = true;
            doToggle(ctx);
        }
        if (ctx.isHost && !destroyed_ && ctx.elapsedMs >= DESTROY_AT_MS) {
            destroyed_ = true;
            doDestroy(ctx);
        }
        if (ctx.elapsedMs >= DURATION_MS) {
            passed_ = placed_ && (censusLogged_ > 0);
            // Both tiers require the full local leg: shack placed + completed,
            // it minted at least one door, the toggle stuck, and (host) the
            // destroy worked. The probe measures what CROSSES; the local
            // levers must work in both arms or the A/B proves nothing.
            passed_ = passed_ && placeOk_ && rampDoneOk_ && doorSeen_ && toggleOk_;
            if (ctx.isHost) passed_ = passed_ && destroyOk_;
            return true;
        }
        return false;
    }

private:
    void logCensus(const ScenarioContext& ctx) {
        engine::DoorRead rows[MAX_DOORS];
        unsigned int n = engine::enumDoorsNear(ctx.gw, 100.0f, rows, MAX_DOORS);
        char c[96];
        _snprintf(c, sizeof(c) - 1, "SCENARIO BDOORCOUNT n=%u t=%lu", n, ctx.elapsedMs);
        c[sizeof(c) - 1] = '\0'; coop::logLine(c);
        for (unsigned int i = 0; i < n && i < MAX_LOG_ROWS; ++i) {
            const engine::DoorRead& r = rows[i];
            char b[288];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO BDOOR bhand=%u.%u.%u.%u.%u idx=%d hand=%u.%u.%u.%u.%u "
                      "open=%d locked=%d state=%d name='%s' t=%lu",
                      r.parentHand[0], r.parentHand[1], r.parentHand[2],
                      r.parentHand[3], r.parentHand[4], r.doorIndex,
                      r.hand[0], r.hand[1], r.hand[2], r.hand[3], r.hand[4],
                      r.open, r.locked, r.state, r.name, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (n > 0) ++censusLogged_;
    }

    void doPlace(const ScenarioContext& ctx) {
        float x = 0, y = 0, z = 0, yaw = 0;
        int rc = engine::probePlaceBuilding(ctx.gw, 10.0f,
                                            ctx.isHost ? -5.0f : 5.0f,
                                            /*wantDoor*/true,
                                            ownHand_, ownSid_, sizeof(ownSid_),
                                            &x, &y, &z, &yaw);
        placeOk_ = (rc == 1) && (ownHand_[4] != 0 || ownHand_[3] != 0);
        if (placeOk_) nextRampMs_ = ctx.elapsedMs + RAMP_STEP_MS;
        char b[256];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO BUILDPLACE who=%s rc=%d ok=%d sid='%s' "
                  "hand=%u.%u.%u.%u.%u pos=(%.1f,%.1f,%.1f) yaw=%.2f t=%lu",
                  ctx.isHost ? "host" : "join", rc, placeOk_ ? 1 : 0, ownSid_,
                  ownHand_[0], ownHand_[1], ownHand_[2], ownHand_[3], ownHand_[4],
                  x, y, z, yaw, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void doRampStep(const ScenarioContext& ctx) {
        ++rampStep_;
        nextRampMs_ = ctx.elapsedMs + RAMP_STEP_MS;
        float want = 0.25f * (float)rampStep_;
        if (want > 1.0f) want = 1.0f;
        engine::BuildRead post;
        int ok = engine::writeBuildProgressByHand(ownHand_, want, &post) ? 1 : 0;
        if (ok && post.complete) rampDoneOk_ = true;
        char b[192];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO BUILDPROG who=%s step=%u write=%.2f ok=%d prog=%.3f "
                  "complete=%d t=%lu",
                  ctx.isHost ? "host" : "join", rampStep_, want, ok,
                  post.progress, post.complete, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void doToggle(const ScenarioContext& ctx) {
        int ok = 0, before = -1, after = -1, want = -1;
        engine::DoorRead cur;
        if (engine::readDoorOfBuilding(ownHand_, 0, &cur)) {
            doorSeen_ = true;
            before = cur.open;
            want = cur.open ? 0 : 1; // toggle
            engine::DoorRead post;
            ok = engine::writeDoorByHand(cur.hand, want, /*lock untouched*/ -1,
                                         &post) ? 1 : 0;
            after = post.open;
        }
        toggleOk_ = (ok == 1) && (after == want);
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO BDOORWRITE who=%s bhand=%u.%u.%u.%u.%u idx=0 want=%d "
                  "ok=%d before=%d after=%d t=%lu",
                  ctx.isHost ? "host" : "join",
                  ownHand_[0], ownHand_[1], ownHand_[2], ownHand_[3], ownHand_[4],
                  want, ok, before, after, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void doDestroy(const ScenarioContext& ctx) {
        destroyOk_ = placeOk_ && engine::destroyBuildingByHand(ctx.gw, ownHand_);
        // The programmatic destroy never passes the dismantle notification;
        // queue the removal edge manually so the sync arm streams it.
        if (destroyOk_) engine::queueRemoveEdge(ownHand_);
        engine::BuildRead post;
        int stillThere = engine::readBuildingByHand(ownHand_, &post) ? 1 : 0;
        char b[192];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO BDESTROY who=%s hand=%u.%u.%u.%u.%u ok=%d "
                  "stillResolves=%d t=%lu",
                  ctx.isHost ? "host" : "join",
                  ownHand_[0], ownHand_[1], ownHand_[2], ownHand_[3], ownHand_[4],
                  destroyOk_ ? 1 : 0, stillThere, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    static const unsigned long HOST_PLACE_AT_MS  = 8000;
    static const unsigned long JOIN_PLACE_AT_MS  = 14000;
    static const unsigned long RAMP_STEP_MS      = 3000;
    static const unsigned long HOST_TOGGLE_AT_MS = 27000;
    static const unsigned long JOIN_TOGGLE_AT_MS = 34000;
    static const unsigned long DESTROY_AT_MS     = 42000;
    static const unsigned long DURATION_MS       = 55000;
    static const unsigned int  MAX_DOORS         = 64;
    static const unsigned int  MAX_LOG_ROWS      = 10;
    static const unsigned int  MAX_RAMP_STEPS    = 8;

    bool          probe_;
    unsigned int  censusLogged_;
    bool          placed_;
    bool          placeOk_;
    unsigned int  rampStep_;
    bool          rampDoneOk_;
    unsigned long nextRampMs_;
    bool          doorSeen_;
    bool          toggled_;
    bool          toggleOk_;
    bool          destroyed_;
    bool          destroyOk_;
    unsigned int  ownHand_[5];
    char          ownSid_[48];
};

// prod_probe (protocol 33 phase 0, probe tier; prodSync forced OFF, the
// protocol-27 mint channel deliberately ON so host-placed machines exist on
// both sides) / prod_sync (probe=false, full tier; prodSync ON). Production
// machines, power and farm growth simulate per-client: an ore drill,
// generator, crafting bench or farm ticks independently on each engine, so
// stored output, fuel, power state and crop growth silently fork. The
// probe's DESIGN questions:
//   * machine census: do machine-class buildings near the interest centers
//     enumerate on both clients, with matching hands for BAKED ones (the
//     PROD rows answer by intersection)?
//   * divergence baseline: the host drives operate() on its bench 1 Hz for
//     30 s - do the output/input amounts move on the operating side only
//     (the "gap is real" evidence)?
//   * do the write levers stick - setProductionItem (native), a direct
//     ConsumptionItem::amount write (does update() clamp it next tick?),
//     switchPowerOn (does the power bit persist)?
//   * research evidence: census logs getTechLevel() per research bench and
//     the host drives operate() on one if present - where does progress
//     live (follow-up spike input; no wire commitment here)?
// Script: HOST places a generator (t=8s, side -6) + crafting bench (t=8s,
// side -2) leader-relative and ramps both complete (+0.5/3s via the
// protocol-27 setter, minting proxies on the join when buildSync is on);
// 1 Hz machine census on BOTH sides; host operates the bench 1 Hz
// t=20..50s (and a baked research bench 1 Hz t=30..40s when the census
// found one); power OFF t=52s / ON t=56s on the generator; native
// setProductionItem +2.5 t=58s; direct amount +1.0 t=61s; 70s duration.
// Both tiers gate the local legs only (place + ramp + census + power write
// + setItem write applied); crossing/convergence is the sync oracle's job.
class ProdProbeScenario : public TimedScenario {
public:
    explicit ProdProbeScenario(bool probe)
        : TimedScenario(probe ? "prod_probe" : "prod_sync", /*evidenceMs=*/1000),
          probe_(probe), censusLogged_(0),
          placed_(false), placeGenOk_(false), placeBenchOk_(false),
          rampStep_(0), rampGenDone_(false), rampBenchDone_(false),
          nextRampMs_(0), nextOpMs_(0), opCount_(0), nextResearchMs_(0),
          researchOps_(0), powerOffDone_(false), powerOnDone_(false),
          powerWriteOk_(false), setItemDone_(false), setItemOk_(false),
          directDone_(false), researchSeen_(false) {
        memset(genHand_, 0, sizeof(genHand_));
        memset(benchHand_, 0, sizeof(benchHand_));
        memset(researchHand_, 0, sizeof(researchHand_));
        genSid_[0] = '\0'; benchSid_[0] = '\0';
    }

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO PRODPROBE start host=%d",
                  ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (evidenceDue(ctx.elapsedMs))
            logCensus(ctx);
        if (ctx.isHost) {
            if (!placed_ && ctx.elapsedMs >= PLACE_AT_MS) {
                placed_ = true;
                doPlace(ctx);
            }
            if ((placeGenOk_ || placeBenchOk_) &&
                !(rampGenDone_ && rampBenchDone_) &&
                rampStep_ < MAX_RAMP_STEPS && ctx.elapsedMs >= nextRampMs_)
                doRampStep(ctx);
            if (benchLive() && ctx.elapsedMs >= OP_START_MS &&
                ctx.elapsedMs < OP_END_MS && ctx.elapsedMs >= nextOpMs_)
                doOperate(ctx);
            if (researchSeen_ && ctx.elapsedMs >= RESEARCH_START_MS &&
                ctx.elapsedMs < RESEARCH_END_MS &&
                ctx.elapsedMs >= nextResearchMs_)
                doResearchOp(ctx);
            if (genLive() && !powerOffDone_ && ctx.elapsedMs >= POWER_OFF_AT_MS) {
                powerOffDone_ = true;
                doPowerWrite(ctx, 0);
            }
            if (genLive() && powerOffDone_ && !powerOnDone_ &&
                ctx.elapsedMs >= POWER_ON_AT_MS) {
                powerOnDone_ = true;
                doPowerWrite(ctx, 1);
            }
            if (benchLive() && !setItemDone_ && ctx.elapsedMs >= SETITEM_AT_MS) {
                setItemDone_ = true;
                doOutputWrite(ctx, /*useSetItem*/true, 2.5f);
            }
            if (benchLive() && !directDone_ && ctx.elapsedMs >= DIRECT_AT_MS) {
                directDone_ = true;
                doOutputWrite(ctx, /*useSetItem*/false, 1.0f);
            }
        }
        if (ctx.elapsedMs >= DURATION_MS) {
            passed_ = (censusLogged_ > 0);
            if (ctx.isHost) {
                // Local legs on the driving side: both machines placed and
                // completed, the power toggle applied, the native output
                // write landed, and the operate loop ran. What CROSSED (or
                // measurably didn't) is the oracle's gate.
                passed_ = passed_ && placeGenOk_ && placeBenchOk_ &&
                          rampGenDone_ && rampBenchDone_ &&
                          powerWriteOk_ && setItemOk_ && opCount_ > 0;
            }
            return true;
        }
        return false;
    }

private:
    bool genLive() const   { return placeGenOk_ && rampGenDone_; }
    bool benchLive() const { return placeBenchOk_ && rampBenchDone_; }

    void logCensus(const ScenarioContext& ctx) {
        engine::ProdRead rows[MAX_MACHINES];
        unsigned int n = engine::enumMachinesNear(ctx.gw, 100.0f, rows, MAX_MACHINES);
        char c[96];
        _snprintf(c, sizeof(c) - 1, "SCENARIO PRODCOUNT n=%u t=%lu", n, ctx.elapsedMs);
        c[sizeof(c) - 1] = '\0'; coop::logLine(c);
        for (unsigned int i = 0; i < n && i < MAX_LOG_ROWS; ++i) {
            const engine::ProdRead& r = rows[i];
            char b[448];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO PROD hand=%u.%u.%u.%u.%u class=%d power=%d "
                      "pwrOut=%.1f state=%d mine=%.2f out='%s' outAmt=%.3f "
                      "outCap=%d in0='%s' in0Amt=%.3f in1='%s' in1Amt=%.3f "
                      "tech=%d grown=%.3f died=%.3f harv=%d sid='%s' name='%s' t=%lu",
                      r.hand[0], r.hand[1], r.hand[2], r.hand[3], r.hand[4],
                      r.classType, r.powerOn, r.powerOutput, r.productionState,
                      r.miningLevel, r.outSid, r.outAmount, r.outCap,
                      r.inSid[0], r.inAmount[0], r.inSid[1], r.inAmount[1],
                      r.techLevel, r.grown, r.died, r.harvested,
                      r.sid, r.name, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            // Research evidence subject: first BAKED research bench seen
            // (host drives operate() on it during the research window).
            if (!researchSeen_ && r.classType == 5 /*BCTYPE_RESEARCH*/) {
                researchSeen_ = true;
                memcpy(researchHand_, r.hand, sizeof(researchHand_));
            }
        }
        if (n > 0) ++censusLogged_;
    }

    void doPlace(const ScenarioContext& ctx) {
        int rcG = engine::probePlaceMachine(ctx.gw, 10.0f, -6.0f, /*kind*/0,
                                            genHand_, genSid_, sizeof(genSid_));
        placeGenOk_ = (rcG == 1) && (genHand_[4] != 0 || genHand_[3] != 0);
        int rcB = engine::probePlaceMachine(ctx.gw, 10.0f, -2.0f, /*kind*/1,
                                            benchHand_, benchSid_, sizeof(benchSid_));
        placeBenchOk_ = (rcB == 1) && (benchHand_[4] != 0 || benchHand_[3] != 0);
        if (placeGenOk_ || placeBenchOk_) nextRampMs_ = ctx.elapsedMs + RAMP_STEP_MS;
        char b[320];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO PRODPLACE who=%s genRc=%d genOk=%d genSid='%s' "
                  "genHand=%u.%u.%u.%u.%u benchRc=%d benchOk=%d benchSid='%s' "
                  "benchHand=%u.%u.%u.%u.%u t=%lu",
                  ctx.isHost ? "host" : "join", rcG, placeGenOk_ ? 1 : 0, genSid_,
                  genHand_[0], genHand_[1], genHand_[2], genHand_[3], genHand_[4],
                  rcB, placeBenchOk_ ? 1 : 0, benchSid_,
                  benchHand_[0], benchHand_[1], benchHand_[2], benchHand_[3],
                  benchHand_[4], ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void doRampStep(const ScenarioContext& ctx) {
        ++rampStep_;
        nextRampMs_ = ctx.elapsedMs + RAMP_STEP_MS;
        float want = 0.5f * (float)rampStep_;
        if (want > 1.0f) want = 1.0f;
        engine::BuildRead post;
        if (placeGenOk_ && !rampGenDone_) {
            if (engine::writeBuildProgressByHand(genHand_, want, &post) &&
                post.complete) rampGenDone_ = true;
        }
        if (placeBenchOk_ && !rampBenchDone_) {
            if (engine::writeBuildProgressByHand(benchHand_, want, &post) &&
                post.complete) rampBenchDone_ = true;
        }
        char b[160];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO PRODRAMP who=%s step=%u write=%.2f genDone=%d "
                  "benchDone=%d t=%lu",
                  ctx.isHost ? "host" : "join", rampStep_, want,
                  rampGenDone_ ? 1 : 0, rampBenchDone_ ? 1 : 0, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void doOperate(const ScenarioContext& ctx) {
        nextOpMs_ = ctx.elapsedMs + OP_PERIOD_MS;
        int ok = engine::operateMachineByHand(ctx.gw, benchHand_, 1.0f) ? 1 : 0;
        ++opCount_;
        // Log every 5th op (plus the first) with the post-read state - the
        // 1 Hz census already carries the series; this pins op->state pairs.
        if (opCount_ == 1 || (opCount_ % 5) == 0) {
            engine::ProdRead post;
            bool have = engine::readMachineByHand(benchHand_, &post);
            char b[224];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO PRODOP who=%s kind=bench n=%u ok=%d state=%d "
                      "outAmt=%.3f in0Amt=%.3f t=%lu",
                      ctx.isHost ? "host" : "join", opCount_, ok,
                      have ? post.productionState : -1,
                      have ? post.outAmount : -1.0f,
                      have ? post.inAmount[0] : -1.0f, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    void doResearchOp(const ScenarioContext& ctx) {
        nextResearchMs_ = ctx.elapsedMs + OP_PERIOD_MS;
        int ok = engine::operateMachineByHand(ctx.gw, researchHand_, 1.0f) ? 1 : 0;
        ++researchOps_;
        engine::ProdRead post;
        bool have = engine::readMachineByHand(researchHand_, &post);
        char b[192];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO RESEARCH who=%s n=%u ok=%d tech=%d power=%d t=%lu",
                  ctx.isHost ? "host" : "join", researchOps_, ok,
                  have ? post.techLevel : -1, have ? post.powerOn : -1,
                  ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void doPowerWrite(const ScenarioContext& ctx, int want) {
        engine::ProdRead before, after;
        int haveBefore = engine::readMachineByHand(genHand_, &before) ? 1 : 0;
        int ok = engine::writeMachineByHand(genHand_, want, /*outAmount*/-1.0f,
                                            /*useSetItem*/false, /*in*/0,
                                            /*farm*/0, &after) ? 1 : 0;
        if (ok && after.powerOn == want) powerWriteOk_ = true;
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO PRODWRITE who=%s kind=power want=%d ok=%d before=%d "
                  "after=%d pwrOut=%.1f t=%lu",
                  ctx.isHost ? "host" : "join", want, ok,
                  haveBefore ? before.powerOn : -1, after.powerOn,
                  after.powerOutput, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void doOutputWrite(const ScenarioContext& ctx, bool useSetItem, float delta) {
        engine::ProdRead before, after;
        int haveBefore = engine::readMachineByHand(benchHand_, &before) ? 1 : 0;
        float base = (haveBefore && before.outAmount >= 0.0f) ? before.outAmount : 0.0f;
        float want = base + delta;
        int ok = engine::writeMachineByHand(benchHand_, /*power*/-1, want,
                                            useSetItem, /*in*/0, /*farm*/0,
                                            &after) ? 1 : 0;
        // The native lever must land; the direct write's next-tick fate
        // (clamped or kept) is a census finding either way.
        if (useSetItem && ok && after.outAmount >= 0.0f) setItemOk_ = true;
        char b[256];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO PRODWRITE who=%s kind=%s want=%.3f ok=%d "
                  "before=%.3f after=%.3f out='%s' t=%lu",
                  ctx.isHost ? "host" : "join",
                  useSetItem ? "setitem" : "direct", want, ok,
                  haveBefore ? before.outAmount : -1.0f, after.outAmount,
                  after.outSid, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    static const unsigned long PLACE_AT_MS       = 8000;
    static const unsigned long RAMP_STEP_MS      = 3000;
    static const unsigned long OP_START_MS       = 20000;
    static const unsigned long OP_END_MS         = 50000;
    static const unsigned long OP_PERIOD_MS      = 1000;
    static const unsigned long RESEARCH_START_MS = 30000;
    static const unsigned long RESEARCH_END_MS   = 40000;
    static const unsigned long POWER_OFF_AT_MS   = 52000;
    static const unsigned long POWER_ON_AT_MS    = 56000;
    static const unsigned long SETITEM_AT_MS     = 58000;
    static const unsigned long DIRECT_AT_MS      = 61000;
    static const unsigned long DURATION_MS       = 70000;
    static const unsigned int  MAX_MACHINES      = 32;
    static const unsigned int  MAX_LOG_ROWS      = 10;
    static const unsigned int  MAX_RAMP_STEPS    = 8;

    bool          probe_;
    unsigned int  censusLogged_;
    bool          placed_;
    bool          placeGenOk_;
    bool          placeBenchOk_;
    unsigned int  rampStep_;
    bool          rampGenDone_;
    bool          rampBenchDone_;
    unsigned long nextRampMs_;
    unsigned long nextOpMs_;
    unsigned int  opCount_;
    unsigned long nextResearchMs_;
    unsigned int  researchOps_;
    bool          powerOffDone_;
    bool          powerOnDone_;
    bool          powerWriteOk_;
    bool          setItemDone_;
    bool          setItemOk_;
    bool          directDone_;
    bool          researchSeen_;
    unsigned int  genHand_[5];
    unsigned int  benchHand_[5];
    unsigned int  researchHand_[5];
    char          genSid_[48];
    char          benchSid_[48];
};

// research_probe (protocol 38 phase 0, probe tier; researchSync forced OFF) /
// research_sync (probe=false, full tier; researchSync ON). The tech tree is
// per-client (spike 401: PlayerInterface::technology never crosses), so a tech
// the host researches stays un-known on the join forever. The probe's DESIGN
// questions:
//   * shared subject: do both clients independently pick the SAME
//     not-known-researchable RESEARCH sid (the wire-key stability leg)?
//   * divergence baseline: after the host's startResearch flips its own
//     isKnown, does the join's stay 0 with the hatch OFF (the gap is real)?
//   * apply lever on the JOIN: does startResearch on the join's own store
//     flip isKnown AND stick (the exact call applyResearch makes)?
// Script: BOTH sides pick the deterministic subject at t=8s and log
// known/can 1 Hz; the HOST fires startResearch at t=10s; the JOIN (probe
// tier only) fires its own startResearch at t=25s - so join known=0 across
// t=10..24s is the divergence window, and known=1 after t=25s is the
// apply-lever proof. In the sync tier the join never self-starts: only
// PKT_RESEARCH can flip it (crossing gated by the oracle AND the join's
// local pass). 45s duration.
// Pass: host = picked + startResearch rc=1 + final known=1. Join =
// picked + final known=1 (probe: via its own lever; sync: via the wire) -
// probe additionally requires its self-start rc=1.
class ResearchProbeScenario : public TimedScenario {
public:
    explicit ResearchProbeScenario(bool probe)
        : TimedScenario(probe ? "research_probe" : "research_sync", /*evidenceMs=*/1000),
          probe_(probe), picked_(false),
          pickRc_(0), startDone_(false), startRc_(0), lastKnown_(-1),
          lastCan_(-1) {
        sid_[0] = '\0';
    }

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO RESEARCHPROBE start host=%d probe=%d",
                  ctx.isHost ? 1 : 0, probe_ ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!picked_ && ctx.elapsedMs >= PICK_AT_MS) {
            picked_ = true;
            pickRc_ = engine::researchPickSubject(ctx.gw, sid_, sizeof(sid_));
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO RESEARCHPICK who=%s rc=%d sid='%s' t=%lu",
                      ctx.isHost ? "host" : "join", pickRc_, sid_, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (picked_ && pickRc_ == 1 && evidenceDue(ctx.elapsedMs)) {
            engine::researchQueryBySid(ctx.gw, sid_, &lastKnown_, &lastCan_);
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO RESEARCH who=%s sid='%s' known=%d can=%d t=%lu",
                      ctx.isHost ? "host" : "join", sid_, lastKnown_, lastCan_,
                      ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // The driving edge: host always; join only in the probe tier (the
        // apply-lever stickiness leg - in the sync tier the WIRE must do it).
        bool mayStart = ctx.isHost || probe_;
        unsigned long startAt = ctx.isHost ? HOST_START_MS : JOIN_START_MS;
        if (mayStart && picked_ && pickRc_ == 1 && !startDone_ &&
            ctx.elapsedMs >= startAt) {
            startDone_ = true;
            startRc_ = engine::researchStartBySid(ctx.gw, sid_);
            int k = -1, c = -1;
            engine::researchQueryBySid(ctx.gw, sid_, &k, &c);
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO RESEARCHSTART who=%s rc=%d sid='%s' known=%d t=%lu",
                      ctx.isHost ? "host" : "join", startRc_, sid_, k,
                      ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (ctx.elapsedMs >= DURATION_MS) {
            int k = -1, c = -1;
            if (picked_ && pickRc_ == 1)
                engine::researchQueryBySid(ctx.gw, sid_, &k, &c);
            bool localOk = picked_ && pickRc_ == 1 && k == 1;
            if (ctx.isHost || probe_) localOk = localOk && startRc_ == 1;
            passed_ = localOk;
            char b[192];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO RESEARCHRESULT who=%s pick=%d start=%d "
                      "known=%d pass=%d sid='%s'",
                      ctx.isHost ? "host" : "join", pickRc_, startRc_, k,
                      passed_ ? 1 : 0, sid_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return true;
        }
        return false;
    }

private:
    static const unsigned long PICK_AT_MS    = 8000;
    static const unsigned long HOST_START_MS = 10000;
    static const unsigned long JOIN_START_MS = 25000;
    static const unsigned long DURATION_MS   = 45000;

    bool          probe_;
    bool          picked_;
    int           pickRc_;
    bool          startDone_;
    int           startRc_;
    int           lastKnown_;
    int           lastCan_;
    char          sid_[48];
};

// store_probe (protocol 34 phase 0, probe tier; storeSync forced OFF, the
// protocol-27 mint channel deliberately ON so host-placed buildings exist on
// both sides) / store_sync (probe=false, full tier; storeSync ON). Storage
// chests and machine inventories hold whole ITEMS that fork per-client:
// protocol 33 syncs the production buffer FLOATS, but the crafted items land
// in the machine's Building inventory, and shared chests hold the base's
// real wealth - the container-inventory channel registers exactly ONE
// container (the leader) today. The probe's DESIGN questions:
//   * container census: do STORAGE + machine-class buildings enumerate with
//     readable inventories on both clients (the CONT rows answer, count/
//     qty/hash per row - the capacity evidence vs INV_ITEMS_MAX rides the
//     same rows)?
//   * write levers on a BUILDING container: does the createItem+tryAddItem
//     fabricate path land items INTO a placed chest (kind=add), does the
//     applyContainerContents reconcile REMOVE surplus from one (kind=recon)?
//   * divergence baseline: the host operates its bench 1 Hz - do whole
//     items accumulate in the bench CONTAINER on the operating side only
//     (the join's minted copy stays empty - the gap)?
//   * churn: after the host force-EMPTIES the bench container (kind=empty,
//     the reconcile-removal worst case), does the machine's own update()
//     immediately re-produce (the fight risk the settle window must bound)?
//     The remaining census ticks carry the answer.
//   * join-side minted-container fabricate: the JOIN adds into the chest
//     copy it minted (the first storage row that APPEARED mid-run) - the
//     apply half protocol 34 needs on translated keys (probe tier only).
// Script: HOST places a crafting bench (t=8s, side -2) + a general-storage
// chest (t=8s, side +2) leader-relative and ramps both complete (+0.5/3s,
// minting proxies on the join when buildSync is on); 1 Hz container census
// on BOTH sides; host adds 5 sentinel items into the chest t=22s; operates
// the bench 1 Hz t=24..54s; JOIN (probe only) adds 3 items into its minted
// chest copy t=40s; host reconciles the chest down to 2 sentinels t=58s
// (removal leg) and empties the bench container t=61s (churn leg); 70s.
// Both tiers gate the local legs only (place + ramp + census + add landed +
// recon removed + ops ran); crossing/convergence is the sync oracle's job.
class StoreProbeScenario : public TimedScenario {
public:
    explicit StoreProbeScenario(bool probe)
        : TimedScenario(probe ? "store_probe" : "store_sync", /*evidenceMs=*/1000),
          probe_(probe), censusLogged_(0),
          placed_(false), placeBenchOk_(false), placeChestOk_(false),
          rampStep_(0), rampBenchDone_(false), rampChestDone_(false),
          nextRampMs_(0), nextOpMs_(0), opCount_(0), addDone_(false),
          addOk_(false), joinAddDone_(false), reconDone_(false),
          reconOk_(false), emptyDone_(false), baseHandsCount_(0),
          baseHandsLatched_(false), joinChestSeen_(false) {
        memset(benchHand_, 0, sizeof(benchHand_));
        memset(chestHand_, 0, sizeof(chestHand_));
        memset(joinChestHand_, 0, sizeof(joinChestHand_));
        benchSid_[0] = '\0'; chestSid_[0] = '\0'; addSid_[0] = '\0';
    }

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO STOREPROBE start host=%d",
                  ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (evidenceDue(ctx.elapsedMs))
            logCensus(ctx);
        if (ctx.isHost) {
            if (!placed_ && ctx.elapsedMs >= PLACE_AT_MS) {
                placed_ = true;
                doPlace(ctx);
            }
            if ((placeBenchOk_ || placeChestOk_) &&
                !(rampBenchDone_ && rampChestDone_) &&
                rampStep_ < MAX_RAMP_STEPS && ctx.elapsedMs >= nextRampMs_)
                doRampStep(ctx);
            if (chestLive() && !addDone_ && ctx.elapsedMs >= ADD_AT_MS) {
                addDone_ = true;
                doAdd(ctx, chestHand_, "chest", SENTINEL_QTY);
            }
            if (benchLive() && ctx.elapsedMs >= OP_START_MS &&
                ctx.elapsedMs < OP_END_MS && ctx.elapsedMs >= nextOpMs_)
                doOperate(ctx);
            if (chestLive() && addOk_ && !reconDone_ &&
                ctx.elapsedMs >= RECON_AT_MS) {
                reconDone_ = true;
                doRecon(ctx);
            }
            if (benchLive() && !emptyDone_ && ctx.elapsedMs >= EMPTY_AT_MS) {
                emptyDone_ = true;
                doEmpty(ctx);
            }
        } else if (probe_) {
            // Probe tier only: fabricate into the MINTED chest copy (the
            // translated-key apply half). Skipped under store_sync, where a
            // join-side add would fight the host-authoritative reconcile.
            if (joinChestSeen_ && !joinAddDone_ && ctx.elapsedMs >= JOINADD_AT_MS) {
                joinAddDone_ = true;
                doAdd(ctx, joinChestHand_, "minted", 3);
            }
        }
        if (ctx.elapsedMs >= DURATION_MS) {
            passed_ = (censusLogged_ > 0);
            if (ctx.isHost) {
                // Local legs on the driving side: both buildings placed and
                // completed, the fabricate-into-chest landed, the reconcile
                // removal landed, and the operate loop ran. What CROSSED (or
                // measurably didn't) is the oracle's gate.
                passed_ = passed_ && placeBenchOk_ && placeChestOk_ &&
                          rampBenchDone_ && rampChestDone_ &&
                          addOk_ && reconOk_ && opCount_ > 0;
            }
            return true;
        }
        return false;
    }

private:
    bool benchLive() const { return placeBenchOk_ && rampBenchDone_; }
    bool chestLive() const { return placeChestOk_ && rampChestDone_; }

    static bool sameHand(const unsigned int a[5], const unsigned int b[5]) {
        return a[1] == b[1] && a[3] == b[3] && a[4] == b[4];
    }

    void logCensus(const ScenarioContext& ctx) {
        engine::ContRead rows[MAX_CONTAINERS];
        unsigned int n = engine::enumContainersNear(ctx.gw, 100.0f, rows,
                                                    MAX_CONTAINERS);
        char c[96];
        _snprintf(c, sizeof(c) - 1, "SCENARIO CONTCOUNT n=%u t=%lu", n, ctx.elapsedMs);
        c[sizeof(c) - 1] = '\0'; coop::logLine(c);
        for (unsigned int i = 0; i < n && i < MAX_LOG_ROWS; ++i) {
            const engine::ContRead& r = rows[i];
            char b[384];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO CONT hand=%u.%u.%u.%u.%u class=%d complete=%d "
                      "inv=%d n=%d qty=%d hash=%u first='%s' firstQty=%d "
                      "sid='%s' name='%s' t=%lu",
                      r.hand[0], r.hand[1], r.hand[2], r.hand[3], r.hand[4],
                      r.classType, r.complete, r.hasInv, r.nEntries, r.qtyTotal,
                      r.hash, r.firstSid, r.firstQty, r.sid, r.name,
                      ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (n > 0) ++censusLogged_;
        // JOIN: latch the minted chest = the first STORAGE row that was NOT
        // in the baseline census (host places at t=8s, so anything storage-
        // class appearing later is the mint; baked chests are there from
        // tick one).
        if (!ctx.isHost) {
            if (!baseHandsLatched_) {
                baseHandsLatched_ = true;
                for (unsigned int i = 0; i < n && baseHandsCount_ < MAX_BASE; ++i)
                    if (rows[i].classType == 3 /*BCTYPE_STORAGE*/)
                        memcpy(baseHands_[baseHandsCount_++], rows[i].hand,
                               sizeof(rows[i].hand));
            } else if (!joinChestSeen_) {
                for (unsigned int i = 0; i < n; ++i) {
                    if (rows[i].classType != 3 || !rows[i].complete ||
                        !rows[i].hasInv) continue;
                    bool known = false;
                    for (unsigned int k = 0; k < baseHandsCount_; ++k)
                        if (sameHand(baseHands_[k], rows[i].hand)) { known = true; break; }
                    if (known) continue;
                    memcpy(joinChestHand_, rows[i].hand, sizeof(joinChestHand_));
                    joinChestSeen_ = true;
                    char b[160];
                    _snprintf(b, sizeof(b) - 1,
                              "SCENARIO CONTMINT hand=%u.%u.%u.%u.%u sid='%s' t=%lu",
                              rows[i].hand[0], rows[i].hand[1], rows[i].hand[2],
                              rows[i].hand[3], rows[i].hand[4], rows[i].sid,
                              ctx.elapsedMs);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    break;
                }
            }
        }
    }

    void doPlace(const ScenarioContext& ctx) {
        int rcB = engine::probePlaceMachine(ctx.gw, 10.0f, -2.0f, /*kind*/1,
                                            benchHand_, benchSid_, sizeof(benchSid_));
        placeBenchOk_ = (rcB == 1) && (benchHand_[4] != 0 || benchHand_[3] != 0);
        int rcC = engine::probePlaceMachine(ctx.gw, 10.0f, 2.0f, /*kind*/2,
                                            chestHand_, chestSid_, sizeof(chestSid_));
        placeChestOk_ = (rcC == 1) && (chestHand_[4] != 0 || chestHand_[3] != 0);
        if (placeBenchOk_ || placeChestOk_) nextRampMs_ = ctx.elapsedMs + RAMP_STEP_MS;
        char b[320];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO CONTPLACE who=%s benchRc=%d benchOk=%d benchSid='%s' "
                  "benchHand=%u.%u.%u.%u.%u chestRc=%d chestOk=%d chestSid='%s' "
                  "chestHand=%u.%u.%u.%u.%u t=%lu",
                  ctx.isHost ? "host" : "join", rcB, placeBenchOk_ ? 1 : 0,
                  benchSid_, benchHand_[0], benchHand_[1], benchHand_[2],
                  benchHand_[3], benchHand_[4], rcC, placeChestOk_ ? 1 : 0,
                  chestSid_, chestHand_[0], chestHand_[1], chestHand_[2],
                  chestHand_[3], chestHand_[4], ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void doRampStep(const ScenarioContext& ctx) {
        ++rampStep_;
        nextRampMs_ = ctx.elapsedMs + RAMP_STEP_MS;
        float want = 0.5f * (float)rampStep_;
        if (want > 1.0f) want = 1.0f;
        engine::BuildRead post;
        if (placeBenchOk_ && !rampBenchDone_) {
            if (engine::writeBuildProgressByHand(benchHand_, want, &post) &&
                post.complete) rampBenchDone_ = true;
        }
        if (placeChestOk_ && !rampChestDone_) {
            if (engine::writeBuildProgressByHand(chestHand_, want, &post) &&
                post.complete) rampChestDone_ = true;
        }
        char b[160];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO CONTRAMP who=%s step=%u write=%.2f benchDone=%d "
                  "chestDone=%d t=%lu",
                  ctx.isHost ? "host" : "join", rampStep_, want,
                  rampBenchDone_ ? 1 : 0, rampChestDone_ ? 1 : 0, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void doAdd(const ScenarioContext& ctx, const unsigned int tgt[5],
               const char* tag, int qty) {
        engine::ContRead before, after;
        int haveBefore = engine::readContainerByHand(tgt, &before) ? 1 : 0;
        char sid[48]; sid[0] = '\0';
        // Walks the common stackables until the (type-limited) storage
        // building accepts one - run-171231: a Fabric Chest refused the
        // fixed iron-plate sentinel outright.
        int got = engine::probeAddAnyToContainer(ctx.gw, tgt, qty, sid, sizeof(sid));
        int haveAfter = engine::readContainerByHand(tgt, &after) ? 1 : 0;
        bool ok = (got > 0) && haveAfter &&
                  (!haveBefore || after.qtyTotal > before.qtyTotal);
        if (ctx.isHost) { addOk_ = ok; strncpy(addSid_, sid, sizeof(addSid_) - 1);
                          addSid_[sizeof(addSid_) - 1] = '\0'; }
        char b[288];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO CONTWRITE who=%s kind=add tgt=%s sid='%s' want=%d "
                  "got=%d ok=%d beforeN=%d beforeQty=%d afterN=%d afterQty=%d "
                  "hash=%u t=%lu",
                  ctx.isHost ? "host" : "join", tag, sid, qty, got, ok ? 1 : 0,
                  haveBefore ? before.nEntries : -1,
                  haveBefore ? before.qtyTotal : -1,
                  haveAfter ? after.nEntries : -1,
                  haveAfter ? after.qtyTotal : -1,
                  haveAfter ? after.hash : 0, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void doOperate(const ScenarioContext& ctx) {
        nextOpMs_ = ctx.elapsedMs + OP_PERIOD_MS;
        int ok = engine::operateMachineByHand(ctx.gw, benchHand_, 1.0f) ? 1 : 0;
        ++opCount_;
        // Log every 5th op (plus the first) with the bench CONTAINER read -
        // the whole-items-landing evidence the buffer floats can't show.
        if (opCount_ == 1 || (opCount_ % 5) == 0) {
            engine::ContRead post;
            bool have = engine::readContainerByHand(benchHand_, &post);
            char b[224];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO CONTOP who=%s n=%u ok=%d contN=%d contQty=%d "
                      "hash=%u first='%s' t=%lu",
                      ctx.isHost ? "host" : "join", opCount_, ok,
                      have ? post.nEntries : -1, have ? post.qtyTotal : -1,
                      have ? post.hash : 0, have ? post.firstSid : "",
                      ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    void doRecon(const ScenarioContext& ctx) {
        // The removal leg: capture the chest, cut the sentinel stack down to
        // RECON_KEEP, reconcile - exactly what the join-side apply does when
        // the host's snapshot says "fewer than you have".
        InvItemEntry ent[64];
        unsigned int hash0 = 0;
        unsigned int n = engine::captureContainerContents(ctx.gw, chestHand_,
                                                          ent, 64, &hash0);
        int beforeQty = 0;
        for (unsigned int i = 0; i < n; ++i) beforeQty += (int)ent[i].quantity;
        int found = -1;
        for (unsigned int i = 0; i < n; ++i)
            if (addSid_[0] && strcmp(ent[i].stringID, addSid_) == 0) { found = (int)i; break; }
        if (found >= 0) ent[found].quantity = (unsigned short)RECON_KEEP;
        bool changed = engine::applyContainerContents(ctx.gw, chestHand_, ent, n);
        engine::ContRead after;
        int haveAfter = engine::readContainerByHand(chestHand_, &after) ? 1 : 0;
        // Gate: the sentinel stack was found, the reconcile reported a change,
        // and the container's TOTAL quantity actually went DOWN (the removal
        // stuck through a re-read).
        reconOk_ = (found >= 0) && changed && haveAfter &&
                   after.qtyTotal < beforeQty;
        char b[256];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO CONTWRITE who=host kind=recon tgt=chest sid='%s' "
                  "capN=%u beforeQty=%d found=%d keep=%d changed=%d afterN=%d "
                  "afterQty=%d hash=%u t=%lu",
                  addSid_, n, beforeQty, found, (int)RECON_KEEP, changed ? 1 : 0,
                  haveAfter ? after.nEntries : -1,
                  haveAfter ? after.qtyTotal : -1,
                  haveAfter ? after.hash : 0, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void doEmpty(const ScenarioContext& ctx) {
        // The churn leg: force-empty the bench CONTAINER (count=0 = "empty it",
        // the reconcile's most aggressive removal). The remaining 1 Hz census
        // ticks answer whether update() immediately re-produces (fight risk).
        engine::ContRead before, after;
        int haveBefore = engine::readContainerByHand(benchHand_, &before) ? 1 : 0;
        bool changed = engine::applyContainerContents(ctx.gw, benchHand_, 0, 0);
        int haveAfter = engine::readContainerByHand(benchHand_, &after) ? 1 : 0;
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO CONTWRITE who=host kind=empty tgt=bench changed=%d "
                  "beforeN=%d beforeQty=%d afterN=%d afterQty=%d t=%lu",
                  changed ? 1 : 0,
                  haveBefore ? before.nEntries : -1,
                  haveBefore ? before.qtyTotal : -1,
                  haveAfter ? after.nEntries : -1,
                  haveAfter ? after.qtyTotal : -1, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    static const unsigned long PLACE_AT_MS   = 8000;
    static const unsigned long RAMP_STEP_MS  = 3000;
    static const unsigned long ADD_AT_MS     = 22000;
    static const unsigned long OP_START_MS   = 24000;
    static const unsigned long OP_END_MS     = 54000;
    static const unsigned long OP_PERIOD_MS  = 1000;
    static const unsigned long JOINADD_AT_MS = 40000;
    static const unsigned long RECON_AT_MS   = 58000;
    static const unsigned long EMPTY_AT_MS   = 61000;
    static const unsigned long DURATION_MS   = 70000;
    static const unsigned int  MAX_CONTAINERS = 32;
    static const unsigned int  MAX_LOG_ROWS   = 10;
    static const unsigned int  MAX_RAMP_STEPS = 8;
    static const unsigned int  MAX_BASE       = 8;
    static const int           SENTINEL_QTY   = 5;
    static const int           RECON_KEEP     = 2;

    bool          probe_;
    unsigned int  censusLogged_;
    bool          placed_;
    bool          placeBenchOk_;
    bool          placeChestOk_;
    unsigned int  rampStep_;
    bool          rampBenchDone_;
    bool          rampChestDone_;
    unsigned long nextRampMs_;
    unsigned long nextOpMs_;
    unsigned int  opCount_;
    bool          addDone_;
    bool          addOk_;
    bool          joinAddDone_;
    bool          reconDone_;
    bool          reconOk_;
    bool          emptyDone_;
    unsigned int  baseHands_[8][5];
    unsigned int  baseHandsCount_;
    bool          baseHandsLatched_;
    bool          joinChestSeen_;
    unsigned int  benchHand_[5];
    unsigned int  chestHand_[5];
    unsigned int  joinChestHand_[5];
    char          benchSid_[48];
    char          chestSid_[48];
    char          addSid_[48];
};

} // namespace

Scenario* makeBuildingScenario(const std::string& name) {
    if (name == "door_probe")    return new DoorProbeScenario(true);
    if (name == "door_sync")     return new DoorProbeScenario(false);
    if (name == "build_probe")   return new BuildProbeScenario(true);
    if (name == "build_sync")    return new BuildProbeScenario(false);
    if (name == "bdoor_probe")   return new BdoorProbeScenario(true);
    if (name == "bdoor_sync")    return new BdoorProbeScenario(false);
    if (name == "prod_probe")     return new ProdProbeScenario(true);
    if (name == "prod_sync")      return new ProdProbeScenario(false);
    if (name == "research_probe") return new ResearchProbeScenario(true);
    if (name == "research_sync")  return new ResearchProbeScenario(false);
    if (name == "store_probe")    return new StoreProbeScenario(true);
    if (name == "store_sync")     return new StoreProbeScenario(false);
    return 0;
}

} // namespace coop
