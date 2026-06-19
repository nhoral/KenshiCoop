#define _CRT_SECURE_NO_WARNINGS 1

#include "Replicator.h"
#include "../game/Engine.h"
#include "../CoopLog.h"

#include <windows.h> // GetTickCount
#include <deque>
#include <set>
#include <cstdio>
#include <cstring>
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
const unsigned long TASK_GRACE_MS = 4000;  // settle time before drift-checking a pose
const float TASK_DRIFT_MAX = 4.0f;         // committed pose drift beyond which we park
const float TRANSLATE_EPS = 0.02f; // per-frame actual movement counted as "translating"
// Stage 3c combat. A combatant is engine-driven locally (its own footwork), so we do
// NOT walk-drive/park it; we only soft-correct large positional drift (a wider gate
// than rest, since the fight legitimately moves the body) and re-issue the attack
// order on a throttle if the local copy disengaged while the host still reports combat.
const float COMBAT_SNAP_DIST = 6.0f;       // teleport-correct combat drift beyond this
const unsigned long COMBAT_REISSUE_MS = 1500; // re-arm the attack at most this often

float dist3(float ax, float ay, float az, float bx, float by, float bz) {
    float dx = ax - bx, dy = ay - by, dz = az - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
} // namespace

Replicator::Replicator()
    : leaderOnly_(true), streamNpcs_(false),
      activeFrames_(0), zeroWhileActive_(0), maxStep_(0.0f),
      translateFrames_(0), walkTruthFrames_(0),
      restSampleFrames_(0), marchFrames_(0),
      gateSamples_(0), gateAgree_(0), gateLogTick_(0),
      probeRecruit_(false), probedCount_(0),
      probeAiSuspend_(false), aiLogTick_(0), nextEventId_(1) {}

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

    // Emit reliable transition events on bodyState edges. Continuous bodyState
    // already self-heals the down/dead POSTURE over the unreliable channel; the
    // event guarantees the TRANSITION moment is delivered exactly once (a dropped
    // batch can't lose a death), which is what combat (L5) will build on.
    for (unsigned int i = 0; i < n; ++i) {
        const EntityState& e = buf[i];
        Key k = keyOf(e);
        std::map<Key, u16>::iterator pit = hostBody_.find(k);
        u16 prev = (pit != hostBody_.end()) ? pit->second : 0;
        u16 cur  = e.bodyState;
        if (cur != prev) {
            bool wasDown = bodyIsDown(prev), isDownNow = bodyIsDown(cur);
            bool wasDead = (prev & BODY_DEAD) != 0, isDeadNow = (cur & BODY_DEAD) != 0;
            u8 evType = EVT_NONE;
            if (isDeadNow && !wasDead)       evType = EVT_DEATH;
            else if (isDownNow && !wasDown)  evType = EVT_KNOCKOUT;
            else if (!isDownNow && wasDown)  evType = EVT_REVIVE;
            if (evType != EVT_NONE) {
                EventPacket ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = (u8)PKT_EVENT; ev.event = evType;
                ev.ownerId = ownerId;    ev.eventId = nextEventId_++;
                ev.sType = e.hType; ev.sContainer = e.hContainer;
                ev.sContainerSerial = e.hContainerSerial;
                ev.sIndex = e.hIndex; ev.sSerial = e.hSerial;
                net.queueEvent(ev);
                char b[160]; _snprintf(b, sizeof(b) - 1,
                    "[event] SEND id=%u ev=%u hand=%u,%u,%u,%u,%u bs %u->%u",
                    ev.eventId, (unsigned)evType, e.hType, e.hContainer,
                    e.hContainerSerial, e.hIndex, e.hSerial,
                    (unsigned)prev, (unsigned)cur);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        hostBody_[k] = cur;
    }
}

void Replicator::applyEvents(Inbound& in) {
    std::deque<InboundEvent> got;
    in.drainEvents(got);
    for (std::deque<InboundEvent>::iterator it = got.begin(); it != got.end(); ++it) {
        const EventPacket& ev = it->ev;
        Key k; k.t = ev.sType; k.c = ev.sContainer; k.cs = ev.sContainerSerial;
        k.i = ev.sIndex; k.s = ev.sSerial;
        Driven& d = targets_[k]; // creates a placeholder if the body isn't streamed yet
        switch (ev.event) {
            case EVT_DEATH:    d.deathLatched = true;  d.koLatched = true;  break;
            case EVT_KNOCKOUT: d.koLatched = true;                          break;
            case EVT_REVIVE:   d.deathLatched = false; d.koLatched = false; break;
            default: break;
        }
        char b[160]; _snprintf(b, sizeof(b) - 1,
            "[event] RECV id=%u ev=%u owner=%u hand=%u,%u,%u,%u,%u",
            ev.eventId, (unsigned)ev.event, ev.ownerId,
            ev.sType, ev.sContainer, ev.sContainerSerial, ev.sIndex, ev.sSerial);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::applyTargets(GameWorld* gw) {
    (void)gw;
    unsigned long now = nowMs();
    // Rebuild the AI-suspend set from scratch each tick: only NPCs we drive this
    // tick stay suspended; anything we stop driving (stale/suppressed) is dropped
    // here so its AI resumes. Safe to rebuild now - the periodicUpdate detour only
    // reads the set during the engine tick, which already ran this frame.
    if (probeAiSuspend_) engine::clearAiSuspend();
    for (std::map<Key, Driven>::iterator it = targets_.begin(); it != targets_.end(); ++it) {
        Driven& d = it->second;
        EntityState out;
        if (!d.interp.sample(now, cfg_, &out)) {
            // Stream stale: release the body back to local AI (stop driving).
            d.haveActual = false; d.parked = false; d.fresh = false;
            continue;
        }
        d.fresh = true;

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

        // ---- Stage 2: body-state override (down / KO / ragdoll / dead) --------
        // A body the host reports as down (on the ground) must NOT be walk-driven or
        // parked upright - reproducing locomotion on a corpse/KO is exactly the
        // "marching/sliding while down" artifact. Instead drop the local copy into
        // ragdoll and skip ALL locomotion + oracle work for it this tick. The local
        // medical/AI tries to wake the body when its KO timer lapses, so:
        //   - if the local body has actually stood back up, re-collapse it (knockDown
        //     re-triggers the ragdoll fall), else
        //   - top the KO timer EVERY tick (holdDown) so the timer never reaches 0 and
        //     the wake AI never fires - this kills the get-up/flop/ragdoll-spike
        //     flicker proactively instead of re-collapsing after the body stood.
        // When the host reports the body upright again, release the KO once.
        //
        // A reliable EVT_DEATH/EVT_KNOCKOUT latch (d.deathLatched/koLatched) FORCES
        // the down treatment even if this tick's (lossy) continuous sample momentarily
        // reads upright - the whole point of the reliable event is that the down/dead
        // transition is honoured regardless of a dropped batch. EVT_REVIVE clears it.
        if (coop::bodyIsDown(out.bodyState) || d.deathLatched || d.koLatched) {
            unsigned short localBs = engine::readBodyState(c);
            if (!coop::bodyIsDown(localBs)) engine::knockDown(c, true);
            else                            engine::holdDown(c);
            // A ragdoll/KO falls independently on each client (and the join's local
            // AI may have walked the body elsewhere before the down state arrived),
            // so co-locate it with the host's down position when it has drifted.
            // Teleport (not walk) - a limp body has no gait to preserve.
            if (haveActual && dist3(ax, ay, az, out.x, out.y, out.z) > 2.0f)
                engine::applyRaw(c, out);
            d.downApplied = true;
            d.parked = false; d.haveDest = false;
            if (haveActual) { d.haveActual = true; d.lx = ax; d.ly = ay; d.lz = az; }
            continue;
        }
        if (d.downApplied) {
            engine::knockDown(c, false); // host says upright again -> stand back up
            d.downApplied = false;
        }

        // ---- Stage 3c: combat override (melee) --------------------------------
        // The host streams a combat INTENT (task == TASK_COMBAT_MELEE, subject = the
        // attack target's hand). Reproduce the cause: order the local copy to melee the
        // same resolved target and let the join's own engine run the fight (draw, swing,
        // footwork) - the proven "replicate the intent" path (sit/work/down all do this).
        // We deliberately do NOT walk-drive or park a combatant: that fights the local
        // combat movement and would freeze/slide the body. Position is only soft-corrected
        // on large drift (the fight legitimately moves the body around). The attack order
        // is issued once and re-armed on a throttle if the local copy disengaged while the
        // host still reports combat. Combatants skip the AI-suspend path below (their AI
        // must run to animate), reached only via this early `continue`.
        if (coop::taskIsCombat(out.task)) {
            if (!d.detached) d.detached = engine::detachFromTownAI(c);
            engine::CombatRead lc;
            bool localFighting = engine::readCombat(c, &lc) && lc.inCombat;
            if (!d.combatArmed || (!localFighting && (now - d.combatTick) >= COMBAT_REISSUE_MS)) {
                int r = engine::applyCombat(c, out);
                d.combatArmed = true; d.combatTick = now;
                { char b[176]; _snprintf(b, sizeof(b) - 1,
                    "[combat] order hand=%u,%u tgt=%u,%u localFight=%d r=%d",
                    out.hIndex, out.hSerial, out.sIndex, out.sSerial,
                    localFighting ? 1 : 0, r); b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            }
            // Soft position correction only (don't kill the gait): teleport-converge
            // only when the bodies have drifted far apart.
            if (haveActual && dist3(ax, ay, az, out.x, out.y, out.z) > COMBAT_SNAP_DIST)
                engine::applyRaw(c, out);
            d.parked = false; d.haveDest = false;
            if (haveActual) { d.haveActual = true; d.lx = ax; d.ly = ay; d.lz = az; }
            continue;
        }
        // Host no longer reports combat for this body -> disarm so the next fight re-arms
        // and the body falls back to the normal locomotion/rest drive below.
        if (d.combatArmed) {
            d.combatArmed = false;
            engine::clearGoals(c); // drop the stale attack goal before re-parking
        }

        // AI-suspend decision is made BELOW, once we know whether this NPC is
        // genuinely moving and whether the host has it node-anchored - node-sitters
        // must keep their local AI so they can execute the node behavior.

        // AI-gating spike: compare the join's LOCAL task for this NPC to the host's
        // raw task. High agreement means the local AI is mostly doing the same
        // thing the host is, so we could gate it (freeze on match / release on
        // divergence) rather than replicate animation data. Logged, not acted on.
        if (!isSquad) {
            int localKey = engine::readTaskKey(c);
            ++gateSamples_;
            bool agree = (localKey == (int)out.rawTask);
            if (agree) ++gateAgree_;
            if (!agree && (now - gateLogTick_) > 2000) {
                gateLogTick_ = now;
                unsigned long pct = gateSamples_ ? (gateAgree_ * 100 / gateSamples_) : 0;
                char b[160]; _snprintf(b, sizeof(b) - 1,
                    "[gate] hand=%u,%u host=%u local=%d  agree=%lu/%lu (%lu%%)",
                    out.hIndex, out.hSerial, (unsigned)out.rawTask, localKey,
                    gateAgree_, gateSamples_, pct);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }

            // Probe the "inhabit" lever: the first time we see a diverged NPC,
            // recruit it into the player squad ONCE (capped). If the lever works,
            // it stops self-tasking and next tick resolves as isSquad => the proven
            // squad drive path takes over (walkTo + addGoal), and [gate] for this
            // hand should converge as its local AI goes idle.
            if (probeRecruit_ && !agree && probedCount_ < 8) {
                Key k = keyOf(out);
                if (probed_.find(k) == probed_.end()) {
                    probed_.insert(k);
                    bool ok = engine::recruitNpc(gw, c);
                    ++probedCount_;
                    char b[160]; _snprintf(b, sizeof(b) - 1,
                        "[probe] recruit hand=%u,%u host=%u local=%d ok=%d (#%u)",
                        out.hIndex, out.hSerial, (unsigned)out.rawTask, localKey,
                        ok ? 1 : 0, probedCount_);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
        }

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

        // AI-suspend probe: for a host-driven world NPC, suspend its AI decision
        // layer (faction-safe) so it stops self-tasking but keeps animating. The
        // host stream is the sole task authority; the body holds + animates its
        // current/injected action instead of the AI re-deciding every tick.
        // (Releasing node-anchored sitters to local AI was tried - Idea I4 - and
        // regressed: the freed AI wandered them off-host, CROSSCHECK 0.5, and it
        // still did not reliably sit them. So we suspend uniformly.)
        if (probeAiSuspend_ && !isSquad) engine::addAiSuspend(c);

        // Re-arm rest-pose reproduction whenever the body is genuinely moving, so
        // the next time it stops we re-evaluate the host's (possibly new) task.
        bool genuinelyMoving = isSquad ? hostMoving : npcMoving;
        if (genuinelyMoving) {
            d.taskApplied = false; d.taskBad = false; d.issuedTask = TASK_NONE;
        }

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
                // At rest, task-authoritative: reproduce the host's sit/idle pose at
                // the same fixture, else quiet + park.
                //
                // The earlier AI-suspend-only path just snapped position and trusted
                // a "currently-held" pose - but bar patrons sit DYNAMICALLY (they
                // walk in and SIT_AROUND a stool), so at the moment we suspend them
                // they are still standing and freeze there. The host streams task 87
                // (SIT_AROUND, reproducible, subject resolves), so we must actively
                // INJECT the seat via applyRest->applyTask. AI-suspend (set above for
                // this NPC) is what stops the local AI from standing it back up - the
                // thrash that broke this on the non-suspend path.
                applyRest(c, d, out, haveActual, ax, ay, az, now);
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
            // Squad member at rest: reproduce the host's pose (e.g. seated on the
            // same chair) at the same fixture, else quiet + park (Stage 5). This is
            // what makes a join squad-mate sit instead of standing on the chair.
            applyRest(c, d, out, haveActual, ax, ay, az, now);
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
        } else if (!oracleActive && haveActual && d.haveActual) {
            // Host is AT REST. Is the driven body marching in place? (walk clip
            // playing while the body does not translate). This is the bug the
            // float oracle is blind to.
            float step = dist3(ax, ay, az, d.lx, d.ly, d.lz);
            ++restSampleFrames_;
            bool m = false; float sp = 0.0f;
            if (step < TRANSLATE_EPS && engine::readMotion(c, &m, &sp) && m && sp > 0.1f)
                ++marchFrames_;
        }
        if (haveActual) { d.haveActual = true; d.lx = ax; d.ly = ay; d.lz = az; }
    }
    if (probeAiSuspend_ && (now - aiLogTick_) > 3000) {
        aiLogTick_ = now;
        char b[96];
        _snprintf(b, sizeof(b), "[ai] suspended=%u driven=%u",
                  engine::aiSuspendCount(), (unsigned)targets_.size());
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::enforceHostAuthority(GameWorld* gw) {
    if (!gw) return;
    // Hands the host streamed a fresh sample for this tick = the authoritative set.
    std::set<Key> keep;
    for (std::map<Key, Driven>::iterator it = targets_.begin(); it != targets_.end(); ++it) {
        if (it->second.fresh) keep.insert(it->first);
    }

    // Enumerate the join's local world NPCs (same interest query as the host).
    const unsigned int MAX_NPCS = 256;
    static Character*  chars[MAX_NPCS]; // main-thread only
    static EntityState states[MAX_NPCS];
    unsigned int n = engine::listNpcs(gw, chars, states, MAX_NPCS);

    for (unsigned int i = 0; i < n; ++i) {
        Key k = keyOf(states[i]);
        bool streamed = keep.find(k) != keep.end();
        std::map<Key, Character*>::iterator s = suppressed_.find(k);
        if (streamed) {
            // Host owns it now: hand it back to the engine so the drive can pose it.
            if (s != suppressed_.end()) {
                engine::restoreNpc(gw, chars[i]);
                suppressed_.erase(s);
                { char b[96]; _snprintf(b, sizeof(b) - 1,
                    "[authority] restore NPC hand=%u,%u (supp=%u)",
                    states[i].hIndex, states[i].hSerial,
                    (unsigned)suppressed_.size()); b[sizeof(b) - 1] = '\0';
                  coop::logLine(b); }
            }
        } else {
            // Host isn't streaming it: hide + freeze so the local AI can't run a
            // divergent (standing) copy on top of the host-driven world.
            if (s == suppressed_.end()) {
                engine::suppressNpc(gw, chars[i]);
                suppressed_[k] = chars[i];
                { char b[96]; _snprintf(b, sizeof(b) - 1,
                    "[authority] suppress NPC hand=%u,%u (streamed=%u local=%u supp=%u)",
                    states[i].hIndex, states[i].hSerial, (unsigned)keep.size(), n,
                    (unsigned)suppressed_.size()); b[sizeof(b) - 1] = '\0';
                  coop::logLine(b); }
            }
        }
    }
}

void Replicator::applyRest(Character* c, Driven& d, const EntityState& out,
                           bool haveActual, float ax, float ay, float az,
                           unsigned long now) {
    // Re-arm only when the host adopts a genuinely NEW non-NONE rest pose (stood up
    // then sat somewhere else). Crucially we IGNORE transient host->NONE frames: the
    // host capture intermittently reads currentAction==NONE for an otherwise-seated
    // NPC (transition frames), and re-arming on those tore the committed sit back
    // down to a standing park every few frames -> the body oscillated sit/stand and
    // mostly rendered standing. Holding through NONE keeps the seated pose sticky;
    // genuine stand-up is caught by the movement re-arm in applyTargets.
    if (out.task != TASK_NONE && out.task != d.issuedTask) {
        { char b[96]; _snprintf(b, sizeof(b) - 1,
            "[pose] rest re-arm task %u -> %u", (unsigned)d.issuedTask,
            (unsigned)out.task); b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
        d.taskApplied = false; d.taskBad = false; d.issuedTask = out.task;
    }
    // Commit a reproducible pose (sit/operate) at the SAME fixture, once.
    if (out.task != TASK_NONE && !d.taskBad && !d.taskApplied) {
        // I9: detach from the town-AI FIRST (once) so nothing auto-tasks this NPC,
        // then reproduce the pose via the PLAYER-ORDER path (explicit seat location)
        // so it pins THIS stool instead of running SIT_AROUND's own seat search.
        if (!d.detached) { d.detached = engine::detachFromTownAI(c); }
        int r = engine::applyTaskOrder(c, out);
        d.taskTick = now;
        { char b[176]; _snprintf(b, sizeof(b) - 1,
            "[pose] applyOrder hand=%u,%u task=%u subj=%u,%u,%u det=%d r=%d",
            out.hIndex, out.hSerial, (unsigned)out.task,
            out.sIndex, out.sSerial, out.sType, d.detached ? 1 : 0, r);
          b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
        if (r == 2)              d.taskApplied = true; // resolved at host xform + posed
        else if (r == 1 || r == 3) d.taskBad   = true; // not loaded / WRONG far seat -> park
        // r <= 0 / -1: leave unapplied this frame; fall through to park.
    }
    // Drift guard: a committed pose that wandered off the host transform (the
    // engine re-pathed the body to the fixture) is abandoned for a held park.
    if (d.taskApplied && haveActual && (now - d.taskTick) > TASK_GRACE_MS &&
        dist3(ax, ay, az, out.x, out.y, out.z) > TASK_DRIFT_MAX) {
        float dd = dist3(ax, ay, az, out.x, out.y, out.z);
        { char b[160]; _snprintf(b, sizeof(b) - 1,
            "[pose] drift-abandon hand=%u,%u drift=%.2f > %.2f",
            out.hIndex, out.hSerial, dd, (double)TASK_DRIFT_MAX);
          b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
        d.taskApplied = false; d.taskBad = true;
    }
    if (d.taskApplied) {
        d.parked = false; // the engine holds the seated/idle pose; don't fight it
        return;
    }
    // Fallback (no task / fixture missing / drifted): quiet the AI and hold the
    // host transform. Settle once (clean halt+teleport), then only re-place on
    // drift WITHOUT halting (halting every frame freezes the idle clip on frame 0).
    //
    // I10: a node-anchored stander (STAND_AT_NODE, not reproducible) that we
    // suspend mid-walk keeps EXECUTING its walk-to-node action, so held in place it
    // marches. END its current action once so the (already AI-suspended) body drops
    // to idle instead of marching, then hold the transform.
    //
    // NOTE: we deliberately do NOT detach standers from the town-AI here. Detaching
    // a sitter is safe because it is immediately re-anchored by a persistent sit
    // ORDER; a stander gets no replacement intent, so once detached into its own
    // squad it wanders off the spot (observed: standers went ABSENT). endAction
    // under the existing AI-suspend is enough to quiet the march without detaching.
    engine::clearGoals(c);
    float gapOut = haveActual ? dist3(ax, ay, az, out.x, out.y, out.z) : 0.0f;
    if (!d.parked) {
        engine::endAction(c); // stop the residual walk -> idle (not march in place)
        if (engine::park(c, out.x, out.y, out.z, out.heading)) d.parked = true;
    } else if (gapOut > REPARK_DIST) {
        engine::applyRaw(c, out);
    }
    // I11: endAction once is not enough for every stander - some RE-ACQUIRE a
    // walk action after settling and march again. Re-quiet only when the body
    // actually reports a walk motion while we hold it stationary (the precise
    // march signature), so a genuinely idle body's clip is never reset.
    {
        bool reMoving = false; float reSpeed = 0.0f;
        if (engine::readMotion(c, &reMoving, &reSpeed) && reMoving && reSpeed > 0.1f)
            engine::endAction(c);
    }
    engine::applyMotion(c, false, 0.0f, 0.0f, 0.0f, 0.0f);
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

    // March-in-place oracle: of the at-rest frames, fraction where the body played
    // a walk clip while NOT moving. High == "walking on the spot" (the failure the
    // float oracle cannot see, e.g. a host-seated NPC stuck walking on the join).
    float marchFrac = (restSampleFrames_ > 0)
                          ? (float)marchFrames_ / (float)restSampleFrames_
                          : 0.0f;
    char m[160];
    _snprintf(m, sizeof(m) - 1,
              "SCENARIO MARCH restSamples=%lu march=%lu marchFrac=%.3f",
              restSampleFrames_, marchFrames_, marchFrac);
    m[sizeof(m) - 1] = '\0';
    coop::logLine(m);
}

} // namespace coop
