// ReplicatorPublish.cpp - the host/owner SEND half (monolith split from
// Replicator.cpp, 2026-07-12): publishOwned (owned-squad + near-band NPC
// entity states, the 2 Hz mid-band slice, combat-intent capture incl. the
// canonical-hand translation) and publishNpcCensus (protocol 36 existence
// broadcast).
//
// Shared hubs: writes ownHands_/tabRank_ (per-tick owned set), midCursor_;
// reads canonicalOf_ (stamped by the drive TU), censusHands_.
// Must NOT: change any log string - log phrasing is the API consumed by the
// PowerShell oracles (see resources/CODE_MAP.md, log-tag index).

#include "ReplicatorUtil.h"

namespace coop {

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
    // Full squad roster (own + peer) for the trade veto's owner classifier: every
    // captured member, before the ownership partition below decides which we own.
    allSquad_.clear();
    for (unsigned int i = 0; i < nSquad; ++i) allSquad_.insert(keyOf(raw[i]));
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
        // Drive-exclusion (Phase 1b recruit membership): a body we are DRIVING
        // from the peer's stream is peer-owned regardless of the local tab
        // rank/hand it sits in. insertPeerMember re-containers a recruit into
        // OUR squad, giving it a NEW local index the owner's hand-pin (keyed on
        // the OWNER's reported hand) does not cover - without this it escapes the
        // pin, publishes as owned, and the owner mints a DUPLICATE proxy of its
        // own recruit (recruit_sync run 093032: host squad 6 vs join 8).
        // canonicalOf_ is stamped every drive tick by Character*, so it tracks
        // the body across index drift; the body was already driven as a
        // proxy/re-keyed copy BEFORE it became a member, so there is no lag. Our
        // OWN recruits are pinOwned_ (never driven) and keep publishing.
        if (owned && !pinOwned_.count(hk)) {
            Character* bc = engine::resolveCharByHand(
                raw[i].hIndex, raw[i].hSerial, raw[i].hType,
                raw[i].hContainer, raw[i].hContainerSerial);
            if (bc && canonicalOf_.find(bc) != canonicalOf_.end())
                owned = false;
        }
        if (!owned) continue;
        buf[n++] = raw[i];
        ownHands_.insert(hk);
    }
    // Jail put-to-work desync spike (KENSHICOOP_JAIL_PROBE, read-only): the
    // OWNED view of any captive body (the join's real, authoritative PC while it
    // is jailed). Correlate side=own here against side=drv from the host's
    // driven copy (applyTargets) to pin the brief cage-exit/re-cage twitch.
    if (jailProbe_) {
        static std::map<Key, unsigned long> s_ownJailMs;
        unsigned long jNow = nowMs();
        for (unsigned int i = 0; i < n; ++i) {
            const EntityState& e = buf[i];
            Character* oc = engine::resolveCharByHand(
                e.hIndex, e.hSerial, e.hType, e.hContainer, e.hContainerSerial);
            if (!oc) continue;
            engine::FurnitureRead fr;
            engine::ShackleRead sr;
            bool haveF = engine::readFurniture(oc, &fr) && fr.valid;
            bool haveS = engine::readShackle(oc, &sr) && sr.valid;
            int kind = haveF ? fr.kind : 0;
            bool chained = haveS ? sr.chained : false;
            if (kind == 0 && !chained) continue;   // only captive bodies
            Key k = keyOf(e);
            std::map<Key, unsigned long>::iterator jt = s_ownJailMs.find(k);
            if (jt != s_ownJailMs.end() && (jNow - jt->second) < 250) continue;
            s_ownJailMs[k] = jNow;
            int slave = engine::readSlaveState(oc);
            char b[224];
            _snprintf(b, sizeof(b) - 1,
                      "[jail] STATE side=own hand=%u,%u kind=%d chained=%d "
                      "slaveOwner=%u,%u isSlave=%d task=%u raw=%u pos=%.1f,%.1f,%.1f mv=%d",
                      e.hIndex, e.hSerial, kind, chained ? 1 : 0,
                      haveS ? sr.owner[3] : 0u, haveS ? sr.owner[4] : 0u,
                      slave, e.task, e.rawTask, e.x, e.y, e.z, e.cMoving ? 1 : 0);
            b[sizeof(b) - 1] = '\0';
            coop::logLine(b);
        }
    }
    // Combat-subject CANONICAL translation (join-initiated town combat, run
    // 20260712_180913): the capture reads the target's LOCAL hand, but a body
    // this client is DRIVING can live in a local runtime container that the
    // peer has never heard of (the engine separateIntoMyOwnSquad's a town NPC
    // when a fight starts; a minted proxy never had the peer's hand at all).
    // Publish the SUBJECT under the key the peer streams it by (canonicalOf_,
    // stamped every drive tick) or the peer's applyCombat resolves nothing
    // (r=1 forever) and the fight renders on one client only.
    for (unsigned int i = 0; i < n; ++i) {
        EntityState& e = buf[i];
        if (!coop::taskIsCombat(e.task)) continue;
        Character* tc = engine::resolveCharByHand(e.sIndex, e.sSerial, e.sType,
                                                  e.sContainer, e.sContainerSerial);
        if (!tc) continue;
        std::map<Character*, Key>::const_iterator cit = canonicalOf_.find(tc);
        if (cit == canonicalOf_.end()) continue;
        const Key& ck = cit->second;
        if (ck.i == e.sIndex && ck.s == e.sSerial && ck.t == e.sType &&
            ck.c == e.sContainer && ck.cs == e.sContainerSerial)
            continue;
        char b[176]; _snprintf(b, sizeof(b) - 1,
            "[combat] CAP xlate hand=%u,%u tgt local=%u,%u,%u,%u,%u -> wire=%u,%u,%u,%u,%u",
            e.hIndex, e.hSerial,
            e.sType, e.sContainer, e.sContainerSerial, e.sIndex, e.sSerial,
            ck.t, ck.c, ck.cs, ck.i, ck.s);
        b[sizeof(b) - 1] = '\0';
        e.sType = ck.t; e.sContainer = ck.c; e.sContainerSerial = ck.cs;
        e.sIndex = ck.i; e.sSerial = ck.s;
        // Throttle the log per canonical victim (the rewrite itself runs every
        // publish frame).
        unsigned long xNow = nowMs();
        std::map<Key, unsigned long>::iterator xt = combatCapMs_.find(ck);
        if (xt == combatCapMs_.end() || (xNow - xt->second) >= 2000) {
            combatCapMs_[ck] = xNow;
            coop::logLine(b);
        }
    }
    // Capture-side combat visibility (join-initiated town combat investigation):
    // the receive side logs [combat] order when an intent ARRIVES, but nothing
    // ever recorded what this client SENDS - a fight that never crosses is
    // indistinguishable from one never captured. One throttled line per owned
    // combatant while its streamed task is a combat stance.
    {
        unsigned long capNow = nowMs();
        for (unsigned int i = 0; i < n; ++i) {
            const EntityState& e = buf[i];
            if (!coop::taskIsCombat(e.task)) continue;
            Key k = keyOf(e);
            std::map<Key, unsigned long>::iterator ct = combatCapMs_.find(k);
            if (ct != combatCapMs_.end() && (capNow - ct->second) < 2000) continue;
            combatCapMs_[k] = capNow;
            char b[176]; _snprintf(b, sizeof(b) - 1,
                "[combat] CAP hand=%u,%u task=%u tgt=%u,%u,%u,%u,%u",
                e.hIndex, e.hSerial, (unsigned)e.task,
                e.sType, e.sContainer, e.sContainerSerial, e.sIndex, e.sSerial);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
    // Host also streams nearby world NPCs (host-authoritative world). The join leaves
    // streamNpcs_ off, so on the join this publishes ONLY its owned squad subset.
    if (streamNpcs_ && n < MAX_PUBLISH)
        n += engine::captureNpcs(gw, buf + n, MAX_PUBLISH - n);
    // Phase 2 mid-band tier (host): append a rotating slice of the census-walk
    // NPCs beyond the stream bubble (midBand_, nearest-first, rebuilt at 1 Hz
    // by publishNpcCensus). Quota = |midBand|/10 puts each mid NPC in ~1 of
    // every 10 snapshots; the net thread samples snapshots at 20 Hz, so each
    // NPC hits the wire at ~2 Hz aggregate - real movement between census
    // beats instead of a frozen local sim (the "zombie NPC" report). The
    // wrap-around phase bump (midRot_) keeps a fixed frame-rate/net-tick
    // ratio from aliasing the same slice positions into every sampled
    // snapshot. Hands are resolved fresh each frame: a despawn since the
    // census walk degrades to a skip, and the near tier is deduped by hand
    // (an NPC walking into the bubble is already in buf at 20 Hz).
    if (streamNpcs_ && !midBand_.empty() && n < MAX_PUBLISH) {
        const unsigned int nearEnd = n;
        unsigned int sz = (unsigned int)midBand_.size();
        unsigned int quota = (sz + 9) / 10;
        if (quota > 16) quota = 16;
        // Advance the slice on the NET-TICK cadence (50 ms), not per frame:
        // publishOwned runs every render frame but the net thread samples
        // the latest snapshot only at 20 Hz, so per-frame rotation dropped
        // 2/3 of the slices on the floor at 75 fps and starved entries for
        // whole rotations (run 103044: starve=5..10, driven bodies flapping
        // out of the driven set). A slice that persists >= one net tick is
        // guaranteed on the wire: quota/10th of the list every 50 ms = each
        // mid NPC at ~2 Hz, deterministically.
        unsigned long nowPub = nowMs();
        if (midSliceMs_ == 0 || (nowPub - midSliceMs_) >= 50) {
            midSliceMs_ = nowPub;
            midCursor_ += quota; // linear cursor, index mod size below
        }
        unsigned int tried = 0, added = 0;
        while (tried < sz && added < quota && n < MAX_PUBLISH) {
            const Key mk = midBand_[(midCursor_ + tried) % sz].k;
            ++tried;
            bool dup = false;
            for (unsigned int i = 0; i < nearEnd && !dup; ++i)
                dup = buf[i].hIndex == mk.i && buf[i].hSerial == mk.s;
            if (dup) continue;
            if (engine::captureNpcByHand(gw, mk.i, mk.s, mk.t, mk.c, mk.cs,
                                         &buf[n])) {
                // Movers only (Phase 2 refinement, run 112835): a stationary
                // far NPC is covered by the 1 Hz census position (park
                // fallback) - streaming it just fed the join a body to drive
                // and starved Kenshi's character-update budget town-wide.
                // Skipping WITHOUT consuming quota lets the scan reach past
                // parked bodies, so a lone approaching raid effectively
                // streams at near-full rate while a busy field shares ~2 Hz.
                if (buf[n].cMoving == 0 && buf[n].cSpeed <= 0.25f) continue;
                ++n;
                ++added;
            }
        }
    }
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
            // Chained/pole prisoner (protocol 41) rides this pipeline as kind=3
            // (readFurniture puts the OWNER hand in fr.furn). Gated by chainSync_
            // so it can be disabled without losing bed/cage sync.
            int curKind = (cur & BODY_IN_BED) ? 1 :
                          ((cur & BODY_IN_CAGE) ? 2 :
                          ((chainSync_ && (cur & BODY_CHAINED)) ? 3 : 0));
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

// world_parity roster row: legacy WNPC schema (hand/pos/cls/name - the
// travel_parity parser keys on those) with the parity fields APPENDED:
//   task=   reproducible pose/combat task enum (same vocabulary as MEMBER/RECV)
//   pelvis= Bip01 height off the rendered skeleton (seated/downed vs standing)
//   mv=     locomotion bit (cMoving, or speed > walk threshold)
// cls=pc rows carry the player characters, which the NPC dumps exclude
// (isPlayerSquad skip) - the class where a diverged host-PC hid from every gate.
void Replicator::emitWnpcRow(Character* c, const EntityState& st, const char* cls) {
    char nm[40]; engine::charName(c, nm, sizeof(nm));
    float pelvis = -1.0f; int idle = -1, crouch = -1, ptask = (int)st.task;
    if (c) engine::readPoseState(c, &pelvis, &idle, &crouch, &ptask);
    int mv = (st.cMoving || st.cSpeed > 0.25f) ? 1 : 0;
    char r[256];
    _snprintf(r, sizeof(r) - 1,
              "SCENARIO WNPC hand=%u,%u,%u,%u,%u pos=%.1f,%.1f,%.1f "
              "cls=%s name='%s' task=%u pelvis=%.2f mv=%d",
              st.hIndex, st.hSerial, st.hType,
              st.hContainer, st.hContainerSerial,
              st.x, st.y, st.z, cls, nm, (unsigned int)st.task, pelvis, mv);
    r[sizeof(r) - 1] = '\0'; coop::logLine(r);
}

void Replicator::emitPcRows(GameWorld* gw) {
    const unsigned int MAX_PC = 32;
    static EntityState pcs[MAX_PC]; // main-thread only
    unsigned int nPc = engine::captureSquad(gw, /*leaderOnly*/ false, pcs, MAX_PC);
    for (unsigned int i = 0; i < nPc; ++i)
        emitWnpcRow(engine::resolve(pcs[i]), pcs[i], "pc");
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

    // Phase 2 mid-band tier: rebuild the round-robin list from this census
    // walk. Everything beyond the stream bubble's KEEP band belongs to the
    // mid tier; nearest-first so a MAX_PUBLISH squeeze drops the farthest.
    // Distance is to the closest interest ANCHOR (protocol 43: tab leaders +
    // local camera + peer camera hint) - the same anchors the stream bubble
    // uses, so a camera-watched far NPC gets a mid-band drive slot too.
    {
        const float MID_NEAR_EDGE = 260.0f; // captureNpcs' NPC_CAPTURE_KEEP
        float anchors[12];
        unsigned int nAnchor = engine::interestAnchors(gw, anchors);
        midBand_.clear();
        for (unsigned int i = 0; i < n; ++i) {
            float best = -1.0f;
            for (unsigned int s = 0; s < nAnchor; ++s) {
                float d = dist3(states[i].x, states[i].y, states[i].z,
                                anchors[s * 3 + 0], anchors[s * 3 + 1],
                                anchors[s * 3 + 2]);
                if (best < 0.0f || d < best) best = d;
            }
            if (best < 0.0f || best <= MID_NEAR_EDGE) continue; // near tier
            MidBandEntry e;
            e.k.t  = states[i].hType;
            e.k.c  = states[i].hContainer;
            e.k.cs = states[i].hContainerSerial;
            e.k.i  = states[i].hIndex;
            e.k.s  = states[i].hSerial;
            e.dist = best;
            midBand_.push_back(e);
        }
        std::sort(midBand_.begin(), midBand_.end());
        // Nearest-first BUDGET: driving every census NPC measurably slowed
        // the join's sim (run 111445: slewSkip 7949 vs baseline ~1-2.6k, and
        // the sim-tick/render-frame ratio degraded enough to fail the
        // smoothness gate on bodies that WERE tracking). The nearest ~48
        // cover everything the join player can meaningfully watch; the far
        // remainder keeps the census-park fallback it always had.
        const unsigned int MID_BAND_MAX = 48;
        if (midBand_.size() > MID_BAND_MAX) midBand_.resize(MID_BAND_MAX);
        if (midCursor_ >= midBand_.size()) midCursor_ = 0;
    }

    // travel_parity worldstate rows (host side): dump every census NPC on a
    // 5 s cadence so Test-TravelParity can cross-compare the two worlds'
    // populations. cls=host marks the row as the host's authoritative view.
    if (auditRows_) {
        static unsigned long rowsMs = 0; // main-thread only
        if (rowsMs == 0 || (now - rowsMs) >= 5000) {
            rowsMs = now;
            char w[64];
            _snprintf(w, sizeof(w) - 1, "SCENARIO WORLD n=%u cls=host", n);
            w[sizeof(w) - 1] = '\0'; coop::logLine(w);
            for (unsigned int i = 0; i < n; ++i)
                emitWnpcRow(chars[i], states[i], "host");
            // world_parity: the player characters, excluded from the census
            // walk, get their own cls=pc rows so PC divergence is judged too.
            emitPcRows(gw);
        }
    }
    // ~10 s cadence log so free-play sessions show the census breathing
    // without 1 Hz spam.
    static unsigned long logTick = 0;
    if ((now - logTick) > 10000) {
        logTick = now;
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[census] sent n=%u radius=%.0f mid=%u",
                  n, censusRadius_, (unsigned)midBand_.size());
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

void Replicator::syncCamHint(GameWorld* gw, Inbound& in, NetLink& net, u32 ownerId,
                             bool isHost) {
    if (!gw) return;
    unsigned long now = nowMs();

    // Both sides: publish the LOCAL camera center to the engine's interest
    // layer (never crosses the wire - each client reads its own camera).
    float local[3];
    bool haveLocal = engine::cameraCenter(gw, local);
    engine::setLocalCamAnchor(haveLocal, local[0], local[1], local[2]);

    if (!isHost) {
        // JOIN: ship the camera center to the host at ~1 Hz (unreliable,
        // latest wins - a lost hint is replaced a second later).
        if (haveLocal && (camHintSendMs_ == 0 || (now - camHintSendMs_) >= 1000)) {
            camHintSendMs_ = now;
            CamHintPacket p;
            p.type = (u8)PKT_CAM_HINT;
            p.ownerId = ownerId;
            p.x = local[0]; p.y = local[1]; p.z = local[2];
            net.queueCamHint(p);
        }
        return;
    }

    // HOST: drain received hints (latest wins) into peerCam_ + staleness
    // stamp, and publish a FRESH hint to the engine's interest layer. A
    // stale hint (silent join > 3 s: alt-tabbed, loading, disconnecting)
    // drops out of the anchor set rather than pinning interest forever.
    std::deque<InboundCamHint> got;
    in.drainCamHints(got);
    if (!got.empty()) {
        const CamHintPacket& p = got.back().pkt;
        peerCam_[0] = p.x; peerCam_[1] = p.y; peerCam_[2] = p.z;
        peerCamMs_ = now;
        static unsigned long logTick = 0; // main-thread only
        if (logTick == 0 || (now - logTick) >= 5000) {
            logTick = now;
            char b[96];
            _snprintf(b, sizeof(b) - 1, "[cam] hint recv=%.1f,%.1f,%.1f",
                      p.x, p.y, p.z);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
    bool fresh = (peerCamMs_ != 0) && (now - peerCamMs_) <= 3000;
    engine::setPeerCamHint(fresh, peerCam_[0], peerCam_[1], peerCam_[2]);
}


} // namespace coop
