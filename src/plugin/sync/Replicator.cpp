#define _CRT_SECURE_NO_WARNINGS 1

#include "Replicator.h"
#include "../game/Engine.h"
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
const float LEAD_SECONDS = 0.6f;  // project the walk target this far along source velocity
const float NPC_MOVE_VEL = 0.75f; // NPC est. velocity (u/s) above which it is "walking"
                                  // (vs a fidget/turn in place -> treat as at rest)
const unsigned long TASK_GRACE_MS = 4000;  // settle time before drift-checking a pose
const float TASK_DRIFT_MAX = 4.0f;         // committed pose drift beyond which we park
const unsigned long TASK_RETRY_MS = 1500;  // throttle between pose apply attempts
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
const float COMBAT_SNAP_DIST = 20.0f;      // teleport-correct combat drift beyond this
                                           // (measured: a driven brawl legitimately churns
                                           // 12-18 u against the walk band - snapping there
                                           // is visible teleporting for no fidelity gain;
                                           // past 20 u the host body has genuinely LEFT -
                                           // sprint-chases measured 45-110 u)
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
// Furniture occupancy sync (protocol 19): same shape as the carry self-heal.
const unsigned long FURN_HEAL_MS = 1500;  // min gap between self-heal enters
const unsigned long FURN_EXIT_MS = 3000;  // stream must stop reporting occupancy
                                          // this long before the local copy exits
const unsigned long FURN_PEER_MS = 5000;  // third-party PEER-ENTER re-author gap
                                          // (protocol 36: guard jails a peer PC)
const float FURN_MATCH_DIST = 6.0f;       // self-heal fixture search radius around
                                          // the streamed occupant position
// Stealth sync (protocol 20). The posture is CONTINUOUS state (a pure bool in
// every batch - the speed-sync class): apply throttled so a mode-flap (engine
// auto-clearing stealth on a driven copy) doesn't fight setStealthMode every
// frame. The detection feedback publishes at ~4 Hz while a driven sneaker's
// map is non-empty (arrows animate progress; 4 Hz tracks it acceptably).
const unsigned long SNEAK_APPLY_MS   = 1000; // min gap between setStealthMode applies
const unsigned long STEALTH_SEND_MS  = 250;  // detection snapshot cadence (~4 Hz)
const unsigned long STEALTH_RESEND_MS = 2000; // unchanged-map safety resend (unreliable channel)
// Step 4 divergence-gated authority (doctrine 18, behind KENSHICOOP_GATE_AUTHORITY).
// applyTargets runs per FRAME, so the streak is frame-denominated: ~2 s at 75 fps.
const unsigned int  TRUST_STREAK_FRAMES = 150;  // sustained agreement before trusting
const float         TRUST_DRIFT_MAX     = 4.0f; // trusted body must stay this close

float dist3(float ax, float ay, float az, float bx, float by, float bz) {
    float dx = ax - bx, dy = ay - by, dz = az - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Conservation channel item types. itemType 2 = WEAPON and 3 = ARMOUR/clothing.
// Both are non-stackable EQUIPPABLE gear, so each unit is a distinct
// object the peer already mirrors (weapons via shared save; armour also reconstructed by inv
// sync) - the real object can be relocated bag<->ground on every client and re-homed on pickup
// WITHOUT fabrication. The W1 host-authored proxy stream handles everything else (stacks, loot)
// and skips these. Routing gear through conservation also fixes a host-dropped item lingering
// on the host ground after the JOIN picks it up (the W1 cull only removes the join's proxy).
inline bool isGearType(unsigned int t) { return t == 2u || t == 3u; }
} // namespace

Replicator::Replicator()
    : catchupK_(CATCHUP_K), snapDist_(SNAP_DIST), snapSeconds_(SNAP_SECONDS),
      sendStamp_(true),
      starveHoldMs_(10000), starveHeldNow_(0),
      leaderOnly_(true), streamNpcs_(false),
      activeFrames_(0), zeroWhileActive_(0), maxStep_(0.0f), slewSkipFrames_(0),
      interpLerp_(0), interpSingle_(0), interpClampOld_(0),
      interpExtrap_(0), interpSegSnap_(0),
      hardSnapSquad_(0), hardSnapNpc_(0),
      walkReissueSquad_(0), walkReissueNpc_(0), restFlipNpc_(0), interpLogTick_(0),
      translateFrames_(0), walkTruthFrames_(0),
      restSampleFrames_(0), marchFrames_(0),
      gateSamples_(0), gateAgree_(0), gateLogTick_(0),
      probeRecruit_(false), probedCount_(0),
      aiSuspend_(false), aiLogTick_(0), nextEventId_(1),
      nextWorldNetId_(1), nextDropId_(1), nextPickupId_(1), nextXferId_(1),
      xferScanMs_(0), nextTreatId_(1),
      quietRelapse_(0), sitOrders_(0), detachUses_(0), noDetach_(false),
      dmgGuard_(false), carrySync_(true), furnSync_(true), stealthSync_(true),
      gateAuthority_(false), trustLogTick_(0),
      trustGrants_(0), trustRevokes_(0),
      authSuppresses_(0), authRestores_(0), authReassertMs_(0), authPruned_(0),
      censusRadius_(0.0f), censusSendMs_(0), censusRecvMs_(0), censusCulls_(0),
      censusParkDist_(0.0f), censusParks_(0),
      speedLastApplied_(-1.0f), speedMyReq_(-1.0f), speedPeerReq_(-1.0f),
      speedMyCombat_(false), speedPeerCombat_(false), speedLastSet_(-1.0f),
      speedSeqOut_(1), speedSeqSeen_(0),
      speedLastSendMs_(0), speedCombatSampleMs_(0),
      spawnSync_(false), spawnPosLogMs_(0),
      spawnMintRadius_(0.0f), censusScanMs_(0),
      moneySync_(true), recruitSync_(true),
      squadSync_(true),
      facSeqOut_(1), facSampleMs_(0), factionSync_(true),
      doorSeqOut_(1), doorSampleMs_(0), doorSync_(true),
      buildSeqOut_(1), buildSampleMs_(0), buildSync_(true),
      bdoorSeqOut_(1), bdoorSampleMs_(0), bdoorSync_(true),
      hungerSync_(true),
      prodSeqOut_(1), prodSampleMs_(0), prodSync_(true),
      researchSeqOut_(1), researchSampleMs_(0), researchSync_(true),
      storeSync_(false), contCensusMs_(0),
      timeSync_(true), timeSlew_(1.0f), timeSeqOut_(1), timeSeqSeen_(0),
      timeLastSendMs_(0), timeLastLogMs_(0), timeSlewApplied_(-1.0f) {}

void Replicator::resetSession() {
    // Pointer caches (dangling after the swap) + everything keyed off them.
    targets_.clear();          // interp buffers + per-body drive state
    drivenChars_.clear();
    proxyByKey_.clear();
    suppressed_.clear();
    // Debug markers hold raw Character* from the OLD world plus GUI label
    // objects we own - destroy the labels and drop the map before either
    // dangles into the new session.
    for (std::map<Character*, DebugMarker>::iterator mi = debugMarkers_.begin();
         mi != debugMarkers_.end(); ++mi)
        engine::markerDestroy(mi->second.label);
    debugMarkers_.clear();
    hostBody_.clear();
    attackerOf_.clear();
    authCount_.clear();
    ownHands_.clear();
    // Protocol 36: the existence census describes the OLD world's hands; the
    // host re-publishes within a second of the new world going live.
    censusHands_.clear();
    censusPos_.clear();
    parkMs_.clear();
    censusRecvMs_ = 0;
    censusSendMs_ = 0;
    furnPeerPend_.clear();
    ownFurnExit_.clear();
    // Session maps + change-gate baselines (they describe the OLD world; the
    // reloaded save re-seeds them on first sample).
    ownBuilds_.clear();
    peerBuilds_.clear();
    mintByLocal_.clear();
    bdoorRows_.clear();
    doorRows_.clear();
    prodRows_.clear();
    researchRows_.clear(); // protocol 38: re-baselined from the new world's store
    facRows_.clear();
    invPub_.clear();
    invRecv_.clear();
    ownedContainers_.clear();
    censusContainers_.clear(); // protocol 34: re-censused in the new world
    worldTrack_.clear();
    worldProxies_.clear();
    weaponCensus_.clear();
    appliedDrops_.clear();
    appliedPickups_.clear();
    groundedWeapons_.clear();
    // Protocol 37: every container hand and Item* baseline is stale in the new world.
    xferBase_.clear();
    xferSeeded_.clear();
    xferPend_.clear();
    xferLatch_.clear();
    xferDefer_.clear();
    appliedXfers_.clear();
    wdSuppress_.clear();
    xferScanMs_ = 0;
    medPub_.clear();
    medRecv_.clear();
    medNpc_.clear();
    statsPub_.clear();
    moneyPub_.clear();
    stealthPub_.clear();
    pinOwned_.clear();
    pinPeer_.clear();
    // Protocol 35: the rank latch + the engine's pointer->hand baseline both
    // describe the OLD world (containers and Character* dangle after a swap);
    // the reloaded save re-seeds them at first census/poll.
    tabRank_.clear();
    rekeyedOld_.clear();
    engine::clearSquadRoster();
    probed_.clear();
    spawnReq_.clear();
    unresolvedHands_.clear();
    spawnLogged_.clear();
    spawnReplyMs_.clear();
    censusScanMs_ = 0;
    // Speed/time consensus: re-seed from the fresh world's live state (the
    // save's speed becomes the new baseline; the join's slew re-measures).
    speedLastApplied_ = -1.0f;
    speedMyReq_       = -1.0f;
    speedPeerReq_     = -1.0f;
    speedMyCombat_    = false;
    speedPeerCombat_  = false;
    speedLastSet_     = -1.0f;
    speedSeqSeen_     = 0;
    speedLastSendMs_  = 0;
    speedCombatSampleMs_ = 0;
    timeSlew_         = 1.0f;
    timeSeqSeen_      = 0;
    timeLastSendMs_   = 0;
    timeSlewApplied_  = -1.0f;
    // Sample-cadence clocks restart.
    facSampleMs_ = doorSampleMs_ = buildSampleMs_ = bdoorSampleMs_ = 0;
    prodSampleMs_ = 0;
    researchSampleMs_ = 0;
    contCensusMs_ = 0;
    authReassertMs_ = 0;
    // Config gates, ownRanks_ and every OUTBOUND seq counter are deliberately
    // preserved (see the header comment).
    coop::logLine("[load] session reset: pointer caches, session maps, change gates cleared");
}

void Replicator::ingest(Inbound& in) {
    std::deque<InboundEntity> got;
    in.drainEntities(got);
    if (got.empty()) return;
    unsigned long now = nowMs();
    for (std::deque<InboundEntity>::iterator it = got.begin(); it != got.end(); ++it) {
        // Wire v35: index the interp ring on the SENDER's capture time mapped
        // into the local clock, not the arrival time - path jitter (Steam
        // relay) otherwise smears straight into the snapshot spacing and the
        // buffer starves into extrapolation/snap cycles (the jumpy remote-
        // player movement). Mapping = sendMs + min-tracked offset (see
        // PeerClock); clamped to 'now' so a stamp can never land in the future.
        unsigned long t = now;
        if (sendStamp_) {
            PeerClock& pc = peerClock_[it->ownerId];
            long off = (long)(now - (unsigned long)it->sendMs);
            if (!pc.have) {
                pc.offsetMs = off; pc.have = true; pc.lastCreepMs = now;
            } else {
                unsigned long dt = now - pc.lastCreepMs;
                if (dt >= 500) { // creep ~2 ms/s toward slower routes
                    pc.offsetMs += (long)(dt / 500);
                    pc.lastCreepMs = now;
                }
                if (off < pc.offsetMs) pc.offsetMs = off;
            }
            t = (unsigned long)((long)it->sendMs + pc.offsetMs);
            if ((long)(t - now) > 0) t = now;
        }
        Driven& d = targets_[keyOf(it->e)];
        d.interp.push(it->e, t, now);
        d.lastSeenMs = now;
    }
}

void Replicator::setOwnedContainerHand(const unsigned int hand[5]) {
    ownedContainers_.clear();
    Key k; k.t = hand[0]; k.c = hand[1]; k.cs = hand[2]; k.i = hand[3]; k.s = hand[4];
    ownedContainers_.insert(k);
}

void Replicator::ingestInv(Inbound& in) {
    std::deque<InboundInv> got;
    in.drainInv(got);
    for (std::deque<InboundInv>::iterator it = got.begin(); it != got.end(); ++it) {
        Key k; k.t = it->cKey[0]; k.c = it->cKey[1]; k.cs = it->cKey[2];
        k.i = it->cKey[3]; k.s = it->cKey[4];
        // Protocol 34: a placer-key row resolves through OUR build maps to
        // the LOCAL building hand (own placement = own hand; the host's
        // placement = our minted proxy). An unresolvable key (mint not
        // landed yet / refused / tombstoned) is dropped - the sender's 5 s
        // safety resend re-delivers once the mint exists.
        if (it->keyKind == 1) {
            std::map<Key, OwnBuild>::iterator ob = ownBuilds_.find(k);
            if (ob != ownBuilds_.end()) {
                if (ob->second.removed) continue;
                k.t = ob->second.hand[0]; k.c = ob->second.hand[1];
                k.cs = ob->second.hand[2]; k.i = ob->second.hand[3];
                k.s = ob->second.hand[4];
            } else {
                std::map<Key, PeerBuild>::iterator pb = peerBuilds_.find(k);
                if (pb == peerBuilds_.end() || pb->second.minted != 1 ||
                    pb->second.removed)
                    continue;
                k.t = pb->second.localHand[0]; k.c = pb->second.localHand[1];
                k.cs = pb->second.localHand[2]; k.i = pb->second.localHand[3];
                k.s = pb->second.localHand[4];
            }
        }
        InvRecv& r = invRecv_[k];
        r.ownerId = it->ownerId;
        r.items   = it->items; // latest snapshot supersedes
        r.dirty   = true;
    }
}

void Replicator::latchTabs(const std::vector<std::pair<u32, u32> >& ctnrs) {
    if (!squadSync_) return; // legacy per-tick ranking needs no state
    for (unsigned int i = 0; i < ctnrs.size(); ++i) {
        if (tabRank_.find(ctnrs[i]) != tabRank_.end()) continue;
        unsigned int next = (unsigned int)tabRank_.size();
        tabRank_[ctnrs[i]] = next;
        if (next >= 2) { // session-start seeding of the standard 2-tab save is silent
            char b[96];
            _snprintf(b, sizeof(b) - 1, "[squad] LATCH cont=%u,%u rank=%u",
                      ctnrs[i].first, ctnrs[i].second, next);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
}

unsigned int Replicator::tabRankFor(const std::pair<u32, u32>& key,
                                    const std::vector<std::pair<u32, u32> >& ctnrs) const {
    if (squadSync_) {
        std::map<std::pair<u32, u32>, unsigned int>::const_iterator it =
            tabRank_.find(key);
        return it == tabRank_.end() ? 0xFFFFFFFFu : it->second;
    }
    return (unsigned int)(std::lower_bound(ctnrs.begin(), ctnrs.end(), key)
                          - ctnrs.begin());
}

void Replicator::publishOwned(GameWorld* gw, NetLink& net, u32 ownerId) {
    // Capture the OWNED squad subset first, then (Stage 4) the nearby world NPCs.
    // The net layer chunks the whole vector into datagram-sized batches, so the
    // count is only bounded by MAX_PUBLISH (a bar holds well under that).
    const unsigned int MAX_PUBLISH = 160;
    static EntityState raw[MAX_PUBLISH]; // all squad members (pre ownership filter)
    static EntityState buf[MAX_PUBLISH]; // owned subset (+ NPCs) actually published
    // Both clients load the SAME save, so the shared playerCharacters list is
    // identical on each. Capture the WHOLE squad, then partition it by SQUAD TAB:
    // a Kenshi squad tab is a Platoon, and a member's tab identity is carried in its
    // hand's CONTAINER (hContainer,hContainerSerial). Rank the DISTINCT containers
    // (sorted -> the same ordering on both machines, save-stable), and own a member
    // iff its tab's rank is in ownRanks_. So each player owns WHOLE squad tabs (host
    // tab 0, join tab 1 by default) and the streams are disjoint (Doctrine 8).
    // On a single-tab save only rank 0 exists, so the join owns nothing and the prior
    // one-directional behaviour is preserved exactly. ownHands_ records owned keys
    // for the drive-exclusion guard.
    unsigned int nSquad = engine::captureSquad(gw, /*leaderOnly*/ false, raw, MAX_PUBLISH);
    std::vector<std::pair<u32, u32> > ctnrs; // distinct squad-tab containers, sorted
    ctnrs.reserve(nSquad);
    for (unsigned int i = 0; i < nSquad; ++i)
        ctnrs.push_back(std::make_pair(raw[i].hContainer, raw[i].hContainerSerial));
    std::sort(ctnrs.begin(), ctnrs.end());
    ctnrs.erase(std::unique(ctnrs.begin(), ctnrs.end()), ctnrs.end());
    // Protocol 35 rank latch: with squad sync on, ranks are assigned once
    // (first census = the sorted order, identical to the legacy ranking) and
    // newly-seen containers APPEND - a mid-session move/createSquad can never
    // reshuffle existing ranks and silently flip whole-tab ownership.
    latchTabs(ctnrs);
    ownHands_.clear();
    unsigned int n = 0;
    for (unsigned int i = 0; i < nSquad && n < MAX_PUBLISH; ++i) {
        std::pair<u32, u32> key(raw[i].hContainer, raw[i].hContainerSerial);
        unsigned int rank = tabRankFor(key, ctnrs);
        // Empty ownRanks_ (never configured) is a safety fallback to the first tab,
        // so a missing setOwnRanks never makes us stream every tab or nothing.
        bool owned = ownRanks_.empty() ? (rank == 0u) : (ownRanks_.count(rank) != 0);
        // Ownership pins (protocols 23 + 35): a RECRUIT belongs to its
        // RECRUITER and a MOVED member to its MOVER regardless of which local
        // tab rank the engine parked it in (recruit_probe: a join recruit
        // landed in the host-owned rank-0 container). Our own edges always
        // publish; hands the peer authored never do.
        Key hk = keyOf(raw[i]);
        if (pinOwned_.count(hk))     owned = true;
        else if (pinPeer_.count(hk)) owned = false;
        if (!owned) continue;
        buf[n++] = raw[i];
        ownHands_.insert(hk);
    }
    // Host also streams nearby world NPCs (host-authoritative world). The join leaves
    // streamNpcs_ off, so on the join this publishes ONLY its owned squad subset.
    if (streamNpcs_ && n < MAX_PUBLISH)
        n += engine::captureNpcs(gw, buf + n, MAX_PUBLISH - n);
    net.setOwnedEntities(ownerId, buf, n);

    // Refresh the (sticky) attacker map from this tick's combat intents: a captured
    // entity with a combat-stance task carries its target in the subject fields, so it
    // is the ATTACKER of that subject. Stamp lastSeen=now; entries persist (recency
    // window below) so a KO/death edge - where the attacker has already dropped its
    // now-fallen target - can still recover who did it. Prune entries older than the
    // window so stale pairings don't mis-attribute a later, unrelated death.
    // An ACTIVE (slot-holding) attacker outranks a WAITING one: the queued crowd
    // also targets the victim, but the KO/death lands from whoever is swinging.
    unsigned long nowPub = nowMs();
    for (unsigned int i = 0; i < n; ++i) {
        const EntityState& e = buf[i];
        if (!coop::taskIsCombat(e.task)) continue;
        Key victim; victim.t = e.sType; victim.c = e.sContainer;
        victim.cs = e.sContainerSerial; victim.i = e.sIndex; victim.s = e.sSerial;
        if (coop::taskIsCombatWait(e.task)) {
            std::map<Key, std::pair<Key, unsigned long> >::iterator ex =
                attackerOf_.find(victim);
            if (ex != attackerOf_.end() &&
                (nowPub - ex->second.second) <= ATTR_WINDOW_MS)
                continue; // a live ACTIVE stamp wins over the waiting crowd
        }
        attackerOf_[victim] = std::make_pair(keyOf(e), nowPub);
    }
    for (std::map<Key, std::pair<Key, unsigned long> >::iterator pr = attackerOf_.begin();
         pr != attackerOf_.end(); ) {
        if (nowPub - pr->second.second > ATTR_WINDOW_MS) attackerOf_.erase(pr++);
        else ++pr;
    }

    // Phase B (protocol 16): refresh the combat-scoped NPC vitals set. The NPC
    // segment of buf is [nOwned..n) (host only - the join streams no NPCs). An
    // NPC qualifies while it FIGHTS (combat stance), is FOUGHT (a victim in the
    // attacker map or the subject of any captured combat intent), or is DOWN /
    // DEAD in interest. A stale grace window keeps vitals flowing over a brief
    // stance flicker, then the NPC drops back to the events-only model.
    if (streamNpcs_) {
        unsigned int nOwned = (unsigned int)ownHands_.size();
        std::set<Key> npcKeys;
        for (unsigned int i = nOwned; i < n; ++i) npcKeys.insert(keyOf(buf[i]));
        for (unsigned int i = nOwned; i < n; ++i) {
            const EntityState& e = buf[i];
            Key k = keyOf(e);
            bool fighting = coop::taskIsCombat(e.task);
            bool fought   = attackerOf_.find(k) != attackerOf_.end();
            bool down     = coop::bodyIsDown(e.bodyState) ||
                            (e.bodyState & BODY_DEAD) != 0;
            if (fighting || fought || down) medNpc_[k] = nowPub;
        }
        // A combat intent's SUBJECT is a victim; if it's a world NPC (not a
        // player-squad body - those have their own owner-authoritative stream),
        // its vitals qualify too, even before it fights back.
        for (unsigned int i = 0; i < n; ++i) {
            const EntityState& e = buf[i];
            if (!coop::taskIsCombat(e.task)) continue;
            Key victim; victim.t = e.sType; victim.c = e.sContainer;
            victim.cs = e.sContainerSerial; victim.i = e.sIndex; victim.s = e.sSerial;
            if (npcKeys.find(victim) != npcKeys.end()) medNpc_[victim] = nowPub;
        }
        const unsigned long MEDNPC_STALE_MS = 10000;
        for (std::map<Key, unsigned long>::iterator mit = medNpc_.begin();
             mit != medNpc_.end(); ) {
            if (nowPub - mit->second > MEDNPC_STALE_MS) medNpc_.erase(mit++);
            else ++mit;
        }
    }

    // Emit reliable transition events on bodyState edges. Continuous bodyState
    // already self-heals the down/dead POSTURE over the unreliable channel; the
    // event guarantees the TRANSITION moment is delivered exactly once (a dropped
    // batch can't lose a death), which is what combat (L5) will build on.
    for (unsigned int i = 0; i < n; ++i) {
        const EntityState& e = buf[i];
        Key k = keyOf(e);
        std::map<Key, HostBody>::iterator pit = hostBody_.find(k);
        u16 prev = (pit != hostBody_.end()) ? pit->second.bs : 0;
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
                // Causality: if this victim was being meleed within the recency window,
                // stamp the attacker as the ACTOR (combat KO/death "downed BY X"). A
                // KO/death from a non-combat cause (scaffold kill, fall) leaves it zeroed.
                std::map<Key, std::pair<Key, unsigned long> >::iterator ait = attackerOf_.find(k);
                bool haveActor = (ait != attackerOf_.end());
                if (haveActor) {
                    const Key& a = ait->second.first;
                    ev.aType = a.t; ev.aContainer = a.c; ev.aContainerSerial = a.cs;
                    ev.aIndex = a.i; ev.aSerial = a.s;
                }
                net.queueEvent(ev);
                char b[200]; _snprintf(b, sizeof(b) - 1,
                    "[event] SEND id=%u ev=%u hand=%u,%u,%u,%u,%u actor=%u,%u bs %u->%u",
                    ev.eventId, (unsigned)evType, e.hType, e.hContainer,
                    e.hContainerSerial, e.hIndex, e.hSerial,
                    haveActor ? ev.aIndex : 0u, haveActor ? ev.aSerial : 0u,
                    (unsigned)prev, (unsigned)cur);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        HostBody& hb = hostBody_[k];
        // Carried-body sync (protocol 18): emit reliable pickup/drop edges on
        // carryingObject transitions of OWNED members AND (host only) streamed
        // world NPCs (the entity's streamer authors the edge; TASK_CARRY_BODY +
        // BODY_CARRIED are the self-heal). captureOne stamps TASK_CARRY_BODY +
        // the carried hand as the subject whenever the character carries (combat
        // overrides it, so a carrier that starts fighting reads as a drop here -
        // the engine drops the body to fight anyway). The NPC extension covers
        // the 2026-07-07 session gap: a host-side NPC hauling a downed PC never
        // reached the join. Join NPCs never take this branch (streamNpcs_ is
        // host-only), so NPC carry authorship stays one-directional by design.
        bool carryAuthor = ownHands_.find(k) != ownHands_.end() || streamNpcs_;
        if (carrySync_ && carryAuthor) {
            bool carryNow = coop::taskIsCarry(e.task);
            bool sameBody = hb.carrying && carryNow &&
                            hb.carried[3] == e.sIndex && hb.carried[4] == e.sSerial;
            if ((hb.carrying && !carryNow) || (hb.carrying && carryNow && !sameBody)) {
                // Drop edge (also fires as the first half of a carried-body
                // SWAP): subject = the previously carried body, actor = us.
                EventPacket ev; memset(&ev, 0, sizeof(ev));
                ev.type = (u8)PKT_EVENT; ev.event = (u8)EVT_DROP_BODY;
                ev.ownerId = ownerId;    ev.eventId = nextEventId_++;
                ev.sType = hb.carried[0]; ev.sContainer = hb.carried[1];
                ev.sContainerSerial = hb.carried[2];
                ev.sIndex = hb.carried[3]; ev.sSerial = hb.carried[4];
                ev.aType = e.hType; ev.aContainer = e.hContainer;
                ev.aContainerSerial = e.hContainerSerial;
                ev.aIndex = e.hIndex; ev.aSerial = e.hSerial;
                ev.arg = 1.0f; // ragdoll ground drop (the body is KO'd)
                net.queueEvent(ev);
                char b[176]; _snprintf(b, sizeof(b) - 1,
                    "[carry] SEND DROP id=%u carrier=%u,%u carried=%u,%u",
                    ev.eventId, e.hIndex, e.hSerial, hb.carried[3], hb.carried[4]);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            if (carryNow && !sameBody) {
                // Pickup edge: subject = the carried body, actor = us.
                EventPacket ev; memset(&ev, 0, sizeof(ev));
                ev.type = (u8)PKT_EVENT; ev.event = (u8)EVT_PICKUP_BODY;
                ev.ownerId = ownerId;    ev.eventId = nextEventId_++;
                ev.sType = e.sType; ev.sContainer = e.sContainer;
                ev.sContainerSerial = e.sContainerSerial;
                ev.sIndex = e.sIndex; ev.sSerial = e.sSerial;
                ev.aType = e.hType; ev.aContainer = e.hContainer;
                ev.aContainerSerial = e.hContainerSerial;
                ev.aIndex = e.hIndex; ev.aSerial = e.hSerial;
                net.queueEvent(ev);
                char b[176]; _snprintf(b, sizeof(b) - 1,
                    "[carry] SEND PICKUP id=%u carrier=%u,%u carried=%u,%u",
                    ev.eventId, e.hIndex, e.hSerial, e.sIndex, e.sSerial);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            hb.carrying = carryNow;
            if (carryNow) {
                hb.carried[0] = e.sType; hb.carried[1] = e.sContainer;
                hb.carried[2] = e.sContainerSerial;
                hb.carried[3] = e.sIndex; hb.carried[4] = e.sSerial;
            }
        }
        // Furniture occupancy (protocol 19): emit reliable enter/exit edges on
        // BODY_IN_BED/BODY_IN_CAGE transitions, same authorship scope as carry
        // (owned members + host-streamed world NPCs). The furniture HAND is not
        // in the stream (an unconscious occupant has no task subject), so it is
        // read off the LOCAL character (inWhat) at the ENTER edge and remembered
        // in HostBody for the matching EXIT. Scoped away from CONSCIOUS bed
        // poses (USE_BED / USE_BED_ORDER / SLEEP_ON_FLOOR): those stream their
        // TASK and the peer's copy walks in via the validated L3 fixture-pose
        // path (bed_pose) - an ENTER event would teleport it in and fight that.
        if (furnSync_ && carryAuthor && !engine::taskIsBedPose((int)e.task)) {
            int curKind = (cur & BODY_IN_BED) ? 1 : ((cur & BODY_IN_CAGE) ? 2 : 0);
            if (curKind != hb.furnKind) {
                if (hb.furnKind != 0) {
                    // Exit edge: subject = occupant, actor = the remembered furniture.
                    EventPacket ev; memset(&ev, 0, sizeof(ev));
                    ev.type = (u8)PKT_EVENT; ev.event = (u8)EVT_EXIT_FURNITURE;
                    ev.ownerId = ownerId;    ev.eventId = nextEventId_++;
                    ev.sType = e.hType; ev.sContainer = e.hContainer;
                    ev.sContainerSerial = e.hContainerSerial;
                    ev.sIndex = e.hIndex; ev.sSerial = e.hSerial;
                    ev.aType = hb.furn[0]; ev.aContainer = hb.furn[1];
                    ev.aContainerSerial = hb.furn[2];
                    ev.aIndex = hb.furn[3]; ev.aSerial = hb.furn[4];
                    ev.arg = (f32)hb.furnKind;
                    net.queueEvent(ev);
                    char b[176]; _snprintf(b, sizeof(b) - 1,
                        "[furn] SEND EXIT id=%u occ=%u,%u furn=%u,%u kind=%d",
                        ev.eventId, e.hIndex, e.hSerial, hb.furn[3], hb.furn[4],
                        hb.furnKind);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    // Protocol 36 race guard: a stale in-flight PEER-ENTER
                    // must not re-jail this body right after we freed it.
                    ownFurnExit_[keyOf(e)] = nowPub;
                    hb.furnKind = 0;
                    hb.furn[0] = hb.furn[1] = hb.furn[2] = hb.furn[3] = hb.furn[4] = 0;
                }
                if (curKind != 0) {
                    // Enter edge: the local occupant knows WHICH furniture (inWhat).
                    // An unreadable hand this frame leaves furnKind 0 so the edge
                    // re-attempts next publish (the bit is still streaming).
                    engine::FurnitureRead fr;
                    Character* oc = engine::resolve(e);
                    if (oc && engine::readFurniture(oc, &fr) && fr.valid &&
                        fr.kind == curKind) {
                        EventPacket ev; memset(&ev, 0, sizeof(ev));
                        ev.type = (u8)PKT_EVENT; ev.event = (u8)EVT_ENTER_FURNITURE;
                        ev.ownerId = ownerId;    ev.eventId = nextEventId_++;
                        ev.sType = e.hType; ev.sContainer = e.hContainer;
                        ev.sContainerSerial = e.hContainerSerial;
                        ev.sIndex = e.hIndex; ev.sSerial = e.hSerial;
                        ev.aType = fr.furn[0]; ev.aContainer = fr.furn[1];
                        ev.aContainerSerial = fr.furn[2];
                        ev.aIndex = fr.furn[3]; ev.aSerial = fr.furn[4];
                        ev.arg = (f32)curKind;
                        net.queueEvent(ev);
                        hb.furnKind = curKind;
                        for (int fi = 0; fi < 5; ++fi) hb.furn[fi] = fr.furn[fi];
                        char b[176]; _snprintf(b, sizeof(b) - 1,
                            "[furn] SEND ENTER id=%u occ=%u,%u furn=%u,%u kind=%d",
                            ev.eventId, e.hIndex, e.hSerial, fr.furn[3], fr.furn[4],
                            curKind);
                        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    }
                }
            }
        }
        hb.bs = cur; hb.seenMs = nowPub;
    }
    // Carried-body sync: a carrier that VANISHED from the stream mid-carry
    // (hauled the body out of interest, despawned) can never author its drop
    // edge from a buf transition above - the peer's copy would carry forever
    // (npc_carry run 123255: the NPC walked M2 ~700u out of the interest
    // sphere and the join never saw a DROP). After a short absence debounce
    // (beyond interest-boundary flicker), author the DROP for it here; the
    // peer releases its copy, which then rides the ordinary down channels.
    if (carrySync_) {
        const unsigned long CARRY_GONE_MS = 3000;
        std::set<Key> bufKeys;
        for (unsigned int i = 0; i < n; ++i) bufKeys.insert(keyOf(buf[i]));
        for (std::map<Key, HostBody>::iterator hit = hostBody_.begin();
             hit != hostBody_.end(); ++hit) {
            HostBody& hb = hit->second;
            if (!hb.carrying) continue;
            if (bufKeys.find(hit->first) != bufKeys.end()) continue;
            if (nowPub - hb.seenMs < CARRY_GONE_MS) continue;
            const Key& ck = hit->first;
            EventPacket ev; memset(&ev, 0, sizeof(ev));
            ev.type = (u8)PKT_EVENT; ev.event = (u8)EVT_DROP_BODY;
            ev.ownerId = ownerId;    ev.eventId = nextEventId_++;
            ev.sType = hb.carried[0]; ev.sContainer = hb.carried[1];
            ev.sContainerSerial = hb.carried[2];
            ev.sIndex = hb.carried[3]; ev.sSerial = hb.carried[4];
            ev.aType = ck.t; ev.aContainer = ck.c; ev.aContainerSerial = ck.cs;
            ev.aIndex = ck.i; ev.aSerial = ck.s;
            ev.arg = 1.0f; // ragdoll ground drop (the body is KO'd)
            net.queueEvent(ev);
            hb.carrying = false;
            char b[176]; _snprintf(b, sizeof(b) - 1,
                "[carry] SEND DROP id=%u carrier=%u,%u carried=%u,%u (carrier left stream)",
                ev.eventId, ck.i, ck.s, hb.carried[3], hb.carried[4]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
    // Furniture occupancy: an occupant that VANISHED from the stream mid-
    // occupancy (left interest, despawned) can never author its exit edge from
    // a buf transition above - the peer's copy would stay in the bed/cage
    // forever with nothing correcting it (the npc_carry lesson applied to the
    // stateful attach). After the same absence debounce, author the EXIT here;
    // if the body later re-enters the stream still occupied, the bit re-streams
    // and the enter edge re-fires (idempotent on the receiver).
    if (furnSync_) {
        const unsigned long FURN_GONE_MS = 3000;
        std::set<Key> bufKeys2;
        for (unsigned int i = 0; i < n; ++i) bufKeys2.insert(keyOf(buf[i]));
        for (std::map<Key, HostBody>::iterator hit = hostBody_.begin();
             hit != hostBody_.end(); ++hit) {
            HostBody& hb = hit->second;
            if (hb.furnKind == 0) continue;
            if (bufKeys2.find(hit->first) != bufKeys2.end()) continue;
            if (nowPub - hb.seenMs < FURN_GONE_MS) continue;
            const Key& ok = hit->first;
            EventPacket ev; memset(&ev, 0, sizeof(ev));
            ev.type = (u8)PKT_EVENT; ev.event = (u8)EVT_EXIT_FURNITURE;
            ev.ownerId = ownerId;    ev.eventId = nextEventId_++;
            ev.sType = ok.t; ev.sContainer = ok.c; ev.sContainerSerial = ok.cs;
            ev.sIndex = ok.i; ev.sSerial = ok.s;
            ev.aType = hb.furn[0]; ev.aContainer = hb.furn[1];
            ev.aContainerSerial = hb.furn[2];
            ev.aIndex = hb.furn[3]; ev.aSerial = hb.furn[4];
            ev.arg = (f32)hb.furnKind;
            net.queueEvent(ev);
            char b[176]; _snprintf(b, sizeof(b) - 1,
                "[furn] SEND EXIT id=%u occ=%u,%u furn=%u,%u kind=%d (occupant left stream)",
                ev.eventId, ok.i, ok.s, hb.furn[3], hb.furn[4], hb.furnKind);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            hb.furnKind = 0;
            hb.furn[0] = hb.furn[1] = hb.furn[2] = hb.furn[3] = hb.furn[4] = 0;
        }
    }

    // Third-party placement edges (protocol 36): drain the PEER-ENTER events
    // applyTargets detected on peer-owned driven bodies (host = world
    // authority; the occupant's owner applies them to its own KO'd body).
    if (furnSync_ && !furnPeerPend_.empty()) {
        for (unsigned int pi = 0; pi < furnPeerPend_.size(); ++pi) {
            const PendFurnEnter& pe = furnPeerPend_[pi];
            EventPacket ev; memset(&ev, 0, sizeof(ev));
            ev.type = (u8)PKT_EVENT; ev.event = (u8)EVT_ENTER_FURNITURE;
            ev.ownerId = ownerId;    ev.eventId = nextEventId_++;
            ev.sType = pe.occ.t; ev.sContainer = pe.occ.c;
            ev.sContainerSerial = pe.occ.cs;
            ev.sIndex = pe.occ.i; ev.sSerial = pe.occ.s;
            ev.aType = pe.furn[0]; ev.aContainer = pe.furn[1];
            ev.aContainerSerial = pe.furn[2];
            ev.aIndex = pe.furn[3]; ev.aSerial = pe.furn[4];
            ev.arg = (f32)pe.kind;
            net.queueEvent(ev);
            char b[176]; _snprintf(b, sizeof(b) - 1,
                "[furn] SEND PEER-ENTER id=%u occ=%u,%u furn=%u,%u kind=%d",
                ev.eventId, pe.occ.i, pe.occ.s, pe.furn[3], pe.furn[4], pe.kind);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        furnPeerPend_.clear();
    }
    // Age out entities that left the interest set long ago (step 6): an unbounded
    // hostBody_ leaks a session's worth of passers-by. 60 s is far beyond any
    // interest-boundary flicker, so a pruned entry that returns just re-baselines
    // (prev=0) - and a re-baselined DOWN body re-emits at most one KO edge.
    const unsigned long HOSTBODY_STALE_MS = 60000;
    for (std::map<Key, HostBody>::iterator hit = hostBody_.begin(); hit != hostBody_.end(); ) {
        if (nowPub - hit->second.seenMs > HOSTBODY_STALE_MS) hostBody_.erase(hit++);
        else ++hit;
    }
}

// Quantized fingerprint of a medical snapshot (FNV-1a over rounded fields):
// sub-noise wobble (blood regen ticks at ~0.01) must not chatter the reliable
// channel, so blood/flesh/bandaging quantize to 0.5 units and bleed to 0.05.
// Protocol 16: covers the FULL anatomy (flesh+stun+bandage+juryRig per part)
// plus the 4 LimbStates - a stun hit or a limb loss re-fingerprints too.
static coop::u32 medicalHash(const engine::MedicalRead& m) {
    long q[4 + 12 * 4 + 4 + 2];
    memset(q, 0, sizeof(q));
    q[0] = (long)(m.blood * 2.0f);
    q[1] = (long)(m.bleedRate * 20.0f);
    q[2] = m.unconscious ? 1 : 0;
    q[3] = m.dead ? 1 : 0;
    // Protocol 29: hunger quantizes to 0.1 units (engine scale ~0..3; the
    // probe's heaviest decay was ~0.024/s, so a bucket flips slower than the
    // 3 s safety resend - the fold-in adds no traffic).
    q[4 + 12 * 4 + 4 + 0] = (long)(m.hunger * 10.0f);
    q[4 + 12 * 4 + 4 + 1] = (long)(m.fed * 10.0f);
    unsigned int n = m.nParts; if (n > 12) n = 12;
    for (unsigned int i = 0; i < n; ++i) {
        const engine::MedPartRead& p = m.parts[i];
        if (!p.used) continue;
        q[4 + i * 4 + 0] = (long)(p.flesh * 2.0f);
        q[4 + i * 4 + 1] = (long)(p.fleshStun * 2.0f);
        q[4 + i * 4 + 2] = (long)(p.bandaging * 2.0f);
        q[4 + i * 4 + 3] = (long)(p.juryRig * 2.0f);
    }
    for (int i = 0; i < 4; ++i) q[4 + 12 * 4 + i] = (long)m.limbState[i];
    coop::u32 h = 2166136261u;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(q);
    for (unsigned int i = 0; i < sizeof(q); ++i) { h ^= p[i]; h *= 16777619u; }
    return h ? h : 1u; // 0 is the "never sent" sentinel
}

// Quantized fingerprint of a stats snapshot (protocol 17; the medicalHash
// pattern). Raw stat levels creep by tiny XP fractions every swing; 0.1-unit
// quantization keeps the reliable channel silent until a stat visibly moves.
// xp and freeAttributePoints ride at their natural granularity.
static coop::u32 statsHash(const engine::StatsRead& s) {
    long q[40 + 2];
    memset(q, 0, sizeof(q));
    unsigned int n = s.nStats; if (n > 40) n = 40;
    for (unsigned int i = 0; i < n; ++i)
        q[i] = (s.stats[i] >= 0.0f) ? (long)(s.stats[i] * 10.0f) : -1;
    q[40] = (s.xp >= 0.0f) ? (long)s.xp : -1;
    q[41] = (s.freeAttribPts >= 0.0f) ? (long)s.freeAttribPts : -1;
    coop::u32 h = 2166136261u;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(q);
    for (unsigned int i = 0; i < sizeof(q); ++i) { h ^= p[i]; h *= 16777619u; }
    return h ? h : 1u; // 0 is the "never sent" sentinel
}

// Fill a protocol-16 medical packet from a local read (shared by the owned-
// member and combat-scoped-NPC publish paths). subj = the subject hand in
// readObjectHand layout (t, c, cs, i, s).
static void fillMedicalPacket(MedicalPacket& pkt, const unsigned int subj[5],
                              coop::u32 ownerId, const engine::MedicalRead& mr) {
    memset(&pkt, 0, sizeof(pkt));
    pkt.type    = (u8)PKT_MEDICAL;
    pkt.ownerId = ownerId;
    pkt.sType = subj[0]; pkt.sContainer = subj[1]; pkt.sContainerSerial = subj[2];
    pkt.sIndex = subj[3]; pkt.sSerial = subj[4];
    pkt.blood     = mr.blood;
    pkt.bleedRate = mr.bleedRate;
    pkt.hunger    = mr.hunger; // -1 when not carried (hungerSync off)
    pkt.fed       = mr.fed;
    pkt.flags = (mr.unconscious ? MED_UNCONSCIOUS : 0) | (mr.dead ? MED_DEAD : 0);
    unsigned int n = mr.nParts; if (n > MED_PARTS_MAX) n = MED_PARTS_MAX;
    pkt.nParts = (u8)n;
    for (unsigned int i = 0; i < n; ++i) {
        const engine::MedPartRead& pr = mr.parts[i];
        MedPartEntry& e = pkt.parts[i];
        e.used      = pr.used ? 1 : 0;
        e.partType  = pr.partType;
        e.side      = pr.side;
        e.flesh     = pr.flesh;
        e.fleshStun = pr.fleshStun;
        e.bandaging = pr.bandaging;
        e.juryRig   = pr.juryRig;
    }
    for (int i = 0; i < 4; ++i) {
        pkt.limbState[i] = mr.limbState[i];
        strncpy(pkt.limbSid[i], mr.limbSid[i], sizeof(pkt.limbSid[i]) - 1);
    }
}

void Replicator::publishMedical(GameWorld* gw, NetLink& net, u32 ownerId) {
    const unsigned long RESEND_MS       = 3000; // safety resend (reliable, but cheap insurance)
    const unsigned long MIN_SEND_MS     = 400;  // sampling floor: an active bleed changes the
                                                // fingerprint EVERY tick, and 60 Hz reliable
                                                // sends would flood the channel (and lag the
                                                // copy). ~2.5 Hz is plenty for vitals.
    const unsigned long NPC_MIN_SEND_MS = 1000; // combat-scoped NPCs: ~1 Hz is plenty (a
                                                // whole brawl's worth of bodies shares the
                                                // channel with the squad streams)
    unsigned long now = nowMs();
    // One publish list: owned squad members first, then (host) the combat-
    // scoped world NPCs from medNpc_ (Phase B).
    std::vector<std::pair<Key, bool> > list; // (key, isNpc)
    for (std::set<Key>::const_iterator it = ownHands_.begin(); it != ownHands_.end(); ++it)
        list.push_back(std::make_pair(*it, false));
    if (streamNpcs_) {
        for (std::map<Key, unsigned long>::const_iterator it = medNpc_.begin();
             it != medNpc_.end(); ++it) {
            if (ownHands_.find(it->first) != ownHands_.end()) continue;
            list.push_back(std::make_pair(it->first, true));
        }
    }
    for (unsigned int li = 0; li < list.size(); ++li) {
        const Key& k  = list[li].first;
        bool isNpc    = list[li].second;
        unsigned long minSendMs = isNpc ? NPC_MIN_SEND_MS : MIN_SEND_MS;
        unsigned int hand[5] = { k.t, k.c, k.cs, k.i, k.s };
        engine::MedicalRead mr;
        if (!engine::readMedicalByHand(hand, &mr) || !mr.valid) continue;
        // Protocol 29 A/B hatch: with hungerSync off the fields go out as -1
        // (not carried) and drop out of the fingerprint, so the medical
        // channel behaves exactly as pre-29.
        if (!hungerSync_) { mr.hunger = -1.0f; mr.fed = -1.0f; }
        u32 h = medicalHash(mr);
        MedPub& mp = medPub_[k];
        // Limb-loss transition events (doctrine 16: the reliable event carries
        // the edge, the packet's limbState[] is the self-heal). Detected on the
        // PUBLISH sample so owned members and streamed NPCs share the path.
        for (int i = 0; i < 4; ++i) {
            u8 prev = mp.limbPrev[i], cur = mr.limbState[i];
            if (cur != LIMB_STATE_UNKNOWN && prev != LIMB_STATE_UNKNOWN && cur != prev) {
                u8 evType = EVT_NONE;
                if ((cur == LIMB_WIRE_STUMP || cur == LIMB_WIRE_REPLACED) &&
                    (prev == LIMB_WIRE_ORIGINAL || prev == LIMB_WIRE_CRUSHED))
                    evType = EVT_AMPUTATE;
                else if (cur == LIMB_WIRE_CRUSHED && prev == LIMB_WIRE_ORIGINAL)
                    evType = EVT_CRUSH;
                if (evType != EVT_NONE) {
                    EventPacket ev;
                    memset(&ev, 0, sizeof(ev));
                    ev.type = (u8)PKT_EVENT; ev.event = evType;
                    ev.ownerId = ownerId;    ev.eventId = nextEventId_++;
                    ev.sType = k.t; ev.sContainer = k.c; ev.sContainerSerial = k.cs;
                    ev.sIndex = k.i; ev.sSerial = k.s;
                    ev.arg = (f32)i; // RobotLimbs::Limb
                    net.queueEvent(ev);
                    char eb[160]; _snprintf(eb, sizeof(eb) - 1,
                        "[med] LIMB-EVT SEND id=%u ev=%u hand=%u,%u limb=%d state %u->%u",
                        ev.eventId, (unsigned)evType, k.i, k.s, i,
                        (unsigned)prev, (unsigned)cur);
                    eb[sizeof(eb) - 1] = '\0'; coop::logLine(eb);
                    // Severed-item dedupe (JOIN only): our own damage sim just
                    // created a local severed-limb ground item, but the HOST's
                    // copy is canonical (it spawns one on event-apply and the
                    // world-item channel streams it back as a proxy). Destroy
                    // the local one so the join doesn't end up with two.
                    if (evType == EVT_AMPUTATE && !streamNpcs_ && !isNpc) {
                        int nd = engine::destroySeveredLimbsNear(gw, hand, 15.0f);
                        char db[120]; _snprintf(db, sizeof(db) - 1,
                            "[med] LIMB-ITEM DEDUPE hand=%u,%u destroyed=%d",
                            k.i, k.s, nd);
                        db[sizeof(db) - 1] = '\0'; coop::logLine(db);
                    }
                }
            }
            if (cur != LIMB_STATE_UNKNOWN) mp.limbPrev[i] = cur;
        }
        if (mp.lastSendMs != 0 && (now - mp.lastSendMs) < minSendMs) continue;
        if (h == mp.hash && (now - mp.lastSendMs) < RESEND_MS) continue;
        bool changed = (h != mp.hash);
        mp.hash = h; mp.lastSendMs = now;
        MedicalPacket pkt;
        fillMedicalPacket(pkt, hand, ownerId, mr);
        net.queueMedical(pkt);
        if (changed) { // periodic resends stay silent; changes are the signal
            // Head/chest/stomach summary: min flesh and min stun across ALL
            // parts (the fields the pre-16 stream never carried).
            float minFl = 1e9f, minSt = 1e9f;
            for (unsigned int i = 0; i < mr.nParts && i < 12; ++i) {
                if (!mr.parts[i].used) continue;
                if (mr.parts[i].flesh >= 0.0f && mr.parts[i].flesh < minFl)
                    minFl = mr.parts[i].flesh;
                if (mr.parts[i].fleshStun >= 0.0f && mr.parts[i].fleshStun < minSt)
                    minSt = mr.parts[i].fleshStun;
            }
            if (minFl > 1e8f) minFl = -1.0f;
            if (minSt > 1e8f) minSt = -1.0f;
            char b[240]; _snprintf(b, sizeof(b) - 1,
                "[med] SEND hand=%u,%u npc=%d blood=%.1f bleed=%.2f fl=%.1f,%.1f,%.1f,%.1f bd=%.1f,%.1f,%.1f,%.1f flags=%u nparts=%u pmin=%.1f smin=%.1f ls=%u,%u,%u,%u",
                k.i, k.s, isNpc ? 1 : 0, mr.blood, mr.bleedRate,
                mr.limbFlesh[0], mr.limbFlesh[1], mr.limbFlesh[2], mr.limbFlesh[3],
                mr.limbBand[0], mr.limbBand[1], mr.limbBand[2], mr.limbBand[3],
                (unsigned)pkt.flags, (unsigned)pkt.nParts, minFl, minSt,
                (unsigned)mr.limbState[0], (unsigned)mr.limbState[1],
                (unsigned)mr.limbState[2], (unsigned)mr.limbState[3]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
    // Age out entries no longer published (left the owned set / NPC set went
    // stale) - the map must not grow unbounded across saves or brawls.
    for (std::map<Key, MedPub>::iterator mit = medPub_.begin(); mit != medPub_.end(); ) {
        bool live = ownHands_.find(mit->first) != ownHands_.end() ||
                    medNpc_.find(mit->first) != medNpc_.end();
        if (!live) medPub_.erase(mit++);
        else ++mit;
    }
}

void Replicator::applyMedical(GameWorld* gw, Inbound& in, NetLink& net, u32 ownerId) {
    // 1. Drain received snapshots onto driven copies (never a body we own).
    std::deque<InboundMedical> got;
    in.drainMedical(got);
    for (std::deque<InboundMedical>::iterator it = got.begin(); it != got.end(); ++it) {
        const MedicalPacket& p = it->pkt;
        Key k; k.t = p.sType; k.c = p.sContainer; k.cs = p.sContainerSerial;
        k.i = p.sIndex; k.s = p.sSerial;
        if (ownHands_.find(k) != ownHands_.end()) continue; // never write our own truth
        unsigned int hand[5] = { k.t, k.c, k.cs, k.i, k.s };
        Character* c = engine::resolveCharByHand(hand[3], hand[4], hand[0], hand[1], hand[2]);
        if (!c) continue;
        engine::MedicalRead w;
        memset(&w, 0, sizeof(w));
        w.valid = true;
        w.dazed = -1.0f; // diagnostics-only, never on the wire
        // Protocol 29: hunger applies only when the sender carried it (>= 0)
        // AND our own hatch is on (A/B symmetry); writeMedical skips < 0.
        w.hunger = hungerSync_ ? p.hunger : -1.0f;
        w.fed    = hungerSync_ ? p.fed    : -1.0f;
        w.blood = p.blood; w.bleedRate = p.bleedRate;
        for (int i = 0; i < 4; ++i) {
            // Legacy 4-limb fields unused when nParts > 0 (writeMedical takes
            // the full-anatomy path); keep them inert regardless.
            w.limbFlesh[i] = -1.0f; w.limbBand[i] = -1.0f; w.limbMax[i] = -1.0f;
        }
        unsigned int n = p.nParts; if (n > 12) n = 12;
        w.nParts = n;
        for (unsigned int i = 0; i < n; ++i) {
            const MedPartEntry& e = p.parts[i];
            engine::MedPartRead& pr = w.parts[i];
            pr.used      = e.used != 0;
            pr.partType  = e.partType;
            pr.side      = e.side;
            pr.flesh     = e.flesh;
            pr.fleshStun = e.fleshStun;
            pr.bandaging = e.bandaging;
            pr.juryRig   = e.juryRig;
            pr.maxHealth = -1.0f;
        }
        w.unconscious = (p.flags & MED_UNCONSCIOUS) != 0;
        w.dead        = (p.flags & MED_DEAD) != 0;
        engine::writeMedical(c, w);
        // Limb-state self-heal (Phase C/D): reconcile stump/crushed/robotic
        // state with the owner's. The reliable EVT_AMPUTATE/EVT_CRUSH events
        // carry the transition moment; this closes any gap (late join, missed
        // pre-event state) and fits robotic replacements (sid + gw available).
        // The HOST (streamNpcs_ = world authority) creates the severed ground
        // item - it then streams to everyone via the world-item channel; the
        // join never creates one (the streamed copy is canonical).
        int lchg = engine::applyLimbStates(gw, c, p.limbState, p.limbSid,
                                           /*createSeveredItem*/streamNpcs_);
        if (lchg != 0) {
            char lb[160]; _snprintf(lb, sizeof(lb) - 1,
                "[med] LIMB APPLY hand=%u,%u mask=%d ls=%u,%u,%u,%u",
                k.i, k.s, lchg,
                (unsigned)p.limbState[0], (unsigned)p.limbState[1],
                (unsigned)p.limbState[2], (unsigned)p.limbState[3]);
            lb[sizeof(lb) - 1] = '\0'; coop::logLine(lb);
        }
        MedRecv& r = medRecv_[k];
        for (unsigned int i = 0; i < 12; ++i) {
            float band = (i < n && p.parts[i].used) ? p.parts[i].bandaging : -1.0f;
            r.recvBand[i] = band;
            // The owner's stream now reflects (or supersedes) anything we
            // forwarded; re-arm the detector against the new baseline.
            if (r.sentBand[i] >= 0.0f && band >= r.sentBand[i] - 0.25f)
                r.sentBand[i] = -1.0f;
        }
        r.have = true;
    }

    // 2. Treatment detector: local bandaging risen ABOVE the last received level
    // on a driven copy = first aid administered on THIS machine. Forward the
    // resulting levels (not the call stream) reliably to the owner. ~1 Hz per
    // body; sentBand[] suppresses re-sends while the owner's echo is in flight.
    // Protocol 16: keyed by anatomy part, so a head/chest bandage forwards too.
    const float         RISE_EPS = 0.5f;
    const unsigned long FWD_THROTTLE_MS = 1000;
    unsigned long now = nowMs();
    for (std::map<Key, MedRecv>::iterator it = medRecv_.begin(); it != medRecv_.end(); ++it) {
        const Key& k = it->first;
        MedRecv&   r = it->second;
        if (!r.have || (now - r.lastFwdMs) < FWD_THROTTLE_MS) continue;
        if (ownHands_.find(k) != ownHands_.end()) continue;
        unsigned int hand[5] = { k.t, k.c, k.cs, k.i, k.s };
        engine::MedicalRead mr;
        if (!engine::readMedicalByHand(hand, &mr) || !mr.valid) continue;
        TreatmentPacket tp;
        memset(&tp, 0, sizeof(tp));
        bool rise = false;
        int nRise = 0; float hiBand = -1.0f;
        for (unsigned int i = 0; i < 12; ++i) {
            tp.partBand[i] = -1.0f;
            float local = (i < mr.nParts && mr.parts[i].used) ? mr.parts[i].bandaging : -1.0f;
            if (local < 0.0f || r.recvBand[i] < 0.0f) continue;
            if (local > r.recvBand[i] + RISE_EPS &&
                (r.sentBand[i] < 0.0f || local > r.sentBand[i] + RISE_EPS)) {
                tp.partBand[i] = local;
                r.sentBand[i]  = local;
                rise = true; ++nRise;
                if (local > hiBand) hiBand = local;
            }
        }
        if (!rise) continue;
        r.lastFwdMs = now;
        tp.type    = (u8)PKT_TREATMENT;
        tp.ownerId = ownerId;
        tp.treatId = nextTreatId_++;
        tp.sType = k.t; tp.sContainer = k.c; tp.sContainerSerial = k.cs;
        tp.sIndex = k.i; tp.sSerial = k.s;
        net.queueTreatment(tp);
        char b[160]; _snprintf(b, sizeof(b) - 1,
            "[med] TREAT SEND id=%u hand=%u,%u parts=%d hi=%.1f",
            tp.treatId, k.i, k.s, nRise, hiBand);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::applyTreatments(GameWorld* gw, Inbound& in) {
    (void)gw;
    std::deque<InboundTreatment> got;
    in.drainTreatments(got);
    for (std::deque<InboundTreatment>::iterator it = got.begin(); it != got.end(); ++it) {
        const TreatmentPacket& p = it->pkt;
        Key k; k.t = p.sType; k.c = p.sContainer; k.cs = p.sContainerSerial;
        k.i = p.sIndex; k.s = p.sSerial;
        // Only the AUTHORITY applies a treatment to the real body: the owner of
        // a squad member, or the host for a combat-scoped world NPC (medNpc_).
        // A delta for a body we aren't authoritative for is a partition error
        // (log-visible, ignored).
        bool authority = ownHands_.find(k) != ownHands_.end() ||
                         (streamNpcs_ && medNpc_.find(k) != medNpc_.end());
        if (!authority) continue;
        Character* c = engine::resolveCharByHand(k.i, k.s, k.t, k.c, k.cs);
        if (!c) continue;
        int n = engine::applyBandageParts(c, p.partBand);
        char b[160]; _snprintf(b, sizeof(b) - 1,
            "[med] TREAT RECV id=%u hand=%u,%u applied=%d",
            p.treatId, k.i, k.s, n);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::publishStats(GameWorld* gw, NetLink& net, u32 ownerId) {
    (void)gw;
    const unsigned long RESEND_MS   = 5000; // safety resend (cheap insurance)
    const unsigned long MIN_SEND_MS = 1000; // stats creep every swing; ~1 Hz is plenty
    unsigned long now = nowMs();
    // Owned player-squad members ONLY. World NPCs are deliberately excluded:
    // their authoritative fights run on the host with the host's own (correct)
    // local stats, so streaming them would be traffic without a consumer.
    for (std::set<Key>::const_iterator it = ownHands_.begin(); it != ownHands_.end(); ++it) {
        const Key& k = *it;
        unsigned int hand[5] = { k.t, k.c, k.cs, k.i, k.s };
        engine::StatsRead sr;
        if (!engine::readStatsByHand(hand, &sr) || !sr.valid) continue;
        u32 h = statsHash(sr);
        StatsPub& sp = statsPub_[k];
        if (sp.lastSendMs != 0 && (now - sp.lastSendMs) < MIN_SEND_MS) continue;
        if (h == sp.hash && (now - sp.lastSendMs) < RESEND_MS) continue;
        bool changed = (h != sp.hash);
        sp.hash = h; sp.lastSendMs = now;
        StatsPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type    = (u8)PKT_STATS;
        pkt.ownerId = ownerId;
        pkt.sType = k.t; pkt.sContainer = k.c; pkt.sContainerSerial = k.cs;
        pkt.sIndex = k.i; pkt.sSerial = k.s;
        unsigned int n = sr.nStats; if (n > STATS_SLOT_MAX) n = STATS_SLOT_MAX;
        pkt.nStats = (u8)n;
        for (unsigned int i = 0; i < STATS_SLOT_MAX; ++i)
            pkt.stats[i] = (i < n) ? sr.stats[i] : -1.0f;
        pkt.xp                  = sr.xp;
        pkt.freeAttributePoints = sr.freeAttribPts;
        net.queueStats(pkt);
        if (changed) { // periodic resends stay silent; changes are the signal
            // str/dex/tough/stealth/athletics cover the scenario + the stats a
            // player watches; the full vector is in the packet regardless.
            // StatsEnumerated: STRENGTH=1 STEALTH=16 ATHLETICS=17 DEXTERITY=18
            // TOUGHNESS=21 (Enums.h).
            char b[200]; _snprintf(b, sizeof(b) - 1,
                "[stats] SEND hand=%u,%u str=%.1f dex=%.1f tough=%.1f stealth=%.1f athl=%.1f xp=%.0f fap=%.0f",
                k.i, k.s, sr.stats[1], sr.stats[18], sr.stats[21], sr.stats[16],
                sr.stats[17], sr.xp, sr.freeAttribPts);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
    // Age out entries that left the owned set (save reload, squad change).
    for (std::map<Key, StatsPub>::iterator sit = statsPub_.begin(); sit != statsPub_.end(); ) {
        if (ownHands_.find(sit->first) == ownHands_.end()) statsPub_.erase(sit++);
        else ++sit;
    }
}

void Replicator::applyStats(GameWorld* gw, Inbound& in) {
    (void)gw;
    std::deque<InboundStats> got;
    in.drainStats(got);
    for (std::deque<InboundStats>::iterator it = got.begin(); it != got.end(); ++it) {
        const StatsPacket& p = it->pkt;
        Key k; k.t = p.sType; k.c = p.sContainer; k.cs = p.sContainerSerial;
        k.i = p.sIndex; k.s = p.sSerial;
        if (ownHands_.find(k) != ownHands_.end()) continue; // never write our own truth
        Character* c = engine::resolveCharByHand(k.i, k.s, k.t, k.c, k.cs);
        if (!c) continue;
        engine::StatsRead w;
        memset(&w, 0, sizeof(w));
        w.valid = true;
        unsigned int n = p.nStats; if (n > 40) n = 40;
        w.nStats = n;
        for (unsigned int i = 0; i < 40; ++i)
            w.stats[i] = (i < n) ? p.stats[i] : -1.0f;
        w.xp            = p.xp;
        w.freeAttribPts = p.freeAttributePoints;
        bool ok = engine::writeStats(c, w);
        char b[200]; _snprintf(b, sizeof(b) - 1,
            "[stats] RECV hand=%u,%u ok=%d str=%.1f dex=%.1f tough=%.1f stealth=%.1f athl=%.1f xp=%.0f",
            k.i, k.s, ok ? 1 : 0, p.stats[1], p.stats[18], p.stats[21],
            p.stats[16], p.stats[17], p.xp);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

// ---- Protocol 22: per-tab wallet sync ---------------------------------------

// Squad-tab census for the money channel: fill ranks[] with ONE representative
// hand per distinct squad-tab container, using the same latch-aware ranking
// publishOwned partitions ownership by (protocol 35: an appended mid-session
// tab can therefore never shift which rank a pre-existing tab's wallet is
// keyed under). Returns the rank-space size. rankHand[r] = a member hand of
// the rank-r tab (readObjectHand layout); 0xFFFFFFFF type = no live member
// (a latched rank whose tab emptied, or beyond maxRanks).
unsigned int Replicator::tabRepresentatives(GameWorld* gw, unsigned int rankHand[][5],
                                            unsigned int maxRanks) {
    const unsigned int MAX_SQ = 96;
    static EntityState raw[MAX_SQ];
    unsigned int nSquad = engine::captureSquad(gw, /*leaderOnly*/ false, raw, MAX_SQ);
    std::vector<std::pair<u32, u32> > ctnrs;
    ctnrs.reserve(nSquad);
    for (unsigned int i = 0; i < nSquad; ++i)
        ctnrs.push_back(std::make_pair(raw[i].hContainer, raw[i].hContainerSerial));
    std::sort(ctnrs.begin(), ctnrs.end());
    ctnrs.erase(std::unique(ctnrs.begin(), ctnrs.end()), ctnrs.end());
    latchTabs(ctnrs);
    unsigned int nRanks = squadSync_ ? (unsigned int)tabRank_.size()
                                     : (unsigned int)ctnrs.size();
    if (nRanks > maxRanks) nRanks = maxRanks;
    for (unsigned int r = 0; r < nRanks; ++r) rankHand[r][0] = 0xFFFFFFFFu; // unfilled
    for (unsigned int i = 0; i < nSquad; ++i) {
        std::pair<u32, u32> key(raw[i].hContainer, raw[i].hContainerSerial);
        unsigned int rank = tabRankFor(key, ctnrs);
        if (rank >= nRanks || rankHand[rank][0] != 0xFFFFFFFFu) continue;
        rankHand[rank][0] = raw[i].hType;
        rankHand[rank][1] = raw[i].hContainer;
        rankHand[rank][2] = raw[i].hContainerSerial;
        rankHand[rank][3] = raw[i].hIndex;
        rankHand[rank][4] = raw[i].hSerial;
    }
    return nRanks;
}

void Replicator::publishMoney(GameWorld* gw, NetLink& net, u32 ownerId) {
    if (!moneySync_) return;
    const unsigned long RESEND_MS   = 5000; // safety resend (a lost write self-heals)
    const unsigned long MIN_SEND_MS = 1000; // wallets move in bursts; ~1 Hz is plenty
    const unsigned int  MAX_RANKS   = 8;
    unsigned long now = nowMs();
    unsigned int rankHand[MAX_RANKS][5];
    unsigned int nRanks = tabRepresentatives(gw, rankHand, MAX_RANKS);
    for (unsigned int r = 0; r < nRanks; ++r) {
        // Own-tabs only (the same partition rule as publishOwned's entity filter).
        bool owned = ownRanks_.empty() ? (r == 0u) : (ownRanks_.count(r) != 0);
        if (!owned || rankHand[r][0] == 0xFFFFFFFFu) continue;
        int money = -1;
        if (!engine::readWalletByHand(rankHand[r], &money) || money < 0) continue;
        MoneyPub& mp = moneyPub_[r];
        if (mp.lastSendMs != 0 && (now - mp.lastSendMs) < MIN_SEND_MS) continue;
        if (money == mp.lastSent && (now - mp.lastSendMs) < RESEND_MS) continue;
        bool changed = (money != mp.lastSent);
        mp.lastSent = money; mp.lastSendMs = now;
        MoneyPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type    = (u8)PKT_MONEY;
        pkt.ownerId = ownerId;
        pkt.tabRank = r;
        pkt.money   = money;
        net.queueMoney(pkt);
        if (changed) { // resends stay silent; the change is the signal
            char b[96];
            _snprintf(b, sizeof(b) - 1, "[money] SEND rank=%u cats=%d", r, money);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
}

void Replicator::applyMoney(GameWorld* gw, Inbound& in) {
    std::deque<InboundMoney> got;
    in.drainMoney(got);
    if (got.empty()) return;
    if (!moneySync_) return;
    const unsigned int MAX_RANKS = 8;
    unsigned int rankHand[MAX_RANKS][5];
    unsigned int nRanks = 0;
    bool haveRanks = false;
    for (std::deque<InboundMoney>::iterator it = got.begin(); it != got.end(); ++it) {
        const MoneyPacket& p = it->pkt;
        unsigned int r = p.tabRank;
        // Never write a tab we own - our engine is that wallet's authority.
        bool owned = ownRanks_.empty() ? (r == 0u) : (ownRanks_.count(r) != 0);
        if (owned || p.money < 0) continue;
        if (!haveRanks) { // one census per drain (cheap; usually 1 packet anyway)
            nRanks = tabRepresentatives(gw, rankHand, MAX_RANKS);
            haveRanks = true;
        }
        if (r >= nRanks || rankHand[r][0] == 0xFFFFFFFFu) continue;
        int cur = -1;
        engine::readWalletByHand(rankHand[r], &cur);
        if (cur == p.money) continue; // already converged (resend or echo)
        bool ok = engine::writeWalletByHand(rankHand[r], p.money);
        char b[112];
        _snprintf(b, sizeof(b) - 1, "[money] RECV rank=%u cats=%d was=%d ok=%d",
                  r, p.money, cur, ok ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::publishFactions(GameWorld* gw, NetLink& net, u32 ownerId) {
    if (!factionSync_) return;
    const unsigned long SAMPLE_MS = 1000;  // relations move in bursts; 1 Hz is plenty
    const unsigned long RESEND_MS = 10000; // safety resend for rows we ever sent
    const float         EPS       = 0.5f;  // engine values are whole-ish numbers
    unsigned long now = nowMs();

    // The affectRelations detour saw a REAL mutation this tick: sample NOW so
    // the row crosses within a tick instead of up to a full sample period
    // later. The deltas themselves are evidence (already logged); the value
    // diff below is what actually replicates.
    engine::FactionDelta deltas[16];
    unsigned int nDeltas = engine::drainFactionDeltas(deltas, 16);
    if (nDeltas == 0 && facSampleMs_ != 0 && (now - facSampleMs_) < SAMPLE_MS) return;
    facSampleMs_ = now;

    const unsigned int MAX_FACTIONS = 96;
    static engine::FactionRead rows[MAX_FACTIONS]; // main-thread only
    unsigned int n = engine::listPlayerRelations(gw, rows, MAX_FACTIONS);
    for (unsigned int i = 0; i < n; ++i) {
        const engine::FactionRead& r = rows[i];
        float cur = r.usToThem; // probe: the two table directions stay mirrored
        FacRow& fr = facRows_[std::string(r.sid)];
        if (!fr.seeded) {
            // Both clients load the same save, so the baseline is shared: seed
            // silently and stream only genuine mid-session movement.
            fr.seeded = true; fr.known = cur;
            continue;
        }
        bool changed = (cur - fr.known >= EPS) || (fr.known - cur >= EPS);
        bool resend  = fr.lastSendMs != 0 && (now - fr.lastSendMs) >= RESEND_MS;
        if (!changed && !resend) continue;
        fr.known = cur; fr.lastSendVal = cur; fr.lastSendMs = now;
        FactionPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type    = (u8)PKT_FACTION;
        pkt.ownerId = ownerId;
        pkt.seq     = facSeqOut_++;
        strncpy(pkt.sid, r.sid, sizeof(pkt.sid) - 1);
        pkt.sid[sizeof(pkt.sid) - 1] = '\0';
        pkt.relation = cur;
        net.queueFaction(pkt);
        if (changed) { // resends stay silent; the change is the signal
            char b[144];
            _snprintf(b, sizeof(b) - 1, "[fac] SEND sid='%s' rel=%.1f seq=%u",
                      pkt.sid, cur, pkt.seq);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
}

void Replicator::applyFactions(GameWorld* gw, Inbound& in) {
    std::deque<InboundFaction> got;
    in.drainFaction(got);
    if (got.empty()) return;
    if (!factionSync_) return;
    const float EPS = 0.5f;
    for (std::deque<InboundFaction>::iterator it = got.begin(); it != got.end(); ++it) {
        const FactionPacket& p = it->pkt;
        if (p.sid[0] == '\0') continue;
        FacRow& fr = facRows_[std::string(p.sid)];
        if (fr.seqSeen != 0 && p.seq <= fr.seqSeen) continue; // stale row
        fr.seqSeen = p.seq;
        float us = -999.0f, them = -999.0f;
        engine::readRelationBySid(gw, p.sid, &us, &them);
        // Updating the baseline FIRST is the echo guard: the local change this
        // write causes must not be re-detected as ours next sample.
        fr.known = p.relation;
        fr.seeded = true;
        if (us > -900.0f && (us - p.relation < EPS) && (p.relation - us < EPS))
            continue; // already converged (resend or echo)
        bool ok = engine::writeRelationBySid(gw, p.sid, p.relation,
                                             /*reciprocal*/ true, 0, 0);
        char b[160];
        _snprintf(b, sizeof(b) - 1, "[fac] RECV sid='%s' rel=%.1f was=%.1f ok=%d seq=%u",
                  p.sid, p.relation, us, ok ? 1 : 0, p.seq);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::publishDoors(GameWorld* gw, NetLink& net, u32 ownerId) {
    if (!doorSync_) return;
    const unsigned long SAMPLE_MS = 1000;  // doors move in clicks; 1 Hz is plenty
    const unsigned long RESEND_MS = 10000; // safety resend for rows we ever sent
    unsigned long now = nowMs();
    if (doorSampleMs_ != 0 && (now - doorSampleMs_) < SAMPLE_MS) return;
    doorSampleMs_ = now;

    const unsigned int MAX_DOORS = 64;
    static engine::DoorRead rows[MAX_DOORS]; // main-thread only
    unsigned int n = engine::enumDoorsNear(gw, 100.0f, rows, MAX_DOORS);
    for (unsigned int i = 0; i < n; ++i) {
        const engine::DoorRead& r = rows[i];
        // Protocol 28 partition: doors on SESSION-PLACED buildings (ours or
        // minted proxies) ride PKT_BUILD_DOOR on the translated identity -
        // their runtime hands would never resolve on the peer anyway.
        if (r.doorIndex >= 0) {
            Key pk; pk.t = r.parentHand[0]; pk.c = r.parentHand[1];
            pk.cs = r.parentHand[2]; pk.i = r.parentHand[3]; pk.s = r.parentHand[4];
            if (ownBuilds_.find(pk) != ownBuilds_.end() ||
                mintByLocal_.find(pk) != mintByLocal_.end())
                continue;
        }
        Key k; k.t = r.hand[0]; k.c = r.hand[1]; k.cs = r.hand[2];
        k.i = r.hand[3]; k.s = r.hand[4];
        DoorRow& dr = doorRows_[k];
        if (!dr.seeded) {
            // Both clients load the same save, so the baseline is shared: seed
            // silently and stream only genuine mid-session movement.
            dr.seeded = true; dr.knownOpen = r.open; dr.knownLocked = r.locked;
            continue;
        }
        bool changed = (r.open != dr.knownOpen) || (r.locked != dr.knownLocked);
        bool resend  = dr.lastSendMs != 0 && (now - dr.lastSendMs) >= RESEND_MS;
        if (!changed && !resend) continue;
        dr.knownOpen = r.open; dr.knownLocked = r.locked; dr.lastSendMs = now;
        DoorPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type    = (u8)PKT_DOOR;
        pkt.ownerId = ownerId;
        pkt.seq     = doorSeqOut_++;
        for (unsigned int h = 0; h < 5; ++h) pkt.hand[h] = r.hand[h];
        pkt.open    = (u8)(r.open ? 1 : 0);
        pkt.locked  = (u8)(r.locked ? 1 : 0);
        net.queueDoor(pkt);
        if (changed) { // resends stay silent; the change is the signal
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                      "[door] SEND hand=%u.%u.%u.%u.%u open=%u locked=%u seq=%u",
                      pkt.hand[0], pkt.hand[1], pkt.hand[2], pkt.hand[3],
                      pkt.hand[4], pkt.open, pkt.locked, pkt.seq);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
}

void Replicator::applyDoors(GameWorld* gw, Inbound& in) {
    (void)gw;
    std::deque<InboundDoor> got;
    in.drainDoor(got);
    if (got.empty()) return;
    if (!doorSync_) return;
    for (std::deque<InboundDoor>::iterator it = got.begin(); it != got.end(); ++it) {
        const DoorPacket& p = it->pkt;
        Key k; k.t = p.hand[0]; k.c = p.hand[1]; k.cs = p.hand[2];
        k.i = p.hand[3]; k.s = p.hand[4];
        DoorRow& dr = doorRows_[k];
        if (dr.seqSeen != 0 && p.seq <= dr.seqSeen) continue; // stale row
        dr.seqSeen = p.seq;
        // Updating the baseline FIRST is the echo guard: the local change this
        // write causes must not be re-detected as ours next sample.
        dr.knownOpen = (int)p.open; dr.knownLocked = (int)p.locked;
        dr.seeded = true;
        engine::DoorRead cur;
        if (!engine::readDoorByHand(p.hand, &cur))
            continue; // out-of-interest or runtime door - accepted edge
        bool lockMoves = cur.hasLock && (cur.locked != (int)p.locked);
        if (cur.open == (int)p.open && !lockMoves)
            continue; // already converged (resend or echo)
        bool ok = engine::writeDoorByHand(p.hand, (int)p.open,
                                          cur.hasLock ? (int)p.locked : -1, 0);
        char b[176];
        _snprintf(b, sizeof(b) - 1,
                  "[door] RECV hand=%u.%u.%u.%u.%u open=%u locked=%u was=%d/%d ok=%d seq=%u",
                  p.hand[0], p.hand[1], p.hand[2], p.hand[3], p.hand[4],
                  p.open, p.locked, cur.open, cur.locked, ok ? 1 : 0, p.seq);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

namespace {
// Protocol 33 amount quantization: the change gate compares hundredths, so a
// machine mid-production (amount creeping every engine tick) sends at most
// one row per sample, and true idleness sends nothing. -1 sentinels ("field
// not carried") quantize to -100, distinct from any real amount.
inline int qProd(float v) {
    return (int)(v * 100.0f + (v >= 0.0f ? 0.5f : -0.5f));
}
} // namespace

void Replicator::publishProd(GameWorld* gw, NetLink& net, u32 ownerId) {
    if (!prodSync_) return;
    const unsigned long SAMPLE_MS = 1000;  // machines tick slowly; 1 Hz is plenty
    const unsigned long RESEND_MS = 10000; // safety resend = the join drift corrector
    unsigned long now = nowMs();
    if (prodSampleMs_ != 0 && (now - prodSampleMs_) < SAMPLE_MS) return;
    prodSampleMs_ = now;

    const unsigned int MAX_MACH = 48;
    static engine::ProdRead rows[MAX_MACH]; // main-thread only
    unsigned int n = engine::enumMachinesNear(gw, 100.0f, rows, MAX_MACH);
    for (unsigned int i = 0; i < n; ++i) {
        const engine::ProdRead& r = rows[i];
        Key lk; lk.t = r.hand[0]; lk.c = r.hand[1]; lk.cs = r.hand[2];
        lk.i = r.hand[3]; lk.s = r.hand[4];
        // Wire identity: session-placed machines ride the protocol-27 placer
        // key (our own placement keys by OUR hand; a minted proxy of the
        // join's placement translates through the reverse map). Everything
        // else is a BAKED machine with a save-stable hand.
        int keyKind = 0; Key wk = lk;
        if (ownBuilds_.find(lk) != ownBuilds_.end()) {
            keyKind = 1;
        } else {
            std::map<Key, Key>::iterator mit = mintByLocal_.find(lk);
            if (mit != mintByLocal_.end()) { keyKind = 1; wk = mit->second; }
        }
        ProdRow& pr = prodRows_[std::make_pair(keyKind, wk)];
        int qOut = qProd(r.outAmount);
        int qIn0 = qProd(r.nInputs > 0 ? r.inAmount[0] : -1.0f);
        int qIn1 = qProd(r.nInputs > 1 ? r.inAmount[1] : -1.0f);
        int qGr  = qProd(r.grown), qDi = qProd(r.died);
        int qGs  = qProd(r.growStart), qHv = qProd((float)r.harvested);
        bool changed = !pr.sent ||
                       r.powerOn != pr.knownPower ||
                       r.productionState != pr.knownState ||
                       qOut != pr.qOut || qIn0 != pr.qIn0 || qIn1 != pr.qIn1 ||
                       qGr != pr.qGrown || qDi != pr.qDied ||
                       qGs != pr.qGrowStart || qHv != pr.qHarv;
        bool resend = pr.sent && (now - pr.lastSendMs) >= RESEND_MS;
        if (!changed && !resend) continue;
        bool first = !pr.sent;
        pr.sent = true; pr.lastSendMs = now;
        pr.knownPower = r.powerOn; pr.knownState = r.productionState;
        pr.qOut = qOut; pr.qIn0 = qIn0; pr.qIn1 = qIn1;
        pr.qGrown = qGr; pr.qDied = qDi; pr.qGrowStart = qGs; pr.qHarv = qHv;
        ProdPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type      = (u8)PKT_PROD;
        pkt.ownerId   = ownerId;
        pkt.seq       = prodSeqOut_++;
        pkt.keyKind   = (u8)keyKind;
        pkt.key[0] = wk.t; pkt.key[1] = wk.c; pkt.key[2] = wk.cs;
        pkt.key[3] = wk.i; pkt.key[4] = wk.s;
        pkt.classType = (u8)r.classType;
        pkt.powerOn   = (i8)r.powerOn;
        pkt.prodState = (i8)r.productionState;
        pkt.outAmount = r.outAmount;
        strncpy(pkt.outSid, r.outSid, sizeof(pkt.outSid) - 1);
        pkt.outSid[sizeof(pkt.outSid) - 1] = '\0';
        pkt.inAmount[0] = (r.nInputs > 0) ? r.inAmount[0] : -1.0f;
        pkt.inAmount[1] = (r.nInputs > 1) ? r.inAmount[1] : -1.0f;
        pkt.grown = r.grown; pkt.died = r.died; pkt.growStart = r.growStart;
        pkt.harvested = (float)r.harvested;
        net.queueProd(pkt);
        if (changed) { // resends stay silent; the change is the signal
            char b[256];
            _snprintf(b, sizeof(b) - 1,
                      "[prod] SEND key=%u.%u.%u.%u.%u kind=%d class=%d pwr=%d "
                      "state=%d out='%s' outAmt=%.3f in0=%.3f in1=%.3f "
                      "grown=%.3f first=%d seq=%u",
                      pkt.key[0], pkt.key[1], pkt.key[2], pkt.key[3], pkt.key[4],
                      keyKind, r.classType, r.powerOn, r.productionState,
                      pkt.outSid, r.outAmount, pkt.inAmount[0], pkt.inAmount[1],
                      r.grown, first ? 1 : 0, pkt.seq);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
}

void Replicator::applyProd(GameWorld* gw, Inbound& in) {
    (void)gw;
    std::deque<InboundProd> got;
    in.drainProd(got);
    if (got.empty()) return;
    if (!prodSync_) return;
    for (std::deque<InboundProd>::iterator it = got.begin(); it != got.end(); ++it) {
        const ProdPacket& p = it->pkt;
        Key wk; wk.t = p.key[0]; wk.c = p.key[1]; wk.cs = p.key[2];
        wk.i = p.key[3]; wk.s = p.key[4];
        ProdRow& pr = prodRows_[std::make_pair((int)p.keyKind, wk)];
        if (pr.seqSeen != 0 && p.seq <= pr.seqSeen) continue; // stale row
        pr.seqSeen = p.seq;
        // Resolve the wire key to OUR machine's hand: baked hands resolve
        // directly; a placer key is either a building WE placed (our own
        // hand) or one we MINTED for the host's placement (translation map).
        unsigned int hand[5];
        if (p.keyKind == 0) {
            for (unsigned int h = 0; h < 5; ++h) hand[h] = p.key[h];
        } else {
            std::map<Key, OwnBuild>::iterator ob = ownBuilds_.find(wk);
            if (ob != ownBuilds_.end()) {
                if (ob->second.removed) continue;
                memcpy(hand, ob->second.hand, sizeof(hand));
            } else {
                std::map<Key, PeerBuild>::iterator pb = peerBuilds_.find(wk);
                if (pb == peerBuilds_.end() || pb->second.minted != 1 ||
                    pb->second.removed)
                    continue; // unknown / refused / tombstoned key
                memcpy(hand, pb->second.localHand, sizeof(hand));
            }
        }
        engine::ProdRead cur;
        if (!engine::readMachineByHand(hand, &cur))
            continue; // out-of-interest / not resolvable here - accepted edge
        if (!cur.complete)
            continue; // still a construction site here; protocol 27 will finish it
        // Apply only what actually diverged, through the engine's own levers.
        int wantPower = -1;
        if (p.powerOn >= 0 && cur.powerOn >= 0 && (int)p.powerOn != cur.powerOn)
            wantPower = (int)p.powerOn;
        float outWant = -1.0f;
        if (p.outAmount >= 0.0f &&
            qProd(p.outAmount) != qProd(cur.outAmount >= 0.0f ? cur.outAmount : 0.0f))
            outWant = p.outAmount;
        float inWant[2] = { -1.0f, -1.0f };
        for (unsigned int k = 0; k < 2; ++k)
            if (p.inAmount[k] >= 0.0f && (int)k < cur.nInputs &&
                qProd(p.inAmount[k]) != qProd(cur.inAmount[k]))
                inWant[k] = p.inAmount[k];
        float farmWant[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
        if (p.grown >= 0.0f && qProd(p.grown) != qProd(cur.grown))
            farmWant[0] = p.grown;
        if (p.died >= 0.0f && qProd(p.died) != qProd(cur.died))
            farmWant[1] = p.died;
        if (p.growStart >= 0.0f && qProd(p.growStart) != qProd(cur.growStart))
            farmWant[2] = p.growStart;
        if (p.harvested >= 0.0f && qProd(p.harvested) != qProd((float)cur.harvested))
            farmWant[3] = p.harvested;
        bool needIn   = (inWant[0] >= 0.0f) || (inWant[1] >= 0.0f);
        bool needFarm = farmWant[0] >= 0.0f || farmWant[1] >= 0.0f ||
                        farmWant[2] >= 0.0f || farmWant[3] >= 0.0f;
        if (wantPower < 0 && outWant < 0.0f && !needIn && !needFarm)
            continue; // already converged (resend or settled row)
        // A still-null output buffer can't take the direct amount write -
        // materialize it FIRST via the native setProductionItem (the probe-
        // proven materializing lever), then land the exact amount directly
        // (setProductionItem splits stack into inventory; the direct write
        // is what makes the buffer byte-match the host's).
        if (outWant >= 0.0f && cur.outAmount < 0.0f)
            engine::writeMachineByHand(hand, -1, outWant, /*useSetItem*/true,
                                       0, 0, 0);
        engine::ProdRead after;
        bool ok = engine::writeMachineByHand(hand, wantPower, outWant,
                                             /*useSetItem*/false,
                                             needIn ? inWant : 0,
                                             needFarm ? farmWant : 0, &after);
        char b[256];
        _snprintf(b, sizeof(b) - 1,
                  "[prod] RECV key=%u.%u.%u.%u.%u kind=%u pwr=%d->%d "
                  "out=%.3f->%.3f in0=%.3f->%.3f ok=%d seq=%u",
                  p.key[0], p.key[1], p.key[2], p.key[3], p.key[4],
                  (unsigned)p.keyKind, cur.powerOn, (int)p.powerOn,
                  cur.outAmount, p.outAmount, cur.inAmount[0], p.inAmount[0],
                  ok ? 1 : 0, p.seq);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::publishResearch(GameWorld* gw, NetLink& net, u32 ownerId) {
    if (!researchSync_) return;
    const unsigned long SAMPLE_MS = 1000;  // unlocks are rare; 1 Hz is plenty
    const unsigned long RESEND_MS = 15000; // lost-row / late-prereq corrector
    unsigned long now = nowMs();
    if (researchSampleMs_ != 0 && (now - researchSampleMs_) < SAMPLE_MS) return;
    researchSampleMs_ = now;

    // The known set is bounded by the RESEARCH record count (384 on the sync
    // save); 512 rows x 48 B of main-thread-only scratch covers modded trees.
    const unsigned int MAX_KNOWN = 512;
    const unsigned int SID_CAP   = 48;
    static char sids[MAX_KNOWN * SID_CAP]; // main-thread only
    unsigned int n = engine::researchEnumKnown(gw, sids, SID_CAP, MAX_KNOWN);
    for (unsigned int i = 0; i < n; ++i) {
        const char* sid = sids + (size_t)i * SID_CAP;
        ResearchRow& rr = researchRows_[std::string(sid)];
        bool first  = !rr.sent;
        bool resend = rr.sent && (now - rr.lastSendMs) >= RESEND_MS;
        if (!first && !resend) continue;
        rr.sent = true; rr.lastSendMs = now;
        ResearchPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type    = (u8)PKT_RESEARCH;
        pkt.ownerId = ownerId;
        pkt.seq     = researchSeqOut_++;
        strncpy(pkt.sid, sid, sizeof(pkt.sid) - 1);
        net.queueResearch(pkt);
        if (first) { // resends stay silent; the new unlock is the signal
            char b[128];
            _snprintf(b, sizeof(b) - 1, "[research] SEND sid='%s' seq=%u",
                      pkt.sid, pkt.seq);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
}

void Replicator::applyResearch(GameWorld* gw, Inbound& in) {
    std::deque<InboundResearch> got;
    in.drainResearch(got);
    if (got.empty()) return;
    if (!researchSync_) return;
    for (std::deque<InboundResearch>::iterator it = got.begin();
         it != got.end(); ++it) {
        const ResearchPacket& p = it->pkt;
        char sid[sizeof(p.sid)];
        strncpy(sid, p.sid, sizeof(sid) - 1);
        sid[sizeof(sid) - 1] = '\0';
        if (!sid[0]) continue;
        ResearchRow& rr = researchRows_[std::string(sid)];
        if (rr.seqSeen != 0 && p.seq <= rr.seqSeen) continue; // stale row
        rr.seqSeen = p.seq;
        if (rr.applied) continue; // landed earlier; resends are no-ops
        int known = -1, can = -1;
        int rc = engine::researchQueryBySid(gw, sid, &known, &can);
        if (rc != 1) continue; // store/levers not up yet; the resend retries
        if (known == 1) { rr.applied = true; continue; } // already converged
        int started = engine::researchStartBySid(gw, sid);
        int knownAfter = -1;
        engine::researchQueryBySid(gw, sid, &knownAfter, &can);
        if (knownAfter == 1) rr.applied = true;
        char b[160];
        _snprintf(b, sizeof(b) - 1,
                  "[research] RECV sid='%s' known %d->%d start=%d seq=%u",
                  sid, known, knownAfter, started, p.seq);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::publishBuilds(GameWorld* gw, NetLink& net, u32 ownerId) {
    (void)gw;
    if (!buildSync_) return;

    // 1. Local placement edges -> PLACE announcements (drained every tick so
    // the edge queue never backs up; the detour caps it at 32 anyway).
    engine::BuildEdge edges[8];
    unsigned int n = engine::drainBuildEdges(edges, 8);
    for (unsigned int i = 0; i < n; ++i) {
        const engine::BuildEdge& e = edges[i];
        if (!e.sid[0]) continue; // no template sid = nothing the peer can mint
        Key k; k.t = e.hand[0]; k.c = e.hand[1]; k.cs = e.hand[2];
        k.i = e.hand[3]; k.s = e.hand[4];
        OwnBuild& ob = ownBuilds_[k];
        memcpy(ob.hand, e.hand, sizeof(ob.hand));
        BuildPlacePacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type    = (u8)PKT_BUILD_PLACE;
        pkt.ownerId = ownerId;
        pkt.seq     = buildSeqOut_++;
        for (unsigned int h = 0; h < 5; ++h) pkt.key[h] = e.hand[h];
        strncpy(pkt.sid, e.sid, sizeof(pkt.sid) - 1);
        pkt.sid[sizeof(pkt.sid) - 1] = '\0';
        pkt.x = e.x; pkt.y = e.y; pkt.z = e.z; pkt.yaw = e.yaw;
        pkt.fromUi = (u8)(e.fromUi ? 1 : 0);
        // Protocol 30: retain the announcement so a connect-edge resync can
        // re-send it to a late joiner (the one-shot edge, made repeatable).
        memcpy(&ob.ann, &pkt, sizeof(pkt));
        ob.haveAnn = true;
        net.queueBuildPlace(pkt);
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "[build] PLACE-SEND key=%u.%u.%u.%u.%u sid='%s' ui=%u "
                  "pos=%.1f,%.1f,%.1f seq=%u",
                  pkt.key[0], pkt.key[1], pkt.key[2], pkt.key[3], pkt.key[4],
                  pkt.sid, pkt.fromUi, pkt.x, pkt.y, pkt.z, pkt.seq);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // 1b. Removal edges (protocol 28): the dismantle detour (UI path) and the
    // programmatic destroy both queue hands here. Only buildings WE placed
    // stream a REMOVE (placer-authoritative); a dismantle of a baked building
    // or of a peer's proxy logs at the detour but stays local.
    unsigned int rmEdges[8][5];
    unsigned int rn = engine::drainRemoveEdges(rmEdges, 8);
    for (unsigned int i = 0; i < rn; ++i) {
        Key k; k.t = rmEdges[i][0]; k.c = rmEdges[i][1]; k.cs = rmEdges[i][2];
        k.i = rmEdges[i][3]; k.s = rmEdges[i][4];
        std::map<Key, OwnBuild>::iterator f = ownBuilds_.find(k);
        if (f == ownBuilds_.end() || f->second.removed) continue;
        f->second.removed = true;
        if (!bdoorSync_) continue; // A/B hatch: edge observed, nothing streams
        BuildRemovePacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type    = (u8)PKT_BUILD_REMOVE;
        pkt.ownerId = ownerId;
        pkt.seq     = buildSeqOut_++;
        for (unsigned int h = 0; h < 5; ++h) pkt.key[h] = rmEdges[i][h];
        net.queueBuildRemove(pkt);
        char b[160];
        _snprintf(b, sizeof(b) - 1,
                  "[build] REMOVE-SEND key=%u.%u.%u.%u.%u seq=%u",
                  pkt.key[0], pkt.key[1], pkt.key[2], pkt.key[3], pkt.key[4],
                  pkt.seq);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // 2. Progress rows for buildings WE placed (~1 Hz, change-gated + 10 s
    // safety resend while incomplete; the final complete row latches).
    const unsigned long SAMPLE_MS = 1000;
    const unsigned long RESEND_MS = 10000;
    unsigned long now = nowMs();
    if (buildSampleMs_ != 0 && (now - buildSampleMs_) < SAMPLE_MS) return;
    buildSampleMs_ = now;
    const float EPS = 0.005f;
    for (std::map<Key, OwnBuild>::iterator it = ownBuilds_.begin();
         it != ownBuilds_.end(); ++it) {
        OwnBuild& ob = it->second;
        if (ob.removed)  continue; // dismantled/destroyed: nothing to sample
        if (ob.doneSent) continue; // finished + announced: silent forever
        engine::BuildRead cur;
        if (!engine::readBuildingByHand(ob.hand, &cur))
            continue; // destroyed/unloaded locally - stop streaming quietly
        float dp = cur.progress - ob.lastProg;
        bool changed = (dp > EPS || dp < -EPS) || (cur.complete != ob.lastComplete);
        bool resend  = ob.lastSendMs != 0 && (now - ob.lastSendMs) >= RESEND_MS;
        if (!changed && !resend) continue;
        ob.lastProg = cur.progress; ob.lastComplete = cur.complete;
        ob.lastSendMs = now;
        BuildStatePacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type     = (u8)PKT_BUILD_STATE;
        pkt.ownerId  = ownerId;
        pkt.seq      = buildSeqOut_++;
        for (unsigned int h = 0; h < 5; ++h) pkt.key[h] = ob.hand[h];
        pkt.progress = cur.progress;
        pkt.complete = (u8)(cur.complete ? 1 : 0);
        net.queueBuildState(pkt);
        if (cur.complete) ob.doneSent = true;
        if (changed) { // resends stay silent; the change is the signal
            char b[176];
            _snprintf(b, sizeof(b) - 1,
                      "[build] STATE-SEND key=%u.%u.%u.%u.%u prog=%.3f complete=%u seq=%u",
                      pkt.key[0], pkt.key[1], pkt.key[2], pkt.key[3], pkt.key[4],
                      cur.progress, pkt.complete, pkt.seq);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
}

void Replicator::applyBuilds(GameWorld* gw, Inbound& in) {
    // Announcements first (same-channel ordered-reliable means a STATE row
    // never precedes its PLACE on the wire; keep that property here too).
    std::deque<InboundBuildPlace> places;
    in.drainBuildPlace(places);
    std::deque<InboundBuildState> states;
    in.drainBuildState(states);
    if (!buildSync_) return;
    for (std::deque<InboundBuildPlace>::iterator it = places.begin();
         it != places.end(); ++it) {
        const BuildPlacePacket& p = it->pkt;
        if (p.sid[0] == '\0') continue;
        Key k; k.t = p.key[0]; k.c = p.key[1]; k.cs = p.key[2];
        k.i = p.key[3]; k.s = p.key[4];
        if (peerBuilds_.find(k) != peerBuilds_.end())
            continue; // already minted (or mint already refused) - dedupe
        PeerBuild& pb = peerBuilds_[k];
        // Mint INCOMPLETE always: the placer's STATE rows drive progress from
        // here (a real UI placement starts at 0 anyway).
        int rc = engine::placeBuildingAt(gw, p.sid, p.x, p.y, p.z, p.yaw,
                                         /*completed*/false, pb.localHand);
        pb.minted = (rc == 1) ? 1 : 0;
        if (pb.minted) {
            // Reverse translation (protocol 28): the door sampler and the
            // protocol-26 filter recognize this proxy by its LOCAL hand.
            Key lk; lk.t = pb.localHand[0]; lk.c = pb.localHand[1];
            lk.cs = pb.localHand[2]; lk.i = pb.localHand[3]; lk.s = pb.localHand[4];
            mintByLocal_[lk] = k;
        }
        char b[240];
        _snprintf(b, sizeof(b) - 1,
                  "[build] MINT key=%u.%u.%u.%u.%u sid='%s' ui=%u rc=%d "
                  "local=%u.%u.%u.%u.%u",
                  p.key[0], p.key[1], p.key[2], p.key[3], p.key[4],
                  p.sid, p.fromUi, rc,
                  pb.localHand[0], pb.localHand[1], pb.localHand[2],
                  pb.localHand[3], pb.localHand[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    for (std::deque<InboundBuildState>::iterator it = states.begin();
         it != states.end(); ++it) {
        const BuildStatePacket& p = it->pkt;
        Key k; k.t = p.key[0]; k.c = p.key[1]; k.cs = p.key[2];
        k.i = p.key[3]; k.s = p.key[4];
        std::map<Key, PeerBuild>::iterator f = peerBuilds_.find(k);
        if (f == peerBuilds_.end() || !f->second.minted)
            continue; // mint refused or key unknown - skip silently
        PeerBuild& pb = f->second;
        if (pb.removed) continue; // tombstoned (REMOVE already applied)
        if (pb.seqSeen != 0 && p.seq <= pb.seqSeen) continue; // stale row
        pb.seqSeen = p.seq;
        engine::BuildRead cur;
        if (engine::readBuildingByHand(pb.localHand, &cur)) {
            float d = cur.progress - p.progress;
            bool progClose = (d < 0.005f && d > -0.005f);
            if (progClose && cur.complete == (int)p.complete)
                continue; // already converged (resend)
            if (cur.complete) continue; // completion is latched locally
        }
        engine::BuildRead post;
        bool ok = engine::writeBuildProgressByHand(pb.localHand, p.progress, &post);
        char b[208];
        _snprintf(b, sizeof(b) - 1,
                  "[build] STATE-RECV key=%u.%u.%u.%u.%u prog=%.3f complete=%u "
                  "ok=%d localProg=%.3f localComplete=%d seq=%u",
                  p.key[0], p.key[1], p.key[2], p.key[3], p.key[4],
                  p.progress, p.complete, ok ? 1 : 0,
                  post.progress, post.complete, p.seq);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // Removals (protocol 28, placer-authoritative): destroy the mapped proxy
    // through the engine's own GameWorld::destroy and tombstone the entry so
    // any late STATE/DOOR rows for the key skip silently. Gated on bdoorSync
    // (the removal channel ships with the placed-door slice).
    std::deque<InboundBuildRemove> removes;
    in.drainBuildRemove(removes);
    if (!bdoorSync_) return;
    for (std::deque<InboundBuildRemove>::iterator it = removes.begin();
         it != removes.end(); ++it) {
        const BuildRemovePacket& p = it->pkt;
        Key k; k.t = p.key[0]; k.c = p.key[1]; k.cs = p.key[2];
        k.i = p.key[3]; k.s = p.key[4];
        std::map<Key, PeerBuild>::iterator f = peerBuilds_.find(k);
        if (f == peerBuilds_.end() || !f->second.minted || f->second.removed)
            continue; // never minted here or already gone - nothing to remove
        PeerBuild& pb = f->second;
        pb.removed = true;
        bool ok = engine::destroyBuildingByHand(gw, pb.localHand);
        char b[192];
        _snprintf(b, sizeof(b) - 1,
                  "[build] REMOVE-RECV key=%u.%u.%u.%u.%u ok=%d "
                  "local=%u.%u.%u.%u.%u seq=%u",
                  p.key[0], p.key[1], p.key[2], p.key[3], p.key[4], ok ? 1 : 0,
                  pb.localHand[0], pb.localHand[1], pb.localHand[2],
                  pb.localHand[3], pb.localHand[4], p.seq);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::onPeerConnected(NetLink& net, u32 ownerId) {
    // 1. One-shot edges, replayed: every live placed building's PLACE (and
    // the REMOVE for removed ones) goes out again. The receiver's session
    // maps dedupe - a known key skips the mint, a tombstoned key skips the
    // remove - so a quick reconnect is safe and a fresh joiner finally
    // learns what it missed.
    unsigned int nPlace = 0, nRemove = 0;
    if (buildSync_) {
        for (std::map<Key, OwnBuild>::iterator it = ownBuilds_.begin();
             it != ownBuilds_.end(); ++it) {
            OwnBuild& ob = it->second;
            if (!ob.haveAnn) continue;
            if (ob.removed) {
                if (!bdoorSync_) continue; // removal ships with the bdoor slice
                BuildRemovePacket pkt;
                memset(&pkt, 0, sizeof(pkt));
                pkt.type    = (u8)PKT_BUILD_REMOVE;
                pkt.ownerId = ownerId;
                pkt.seq     = buildSeqOut_++;
                for (unsigned int h = 0; h < 5; ++h) pkt.key[h] = ob.hand[h];
                net.queueBuildRemove(pkt);
                ++nRemove;
            } else {
                ob.ann.ownerId = ownerId;
                ob.ann.seq     = buildSeqOut_++;
                net.queueBuildPlace(ob.ann);
                // Un-latch the STATE row: the next publishBuilds sample sees
                // "changed" against the reset baseline and sends one fresh
                // progress row (complete=1 re-latches doneSent immediately).
                ob.doneSent   = false;
                ob.lastProg   = -1.0f;
                ob.lastComplete = -1;
                ob.lastSendMs = 0;
                ++nPlace;
            }
        }
    }

    // 2. Force-resend pass over the change-gated caches: age lastSendMs to 1
    // on every row EVER SENT, so each channel's own safety-resend condition
    // fires on its next sample (rows never sent - the seeded shared-save
    // baseline - correctly stay silent). Edge-only caches (weaponCensus_,
    // hostBody_, stealthPub_) are left alone: re-seeding them would author
    // phantom drop/KO edges rather than heal state.
    unsigned int nFac = 0, nDoor = 0, nBdoor = 0, nMed = 0, nStats = 0,
                 nMoney = 0, nInv = 0, nWorld = 0, nProd = 0;
    for (std::map<std::string, FacRow>::iterator it = facRows_.begin();
         it != facRows_.end(); ++it)
        if (it->second.lastSendMs != 0) { it->second.lastSendMs = 1; ++nFac; }
    for (std::map<Key, DoorRow>::iterator it = doorRows_.begin();
         it != doorRows_.end(); ++it)
        if (it->second.lastSendMs != 0) { it->second.lastSendMs = 1; ++nDoor; }
    for (std::map<std::pair<Key, int>, BdoorRow>::iterator it = bdoorRows_.begin();
         it != bdoorRows_.end(); ++it)
        if (it->second.lastSendMs != 0) { it->second.lastSendMs = 1; ++nBdoor; }
    for (std::map<Key, MedPub>::iterator it = medPub_.begin();
         it != medPub_.end(); ++it)
        if (it->second.lastSendMs != 0) { it->second.lastSendMs = 1; ++nMed; }
    for (std::map<Key, StatsPub>::iterator it = statsPub_.begin();
         it != statsPub_.end(); ++it)
        if (it->second.lastSendMs != 0) { it->second.lastSendMs = 1; ++nStats; }
    for (std::map<unsigned int, MoneyPub>::iterator it = moneyPub_.begin();
         it != moneyPub_.end(); ++it)
        if (it->second.lastSendMs != 0) { it->second.lastSendMs = 1; ++nMoney; }
    for (std::map<Key, InvPub>::iterator it = invPub_.begin();
         it != invPub_.end(); ++it)
        if (it->second.lastSendMs != 0) { it->second.lastSendMs = 1; ++nInv; }
    for (std::map<Key, WorldTrack>::iterator it = worldTrack_.begin();
         it != worldTrack_.end(); ++it)
        if (it->second.lastSendMs != 0) { it->second.lastSendMs = 1; ++nWorld; }
    for (std::map<std::pair<int, Key>, ProdRow>::iterator it = prodRows_.begin();
         it != prodRows_.end(); ++it)
        if (it->second.lastSendMs != 0) { it->second.lastSendMs = 1; ++nProd; }

    char b[224];
    _snprintf(b, sizeof(b) - 1,
              "[latejoin] RESYNC place=%u remove=%u fac=%u door=%u bdoor=%u "
              "med=%u stats=%u money=%u inv=%u world=%u prod=%u",
              nPlace, nRemove, nFac, nDoor, nBdoor, nMed, nStats, nMoney,
              nInv, nWorld, nProd);
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
}

void Replicator::publishBuildDoors(GameWorld* gw, NetLink& net, u32 ownerId) {
    (void)gw;
    if (!bdoorSync_) return;
    const unsigned long SAMPLE_MS = 1000;  // the protocol-26 door cadence
    const unsigned long RESEND_MS = 10000; // safety resend for rows ever sent
    unsigned long now = nowMs();
    if (bdoorSampleMs_ != 0 && (now - bdoorSampleMs_) < SAMPLE_MS) return;
    bdoorSampleMs_ = now;

    const unsigned int MAX_DOORS_PER_BUILDING = 4;
    // Two passes over the build maps: buildings WE placed (wire key = our
    // hand) and minted proxies (wire key = the placer's, via the map key).
    for (int pass = 0; pass < 2; ++pass) {
        std::map<Key, OwnBuild>::iterator oit = ownBuilds_.begin();
        std::map<Key, PeerBuild>::iterator pit = peerBuilds_.begin();
        for (;;) {
            const Key* wireKey = 0;
            const unsigned int* localHand = 0;
            if (pass == 0) {
                if (oit == ownBuilds_.end()) break;
                if (oit->second.removed) { ++oit; continue; }
                wireKey = &oit->first; localHand = oit->second.hand; ++oit;
            } else {
                if (pit == peerBuilds_.end()) break;
                if (!pit->second.minted || pit->second.removed) { ++pit; continue; }
                wireKey = &pit->first; localHand = pit->second.localHand; ++pit;
            }
            for (unsigned int di = 0; di < MAX_DOORS_PER_BUILDING; ++di) {
                engine::DoorRead dr;
                if (!engine::readDoorOfBuilding(localHand, di, &dr)) break;
                BdoorRow& row = bdoorRows_[std::make_pair(*wireKey, (int)di)];
                if (!row.seeded) {
                    // Both sides mint the door closed and the PLACE seeded the
                    // building, so the first sample is the shared baseline.
                    row.seeded = true;
                    row.knownOpen = dr.open; row.knownLocked = dr.locked;
                    continue;
                }
                bool changed = (dr.open != row.knownOpen) ||
                               (dr.locked != row.knownLocked);
                bool resend  = row.lastSendMs != 0 && (now - row.lastSendMs) >= RESEND_MS;
                if (!changed && !resend) continue;
                row.knownOpen = dr.open; row.knownLocked = dr.locked;
                row.lastSendMs = now;
                BuildDoorPacket pkt;
                memset(&pkt, 0, sizeof(pkt));
                pkt.type    = (u8)PKT_BUILD_DOOR;
                pkt.ownerId = ownerId;
                pkt.seq     = bdoorSeqOut_++;
                pkt.bkey[0] = wireKey->t; pkt.bkey[1] = wireKey->c;
                pkt.bkey[2] = wireKey->cs; pkt.bkey[3] = wireKey->i;
                pkt.bkey[4] = wireKey->s;
                pkt.doorIndex = (u8)di;
                pkt.open    = (u8)(dr.open ? 1 : 0);
                pkt.locked  = (u8)(dr.locked ? 1 : 0);
                net.queueBuildDoor(pkt);
                if (changed) { // resends stay silent; the change is the signal
                    char b[176];
                    _snprintf(b, sizeof(b) - 1,
                              "[bdoor] SEND key=%u.%u.%u.%u.%u idx=%u open=%u locked=%u seq=%u",
                              pkt.bkey[0], pkt.bkey[1], pkt.bkey[2], pkt.bkey[3],
                              pkt.bkey[4], pkt.doorIndex, pkt.open, pkt.locked, pkt.seq);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
        }
    }
}

void Replicator::applyBuildDoors(GameWorld* gw, Inbound& in) {
    (void)gw;
    std::deque<InboundBuildDoor> got;
    in.drainBuildDoor(got);
    if (got.empty()) return;
    if (!bdoorSync_) return;
    for (std::deque<InboundBuildDoor>::iterator it = got.begin(); it != got.end(); ++it) {
        const BuildDoorPacket& p = it->pkt;
        Key k; k.t = p.bkey[0]; k.c = p.bkey[1]; k.cs = p.bkey[2];
        k.i = p.bkey[3]; k.s = p.bkey[4];
        // Resolve the key to the LOCAL building: our own placement (the peer
        // moved a door on OUR building's proxy) or a minted proxy of theirs.
        const unsigned int* localHand = 0;
        std::map<Key, OwnBuild>::iterator oit = ownBuilds_.find(k);
        if (oit != ownBuilds_.end() && !oit->second.removed) {
            localHand = oit->second.hand;
        } else {
            std::map<Key, PeerBuild>::iterator pit = peerBuilds_.find(k);
            if (pit != peerBuilds_.end() && pit->second.minted && !pit->second.removed)
                localHand = pit->second.localHand;
        }
        BdoorRow& row = bdoorRows_[std::make_pair(k, (int)p.doorIndex)];
        if (row.seqSeen != 0 && p.seq <= row.seqSeen) continue; // stale row
        row.seqSeen = p.seq;
        // Updating the baseline FIRST is the echo guard: the local change this
        // write causes must not be re-detected as ours next sample.
        row.knownOpen = (int)p.open; row.knownLocked = (int)p.locked;
        row.seeded = true;
        if (!localHand) continue; // unknown/tombstoned key - skip silently
        engine::DoorRead cur;
        if (!engine::readDoorOfBuilding(localHand, p.doorIndex, &cur))
            continue; // no such door locally (mint refused earlier) - skip
        bool lockMoves = cur.hasLock && (cur.locked != (int)p.locked);
        if (cur.open == (int)p.open && !lockMoves)
            continue; // already converged (resend or echo)
        bool ok = engine::writeDoorByHand(cur.hand, (int)p.open,
                                          cur.hasLock ? (int)p.locked : -1, 0);
        char b[192];
        _snprintf(b, sizeof(b) - 1,
                  "[bdoor] RECV key=%u.%u.%u.%u.%u idx=%u open=%u locked=%u "
                  "was=%d/%d ok=%d seq=%u",
                  p.bkey[0], p.bkey[1], p.bkey[2], p.bkey[3], p.bkey[4],
                  p.doorIndex, p.open, p.locked, cur.open, cur.locked,
                  ok ? 1 : 0, p.seq);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::publishRecruits(GameWorld* gw, NetLink& net, u32 ownerId) {
    (void)gw;
    if (!recruitSync_) return; // hook is only installed when the sync is on
    engine::RecruitEdge edges[8];
    unsigned int n = engine::drainRecruitEdges(edges, 8);
    for (unsigned int i = 0; i < n; ++i) {
        // Pin ownership BEFORE the wire: from this tick publishOwned streams
        // the new hand no matter which tab rank its container maps to.
        Key nk; nk.t = edges[i].after[0]; nk.c = edges[i].after[1];
        nk.cs = edges[i].after[2]; nk.i = edges[i].after[3]; nk.s = edges[i].after[4];
        pinOwned_.insert(nk);
        EventPacket ev;
        memset(&ev, 0, sizeof(ev));
        ev.type    = (u8)PKT_EVENT;
        ev.event   = EVT_RECRUIT;
        ev.ownerId = ownerId;
        ev.eventId = nextEventId_++;
        ev.sType = edges[i].before[0]; ev.sContainer = edges[i].before[1];
        ev.sContainerSerial = edges[i].before[2];
        ev.sIndex = edges[i].before[3]; ev.sSerial = edges[i].before[4];
        ev.aType = edges[i].after[0]; ev.aContainer = edges[i].after[1];
        ev.aContainerSerial = edges[i].after[2];
        ev.aIndex = edges[i].after[3]; ev.aSerial = edges[i].after[4];
        net.queueEvent(ev);
        char b[176];
        _snprintf(b, sizeof(b) - 1,
                  "[recruit] EVT send old=%u,%u,%u,%u,%u new=%u,%u,%u,%u,%u",
                  ev.sType, ev.sContainer, ev.sContainerSerial, ev.sIndex, ev.sSerial,
                  ev.aType, ev.aContainer, ev.aContainerSerial, ev.aIndex, ev.aSerial);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::publishSquadMoves(GameWorld* gw, NetLink& net, u32 ownerId) {
    if (!squadSync_) return;
    // Per-TICK poll (run 192211: a 500 ms throttle lost the race - a member
    // moved back into a rank-owned tab streamed its new hand immediately,
    // and the peer's REQ/mint round-trip built a duplicate proxy before the
    // throttled EVT arrived). Polling every tick puts the reliable edge in
    // the SAME flush as the new hand's first entity batch, so the peer's
    // re-key always lands before its spawn REQ could even be authored.
    engine::pollSquadRoster(gw);
    engine::SquadMoveEdge edges[8];
    unsigned int n = engine::drainSquadMoveEdges(edges, 8);
    for (unsigned int i = 0; i < n; ++i) {
        Key ok; ok.t = edges[i].before[0]; ok.c = edges[i].before[1];
        ok.cs = edges[i].before[2]; ok.i = edges[i].before[3]; ok.s = edges[i].before[4];
        Key nk; nk.t = edges[i].after[0]; nk.c = edges[i].after[1];
        nk.cs = edges[i].after[2]; nk.i = edges[i].after[3]; nk.s = edges[i].after[4];
        bool exited = (nk.t | nk.c | nk.cs | nk.i | nk.s) == 0;
        // The old hand is dead either way - drop any pin it carried (a moved
        // recruit / a re-moved member must not leave a stale claim behind).
        pinOwned_.erase(ok);
        pinPeer_.erase(ok);
        // Pin ownership BEFORE the wire (the recruit pattern): every edge
        // polled from OUR roster is OUR user's action, so the new hand
        // publishes from this side no matter which rank its container latched
        // to (an appended tab inherits our ownership through this pin).
        if (!exited) pinOwned_.insert(nk);
        EventPacket ev;
        memset(&ev, 0, sizeof(ev));
        ev.type    = (u8)PKT_EVENT;
        ev.event   = EVT_SQUAD_MOVE;
        ev.ownerId = ownerId;
        ev.eventId = nextEventId_++;
        ev.sType = edges[i].before[0]; ev.sContainer = edges[i].before[1];
        ev.sContainerSerial = edges[i].before[2];
        ev.sIndex = edges[i].before[3]; ev.sSerial = edges[i].before[4];
        ev.aType = edges[i].after[0]; ev.aContainer = edges[i].after[1];
        ev.aContainerSerial = edges[i].after[2];
        ev.aIndex = edges[i].after[3]; ev.aSerial = edges[i].after[4];
        net.queueEvent(ev);
        char b[176];
        _snprintf(b, sizeof(b) - 1,
                  "[squad] EVT send old=%u,%u,%u,%u,%u new=%u,%u,%u,%u,%u exit=%d",
                  ev.sType, ev.sContainer, ev.sContainerSerial, ev.sIndex, ev.sSerial,
                  ev.aType, ev.aContainer, ev.aContainerSerial, ev.aIndex, ev.aSerial,
                  exited ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::publishStealth(GameWorld* gw, NetLink& net, u32 ownerId) {
    (void)gw;
    if (!stealthSync_) return;
    unsigned long now = nowMs();
    // Subjects: DRIVEN copies only (tracked hands we do NOT own). Our OWN
    // sneakers' indicators are already native on our screen; what the peer
    // cannot compute is what OUR world's detection says about ITS characters.
    for (std::map<Key, Driven>::iterator it = targets_.begin(); it != targets_.end(); ++it) {
        const Key& k = it->first;
        if (ownHands_.find(k) != ownHands_.end()) continue;
        Character* c = engine::resolveCharByHand(k.i, k.s, k.t, k.c, k.cs);
        if (!c) continue;
        engine::StealthRead sr;
        if (!engine::readStealth(c, &sr) || !sr.valid) continue;
        bool active = sr.sneaking && sr.nSeers > 0;
        std::map<Key, StealthPub>::iterator pit = stealthPub_.find(k);
        if (!active && (pit == stealthPub_.end() || !pit->second.active)) {
            // Quiet body and the last snapshot already said so (or we never
            // sent one) - nothing to author.
            continue;
        }
        StealthPub& sp = stealthPub_[k];
        // Fingerprint the visible map (entries + coarse progress) so an
        // unchanged stare-down doesn't re-send every 250 ms.
        u32 h = 2166136261u;
        for (unsigned int i = 0; i < sr.nSeers; ++i) {
            h = (h ^ sr.seers[i].npc[3]) * 16777619u;
            h = (h ^ sr.seers[i].npc[4]) * 16777619u;
            h = (h ^ (u32)sr.seers[i].see) * 16777619u;
            h = (h ^ (u32)(sr.seers[i].prog * 4.0f)) * 16777619u;
        }
        h = (h ^ (u32)sr.unseen) * 16777619u;
        if (active) {
            if (sp.lastSendMs != 0 && (now - sp.lastSendMs) < STEALTH_SEND_MS) continue;
            if (h == sp.hash && (now - sp.lastSendMs) < STEALTH_RESEND_MS) continue;
        }
        // else: falling edge - send ONE empty snapshot immediately (clears the
        // owner's stale arrows), then go quiet.
        sp.hash = h; sp.lastSendMs = now; sp.active = active;
        StealthPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type    = (u8)PKT_STEALTH;
        pkt.ownerId = ownerId;
        pkt.sType = k.t; pkt.sContainer = k.c; pkt.sContainerSerial = k.cs;
        pkt.sIndex = k.i; pkt.sSerial = k.s;
        pkt.unseen = sr.unseen;
        unsigned int n = active ? sr.nSeers : 0;
        if (n > STEALTH_SEER_MAX) n = STEALTH_SEER_MAX;
        pkt.nSeers = (u8)n;
        for (unsigned int i = 0; i < n; ++i) {
            pkt.seers[i].nType            = sr.seers[i].npc[0];
            pkt.seers[i].nContainer       = sr.seers[i].npc[1];
            pkt.seers[i].nContainerSerial = sr.seers[i].npc[2];
            pkt.seers[i].nIndex           = sr.seers[i].npc[3];
            pkt.seers[i].nSerial          = sr.seers[i].npc[4];
            pkt.seers[i].see              = sr.seers[i].see;
            pkt.seers[i].prog             = sr.seers[i].prog;
        }
        net.queueStealth(pkt);
        char b[160]; _snprintf(b, sizeof(b) - 1,
            "[sneak] DETECT SEND hand=%u,%u seers=%u unseen=%u map=%u",
            k.i, k.s, n, (unsigned)sr.unseen, sr.mapSize);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    // Age out publish state for hands no longer tracked (peer left, save cycle).
    for (std::map<Key, StealthPub>::iterator sit = stealthPub_.begin(); sit != stealthPub_.end(); ) {
        if (targets_.find(sit->first) == targets_.end()) stealthPub_.erase(sit++);
        else ++sit;
    }
}

void Replicator::applyStealthFeedback(GameWorld* gw, Inbound& in) {
    std::deque<InboundStealth> got;
    in.drainStealth(got);
    if (!stealthSync_ || got.empty()) return;
    for (std::deque<InboundStealth>::iterator it = got.begin(); it != got.end(); ++it) {
        const StealthPacket& p = it->pkt;
        Key k; k.t = p.sType; k.c = p.sContainer; k.cs = p.sContainerSerial;
        k.i = p.sIndex; k.s = p.sSerial;
        // Only replay onto a body WE own: the feedback stream exists to light
        // up the OWNER's indicators. A driven copy already has its own local
        // map (it IS the detection authority's copy elsewhere) - writing to it
        // would double-count.
        if (ownHands_.find(k) == ownHands_.end()) continue;
        Character* c = engine::resolveCharByHand(k.i, k.s, k.t, k.c, k.cs);
        if (!c) continue;
        unsigned int applied = 0;
        unsigned int n = p.nSeers; if (n > STEALTH_SEER_MAX) n = STEALTH_SEER_MAX;
        for (unsigned int i = 0; i < n; ++i) {
            unsigned int npcHand[5] = {
                p.seers[i].nType, p.seers[i].nContainer, p.seers[i].nContainerSerial,
                p.seers[i].nIndex, p.seers[i].nSerial
            };
            // Seers that don't resolve locally (outside interest / suppressed-
            // hidden) are skipped - they wouldn't be detecting here anyway.
            if (engine::applyStealthSeer(gw, c, npcHand, p.seers[i].see,
                                         p.seers[i].prog))
                ++applied;
        }
        // An EMPTY snapshot is the authority saying "no one sees you anymore";
        // nothing to replay - the local map's entries age out on their own once
        // the notifies stop (spike-verified decay).
        char b[160]; _snprintf(b, sizeof(b) - 1,
            "[sneak] DETECT RECV hand=%u,%u seers=%u applied=%u unseen=%u",
            k.i, k.s, n, applied, (unsigned)p.unseen);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::syncSpeed(GameWorld* gw, Inbound& in, NetLink& net, u32 ownerId,
                           bool isHost) {
    const unsigned long RESEND_MS        = 3000; // safety resend (late join / lost state)
    const unsigned long COMBAT_SAMPLE_MS = 1000; // own-squad combat poll cadence
    const float         EPS              = 0.01f;
    unsigned long now = nowMs();

    // Own-squad combat flag, ~1 Hz (readCombat per owned member; the cap only
    // needs second-level reactivity). An edge forces a REQ send below so the
    // host's cap reacts faster than the safety resend.
    bool combatEdge = false;
    if (speedCombatSampleMs_ == 0 || (now - speedCombatSampleMs_) >= COMBAT_SAMPLE_MS) {
        speedCombatSampleMs_ = now;
        bool fighting = false;
        for (std::set<Key>::const_iterator it = ownHands_.begin();
             it != ownHands_.end(); ++it) {
            Character* c = engine::resolveCharByHand(it->i, it->s, it->t, it->c, it->cs);
            engine::CombatRead cr;
            if (c && engine::readCombat(c, &cr) && cr.inCombat) { fighting = true; break; }
        }
        combatEdge     = (fighting != speedMyCombat_);
        speedMyCombat_ = fighting;
    }

    // Local vote capture: the engine-setter hooks (setGameSpeed / userPause /
    // togglePause) record every REAL user action - UI clicks, keyboard pause,
    // RE_Kenshi controls, and the scenario's simulated writeGameSpeed clicks -
    // INCLUDING clicks equal to the current effective, which the old state-diff
    // detector could never see (the stuck-vote bug). Our own quiet applies are
    // reentrancy-guarded and never register. Pause requests as speed 0; the
    // requested multiplier survives a pause (the engine's own model), so
    // unpausing restores the player's request, not the arbitrated effective.
    bool userActed = false;
    {
        float im = 0.0f; bool ip = false;
        while (engine::consumeSpeedIntent(gw, &im, &ip)) {
            speedMyReq_ = ip ? 0.0f : im;
            userActed = true;
        }
    }
    if (speedMyReq_ < 0.0f) {
        // First tick (or hooks unavailable): seed the request from the live
        // engine state and announce it so the peer's arbitration seeds.
        float mult = 0.0f; bool paused = false;
        if (!engine::readGameSpeed(gw, &mult, &paused)) return;
        speedMyReq_ = paused ? 0.0f : mult;
        userActed = true;
    }
    if (userActed) {
        char b[112]; _snprintf(b, sizeof(b) - 1,
            "[speed] REQ mult=%.2f paused=%d combat=%d",
            speedMyReq_, (speedMyReq_ <= EPS) ? 1 : 0, speedMyCombat_ ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // Drain peer speed packets: the host keeps the join's latest REQUEST; the
    // join applies the host's arbitrated SET. The reliable channel is ordered,
    // but the seq guard keeps a (theoretical) stale packet from rolling back.
    std::deque<InboundSpeed> got;
    in.drainSpeed(got);
    for (std::deque<InboundSpeed>::iterator it = got.begin(); it != got.end(); ++it) {
        const SpeedPacket& p = it->pkt;
        if (p.seq != 0 && speedSeqSeen_ != 0 && (long)(p.seq - speedSeqSeen_) <= 0)
            continue;
        speedSeqSeen_ = p.seq;
        bool pkPaused = (p.flags & SPEED_PAUSED) != 0 || p.speed <= EPS;
        if (p.type == (u8)PKT_SPEED_REQ && isHost) {
            float req = pkPaused ? 0.0f : p.speed;
            bool  cmb = (p.flags & SPEED_IN_COMBAT) != 0;
            if (speedPeerReq_ < 0.0f || fabs(req - speedPeerReq_) > EPS ||
                cmb != speedPeerCombat_) {
                char b[112]; _snprintf(b, sizeof(b) - 1,
                    "[speed] REQ RECV owner=%u mult=%.2f paused=%d combat=%d",
                    (unsigned)it->ownerId, req, pkPaused ? 1 : 0, cmb ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            speedPeerReq_    = req;
            speedPeerCombat_ = cmb;
        } else if (p.type == (u8)PKT_SPEED_SET && !isHost) {
            // QUIET apply: drives the sim to the arbitrated effective without
            // touching the UI buttons - they keep showing this player's VOTE.
            // The clock slew (protocol 25) folds in here: the join's sim runs
            // at effective * timeSlew_ until its game clock matches the host's.
            float eff = pkPaused ? 0.0f : p.speed;
            if (engine::writeGameSpeedQuiet(gw, slewedEffective(eff), pkPaused)) {
                speedLastApplied_ = eff;
                bool changed = (speedLastSet_ < 0.0f || fabs(eff - speedLastSet_) > EPS);
                speedLastSet_ = eff;
                if (changed) { // safety resends stay silent; changes are the signal
                    char b[112]; _snprintf(b, sizeof(b) - 1,
                        "[speed] SET mult=%.2f paused=%d combat=%d",
                        eff, pkPaused ? 1 : 0,
                        (p.flags & SPEED_IN_COMBAT) ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
        }
    }

    if (isHost) {
        // Arbitrate: effective = min(my request, peer request), capped at 1x
        // while either player squad fights. The cap never force-unpauses -
        // pause (0) is already below 1, so min semantics preserve it.
        float eff = (speedMyReq_ >= 0.0f) ? speedMyReq_ : 1.0f;
        if (speedPeerReq_ >= 0.0f && speedPeerReq_ < eff) eff = speedPeerReq_;
        bool combat = speedMyCombat_ || speedPeerCombat_;
        if (combat && eff > 1.0f) eff = 1.0f;
        bool changed = (speedLastSet_ < 0.0f || fabs(eff - speedLastSet_) > EPS);
        // userActed with an UNCHANGED effective = a denied raise (consensus
        // holdback): re-apply immediately so the host engine doesn't run fast
        // until the enforcement below - a click is a request, not an override.
        if (changed || userActed || speedLastSendMs_ == 0 ||
            (now - speedLastSendMs_) >= RESEND_MS) {
            bool effPaused = (eff <= EPS);
            // slewedEffective is identity on the host (timeSlew_ stays 1.0).
            if (engine::writeGameSpeedQuiet(gw, slewedEffective(eff), effPaused))
                speedLastApplied_ = eff;
            SpeedPacket pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.type    = (u8)PKT_SPEED_SET;
            pkt.ownerId = ownerId;
            pkt.seq     = speedSeqOut_++;
            pkt.speed   = eff;
            pkt.flags   = (effPaused ? SPEED_PAUSED : 0) |
                          (combat ? SPEED_IN_COMBAT : 0);
            net.queueSpeed(pkt);
            speedLastSet_    = eff;
            speedLastSendMs_ = now;
            if (changed) {
                char b[128]; _snprintf(b, sizeof(b) - 1,
                    "[speed] SET mult=%.2f paused=%d combat=%d (my=%.2f peer=%.2f)",
                    eff, effPaused ? 1 : 0, combat ? 1 : 0,
                    speedMyReq_, speedPeerReq_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
    } else {
        // Join: send our request on change / combat edge / safety resend.
        if (userActed || combatEdge || speedLastSendMs_ == 0 ||
            (now - speedLastSendMs_) >= RESEND_MS) {
            SpeedPacket pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.type    = (u8)PKT_SPEED_REQ;
            pkt.ownerId = ownerId;
            pkt.seq     = speedSeqOut_++;
            pkt.speed   = (speedMyReq_ >= 0.0f) ? speedMyReq_ : 1.0f;
            pkt.flags   = ((speedMyReq_ >= 0.0f && speedMyReq_ <= EPS) ? SPEED_PAUSED : 0) |
                          (speedMyCombat_ ? SPEED_IN_COMBAT : 0);
            net.queueSpeed(pkt);
            speedLastSendMs_ = now;
        }
    }

    // Continuous enforcement (replaces the old snap-back): a REAL user click
    // passes through the hooked setters, so the engine briefly leaves the
    // effective (e.g. a denied raise runs fast for one tick, a local unpause
    // resumes while the peer still holds pause). Re-assert the effective
    // quietly whenever the engine diverges - the click stays captured as a
    // VOTE above, the sim never keeps a non-arbitrated speed, and the buttons
    // keep showing the vote.
    if (speedLastSet_ >= 0.0f) {
        float mult = 0.0f; bool paused = false;
        if (engine::readGameSpeed(gw, &mult, &paused)) {
            float cur = paused ? 0.0f : mult;
            // The enforcement target carries the clock slew (protocol 25):
            // comparing against the UNSLEWED effective would revert the slew
            // write every tick and the join's clock could never catch up.
            float want = slewedEffective(speedLastSet_);
            if (fabs(cur - want) > EPS) {
                if (engine::writeGameSpeedQuiet(gw, want, speedLastSet_ <= EPS))
                    speedLastApplied_ = speedLastSet_;
            }
        }
    }
}

void Replicator::syncTime(GameWorld* gw, Inbound& in, NetLink& net, u32 ownerId,
                          bool isHost) {
    std::deque<InboundTime> got;
    in.drainTime(got);
    if (!timeSync_) return;
    unsigned long now = nowMs();

    if (isHost) {
        // The authority just broadcasts its absolute clock at ~1 Hz; the join
        // does all the correcting. timeSlew_ stays 1.0 here by construction.
        const unsigned long SEND_MS = 1000;
        if (timeLastSendMs_ != 0 && (now - timeLastSendMs_) < SEND_MS) return;
        double hours = -1.0;
        if (!engine::readGameClock(gw, &hours, 0) || hours < 0.0) return;
        timeLastSendMs_ = now;
        TimePacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type      = (u8)PKT_TIME;
        pkt.ownerId   = ownerId;
        pkt.seq       = timeSeqOut_++;
        pkt.gameHours = hours;
        net.queueTime(pkt);
        return;
    }

    // JOIN: newest sample wins (ordered-reliable channel; the seq guard is
    // belt-and-braces). No new sample this tick = keep the current slew - the
    // next 1 Hz sample re-measures what the slew achieved.
    const TimePacket* newest = 0;
    for (std::deque<InboundTime>::iterator it = got.begin(); it != got.end(); ++it) {
        if (timeSeqSeen_ != 0 && (long)(it->pkt.seq - timeSeqSeen_) <= 0) continue;
        timeSeqSeen_ = it->pkt.seq;
        newest = &it->pkt;
    }
    if (!newest) return;
    double local = -1.0;
    if (!engine::readGameClock(gw, &local, 0) || local < 0.0) return;
    // Sample age is one wire hop (~ms) = well under 0.001 game hours at the
    // measured hour length (109 s/gh, time_probe run 141509) - no extrapolation.
    double off = newest->gameHours - local; // >0 = we are BEHIND, speed up

    // SLEW is the one working lever. A clock STEP was tried and rejected:
    // writing the timeStamper perf-timer base (self-verifying, reverted on
    // mismatch) never moved getTimeStamp_inGameHours - the absolute calendar
    // is a separate global the frame tick advances, not a live read off that
    // timer (run 150001; the RVA clusters agree: the clock reads sit with the
    // environment code, far from SimpleTimeStamper). So the join CATCHES UP
    // by running its sim faster - a visible but bounded session-start
    // transient (~35 s at 2x for the typical ~0.3 gh load skew).
    //
    // Proportional slew with hysteresis. Capped at 2x - a speed the game runs
    // routinely (a 4x cap converged faster but disturbed the join's world
    // enough to dip npc_sync tracking below gate, run 142912); the clock rate
    // tracks fsm exactly (time_probe), so the gain tapers the correction
    // smoothly into the deadband (no bang-bang).
    const double ENGAGE_GH    = 0.01;  // dead until |off| > 36 game-seconds
    const double DISENGAGE_GH = 0.002; // slewing until |off| < 7 game-seconds
    const double GAIN         = 30.0;  // slew delta per game-hour of offset
    bool slewing = (timeSlew_ < 0.999f || timeSlew_ > 1.001f);
    bool engage  = slewing ? (fabs(off) > DISENGAGE_GH) : (fabs(off) > ENGAGE_GH);
    float newSlew = 1.0f;
    if (engage) {
        double d = off * GAIN;
        if (d >  1.0) d =  1.0;  // cap: 2x sim while far behind
        if (d < -0.75) d = -0.75; // floor: 0.25x sim while ahead (never pause)
        newSlew = (float)(1.0 + d);
    }
    bool slewChanged = fabs(newSlew - timeSlew_) > 0.01f;
    timeSlew_ = newSlew;

    // Apply through the consensus layer immediately (its continuous
    // enforcement would converge next tick anyway; this shaves the latency).
    // speedLastSet_ < 0 = no consensus state yet (or speedSync off): degrade
    // to measuring - there is no lever to compose with.
    if (slewChanged && speedLastSet_ > 0.01f) {
        float want = slewedEffective(speedLastSet_);
        if (engine::writeGameSpeedQuiet(gw, want, false))
            timeSlewApplied_ = want;
    }
    bool logNow = slewChanged || timeLastLogMs_ == 0 ||
                  (now - timeLastLogMs_) >= 5000;
    if (logNow) {
        timeLastLogMs_ = now;
        char b[144];
        _snprintf(b, sizeof(b) - 1,
                  "[time] OFFSET off=%.4fgh slew=%.2f host=%.5f local=%.5f",
                  off, timeSlew_, newest->gameHours, local);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::syncSpawns(GameWorld* gw, Inbound& in, NetLink& net, u32 ownerId,
                            bool isHost) {
    // Request pacing: per-hand debounce (the reliable channel guarantees
    // delivery, so a resend only covers "asked before the reply landed"),
    // a hard retry cap, and a long backoff after a NEGATIVE reply (the host
    // couldn't resolve it either - e.g. the squad despawned - or the local
    // proxy spawn failed; neither improves in seconds).
    const unsigned long REQ_DEBOUNCE_MS  = 2000;
    const unsigned int  REQ_MAX_SENDS    = 5;
    const unsigned long DENIED_RETRY_MS  = 30000;
    // Re-keyed-away hands (protocol 35): how long the OLD key stays REQ/mint-
    // dead after a re-key retired it (covers any batch/reply still in flight).
    const unsigned long REKEYED_GRACE_MS = 10000;
    // Host reply throttle: a re-request inside this window is a duplicate in
    // flight, not a new question.
    const unsigned long REPLY_THROTTLE_MS = 2000;
    // Proximity gate: only ask about unresolved hands streamed NEAR our own
    // squad (the interest capture radius is 200u). A far unresolved hand is
    // usually a BAKED NPC in a world block this client hasn't loaded yet -
    // minting a proxy for it would create a DUPLICATE once the block loads.
    const float SPAWN_REQ_RADIUS = 250.0f;
    unsigned long now = nowMs();
    (void)isHost; // protocol 23: the channel is BIDIRECTIONAL - a join RECRUIT
                  // of a runtime-born NPC mints a hand the HOST cannot resolve,
                  // so BOTH sides answer requests AND author them.

    // Proxy liveness sweep (2026-07-11 join crash): the engine owns the proxy
    // bodies and can despawn one at any time; every path below (and
    // applyTargets) would then touch a freed pointer. ~1 s cadence round-trip
    // proof per proxy: SEH-read the pointer's CURRENT hand and resolve it back
    // - getting the same pointer proves the body is alive (and survives engine
    // re-containering, which merely changes the hand). Anything else means the
    // pointer is stale: unbind WITHOUT further touches and let the normal
    // census-missing / REQ machinery re-mint if the host still streams it.
    static unsigned long sweepMs = 0; // main-thread only
    if (!proxyByKey_.empty() && (now - sweepMs) >= 1000) {
        sweepMs = now;
        for (std::map<Key, Character*>::iterator it = proxyByKey_.begin();
             it != proxyByKey_.end(); ) {
            unsigned int h[5];
            Character* live = 0;
            if (engine::readHand(it->second, h))
                live = engine::resolveCharByHand(h[0], h[1], h[2], h[3], h[4]);
            if (live != it->second) {
                char b[160]; _snprintf(b, sizeof(b) - 1,
                    "[spawn] proxy DESPAWNED hand=%u,%u,%u,%u,%u (unbound; proxies=%u)",
                    it->first.t, it->first.c, it->first.cs, it->first.i,
                    it->first.s, (unsigned)proxyByKey_.size() - 1);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                spawnReq_.erase(it->first); // allow a fresh REQ/mint cycle
                proxyByKey_.erase(it++);
            } else ++it;
        }
    }

    // ---- Answering side (both clients) ---------------------------------------
    {
        std::deque<InboundSpawnReq> got;
        in.drainSpawnReqs(got);
        for (std::deque<InboundSpawnReq>::iterator it = got.begin();
             it != got.end(); ++it) {
            const SpawnReqPacket& rq = it->pkt;
            Key k; k.t = rq.hType; k.c = rq.hContainer; k.cs = rq.hContainerSerial;
            k.i = rq.hIndex; k.s = rq.hSerial;
            std::map<Key, unsigned long>::iterator rt = spawnReplyMs_.find(k);
            if (rt != spawnReplyMs_.end() && (now - rt->second) < REPLY_THROTTLE_MS)
                continue;
            spawnReplyMs_[k] = now;

            SpawnInfoPacket pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.type    = (u8)PKT_SPAWN_INFO;
            pkt.ownerId = ownerId;
            pkt.hType = rq.hType; pkt.hContainer = rq.hContainer;
            pkt.hContainerSerial = rq.hContainerSerial;
            pkt.hIndex = rq.hIndex; pkt.hSerial = rq.hSerial;
            Character* c = engine::resolveCharByHand(k.i, k.s, k.t, k.c, k.cs);
            bool dead = false;
            bool found = c && engine::describeCharacter(
                c, pkt.charSid, sizeof(pkt.charSid), pkt.facSid, sizeof(pkt.facSid),
                &pkt.x, &pkt.y, &pkt.z, &pkt.heading, &dead);
            pkt.found = found ? 1 : 0;
            pkt.dead  = dead ? 1 : 0;
            net.queueSpawnInfo(pkt);
            char b[224]; _snprintf(b, sizeof(b) - 1,
                "[spawn] INFO send hand=%u,%u,%u,%u,%u found=%d dead=%d sid='%s' fac='%s'",
                k.t, k.c, k.cs, k.i, k.s, pkt.found, pkt.dead,
                pkt.charSid, pkt.facSid);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    // ---- Requesting side (both clients) ---------------------------------------
    // Land replies first (a proxy bound this tick is driven this same frame).
    std::deque<InboundSpawnInfo> got;
    in.drainSpawnInfos(got);

    // Census-missing scan (2026-07-11 "NPCs spawn on top of the join player"
    // fix): the census is the join's EXISTENCE authority out to ~2000 u, but
    // proxies used to mint only off STREAMED states (host's ~200 u capture
    // bubble) - a raid walking in materialized on top of the player. Ask
    // about every fresh-census hand with no local body; the mint decision
    // happens on the reply (which carries the host's position) against
    // spawnMintRadius_. A resolvable hand is skipped (the shared save's baked
    // NPC), as is anything already proxied, denied, or recently answered
    // "too far". ~0.5 Hz: the resolve sweep is cheap but not free.
    const unsigned long FAR_RETRY_MS   = 5000;
    // Mint duplicate guard: a visible uncorrelated same-template body within
    // this range of the reply position defers the mint (far-retry cadence).
    const float MINT_DUPE_RADIUS = 20.0f;
    const unsigned long CENSUS_SCAN_MS = 2000;
    if (spawnMintRadius_ > 0.0f && !censusHands_.empty() &&
        censusRecvMs_ != 0 && (now - censusRecvMs_) <= 5000 &&
        (now - censusScanMs_) >= CENSUS_SCAN_MS) {
        censusScanMs_ = now;
        for (std::set<Key>::iterator it = censusHands_.begin();
             it != censusHands_.end(); ++it) {
            const Key& k = *it;
            if (proxyByKey_.find(k) != proxyByKey_.end()) continue;
            if (ownHands_.find(k) != ownHands_.end()) continue;
            if (unresolvedHands_.find(k) != unresolvedHands_.end()) continue;
            std::map<Key, unsigned long>::iterator rko = rekeyedOld_.find(k);
            if (rko != rekeyedOld_.end() && (now - rko->second) < REKEYED_GRACE_MS)
                continue;
            std::map<Key, SpawnReqState>::iterator sq = spawnReq_.find(k);
            if (sq != spawnReq_.end()) {
                if (sq->second.deniedMs != 0 &&
                    (now - sq->second.deniedMs) < DENIED_RETRY_MS) continue;
                if (sq->second.farMs != 0 &&
                    (now - sq->second.farMs) < FAR_RETRY_MS) continue;
            }
            if (engine::resolveCharByHand(k.i, k.s, k.t, k.c, k.cs)) continue;
            UnresolvedHand& u = unresolvedHands_[k];
            u.fromCensus = true;
            if (spawnLogged_.insert(k).second) {
                char b[144]; _snprintf(b, sizeof(b) - 1,
                    "[spawn] census-missing hand=%u,%u,%u,%u,%u (no local body)",
                    k.t, k.c, k.cs, k.i, k.s);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
    }

    // Own-squad positions: the reply-side mint gate and the request-side
    // proximity gate both measure against them. One capture per tick, only
    // when there is work.
    const unsigned int MAX_SQUAD = 32;
    static EntityState squad[MAX_SQUAD]; // main-thread only
    unsigned int nSquad = 0;
    if (!got.empty() || !unresolvedHands_.empty())
        nSquad = engine::captureSquad(gw, false, squad, MAX_SQUAD);

    for (std::deque<InboundSpawnInfo>::iterator it = got.begin();
         it != got.end(); ++it) {
        const SpawnInfoPacket& p = it->pkt;
        Key k; k.t = p.hType; k.c = p.hContainer; k.cs = p.hContainerSerial;
        k.i = p.hIndex; k.s = p.hSerial;
        SpawnReqState& rq = spawnReq_[k];
        if (!p.found) {
            rq.deniedMs = now;
            char b[128]; _snprintf(b, sizeof(b) - 1,
                "[spawn] INFO negative hand=%u,%u,%u,%u,%u (host can't resolve)",
                k.t, k.c, k.cs, k.i, k.s);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            continue;
        }
        if (proxyByKey_.find(k) != proxyByKey_.end()) continue; // duplicate reply
        // A hand re-keyed away (recruit/squad move) between our REQ and this
        // reply is DEAD - minting it would resurrect the duplicate the re-key
        // just cleaned up (protocol 35 run 192211).
        std::map<Key, unsigned long>::iterator rko = rekeyedOld_.find(k);
        if (rko != rekeyedOld_.end() && (now - rko->second) < REKEYED_GRACE_MS)
            continue;
        // Mint-radius gate (census-mint fix): the reply position is the
        // host's authority - mint only within spawnMintRadius_ of our own
        // squad. This is the DUPLICATE guard moved from send time to reply
        // time: a baked NPC inside this range sits in a loaded block and
        // would have resolved locally, so an unresolvable hand here is a
        // genuine host runtime spawn. Outside the radius: soft-defer (the
        // NPC may be walking toward us - retry on the FAR_RETRY_MS cadence,
        // and reset the send cap so a long approach can't exhaust it).
        if (spawnMintRadius_ > 0.0f) {
            float best = -1.0f;
            for (unsigned int i = 0; i < nSquad; ++i) {
                float d = dist3(p.x, p.y, p.z, squad[i].x, squad[i].y, squad[i].z);
                if (best < 0.0f || d < best) best = d;
            }
            if (best < 0.0f || best > spawnMintRadius_) {
                rq.farMs = now;
                rq.sends = 0;
                char b[160]; _snprintf(b, sizeof(b) - 1,
                    "[spawn] INFO deferred (far) hand=%u,%u,%u,%u,%u dist=%.0f mint=%.0f",
                    k.t, k.c, k.cs, k.i, k.s, best, spawnMintRadius_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                continue;
            }
        }
        // Mint duplicate guard (2026-07-11): a VISIBLE body of the same
        // template already near the reply position means the census-missing
        // hand is probably THAT body under a hand we cannot correlate (engine
        // re-container, baked block just loaded) - minting would stand a
        // double on top of it. Bound proxies (they answer to their own stream
        // keys - pack members mint meters apart) and suppressed culls (hidden;
        // often the very copy whose old hand got culled) are excluded, so a
        // legit mint only defers while a visible uncorrelated twin stands
        // there; the far-retry cadence re-judges in 5 s.
        {
            static Character* excl[NPC_CENSUS_MAX]; // main-thread only
            unsigned int ne = 0;
            for (std::map<Key, Character*>::iterator pi = proxyByKey_.begin();
                 pi != proxyByKey_.end() && ne < NPC_CENSUS_MAX; ++pi)
                excl[ne++] = pi->second;
            for (std::map<Key, Character*>::iterator si = suppressed_.begin();
                 si != suppressed_.end() && ne < NPC_CENSUS_MAX; ++si)
                excl[ne++] = si->second;
            Character* twin = engine::sameTemplateNear(gw, p.charSid,
                                                       p.x, p.y, p.z,
                                                       MINT_DUPE_RADIUS,
                                                       excl, ne);
            if (twin) {
                rq.farMs = now;
                rq.sends = 0;
                char b[176]; _snprintf(b, sizeof(b) - 1,
                    "[spawn] INFO deferred (dupe) hand=%u,%u,%u,%u,%u sid='%s' "
                    "twin within %.0fu",
                    k.t, k.c, k.cs, k.i, k.s, p.charSid, MINT_DUPE_RADIUS);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                continue;
            }
        }
        Character* proxy = engine::spawnProxyNpc(gw, p.charSid, p.facSid,
                                                 p.x, p.y, p.z, p.heading);
        if (!proxy) {
            // Local mint failed (template/faction absent here - modded host?).
            // Back off hard; retrying in seconds cannot succeed.
            rq.deniedMs = now;
            char b[160]; _snprintf(b, sizeof(b) - 1,
                "[spawn] proxy FAILED hand=%u,%u,%u,%u,%u sid='%s'",
                k.t, k.c, k.cs, k.i, k.s, p.charSid);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            continue;
        }
        proxyByKey_[k] = proxy;
        // Dead on arrival: latch the down state now (the same reliable-latch
        // path an EVT_DEATH would take) so the proxy spawns INTO ragdoll
        // instead of standing up for a frame. Latched entries never age out.
        if (p.dead) targets_[k].deathLatched = true;
        char b[192]; _snprintf(b, sizeof(b) - 1,
            "[spawn] proxy BOUND hand=%u,%u,%u,%u,%u sid='%s' fac='%s' dead=%d "
            "pos=%.1f,%.1f,%.1f (proxies=%u)",
            k.t, k.c, k.cs, k.i, k.s, p.charSid, p.facSid, p.dead ? 1 : 0,
            p.x, p.y, p.z, (unsigned)proxyByKey_.size());
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // Author requests for the hands applyTargets recorded unresolved last
    // tick (proximity-gated to our own squad members) plus this tick's
    // census-missing hands (no position - the reply-side mint gate judges).
    if (!unresolvedHands_.empty()) {
        for (std::map<Key, UnresolvedHand>::iterator it = unresolvedHands_.begin();
             it != unresolvedHands_.end(); ++it) {
            const Key& k = it->first;
            if (proxyByKey_.find(k) != proxyByKey_.end()) continue;
            // Never ask about a hand a re-key just retired (protocol 35): a
            // stale batch of the OLD key may still be in flight, and a REQ
            // for it would mint the duplicate the re-key exists to prevent.
            std::map<Key, unsigned long>::iterator rko = rekeyedOld_.find(k);
            if (rko != rekeyedOld_.end() && (now - rko->second) < REKEYED_GRACE_MS)
                continue;
            SpawnReqState& rq = spawnReq_[k];
            if (rq.deniedMs != 0) {
                if ((now - rq.deniedMs) < DENIED_RETRY_MS) continue;
                rq.deniedMs = 0; rq.sends = 0; // cooldown over: ask again
            }
            // Recent "too far" reply: the hand exists host-side but outside
            // the mint radius - re-ask on the FAR_RETRY_MS cadence only.
            if (rq.farMs != 0 && (now - rq.farMs) < FAR_RETRY_MS) continue;
            if (rq.sends >= REQ_MAX_SENDS) continue;
            if (rq.lastSendMs != 0 && (now - rq.lastSendMs) < REQ_DEBOUNCE_MS) continue;
            // Streamed unresolved hands keep the legacy send-side proximity
            // gate. Census-missing hands have NO position (the census is a
            // bare hand list) - for them the reply-side mint gate is the
            // duplicate guard.
            if (!it->second.fromCensus) {
                bool nearSquad = false;
                for (unsigned int i = 0; i < nSquad && !nearSquad; ++i) {
                    nearSquad = dist3(it->second.x, it->second.y, it->second.z,
                                      squad[i].x, squad[i].y, squad[i].z) <= SPAWN_REQ_RADIUS;
                }
                if (!nearSquad) continue;
            }
            SpawnReqPacket pkt;
            memset(&pkt, 0, sizeof(pkt));
            pkt.type    = (u8)PKT_SPAWN_REQ;
            pkt.ownerId = ownerId;
            pkt.hType = k.t; pkt.hContainer = k.c; pkt.hContainerSerial = k.cs;
            pkt.hIndex = k.i; pkt.hSerial = k.s;
            net.queueSpawnReq(pkt);
            rq.lastSendMs = now;
            ++rq.sends;
            char b[144]; _snprintf(b, sizeof(b) - 1,
                "[spawn] REQ hand=%u,%u,%u,%u,%u send=%u",
                k.t, k.c, k.cs, k.i, k.s, rq.sends);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
    unresolvedHands_.clear();

    // Proxy telemetry (~2 Hz while proxies live): the STREAMED key plus the
    // proxy body's ACTUAL local position, in the SCENARIO series shape (hand
    // order i,s,t,c,cs like MEMBER/RECV lines) so the spawn_sync oracle can
    // time-pair it with the host's MEMBER series per hand.
    if (!proxyByKey_.empty() && (now - spawnPosLogMs_) >= 500) {
        spawnPosLogMs_ = now;
        for (std::map<Key, Character*>::iterator it = proxyByKey_.begin();
             it != proxyByKey_.end(); ++it) {
            float x = 0, y = 0, z = 0;
            if (!engine::readPos(it->second, &x, &y, &z)) continue;
            char b[176]; _snprintf(b, sizeof(b) - 1,
                "SCENARIO PROXY hand=%u,%u,%u,%u,%u pos=%.2f,%.2f,%.2f",
                it->first.i, it->first.s, it->first.t, it->first.c, it->first.cs,
                x, y, z);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
}

void Replicator::publishInventories(GameWorld* gw, NetLink& net, u32 ownerId) {
    // Author the inventory of every container we OWN. ownHands_ is the per-tick set of
    // owned squad-member hands (a character's own hand IS its personal-inventory
    // container hand), so streaming it makes inventory sync fully BIDIRECTIONAL and
    // disjoint by the same tab-rank partition as positional sync (host streams tab-0
    // members, join streams tab-1 members - Doctrine 8). ownedContainers_ adds any
    // explicitly-registered non-squad container (e.g. a baked storage chest / the
    // SETUP-seeded leader). applyInventories skips this same union, so no client ever
    // reconciles a container it authors.
    std::set<Key> owned = ownedContainers_;
    owned.insert(ownHands_.begin(), ownHands_.end());
    // Protocol 34 (storeSync, HOST only): fold in the ~1 Hz container census -
    // every COMPLETE storage chest / machine container near the interest
    // centers becomes an authored container, riding the same per-container
    // hash + settle + safety-resend gate below. The census set is replaced
    // wholesale each pass (containers leaving interest stop being captured;
    // their invPub_ baseline survives for the return).
    if (storeSync_) {
        unsigned long cnow = nowMs();
        if (contCensusMs_ == 0 || (cnow - contCensusMs_) >= 1000) {
            contCensusMs_ = cnow;
            const unsigned int MAX_CONT = 48;
            static engine::ContRead rows[MAX_CONT]; // main-thread only
            unsigned int n = engine::enumContainersNear(gw, 100.0f, rows, MAX_CONT);
            censusContainers_.clear();
            for (unsigned int i = 0; i < n; ++i) {
                if (!rows[i].hasInv) continue; // no Inventory = nothing to author
                Key k; k.t = rows[i].hand[0]; k.c = rows[i].hand[1];
                k.cs = rows[i].hand[2]; k.i = rows[i].hand[3];
                k.s = rows[i].hand[4];
                censusContainers_.insert(k);
            }
        }
        owned.insert(censusContainers_.begin(), censusContainers_.end());
    }
    if (owned.empty()) return;
    const unsigned long INV_RESEND_MS = 5000; // periodic safety resend (loss/late join)
    // A changed snapshot must be STABLE this long before we publish it. A change that only
    // REARRANGES or ADDS (entry count >= last sent) settles fast. A change that REMOVES an
    // entry settles much longer: mid-drag the UI holds the dragged item on the CURSOR, out
    // of the inventory entirely, for up to ~1 s - a transient "item gone" the peer would act
    // on by DESTROYING a worn item. Weapons DO refabricate now (spike 451 recipe), but a
    // destroy+refabricate round-trip still loses identity/quality and churns, so the long
    // window stays. Equip and unequip-to-bag keep the entry count (a MOVE), so they still
    // replicate promptly; only genuine removals (and the in-cursor flicker) wait it out.
    const unsigned long INV_SETTLE_MS        = 350;
    const unsigned long INV_REMOVE_SETTLE_MS = 1800;
    InvItemEntry items[INV_ITEMS_MAX];
    unsigned long now = nowMs();
    for (std::set<Key>::iterator it = owned.begin();
         it != owned.end(); ++it) {
        unsigned int cHand[5] = { it->t, it->c, it->cs, it->i, it->s };
        // Skip until the container actually resolves here (post-load it may not yet),
        // so we never blast a spurious "empty" snapshot that would wipe baked contents.
        if (engine::resolveObjectByHand(cHand) == 0) continue;
        u32 hash = 0;
        unsigned int n = engine::captureContainerContents(gw, cHand, items, INV_ITEMS_MAX, &hash);
        std::map<Key, InvPub>::iterator pit = invPub_.find(*it);
        bool first = (pit == invPub_.end());
        if (first) {
            // Track from now; let it settle before the initial publish (cheap, and avoids
            // emitting a half-built inventory captured mid-load).
            InvPub p; p.hash = 0; p.lastSendMs = 0; p.pendingHash = hash; p.pendingSince = now;
            p.lastSentN = 0;
            invPub_[*it] = p;
            pit = invPub_.find(*it);
        }
        InvPub& pub = pit->second;
        bool sent = (pub.lastSendMs != 0) || (pub.hash != 0);
        bool differs = !sent || (pub.hash != hash);
        // Maintain the settle timer: restart it whenever the captured fingerprint moves.
        if (hash != pub.pendingHash) { pub.pendingHash = hash; pub.pendingSince = now; }
        // A removal (fewer entries than last sent) waits out the long window; everything
        // else (additions, equip<->loose moves) settles fast.
        unsigned long settleMs = (sent && n < pub.lastSentN) ? INV_REMOVE_SETTLE_MS : INV_SETTLE_MS;
        bool settled  = (now - pub.pendingSince >= settleMs);
        bool changed  = differs && settled;
        bool periodic = sent && !differs && (now - pub.lastSendMs >= INV_RESEND_MS);
        if (!changed && !periodic) continue;
        // Protocol 34 wire identity: a session-placed building rides its
        // protocol-27 placer key (own placement = our hand; a minted proxy =
        // the reverse map). Characters / baked containers stay raw (kind 0).
        u8 keyKind = 0;
        u32 wireKey[5] = { it->t, it->c, it->cs, it->i, it->s };
        if (ownBuilds_.find(*it) != ownBuilds_.end()) {
            keyKind = 1;
        } else {
            std::map<Key, Key>::iterator mit = mintByLocal_.find(*it);
            if (mit != mintByLocal_.end()) {
                keyKind = 1;
                wireKey[0] = mit->second.t; wireKey[1] = mit->second.c;
                wireKey[2] = mit->second.cs; wireKey[3] = mit->second.i;
                wireKey[4] = mit->second.s;
            }
        }
        net.queueInvSnapshot(ownerId, keyKind, wireKey, items, n);
        pub.hash = hash; pub.lastSendMs = now; pub.lastSentN = n;
        if (changed) {
            char b[200];
            _snprintf(b, sizeof(b) - 1,
                "[inv] SEND hand=%u,%u,%u,%u,%u kind=%u key=%u,%u,%u,%u,%u items=%u hash=%u",
                it->t, it->c, it->cs, it->i, it->s, (unsigned)keyKind,
                wireKey[0], wireKey[1], wireKey[2], wireKey[3], wireKey[4],
                n, hash);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            static int dumpInv = -1;
            if (dumpInv < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dumpInv = (e && e[0] == '1') ? 1 : 0; }
            if (dumpInv) { coop::logLine("[inv] SEND-state:"); engine::dumpInventory(gw, cHand); }
        }
    }
}

void Replicator::applyInventories(GameWorld* gw) {
    if (invRecv_.empty()) return;
    for (std::map<Key, InvRecv>::iterator it = invRecv_.begin(); it != invRecv_.end(); ++it) {
        if (!it->second.dirty) continue;
        it->second.dirty = false;
        // Never reconcile a container we author (defense-in-depth on the partition):
        // any explicitly-registered container OR any squad member we own this tick.
        if (ownedContainers_.count(it->first) != 0) continue;
        if (ownHands_.count(it->first) != 0) continue;
        const Key& k = it->first;
        unsigned int cHand[5] = { k.t, k.c, k.cs, k.i, k.s };
        const InvItemEntry* items = it->second.items.empty() ? 0 : &it->second.items[0];
        unsigned int n = (unsigned int)it->second.items.size();
        // Protocol 37 (the race that blinded the detector in run 141024): if this
        // peer container's LOCAL contents differ from the transfer detector's
        // baseline, a user mutation (possibly one end of a cross-owner drag) has not
        // been adjudicated yet - reconciling NOW would undo the drag (the dupe/wipe)
        // and the post-apply rebase would erase the evidence. Defer briefly (the
        // detector scans at 400 ms / settles at 600 ms, so ~2 s covers pairing +
        // intent authoring); on deadline fall through (genuine desync heal). Only
        // active while the detector itself runs (xferSync on -> xferScanMs_ != 0).
        if (xferScanMs_ != 0 && xferSeeded_.count(k) != 0 &&
            engine::resolveObjectByHand(cHand) != 0) {
            const unsigned long XFER_DEFER_MS = 3000;
            InvItemEntry cur[64];
            unsigned int nc = engine::captureContainerContents(gw, cHand, cur, 64, 0);
            std::map<XKey, int> tot;
            for (unsigned int i = 0; i < nc; ++i) {
                int q = cur[i].quantity; if (q < 1) q = 1;
                tot[XKey(std::string(cur[i].stringID), cur[i].itemType)] += q;
            }
            if (tot != xferBase_[k]) {
                unsigned long now = nowMs();
                unsigned long& since = xferDefer_[k];
                if (since == 0) since = now;
                if (now - since < XFER_DEFER_MS) {
                    it->second.dirty = true; // re-visit next tick
                    continue;
                }
                char b[160]; _snprintf(b, sizeof(b) - 1,
                    "[xfer] defer-expired hand=%u,%u,%u,%u,%u (unadjudicated local diff; applying)",
                    k.t, k.c, k.cs, k.i, k.s);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            xferDefer_.erase(k);
        }
        // Protocol 37: an active transfer latch means this snapshot may be STALE with
        // respect to a cross-owner move (ours or an applied peer intent) the container's
        // owner hasn't republished yet. Adjust the desired list by each latch - a taken
        // item must not be re-added (the dupe), a given item must not be destroyed (the
        // wipe) - until the owner catches up (raw desired == local for the key) or the
        // grace deadline passes.
        std::vector<InvItemEntry> adj;
        std::map<Key, std::map<XKey, XferLatch> >::iterator lt = xferLatch_.find(k);
        if (lt != xferLatch_.end() && !lt->second.empty() &&
            engine::resolveObjectByHand(cHand) != 0) {
            unsigned long now = nowMs();
            // Local capture: totals for the catch-up check + entries for provenance.
            InvItemEntry loc[64];
            unsigned int nl = engine::captureContainerContents(gw, cHand, loc, 64, 0);
            adj.assign(items, items + n);
            for (std::map<XKey, XferLatch>::iterator le = lt->second.begin();
                 le != lt->second.end(); ) {
                const XKey& key = le->first;
                int want = 0;
                for (unsigned int i = 0; i < n; ++i)
                    if (items[i].itemType == key.second &&
                        strcmp(items[i].stringID, key.first.c_str()) == 0)
                        want += (items[i].quantity < 1) ? 1 : (int)items[i].quantity;
                int local = 0;
                for (unsigned int i = 0; i < nl; ++i)
                    if (loc[i].itemType == key.second &&
                        strcmp(loc[i].stringID, key.first.c_str()) == 0)
                        local += (loc[i].quantity < 1) ? 1 : (int)loc[i].quantity;
                if (want == local || now > le->second.deadlineMs) {
                    char b[200]; _snprintf(b, sizeof(b) - 1,
                        "[xfer] latch-%s hand=%u,%u,%u,%u,%u sid='%s' delta=%d",
                        (want == local) ? "caught-up" : "expired",
                        k.t, k.c, k.cs, k.i, k.s, key.first.c_str(), le->second.delta);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    lt->second.erase(le++);
                    continue;
                }
                int d = le->second.delta;
                if (d < 0) {
                    // We TOOK units: strip them from the desired list (loose stacks
                    // first) so the reconcile doesn't re-fabricate them here.
                    int strip = -d;
                    for (int pass = 0; pass < 2 && strip > 0; ++pass) {
                        for (unsigned int i = 0; i < adj.size() && strip > 0; ++i) {
                            if (adj[i].itemType != key.second) continue;
                            if ((int)adj[i].equipped != pass) continue;
                            if (strcmp(adj[i].stringID, key.first.c_str()) != 0) continue;
                            int have = adj[i].quantity; if (have < 1) have = 1;
                            int cut = (strip < have) ? strip : have;
                            adj[i].quantity = (u16)(have - cut);
                            strip -= cut;
                        }
                    }
                    for (unsigned int i = 0; i < adj.size(); )
                        if (adj[i].quantity == 0) adj.erase(adj.begin() + i); else ++i;
                } else if (d > 0) {
                    // We GAVE units: keep them in the desired list so the reconcile
                    // doesn't destroy them. Copy the real local entry (provenance).
                    InvItemEntry e; memset(&e, 0, sizeof(e));
                    bool found = false;
                    for (int pass = 0; pass < 2 && !found; ++pass)
                        for (unsigned int i = 0; i < nl; ++i) {
                            if (loc[i].itemType != key.second) continue;
                            if ((int)loc[i].equipped != pass) continue;
                            if (strcmp(loc[i].stringID, key.first.c_str()) != 0) continue;
                            e = loc[i]; found = true; break;
                        }
                    if (!found) {
                        strncpy(e.stringID, key.first.c_str(), sizeof(e.stringID) - 1);
                        e.itemType = key.second;
                    }
                    e.equipped = 0; e.slot = 0; e.section = 0;
                    e.quantity = (u16)d;
                    adj.push_back(e);
                }
                ++le;
            }
            if (lt->second.empty()) xferLatch_.erase(lt);
            items = adj.empty() ? 0 : &adj[0];
            n = (unsigned int)adj.size();
        }
        engine::applyContainerContents(gw, cHand, items, n);
        // Keep the transfer detector blind to the reconcile we just performed.
        xferRebase(gw, k);
        char b[160];
        _snprintf(b, sizeof(b) - 1,
            "[inv] APPLY hand=%u,%u,%u,%u,%u items=%u",
            k.t, k.c, k.cs, k.i, k.s, n);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        static int dumpInvA = -1;
        if (dumpInvA < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dumpInvA = (e && e[0] == '1') ? 1 : 0; }
        if (dumpInvA) { coop::logLine("[inv] APPLY-result:"); engine::dumpInventory(gw, cHand); }
    }
}

void Replicator::publishWorldItems(GameWorld* gw, NetLink& net, u32 ownerId) {
    // Owner-authoritative world stream, BOTH directions since the W1 bidir fix (each
    // client streams the free ground items IT authors - the join's dropped materials
    // were invisible on the host before). Scan the interest sphere for free ground
    // items, assign/reuse a netId per item (keyed by its local engine hand), and
    // stream new/changed items + cull vanished ones. A settled world produces stable
    // content+pos - so zero traffic - with a slow periodic safety resend.
    const float         RADIUS       = 60.0f; // interest scope for ground items (v1)
    const float         POS_EPS      = 0.5f;  // re-stream a moved item past this gap
    const unsigned long WI_RESEND_MS = 5000;  // periodic safety resend (loss / late join)
    static int dumpWi = -1;
    if (dumpWi < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dumpWi = (e && e[0] == '1') ? 1 : 0; }

    engine::WorldItemRaw raw[WORLD_ITEMS_MAX];
    unsigned int n = engine::captureWorldItems(gw, raw, WORLD_ITEMS_MAX, RADIUS);
    unsigned long now = nowMs();

    // ECHO GUARD: a proxy we spawned for a PEER's streamed item is a real local
    // RootObject and enumerates like any other ground item - re-publishing it would
    // bounce the item back to its author as a duplicate. Filter every capture row
    // that resolves to an object in our proxy set.
    std::set<RootObject*> proxyObjs;
    for (std::map<std::pair<u32, u32>, WorldProxy>::iterator pi = worldProxies_.begin();
         pi != worldProxies_.end(); ++pi)
        proxyObjs.insert(pi->second.obj);

    for (std::map<Key, WorldTrack>::iterator it = worldTrack_.begin(); it != worldTrack_.end(); ++it)
        it->second.seen = false;

    // Gear (itemType 2 WEAPON / 3 ARMOUR) is handled by the conservation drop/pickup channel
    // (the real object is relocated bag<->ground on each client and re-homed on pickup), so the
    // W1 template-proxy stream skips it - both to avoid a duplicate proxy AND because a W1 cull
    // only removes the join's proxy, leaving a host-dropped real item orphaned on the ground.
    WorldItemEntry send[WORLD_ITEMS_MAX]; unsigned int ns = 0;
    for (unsigned int i = 0; i < n; ++i) {
        if (isGearType(raw[i].itemType)) continue;
        if (!proxyObjs.empty() &&
            proxyObjs.count(engine::resolveObjectByHand(raw[i].hand)) != 0)
            continue; // peer-authored proxy - not ours to publish
        Key k; k.t = raw[i].hand[0]; k.c = raw[i].hand[1]; k.cs = raw[i].hand[2];
        k.i = raw[i].hand[3]; k.s = raw[i].hand[4];
        std::map<Key, WorldTrack>::iterator tit = worldTrack_.find(k);
        bool isNew = (tit == worldTrack_.end());
        if (isNew) {
            WorldTrack t; t.netId = nextWorldNetId_++; t.hash = 0; t.lastSendMs = 0;
            t.x = t.y = t.z = 0.0f; t.seen = true;
            worldTrack_[k] = t;
            tit = worldTrack_.find(k);
        }
        WorldTrack& tr = tit->second;
        tr.seen = true;
        bool sent = (tr.lastSendMs != 0);
        float dx = raw[i].x - tr.x, dy = raw[i].y - tr.y, dz = raw[i].z - tr.z;
        bool moved = (dx*dx + dy*dy + dz*dz) > (POS_EPS * POS_EPS);
        bool changed = !sent || (tr.hash != raw[i].hash) || moved;
        bool periodic = sent && !changed && (now - tr.lastSendMs >= WI_RESEND_MS);
        if (!changed && !periodic) continue;
        if (ns < WORLD_ITEMS_MAX) {
            WorldItemEntry& e = send[ns++];
            e.netId = tr.netId;
            strncpy(e.stringID, raw[i].stringID, sizeof(e.stringID) - 1);
            e.stringID[sizeof(e.stringID) - 1] = '\0';
            e.itemType = raw[i].itemType;
            e.quantity = raw[i].quantity;
            e.quality  = raw[i].quality;
            e.x = raw[i].x; e.y = raw[i].y; e.z = raw[i].z;
            e.state = 0;
        }
        tr.hash = raw[i].hash; tr.lastSendMs = now;
        tr.x = raw[i].x; tr.y = raw[i].y; tr.z = raw[i].z;
        if (changed && dumpWi) {
            char b[200]; _snprintf(b, sizeof(b) - 1,
                "[wi] SEND netId=%u sid='%s' qty=%u pos=%.2f,%.2f,%.2f hash=%u",
                tr.netId, raw[i].stringID, raw[i].quantity, raw[i].x, raw[i].y, raw[i].z, raw[i].hash);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    u32 removed[256]; unsigned int nr = 0;
    for (std::map<Key, WorldTrack>::iterator it = worldTrack_.begin(); it != worldTrack_.end(); ) {
        if (!it->second.seen) {
            if (nr < 256) removed[nr++] = it->second.netId;
            if (dumpWi) { char b[96]; _snprintf(b, sizeof(b) - 1, "[wi] CULL netId=%u", it->second.netId);
                          b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            worldTrack_.erase(it++);
        } else ++it;
    }

    if (ns > 0) net.queueWorldItems(ownerId, send, ns);
    if (nr > 0) net.queueWorldRemove(ownerId, removed, nr);
}

void Replicator::applyWorldItems(GameWorld* gw, Inbound& in) {
    std::deque<InboundWorldItems>  items;
    std::deque<InboundWorldRemove> rems;
    in.drainWorldItems(items);
    in.drainWorldRemove(rems);
    if (items.empty() && rems.empty()) return;
    static int dumpWi = -1;
    if (dumpWi < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dumpWi = (e && e[0] == '1') ? 1 : 0; }
    const float POS_EPS = 0.5f;

    // Snapshots: spawn a proxy for a new (owner, netId), move it if it changed.
    // netId spaces are per-sender (W1 bidir), so the owner scopes every key.
    for (std::deque<InboundWorldItems>::iterator b = items.begin(); b != items.end(); ++b) {
        for (std::vector<WorldItemEntry>::iterator e = b->items.begin(); e != b->items.end(); ++e) {
            std::pair<u32, u32> pk(b->ownerId, e->netId);
            std::map<std::pair<u32, u32>, WorldProxy>::iterator pit = worldProxies_.find(pk);
            if (pit == worldProxies_.end()) {
                RootObject* obj = engine::spawnWorldItemProxy(gw, e->stringID, e->itemType,
                                                              (int)e->quantity, e->x, e->y, e->z);
                if (obj) {
                    WorldProxy wp; wp.obj = obj; wp.x = e->x; wp.y = e->y; wp.z = e->z; wp.hash = 0;
                    worldProxies_[pk] = wp;
                }
                char b2[200]; _snprintf(b2, sizeof(b2) - 1,
                    "[wi] SPAWN owner=%u netId=%u ok=%d sid='%s' pos=%.2f,%.2f,%.2f",
                    b->ownerId, e->netId, obj ? 1 : 0, e->stringID, e->x, e->y, e->z);
                b2[sizeof(b2) - 1] = '\0'; if (dumpWi || !obj) coop::logLine(b2);
            } else {
                WorldProxy& wp = pit->second;
                float dx = e->x - wp.x, dy = e->y - wp.y, dz = e->z - wp.z;
                if ((dx*dx + dy*dy + dz*dz) > (POS_EPS * POS_EPS)) {
                    engine::updateWorldItemProxy(wp.obj, e->x, e->y, e->z);
                    wp.x = e->x; wp.y = e->y; wp.z = e->z;
                    if (dumpWi) { char b2[160]; _snprintf(b2, sizeof(b2) - 1,
                        "[wi] MOVE netId=%u pos=%.2f,%.2f,%.2f", e->netId, e->x, e->y, e->z);
                        b2[sizeof(b2) - 1] = '\0'; coop::logLine(b2); }
                }
            }
        }
    }
    // Culls: destroy the proxy and drop the mapping (scoped to the authoring owner).
    for (std::deque<InboundWorldRemove>::iterator b = rems.begin(); b != rems.end(); ++b) {
        for (std::vector<u32>::iterator id = b->netIds.begin(); id != b->netIds.end(); ++id) {
            std::map<std::pair<u32, u32>, WorldProxy>::iterator pit =
                worldProxies_.find(std::make_pair(b->ownerId, *id));
            if (pit == worldProxies_.end()) continue;
            engine::removeWorldItemProxy(gw, pit->second.obj);
            worldProxies_.erase(pit);
            if (dumpWi) { char b2[128]; _snprintf(b2, sizeof(b2) - 1,
                "[wi] CULL owner=%u netId=%u", b->ownerId, *id);
                b2[sizeof(b2) - 1] = '\0'; coop::logLine(b2); }
        }
    }
}

void Replicator::detectAndPublishWeaponDrops(GameWorld* gw, NetLink& net, u32 ownerId) {
    if (ownHands_.empty()) return;
    // Correlate the bag-loss with a FREE ground item anywhere in the interest sphere (not just
    // at the feet): a UI drop lands at the cursor, which can be many metres away. A TRADE moves
    // the item into another BAG (isInInventory=true), so it is never a free ground item - hence
    // even a generous radius cannot mistake a trade for a drop.
    const float        GROUND_R    = 60.0f;
    const int          MAX_RETRY   = 30;    // ticks to keep looking for the ground copy
    static int dumpWd = -1;
    if (dumpWd < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dumpWd = (e && e[0] == '1') ? 1 : 0; }
    InvItemEntry items[INV_ITEMS_MAX];
    for (std::set<Key>::iterator it = ownHands_.begin(); it != ownHands_.end(); ++it) {
        unsigned int cHand[5] = { it->t, it->c, it->cs, it->i, it->s };
        if (engine::resolveObjectByHand(cHand) == 0) continue;
        unsigned int n = engine::captureContainerContents(gw, cHand, items, INV_ITEMS_MAX, 0);
        // Build this character's CURRENT GEAR census (copies per "sid", with provenance + type).
        std::map<std::string, WCensusItem> cur;
        for (unsigned int i = 0; i < n; ++i) {
            if (!isGearType(items[i].itemType)) continue;
            WCensusItem& wc = cur[std::string(items[i].stringID)];
            int q = items[i].quantity; if (q < 1) q = 1;
            if (wc.count == 0) {
                strncpy(wc.manufacturer, items[i].manufacturer, sizeof(wc.manufacturer) - 1);
                strncpy(wc.material,     items[i].material,     sizeof(wc.material) - 1);
                wc.quality  = items[i].quality;
                wc.itemType = items[i].itemType;
            }
            wc.count += q;
        }
        // Capture the REAL Item* of each weapon this tick (parallel to the census), so a
        // DROP can remember the exact now-grounded object for a later pickup (the spatial
        // query can't re-find it in towns).
        char wsids[INV_ITEMS_MAX][48];
        void* wptrs[INV_ITEMS_MAX];
        unsigned int nwp = engine::captureWeaponPtrs(gw, cHand, wsids, wptrs, INV_ITEMS_MAX);
        std::map<std::string, std::deque<void*> > curPtrs;
        for (unsigned int i = 0; i < nwp; ++i) curPtrs[std::string(wsids[i])].push_back(wptrs[i]);

        WCensus& prevC = weaponCensus_[*it];
        if (!prevC.seeded) {
            prevC.items = cur; prevC.ptrs = curPtrs; prevC.seeded = true; // baseline; never emit
            if (dumpWd) { char b[140]; _snprintf(b, sizeof(b) - 1,
                "[wd] census-seed hand=%u,%u,%u,%u,%u weaponKinds=%u",
                it->t, it->c, it->cs, it->i, it->s, (unsigned)cur.size());
                b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            continue;
        }
        // INCREASE pass (PICKUP): a weapon kind whose count rose was picked up by this owned
        // character. Author a reliable PICKUP intent so the peer re-homes its tracked ground
        // copy into this character's bag; consume one of OUR tracked ground copies (the local
        // UI pickup already moved the real object from ground to bag). Done before the drop
        // pass mutates `cur` for debounce. (No tracked copy on the peer => no-op there, so a
        // brand-new/looted weapon never spuriously appears.)
        for (std::map<std::string, WCensusItem>::iterator ce = cur.begin(); ce != cur.end(); ++ce) {
            std::map<std::string, WCensusItem>::iterator pp = prevC.items.find(ce->first);
            int prevCount = (pp != prevC.items.end()) ? pp->second.count : 0;
            int inc = ce->second.count - prevCount;
            // Protocol 37: a pending/applied cross-owner gear transfer must not be
            // read as a ground PICKUP of the same sid (the count edge is the trade).
            // The end-of-loop baseline update absorbs the new count silently.
            if (inc > 0 && wdSuppressed(*it, ce->first.c_str(), nowMs())) {
                if (dumpWd) { char b[160]; _snprintf(b, sizeof(b) - 1,
                    "[wd] increase-suppressed (xfer) sid='%s' inc=%d", ce->first.c_str(), inc);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                continue;
            }
            for (int k = 0; k < inc; ++k) {
                WorldPickupPacket pkt; memset(&pkt, 0, sizeof(pkt));
                pkt.type = (u8)PKT_WORLD_PICKUP; pkt.ownerId = ownerId;
                pkt.pickupId = nextPickupId_++;
                pkt.oType = it->t; pkt.oContainer = it->c; pkt.oContainerSerial = it->cs;
                pkt.oIndex = it->i; pkt.oSerial = it->s;
                strncpy(pkt.stringID, ce->first.c_str(), sizeof(pkt.stringID) - 1);
                pkt.itemType = ce->second.itemType; pkt.quality = ce->second.quality;
                net.queueWorldPickup(pkt);
                std::deque<void*>& q = groundedWeapons_[ce->first];
                if (!q.empty()) q.pop_front(); // our ground copy is now in the bag
                if (dumpWd) { char b[200]; _snprintf(b, sizeof(b) - 1,
                    "[wd] PICKUP id=%u sid='%s' owner=%u,%u,%u,%u,%u prev=%d now=%d trackedLeft=%u",
                    pkt.pickupId, pkt.stringID, it->t, it->c, it->cs, it->i, it->s,
                    prevCount, ce->second.count, (unsigned)q.size());
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            }
        }
        // Walk every previously-held weapon kind; a count DECREASE is a candidate drop.
        for (std::map<std::string, WCensusItem>::iterator pe = prevC.items.begin();
             pe != prevC.items.end(); ++pe) {
            std::map<std::string, WCensusItem>::iterator ce = cur.find(pe->first);
            int now = (ce != cur.end()) ? ce->second.count : 0;
            int delta = pe->second.count - now;
            if (delta <= 0) { prevC.retries.erase(pe->first); continue; }
            // Protocol 37: a pending/applied cross-owner gear transfer must not be
            // read as a ground DROP of the same sid (the count edge is the trade;
            // the baseline update below absorbs it silently).
            if (wdSuppressed(*it, pe->first.c_str(), nowMs())) {
                prevC.retries.erase(pe->first);
                if (dumpWd) { char b[160]; _snprintf(b, sizeof(b) - 1,
                    "[wd] decrease-suppressed (xfer) sid='%s' delta=%d", pe->first.c_str(), delta);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                continue;
            }
            float pos[3] = { 0, 0, 0 };
            unsigned int gtype = pe->second.itemType;
            bool onGround = engine::firstFreeGroundItemPos(gw, cHand, pe->first.c_str(),
                                                           gtype, GROUND_R, pos) != 0;
            if (!onGround) {
                // The engine's spatial item query couldn't locate a free ground copy. This is
                // common in towns and for equipped-then-dropped weapons (the query returns
                // nothing even at a wide radius - see diagGroundScan). Debounce a few ticks to
                // shrug off a 1-frame equip/swap transient WITHOUT committing the lower count.
                int& r = prevC.retries[pe->first];
                if (r == 0) {
                    r = MAX_RETRY;
                    if (dumpWd) engine::diagGroundScan(gw, cHand, pe->first.c_str(), GROUND_R);
                }
                // Protocol 37: while the transfer detector is still watching an
                // unresolved LOSS of this sid from this container, the count edge may
                // be a bag-to-bag trade mid-detection - keep holding rather than
                // committing the drop-fallback (the detector either fires the intent,
                // which registers a suppression, or folds the diff and releases us).
                if (r <= 1 && xferPendingLoss(*it, pe->first.c_str())) r = MAX_RETRY;
                if (--r > 0) {
                    if (dumpWd) { char b[160]; _snprintf(b, sizeof(b) - 1,
                        "[wd] decrease-pending hand=%u,%u,%u,%u,%u sid='%s' prev=%d now=%d retry=%d",
                        it->t, it->c, it->cs, it->i, it->s, pe->first.c_str(),
                        pe->second.count, now, r); b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                    cur[pe->first] = pe->second;        // hold the old count so we re-check next tick
                    curPtrs[pe->first] = prevC.ptrs[pe->first]; // and keep the departed Item* handle(s)
                    continue;
                }
                // Debounce expired: the weapon really LEFT this owned character (we never mutate
                // owned inventories ourselves, so this is a genuine user action). Author the drop
                // at the OWNER's position as the mirror target - the weapon was dropped at its
                // feet, so the peer relocating its own copy there reproduces the drop. (A rare
                // intra-squad trade would be mirrored as a drop here; reconcile then corrects it.)
                prevC.retries.erase(pe->first);
                if (!engine::objectWorldPos(cHand, pos)) {
                    if (dumpWd) { char b[160]; _snprintf(b, sizeof(b) - 1,
                        "[wd] decrease-nopos hand=%u,%u,%u,%u,%u sid='%s' (owner pos unresolved; skip)",
                        it->t, it->c, it->cs, it->i, it->s, pe->first.c_str());
                        b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                    continue;
                }
                if (dumpWd) { char b[200]; _snprintf(b, sizeof(b) - 1,
                    "[wd] drop-fallback hand=%u,%u,%u,%u,%u sid='%s' (no ground copy; owner pos=%.1f,%.1f,%.1f)",
                    it->t, it->c, it->cs, it->i, it->s, pe->first.c_str(), pos[0], pos[1], pos[2]);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            } else {
                prevC.retries.erase(pe->first);
            }
            // The departed weapon's REAL Item* is the prior tick's handle for this sid; after a
            // UI drop it persists as the now-grounded object (conservation). Remember it so a
            // later PICKUP intent re-homes this exact object without a spatial re-query.
            std::deque<void*>& departed = prevC.ptrs[pe->first];
            for (int d = 0; d < delta; ++d) {
                void* di = departed.empty() ? 0 : departed.front();
                // Prefer the REAL dropped object's position (the exact cursor-drop spot) over
                // the owner-feet fallback - and it's query-free, so both clients agree.
                float dpos[3] = { pos[0], pos[1], pos[2] };
                if (di) { float ip[3]; if (engine::itemWorldPos(di, ip)) {
                    dpos[0] = ip[0]; dpos[1] = ip[1]; dpos[2] = ip[2]; } }
                WorldDropPacket pkt; memset(&pkt, 0, sizeof(pkt));
                pkt.type = (u8)PKT_WORLD_DROP; pkt.ownerId = ownerId; pkt.dropId = nextDropId_++;
                pkt.oType = it->t; pkt.oContainer = it->c; pkt.oContainerSerial = it->cs;
                pkt.oIndex = it->i; pkt.oSerial = it->s;
                strncpy(pkt.stringID, pe->first.c_str(), sizeof(pkt.stringID) - 1);
                pkt.itemType = gtype; pkt.quality = pe->second.quality;
                strncpy(pkt.manufacturer, pe->second.manufacturer, sizeof(pkt.manufacturer) - 1);
                strncpy(pkt.material,     pe->second.material,     sizeof(pkt.material) - 1);
                pkt.x = dpos[0]; pkt.y = dpos[1]; pkt.z = dpos[2];
                net.queueWorldDrop(pkt);
                if (di) {
                    groundedWeapons_[pe->first].push_back(di);
                    departed.pop_front();
                }
                char b[220]; _snprintf(b, sizeof(b) - 1,
                    "[wd] DROP id=%u sid='%s' owner=%u,%u,%u,%u,%u pos=%.2f,%.2f,%.2f tracked=%u",
                    pkt.dropId, pkt.stringID, it->t, it->c, it->cs, it->i, it->s,
                    pkt.x, pkt.y, pkt.z, (unsigned)groundedWeapons_[pe->first].size());
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        prevC.items = cur;
        prevC.ptrs  = curPtrs;
    }
}

void Replicator::applyWeaponDrops(GameWorld* gw, Inbound& in) {
    std::deque<InboundWorldDrop> got;
    in.drainWorldDrops(got);
    if (got.empty()) return;
    for (std::deque<InboundWorldDrop>::iterator it = got.begin(); it != got.end(); ++it) {
        const WorldDropPacket& p = it->pkt;
        std::pair<u32, u32> id(p.ownerId, p.dropId);
        if (appliedDrops_.count(id) != 0) continue; // idempotent (reliable resend / replay)
        appliedDrops_.insert(id);
        // Bounded (step 6): ids are per-sender monotonic, so evicting the smallest
        // discards the oldest - far outside any plausible reliable-channel replay.
        if (appliedDrops_.size() > 4096) appliedDrops_.erase(appliedDrops_.begin());
        Key ok; ok.t = p.oType; ok.c = p.oContainer; ok.cs = p.oContainerSerial;
        ok.i = p.oIndex; ok.s = p.oSerial;
        if (ownHands_.count(ok) != 0) continue;     // we own this char -> we dropped it locally
        unsigned int ownerHand[5] = { p.oType, p.oContainer, p.oContainerSerial,
                                      p.oIndex, p.oSerial };
        void* dropped = 0;
        int moved = engine::relocateWeaponToGround(gw, ownerHand, p.stringID, p.itemType,
                                                   p.x, p.y, p.z, &dropped);
        // Track the relocated REAL object so a later PICKUP intent re-homes this exact handle
        // back into the owner's bag (no spatial re-query, which fails in towns).
        if (moved > 0 && dropped) groundedWeapons_[std::string(p.stringID)].push_back(dropped);
        // Keep the transfer detector blind to the relocation we just made.
        if (moved > 0) xferRebase(gw, ok);
        char b[240]; _snprintf(b, sizeof(b) - 1,
            "[wd] APPLY id=%u sid='%s' owner=%u,%u,%u,%u,%u moved=%d pos=%.2f,%.2f,%.2f tracked=%u",
            p.dropId, p.stringID, p.oType, p.oContainer, p.oContainerSerial, p.oIndex,
            p.oSerial, moved, p.x, p.y, p.z,
            (unsigned)groundedWeapons_[std::string(p.stringID)].size());
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::applyWeaponPickups(GameWorld* gw, Inbound& in) {
    std::deque<InboundWorldPickup> got;
    in.drainWorldPickups(got);
    if (got.empty()) return;
    for (std::deque<InboundWorldPickup>::iterator it = got.begin(); it != got.end(); ++it) {
        const WorldPickupPacket& p = it->pkt;
        std::pair<u32, u32> id(p.ownerId, p.pickupId);
        if (appliedPickups_.count(id) != 0) continue; // idempotent (reliable resend / replay)
        appliedPickups_.insert(id);
        if (appliedPickups_.size() > 4096) appliedPickups_.erase(appliedPickups_.begin());
        Key ok; ok.t = p.oType; ok.c = p.oContainer; ok.cs = p.oContainerSerial;
        ok.i = p.oIndex; ok.s = p.oSerial;
        if (ownHands_.count(ok) != 0) continue;        // we own this char -> we picked it up locally
        unsigned int targetHand[5] = { p.oType, p.oContainer, p.oContainerSerial,
                                       p.oIndex, p.oSerial };
        std::deque<void*>& q = groundedWeapons_[std::string(p.stringID)];
        int moved = 0;
        if (!q.empty()) {
            void* item = q.front();
            moved = engine::addItemPtrToInventory(gw, targetHand, item);
            if (moved) q.pop_front(); // re-homed; stop tracking it on the ground
        }
        char b[240]; _snprintf(b, sizeof(b) - 1,
            "[wd] PICKUP-APPLY id=%u sid='%s' owner=%u,%u,%u,%u,%u moved=%d trackedLeft=%u",
            p.pickupId, p.stringID, p.oType, p.oContainer, p.oContainerSerial, p.oIndex,
            p.oSerial, moved, (unsigned)q.size());
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        // Keep the transfer detector blind to the relocation we just made.
        Key tk; tk.t = p.oType; tk.c = p.oContainer; tk.cs = p.oContainerSerial;
        tk.i = p.oIndex; tk.s = p.oSerial;
        xferRebase(gw, tk);
    }
}

// ---- Protocol 37: cross-owner transfer intents ------------------------------

void Replicator::xferRebase(GameWorld* gw, const Key& k) {
    unsigned int cHand[5] = { k.t, k.c, k.cs, k.i, k.s };
    std::map<XKey, int>& base = xferBase_[k];
    base.clear();
    if (engine::resolveObjectByHand(cHand) != 0) {
        InvItemEntry items[64];
        unsigned int n = engine::captureContainerContents(gw, cHand, items, 64, 0);
        for (unsigned int i = 0; i < n; ++i) {
            int q = items[i].quantity; if (q < 1) q = 1;
            base[XKey(std::string(items[i].stringID), items[i].itemType)] += q;
        }
    }
    xferSeeded_[k] = true;
    xferPend_.erase(k);
}

bool Replicator::xferPendingLoss(const Key& k, const char* sid) {
    std::map<Key, std::map<XKey, XferPend> >::iterator pit = xferPend_.find(k);
    if (pit == xferPend_.end()) return false;
    for (std::map<XKey, XferPend>::iterator e = pit->second.begin();
         e != pit->second.end(); ++e)
        if (e->second.delta < 0 && e->first.first == sid) return true;
    return false;
}

bool Replicator::wdSuppressed(const Key& k, const char* sid, unsigned long now) {
    std::map<std::pair<Key, std::string>, unsigned long>::iterator it =
        wdSuppress_.find(std::make_pair(k, std::string(sid)));
    if (it == wdSuppress_.end()) return false;
    if (now > it->second) { wdSuppress_.erase(it); return false; }
    return true;
}

void Replicator::detectAndPublishTransfers(GameWorld* gw, NetLink& net, u32 ownerId) {
    const unsigned long XFER_SCAN_MS   = 400;   // detector cadence
    const unsigned long XFER_SETTLE_MS = 600;   // a diff must persist (mid-drag cursor hold)
    const unsigned long XFER_PEND_MS   = 6000;  // unpaired diff folds back into the baseline
    const unsigned long XFER_GRACE_MS  = 10000; // reconcile-suppression latch lifetime
    unsigned long now = nowMs();
    if (xferScanMs_ != 0 && now - xferScanMs_ < XFER_SCAN_MS) return;
    xferScanMs_ = now;
    static int dumpX = -1;
    if (dumpX < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dumpX = (e && e[0] == '1') ? 1 : 0; }

    // Tracked set: every container we author + every peer container we have received a
    // snapshot for. Both ends of any drag a player can perform live in this union.
    std::set<Key> tracked = ownedContainers_;
    tracked.insert(ownHands_.begin(), ownHands_.end());
    for (std::map<Key, InvRecv>::iterator ri = invRecv_.begin(); ri != invRecv_.end(); ++ri)
        tracked.insert(ri->first);
    if (tracked.empty()) return;

    // Capture this scan's per-item totals for each resolvable container.
    InvItemEntry items[64];
    std::map<Key, std::map<XKey, int> > cur;
    for (std::set<Key>::iterator it = tracked.begin(); it != tracked.end(); ++it) {
        unsigned int cHand[5] = { it->t, it->c, it->cs, it->i, it->s };
        if (engine::resolveObjectByHand(cHand) == 0) continue;
        std::map<XKey, int>& tot = cur[*it];
        unsigned int n = engine::captureContainerContents(gw, cHand, items, 64, 0);
        for (unsigned int i = 0; i < n; ++i) {
            int q = items[i].quantity; if (q < 1) q = 1;
            tot[XKey(std::string(items[i].stringID), items[i].itemType)] += q;
        }
        if (!xferSeeded_[*it]) { xferBase_[*it] = tot; xferSeeded_[*it] = true; cur.erase(*it); }
    }

    // Refresh the pend set: per container, per item key, the current diff vs baseline.
    // A diff that returns to zero (cursor put the item back) drops its pend; a diff
    // that CHANGES restarts its settle clock; a diff that outlives XFER_PEND_MS never
    // paired - fold it into the baseline (a lone loss is a drop/consume, a lone gain
    // is loot/craft: the owner's own snapshot channel carries those).
    for (std::map<Key, std::map<XKey, int> >::iterator ci = cur.begin(); ci != cur.end(); ++ci) {
        const Key& k = ci->first;
        std::map<XKey, int>& base = xferBase_[k];
        std::map<XKey, XferPend>& pend = xferPend_[k];
        std::set<XKey> keys;
        for (std::map<XKey, int>::iterator b = base.begin(); b != base.end(); ++b) keys.insert(b->first);
        for (std::map<XKey, int>::iterator c = ci->second.begin(); c != ci->second.end(); ++c) keys.insert(c->first);
        for (std::set<XKey>::iterator ky = keys.begin(); ky != keys.end(); ++ky) {
            std::map<XKey, int>::iterator bi = base.find(*ky);
            std::map<XKey, int>::iterator cv = ci->second.find(*ky);
            int delta = ((cv != ci->second.end()) ? cv->second : 0)
                      - ((bi != base.end()) ? bi->second : 0);
            std::map<XKey, XferPend>::iterator pe = pend.find(*ky);
            if (delta == 0) {
                if (pe != pend.end()) pend.erase(pe);
                continue;
            }
            if (pe == pend.end()) {
                XferPend p; p.delta = delta; p.sinceMs = now;
                pend[*ky] = p;
            } else if (pe->second.delta != delta) {
                pe->second.delta = delta; pe->second.sinceMs = now;
            } else if (now - pe->second.sinceMs >= XFER_PEND_MS) {
                // Never paired: fold into the baseline and stop watching.
                if (cv != ci->second.end()) base[*ky] = cv->second; else base.erase(*ky);
                if (dumpX) { char b[200]; _snprintf(b, sizeof(b) - 1,
                    "[xfer] fold hand=%u,%u,%u,%u,%u sid='%s' delta=%d (unpaired)",
                    k.t, k.c, k.cs, k.i, k.s, ky->first.c_str(), delta);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                pend.erase(*ky);
            }
        }
    }

    // PAIR pass: a settled LOSS of an item key in one container + the matching settled
    // GAIN in another is a completed drag between the two. Collect first (rebase
    // invalidates the pend iterators), then act.
    struct Fire { Key src; Key dst; XKey key; int qty; };
    std::vector<Fire> fires;
    std::set<Key> consumed;
    for (std::map<Key, std::map<XKey, XferPend> >::iterator li = xferPend_.begin();
         li != xferPend_.end(); ++li) {
        if (consumed.count(li->first) || cur.find(li->first) == cur.end()) continue;
        for (std::map<XKey, XferPend>::iterator le = li->second.begin();
             le != li->second.end(); ++le) {
            if (le->second.delta >= 0) continue;
            if (now - le->second.sinceMs < XFER_SETTLE_MS) continue;
            for (std::map<Key, std::map<XKey, XferPend> >::iterator gi = xferPend_.begin();
                 gi != xferPend_.end(); ++gi) {
                if (gi == li || consumed.count(gi->first) || cur.find(gi->first) == cur.end())
                    continue;
                std::map<XKey, XferPend>::iterator ge = gi->second.find(le->first);
                if (ge == gi->second.end() || ge->second.delta <= 0) continue;
                if (now - ge->second.sinceMs < XFER_SETTLE_MS) continue;
                Fire f; f.src = li->first; f.dst = gi->first; f.key = le->first;
                f.qty = -le->second.delta;
                if (ge->second.delta < f.qty) f.qty = ge->second.delta;
                fires.push_back(f);
                consumed.insert(f.src); consumed.insert(f.dst);
                break;
            }
            if (consumed.count(li->first)) break;
        }
    }

    for (unsigned int i = 0; i < fires.size(); ++i) {
        const Fire& f = fires[i];
        bool srcOwn = ownedContainers_.count(f.src) != 0 || ownHands_.count(f.src) != 0;
        bool dstOwn = ownedContainers_.count(f.dst) != 0 || ownHands_.count(f.dst) != 0;
        if (!srcOwn || !dstOwn) {
            // At least one end is peer-authored: the single-writer snapshots cannot
            // carry this move - author the reliable transfer intent.
            InvXferPacket pkt; memset(&pkt, 0, sizeof(pkt));
            pkt.type = (u8)PKT_INV_XFER; pkt.ownerId = ownerId; pkt.xferId = nextXferId_++;
            pkt.sType = f.src.t; pkt.sContainer = f.src.c; pkt.sContainerSerial = f.src.cs;
            pkt.sIndex = f.src.i; pkt.sSerial = f.src.s;
            pkt.dType = f.dst.t; pkt.dContainer = f.dst.c; pkt.dContainerSerial = f.dst.cs;
            pkt.dIndex = f.dst.i; pkt.dSerial = f.dst.s;
            strncpy(pkt.stringID, f.key.first.c_str(), sizeof(pkt.stringID) - 1);
            pkt.itemType = f.key.second;
            pkt.quantity = (u16)((f.qty > 65535) ? 65535 : f.qty);
            // Provenance/quality off the moved stack (it lives in dst now) - a peer
            // may need them if it has to fabricate a missing non-gear copy.
            unsigned int dHand[5] = { f.dst.t, f.dst.c, f.dst.cs, f.dst.i, f.dst.s };
            unsigned int nd = engine::captureContainerContents(gw, dHand, items, 64, 0);
            for (unsigned int j = 0; j < nd; ++j) {
                if (items[j].itemType != f.key.second) continue;
                if (strcmp(items[j].stringID, f.key.first.c_str()) != 0) continue;
                pkt.quality = items[j].quality;
                strncpy(pkt.manufacturer, items[j].manufacturer, sizeof(pkt.manufacturer) - 1);
                strncpy(pkt.material,     items[j].material,     sizeof(pkt.material) - 1);
                break;
            }
            net.queueInvXfer(pkt);
            // Latch the pending move on each PEER end so applyInventories cannot
            // reconcile it back while the owner's snapshots are still stale.
            if (!srcOwn) {
                XferLatch& L = xferLatch_[f.src][f.key];
                L.delta -= f.qty; L.deadlineMs = now + XFER_GRACE_MS;
                if (L.delta == 0) xferLatch_[f.src].erase(f.key);
            }
            if (!dstOwn) {
                XferLatch& L = xferLatch_[f.dst][f.key];
                L.delta += f.qty; L.deadlineMs = now + XFER_GRACE_MS;
                if (L.delta == 0) xferLatch_[f.dst].erase(f.key);
            }
            // A gear trade must not be read by the W2 weapon census as a ground
            // drop (src) / pickup (dst) of the same sid.
            if (isGearType(f.key.second)) {
                wdSuppress_[std::make_pair(f.src, f.key.first)] = now + XFER_GRACE_MS;
                wdSuppress_[std::make_pair(f.dst, f.key.first)] = now + XFER_GRACE_MS;
            }
            char b[240]; _snprintf(b, sizeof(b) - 1,
                "[xfer] SEND id=%u sid='%s' type=%u qty=%d src=%u,%u,%u,%u,%u(%s) dst=%u,%u,%u,%u,%u(%s)",
                pkt.xferId, pkt.stringID, pkt.itemType, f.qty,
                f.src.t, f.src.c, f.src.cs, f.src.i, f.src.s, srcOwn ? "own" : "peer",
                f.dst.t, f.dst.c, f.dst.cs, f.dst.i, f.dst.s, dstOwn ? "own" : "peer");
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // Own<->own moves need no intent (our own snapshots carry both ends); either
        // way the baselines absorb the move so the detector never re-fires on it.
        xferRebase(gw, f.src);
        xferRebase(gw, f.dst);
    }
}

void Replicator::applyTransfers(GameWorld* gw, Inbound& in, u32 localId) {
    std::deque<InboundInvXfer> got;
    in.drainInvXfers(got);
    if (got.empty()) return;
    const unsigned long XFER_GRACE_MS = 10000;
    unsigned long now = nowMs();
    for (std::deque<InboundInvXfer>::iterator it = got.begin(); it != got.end(); ++it) {
        const InvXferPacket& p = it->pkt;
        if (p.ownerId == localId) continue; // never act on our own (relay safety)
        std::pair<u32, u32> id(p.ownerId, p.xferId);
        if (appliedXfers_.count(id) != 0) continue; // idempotent (reliable resend / replay)
        appliedXfers_.insert(id);
        if (appliedXfers_.size() > 4096) appliedXfers_.erase(appliedXfers_.begin());
        unsigned int sHand[5] = { p.sType, p.sContainer, p.sContainerSerial, p.sIndex, p.sSerial };
        unsigned int dHand[5] = { p.dType, p.dContainer, p.dContainerSerial, p.dIndex, p.dSerial };
        Key sk; sk.t = p.sType; sk.c = p.sContainer; sk.cs = p.sContainerSerial;
        sk.i = p.sIndex; sk.s = p.sSerial;
        Key dk; dk.t = p.dType; dk.c = p.dContainer; dk.cs = p.dContainerSerial;
        dk.i = p.dIndex; dk.s = p.dSerial;
        // Relocate OUR copy of the real item between the same two containers - the
        // conservation move (never fabricates or destroys), so gear survives.
        int moved = engine::moveItemBetweenContainers(gw, sHand, dHand, p.stringID,
                                                      p.itemType, (int)p.quantity);
        int fab = 0;
        if (moved < (int)p.quantity) {
            // Our src copy is short (desync) - fabricate the shortfall into dst so the
            // trade still lands. Non-gear always did this; gear joined once spike 451
            // made weapon fabrication work (armour always could). Dupe safety: the
            // latch below keeps stale snapshots from reconciling the fab away, and
            // wdSuppress_ keeps the W2 weapon census from reading the count edge as a
            // ground pickup. KENSHICOOP_WEAPON_FAB=0 restores gear-never-fabricates
            // (weapons also die inside createItemAndAdd on the same env).
            static int gearFab = -1;
            if (gearFab < 0) { const char* e = getenv("KENSHICOOP_WEAPON_FAB"); gearFab = (e && e[0] == '0') ? 0 : 1; }
            if (!isGearType(p.itemType) || gearFab)
                fab = engine::addItemsToContainerBySid(gw, dHand, p.stringID, p.itemType,
                                                       (int)p.quantity - moved, (int)p.quality,
                                                       p.manufacturer, p.material);
        }
        XKey key(std::string(p.stringID), p.itemType);
        // Latch OUR peer end(s) too: an in-flight stale snapshot (captured by its
        // owner before this transfer) must not reconcile the relocation away.
        bool srcOwn = ownedContainers_.count(sk) != 0 || ownHands_.count(sk) != 0;
        bool dstOwn = ownedContainers_.count(dk) != 0 || ownHands_.count(dk) != 0;
        int applied = moved + fab;
        if (applied > 0) {
            if (!srcOwn) {
                XferLatch& L = xferLatch_[sk][key];
                L.delta -= applied; L.deadlineMs = now + XFER_GRACE_MS;
                if (L.delta == 0) xferLatch_[sk].erase(key);
            }
            if (!dstOwn) {
                XferLatch& L = xferLatch_[dk][key];
                L.delta += applied; L.deadlineMs = now + XFER_GRACE_MS;
                if (L.delta == 0) xferLatch_[dk].erase(key);
            }
        }
        if (isGearType(p.itemType)) {
            wdSuppress_[std::make_pair(sk, key.first)] = now + XFER_GRACE_MS;
            wdSuppress_[std::make_pair(dk, key.first)] = now + XFER_GRACE_MS;
        }
        // Keep the transfer detector blind to the relocation we just made.
        xferRebase(gw, sk);
        xferRebase(gw, dk);
        char b[240]; _snprintf(b, sizeof(b) - 1,
            "[xfer] APPLY id=%u from=%u sid='%s' type=%u qty=%u moved=%d fab=%d",
            p.xferId, p.ownerId, p.stringID, p.itemType, (unsigned)p.quantity, moved, fab);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::applyEvents(GameWorld* gw, Inbound& in) {
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
            case EVT_AMPUTATE:
            case EVT_CRUSH: {
                // Reliable limb-loss transition (protocol 16): apply the same
                // engine lever the owner's damage sim ran (amputate WITHOUT the
                // severed item - the world-item channel owns the ground copy).
                // Never on a body we own (our sim is the authority there), and
                // idempotent: applyLimbStates no-ops when states already match.
                if (ownHands_.find(k) != ownHands_.end()) break;
                int limb = (int)ev.arg;
                if (limb < 0 || limb > 3) break;
                Character* c = engine::resolveCharByHand(k.i, k.s, k.t, k.c, k.cs);
                if (!c) break; // not loaded here; the medical stream self-heals later
                unsigned char states[4] = { LIMB_STATE_UNKNOWN, LIMB_STATE_UNKNOWN,
                                            LIMB_STATE_UNKNOWN, LIMB_STATE_UNKNOWN };
                states[limb] = (ev.event == EVT_AMPUTATE) ? LIMB_WIRE_STUMP
                                                          : LIMB_WIRE_CRUSHED;
                // Host = world authority: create the real severed ground item
                // here (it streams to the join via the world-item channel).
                int r = engine::applyLimbStates(0, c, states, 0,
                                                /*createSeveredItem*/streamNpcs_);
                char lb[140]; _snprintf(lb, sizeof(lb) - 1,
                    "[med] LIMB-EVT APPLY ev=%u hand=%u,%u limb=%d mask=%d",
                    (unsigned)ev.event, k.i, k.s, limb, r);
                lb[sizeof(lb) - 1] = '\0'; coop::logLine(lb);
                break;
            }
            case EVT_PICKUP_BODY: {
                // Carried-body sync (protocol 18): subject = the CARRIED body,
                // actor = the CARRIER. Resolve BOTH locally and run the engine's
                // own pickup between the LOCAL pair - the shoulder attach, carry
                // animation, and transform-follow are all engine-native here.
                // Works for own-tab, cross-tab, and either direction: each
                // machine mirrors the relationship between its own instances.
                if (!carrySync_) break;
                Character* carrier = engine::resolveCharByHand(
                    ev.aIndex, ev.aSerial, ev.aType, ev.aContainer, ev.aContainerSerial);
                unsigned int ch[5] = { ev.sType, ev.sContainer, ev.sContainerSerial,
                                       ev.sIndex, ev.sSerial };
                bool ok = carrier && engine::applyPickup(0, carrier, ch);
                char cb[160]; _snprintf(cb, sizeof(cb) - 1,
                    "[carry] RECV PICKUP id=%u carrier=%u,%u carried=%u,%u ok=%d",
                    ev.eventId, ev.aIndex, ev.aSerial, ev.sIndex, ev.sSerial,
                    ok ? 1 : 0);
                cb[sizeof(cb) - 1] = '\0'; coop::logLine(cb);
                break;
            }
            case EVT_DROP_BODY: {
                // The inverse: release the local carrier's carried body (arg =
                // ragdoll flag). Idempotent - a carrier that never picked up
                // locally (lost/late pickup) is a no-op success.
                if (!carrySync_) break;
                Character* carrier = engine::resolveCharByHand(
                    ev.aIndex, ev.aSerial, ev.aType, ev.aContainer, ev.aContainerSerial);
                bool ok = carrier && engine::applyDrop(carrier, ev.arg != 0.0f);
                char cb[160]; _snprintf(cb, sizeof(cb) - 1,
                    "[carry] RECV DROP id=%u carrier=%u,%u carried=%u,%u ok=%d",
                    ev.eventId, ev.aIndex, ev.aSerial, ev.sIndex, ev.sSerial,
                    ok ? 1 : 0);
                cb[sizeof(cb) - 1] = '\0'; coop::logLine(cb);
                break;
            }
            case EVT_ENTER_FURNITURE: {
                // Furniture occupancy (protocol 19): subject = the OCCUPANT,
                // actor = the FURNITURE's save-stable hand (both clients loaded
                // the same save, so it resolves locally). Run the engine's own
                // setBedMode/setPrisonMode between the LOCAL pair - the in-bed/
                // in-cage pose and transform are engine-native here. On a body
                // we OWN, only a THIRD-PARTY placement is honoured (protocol
                // 36): the world authority jailed our KO'd body (a guard
                // action that runs purely on the host sim - our engine never
                // executed it). Conscious voluntary use stays owner-authored:
                // for those our engine did the real placement and this event
                // is just the echo of our own edge.
                if (!furnSync_) break;
                if (ownHands_.find(k) != ownHands_.end()) {
                    Character* own = engine::resolveCharByHand(k.i, k.s, k.t, k.c, k.cs);
                    bool down = own && coop::bodyIsDown(engine::readBodyState(own));
                    engine::FurnitureRead ofr;
                    bool already = own && engine::readFurniture(own, &ofr) &&
                                   ofr.valid && ofr.kind == (int)ev.arg;
                    // Race guard: the host re-authors PEER-ENTER on a 5 s
                    // cadence, so one can be in flight when we free our own
                    // body - a recent owner-side exit vetoes the stale enter.
                    std::map<Key, unsigned long>::iterator ox = ownFurnExit_.find(k);
                    bool justExited = ox != ownFurnExit_.end() &&
                                      (nowMs() - ox->second) < 10000;
                    if (!down || already || justExited) {
                        char sb[160]; _snprintf(sb, sizeof(sb) - 1,
                            "[furn] RECV PEER-ENTER own occ=%u,%u SKIP (down=%d already=%d exited=%d)",
                            k.i, k.s, down ? 1 : 0, already ? 1 : 0,
                            justExited ? 1 : 0);
                        sb[sizeof(sb) - 1] = '\0'; coop::logLine(sb);
                        break;
                    }
                }
                Character* occ = engine::resolveCharByHand(k.i, k.s, k.t, k.c, k.cs);
                unsigned int fh[5] = { ev.aType, ev.aContainer, ev.aContainerSerial,
                                       ev.aIndex, ev.aSerial };
                int kind = (int)ev.arg;
                bool ok = occ && engine::applyFurniture(0, occ, fh, kind, true);
                char fb[160]; _snprintf(fb, sizeof(fb) - 1,
                    "[furn] RECV ENTER id=%u occ=%u,%u furn=%u,%u kind=%d ok=%d",
                    ev.eventId, ev.sIndex, ev.sSerial, ev.aIndex, ev.aSerial,
                    kind, ok ? 1 : 0);
                fb[sizeof(fb) - 1] = '\0'; coop::logLine(fb);
                break;
            }
            case EVT_EXIT_FURNITURE: {
                // The inverse: release the local occupant. Idempotent - a copy
                // that never entered locally (lost/late enter) is a no-op success.
                if (!furnSync_) break;
                if (ownHands_.find(k) != ownHands_.end()) break;
                Character* occ = engine::resolveCharByHand(k.i, k.s, k.t, k.c, k.cs);
                unsigned int fh[5] = { ev.aType, ev.aContainer, ev.aContainerSerial,
                                       ev.aIndex, ev.aSerial };
                int kind = (int)ev.arg;
                bool ok = occ && engine::applyFurniture(0, occ, fh, kind, false);
                char fb[160]; _snprintf(fb, sizeof(fb) - 1,
                    "[furn] RECV EXIT id=%u occ=%u,%u furn=%u,%u kind=%d ok=%d",
                    ev.eventId, ev.sIndex, ev.sSerial, ev.aIndex, ev.aSerial,
                    kind, ok ? 1 : 0);
                fb[sizeof(fb) - 1] = '\0'; coop::logLine(fb);
                break;
            }
            case EVT_RECRUIT: {
                // Protocol 23: the sender recruited subject (OLD hand) into
                // its squad as actor (NEW hand). Shared re-key path below.
                if (!recruitSync_) break;
                Key nk; nk.t = ev.aType; nk.c = ev.aContainer;
                nk.cs = ev.aContainerSerial; nk.i = ev.aIndex; nk.s = ev.aSerial;
                rekeyPeerBody(gw, k, nk, "recruit");
                break;
            }
            case EVT_SQUAD_MOVE: {
                // Protocol 35: the sender MOVED subject (OLD hand) between its
                // squad tabs; actor = the fresh hand the move minted
                // (squad_probe: index/serial do not survive a re-container).
                // A zeroed actor = the body LEFT the sender's roster
                // (dismissal): just drop our pins/binding for the old key -
                // the body reverts to whatever the world partition says.
                if (!squadSync_) break;
                Key nk; nk.t = ev.aType; nk.c = ev.aContainer;
                nk.cs = ev.aContainerSerial; nk.i = ev.aIndex; nk.s = ev.aSerial;
                if ((nk.t | nk.c | nk.cs | nk.i | nk.s) == 0) {
                    pinPeer_.erase(k);
                    pinOwned_.erase(k);
                    proxyByKey_.erase(k);
                    targets_.erase(k);
                    rekeyedOld_[k] = nowMs(); // no REQ for the dead key's tail
                    char xb[128]; _snprintf(xb, sizeof(xb) - 1,
                        "[squad] RECV EXIT old=%u,%u,%u,%u,%u (pins cleared)",
                        k.t, k.c, k.cs, k.i, k.s);
                    xb[sizeof(xb) - 1] = '\0'; coop::logLine(xb);
                    break;
                }
                rekeyPeerBody(gw, k, nk, "squad");
                break;
            }
            default: break;
        }
        char b[200]; _snprintf(b, sizeof(b) - 1,
            "[event] RECV id=%u ev=%u owner=%u hand=%u,%u,%u,%u,%u actor=%u,%u",
            ev.eventId, (unsigned)ev.event, ev.ownerId,
            ev.sType, ev.sContainer, ev.sContainerSerial, ev.sIndex, ev.sSerial,
            ev.aIndex, ev.aSerial);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

// Shared EVT_RECRUIT / EVT_SQUAD_MOVE receive half (protocols 23 + 35): the
// sender re-containered a body it owns (recruit or squad-tab move), minting
// the NEW hand it streams under from now on. RE-KEY our local copy of the old
// hand to the new stream key - the body it already has IS the subject, so
// binding it in proxyByKey_ makes the whole driven-NPC path (AI-suspend,
// damage guard, latches) inherit it with no duplicate proxy mint. If the old
// hand doesn't resolve here (runtime-born subject), the bidirectional
// describe/mint channel covers it instead.
void Replicator::rekeyPeerBody(GameWorld* gw, const Key& oldK, const Key& newK,
                               const char* tag) {
    // A hand WE authored must never enter pinPeer_ (that set vetoes
    // publishing): an echo - or both sides recruiting the SAME baked NPC,
    // which lands on the SAME new hand (run 120738) - would otherwise
    // silence our own edge's stream.
    if (pinOwned_.count(newK) || ownHands_.count(newK)) return;
    // Never ours to drive again: the author owns this hand even if a local
    // tab census would rank it into a tab we own.
    pinPeer_.insert(newK);
    // A chained edge (recruit then move, move then move) leaves the OLD key's
    // pin dead - drop it so the pin sets track only live hands.
    pinPeer_.erase(oldK);
    // Drop the old key's stream state too (run 192211: the interp TAIL of a
    // re-keyed hand kept replaying after the migration, went unresolved and
    // REQ'd a duplicate proxy). The grace stamp suppresses spawn REQs/mints
    // from any batch or reply still in flight for the dead key.
    targets_.erase(oldK);
    spawnReq_.erase(oldK);
    rekeyedOld_[oldK] = nowMs();
    Character* c = engine::resolveCharByHand(oldK.i, oldK.s, oldK.t, oldK.c, oldK.cs);
    if (!c) {
        // The old hand may itself be a MINTED proxy (the sender re-keyed a
        // runtime body we were already driving as a proxy - the mid-fight
        // ambusher case, or a squad move of an earlier runtime recruit).
        // Migrate the binding to the new key instead of orphaning the body.
        std::map<Key, Character*>::iterator pit = proxyByKey_.find(oldK);
        if (pit != proxyByKey_.end()) {
            c = pit->second;
            proxyByKey_.erase(pit);
        }
    }
    int repaired = 0, culled = -1;
    std::map<Key, Character*>::iterator ex = proxyByKey_.find(newK);
    if (ex != proxyByKey_.end()) {
        if (!c || ex->second == c) {
            // True rebound (duplicate event delivery / already migrated).
            char db[160]; _snprintf(db, sizeof(db) - 1,
                "[%s] REKEY new=%u,%u,%u,%u,%u ok=1 rebound=1",
                tag, newK.t, newK.c, newK.cs, newK.i, newK.s);
            db[sizeof(db) - 1] = '\0'; coop::logLine(db);
            return;
        }
        // A spawn REQ/mint round-trip beat the reliable edge (WAN race): a
        // duplicate proxy stands under the new hand while the REAL local
        // copy is still ours under the old key. Repair: cull the mint,
        // rebind the real body below.
        Character* mint = ex->second;
        proxyByKey_.erase(ex);
        culled = engine::removeWorldItemProxy(
                     gw, reinterpret_cast<RootObject*>(mint)) ? 1 : 0;
        repaired = 1;
    }
    if (c) {
        // Host-authority suppression may have already hidden the old copy
        // (its hand left the peer's stream the moment the edge re-containered
        // it) - bring the body back first.
        std::map<Key, Character*>::iterator sit = suppressed_.find(oldK);
        if (sit != suppressed_.end()) {
            engine::restoreNpc(gw, c);
            suppressed_.erase(sit);
        }
        proxyByKey_[newK] = c;
    }
    char rb[224]; _snprintf(rb, sizeof(rb) - 1,
        "[%s] REKEY old=%u,%u,%u,%u,%u new=%u,%u,%u,%u,%u ok=%d repaired=%d culled=%d",
        tag, oldK.t, oldK.c, oldK.cs, oldK.i, oldK.s,
        newK.t, newK.c, newK.cs, newK.i, newK.s, c ? 1 : 0, repaired, culled);
    rb[sizeof(rb) - 1] = '\0'; coop::logLine(rb);
}

void Replicator::logHardSnap(Character* c, const EntityState& out, const char* kind,
                             float gap, float srcVel, float gate, bool hadDest) {
    // Throttle to ~4 lines/s so a snap storm (the thing under investigation)
    // stays legible; skipped lines are accounted for in the next one.
    static unsigned long tick = 0;      // main-thread only
    static unsigned long skipped = 0;
    unsigned long now = nowMs();
    if (tick != 0 && (now - tick) < 250) { ++skipped; return; }
    tick = now;
    char nm[48];
    engine::charName(c, nm, sizeof(nm));
    char b[240];
    _snprintf(b, sizeof(b) - 1,
              "[snap] %s hand=%u,%u name='%s' gap=%.1f gate=%.1f srcVel=%.1f "
              "cSpeed=%.1f mult=%.2f slew=%.2f dest=%d skipped=%lu",
              kind, out.hIndex, out.hSerial, nm, gap, gate, srcVel, out.cSpeed,
              speedLastSet_, timeSlew_, hadDest ? 1 : 0, skipped);
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    skipped = 0;
}

void Replicator::applyTargets(GameWorld* gw) {
    (void)gw;
    unsigned long now = nowMs();
    // Rebuild the AI-suspend set from scratch each tick: only NPCs we drive this
    // tick stay suspended; anything we stop driving (stale/suppressed) is dropped
    // here so its AI resumes. Safe to rebuild now - the periodicUpdate detour only
    // reads the set during the engine tick, which already ran this frame.
    if (aiSuspend_) engine::clearAiSuspend();
    // Damage-guard set rebuilds the same way: every body we DRIVE this tick (all
    // are non-owned - owned hands are skipped below) is protected from local melee
    // damage, so the join's cosmetic fights cannot diverge the local-only medical
    // model. A body we stop driving drops out and takes local damage again.
    if (dmgGuard_) engine::clearDamageGuard();
    // Driven-body pointer set rebuilds per tick too: enforceHostAuthority uses it
    // to recognise a streamed body whose LOCAL hand key changed (combat detach
    // re-containers world NPCs) so it never hides a body we are driving.
    drivenChars_.clear();
    starveHeldNow_ = 0; // per-tick starved-hold census (stat line)
    for (std::map<Key, Driven>::iterator it = targets_.begin(); it != targets_.end(); ++it) {
        // Never drive a body WE own: we control + stream it locally, the peer drives
        // its copy from our stream. The disjoint partition + no local loopback means
        // our own hand shouldn't appear in targets_, but guard regardless (a stray
        // self-owned sample would otherwise fight our own control every frame).
        if (ownHands_.find(it->first) != ownHands_.end()) continue;
        Driven& d = it->second;
        EntityState out;
        if (!d.interp.sample(now, cfg_, &out)) {
            // Stream stale: stop DRIVING the body - but a stall is not an
            // authority transfer (architecture review 2026-07-10). Two guards
            // used to drop here instantly, and they starve differently:
            //   * DAMAGE guard - held for EVERY driven body for the bounded
            //     window. Locally-simulated melee mutating the local-only
            //     medical model during a WAN hiccup is the silent divergence
            //     this fix exists for.
            //   * AI suspend (the freeze) - held ONLY for squad-class bodies
            //     (a peer's player characters: engine-inert when uncontrolled,
            //     so the park is free, and a peer PC acting autonomously is
            //     the worst face of the bug). World NPCs release to local AI
            //     exactly as before: A/B 2026-07-10 showed freezing a stale
            //     interest-boundary wanderer while the host copy keeps
            //     patrolling degrades npc_sync tracking (ratio 0.64-0.73 vs
            //     the 0.8 gate; hold-off passed) - the local AI on the shared
            //     save shadows the host's patrol better than a freeze, and
            //     host-authority suppression + census already police NPC
            //     existence.
            // After the hold (or with the knob at 0) everything releases as
            // before; targets_ prunes at 30 s regardless.
            d.haveActual = false; d.parked = false; d.fresh = false;
            if (starveHoldMs_ > 0 && d.lastSeenMs != 0 &&
                (now - d.lastSeenMs) <= cfg_.staleMs + starveHoldMs_ &&
                d.interp.latest(&out, 0, 0, 0)) {
                Character* c = engine::resolve(out);
                if (!c && (spawnSync_ || recruitSync_)) {
                    std::map<Key, Character*>::iterator pit =
                        proxyByKey_.find(it->first);
                    if (pit != proxyByKey_.end()) c = pit->second;
                }
                if (c) {
                    if (dmgGuard_) engine::addDamageGuard(c);
                    if (engine::isLocalPlayerChar(gw, c)) {
                        // Squad-class: full park. drivenChars_ membership also
                        // keeps host-authority suppression off the body.
                        drivenChars_.insert(c);
                        if (aiSuspend_) engine::addAiSuspend(c);
                    }
                    // World NPCs: damage guard only - AI, suppression and
                    // census treat them exactly as the pre-hold release did.
                    ++starveHeldNow_;
                }
            }
            continue;
        }
        d.fresh = true;
        switch (d.interp.lastMode()) {
        case EntityInterp::SM_LERP:      ++interpLerp_;     break;
        case EntityInterp::SM_SINGLE:    ++interpSingle_;   break;
        case EntityInterp::SM_CLAMP_OLD: ++interpClampOld_; break;
        case EntityInterp::SM_EXTRAP:    ++interpExtrap_;   break;
        case EntityInterp::SM_SEG_SNAP:  ++interpSegSnap_;  break;
        default: break;
        }

        Character* c = engine::resolve(out);
        // Protocol 21: a streamed hand with NO local body is a host RUNTIME
        // spawn (roaming squad, dialog ambush - its hand exists only in the
        // host's session). If a proxy was already minted for it, drive THAT
        // body - this single translation point makes the proxy inherit the
        // entire world-NPC path below (AI-suspend, damage guard, combat,
        // down/death latches).
        // (Protocol 23 reuses the same translation point for RE-KEYED recruit
        // bodies, so the lookup also runs when only recruit sync is on.)
        if (!c && (spawnSync_ || recruitSync_)) {
            std::map<Key, Character*>::iterator pit = proxyByKey_.find(it->first);
            if (pit != proxyByKey_.end()) c = pit->second;
        }
        if (!c) {
            // Unresolved-hand telemetry (Phase 0 diagnostics; logged even with
            // spawnSync off - spawn_probe baselines this failure mode). Once
            // per hand: these repeat every frame while the host fights an
            // enemy the join can't see.
            if (spawnLogged_.insert(it->first).second) {
                char b[160]; _snprintf(b, sizeof(b) - 1,
                    "[spawn] unresolved hand=%u,%u,%u,%u,%u pos=%.1f,%.1f,%.1f",
                    it->first.t, it->first.c, it->first.cs, it->first.i,
                    it->first.s, out.x, out.y, out.z);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            if (spawnSync_) {
                UnresolvedHand& u = unresolvedHands_[it->first];
                u.x = out.x; u.y = out.y; u.z = out.z;
            }
            continue;
        }
        drivenChars_.insert(c);
        debugMark(c, 0, "DRV");

        // Every driven body is damage-guarded (locally-simulated hits must not
        // mutate the local-only medical model; outcomes arrive as host events).
        if (dmgGuard_) engine::addDamageGuard(c);

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
        //
        // ---- Carried carve-out (protocol 18) -----------------------------------
        // A body on someone's shoulder (streamed BODY_CARRIED, or LOCALLY attached
        // - the local pickup may lead/trail the stream by a beat) is transform-
        // owned by its local carry attach: the down override (knockDown/holdDown
        // + the 2u co-locate snap) would rip it off the shoulder and pin it to the
        // ground - the dragged/teleported-body artifact this feature fixes. Skip
        // the down path AND all locomotion driving for it this tick. koLatched is
        // deliberately NOT cleared: the body is still KO'd, and the hold re-engages
        // the tick after the local drop releases it.
        if (carrySync_) {
            engine::CarryRead lcr;
            bool locallyCarried = engine::readCarry(c, &lcr) && lcr.beingCarried;
            if (coop::bodyIsCarried(out.bodyState) || locallyCarried) {
                d.parked = false; d.haveDest = false;
                if (haveActual) { d.haveActual = true; d.lx = ax; d.ly = ay; d.lz = az; }
                continue;
            }
        }
        // ---- Furniture carve-out + self-heal (protocol 19) ----------------------
        // A body in a bed/cage (streamed BODY_IN_BED/BODY_IN_CAGE, or LOCALLY
        // occupying - the local placement may lead/trail the stream by a beat) is
        // transform-owned by its furniture attach: the down override and any
        // locomotion driving would rip it out onto the floor. Skip both.
        // Scoped AWAY from conscious bed poses (USE_BED / USE_BED_ORDER /
        // SLEEP_ON_FLOOR): those ride the validated L3 fixture-pose path
        // (bed_pose) - a sleeper streams the bed TASK, walks to the bed and
        // climbs in engine-natively; occupancy owns the task-less (unconscious
        // placement) case. The reliable enter/exit edges do the work; this
        // repairs the losses:
        //   * bit streamed but not locally occupied -> throttled enter into the
        //     nearest matching fixture at the streamed position (the continuous
        //     bit carries no furniture hand),
        //   * locally occupied after the stream stopped reporting the bit ->
        //     debounced local exit (a 1-batch blip must not eject a valid
        //     occupant - the carry-drop lesson).
        if (furnSync_ && !engine::taskIsBedPose((int)out.task)) {
            int streamKind = (out.bodyState & BODY_IN_BED) ? 1
                           : ((out.bodyState & BODY_IN_CAGE) ? 2 : 0);
            engine::FurnitureRead lfr;
            bool haveFr = engine::readFurniture(c, &lfr);
            int localKind = (haveFr && lfr.valid) ? lfr.kind : 0;
            if (streamKind != 0) {
                d.furnNoSeeTick = 0;
                if (haveFr && localKind != streamKind &&
                    (now - d.furnHealTick) >= FURN_HEAL_MS) {
                    d.furnHealTick = now;
                    bool ok = engine::enterFurnitureNearPos(
                        gw, c, streamKind, out.x, out.y, out.z, FURN_MATCH_DIST);
                    char b[160]; _snprintf(b, sizeof(b) - 1,
                        "[furn] HEAL ENTER occ=%u,%u kind=%d ok=%d",
                        out.hIndex, out.hSerial, streamKind, ok ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
                d.parked = false; d.haveDest = false;
                if (haveActual) { d.haveActual = true; d.lx = ax; d.ly = ay; d.lz = az; }
                continue;
            } else if (localKind != 0) {
                // Third-party placement authority (protocol 36): a HOST-sim
                // actor (a guard jailing an arrested player) put this PEER-
                // OWNED squad body into furniture. The occupant's owner never
                // sees the action, so the occupant-owner ENTER can't fire -
                // the owner's stream keeps reporting no bit and the debounced
                // HEAL EXIT below ejected the body every 3 s ("the host kept
                // taking it out of the cage", 2026-07-09). The host is the
                // world authority for NPC actions: author the ENTER for the
                // owner (buffered; publishOwned sends), HOLD the self-heal
                // exit while it crosses, and re-author every FURN_PEER_MS
                // until the owner's stream carries the bit. KO'd/down bodies
                // only - a conscious voluntary use stays owner-authored.
                bool downish = coop::bodyIsDown(out.bodyState) || d.koLatched ||
                               d.deathLatched ||
                               coop::bodyIsDown(engine::readBodyState(c));
                if (streamNpcs_ && isSquad && downish) {
                    if (d.furnPeerTick == 0 || (now - d.furnPeerTick) >= FURN_PEER_MS) {
                        d.furnPeerTick = now;
                        PendFurnEnter pe;
                        pe.occ = keyOf(out);
                        for (int fi = 0; fi < 5; ++fi) pe.furn[fi] = lfr.furn[fi];
                        pe.kind = localKind;
                        furnPeerPend_.push_back(pe);
                        char b[160]; _snprintf(b, sizeof(b) - 1,
                            "[furn] PEER-ENTER author occ=%u,%u furn=%u,%u kind=%d",
                            out.hIndex, out.hSerial, lfr.furn[3], lfr.furn[4],
                            localKind);
                        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    }
                    d.furnNoSeeTick = 0; // never self-heal-eject a host placement
                    d.parked = false; d.haveDest = false;
                    if (haveActual) { d.haveActual = true; d.lx = ax; d.ly = ay; d.lz = az; }
                    continue;
                }
                if (d.furnNoSeeTick == 0) {
                    d.furnNoSeeTick = now;
                } else if ((now - d.furnNoSeeTick) > FURN_EXIT_MS) {
                    d.furnNoSeeTick = 0;
                    bool ok = engine::applyFurniture(gw, c, lfr.furn, localKind, false);
                    char b[160]; _snprintf(b, sizeof(b) - 1,
                        "[furn] HEAL EXIT occ=%u,%u kind=%d ok=%d",
                        out.hIndex, out.hSerial, localKind, ok ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
                // Still locally attached this tick: hold off all driving until
                // the debounced exit (or a fresh stream bit) resolves it.
                d.parked = false; d.haveDest = false;
                if (haveActual) { d.haveActual = true; d.lx = ax; d.ly = ay; d.lz = az; }
                continue;
            } else {
                d.furnNoSeeTick = 0;
            }
        }
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

        // ---- Stealth posture (protocol 20) -------------------------------------
        // Continuous mode apply: the streamed BODY_SNEAK bit IS Character::
        // stealthMode on the owner, so a difference on the local copy just
        // re-runs the engine's own setStealthMode (sneak-walk + stealthUpdate
        // scanning, all native). Reached only by an upright, un-carried,
        // un-occupied body (the branches above continue out), so a KO'd or
        // bedridden copy is never stealth-toggled. Throttled: a copy whose
        // engine keeps clearing the mode (combat) re-applies at 1 Hz, not per
        // frame.
        if (stealthSync_) {
            bool want  = coop::bodySneaking(out.bodyState);
            int  local = engine::readStealthMode(c);
            if (local >= 0 && ((local != 0) != want) &&
                (d.sneakTick == 0 || (now - d.sneakTick) >= SNEAK_APPLY_MS)) {
                d.sneakTick = now;
                bool ok = engine::applyStealth(c, want);
                char b[128]; _snprintf(b, sizeof(b) - 1,
                    "[sneak] APPLY hand=%u,%u on=%d ok=%d",
                    out.hIndex, out.hSerial, want ? 1 : 0, ok ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        // ---- Stage 3c: combat override (melee) --------------------------------
        // The host streams a combat INTENT (task == TASK_COMBAT_MELEE for an ACTIVE
        // attacker, TASK_COMBAT_WAIT for one queued by the AttackSlotManager; subject
        // = the attack target's hand). Reproduce the cause: order the local copy to
        // melee the same resolved target and let the join's own engine run the fight
        // (draw, swing, footwork) - the proven "replicate the intent" path.
        // A WAITING combatant is a STANCE, not a failed attack: its copy holds the
        // goal and menaces in the ring, and is never re-issued on a timer (each
        // re-issue clearGoals-resets the local AI - THE teleporting-crowd artifact).
        // Re-issues happen only on a new episode, a target change, or an ACTIVE copy
        // that disengaged, with exponential backoff. Positional drift is corrected in
        // graded bands (leave / walk-converge / logged teleport). Combatants skip the
        // AI-suspend path below (their AI must run to animate), reached only via this
        // early `continue`.
        if (coop::taskIsCombat(out.task)) {
            bool hostWaiting = coop::taskIsCombatWait(out.task);
            d.combatSeenTick = now; // feeds the disarm debounce below
            // Detach only WORLD NPCs from their town AI. A driven SQUAD member must
            // NEVER be detached: separateIntoMyOwnSquad changes its hand CONTAINER,
            // which breaks the cross-client identity (the standers lesson, doctrine
            // asymmetry rule) - found when player_combat first drove a peer-owned
            // player character into a fight.
            if (!isSquad && !d.detached) d.detached = engine::detachFromTownAI(c);
            engine::CombatRead lc;
            // modeActive is the STABLE engaged read; isInCombatMode flickers off
            // between combo sections and slot rotations (the crowd lesson).
            bool localFighting = engine::readCombat(c, &lc) &&
                                 (lc.inCombat || lc.modeActive);
            // The copy is engaged with the WRONG body (the local brawl grabbed it).
            bool wrongLocalTgt = localFighting && lc.hasTarget &&
                (lc.target[3] != out.sIndex || lc.target[4] != out.sSerial);
            // The host retargeted since our last order.
            bool tgtChanged = d.combatArmed &&
                (d.combatTgtIdx != out.sIndex || d.combatTgtSer != out.sSerial);
            // Backoff: 1.5 s base, doubling per re-issue in this episode, 6 s cap -
            // a copy that legitimately cannot engage is not AI-reset forever.
            unsigned long interval = COMBAT_REISSUE_MS;
            if (d.combatOrders > 1) {
                unsigned int shift = d.combatOrders - 1;
                if (shift > 2) shift = 2;
                interval = COMBAT_REISSUE_MS << shift;
                if (interval > COMBAT_REISSUE_MAX_MS) interval = COMBAT_REISSUE_MAX_MS;
            }
            bool reissue = false;
            if (!d.combatArmed) {
                reissue = true;
                d.combatOrders = 0; // new episode: backoff restarts
            } else if (tgtChanged && (now - d.combatTick) >= COMBAT_REISSUE_MS) {
                reissue = true;     // retarget promptly (base throttle, no backoff)
            } else if ((wrongLocalTgt || (!hostWaiting && !localFighting)) &&
                       (now - d.combatTick) >= interval &&
                       d.combatOrders <= COMBAT_REISSUE_CAP) {
                // Active copy disengaged / fighting the wrong body - and the
                // WAIT -> MELEE promotion case (the slot rotates every few
                // seconds, so promotions recur: backoff applies, and after
                // COMBAT_REISSUE_CAP failed attempts the copy is left to the
                // position bands - a template that won't fight here (fear,
                // blocked ring spot) must not be clearGoals-reset all fight;
                // that WAS the artifact. The backoff counter deliberately
                // never resets mid-episode: local engagement flickers (combo
                // gaps), and a flicker-reset defeated the backoff (measured:
                // 30 orders/hand, every one at the base interval).
                reissue = true;
            }
            if (reissue) {
                // A seat-INJECTED copy (applyRest committed a player order at the
                // stool) ignores the goal-path attack: player orders outrank AI
                // goals, so the body stays seated and the fight never starts (run
                // 014713: the pre-seated striker re-ordered 15x, localFight=0 all
                // window). Flush the order via the order-path attack, once.
                bool breakSeat = d.taskApplied || d.issuedTask != TASK_NONE;
                int r = engine::applyCombat(c, out, breakSeat);
                if (breakSeat && r == 2) {
                    d.taskApplied = false; d.taskBad = false;
                    d.issuedTask  = TASK_NONE;
                }
                d.combatArmed = true; d.combatTick = now;
                if (d.combatOrders < 1000000u) ++d.combatOrders;
                d.combatTgtIdx = out.sIndex; d.combatTgtSer = out.sSerial;
                { char b[192]; _snprintf(b, sizeof(b) - 1,
                    "[combat] order hand=%u,%u tgt=%u,%u localFight=%d r=%d wait=%d n=%u%s",
                    out.hIndex, out.hSerial, out.sIndex, out.sSerial,
                    localFighting ? 1 : 0, r, hostWaiting ? 1 : 0, d.combatOrders,
                    breakSeat ? " seatbrk=1" : "");
                  b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            }
            // Graded position correction (don't kill the gait): under the soft band
            // the fight owns the footwork; drifted past it, a WAITING copy converges
            // with a real walk (stance preserved, no AI reset); far gone, teleport
            // and say so - but on a COOLDOWN: a snap that cannot stick (mid-stagger,
            // stale interp while the host body sprints) must not re-fire every frame
            // (measured: one hand snapped ~50x/s at constant drift).
            // The walk band never runs while the copy is still ARMING (an active
            // stance with re-issues left): walkTo is the player-move/HIGH_PRIORITY-
            // destination path and it STOMPS a pending attack goal, so walk-driving
            // there keeps the copy from ever engaging (player_combat: the striker
            // must arm and land real blood on the victim's owner). Once armed - or
            // once the arming budget is spent (a copy that won't fight here) - the
            // walk band is what keeps a non-engaging body tracking the host's
            // roaming brawl (combat_crowd: without it, medians hit 50+ u).
            if (haveActual) {
                bool arming = !hostWaiting && !localFighting &&
                              d.combatOrders <= COMBAT_REISSUE_CAP;
                float drift = dist3(ax, ay, az, out.x, out.y, out.z);
                if (drift > COMBAT_SNAP_DIST &&
                    (now - d.combatSnapTick) >= COMBAT_SNAP_COOL_MS) {
                    engine::applyRaw(c, out);
                    d.combatSnapTick = now;
                    { char b[144]; _snprintf(b, sizeof(b) - 1,
                        "[combat] snap hand=%u,%u drift=%.1f wait=%d",
                        out.hIndex, out.hSerial, drift, hostWaiting ? 1 : 0);
                      b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                } else if (drift > COMBAT_SOFT_DIST && !localFighting && !arming) {
                    engine::walkTo(c, out.x, out.y, out.z, 0.0f);
                }
            }
            d.parked = false; d.haveDest = false;
            if (haveActual) { d.haveActual = true; d.lx = ax; d.ly = ay; d.lz = az; }
            continue;
        }
        // Host no longer reports combat for this body. The stance rides the LOSSY
        // entity batch and the engine's own combat read blips off mid-fight, so a
        // short gap is NOISE: hold the fight (skip the rest-drive entirely) and
        // only disarm - clearGoals + fall back to locomotion/rest - after a
        // sustained combat-free window. Pre-debounce, every blip disarmed the copy
        // (clearGoals), re-armed it next batch (another order), and the AI reset
        // wandered it until the snap teleported it - the crowd artifact's second
        // driver, alongside the waiting-stance re-issue loop.
        if (d.combatArmed) {
            if ((now - d.combatSeenTick) < COMBAT_DISARM_MS) {
                if (haveActual) { d.haveActual = true; d.lx = ax; d.ly = ay; d.lz = az; }
                continue;
            }
            d.combatArmed = false;
            d.combatOrders = 0;
            d.combatTgtIdx = 0; d.combatTgtSer = 0;
            engine::clearGoals(c); // drop the stale attack goal before re-parking
        }

        // ---- Carried-body sync (protocol 18): carrier self-heal ----------------
        // The reliable pickup/drop edges do the work; this repairs the losses.
        // ANY driven carrier (squad member or host-streamed world NPC) streaming
        // TASK_CARRY_BODY whose local copy is not carrying that body gets a
        // throttled local pickup (a lost/failed pickup event, or the carried
        // body resolved late). A local copy still carrying after the stream
        // stopped reporting the carry (debounced - stance samples ride the lossy
        // batch) gets a local drop. A SQUAD carrier then falls through to the
        // ordinary locomotion drive: it walks like any squad member; the carried
        // body follows its local attach. An NPC carrier with an ACTIVE local
        // carry instead ends its tick here (early continue): the kinematic
        // walk-drive/park/rest/trust paths below applyRaw-teleport and pose-
        // inject, which rips the shoulder attach apart - its local AI keeps
        // running (never reaches the AI-suspend add) so the carry walk animates,
        // and a graded position band below keeps it tracking the host's path.
        if (carrySync_) {
            if (coop::taskIsCarry(out.task)) {
                d.carryNoSeeTick = 0;
                engine::CarryRead lcr;
                bool haveCr = engine::readCarry(c, &lcr);
                bool carryingRight = haveCr && lcr.carrying &&
                                     lcr.carried[3] == out.sIndex &&
                                     lcr.carried[4] == out.sSerial;
                if (haveCr && !carryingRight &&
                    (now - d.carryHealTick) >= CARRY_HEAL_MS) {
                    d.carryHealTick = now;
                    unsigned int ch[5] = { out.sType, out.sContainer,
                                           out.sContainerSerial,
                                           out.sIndex, out.sSerial };
                    bool ok = engine::applyPickup(gw, c, ch);
                    char b[160]; _snprintf(b, sizeof(b) - 1,
                        "[carry] HEAL PICKUP carrier=%u,%u carried=%u,%u ok=%d",
                        out.hIndex, out.hSerial, out.sIndex, out.sSerial,
                        ok ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    // Refresh the read: a pickup that just landed must take the
                    // NPC early-continue below THIS tick, not after one more
                    // pass through the kinematic drive (which would rip it off).
                    if (ok) haveCr = engine::readCarry(c, &lcr);
                }
                if (!isSquad && haveCr && lcr.carrying) {
                    // NPC carrier with a live local attach: position band only.
                    // Under the soft band the local carry walk owns the feet;
                    // drifted past it, converge with a real walk (walkTo keeps
                    // the carried body on the shoulder - move orders don't
                    // release a carry); far gone, snap once on a cooldown.
                    if (haveActual) {
                        float drift = dist3(ax, ay, az, out.x, out.y, out.z);
                        if (drift > COMBAT_SNAP_DIST &&
                            (now - d.combatSnapTick) >= COMBAT_SNAP_COOL_MS) {
                            engine::applyRaw(c, out);
                            d.combatSnapTick = now;
                            { char b[128]; _snprintf(b, sizeof(b) - 1,
                                "[carry] npc snap hand=%u,%u drift=%.1f",
                                out.hIndex, out.hSerial, drift);
                              b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                        } else if (drift > COMBAT_SOFT_DIST) {
                            engine::walkTo(c, out.x, out.y, out.z, 0.0f);
                        }
                    }
                    d.parked = false; d.haveDest = false;
                    if (haveActual) { d.haveActual = true; d.lx = ax; d.ly = ay; d.lz = az; }
                    continue;
                }
            } else {
                engine::CarryRead lcr;
                if (engine::readCarry(c, &lcr) && lcr.carrying) {
                    if (d.carryNoSeeTick == 0) {
                        d.carryNoSeeTick = now;
                    } else if ((now - d.carryNoSeeTick) > CARRY_DROP_MS) {
                        d.carryNoSeeTick = 0;
                        bool ok = engine::applyDrop(c, true);
                        char b[144]; _snprintf(b, sizeof(b) - 1,
                            "[carry] HEAL DROP carrier=%u,%u ok=%d",
                            out.hIndex, out.hSerial, ok ? 1 : 0);
                        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    }
                } else {
                    d.carryNoSeeTick = 0;
                }
            }
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

            // ---- Step 4: divergence-gated authority (doctrine 18, flagged) -----
            // A world NPC whose LOCAL AI has agreed with the host's raw task for a
            // sustained streak while staying in position is TRUSTED: its own AI is
            // provably doing what the host's is, so we stop suspending + driving
            // it (fewer fights with the AI, graceful under latency). Divergence or
            // drift revokes trust instantly and the normal drive re-engages this
            // same tick. Bodies in host-reported combat/down never reach here
            // (their branches continue earlier), so trust only governs the
            // locomotion/rest regime.
            if (gateAuthority_) {
                float drift = haveActual ? dist3(ax, ay, az, out.x, out.y, out.z) : 1e9f;
                bool inPos = (drift <= TRUST_DRIFT_MAX);
                if (agree && inPos) {
                    if (d.agreeStreak < 1000000u) ++d.agreeStreak;
                } else {
                    d.agreeStreak = 0;
                }
                if (d.trusted) {
                    if (agree && inPos) {
                        // Stay trusted: no suspend (set not re-added), no drive.
                        if (haveActual) { d.haveActual = true; d.lx = ax; d.ly = ay; d.lz = az; }
                        continue;
                    }
                    d.trusted = false;
                    ++trustRevokes_;
                    d.parked = false; d.haveDest = false;
                    d.taskApplied = false; d.taskBad = false; d.goalsCleared = false;
                    { char b[128]; _snprintf(b, sizeof(b) - 1,
                        "[trust] revoke hand=%u,%u reason=%s drift=%.1f",
                        out.hIndex, out.hSerial, agree ? "drift" : "task", drift);
                      b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                    // fall through: the normal drive re-engages below this tick
                } else if (d.agreeStreak >= TRUST_STREAK_FRAMES) {
                    d.trusted = true;
                    ++trustGrants_;
                    // Hand the body back to its own AI cleanly.
                    d.parked = false; d.haveDest = false;
                    d.taskApplied = false; d.taskBad = false; d.goalsCleared = false;
                    { char b[112]; _snprintf(b, sizeof(b) - 1,
                        "[trust] grant hand=%u,%u streak=%u",
                        out.hIndex, out.hSerial, d.agreeStreak);
                      b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                    if (haveActual) { d.haveActual = true; d.lx = ax; d.ly = ay; d.lz = az; }
                    continue;
                }
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
        //
        // Walk/rest DEBOUNCE (2026-07-11 choppiness fix): the instantaneous
        // 2-sample velocity dips below NPC_MOVE_VEL at every sample-pair
        // boundary of a walking source, so the raw classifier FLAPPED
        // walk->rest->walk several times a second - each rest entry parks/
        // halts the body, each walk re-entry restarts the path = the observed
        // stutter. Hold the walking verdict for a fixed TIME after the last
        // genuinely-moving sample instead: a walking source's dips are always
        // shorter than the hold, a genuine stop enters rest ~1 s later.
        // (A velPeak-based debounce was tried first and reverted same-day:
        // the peak is magnitude-sensitive, so one teleport-artifact velocity
        // spike in the stream - srcVel 90-150 u/s on a seg snap - held a
        // genuinely SEATED divergent NPC in the walk branch for ~7 s per
        // spike, hard-snapping every frame; spawn_far run 124346.)
        const unsigned long NPC_MOVE_HOLD_MS = 1000;
        float vlen = std::sqrt(vx * vx + vy * vy + vz * vz);
        if (vlen > d.velPeak) d.velPeak = vlen;
        else                  d.velPeak *= 0.99f; // ~1 s half-life at 75 fps
        if (haveNewest && vlen > NPC_MOVE_VEL) d.moveSeenMs = now;
        bool npcMoving = haveNewest && d.moveSeenMs != 0 &&
                         (now - d.moveSeenMs) <= NPC_MOVE_HOLD_MS;
        if (!isSquad) {
            if (d.wasMoving && !npcMoving) ++restFlipNpc_;
            d.wasMoving = npcMoving;
        }

        // Velocity-aware hard-snap gate (2026-07-11 rubber-banding fix). The
        // walk-drive's natural trailing distance behind 'newest' scales with the
        // source's WALL-CLOCK speed (render delay + batch cadence are time, not
        // distance): a sprinter at ~50 u/s trails ~8-9 u in steady state, which
        // sat exactly on the old fixed 8 u gate ([snap] measured gap=8.6 with
        // srcVel~50 repeatedly), and any game-speed multiplier scales measured
        // velocity the same way (5x turned the gate into a per-sample teleport).
        // Gate on TIME behind the source instead - snap only when the body
        // trails by more than snapSeconds_ of travel. Two hardenings from the
        // fast_march validation run:
        //   * the velocity estimate is a slow-decaying PEAK (~1 s half-life),
        //     not the instantaneous sample - a source stopping at a leg end
        //     deflated the gate to the floor while the body was still tens of
        //     units out, turning every stop into a teleport;
        //   * the distance floor scales with the consensus game speed - at 5x
        //     every trailing distance is 5x in world units for the same time
        //     lag, and burst onsets outrun the engine's real max locomotion
        //     speed for a moment regardless of the commanded catch-up.
        // (velPeak itself is updated above, where the walk/rest debounce
        // shares it.)
        float multEff = (speedLastSet_ > 1.0f) ? speedLastSet_ : 1.0f;
        float snapGate = snapDist_ * multEff;
        if (d.velPeak * snapSeconds_ > snapGate) snapGate = d.velPeak * snapSeconds_;

        // AI-suspend probe: for a host-driven world NPC, suspend its AI decision
        // layer (faction-safe) so it stops self-tasking but keeps animating. The
        // host stream is the sole task authority; the body holds + animates its
        // current/injected action instead of the AI re-deciding every tick.
        // (Releasing node-anchored sitters to local AI was tried - Idea I4 - and
        // regressed: the freed AI wandered them off-host, CROSSCHECK 0.5, and it
        // still did not reliably sit them. So we suspend uniformly.)
        if (aiSuspend_ && !isSquad) engine::addAiSuspend(c);

        // Re-arm rest-pose reproduction whenever the body is genuinely moving, so
        // the next time it stops we re-evaluate the host's (possibly new) task.
        bool genuinelyMoving = isSquad ? hostMoving : npcMoving;
        if (genuinelyMoving) {
            // Seat-break (2026-07-11): a rest pose applyRest committed is a
            // PLAYER-RANK order, and a seated body both ignores goal-level
            // movement and re-places itself at the fixture - applyRaw
            // teleports NO-OP on it (spawn_far run 124346: 'Bar Thug' hard-
            // snapped every frame at constant gap=343 while locally still
            // task=87). When the host copy starts moving, flush the order by
            // issuing the walk through the player move-order path once.
            if (!isSquad && haveNewest &&
                (d.taskApplied || d.issuedTask != TASK_NONE)) {
                float spd = (out.cSpeed > 1.0f) ? out.cSpeed : 12.0f;
                engine::walkTo(c, newest.x, newest.y, newest.z, spd);
                d.haveDest = true; d.dx = newest.x; d.dy = newest.y; d.dz = newest.z;
                char b[112]; _snprintf(b, sizeof(b) - 1,
                    "[interp] seat-break hand=%u,%u task=%u",
                    out.hIndex, out.hSerial, (unsigned)d.issuedTask);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            d.taskApplied = false; d.taskBad = false; d.issuedTask = TASK_NONE;
            d.goalsCleared = false; // next rest episode gets one fresh goal-clear
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
            if (npcMoving && haveActual && gapNewest > snapGate) {
                engine::applyRaw(c, newest);
                ++hardSnapNpc_;
                logHardSnap(c, out, "npc", gapNewest, vlen, snapGate, d.haveDest);
                d.parked = false; d.haveDest = false;
            } else if (npcMoving) {
                float tx = newest.x, ty = newest.y, tz = newest.z;
                // Lead only while the instantaneous velocity is meaningful:
                // the debounced classifier keeps npcMoving true through
                // mid-walk velocity dips (vlen ~ 0), where a lead projection
                // would divide by zero - aim at the newest position instead.
                if (vlen > 0.01f) {
                    float lead = vlen * LEAD_SECONDS;
                    tx += vx / vlen * lead; ty += vy / vlen * lead; tz += vz / vlen * lead;
                }
                float moved = d.haveDest ? dist3(tx, ty, tz, d.dx, d.dy, d.dz)
                                         : (REISSUE_DIST + 1.0f);
                if (moved > REISSUE_DIST) {
                    float spd = out.cSpeed + gapNewest * catchupK_;
                    float base = (out.cSpeed > 1.0f) ? out.cSpeed : 12.0f;
                    float cap = base * 2.5f;
                    if (spd > cap) spd = cap;
                    engine::walkTo(c, tx, ty, tz, spd);
                    ++walkReissueNpc_;
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
                applyRest(c, d, out, haveActual, ax, ay, az, now, /*isSquad*/false);
                d.haveDest = false;
            }
        } else if (hostMoving && haveActual && haveNewest && gapNewest > snapGate) {
            // Fell behind / source warped: hard-snap to the true position (no-halt
            // teleport keeps the clip phase advancing rather than freezing).
            engine::applyRaw(c, newest);
            ++hardSnapSquad_;
            logHardSnap(c, out, "squad", gapNewest, vlen, snapGate, d.haveDest);
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
                spd += gapNewest * catchupK_;
                float base = (out.cSpeed > 1.0f) ? out.cSpeed : 12.0f;
                float cap = base * 2.5f;
                if (spd > cap) spd = cap;
                engine::walkTo(c, tx, ty, tz, spd);
                ++walkReissueSquad_;
                d.haveDest = true; d.dx = tx; d.dy = ty; d.dz = tz;
            }
            d.parked = false;
            // No motion mirror while genuinely moving: the engine selects the
            // grounded walk clip itself from the locomotion it is performing.
        } else {
            // Squad member at rest: reproduce the host's pose (e.g. seated on the
            // same chair) at the same fixture, else quiet + park (Stage 5). This is
            // what makes a join squad-mate sit instead of standing on the chair.
            applyRest(c, d, out, haveActual, ax, ay, az, now, /*isSquad*/true);
            d.haveDest = false;
        }

        // ---- Oracles (measured from the body's ACTUAL rendered motion) --------
        // "Active" == the host is genuinely translating, matching the drive's own
        // walk-vs-rest decision (velocity-gated for NPCs, flag-based for the squad
        // leader as validated in Stage 3), so a correctly-parked body at a host
        // fidget is not scored as a smoothness miss.
        // NPCs are judged by the INSTANTANEOUS stream velocity, not the
        // debounced npcMoving: the 1 s walk-hold keeps the drive in the walk
        // branch through source stops, and scoring that trailing second as
        // "active" charges ~75 legitimate at-rest frames per stop to
        // zeroFrac (leader_move zeroFrac 0.66-0.77 vs the 0.3 baseline).
        bool oracleActive = isSquad ? hostMoving
                                    : (haveNewest && vlen > NPC_MOVE_VEL);
        if (oracleActive && haveActual && d.haveActual) {
            float step = dist3(ax, ay, az, d.lx, d.ly, d.lz);
            // Smoothness is only scored at steady sim speed. During the
            // session-start clock catch-up the join sims at up to 2x
            // (timeSlew_, protocol 25) while the host streams positions at
            // 1x wall-clock - about twice the render frames per streamed
            // step, a structural zero-step source that measured the SLEW,
            // not the interp pipeline (zeroFrac flaked 0.2-0.9 run-to-run
            // with the transient inside the window; user-confirmed "join
            // NPCs animate faster" 2026-07-10). Skipped frames are counted
            // so the summary shows how much of the run was excluded.
            if (timeSlew_ > 0.99f && timeSlew_ < 1.01f) {
                ++activeFrames_;
                if (step < 0.01f) ++zeroWhileActive_;
                if (step > maxStep_) maxStep_ = step;
            } else {
                ++slewSkipFrames_;
            }

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
    if (aiSuspend_ && (now - aiLogTick_) > 3000) {
        aiLogTick_ = now;
        char b[96];
        _snprintf(b, sizeof(b), "[ai] suspended=%u driven=%u",
                  engine::aiSuspendCount(), (unsigned)targets_.size());
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    // Interp/drive stat line (~5 s, protocol 36 jumpiness instrumentation).
    // Cumulative counters, so two lines diff into a rate; delay/jit report the
    // WORST live buffer (the adaptive render delay + its jitter estimate) -
    // a delay pinned at maxDelayMs with high jitter means the buffer can no
    // longer absorb the path's jitter and starvation (extrap/clamp) follows.
    if (!targets_.empty() && (now - interpLogTick_) > 5000) {
        interpLogTick_ = now;
        unsigned long maxDelay = 0; float maxJit = 0.0f;
        for (std::map<Key, Driven>::iterator it = targets_.begin();
             it != targets_.end(); ++it) {
            if (!it->second.fresh) continue;
            if (it->second.interp.lastDelayMs() > maxDelay)
                maxDelay = it->second.interp.lastDelayMs();
            if (it->second.interp.jitter() > maxJit)
                maxJit = it->second.interp.jitter();
        }
        char b[256];
        _snprintf(b, sizeof(b) - 1,
            "[interp] lerp=%lu extrap=%lu clamp=%lu seg=%lu single=%lu "
            "snapSq=%lu snapNpc=%lu reissueSq=%lu reissueNpc=%lu restFlip=%lu "
            "delay=%lu jit=%.1f starve=%u",
            interpLerp_, interpExtrap_, interpClampOld_, interpSegSnap_,
            interpSingle_, hardSnapSquad_, hardSnapNpc_,
            walkReissueSquad_, walkReissueNpc_, restFlipNpc_, maxDelay, maxJit,
            starveHeldNow_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    if (gateAuthority_ && (now - trustLogTick_) > 3000) {
        trustLogTick_ = now;
        unsigned int trusted = 0;
        for (std::map<Key, Driven>::iterator it = targets_.begin(); it != targets_.end(); ++it)
            if (it->second.trusted) ++trusted;
        char b[112];
        _snprintf(b, sizeof(b), "[trust] trusted=%u driven=%u grants=%lu revokes=%lu",
                  trusted, (unsigned)targets_.size() - trusted, trustGrants_, trustRevokes_);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
    }

    // Step 6: age out long-stale entries so a session's worth of interest-boundary
    // passers-by doesn't accumulate forever. Reliable-event latches are PRESERVED
    // (a dead body must stay dead even while unstreamed); everything else is
    // reconstructed from the stream if the entity ever returns.
    const unsigned long TARGET_STALE_MS = 30000;
    for (std::map<Key, Driven>::iterator it = targets_.begin(); it != targets_.end(); ) {
        Driven& d = it->second;
        bool stale   = (d.lastSeenMs == 0) || (now - d.lastSeenMs > TARGET_STALE_MS);
        bool latched = d.deathLatched || d.koLatched;
        if (stale && !latched) targets_.erase(it++);
        else ++it;
    }
    // The authority hysteresis counters are pruned in enforceHostAuthority (by
    // what its local-NPC enumeration actually saw), NOT here by targets_
    // membership: a join-local NPC the host NEVER streamed is never in targets_,
    // and erasing its counter every tick reset the unstreamed streak to 1 forever,
    // so the suppress threshold (75 frames) was unreachable - the "phantom walker
    // on the join that never gets hidden" bug.
}

void Replicator::sweepCarries(GameWorld* gw) {
    if (!carrySync_ && !furnSync_) return;
    // The departed peer's stream will never author its drop/exit edges, so any
    // driven (non-owned) copy still carrying gets a local ragdoll drop here
    // (the carried body then returns to the ordinary KO/down channels), and
    // any driven copy still occupying furniture (protocol 19) gets a local
    // release the same way.
    for (std::map<Key, Driven>::iterator it = targets_.begin();
         it != targets_.end(); ++it) {
        const Key& k = it->first;
        if (ownHands_.find(k) != ownHands_.end()) continue;
        Character* c = engine::resolveCharByHand(k.i, k.s, k.t, k.c, k.cs);
        if (!c) continue;
        if (carrySync_) {
            engine::CarryRead cr;
            if (engine::readCarry(c, &cr) && cr.carrying) {
                bool ok = engine::applyDrop(c, true);
                char b[128]; _snprintf(b, sizeof(b) - 1,
                    "[carry] SWEEP DROP carrier=%u,%u ok=%d", k.i, k.s, ok ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            it->second.carryNoSeeTick = 0;
        }
        if (furnSync_) {
            engine::FurnitureRead fr;
            if (engine::readFurniture(c, &fr) && fr.valid && fr.kind != 0) {
                bool ok = engine::applyFurniture(gw, c, fr.furn, fr.kind, false);
                char b[128]; _snprintf(b, sizeof(b) - 1,
                    "[furn] SWEEP EXIT occ=%u,%u kind=%d ok=%d",
                    k.i, k.s, fr.kind, ok ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            it->second.furnNoSeeTick = 0;
        }
    }
}

void Replicator::publishNpcCensus(GameWorld* gw, NetLink& net, u32 ownerId) {
    // Host-only existence broadcast (protocol 36): hands of every world NPC
    // within the census radius, 1 Hz. Position streaming stays at the ~200 u
    // bubble; this only answers "does this NPC exist on the host" so the join
    // can cull local-only ghosts at render range.
    if (!gw || !streamNpcs_ || censusRadius_ <= 0.0f) return;
    unsigned long now = nowMs();
    if (censusSendMs_ != 0 && (now - censusSendMs_) < 1000) return;
    censusSendMs_ = now;
    static Character*  chars[NPC_CENSUS_MAX];  // main-thread only
    static EntityState states[NPC_CENSUS_MAX];
    // Publish 25% WIDER than the join culls against: an unstreamed far NPC is
    // locally simulated on BOTH sides, so its two positions legitimately
    // diverge - without the margin a real NPC wandering near the boundary
    // (inside the join's scan, outside the host's) would be false-culled.
    unsigned int n = engine::listNpcsWide(gw, censusRadius_ * 1.25f, chars, states,
                                          NPC_CENSUS_MAX);
    static u32   hands[NPC_CENSUS_MAX * 5];
    static float poss[NPC_CENSUS_MAX * 3];
    for (unsigned int i = 0; i < n; ++i) {
        hands[i * 5 + 0] = states[i].hType;
        hands[i * 5 + 1] = states[i].hContainer;
        hands[i * 5 + 2] = states[i].hContainerSerial;
        hands[i * 5 + 3] = states[i].hIndex;
        hands[i * 5 + 4] = states[i].hSerial;
        poss[i * 3 + 0]  = states[i].x;
        poss[i * 3 + 1]  = states[i].y;
        poss[i * 3 + 2]  = states[i].z;
    }
    net.queueNpcCensus(ownerId, hands, poss, n);
    // ~10 s cadence log so free-play sessions show the census breathing
    // without 1 Hz spam.
    static unsigned long logTick = 0;
    if ((now - logTick) > 10000) {
        logTick = now;
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[census] sent n=%u radius=%.0f", n, censusRadius_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        // KENSHICOOP_DEBUG_CENSUS=1: dump every census row (hand + name) at the
        // same 10 s cadence, so a join-side cull can be classified against the
        // host's actual membership (true ghost vs host enumeration miss).
        static int dump = -1;
        if (dump < 0) {
            const char* e = getenv("KENSHICOOP_DEBUG_CENSUS");
            dump = (e && e[0] == '1') ? 1 : 0;
        }
        if (dump == 1) {
            for (unsigned int i = 0; i < n; ++i) {
                char nm[48];
                engine::charName(chars[i], nm, sizeof(nm));
                char r[160];
                _snprintf(r, sizeof(r) - 1,
                          "[census] row %u hand=%u,%u pos=%.0f,%.0f,%.0f name='%s'",
                          i, states[i].hIndex, states[i].hSerial,
                          states[i].x, states[i].y, states[i].z, nm);
                r[sizeof(r) - 1] = '\0'; coop::logLine(r);
            }
        }
    }
}

void Replicator::debugMark(Character* c, int colorId, const char* tag) {
    static int en = -1;
    if (en < 0) {
        const char* e = getenv("KENSHICOOP_DEBUG_MARKERS");
        en = (e && e[0] == '1') ? 1 : 0;
    }
    if (en != 1 || !c) return;
    std::map<Character*, DebugMarker>::iterator it = debugMarkers_.find(c);
    if (it != debugMarkers_.end() && it->second.color == colorId) return;
    char nm[40];
    engine::charName(c, nm, sizeof(nm));
    char cap[64];
    _snprintf(cap, sizeof(cap) - 1, "%s %s", tag, nm);
    cap[sizeof(cap) - 1] = '\0';
    if (it == debugMarkers_.end()) {
        void* l = engine::markerCreate(c, cap, colorId);
        if (l) {
            DebugMarker m; m.label = l; m.color = colorId;
            debugMarkers_[c] = m;
        }
    } else if (engine::markerUpdate(it->second.label, cap, colorId)) {
        it->second.color = colorId;
    }
}

void Replicator::applyNpcCensus(Inbound& in) {
    std::deque<InboundNpcCensus> got;
    in.drainNpcCensus(got);
    if (got.empty()) return;
    // Latest wins (reliable-ordered channel, 1 Hz - normally one pending).
    const InboundNpcCensus& nc = got.back();
    censusHands_.clear();
    censusPos_.clear();
    unsigned int n = (unsigned int)(nc.hands.size() / 5);
    bool havePos = nc.pos.size() >= (size_t)n * 3;
    for (unsigned int i = 0; i < n; ++i) {
        Key k;
        k.t  = nc.hands[i * 5 + 0];
        k.c  = nc.hands[i * 5 + 1];
        k.cs = nc.hands[i * 5 + 2];
        k.i  = nc.hands[i * 5 + 3];
        k.s  = nc.hands[i * 5 + 4];
        censusHands_.insert(k);
        if (havePos) {
            CensusPos cp;
            cp.x = nc.pos[i * 3 + 0];
            cp.y = nc.pos[i * 3 + 1];
            cp.z = nc.pos[i * 3 + 2];
            censusPos_[k] = cp;
        }
    }
    censusRecvMs_ = nowMs();
    static unsigned long logTick = 0;
    if ((censusRecvMs_ - logTick) > 10000) {
        logTick = censusRecvMs_;
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[census] recv n=%u culls=%lu",
                  n, censusCulls_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::enforceHostAuthority(GameWorld* gw) {
    if (!gw) return;
    // Hysteresis (step 5, spike 18): a hard streamed/unstreamed edge churned
    // boundary NPCs. Suppress only after a sustained unstreamed run (~1 s), and
    // restore only after a sustained streamed dwell (~2 s), counted in frames.
    const unsigned int SUPPRESS_AFTER_FRAMES = 75;
    const unsigned int RESTORE_AFTER_FRAMES  = 150;

    // Hands the host streamed a fresh sample for this tick = the authoritative set.
    std::set<Key> keep;
    for (std::map<Key, Driven>::iterator it = targets_.begin(); it != targets_.end(); ++it) {
        if (it->second.fresh) keep.insert(it->first);
    }

    // Proxy bodies are EXEMPT from authority judgment (2026-07-11 census-mint
    // fix): a proxy's LOCAL hand never matches its streamed key, so the wide
    // pass saw every far-minted proxy as a census-absent ghost and froze it
    // ~1 s after binding (spawn_far run 124346: all four proxies culled at
    // their mint position, then the drive's teleports no-opped on the frozen
    // bodies forever). Their existence authority is the census entry for the
    // STREAM key that minted them; drive/starve policy is applyTargets'.
    std::set<Character*> proxyChars;
    for (std::map<Key, Character*>::iterator it = proxyByKey_.begin();
         it != proxyByKey_.end(); ++it) proxyChars.insert(it->second);
    // Un-hide anything suppressed before it became a proxy / driven body (the
    // mint can land on a body a previous pass already judged).
    for (std::map<Key, Character*>::iterator it = suppressed_.begin();
         it != suppressed_.end(); ) {
        if (proxyChars.find(it->second) != proxyChars.end() ||
            drivenChars_.find(it->second) != drivenChars_.end()) {
            engine::restoreNpc(gw, it->second);
            ++authRestores_;
            { char b[128]; _snprintf(b, sizeof(b) - 1,
                "[authority] restore NPC hand=%u,%u (proxy/driven exemption; supp=%u)",
                (unsigned)it->first.i, (unsigned)it->first.s,
                (unsigned)suppressed_.size() - 1);
              b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            suppressed_.erase(it++);
        } else ++it;
    }

    // Enumerate the join's local world NPCs (same interest query as the host).
    const unsigned int MAX_NPCS = 256;
    static Character*  chars[MAX_NPCS]; // main-thread only
    static EntityState states[MAX_NPCS];
    unsigned int n = engine::listNpcs(gw, chars, states, MAX_NPCS);

    // Protocol 36 wide-radius existence pass: enumerate out to the census
    // radius so local-only ghosts get culled at render range instead of at the
    // ~200 u stream bubble (the 2026-07-09 field report). Only while the
    // host's census is FRESH - a silent census (host lagging, channel down)
    // DISABLES wide culling rather than mass-suppressing the loaded area.
    static Character*  wChars[NPC_CENSUS_MAX]; // main-thread only
    static EntityState wStates[NPC_CENSUS_MAX];
    unsigned int wn = 0;
    bool censusFresh = censusRadius_ > 0.0f && censusRecvMs_ != 0 &&
                       (nowMs() - censusRecvMs_) <= 5000;
    if (censusFresh)
        wn = engine::listNpcsWide(gw, censusRadius_, wChars, wStates, NPC_CENSUS_MAX);

    // Prune counters for hands the enumeration no longer sees (left interest),
    // preserving suppressed entries (a hidden body may drop out of the query but
    // must keep its counters so the restore dwell works when it returns).
    std::set<Key> seen;
    for (unsigned int i = 0; i < n; ++i) seen.insert(keyOf(states[i]));
    for (unsigned int i = 0; i < wn; ++i) seen.insert(keyOf(wStates[i]));
    for (std::map<Key, AuthCount>::iterator it = authCount_.begin(); it != authCount_.end(); ) {
        if (seen.find(it->first) == seen.end() &&
            suppressed_.find(it->first) == suppressed_.end()) authCount_.erase(it++);
        else ++it;
    }

    for (unsigned int i = 0; i < n; ++i) {
        // Proxy bodies answer to their streamed key's census entry, not their
        // local hand (which exists on no other client) - never judge them.
        if (proxyChars.find(chars[i]) != proxyChars.end()) continue;
        Key k = keyOf(states[i]);
        // Streamed = the hand is in this tick's fresh set, OR the body pointer is
        // one applyTargets drove this tick. The pointer check covers combat-
        // detached NPCs: detachFromTownAI re-containers the body, so its LOCAL
        // key differs from the streamed key and the hand lookup alone would
        // suppress an actively-driven combatant (crowd copies froze mid-brawl).
        bool streamed = (keep.find(k) != keep.end()) ||
                        (drivenChars_.find(chars[i]) != drivenChars_.end());
        // Pop-out fix (2026-07-11 field report): existence authority is the
        // CENSUS, drive authority is the STREAM. An NPC at the ~200 u stream-
        // bubble boundary flickers in/out of the host's fresh set (the two
        // clients disagree slightly on its position), and the old streamed-only
        // signal hid REAL host-present NPCs ('Saint'/'Kumo' measured churning
        // suppress->restore->suppress every few seconds). While the census is
        // fresh, a census-present NPC is never suppressed - its local AI copy
        // may drift, but it EXISTS; only census-absent ghosts get hidden. With
        // no fresh census (hatch off / host lagging) the legacy streamed-only
        // behavior stands.
        bool exists = streamed ||
                      (censusFresh && censusHands_.find(k) != censusHands_.end());
        std::map<Key, Character*>::iterator s = suppressed_.find(k);
        AuthCount& ac = authCount_[k];
        if (exists) { ac.unstreamed = 0; if (ac.streamed < 1000000u) ++ac.streamed; }
        else        { ac.streamed = 0;   if (ac.unstreamed < 1000000u) ++ac.unstreamed; }
        if (exists) {
            // Host owns it again: hand it back once the stream has DWELLED (a
            // boundary NPC that flickers into the set for a frame stays hidden).
            if (s != suppressed_.end() && ac.streamed >= RESTORE_AFTER_FRAMES) {
                engine::restoreNpc(gw, chars[i]);
                suppressed_.erase(s);
                s = suppressed_.end();
                ++authRestores_;
                { char nm[48]; engine::charName(chars[i], nm, sizeof(nm));
                  char b[160]; _snprintf(b, sizeof(b) - 1,
                    "[authority] restore NPC hand=%u,%u name='%s' (supp=%u churn=%lu/%lu)",
                    states[i].hIndex, states[i].hSerial, nm,
                    (unsigned)suppressed_.size(), authSuppresses_, authRestores_);
                  b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            }
            if (s == suppressed_.end())
                debugMark(chars[i], streamed ? 0 : 2, streamed ? "DRV" : "LOC");
            // NOTE: census position parking deliberately does NOT run inside
            // the stream bubble (npc_sync regression, run 185524): a bar NPC
            // whose two schedules seat it ~50 u apart is re-placed by its own
            // seat AI the same frame, so the park never sticks and the fight
            // wrecked tracking/march. Inside the bubble the stream owns
            // position truth; parking is a WIDE-pass render-range tool.
        } else {
            // Host neither streams nor lists it (census-absent ghost): after
            // the debounce, hide + freeze so the local AI can't run a divergent
            // copy on top of the host-driven world.
            if (s == suppressed_.end() && ac.unstreamed >= SUPPRESS_AFTER_FRAMES) {
                // Phase 2 hardening: only RECORD the suppression when the engine
                // call actually landed. A faulted hide used to be booked as done,
                // leaving the body visible forever with no evidence - the silent
                // version of the join-only-enemies field report. On failure the
                // unstreamed streak keeps climbing, so this retries every frame;
                // log the miss once at the threshold crossing.
                if (engine::suppressNpc(gw, chars[i])) {
                    suppressed_[k] = chars[i];
                    ++authSuppresses_;
                    debugMark(chars[i], 1, "HID");
                    { char nm[48]; engine::charName(chars[i], nm, sizeof(nm));
                      char b[192]; _snprintf(b, sizeof(b) - 1,
                        "[authority] suppress NPC hand=%u,%u name='%s' (streamed=%u local=%u supp=%u churn=%lu/%lu)",
                        states[i].hIndex, states[i].hSerial, nm, (unsigned)keep.size(), n,
                        (unsigned)suppressed_.size(), authSuppresses_, authRestores_);
                      b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                } else if (ac.unstreamed == SUPPRESS_AFTER_FRAMES) {
                    char b[96]; _snprintf(b, sizeof(b) - 1,
                        "[authority] suppress MISS hand=%u,%u (engine call failed; retrying)",
                        states[i].hIndex, states[i].hSerial);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
        }
    }

    // Wide pass (protocol 36): an NPC beyond the stream bubble is never in
    // this tick's fresh set, so its authority signal is EXISTENCE (its hand in
    // the host's census) rather than streaming. NPCs the near pass already
    // judged are skipped by pointer (its streamed logic is authoritative
    // inside the bubble), as is anything applyTargets drove this tick. Same
    // hysteresis counters so a census-boundary NPC doesn't churn.
    if (censusFresh && wn > 0) {
        std::set<Character*> nearSet;
        for (unsigned int i = 0; i < n; ++i) nearSet.insert(chars[i]);
        for (unsigned int i = 0; i < wn; ++i) {
            if (nearSet.find(wChars[i]) != nearSet.end()) continue;
            if (drivenChars_.find(wChars[i]) != drivenChars_.end()) continue;
            if (proxyChars.find(wChars[i]) != proxyChars.end()) continue;
            Key k = keyOf(wStates[i]);
            bool exists = censusHands_.find(k) != censusHands_.end() ||
                          keep.find(k) != keep.end();
            std::map<Key, Character*>::iterator s = suppressed_.find(k);
            AuthCount& ac = authCount_[k];
            if (exists) { ac.unstreamed = 0; if (ac.streamed < 1000000u) ++ac.streamed; }
            else        { ac.streamed = 0;   if (ac.unstreamed < 1000000u) ++ac.unstreamed; }
            if (exists) {
                if (s != suppressed_.end() && ac.streamed >= RESTORE_AFTER_FRAMES) {
                    engine::restoreNpc(gw, wChars[i]);
                    suppressed_.erase(s);
                    s = suppressed_.end();
                    ++authRestores_;
                    debugMark(wChars[i], 2, "LOC");
                    { char nm[48]; engine::charName(wChars[i], nm, sizeof(nm));
                      char b[160]; _snprintf(b, sizeof(b) - 1,
                        "[census] restore NPC hand=%u,%u name='%s' (supp=%u culls=%lu)",
                        wStates[i].hIndex, wStates[i].hSerial, nm,
                        (unsigned)suppressed_.size(), censusCulls_);
                      b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                }
                // v38 parking, wide-radius flavor (this is where the pack-
                // hidden class lives: census-present wilderness NPCs far
                // outside the stream bubble, each side simulating its own).
                if (s == suppressed_.end())
                    parkDivergedCopy(wChars[i], wStates[i], k);
            } else if (s == suppressed_.end() && ac.unstreamed >= SUPPRESS_AFTER_FRAMES) {
                if (engine::suppressNpc(gw, wChars[i])) {
                    suppressed_[k] = wChars[i];
                    ++authSuppresses_;
                    ++censusCulls_;
                    debugMark(wChars[i], 1, "HID");
                    { char nm[48]; engine::charName(wChars[i], nm, sizeof(nm));
                      char b[192]; _snprintf(b, sizeof(b) - 1,
                        "[census] cull NPC hand=%u,%u name='%s' pos=%.0f,%.0f,%.0f "
                        "(census=%u wide=%u supp=%u culls=%lu)",
                        wStates[i].hIndex, wStates[i].hSerial, nm,
                        wStates[i].x, wStates[i].y, wStates[i].z,
                        (unsigned)censusHands_.size(), wn,
                        (unsigned)suppressed_.size(), censusCulls_);
                      b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                } else if (ac.unstreamed == SUPPRESS_AFTER_FRAMES) {
                    char b[96]; _snprintf(b, sizeof(b) - 1,
                        "[census] cull MISS hand=%u,%u (engine call failed; retrying)",
                        wStates[i].hIndex, wStates[i].hSerial);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
        }
    }

    // Phase 2 hardening: RE-ASSERT the hide on a ~2 s cadence. suppressNpc is a
    // one-shot (remove-from-update + clearGoals + setVisible(false)), but the
    // engine can undo it on its own: an ambush squad's dialog/combat package
    // re-tasks the body and zone streaming can re-add it to the update list -
    // exactly the field report ("join-only enemies started dialog then attacked
    // and stayed visible"). The call is idempotent, so re-applying to a body the
    // engine never touched is a no-op; keys the host started streaming again are
    // skipped (the restore dwell owns those).
    //
    // Lifetime guard (2026-07-11 join crash): the engine owns every suppressed
    // body and can despawn it at any time - the 17:53 session held 93 hidden
    // wildlife bodies through a zone stream, and the dump shows the engine's
    // own sensory update walking a freed body. Before ANY touch, prove each
    // entry live with a hand round-trip: SEH-read the pointer's CURRENT hand
    // and resolve it back - the same pointer proves the body is alive (this
    // survives engine re-containering, which only changes the hand); anything
    // else means despawned, and the entry + marker + counters are dropped
    // without touching the pointer. A live body whose hand CHANGED (combat
    // detach re-containered it while hidden) migrates its entry to the new key
    // so the hide keeps re-asserting - the old key would never resolve again.
    unsigned long now = nowMs();
    if ((now - authReassertMs_) >= 2000) {
        authReassertMs_ = now;
        unsigned int pruned = 0;
        std::map<Key, Character*> migrated;
        for (std::map<Key, Character*>::iterator it = suppressed_.begin();
             it != suppressed_.end(); ) {
            unsigned int h[5];
            Character* live = 0;
            if (engine::readHand(it->second, h))
                live = engine::resolveCharByHand(h[0], h[1], h[2], h[3], h[4]);
            if (live != it->second) {
                std::map<Character*, DebugMarker>::iterator mi =
                    debugMarkers_.find(it->second);
                if (mi != debugMarkers_.end()) {
                    engine::markerDestroy(mi->second.label);
                    debugMarkers_.erase(mi);
                }
                authCount_.erase(it->first);
                ++pruned; ++authPruned_;
                suppressed_.erase(it++);
                continue;
            }
            Key ck; ck.i = h[0]; ck.s = h[1]; ck.t = h[2];
            ck.c = h[3]; ck.cs = h[4];
            if (ck < it->first || it->first < ck) {
                migrated[ck] = it->second;
                authCount_.erase(it->first);
                suppressed_.erase(it++);
                continue;
            }
            if (keep.find(it->first) != keep.end()) { ++it; continue; }
            // A combat-detached body is driven under a DIFFERENT streamed key
            // (pointer identity survives the re-containering); never re-hide a
            // body applyTargets drove this tick.
            if (drivenChars_.find(it->second) != drivenChars_.end()) { ++it; continue; }
            engine::suppressNpc(gw, it->second);
            ++it;
        }
        for (std::map<Key, Character*>::iterator it = migrated.begin();
             it != migrated.end(); ++it) {
            if (suppressed_.find(it->first) != suppressed_.end()) continue;
            suppressed_[it->first] = it->second;
            if (keep.find(it->first) == keep.end() &&
                drivenChars_.find(it->second) == drivenChars_.end())
                engine::suppressNpc(gw, it->second);
        }
        if (pruned > 0) {
            char b[128]; _snprintf(b, sizeof(b) - 1,
                "[authority] pruned %u stale suppressed entries (despawned; supp=%u total=%lu)",
                pruned, (unsigned)suppressed_.size(), authPruned_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // Marker map hygiene on the same cadence: drop labels whose Character*
        // wasn't vouched live this pass (this tick's enumerations, driven and
        // proxy sets, plus the just-validated suppressed bodies). A pruned
        // body that comes back into judgment simply gets a fresh label.
        if (!debugMarkers_.empty()) {
            std::set<Character*> vouched;
            for (unsigned int i = 0; i < n; ++i)  vouched.insert(chars[i]);
            for (unsigned int i = 0; i < wn; ++i) vouched.insert(wChars[i]);
            vouched.insert(drivenChars_.begin(), drivenChars_.end());
            vouched.insert(proxyChars.begin(), proxyChars.end());
            for (std::map<Key, Character*>::iterator it = suppressed_.begin();
                 it != suppressed_.end(); ++it) vouched.insert(it->second);
            pruneDebugMarkers(vouched);
        }
    }

    // Existence-audit probe (pack-hidden investigation, 2026-07-11): a 5 s
    // census of what THIS client's world holds vs what the host vouches for,
    // classifying every enumerated NPC into exactly one bucket:
    //   drv   - streamed/driven this tick (host actively drives it)
    //   cen   - census-present, unstreamed (legit local-sim copy, host has it)
    //   hid   - booked suppressed (we hid it)
    //   ghost - census-absent, NOT suppressed (the visible-on-join-only class:
    //           either inside the suppress debounce, judged only while the
    //           census was stale, or escaping judgment entirely)
    // ghost is the bucket the field reports live in - it should only ever be
    // transient (one debounce, ~1 s). Test-ExistenceParity gates on it.
    // KENSHICOOP_DEBUG_CENSUS=1 additionally dumps a row per ghost.
    static unsigned long auditMs = 0; // main-thread only
    if ((now - auditMs) >= 5000) {
        auditMs = now;
        unsigned int cDrv = 0, cCen = 0, cHid = 0, cGhost = 0;
        static int dumpGhost = -1;
        if (dumpGhost < 0) {
            const char* e = getenv("KENSHICOOP_DEBUG_CENSUS");
            dumpGhost = (e && e[0] == '1') ? 1 : 0;
        }
        std::set<Character*> counted;
        unsigned int ghostRows = 0;
        for (int pass = 0; pass < 2; ++pass) {
            unsigned int cnt = (pass == 0) ? n : wn;
            Character**  cs  = (pass == 0) ? chars : wChars;
            EntityState* sts = (pass == 0) ? states : wStates;
            for (unsigned int i = 0; i < cnt; ++i) {
                if (!counted.insert(cs[i]).second) continue;
                if (proxyChars.find(cs[i]) != proxyChars.end()) { ++cDrv; continue; }
                Key k = keyOf(sts[i]);
                bool streamed = keep.find(k) != keep.end() ||
                                drivenChars_.find(cs[i]) != drivenChars_.end();
                if (streamed) { ++cDrv; continue; }
                if (suppressed_.find(k) != suppressed_.end()) { ++cHid; continue; }
                if (censusFresh && censusHands_.find(k) != censusHands_.end()) {
                    ++cCen; continue;
                }
                ++cGhost;
                if (dumpGhost == 1 && ghostRows < 10) {
                    ++ghostRows;
                    char nm[48]; engine::charName(cs[i], nm, sizeof(nm));
                    char r[192]; _snprintf(r, sizeof(r) - 1,
                        "[audit] ghost hand=%u,%u name='%s' pos=%.0f,%.0f,%.0f "
                        "unstreak=%u",
                        sts[i].hIndex, sts[i].hSerial, nm,
                        sts[i].x, sts[i].y, sts[i].z, authCount_[k].unstreamed);
                    r[sizeof(r) - 1] = '\0'; coop::logLine(r);
                }
            }
        }
        char b[192]; _snprintf(b, sizeof(b) - 1,
            "[audit] exist near=%u wide=%u drv=%u cen=%u hid=%u ghost=%u "
            "supp=%u census=%u fresh=%d parks=%lu",
            n, wn, cDrv, cCen, cHid, cGhost,
            (unsigned)suppressed_.size(), (unsigned)censusHands_.size(),
            censusFresh ? 1 : 0, censusParks_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::parkDivergedCopy(Character* c, const EntityState& st, const Key& k) {
    // v38 pack-hidden fix: existence culling exempts census-present NPCs, but
    // both clients then run INDEPENDENT sims of the same body - the join's
    // copy can stand somewhere the host's copy isn't (the "pack hidden" save:
    // a pack visible on the join with no host counterpart at that spot).
    // The census now carries the host position per row; reconcile a local
    // copy that drifted past the park distance with a halt+teleport onto the
    // host's spot. 120 u default: ABOVE town-schedule divergence (two sims
    // seating the same bar NPC ~50 u apart - run 185524), so only genuinely
    // divergent wanderers (measured 500-900 u) trip it.
    if (censusParkDist_ <= 0.0f) return;
    std::map<Key, CensusPos>::iterator it = censusPos_.find(k);
    if (it == censusPos_.end()) return;
    float d = dist3(st.x, st.y, st.z, it->second.x, it->second.y, it->second.z);
    if (d <= censusParkDist_) return;
    // Per-key cooldown (npc_sync regression, run 185524): the engine's own
    // schedule AI can re-place the body the same frame (a seated copy), so an
    // unthrottled park re-teleported every frame and wrecked tracking. One
    // park per key per cooldown bounds the fight; a free wanderer sticks on
    // the first try.
    unsigned long nowP = nowMs();
    std::map<Key, unsigned long>::iterator pm = parkMs_.find(k);
    if (pm != parkMs_.end() && (nowP - pm->second) < 5000) return;
    parkMs_[k] = nowP;
    if (engine::park(c, it->second.x, it->second.y, it->second.z, st.heading)) {
        ++censusParks_;
        static unsigned long logTick = 0; // main-thread only, ~4 lines/s
        unsigned long now = nowMs();
        if ((now - logTick) >= 250) {
            logTick = now;
            char nm[48]; engine::charName(c, nm, sizeof(nm));
            char b[192]; _snprintf(b, sizeof(b) - 1,
                "[census] park hand=%u,%u name='%s' d=%.0f local=%.0f,%.0f,%.0f "
                "host=%.0f,%.0f,%.0f (parks=%lu)",
                k.i, k.s, nm, d, st.x, st.y, st.z,
                it->second.x, it->second.y, it->second.z, censusParks_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
}

void Replicator::pruneDebugMarkers(const std::set<Character*>& live) {
    for (std::map<Character*, DebugMarker>::iterator it = debugMarkers_.begin();
         it != debugMarkers_.end(); ) {
        if (live.find(it->first) == live.end()) {
            engine::markerDestroy(it->second.label);
            debugMarkers_.erase(it++);
        } else ++it;
    }
}

void Replicator::applyRest(Character* c, Driven& d, const EntityState& out,
                           bool haveActual, float ax, float ay, float az,
                           unsigned long now, bool isSquad) {
    // Re-arm only when the host adopts a genuinely NEW non-NONE rest pose (stood up
    // then sat somewhere else). Crucially we IGNORE transient host->NONE frames: the
    // host capture intermittently reads currentAction==NONE for an otherwise-seated
    // NPC (transition frames), and re-arming on those tore the committed sit back
    // down to a standing park every few frames -> the body oscillated sit/stand and
    // mostly rendered standing. Holding through NONE keeps the seated pose sticky;
    // genuine stand-up is caught by the movement re-arm in applyTargets.
    // Carried-body sync (protocol 18): TASK_CARRY_BODY is a SYNTHETIC marker
    // (the carry self-heal in applyTargets owns it), not an engine task - never
    // inject it as a pose. A stationary carrier just parks like a task-less body
    // (the engine's own carry attach keeps the shoulder animation running).
    bool syntheticCarry = coop::taskIsCarry(out.task);
    if (!syntheticCarry && out.task != TASK_NONE && out.task != d.issuedTask) {
        { char b[96]; _snprintf(b, sizeof(b) - 1,
            "[pose] rest re-arm task %u -> %u", (unsigned)d.issuedTask,
            (unsigned)out.task); b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
        d.taskApplied = false; d.taskBad = false; d.taskRetries = 0;
        d.issuedTask = out.task;
    }
    // Commit a reproducible pose (sit/operate) at the SAME fixture, once.
    // Attempts are throttled (TASK_RETRY_MS) so a retried far-fixture apply
    // doesn't clearGoals every frame.
    if (!syntheticCarry && out.task != TASK_NONE && !d.taskBad && !d.taskApplied &&
        (d.taskRetries == 0 || (now - d.taskTick) >= TASK_RETRY_MS)) {
        // I9: detach from the town-AI FIRST (once) so nothing auto-tasks this NPC,
        // then reproduce the pose via the PLAYER-ORDER path (explicit seat location)
        // so it pins THIS stool instead of running SIT_AROUND's own seat search.
        //
        // Step-2 pruning candidate: with AI-suspend as the default quieting layer
        // the town-AI's re-tasker never runs, so the detach (which carries the
        // hand-identity hazard - it re-containers the body) may be redundant.
        // detachUses_ measures how often this fires; KENSHICOOP_NO_DETACH=1 skips
        // it for a manual A/B before any deletion.
        // NEVER detach a player-squad member: separateIntoMyOwnSquad re-containers
        // the body into a NEW platoon (a new hand), destroying the save-stable
        // identity every hand-keyed protocol relies on. Squad members have no
        // town-AI re-tasker anyway (they are player-controlled + peer-driven).
        if (!d.detached && !noDetach_ && !isSquad) {
            d.detached = engine::detachFromTownAI(c);
            if (d.detached) ++detachUses_;
        }
        int r = engine::applyTaskOrder(c, out);
        ++sitOrders_;
        d.taskTick = now;
        { char b[176]; _snprintf(b, sizeof(b) - 1,
            "[pose] applyOrder hand=%u,%u task=%u subj=%u,%u,%u det=%d r=%d try=%u",
            out.hIndex, out.hSerial, (unsigned)out.task,
            out.sIndex, out.sSerial, out.sType, d.detached ? 1 : 0, r,
            d.taskRetries);
          b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
        if (r == 2) { d.taskApplied = true; d.taskRetries = 0; } // posed at the fixture
        else if (r == 1) d.taskBad = true; // fixture not loaded here -> park
        else if (r == 3) {
            // Far fixture: usually the interp target lagging a snap-into-fixture
            // on the owner (bed entry teleports the body instantly). The park
            // drive converges the body meanwhile; retry until the gate passes,
            // latch bad only when the mismatch persists (genuinely wrong fixture).
            if (++d.taskRetries >= TASK_FAR_RETRY_MAX) d.taskBad = true;
        }
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
    // Step-2 finding (2026-07-05 A/B): the once-on-rest-entry clearGoals variant
    // was tried and REVERTED - coupled with suspension it degraded npc_sync
    // smoothness/tracking, and the relapse counters below proved the residual
    // quieting patchwork is still load-bearing even under AI-suspend (relapse
    // fired ~100-1000x/run in BOTH modes). The per-tick clear stays; the QUIET
    // counters stay as permanent health telemetry.
    engine::clearGoals(c);
    d.goalsCleared = true;
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
    //
    // Step-2 pruning candidate: under default AI-suspend the relapse source (the
    // AI re-acquiring a walk) should be gone. quietRelapse_ counts every firing
    // ("SCENARIO QUIET relapse=N" per run); sustained zeros = safe to delete.
    {
        bool reMoving = false; float reSpeed = 0.0f;
        if (engine::readMotion(c, &reMoving, &reSpeed) && reMoving && reSpeed > 0.1f) {
            engine::endAction(c);
            ++quietRelapse_;
        }
    }
    engine::applyMotion(c, false, 0.0f, 0.0f, 0.0f, 0.0f);
}

void Replicator::logSmoothSummary() {
    float zeroFrac = (activeFrames_ > 0)
                         ? (float)zeroWhileActive_ / (float)activeFrames_
                         : 0.0f;
    char b[176];
    _snprintf(b, sizeof(b) - 1,
              "SCENARIO SMOOTH active=%lu zeroWhileActive=%lu zeroFrac=%.3f maxStep=%.3f slewSkip=%lu",
              activeFrames_, zeroWhileActive_, zeroFrac, maxStep_, slewSkipFrames_);
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

    // Step-2 pruning evidence: how often the legacy quieting patchwork actually
    // fired this run. Sustained relapse=0 across regressions = the I11 re-quiet is
    // dead code under default AI-suspend and can be deleted; detach counts feed the
    // KENSHICOOP_NO_DETACH A/B decision.
    char q[160];
    _snprintf(q, sizeof(q) - 1,
              "SCENARIO QUIET relapse=%lu sitOrders=%lu detach=%lu noDetach=%d",
              quietRelapse_, sitOrders_, detachUses_, noDetach_ ? 1 : 0);
    q[sizeof(q) - 1] = '\0';
    coop::logLine(q);

    // Step-4 evidence: how much of the driven set the divergence gate handed back
    // to local AI. grants>0 = the mechanism engages; the npc_track oracle proves
    // trusted bodies still track.
    if (gateAuthority_) {
        char t[128];
        _snprintf(t, sizeof(t) - 1,
                  "SCENARIO TRUST grants=%lu revokes=%lu", trustGrants_, trustRevokes_);
        t[sizeof(t) - 1] = '\0';
        coop::logLine(t);
    }

    // Step-5 evidence: suppression churn under the hysteresis band (split_interest
    // metric; boundary flip-flops show up as high counts).
    char au[112];
    _snprintf(au, sizeof(au) - 1,
              "SCENARIO AUTH suppresses=%lu restores=%lu", authSuppresses_, authRestores_);
    au[sizeof(au) - 1] = '\0';
    coop::logLine(au);

    // Protocol 36 jumpiness evidence: what regime the interp buffer ran in
    // (extrapFrac = starvation share of all samples) and how often the drive
    // layer had to hard-snap / re-path. Under WAN jitter these are the numbers
    // the interp fixes must move.
    {
        unsigned long total = interpLerp_ + interpSingle_ + interpClampOld_ +
                              interpExtrap_ + interpSegSnap_;
        float extrapFrac = (total > 0)
                               ? (float)(interpExtrap_ + interpClampOld_) / (float)total
                               : 0.0f;
        char ip[240];
        _snprintf(ip, sizeof(ip) - 1,
                  "SCENARIO INTERP samples=%lu lerp=%lu extrap=%lu clamp=%lu seg=%lu "
                  "extrapFrac=%.3f snapSq=%lu snapNpc=%lu reissueSq=%lu reissueNpc=%lu "
                  "restFlip=%lu pruned=%lu",
                  total, interpLerp_, interpExtrap_, interpClampOld_, interpSegSnap_,
                  extrapFrac, hardSnapSquad_, hardSnapNpc_,
                  walkReissueSquad_, walkReissueNpc_, restFlipNpc_, authPruned_);
        ip[sizeof(ip) - 1] = '\0';
        coop::logLine(ip);
    }

    // Step-3 evidence: how many locally-simulated melee hits the damage guard
    // intercepted vs passed through. guarded>0 in a combat scenario proves the
    // hook engaged (complements the blood-flat vitals check).
    if (dmgGuard_) {
        unsigned long guarded = 0, passed = 0;
        engine::damageGuardStats(&guarded, &passed);
        char dg[112];
        _snprintf(dg, sizeof(dg) - 1,
                  "SCENARIO DMGGUARD guarded=%lu passed=%lu", guarded, passed);
        dg[sizeof(dg) - 1] = '\0';
        coop::logLine(dg);
    }
}

} // namespace coop
