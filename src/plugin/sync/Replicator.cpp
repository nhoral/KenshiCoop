#define _CRT_SECURE_NO_WARNINGS 1

#include "Replicator.h"
#include "../game/Engine.h"
#include "../CoopLog.h"

#include <windows.h> // GetTickCount
#include <deque>
#include <cstdio>
#include <cmath>

class Character;

namespace coop {

namespace {
// High-resolution millisecond clock. GetTickCount's ~15 ms granularity quantizes
// the render time, so at high frame-rates several frames share one render time
// and the interpolated pose doesn't advance (stair-stepped motion + a false
// "no movement" reading in the smoothness oracle). QueryPerformanceCounter gives
// true sub-ms timing, floored to ms here (frames are >1 ms apart).
unsigned long nowMs() {
    static LARGE_INTEGER freq = { 0 };
    if (freq.QuadPart == 0) {
        if (!QueryPerformanceFrequency(&freq) || freq.QuadPart == 0)
            return GetTickCount();
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (unsigned long)(((unsigned __int64)c.QuadPart * 1000ULL) /
                           (unsigned __int64)freq.QuadPart);
}
} // namespace

namespace {
const float MOVE_EPS    = 0.20f;  // source speed above which we treat it as moving
const float SNAP_DIST   = 8.0f;   // gap beyond which we hard-snap (teleport)
const float REPARK_DIST = 1.0f;   // at rest, re-place if it drifts past this
const float CATCHUP_K   = 2.0f;   // gap-proportional speed boost (chase a moving tgt)
const float REISSUE_DIST = 1.0f;  // re-issue the walk order only when tgt moved this far
const float LEAD_SECONDS = 0.6f;  // project the walk target this far along source velocity
const float NPC_MOVE_VEL = 0.75f; // NPC est. velocity (u/s) above which it is "walking"
                                  // (vs a fidget/turn in place -> treat as at rest)
const float TRANSLATE_EPS = 0.02f; // per-frame actual movement counted as "translating"

float dist3(float ax, float ay, float az, float bx, float by, float bz) {
    float dx = ax - bx, dy = ay - by, dz = az - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
} // namespace

Replicator::Replicator()
    : leaderOnly_(true), streamNpcs_(false),
      activeFrames_(0), zeroWhileActive_(0), maxStep_(0.0f),
      translateFrames_(0), walkTruthFrames_(0) {}

void Replicator::ingest(Inbound& in) {
    std::deque<InboundEntity> got;
    in.drainEntities(got);
    if (got.empty()) return;
    unsigned long now = nowMs();
    for (std::deque<InboundEntity>::iterator it = got.begin(); it != got.end(); ++it) {
        targets_[keyOf(it->e)].interp.push(it->e, now);
    }
}

void Replicator::publishOwned(GameWorld* gw, NetLink& net, u32 ownerId) {
    // Capture the locally-owned squad first, then (Stage 4) the nearby world NPCs.
    // The net layer chunks the whole vector into datagram-sized batches, so the
    // count is only bounded by MAX_PUBLISH (a bar holds well under that).
    const unsigned int MAX_PUBLISH = 160;
    static EntityState buf[MAX_PUBLISH]; // main-thread only; avoids a big stack frame
    unsigned int n = engine::captureSquad(gw, leaderOnly_, buf, MAX_PUBLISH);
    if (streamNpcs_ && n < MAX_PUBLISH)
        n += engine::captureNpcs(gw, buf + n, MAX_PUBLISH - n);
    net.setOwnedEntities(ownerId, buf, n);
}

void Replicator::applyTargets(GameWorld* gw) {
    (void)gw;
    unsigned long now = nowMs();
    for (std::map<Key, Driven>::iterator it = targets_.begin(); it != targets_.end(); ++it) {
        Driven& d = it->second;
        EntityState out;
        if (!d.interp.sample(now, cfg_, &out)) {
            // Stream stale: release the body back to local AI (stop driving).
            d.haveActual = false; d.parked = false;
            continue;
        }

        Character* c = engine::resolve(out);
        if (!c) continue;

        float ax, ay, az;
        bool haveActual = engine::readPos(c, &ax, &ay, &az);
        bool hostMoving = (out.cMoving != 0) || (out.cSpeed > MOVE_EPS);

        // Two drive regimes (see Engine::isLocalPlayerChar):
        //   * SQUAD member - a player-controlled body, inert when uncontrolled, so
        //     the engine obeys our move-order: true grounded walk-drive (Stage 3).
        //   * world NPC - fully AI-simulated locally, so a move-order gets fought;
        //     drive it kinematically (teleport wins as the last word) + mirror the
        //     host locomotion so it still animates. Grounded engine-walk + real
        //     sit/idle poses for NPCs arrive in Stage 5 (AI quiet-in-place).
        bool isSquad = engine::isLocalPlayerChar(gw, c);

        // Newest received pose is the position authority while moving; the interp
        // sample ('out') is the smoothed authority at rest. gapNewest measures how
        // far the body trails the true host position.
        EntityState newest;
        float vx = 0.0f, vy = 0.0f, vz = 0.0f;
        bool haveNewest = d.interp.latest(&newest, &vx, &vy, &vz);
        float gapNewest = (haveActual && haveNewest)
                              ? dist3(ax, ay, az, newest.x, newest.y, newest.z) : 0.0f;

        // Genuine translation speed (estimated from the snapshot stream). For NPCs
        // this - not the cMoving flag (which a fidget/turn sets in place) - decides
        // walk-vs-rest, and the smoothness oracle uses the same notion of "active"
        // so a correctly-held (parked) body is not counted as missed movement.
        float vlen = std::sqrt(vx * vx + vy * vy + vz * vz);
        bool npcMoving = haveNewest && (vlen > NPC_MOVE_VEL);

        if (!isSquad) {
            // ---- NPC: velocity-gated drive (smooth walk OR quieted rest) -------
            // An NPC is fully AI-simulated locally, so we classify by the host's
            // actual VELOCITY (not the cMoving flag, which a fidget/turn sets while
            // the body stays put):
            //   * GENUINELY translating -> throttled lead-point walk-drive with NO
            //     clearGoals. The HIGH_PRIORITY move-order already overrides the
            //     AI's movement, and clearGoals would CANCEL our destination (the
            //     engine drops the path), forcing per-frame re-issue => path-restart
            //     stutter. So we leave the AI's goals alone and let the body walk.
            //   * NEAR-STATIONARY -> the AI wants to wander off, so clearGoals to
            //     quiet it, then park at the host transform (held position). This is
            //     where the fidget-in-place drift came from.
            // removeFromUpdateList is NOT used: it freezes the movement controller
            // (walk + teleport both no-op). Real sit/idle poses come in Stage 5.
            if (npcMoving && haveActual && gapNewest > SNAP_DIST) {
                engine::applyRaw(c, newest);
                d.parked = false; d.haveDest = false;
            } else if (npcMoving) {
                float tx = newest.x, ty = newest.y, tz = newest.z;
                float lead = vlen * LEAD_SECONDS;
                tx += vx / vlen * lead; ty += vy / vlen * lead; tz += vz / vlen * lead;
                float moved = d.haveDest ? dist3(tx, ty, tz, d.dx, d.dy, d.dz)
                                         : (REISSUE_DIST + 1.0f);
                if (moved > REISSUE_DIST) {
                    float spd = out.cSpeed + gapNewest * CATCHUP_K;
                    float base = (out.cSpeed > 1.0f) ? out.cSpeed : 12.0f;
                    float cap = base * 2.5f;
                    if (spd > cap) spd = cap;
                    engine::walkTo(c, tx, ty, tz, spd);
                    d.haveDest = true; d.dx = tx; d.dy = ty; d.dz = tz;
                }
                d.parked = false;
            } else {
                engine::clearGoals(c); // quiet the wander only when near-stationary
                float gapOut = haveActual ? dist3(ax, ay, az, out.x, out.y, out.z) : 0.0f;
                if (!d.parked) {
                    if (engine::park(c, out.x, out.y, out.z, out.heading)) d.parked = true;
                } else if (gapOut > REPARK_DIST) {
                    engine::applyRaw(c, out);
                }
                engine::applyMotion(c, false, 0.0f, 0.0f, 0.0f, 0.0f);
                d.haveDest = false;
            }
        } else if (hostMoving && haveActual && haveNewest && gapNewest > SNAP_DIST) {
            // Fell behind / source warped: hard-snap to the true position (no-halt
            // teleport keeps the clip phase advancing rather than freezing).
            engine::applyRaw(c, newest);
            d.parked = false;
            d.haveDest = false;
        } else if (hostMoving) {
            // Engine-WALK toward a LEAD point ahead of the body - the fix for the
            // teleport-slide "float". Aiming at the (render-delayed) interp target
            // makes the char reach it instantly and stop, then wait for the target
            // to creep forward => stop-start stutter. Instead aim at the newest
            // received position projected along the source's velocity, so the char
            // always has somewhere to walk and keeps a continuous gait; catch-up
            // speed converges it, and when the source halts the lead collapses so it
            // settles exactly. Re-issued only when the lead point moves enough (the
            // player move-order recomputes the path, so per-frame re-issue stutters).
            float tx = newest.x, ty = newest.y, tz = newest.z;
            float vlen = std::sqrt(vx * vx + vy * vy + vz * vz);
            if (vlen > 0.01f) {
                float lead = vlen * LEAD_SECONDS;
                tx += vx / vlen * lead;
                ty += vy / vlen * lead;
                tz += vz / vlen * lead;
            }
            float moved = d.haveDest ? dist3(tx, ty, tz, d.dx, d.dy, d.dz)
                                     : (REISSUE_DIST + 1.0f);
            if (moved > REISSUE_DIST) {
                float spd = out.cSpeed;
                spd += gapNewest * CATCHUP_K;
                float base = (out.cSpeed > 1.0f) ? out.cSpeed : 12.0f;
                float cap = base * 2.5f;
                if (spd > cap) spd = cap;
                engine::walkTo(c, tx, ty, tz, spd);
                d.haveDest = true; d.dx = tx; d.dy = ty; d.dz = tz;
            }
            d.parked = false;
            // No motion mirror while genuinely moving: the engine selects the
            // grounded walk clip itself from the locomotion it is performing.
        } else {
            // Squad member at rest: settle once (clean halt+teleport), then only
            // re-place on drift WITHOUT halting (halting every frame freezes the
            // idle clip on frame 0). Mirror an idle locomotion state.
            engine::clearGoals(c);
            float gapOut = haveActual ? dist3(ax, ay, az, out.x, out.y, out.z) : 0.0f;
            if (!d.parked) {
                if (engine::park(c, out.x, out.y, out.z, out.heading)) d.parked = true;
            } else if (gapOut > REPARK_DIST) {
                engine::applyRaw(c, out);
            }
            engine::applyMotion(c, false, 0.0f, 0.0f, 0.0f, 0.0f);
            d.haveDest = false;
        }

        // ---- Oracles (measured from the body's ACTUAL rendered motion) --------
        // "Active" == the host is genuinely translating, matching the drive's own
        // walk-vs-rest decision (velocity-gated for NPCs, flag-based for the squad
        // leader as validated in Stage 3), so a correctly-parked body at a host
        // fidget is not scored as a smoothness miss.
        bool oracleActive = isSquad ? hostMoving : npcMoving;
        if (oracleActive && haveActual && d.haveActual) {
            float step = dist3(ax, ay, az, d.lx, d.ly, d.lz);
            ++activeFrames_;
            if (step < 0.01f) ++zeroWhileActive_;
            if (step > maxStep_) maxStep_ = step;

            if (step > TRANSLATE_EPS) {
                // The body physically moved this frame: it MUST report a real
                // walk state, else it is sliding a static pose (the float bug).
                ++translateFrames_;
                bool m = false; float sp = 0.0f;
                if (engine::readMotion(c, &m, &sp) && m && sp > 0.1f)
                    ++walkTruthFrames_;
            }
        }
        if (haveActual) { d.haveActual = true; d.lx = ax; d.ly = ay; d.lz = az; }
    }
}

void Replicator::logSmoothSummary() {
    float zeroFrac = (activeFrames_ > 0)
                         ? (float)zeroWhileActive_ / (float)activeFrames_
                         : 0.0f;
    char b[160];
    _snprintf(b, sizeof(b) - 1,
              "SCENARIO SMOOTH active=%lu zeroWhileActive=%lu zeroFrac=%.3f maxStep=%.3f",
              activeFrames_, zeroWhileActive_, zeroFrac, maxStep_);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);

    // Anim-truth oracle: fraction of translating frames that did NOT report a
    // real walk state. Low == engine is walking the body (Stage 3 goal); high ==
    // the body slides a static pose (the float bug).
    unsigned long floatFrames = (translateFrames_ > walkTruthFrames_)
                                    ? (translateFrames_ - walkTruthFrames_) : 0;
    float floatFrac = (translateFrames_ > 0)
                          ? (float)floatFrames / (float)translateFrames_
                          : 0.0f;
    char a[160];
    _snprintf(a, sizeof(a) - 1,
              "SCENARIO ANIM translate=%lu walkTruth=%lu floatFrac=%.3f",
              translateFrames_, walkTruthFrames_, floatFrac);
    a[sizeof(a) - 1] = '\0';
    coop::logLine(a);
}

} // namespace coop
