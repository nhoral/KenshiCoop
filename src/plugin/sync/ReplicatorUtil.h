// ReplicatorUtil.h - the PRIVATE shared prelude for the Replicator*.cpp
// partial-class TUs (monolith split of Replicator.cpp, 2026-07-12): the
// include set, the nowMs() clock, the tuning constants and the tiny geometry/
// classification helpers that were the monolith's anonymous-namespace head.
// They stay inside an anonymous namespace ON PURPOSE - each TU gets its own
// internal-linkage copy, byte-identical semantics to the monolith. Anything
// stateful shared between TUs is a Replicator MEMBER (see Replicator.h);
// never add file-scope mutable state here.
//
// The class stays ONE Replicator (Plugin.cpp drives ~35 entry points on
// g_repl); the TUs split the implementation only:
//   ReplicatorCore.cpp      ctor, lifecycle audit, resetSession, ingest*, tabs,
//                           smoothness summary
//   ReplicatorPublish.cpp   publishOwned (+ mid band), publishNpcCensus
//   ReplicatorDrive.cpp     applyTargets, applyRest, sweepCarries, logHardSnap
//   ReplicatorAuthority.cpp applyNpcCensus, enforceHostAuthority,
//                           parkDivergedCopy, debug markers
//   ReplicatorSpawn.cpp     syncSpawns, applyEvents, rekeyPeerBody
//   ReplicatorItems.cpp     inventories, world items, weapon drops/pickups,
//                           cross-owner transfers
//   ReplicatorChannels.cpp  medical/stats/money/factions/doors/builds/prod/
//                           research/recruits/squad-moves/stealth/speed/time,
//                           onPeerConnected

#ifndef KENSHICOOP_REPLICATOR_UTIL_H
#define KENSHICOOP_REPLICATOR_UTIL_H

#define _CRT_SECURE_NO_WARNINGS 1

#include "Replicator.h"
#include "../game/EngineSync.h" // Phase 5a: the Replicator's canonical narrow engine include
#include "../core/WorkPose.h" // poseClearElapsed (debounced task-clear predicate)
#include "../core/DeathLatch.h" // rekeyCarryLatch (down/death latch carry on re-key)
#include "ChangeGate.h" // Phase 6: shared change-gated send/accept policy
#include "SyncContext.h" // Phase 6: per-tick channel call environment
#include "../../netproto/HostIntent.h" // monotonic intent/ack/retry policy
#include "../CoopLog.h"

#include <windows.h> // GetTickCount
#include <deque>
#include <set>
#include <vector>    // rank/sort the shared squad for the ownership partition
#include <algorithm> // std::sort
#include <utility> // std::pair, std::make_pair
#include <string>  // weapon-census keys (Phase W2)
#include <cstdio>
#include <cstring>
#include <cstdlib> // getenv (KENSHICOOP_INV_DUMP diagnostic gate)
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
                                  // (default for snapDist_ - env-tunable, proto 36)
const float SNAP_SECONDS = 0.75f; // velocity-aware snap gate default: hard-snap
                                  // only when the body trails the source by more
                                  // than this much travel time (measured steady-
                                  // state trail while tracking a sprinter: ~0.17s;
                                  // WAN adds ~0.3s - 0.75s only fires on genuine
                                  // fell-behind/warp) (KENSHICOOP_SNAP_SECONDS)
const float REPARK_DIST = 1.0f;   // at rest, re-place if it drifts past this
const float CATCHUP_K   = 2.0f;   // gap-proportional speed boost (chase a moving tgt)
                                  // (default for catchupK_ - env-tunable, proto 36)
const float REISSUE_DIST = 1.0f;  // re-issue the walk order only when tgt moved this far
const float WALK_ARRIVE_DIST = 2.0f; // ...and cancel it once the body is this close to the
                                  // issued point while the source has stopped (march-in-place:
                                  // an order outstanding on a body that already stands there
                                  // holds currentlyMoving with no translation)
const float WALK_STALL_ADV = 0.02f; // per-frame advance below which a walk is going nowhere
const unsigned int WALK_STALL_FRAMES = 15; // ...for this many frames (~200 ms) before the order
                                  // is cancelled as futile. Covers the body that never REACHES
                                  // its point - blocked, or aiming at a lead point projected
                                  // before the source stopped - which arrival alone misses.
const float LEAD_SECONDS = 0.6f;  // project the walk target this far along source velocity
// zeroFrac audit: how long a driven body may go without advancing before a
// zero-step frame stops being render/update-cadence aliasing and starts being a
// freeze. One engine character-update step is well inside this.
const unsigned long ZERO_ALIAS_MS = 100;
// Motion-onset audit buckets: how old a scored frame is, measured from the tick
// the body ENTERED the oracle-active population. Early is one alias window, so a
// zero frame there is a body the render has not picked up yet rather than one
// that stopped; settle matches the drive's own NPC_MOVE_HOLD_MS walk debounce,
// which is the engine's idea of how long a walk takes to be believed.
const unsigned long ONSET_EARLY_MS  = 100;
const unsigned long ONSET_SETTLE_MS = 300;
const float NPC_MOVE_VEL = 0.75f; // NPC est. velocity (u/s) above which it is "walking"
                                  // (vs a fidget/turn in place -> treat as at rest)
const unsigned long TASK_GRACE_MS = 4000;  // settle time before drift-checking a pose
const float TASK_DRIFT_MAX = 4.0f;         // committed pose drift beyond which we park
const unsigned long TASK_RETRY_MS = 1500;  // throttle between pose apply attempts
// Debounced task-clear: the host intermittently reads currentAction==NONE for an
// otherwise-posed NPC (transition frames), so a single NONE frame must NOT tear
// down a committed sit/operate pose. But a genuine job removal (host un-assigns and
// the body stays STATIONARY, so the movement re-arm never fires) streams NONE
// continuously - after this window we release the held pose so the join stops
// reproducing the order. Above transient blips (1-2 frames at 20 Hz), below the
// player-perceptible "why is it still mining" threshold.
const unsigned long TASK_CLEAR_MS = 1200;
// A far-fixture apply (r=3) is RETRIED, not latched bad: a snap-into-fixture on
// the owner (bed entry teleports the body ~9 u instantly) streams the task while
// the join's interp target is still mid-glide, so the first distance gate fails
// spuriously. The park drive walks the body to the fixture meanwhile; only a
// persistent mismatch is a genuinely wrong fixture.
const unsigned int TASK_FAR_RETRY_MAX = 8;
const float TRANSLATE_EPS = 0.02f; // per-frame actual movement counted as "translating"
// Stage 3c combat. A combatant is engine-driven locally (its own footwork), so we do
// NOT walk-drive/park it; positional drift is corrected in GRADED bands (a fight
// legitimately moves the body): under COMBAT_SOFT_DIST leave it alone, between the
// bands nudge it back with a real walk (stance/gait preserved), beyond
// COMBAT_SNAP_DIST hard-teleport (logged - teleports should be rare, they were THE
// waiting-crowd artifact). The attack order re-issues only when the local copy
// disengaged from an ACTIVE fight or its target changed - never on a timer against
// a WAITING (slot-queued) copy, and with exponential backoff (1.5 s -> 6 s cap) so
// a copy that legitimately cannot engage is not clearGoals-reset forever.
const float COMBAT_SOFT_DIST = 6.0f;       // walk-converge combat drift beyond this
const float COMBAT_SNAP_DIST = 20.0f;      // churn ceiling: a correctly-engaged fight owns its
                                           // footwork up to here (measured: a driven brawl
                                           // legitimately churns 12-18 u); a NON-fighting copy
                                           // (arming / idle / waiting / wrong-target) converges
                                           // above the soft band instead. Past this a drifted
                                           // body FAST-SLIDES to the host pose - no teleport
// Convergence-first correction (2026-07-16 smoothness pass). The old gate teleported the
// instant a copy passed COMBAT_SNAP_DIST, which was the visible warp during dense fights.
// Now the copy fast-SLIDES (a quick walk, gait preserved) and only INSTANT-teleports on a
// genuine LEAVE: drift past COMBAT_BIG_SNAP_DIST, a source teleport (SM_SEG_SNAP), or a drift
// that SAT over the snap band for COMBAT_CONVERGE_MS on a source actually moving
// (>= COMBAT_SNAP_VEL). A WAITING stance never "leaves" - it only ever converges, at the
// tighter COMBAT_WAIT_DIST band (a queued body should not wander).
const float COMBAT_BIG_SNAP_DIST = 60.0f;  // true-leave distance: only past this (or a source
                                           // teleport) is an INSTANT warp ever allowed
const float COMBAT_WAIT_DIST     = 3.0f;   // waiting-stance converge band (they shouldn't move)
const float COMBAT_SLIDE_MAX     = 60.0f;  // cap on the fast catch-up walk speed (u/s); a large
                                           // gap glides quickly without a teleport
const float COMBAT_SNAP_VEL      = 8.0f;   // source speed (u/s) below which a big drift is churn
                                           // / wrong-place, not a chase - converge, never warp
const unsigned long COMBAT_CONVERGE_MS = 400; // drift must persist over the snap band this long
                                              // (on a moving source) before an instant teleport
const unsigned long COMBAT_REISSUE_MS     = 1500; // base re-issue throttle
const unsigned long COMBAT_REISSUE_MAX_MS = 6000; // backoff cap
const unsigned int  COMBAT_REISSUE_CAP    = 5;    // max timer re-issues per episode: a copy
                                                  // that won't engage (fleeing template,
                                                  // blocked ring spot) is POSITION-driven
                                                  // from then on, never AI-reset forever
const unsigned long COMBAT_DISARM_MS      = 4000; // host-stream combat gap before the copy
                                                  // is disarmed (stance samples ride the
                                                  // lossy batch; a gap must not AI-reset)
const unsigned long COMBAT_SNAP_COOL_MS   = 3000; // min gap between hard snaps per body (a
                                                  // snap that can't stick - stagger, stale
                                                  // interp - must not re-fire every frame)
const unsigned long NPC_SNAP_COOL_MS      = 3000; // same idea for the locomotion drive
                                                  // (Phase 2): between snaps the walk band
                                                  // converges; a seat-anchored/unloaded body
                                                  // can't be teleport-spammed into place
const unsigned long ATTR_WINDOW_MS = 3000; // remember a combatant's victim this long, so a
                                           // KO/death edge can still name the attacker
// Carried-body sync (protocol 18): the reliable pickup/drop edges do the work;
// these govern the SELF-HEAL only. Heal-pickup throttle mirrors the combat
// re-issue base (each attempt runs the engine's real pickup, don't spam it);
// the drop debounce must sit above the lossy batch's worst gap (the disarm
// lesson: a 1-batch stream blip must not tear a valid carry down).
const unsigned long CARRY_HEAL_MS = 1500; // min gap between self-heal pickups
const unsigned long CARRY_DROP_MS = 3000; // stream must stop reporting the carry
                                          // this long before the local copy drops
// Furniture occupancy sync (protocol 58): same shape as the carry self-heal.
const unsigned long FURN_HEAL_MS = 1500;  // min gap between self-heal enters
const unsigned long FURN_EXIT_MS = 3000;  // stream must stop reporting occupancy
                                          // this long before the local copy exits
const float FURN_MATCH_DIST = 6.0f;       // self-heal fixture search radius around
                                          // the streamed occupant position
// Stealth sync (protocol 20). The posture is CONTINUOUS state (a pure bool in
// every batch - the speed-sync class): apply throttled so a mode-flap (engine
// auto-clearing stealth on a driven copy) doesn't fight setStealthMode every
// frame. The detection feedback publishes at ~4 Hz while a driven sneaker's
// map is non-empty (arrows animate progress; 4 Hz tracks it acceptably).
const unsigned long SNEAK_APPLY_MS   = 1000; // min gap between setStealthMode applies
// Protocol 53: same reasoning for the prone posture. A copy whose engine keeps
// re-deriving the posture (or an AI-suspended one whose medical model has not
// caught up yet) must be re-posed at 1 Hz, never fought per frame.
const unsigned long PRONE_APPLY_MS   = 1000; // min gap between setProneState applies
const unsigned long STEALTH_SEND_MS  = 250;  // detection snapshot cadence (~4 Hz)
const unsigned long STEALTH_RESEND_MS = 2000; // unchanged-map safety resend (unreliable channel)
// Step 4 divergence-gated authority (doctrine 18, behind KENSHICOOP_GATE_AUTHORITY).
// applyTargets runs per FRAME, so the streak is frame-denominated: ~2 s at 75 fps.
const unsigned int  TRUST_STREAK_FRAMES = 150;  // sustained agreement before trusting
const float         TRUST_DRIFT_MAX     = 4.0f; // trusted body must stay this close

// Absolute ceiling on how long publishInventories will hold a container's snapshot while
// the W2 gear census adjudicates a drop (see Replicator::wdPendingDrop). The census retry
// budget is frame-denominated AND re-armable by xferPendingLoss, so it can cycle for as long
// as the transfer detector keeps an unresolved loss - without this cap a single unresolved
// gear decrease starves the peer's view of that bag indefinitely.
const unsigned long WD_HOLD_MAX_MS = 1500;

// Consecutive ZERO-ROW container reads before an EMPTY gear baseline is accepted. A
// container that is merely still resolving fills in within a tick or two; one that is
// genuinely empty never does, and must eventually get a baseline or its owner's first
// pickup is never detected. Committing the FIRST zero read was what produced the burst
// of identity-less PICKUP intents at connect.
const unsigned int  WD_EMPTY_SEED_TICKS = 5;

// How long the author keeps retrying a FAILED conservation re-home (peer picked our
// dropped gear up, but our own bag refused the object - occupied equip slot, no room,
// container still resolving) before it may resolve the split by destroying its ground
// copy. Only ever destroys after SEEING the item in the target container, so the item
// cannot be lost; the cap exists so a permanently-refusing bag does not leave a
// duplicate lying around forever.
const unsigned long WD_REHOME_MAX_MS = 8000;

// Consecutive ticks a tracked ground object must fail to read as a free ground item before
// its track is retired. Forgetting a track that is actually still on the ground is the
// expensive mistake - the author then has no answer for the peer's pickup and its copy stays
// there forever - so the read has to agree with itself first.
const unsigned int  WD_DEAD_READS_MAX = 3;

// ...and how long that disagreement must LAST. The read count alone is denominated in engine
// ticks, and the main loop runs at ~100-125 Hz, so three of them is a ~25 ms tolerance: a
// player's session showed a track retired 29 ms after its own drop, and the pickup that
// arrived 32 s later was answered "untracked" with the ground copy left lying there - the
// duplicate. The cost is asymmetric (a forgotten track is a permanent duplicate; a stale one
// costs a pointer until the next read), so the streak must also survive real time.
const unsigned long WD_DEAD_HOLD_MS = 3000;

// Ceiling for a track whose object has NEVER once read live. WD_DEAD_HOLD_MS assumes there was
// something there to lose; a track minted around an object that never resolved at all has
// nothing to protect and would otherwise be retried forever.
const unsigned long WD_NEVER_LIVE_MAX_MS = 10000;

// Cap on parked-but-unsatisfied pickup intents (see Replicator::PendingPickup). Each expires on
// its own within WD_REHOME_MAX_MS, so this only bounds a pathological burst.
const unsigned int  WD_PENDING_PICKUPS_MAX = 64;

// Radius for the LAST-RESORT spatial re-home: an identified pickup arrived for a drop we no
// longer have tracked, so the object is found by (sid,type) near the picking character
// instead. Only ever used for an intent that NAMES a drop, never for an identity-less one.
const float         WD_REHOME_SCAN_R = 60.0f;

float dist3(float ax, float ay, float az, float bx, float by, float bz) {
    float dx = ax - bx, dy = ay - by, dz = az - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}


// Conservation channel item types - see engine::isConservedItemType for the
// definition and rationale. It lives in the engine layer because the engine's
// own capture (captureWeaponPtrs) applies the SAME gate, and the two copies
// drifted: the engine list omitted the worn container (46), so dropping a
// backpack authored no relocate intent while the inventory snapshot still
// authored a REMOVE - the peer destroyed its backpack with no ground copy.
inline bool isGearType(unsigned int t) { return engine::isConservedItemType(t); }
} // namespace

} // namespace coop

#endif // KENSHICOOP_REPLICATOR_UTIL_H
