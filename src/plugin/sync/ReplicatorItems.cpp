// ReplicatorItems.cpp - item planes (monolith split from Replicator.cpp,
// 2026-07-12): publishInventories/applyInventories (Phase 4a container
// contents), publishWorldItems/applyWorldItems (Phase W1 ground-item
// proxies), detectAndPublishWeaponDrops + applyWeaponDrops/Pickups (Phase W2
// gear conservation) and the protocol 37 cross-owner transfer intents
// (xferRebase/xferPendingLoss/wdSuppressed/detectAndPublishTransfers/
// applyTransfers).
//
// Shared hubs: owns invSnap_/worldItems_/weaponCensus_/xfer* bookkeeping;
// reads ownHands_ (stamped by the publish TU).
// Must NOT: change any log string - log phrasing is the API consumed by the
// PowerShell oracles (see resources/CODE_MAP.md, log-tag index).

#include "ReplicatorUtil.h"

namespace coop {

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

// Local content fingerprint for a tracked world item (change-detection ONLY, so it
// need only be stable on this client - cross-client matching uses netId + position
// tolerance). Mirrors the engine-side worldItemHash inputs (sid + type + qty + qual).
static u32 worldTrackHash(const char* sid, u32 type, u16 qty, u16 qual) {
    u32 h = 2166136261u;
    if (sid) for (const char* p = sid; *p; ++p) { h ^= (unsigned char)*p; h *= 16777619u; }
    h ^= type * 2654435761u;
    h ^= (u32)qty  * 40503u;
    h ^= (u32)qual * 2246822519u;
    return h ? h : 1u;
}

void Replicator::publishWorldItems(GameWorld* gw, NetLink& net, u32 ownerId) {
    // Owner-authoritative world stream, BOTH directions since the W1 bidir fix (each
    // client streams the free ground items IT authors). DISCOVERY has two sources now:
    //   (1) the query-free drop hook (engine::drainItemDrops) - a drop captured at
    //       Inventory::dropItem, so a TOWN drop is found even when the spatial query
    //       misses it (the core town-reliability fix); and
    //   (2) the spatial scan (captureWorldItems) - best-effort, for pre-existing save
    //       items / host runtime drops the drop hook didn't author.
    // Both key a track by the item's LOCAL engine hand, so a drop found by BOTH sources
    // converges on ONE track (one netId) - no duplicate proxy. CULLING is now HANDLE-
    // based (engine::groundItemLiveness): a track is removed only when its real item is
    // gone or picked up, NOT when a single scan misses it - killing the town flicker.
    const float         RADIUS       = 60.0f; // interest scope for ground items (v1)
    const float         POS_EPS      = 0.5f;  // re-stream a moved item past this gap
    const unsigned long WI_RESEND_MS = 5000;  // periodic safety resend (loss / late join)
    static int dumpWi = -1;
    if (dumpWi < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dumpWi = (e && e[0] == '1') ? 1 : 0; }
    unsigned long now = nowMs();

    // ECHO GUARD: a proxy we spawned for a PEER's streamed item is a real local
    // RootObject and enumerates like any other ground item - re-publishing it would
    // bounce the item back to its author as a duplicate. Filter every discovery row
    // that resolves to an object in our proxy set.
    std::set<RootObject*> proxyObjs;
    for (std::map<std::pair<u32, u32>, WorldProxy>::iterator pi = worldProxies_.begin();
         pi != worldProxies_.end(); ++pi)
        proxyObjs.insert(pi->second.obj);

    // ---- First-scan baseline (Phase 3 item-dup fix) ------------------------
    // Every non-gear ground item present at the FIRST publish pass after a load
    // is a SHARED save-native: the peer loaded the same save (or the host's
    // connect-pushed save) and already holds an identical copy. Streaming it
    // would mint a proxy on top of the peer's own native - the "rejoin/reload
    // duplicated all items" report, compounding one layer per reload. Seed them
    // as baseline tracks (identity + liveness only, NEVER emitted). Only items
    // that appear AFTER this baseline (session drops via the hook, host runtime
    // spawns) stream. resetSession() clears worldSeeded_ so each reload re-
    // baselines the (possibly newly-baked) save-natives instead of re-streaming.
    if (!worldSeeded_) {
        worldSeeded_ = true;
        engine::WorldItemRaw raw[WORLD_ITEMS_MAX];
        unsigned int n = engine::captureWorldItems(gw, raw, WORLD_ITEMS_MAX, RADIUS);
        unsigned int seeded = 0;
        for (unsigned int i = 0; i < n; ++i) {
            if (isGearType(raw[i].itemType)) continue;
            if (!proxyObjs.empty() &&
                proxyObjs.count(engine::resolveObjectByHand(raw[i].hand)) != 0)
                continue; // already our proxy (defensive; unlikely at first scan)
            Key k; k.t = raw[i].hand[0]; k.c = raw[i].hand[1]; k.cs = raw[i].hand[2];
            k.i = raw[i].hand[3]; k.s = raw[i].hand[4];
            if (worldTrack_.find(k) != worldTrack_.end()) continue;
            WorldTrack t; memset(&t, 0, sizeof(t));
            t.netId = nextWorldNetId_++; t.hash = 0; t.lastSendMs = 0;
            t.x = raw[i].x; t.y = raw[i].y; t.z = raw[i].z; t.seen = true;
            t.baseline = true; // never emit
            strncpy(t.stringID, raw[i].stringID, sizeof(t.stringID) - 1);
            t.stringID[sizeof(t.stringID) - 1] = '\0';
            t.itemType = raw[i].itemType; t.quantity = raw[i].quantity; t.quality = raw[i].quality;
            worldTrack_[k] = t;
            ++seeded;
        }
        char b[96]; _snprintf(b, sizeof(b) - 1,
            "[wi] BASELINE seeded=%u (save-native, never-stream)", seeded);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // Gear (itemType WEAPON/ARMOUR) rides the W2 conservation drop/pickup channel
    // (the real shared-save object is relocated bag<->ground on each client), so the
    // W1 template-proxy stream skips it in BOTH discovery sources below.

    // ---- Discovery 1: query-free drop-hook edges (town reliable) -----------
    {
        engine::ItemDropEdge de[64];
        unsigned int nde = engine::drainItemDrops(de, 64);
        for (unsigned int i = 0; i < nde; ++i) {
            if (isGearType(de[i].itemType)) continue;
            if (de[i].itemHand[3] == 0 && de[i].itemHand[4] == 0) continue; // unresolved hand
            // A drop from a PEER-owned squad copy is the peer's to author (it streams
            // its own drop); authoring it here too would duplicate the proxy. World
            // NPC (class 0) and our own squad (class 1) drops still stream.
            if (ownerClassForHand(de[i].ownerHand) == 2) continue;
            Key k; k.t = de[i].itemHand[0]; k.c = de[i].itemHand[1]; k.cs = de[i].itemHand[2];
            k.i = de[i].itemHand[3]; k.s = de[i].itemHand[4];
            if (worldTrack_.find(k) != worldTrack_.end()) continue; // already tracked
            WorldTrack t; memset(&t, 0, sizeof(t));
            t.netId = nextWorldNetId_++; t.hash = 0; t.lastSendMs = 0;
            t.x = de[i].x; t.y = de[i].y; t.z = de[i].z; t.seen = true;
            strncpy(t.stringID, de[i].stringID, sizeof(t.stringID) - 1);
            t.stringID[sizeof(t.stringID) - 1] = '\0';
            t.itemType = de[i].itemType; t.quantity = de[i].quantity; t.quality = de[i].quality;
            worldTrack_[k] = t;
            if (dumpWi) { char b[200]; _snprintf(b, sizeof(b) - 1,
                "[wi] DROP-CAP netId=%u sid='%s' qty=%u pos=%.2f,%.2f,%.2f (query-free)",
                t.netId, t.stringID, t.quantity, t.x, t.y, t.z);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
        }
    }

    // ---- Discovery 2: the spatial scan (best-effort) -----------------------
    {
        engine::WorldItemRaw raw[WORLD_ITEMS_MAX];
        unsigned int n = engine::captureWorldItems(gw, raw, WORLD_ITEMS_MAX, RADIUS);
        for (unsigned int i = 0; i < n; ++i) {
            if (isGearType(raw[i].itemType)) continue;
            if (!proxyObjs.empty() &&
                proxyObjs.count(engine::resolveObjectByHand(raw[i].hand)) != 0)
                continue; // peer-authored proxy - not ours to publish
            Key k; k.t = raw[i].hand[0]; k.c = raw[i].hand[1]; k.cs = raw[i].hand[2];
            k.i = raw[i].hand[3]; k.s = raw[i].hand[4];
            std::map<Key, WorldTrack>::iterator tit = worldTrack_.find(k);
            if (tit == worldTrack_.end()) {
                WorldTrack t; memset(&t, 0, sizeof(t));
                t.netId = nextWorldNetId_++; t.hash = 0; t.lastSendMs = 0;
                t.x = raw[i].x; t.y = raw[i].y; t.z = raw[i].z; t.seen = true;
                strncpy(t.stringID, raw[i].stringID, sizeof(t.stringID) - 1);
                t.stringID[sizeof(t.stringID) - 1] = '\0';
                t.itemType = raw[i].itemType; t.quantity = raw[i].quantity; t.quality = raw[i].quality;
                worldTrack_[k] = t;
            } else {
                // Refresh the description (a re-stack can change qty/quality).
                WorldTrack& tr = tit->second;
                strncpy(tr.stringID, raw[i].stringID, sizeof(tr.stringID) - 1);
                tr.stringID[sizeof(tr.stringID) - 1] = '\0';
                tr.itemType = raw[i].itemType; tr.quantity = raw[i].quantity; tr.quality = raw[i].quality;
            }
        }
    }

    // ---- Stream new/changed + HANDLE-based cull over every track -----------
    WorldItemEntry send[WORLD_ITEMS_MAX]; unsigned int ns = 0;
    u32 removed[256]; unsigned int nr = 0;
    for (std::map<Key, WorldTrack>::iterator it = worldTrack_.begin(); it != worldTrack_.end(); ) {
        WorldTrack& tr = it->second;
        unsigned int ihand[5] = { it->first.t, it->first.c, it->first.cs, it->first.i, it->first.s };
        float pos[3] = { tr.x, tr.y, tr.z };
        // Query-free liveness: is the real item still on the ground (not gone/picked-up)?
        if (!engine::groundItemLiveness(ihand, pos)) {
            // Baseline (save-native) tracks were never streamed, so the peer has
            // no proxy to remove - just drop our track. Streamed tracks emit a
            // remove so the peer despawns its proxy.
            if (!tr.baseline) { if (nr < 256) removed[nr++] = tr.netId; }
            if (dumpWi) { char b[112]; _snprintf(b, sizeof(b) - 1,
                "[wi] CULL netId=%u (gone/picked-up) baseline=%d", tr.netId, tr.baseline ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            worldTrack_.erase(it++);
            continue;
        }
        // Baseline save-natives never stream (both clients hold them identically).
        if (tr.baseline) { tr.x = pos[0]; tr.y = pos[1]; tr.z = pos[2]; ++it; continue; }
        bool sent = (tr.lastSendMs != 0);
        float dx = pos[0] - tr.x, dy = pos[1] - tr.y, dz = pos[2] - tr.z;
        bool moved = (dx*dx + dy*dy + dz*dz) > (POS_EPS * POS_EPS);
        u32 h = worldTrackHash(tr.stringID, tr.itemType, tr.quantity, tr.quality);
        bool changed = !sent || (tr.hash != h) || moved;
        bool periodic = sent && !changed && (now - tr.lastSendMs >= WI_RESEND_MS);
        if (changed || periodic) {
            if (ns < WORLD_ITEMS_MAX) {
                WorldItemEntry& e = send[ns++];
                e.netId = tr.netId;
                strncpy(e.stringID, tr.stringID, sizeof(e.stringID) - 1);
                e.stringID[sizeof(e.stringID) - 1] = '\0';
                e.itemType = tr.itemType;
                e.quantity = tr.quantity;
                e.quality  = tr.quality;
                e.x = pos[0]; e.y = pos[1]; e.z = pos[2];
                e.state = 0;
            }
            tr.hash = h; tr.lastSendMs = now;
            if (changed && dumpWi) {
                char b[200]; _snprintf(b, sizeof(b) - 1,
                    "[wi] SEND netId=%u sid='%s' qty=%u pos=%.2f,%.2f,%.2f hash=%u",
                    tr.netId, tr.stringID, tr.quantity, pos[0], pos[1], pos[2], h);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        tr.x = pos[0]; tr.y = pos[1]; tr.z = pos[2];
        ++it;
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
            // Which REAL Item*(s) just ENTERED this bag? The picked-up ground object is the
            // same handle (conservation), so the pointers present now but not last tick are
            // exactly the ones picked up. Correlating each to the ground copy WE tracked
            // recovers its shared (dropOwnerId, dropId) - the identity both clients agree on -
            // so the peer re-homes the EXACT instance rather than guessing FIFO-by-sid.
            std::set<void*> prevSet;
            {
                std::map<std::string, std::deque<void*> >::iterator pit = prevC.ptrs.find(ce->first);
                if (pit != prevC.ptrs.end())
                    for (std::deque<void*>::iterator q = pit->second.begin(); q != pit->second.end(); ++q)
                        prevSet.insert(*q);
            }
            std::deque<void*> added;
            {
                std::map<std::string, std::deque<void*> >::iterator cit = curPtrs.find(ce->first);
                if (cit != curPtrs.end())
                    for (std::deque<void*>::iterator q = cit->second.begin(); q != cit->second.end(); ++q)
                        if (prevSet.count(*q) == 0) added.push_back(*q);
            }
            std::deque<GroundWeapon>& q = groundedWeapons_[ce->first];
            for (int k = 0; k < inc; ++k) {
                WorldPickupPacket pkt; memset(&pkt, 0, sizeof(pkt));
                pkt.type = (u8)PKT_WORLD_PICKUP; pkt.ownerId = ownerId;
                pkt.pickupId = nextPickupId_++;
                pkt.oType = it->t; pkt.oContainer = it->c; pkt.oContainerSerial = it->cs;
                pkt.oIndex = it->i; pkt.oSerial = it->s;
                strncpy(pkt.stringID, ce->first.c_str(), sizeof(pkt.stringID) - 1);
                pkt.itemType = ce->second.itemType; pkt.quality = ce->second.quality;
                // Match a newly-arrived pointer to a tracked ground instance for its identity.
                bool matched = false;
                if (!added.empty()) {
                    void* got = added.front(); added.pop_front();
                    for (std::deque<GroundWeapon>::iterator g = q.begin(); g != q.end(); ++g) {
                        if (g->item == got) {
                            pkt.refDropOwnerId = g->dropOwnerId; pkt.refDropId = g->dropId;
                            q.erase(g); matched = true; break;
                        }
                    }
                }
                if (!matched && !q.empty()) { // couldn't pin the instance -> oldest same-sid copy
                    pkt.refDropOwnerId = q.front().dropOwnerId; pkt.refDropId = q.front().dropId;
                    q.pop_front();
                }
                net.queueWorldPickup(pkt);
                if (dumpWd) { char b[220]; _snprintf(b, sizeof(b) - 1,
                    "[wd] PICKUP id=%u sid='%s' owner=%u,%u,%u,%u,%u ref=%u/%u prev=%d now=%d trackedLeft=%u",
                    pkt.pickupId, pkt.stringID, it->t, it->c, it->cs, it->i, it->s,
                    pkt.refDropOwnerId, pkt.refDropId, prevCount, ce->second.count, (unsigned)q.size());
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
                // the owner-feet fallback - and it's query-free, so both clients agree. But a
                // town-dropped item frequently reports its transform as (0,0,0) the frame it
                // grounds; that sentinel must NOT clobber the good owner-feet fallback (else the
                // peer relocates its copy to world origin and it's invisible near the player).
                float dpos[3] = { pos[0], pos[1], pos[2] };
                if (di) { float ip[3]; if (engine::itemWorldPos(di, ip) &&
                          !(ip[0] == 0.0f && ip[1] == 0.0f && ip[2] == 0.0f)) {
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
                    GroundWeapon gw2; gw2.dropOwnerId = ownerId; gw2.dropId = pkt.dropId; gw2.item = di;
                    groundedWeapons_[pe->first].push_back(gw2);
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
        // Track the relocated REAL object under the drop's SHARED identity so a later PICKUP
        // intent naming (ownerId, dropId) re-homes this exact handle back into the owner's bag
        // (no spatial re-query, which fails in towns; no FIFO-by-sid guess between duplicates).
        if (moved > 0 && dropped) {
            GroundWeapon gw2; gw2.dropOwnerId = p.ownerId; gw2.dropId = p.dropId; gw2.item = dropped;
            groundedWeapons_[std::string(p.stringID)].push_back(gw2);
        }
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
        std::deque<GroundWeapon>& q = groundedWeapons_[std::string(p.stringID)];
        int moved = 0;
        std::deque<GroundWeapon>::iterator pick = q.end();
        if (p.refDropId != 0) {
            // Re-home the EXACT instance the picker named (both clients tracked it under this
            // (owner,id)). If we don't have it tracked, do NOT guess a same-sid copy - that is
            // the very mistake this identity fixes; a genuine peer copy will match.
            for (std::deque<GroundWeapon>::iterator g = q.begin(); g != q.end(); ++g)
                if (g->dropOwnerId == p.refDropOwnerId && g->dropId == p.refDropId) { pick = g; break; }
        } else if (!q.empty()) {
            pick = q.begin(); // legacy/no-identity: fall back to oldest same-sid copy
        }
        if (pick != q.end()) {
            void* item = pick->item;
            moved = engine::addItemPtrToInventory(gw, targetHand, item);
            if (moved) q.erase(pick); // re-homed; stop tracking it on the ground
        }
        char b[240]; _snprintf(b, sizeof(b) - 1,
            "[wd] PICKUP-APPLY id=%u sid='%s' owner=%u,%u,%u,%u,%u ref=%u/%u moved=%d trackedLeft=%u",
            p.pickupId, p.stringID, p.oType, p.oContainer, p.oContainerSerial, p.oIndex,
            p.oSerial, p.refDropOwnerId, p.refDropId, moved, (unsigned)q.size());
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


} // namespace coop
