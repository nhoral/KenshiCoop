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
const unsigned long ATTR_WINDOW_MS = 3000; // remember a combatant's victim this long, so a
                                           // KO/death edge can still name the attacker

float dist3(float ax, float ay, float az, float bx, float by, float bz) {
    float dx = ax - bx, dy = ay - by, dz = az - bz;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Conservation channel item types. itemType 2 = WEAPON (createItem can't rebuild a proxy)
// and 3 = ARMOUR/clothing. Both are non-stackable EQUIPPABLE gear, so each unit is a distinct
// object the peer already mirrors (weapons via shared save; armour also reconstructed by inv
// sync) - the real object can be relocated bag<->ground on every client and re-homed on pickup
// WITHOUT fabrication. The W1 host-authored proxy stream handles everything else (stacks, loot)
// and skips these. Routing gear through conservation also fixes a host-dropped item lingering
// on the host ground after the JOIN picks it up (the W1 cull only removes the join's proxy).
inline bool isGearType(unsigned int t) { return t == 2u || t == 3u; }
} // namespace

Replicator::Replicator()
    : leaderOnly_(true), streamNpcs_(false),
      activeFrames_(0), zeroWhileActive_(0), maxStep_(0.0f),
      translateFrames_(0), walkTruthFrames_(0),
      restSampleFrames_(0), marchFrames_(0),
      gateSamples_(0), gateAgree_(0), gateLogTick_(0),
      probeRecruit_(false), probedCount_(0),
      probeAiSuspend_(false), aiLogTick_(0), nextEventId_(1),
      nextWorldNetId_(1), nextDropId_(1), nextPickupId_(1) {}

void Replicator::ingest(Inbound& in) {
    std::deque<InboundEntity> got;
    in.drainEntities(got);
    if (got.empty()) return;
    unsigned long now = nowMs();
    for (std::deque<InboundEntity>::iterator it = got.begin(); it != got.end(); ++it) {
        targets_[keyOf(it->e)].interp.push(it->e, now);
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
        Key k; k.t = it->cHand[0]; k.c = it->cHand[1]; k.cs = it->cHand[2];
        k.i = it->cHand[3]; k.s = it->cHand[4];
        InvRecv& r = invRecv_[k];
        r.ownerId = it->ownerId;
        r.items   = it->items; // latest snapshot supersedes
        r.dirty   = true;
    }
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
    ownHands_.clear();
    unsigned int n = 0;
    for (unsigned int i = 0; i < nSquad && n < MAX_PUBLISH; ++i) {
        std::pair<u32, u32> key(raw[i].hContainer, raw[i].hContainerSerial);
        unsigned int rank = (unsigned int)(std::lower_bound(ctnrs.begin(), ctnrs.end(), key)
                                           - ctnrs.begin());
        // Empty ownRanks_ (never configured) is a safety fallback to the first tab,
        // so a missing setOwnRanks never makes us stream every tab or nothing.
        bool owned = ownRanks_.empty() ? (rank == 0u) : (ownRanks_.count(rank) != 0);
        if (!owned) continue;
        buf[n++] = raw[i];
        ownHands_.insert(keyOf(raw[i]));
    }
    // Host also streams nearby world NPCs (host-authoritative world). The join leaves
    // streamNpcs_ off, so on the join this publishes ONLY its owned squad subset.
    if (streamNpcs_ && n < MAX_PUBLISH)
        n += engine::captureNpcs(gw, buf + n, MAX_PUBLISH - n);
    net.setOwnedEntities(ownerId, buf, n);

    // Refresh the (sticky) attacker map from this tick's combat intents: a captured
    // entity with task==TASK_COMBAT_MELEE carries its target in the subject fields, so it
    // is the ATTACKER of that subject. Stamp lastSeen=now; entries persist (recency
    // window below) so a KO/death edge - where the attacker has already dropped its
    // now-fallen target - can still recover who did it. Prune entries older than the
    // window so stale pairings don't mis-attribute a later, unrelated death.
    unsigned long nowPub = nowMs();
    for (unsigned int i = 0; i < n; ++i) {
        const EntityState& e = buf[i];
        if (e.task != TASK_COMBAT_MELEE) continue;
        Key victim; victim.t = e.sType; victim.c = e.sContainer;
        victim.cs = e.sContainerSerial; victim.i = e.sIndex; victim.s = e.sSerial;
        attackerOf_[victim] = std::make_pair(keyOf(e), nowPub);
    }
    for (std::map<Key, std::pair<Key, unsigned long> >::iterator pr = attackerOf_.begin();
         pr != attackerOf_.end(); ) {
        if (nowPub - pr->second.second > ATTR_WINDOW_MS) attackerOf_.erase(pr++);
        else ++pr;
    }

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
        hostBody_[k] = cur;
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
    if (owned.empty()) return;
    const unsigned long INV_RESEND_MS = 5000; // periodic safety resend (loss/late join)
    // A changed snapshot must be STABLE this long before we publish it. A change that only
    // REARRANGES or ADDS (entry count >= last sent) settles fast. A change that REMOVES an
    // entry settles much longer: mid-drag the UI holds the dragged item on the CURSOR, out
    // of the inventory entirely, for up to ~1 s - a transient "item gone" the peer would act
    // on by DESTROYING a worn item it cannot refabricate (createItemAndAdd is unreliable for
    // weapons; equipped fabrication is non-persistent, d25), losing it for good. Equip and
    // unequip-to-bag keep the entry count (a MOVE), so they still replicate promptly; only
    // genuine removals (and the in-cursor flicker) wait out the longer window.
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
        net.queueInvSnapshot(ownerId, cHand, items, n);
        pub.hash = hash; pub.lastSendMs = now; pub.lastSentN = n;
        if (changed) {
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                "[inv] SEND hand=%u,%u,%u,%u,%u items=%u hash=%u",
                it->t, it->c, it->cs, it->i, it->s, n, hash);
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
        engine::applyContainerContents(gw, cHand, items, n);
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
    // Host-authoritative world stream. Scan the interest sphere for free ground items,
    // assign/reuse a netId per item (keyed by its local engine hand), and stream new/
    // changed items + cull vanished ones. A settled world produces stable content+pos -
    // so zero traffic - with a slow periodic safety resend.
    const float         RADIUS       = 60.0f; // interest scope for ground items (v1)
    const float         POS_EPS      = 0.5f;  // re-stream a moved item past this gap
    const unsigned long WI_RESEND_MS = 5000;  // periodic safety resend (loss / late join)
    static int dumpWi = -1;
    if (dumpWi < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dumpWi = (e && e[0] == '1') ? 1 : 0; }

    engine::WorldItemRaw raw[WORLD_ITEMS_MAX];
    unsigned int n = engine::captureWorldItems(gw, raw, WORLD_ITEMS_MAX, RADIUS);
    unsigned long now = nowMs();

    for (std::map<Key, WorldTrack>::iterator it = worldTrack_.begin(); it != worldTrack_.end(); ++it)
        it->second.seen = false;

    // Gear (itemType 2 WEAPON / 3 ARMOUR) is handled by the conservation drop/pickup channel
    // (the real object is relocated bag<->ground on each client and re-homed on pickup), so the
    // W1 template-proxy stream skips it - both to avoid a duplicate proxy AND because a W1 cull
    // only removes the join's proxy, leaving a host-dropped real item orphaned on the ground.
    WorldItemEntry send[WORLD_ITEMS_MAX]; unsigned int ns = 0;
    for (unsigned int i = 0; i < n; ++i) {
        if (isGearType(raw[i].itemType)) continue;
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

    // Snapshots: spawn a proxy for a new netId, move it if it changed.
    for (std::deque<InboundWorldItems>::iterator b = items.begin(); b != items.end(); ++b) {
        for (std::vector<WorldItemEntry>::iterator e = b->items.begin(); e != b->items.end(); ++e) {
            std::map<u32, WorldProxy>::iterator pit = worldProxies_.find(e->netId);
            if (pit == worldProxies_.end()) {
                RootObject* obj = engine::spawnWorldItemProxy(gw, e->stringID, e->itemType,
                                                              (int)e->quantity, e->x, e->y, e->z);
                if (obj) {
                    WorldProxy wp; wp.obj = obj; wp.x = e->x; wp.y = e->y; wp.z = e->z; wp.hash = 0;
                    worldProxies_[e->netId] = wp;
                }
                char b2[200]; _snprintf(b2, sizeof(b2) - 1,
                    "[wi] SPAWN netId=%u ok=%d sid='%s' pos=%.2f,%.2f,%.2f",
                    e->netId, obj ? 1 : 0, e->stringID, e->x, e->y, e->z);
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
    // Culls: destroy the proxy and drop the mapping.
    for (std::deque<InboundWorldRemove>::iterator b = rems.begin(); b != rems.end(); ++b) {
        for (std::vector<u32>::iterator id = b->netIds.begin(); id != b->netIds.end(); ++id) {
            std::map<u32, WorldProxy>::iterator pit = worldProxies_.find(*id);
            if (pit == worldProxies_.end()) continue;
            engine::removeWorldItemProxy(gw, pit->second.obj);
            worldProxies_.erase(pit);
            if (dumpWi) { char b2[96]; _snprintf(b2, sizeof(b2) - 1, "[wi] CULL netId=%u", *id);
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
        char b[200]; _snprintf(b, sizeof(b) - 1,
            "[event] RECV id=%u ev=%u owner=%u hand=%u,%u,%u,%u,%u actor=%u,%u",
            ev.eventId, (unsigned)ev.event, ev.ownerId,
            ev.sType, ev.sContainer, ev.sContainerSerial, ev.sIndex, ev.sSerial,
            ev.aIndex, ev.aSerial);
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
        // Never drive a body WE own: we control + stream it locally, the peer drives
        // its copy from our stream. The disjoint partition + no local loopback means
        // our own hand shouldn't appear in targets_, but guard regardless (a stray
        // self-owned sample would otherwise fight our own control every frame).
        if (ownHands_.find(it->first) != ownHands_.end()) continue;
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
