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
    localId_ = ownerId;   // the drive path reads it; see the host-side guard
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
    // ...and decide who OWNS each tab. The latch alone was not enough: an
    // appended tab's rank is outside ownRanks_ on both clients, so it belonged
    // to neither of them until this ran.
    decideTabs(raw, nSquad, ctnrs);
    ownHands_.clear();
    // Full squad roster (own + peer) for the trade veto's owner classifier: every
    // captured member, before the ownership partition below decides which we own.
    allSquad_.clear();
    for (unsigned int i = 0; i < nSquad; ++i) allSquad_.insert(keyOf(raw[i]));
    unsigned int n = 0;
    for (unsigned int i = 0; i < nSquad && n < MAX_PUBLISH; ++i) {
        std::pair<u32, u32> key(raw[i].hContainer, raw[i].hContainerSerial);
        unsigned int rank = tabRankFor(key, ctnrs);
        // The per-tab verdict (decideTabs). For the save's own tabs this is the
        // historical rank rule; for a tab created mid-session it is the side that
        // authored it, or the host. Empty ownRanks_ (never configured) still falls
        // back to the first tab, so a missing setOwnRanks never makes us stream
        // every tab or nothing.
        bool owned = ownsTab(key, rank);
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
    // Build-site subject translation (2026-08-01 construction animation). Same
    // shape as the combat rewrite above and for the same underlying reason: the
    // capture reads the site's LOCAL hand, but a construction site is created at
    // RUNTIME so that hand means nothing on the peer. Publish it in the PLACER's
    // key space (our own hand if we placed it, the placer's key if we minted it)
    // so the peer can map it back to its own copy. A subject that is not a placed
    // site at all is left alone - a save-resident building resolves directly.
    for (unsigned int i = 0; i < n; ++i) {
        EntityState& e = buf[i];
        if (!engine::isBuildSiteTask((int)e.task)) continue;
        Key lk; lk.t = e.sType; lk.c = e.sContainer; lk.cs = e.sContainerSerial;
        lk.i = e.sIndex; lk.s = e.sSerial;
        Key wk;
        if (!buildKeyForLocalHand(lk, wk)) continue;
        if (wk.t == lk.t && wk.c == lk.c && wk.cs == lk.cs &&
            wk.i == lk.i && wk.s == lk.s)
            continue; // we placed it: local hand already is the wire key
        char b[192]; _snprintf(b, sizeof(b) - 1,
            "[build] POSE-CAP xlate hand=%u,%u task=%u site local=%u.%u.%u.%u.%u "
            "-> wire=%u.%u.%u.%u.%u",
            e.hIndex, e.hSerial, (unsigned)e.task,
            lk.t, lk.c, lk.cs, lk.i, lk.s, wk.t, wk.c, wk.cs, wk.i, wk.s);
        b[sizeof(b) - 1] = '\0';
        e.sType = wk.t; e.sContainer = wk.c; e.sContainerSerial = wk.cs;
        e.sIndex = wk.i; e.sSerial = wk.s;
        // Throttle per site (the rewrite itself runs every publish frame).
        unsigned long pNow = nowMs();
        std::map<Key, unsigned long>::iterator pt = buildPoseCapMs_.find(wk);
        if (pt == buildPoseCapMs_.end() || (pNow - pt->second) >= 2000) {
            buildPoseCapMs_[wk] = pNow;
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
    // Under presence authority BOTH sides stream, so each must keep to the cells
    // it actually owns or the two of us drive the same bodies.
    if (streamNpcs_ && n < MAX_PUBLISH) {
        unsigned int got = engine::captureNpcs(gw, buf + n, MAX_PUBLISH - n);
        // The attention gate lives HERE, not on the census. Streaming is the
        // expensive half - a transform per body per tick - and it is the half
        // that is genuinely pointless when the peer has no camera or squad near
        // the body: nobody can see the difference between a driven body and a
        // still one they are not looking at. Note this gates only the NPC
        // capture; our own squad (already in buf[0..n)) streams unconditionally,
        // because those are the peer's window into what we are DOING and it must
        // never depend on where their camera happens to point.
        if (cellAuth_) {
            float peerAnch[12];
            unsigned int nPeerAnch = peerAnchors(gw, peerAnch);
            unsigned long cNow = nowMs();
            unsigned int kept = 0;
            for (unsigned int i = 0; i < got; ++i) {
                if (!weAuthor(gw, ownerId, buf[n + i].x, buf[n + i].z)) continue;
                // The peer's census is the other half of incumbent-holds. weAuthor
                // asks a question about OUR copy's position, and two copies of one
                // fighting NPC drift apart far enough to answer it differently on
                // each side (163 u across a cell boundary, measured), so a cell
                // verdict alone cannot keep us to one writer. A census row is the
                // peer stating they author that body - and since the census now
                // covers everything they author, its silence is meaningful too.
                // Believing them costs nothing if they are wrong (the body simply
                // goes unpublished for a beat) where disbelieving them costs two
                // writers on one Character.
                // Only while it is current, and only its PRESENCE is trusted: a
                // census we have stopped receiving says nothing about who authors
                // what now, and reading its silence as "they gave this up" would
                // have us start writing bodies they are still driving.
                if (censusRecvMs_ != 0 && (cNow - censusRecvMs_) <= 5000 &&
                    censusHands_.find(keyOf(buf[n + i])) != censusHands_.end())
                    continue;
                // Echo guard (2026-08-08, the dual-drive fix). captureNpcs
                // returns every local NPC in the bubble, and a MINTED PROXY is
                // one - so a body that exists only because the host streams it
                // gets captured and published straight back. The host then has
                // its own body's state arriving from the join, drives its copy
                // from it, and both clients end up in the other's driven set:
                // the two bodies escape_cohesion caught (Tako, a Holy Sentinel)
                // were both cases where the join had no native body at all
                // (lifecycle: DISCOVERED reason=census-miss, "no local body").
                //
                // Anything the peer is streaming to us right now is, by that
                // fact, theirs to write - whatever our cell map says. This
                // subsumes the census hold above with a 20 Hz signal instead of
                // a 1 Hz one, but does not replace it: the census also speaks
                // for bodies too far out to stream.
                //
                // Only the JOIN yields, matching rebuildClaimedCells' host-wins
                // rule. A symmetric guard would let both sides go quiet at once
                // on a contested body and then both resume when the streams went
                // stale, oscillating; with a fixed winner the loop just stops.
                if (ownerId != (u32)CELL_OWNER_HOST &&
                    peerStreamFresh(keyOf(buf[n + i]), cNow)) {
                    ++cellYields_;
                    if (cellYieldLogged_.insert(keyOf(buf[n + i])).second) {
                        const Key yk = keyOf(buf[n + i]);
                        char yb[192]; _snprintf(yb, sizeof(yb) - 1,
                            "[cell] yield hand=%u,%u,%u,%u,%u pos=%.0f,%.0f,%.0f"
                            " (host streams it)",
                            yk.t, yk.c, yk.cs, yk.i, yk.s,
                            buf[n + i].x, buf[n + i].y, buf[n + i].z);
                        yb[sizeof(yb) - 1] = '\0'; coop::logLine(yb);
                    }
                    continue;
                }
                if (nPeerAnch > 0 &&
                    !observedByPeer(keyOf(buf[n + i]), peerAnch, nPeerAnch,
                                    buf[n + i].x, buf[n + i].y, buf[n + i].z))
                    continue;
                if (kept != i) buf[n + kept] = buf[n + i];
                ++kept;
            }
            got = kept;
        }
        n += got;
    }
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
    // Carrier promotion: a body CARRYING someone owns that PASSENGER's
    // transform on both clients, so every unit the carrier's copy trails is
    // inherited by the passenger - and the passenger can be the peer's own
    // player character. The ~2 Hz mid rotation below is nowhere near enough for
    // that: on the camp prison save the host's Holy Sentinels arrest the
    // join-owned PC and haul it across town, and the carrier's copy trailed
    // 361-581 u (snap srcVel=0.0 against a local cSpeed of 79 - a stale mid
    // ring, not a drive failure) while the PC's cross-client gap equalled the
    // carrier's to within 0.1 u sample for sample (run 20260806_100102). Worse,
    // the divergence OUTLIVES the haul: each side drops the body at its own
    // carrier's feet, leaving a 148 u split the drive cannot close while the
    // body is furniture-anchored. So stream carriers at the full near-band
    // cadence - no quota, no mover gate. Carriers are a handful of guards at
    // most, and MAX_PUBLISH still bounds the pass.
    //
    // Fast movers are promoted here too, for the same reason and by the same
    // rule: what a body needs from the wire is set by how fast it moves, not by
    // how far away it is. Between two samples of the ~2 Hz rotation a running
    // body covers more ground than the snap band is wide, so the catch-up walk
    // can never close the gap - the drive teleports, waits out
    // NPC_SNAP_COOL_MS, and teleports again. Each of those snaps is individually
    // correct; the repetition is the artefact. mint_aggro measures it plainly:
    // a single charging hand snapped 9-15 times in 52 s at a median drift of
    // 87-196 u, every snap classified "chase". Nearest-first and capped, so a
    // stampede cannot crowd out the near band or hand the peer a whole field to
    // drive at 20 Hz (the sim-cost lesson behind MID_BAND_MAX).
    if (streamNpcs_ && !midBand_.empty() && n < MAX_PUBLISH) {
        // Where to put the bar, measured rather than guessed: the cSpeed of the
        // bodies that actually needed a mid-tier snap on camp_approach clusters
        // at 14-16 with a tail to 80, while the strollers that never needed one
        // sit at 4-10. 12 takes the cluster and leaves the strollers on the
        // rotation, which matters because promotion is not free - every row here
        // is a row in every snapshot, and at 8 the cap saturated on a whole town.
        const float        MID_FAST_SPEED = 12.0f;
        // A hard ceiling on how much of the mid band can buy its way to the
        // near-band rate, because the cost lands on the PEER's sim: at 16 a
        // busy camp promoted the full quota on every sweep and the join's
        // driven bodies started freezing outright (zeroFrac 0.16 -> 0.73,
        // near-tier snaps 6 -> 74) - the same budget lesson as MID_BAND_MAX,
        // arrived at from the other direction. Nearest-first, so what fits is
        // what the peer is closest to.
        // KENSHICOOP_MID_FAST overrides it, and 0 turns promotion off - the
        // control arm for measuring what this pass is worth on a given scene.
        static int fastCap = -1;
        if (fastCap < 0) {
            const char* e = getenv("KENSHICOOP_MID_FAST");
            fastCap = (e && e[0]) ? atoi(e) : 8;
            if (fastCap < 0) fastCap = 0;
        }
        const unsigned int MID_FAST_MAX   = (unsigned int)fastCap;
        // ...and a distance ceiling, which is the other half of affording it. A
        // runner 1500 u out is not a chase anyone can see; it is just a body the
        // census will place correctly a second from now. Only inside this ring
        // does the difference between 2 Hz and 20 Hz show on the peer's screen,
        // and confining the spend there is what keeps a busy camp - which always
        // has more than eight bodies running somewhere - from paying for all of
        // them. The mid band starts at MID_NEAR_EDGE (260 u).
        const float MID_FAST_EDGE = 700.0f;
        const unsigned int nearEnd0 = n;
        unsigned int szc = (unsigned int)midBand_.size();
        unsigned int nFast = 0;
        for (unsigned int i = 0; i < szc && n < MAX_PUBLISH; ++i) {
            const Key mk = midBand_[i].k;
            bool dup = false;
            for (unsigned int j = 0; j < nearEnd0 && !dup; ++j)
                dup = buf[j].hIndex == mk.i && buf[j].hSerial == mk.s;
            if (dup) continue;
            // captureOne stamps TASK_CARRY_BODY whenever the body carries, so
            // the captured row identifies a carrier without a second read. A
            // non-carrier just leaves buf[n] to be overwritten by the next probe.
            if (!engine::captureNpcByHand(gw, mk.i, mk.s, mk.t, mk.c, mk.cs,
                                          &buf[n]))
                continue;
            // Re-check authorship against the position we just captured.
            // midBand_ membership was filtered by weAuthor when publishNpcCensus
            // last rebuilt the list, but that is 1 Hz: across a claim handover
            // the stale list keeps streaming bodies in a cell that is no longer
            // ours for up to a second, which is precisely the window the
            // dual-drive bug lives in. The row is in hand, so the check is free.
            if (cellAuth_ && !weAuthor(gw, ownerId, buf[n].x, buf[n].z)) continue;
            bool promote = coop::taskIsCarry(buf[n].task);
            if (!promote && nFast < MID_FAST_MAX &&
                midBand_[i].dist <= MID_FAST_EDGE && buf[n].cMoving != 0 &&
                buf[n].cSpeed >= MID_FAST_SPEED) {
                promote = true; ++nFast;
            }
            if (!promote) continue;
            ++n;
        }
        midFastPromoted_ = nFast;
    }
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
                // Same 1 Hz staleness as the promotion loop above: re-check
                // authorship against the freshly captured position.
                if (cellAuth_ && !weAuthor(gw, ownerId, buf[n].x, buf[n].z)) continue;
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
            // Protocol 53: the KO/REVIVE edges are computed on "down and NOT
            // crawling". A crippled body sets BODY_DOWN while fully conscious
            // (measured: bs 0->1033, unc=0), so on plain bodyIsDown losing a leg
            // emitted EVT_KNOCKOUT and the peer latched KO permanently - which
            // pins the body down for good, since the recovery edge (PS_KO ->
            // PS_CRIPPLED) is down->down and emitted no REVIVE to clear it.
            // With the carve-out all four transitions read correctly: upright ->
            // crawl and crawl -> upright are non-events, crawl -> PS_KO is a real
            // KNOCKOUT, and PS_KO -> crawl is a REVIVE (it regained consciousness).
            bool wasDown = bodyDownNotCrawling(prev), isDownNow = bodyDownNotCrawling(cur);
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
        // Furniture occupancy: protocol 58 sends Join intent / Host canonical
        // state on BODY_IN_BED/BODY_IN_CAGE transitions, same scope as carry
        // (owned members + host-streamed world NPCs). The furniture HAND is not
        // in the stream (an unconscious occupant has no task subject), so it is
        // read off the LOCAL character (inWhat) at the ENTER edge and remembered
        // in HostBody for the matching EXIT. Scoped away from CONSCIOUS bed
        // poses (USE_BED / USE_BED_ORDER / SLEEP_ON_FLOOR): those stream their
        // TASK and the peer's copy walks in via the validated L3 fixture-pose
        // path (bed_pose) - an ENTER event would teleport it in and fight that.
        const Key furnitureOccKey = keyOf(e);
        std::map<Key, FurnitureRow>::iterator furnitureRowIt =
            furnitureRows_.find(furnitureOccKey);
        bool furnitureCorrectionPending = !hostRole_ &&
            furnitureRowIt != furnitureRows_.end() &&
            furnitureRowIt->second.applyPending;
        if (furnSync_ && carryAuthor && !furnitureCorrectionPending &&
            !engine::taskIsBedPose((int)e.task)) {
            // Chained/pole prisoner (protocol 41) rides this pipeline as kind=3
            // (readFurniture puts the OWNER hand in fr.furn). Gated by chainSync_
            // so it can be disabled without losing bed/cage sync.
            int curKind = (cur & BODY_IN_BED) ? 1 :
                          ((cur & BODY_IN_CAGE) ? 2 :
                          ((chainSync_ && (cur & BODY_CHAINED)) ? 3 : 0));
            if (curKind != hb.furnKind) {
                const Key& occKey = furnitureOccKey;
                if (hb.furnKind != 0) {
                    if (hb.furnKind == 1 || hb.furnKind == 2) {
                        // Protocol 58: the Host publishes fact; the Join asks.
                        if (hostRole_)
                            queueFurnitureState(net, ownerId, occKey, false,
                                                hb.furnKind, hb.furn, 0, 0);
                        else
                            queueFurnitureIntent(net, ownerId, occKey, false,
                                                 hb.furnKind, hb.furn, nowPub);
                    } else {
                        // Chained/pole prisoners remain on protocol 41's reliable
                        // event path, independent of the bed/cage authority change.
                        EventPacket ev; memset(&ev, 0, sizeof(ev));
                        ev.type = (u8)PKT_EVENT; ev.event = (u8)EVT_EXIT_FURNITURE;
                        ev.ownerId = ownerId; ev.eventId = nextEventId_++;
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
                    }
                    hb.furnKind = 0;
                    for (int fi = 0; fi < 5; ++fi) hb.furn[fi] = 0;
                }
                if (curKind != 0) {
                    // Enter edge: the local occupant knows WHICH furniture (inWhat).
                    // An unreadable hand this frame leaves furnKind 0 so the edge
                    // re-attempts next publish (the bit is still streaming).
                    engine::FurnitureRead fr;
                    Character* oc = engine::resolve(e);
                    if (oc && engine::readFurniture(oc, &fr) && fr.valid &&
                        fr.kind == curKind) {
                        if (curKind == 1 || curKind == 2) {
                            if (hostRole_)
                                queueFurnitureState(net, ownerId, occKey, true,
                                                    curKind, fr.furn, 0, 0);
                            else
                                queueFurnitureIntent(net, ownerId, occKey, true,
                                                     curKind, fr.furn, nowPub);
                        } else {
                            EventPacket ev; memset(&ev, 0, sizeof(ev));
                            ev.type = (u8)PKT_EVENT; ev.event = (u8)EVT_ENTER_FURNITURE;
                            ev.ownerId = ownerId; ev.eventId = nextEventId_++;
                            ev.sType = e.hType; ev.sContainer = e.hContainer;
                            ev.sContainerSerial = e.hContainerSerial;
                            ev.sIndex = e.hIndex; ev.sSerial = e.hSerial;
                            ev.aType = fr.furn[0]; ev.aContainer = fr.furn[1];
                            ev.aContainerSerial = fr.furn[2];
                            ev.aIndex = fr.furn[3]; ev.aSerial = fr.furn[4];
                            ev.arg = (f32)curKind;
                            net.queueEvent(ev);
                            char b[176]; _snprintf(b, sizeof(b) - 1,
                                "[furn] SEND ENTER id=%u occ=%u,%u furn=%u,%u kind=%d",
                                ev.eventId, e.hIndex, e.hSerial, fr.furn[3], fr.furn[4],
                                curKind);
                            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                        }
                        hb.furnKind = curKind;
                        for (int fi = 0; fi < 5; ++fi) hb.furn[fi] = fr.furn[fi];
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
            if (hb.furnKind == 1 || hb.furnKind == 2) {
                if (hostRole_)
                    queueFurnitureState(net, ownerId, ok, false, hb.furnKind,
                                        hb.furn, 0, 0);
                else
                    queueFurnitureIntent(net, ownerId, ok, false, hb.furnKind,
                                         hb.furn, nowPub);
            } else {
                EventPacket ev; memset(&ev, 0, sizeof(ev));
                ev.type = (u8)PKT_EVENT; ev.event = (u8)EVT_EXIT_FURNITURE;
                ev.ownerId = ownerId; ev.eventId = nextEventId_++;
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
            }
            hb.furnKind = 0;
            hb.furn[0] = hb.furn[1] = hb.furn[2] = hb.furn[3] = hb.furn[4] = 0;
        }
    }

    // Host-only engine changes on a peer-owned captive (for example a guard
    // jailing it) join the same canonical state stream as accepted intents.
    if (furnSync_ && hostRole_ && !furnitureHostPend_.empty()) {
        for (unsigned int pi = 0; pi < furnitureHostPend_.size(); ++pi) {
            const PendFurnState& pe = furnitureHostPend_[pi];
            queueFurnitureState(net, ownerId, pe.occ, pe.on, pe.kind,
                                pe.furn, 0, 0);
        }
        furnitureHostPend_.clear();
    }

    // A Join retries the SAME sequence until an owner-scoped Host ack covers
    // it. At rest this loop sends nothing; packet loss costs at most two seconds.
    if (furnSync_ && !hostRole_) {
        const unsigned long FURN_INTENT_RETRY_MS = 2000;
        for (std::map<Key, FurnitureRow>::iterator fi = furnitureRows_.begin();
             fi != furnitureRows_.end(); ++fi) {
            FurnitureRow& row = fi->second;
            if (row.pendingSeq == 0 || !hostIntentRetryDue(
                    nowPub, row.pendingSendMs, FURN_INTENT_RETRY_MS)) continue;
            queueFurnitureIntent(net, ownerId, fi->first, row.pendingOn,
                                 row.pendingKind, row.pendingFurn, nowPub);
        }
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
//   carry=  this body is a PASSENGER on someone's shoulder (isBeingCarried)
// cls=pc rows carry the player characters, which the NPC dumps exclude
// (isPlayerSquad skip) - the class where a diverged host-PC hid from every gate.
//
// carry= exists because a passenger has NO position of its own: its transform is
// owned by the carrier's shoulder attach on BOTH clients, so its cross-client gap
// IS the carrier's gap, not its own tracking error. Measured on the camp save,
// where the host's Holy Sentinels arrest the join-owned PC and haul it to prison:
// across 11 carry intervals the PC's host<->join gap equalled the carrier's to
// within 0.1 u (38.7/38.7, 66.1/66.1, 124.8/124.8, run 20260806_100102). A
// carried body also reports mv=0 - it is not locomoting itself - so the PC gate's
// "at rest" filter SELECTED exactly those samples and charged the carrier's NPC
// tracking error to the PC at a 5 u bound (10 of 15 rest pairs; median 10.0 ->
// 63.5 u). The oracle judges the carrier on its own NPC row instead.
void Replicator::emitWnpcRow(Character* c, const EntityState& st, const char* cls) {
    char nm[40]; engine::charName(c, nm, sizeof(nm));
    float pelvis = -1.0f; int idle = -1, crouch = -1, ptask = (int)st.task;
    if (c) engine::readPoseState(c, &pelvis, &idle, &crouch, &ptask);
    int mv = (st.cMoving || st.cSpeed > 0.25f) ? 1 : 0;
    // A body seated in a bed/cage cannot locomote. The engine still reports
    // currentlyMoving when a drive walk was issued against that transform
    // anchor (world_parity Flashbox: 10 of 12 consecutive frozen host samples
    // reported mv=1), which both starves the oracle's rest filter and keeps
    // the drive on the walk branch so applyRest never parks the offset. Trust
    // the seat over the motion bit. Chain (kind=3) is equip, not a seat - a
    // shackled worker walks, so it keeps the engine bit.
    if (c && mv) {
        engine::FurnitureRead fr;
        if (engine::readFurniture(c, &fr) && fr.valid &&
            (fr.kind == 1 || fr.kind == 2))
            mv = 0;
    }
    // LOCAL read (like pelvis): the streamed BODY_CARRIED bit describes the
    // OWNER's body, but each side must report where ITS OWN copy is attached -
    // the local pickup/drop may lead or trail the stream by a beat.
    int carried = 0;
    engine::CarryRead cr;
    if (c && engine::readCarry(c, &cr) && cr.valid && cr.beingCarried) carried = 1;
    char r[256];
    _snprintf(r, sizeof(r) - 1,
              "SCENARIO WNPC hand=%u,%u,%u,%u,%u pos=%.1f,%.1f,%.1f "
              "cls=%s name='%s' task=%u pelvis=%.2f mv=%d carry=%d",
              st.hIndex, st.hSerial, st.hType,
              st.hContainer, st.hContainerSerial,
              st.x, st.y, st.z, cls, nm, (unsigned int)st.task, pelvis, mv,
              carried);
    r[sizeof(r) - 1] = '\0'; coop::logLine(r);
}

void Replicator::notePlatoons(GameWorld* gw, const EntityState* sts,
                              unsigned int n, const char* side) {
    if (!sts) return;
    unsigned long now = nowMs();
    if (platoonT0_ == 0) platoonT0_ = now;
    float anch[12];
    unsigned int nAnch = engine::interestAnchors(gw, anch);
    for (unsigned int i = 0; i < n; ++i) {
        std::pair<unsigned int, unsigned int> p(sts[i].hContainer,
                                                sts[i].hContainerSerial);
        if (!seenPlatoons_.insert(p).second) continue;
        float nearest = -1.0f;
        for (unsigned int a = 0; a < nAnch; ++a) {
            float d = dist3(sts[i].x, sts[i].y, sts[i].z,
                            anch[a * 3 + 0], anch[a * 3 + 1], anch[a * 3 + 2]);
            if (nearest < 0.0f || d < nearest) nearest = d;
        }
        Character* c = engine::resolve(sts[i]);
        char nm[40]; engine::charName(c, nm, sizeof(nm));
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "[platoon] first-sight side=%s id=%u,%u at=%.0f,%.0f,%.0f "
                  "dAnchor=%.0f zone=%d elapsed=%lums name='%s'",
                  side, sts[i].hContainer, sts[i].hContainerSerial,
                  sts[i].x, sts[i].y, sts[i].z, nearest,
                  engine::isZoneLoadedAt(gw, sts[i].x, sts[i].y, sts[i].z) ? 1 : 0,
                  now - platoonT0_, nm);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
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
    bool trunc = false;
    unsigned int n = engine::listNpcsWide(gw, censusRadius_ * 1.25f, chars, states,
                                          NPC_CENSUS_MAX, &trunc);
    // A truncated census is an ACTIVE falsehood, not just a thin one: every NPC
    // past the cap is broadcast as "does not exist on the host", and the join
    // culls its real local copy against that. The fill is per-anchor, so a dense
    // region around one anchor can consume the whole budget and starve the
    // peer's. Log the edges (and re-log slowly while it persists) rather than
    // every beat - this runs at 1 Hz forever.
    static unsigned long truncLogMs = 0; // main-thread only
    bool truncEdge = (trunc != censusPubTrunc_);
    if (truncEdge || (trunc && (now - truncLogMs) >= 30000)) {
        censusPubTrunc_ = trunc;
        truncLogMs = now;
        char b[176]; _snprintf(b, sizeof(b) - 1,
            "[census] publish %s n=%u cap=%u radius=%.0f",
            trunc ? "TRUNCATED (cap hit; far NPCs broadcast as absent)"
                  : "complete again",
            n, (unsigned)NPC_CENSUS_MAX, censusRadius_ * 1.25f);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    static u32   hands[NPC_CENSUS_MAX * 5];
    static float poss[NPC_CENSUS_MAX * 3];
    // A census row is read on the far side as "this exists", and the ABSENCE of a
    // row as "this does not". That makes this list an existence claim, and the
    // only thing entitled to narrow it is whether the body is OURS to speak for -
    // handled by the authority gate in the loop. Attention has no business here
    // and now lives on the stream instead (see the captureNpcs gate); the history
    // of trying to put it here is in the loop comment, along with the measurement
    // that settled it.
    float rawAnch[12];
    unsigned int nRawAnch = engine::interestAnchors(gw, rawAnch);
    // A proxy's own hand is one WE minted and no other client has ever heard of,
    // so censusing it under that hand vouches for nothing: the peer looks for the
    // body it knows, does not find it, and hides its own real copy. That is how a
    // body ends up visible on one client and suppressed on the other - measured
    // 2026-08-04 on hand 1,321,597290048,1,3427978496, which the host displayed as
    // a driven proxy while the join had the original culled, and it is why the pair
    // saw different populations after travelling together. The body a proxy STANDS
    // FOR is the hand it was bound to, so that is the hand it is vouched for under.
    std::map<Character*, Key> proxyKeyOf;
    for (std::map<Key, Character*>::const_iterator pi = proxyByKey_.begin();
         pi != proxyByKey_.end(); ++pi)
        if (pi->second) proxyKeyOf[pi->second] = pi->first;
    std::set<Key> censusKeys;
    unsigned int m = 0;          // rows that survived the gate
    unsigned int nNotMine = 0;   // omitted because another client authors them
    unsigned int nProxyRow = 0;  // rows published under a bound key, not a local one
    for (unsigned int i = 0; i < n; ++i) {
        Key k = keyOf(states[i]);
        std::map<Character*, Key>::const_iterator px = proxyKeyOf.find(chars[i]);
        if (px != proxyKeyOf.end()) { k = px->second; ++nProxyRow; }
        censusKeys.insert(k);
        // The attention gate USED to sit here, and it was the wrong channel for
        // it. A census row is a statement that a body EXISTS, and existence
        // cannot depend on who happens to be looking - but the entity stream was
        // never gated the same way, so we streamed bodies to the peer while this
        // list told the peer they did not exist. Measured 2026-08-03: the join
        // published n=5 of 258 enumerated bodies while streaming enough for the
        // host to drive 134, and the host duly hid 108 of its own real bodies
        // against that 5-row census ("[census] recv n=5 culls=108") while showing
        // the 134 driven ones, which proxies are exempt from judging. Attention
        // now gates the STREAM, where the per-tick cost actually is; the census
        // stays a complete claim over what we author. That ordering also keeps
        // census a superset of stream, so we can never again stream a body we
        // refuse to vouch for.
        //
        // Presence authority: a census row is an existence CLAIM, and we only
        // get to make it about cells we own. Without this the two clients would
        // each broadcast the whole overlapping walk and each cull the other's
        // bodies against it.
        if (cellAuth_ && !weAuthor(gw, ownerId, states[i].x, states[i].z)) {
            ++nNotMine;
            continue;
        }
        // The same echo guard the entity stream carries, for the same reason.
        // Gating only the stream left the slower half of the loop intact: the
        // join would stop streaming a body the host owns but still vouch for it
        // here, and a census row is what drives the park/walk/FREEZE
        // reconciliation - so both clients went on correcting one body's
        // position between them. That is the residue escape_cohesion caught on
        // 2026-08-08 (run 213339: hand 1,1754760704 held by both for 62 s with
        // only 2 host / 8 join frames of stream overlap, and both sides calling
        // it 'mine' in the roster dumps).
        //
        // Census is meant to be a superset of what we stream, so anything
        // dropped here must also be dropped there - it is, by the guard in
        // publishOwned, which is keyed on the same question about the same hand.
        if (cellAuth_ && ownerId != (u32)CELL_OWNER_HOST &&
            peerStreamFresh(k, nowMs())) {
            ++nNotMine;
            ++cellYields_;
            continue;
        }
        // k, not states[i]: identical for an ordinary body, and the bound hand for
        // a proxy (above). Position stays local - that is where the body actually
        // is, whichever hand names it.
        hands[m * 5 + 0] = k.t;
        hands[m * 5 + 1] = k.c;
        hands[m * 5 + 2] = k.cs;
        hands[m * 5 + 3] = k.i;
        hands[m * 5 + 4] = k.s;
        poss[m * 3 + 0]  = states[i].x;
        poss[m * 3 + 1]  = states[i].y;
        poss[m * 3 + 2]  = states[i].z;
        ++m;
    }
    pruneAttention(censusKeys);
    net.queueNpcCensus(ownerId, hands, poss, m);

    // Phase 2 mid-band tier: rebuild the round-robin list from this census
    // walk. Everything beyond the stream bubble's KEEP band belongs to the
    // mid tier; nearest-first so a MAX_PUBLISH squeeze drops the farthest.
    // Distance is to the closest interest ANCHOR (protocol 43: tab leaders +
    // local camera + peer camera hint) - the same anchors the stream bubble
    // uses, so a camera-watched far NPC gets a mid-band drive slot too.
    {
        const float MID_NEAR_EDGE = 260.0f; // captureNpcs' NPC_CAPTURE_KEEP
        // RAW anchors: drive tiers are a bandwidth decision, not an authority
        // one, so the attention gate and its zone veto do not apply here.
        const float* anchors = rawAnch;
        unsigned int nAnchor = nRawAnch;
        // Attention, for MEMBERSHIP (not for the distance tiering above): the
        // mid band exists purely to spend bandwidth driving far bodies, so it is
        // the clearest case of work worth skipping when the peer is not watching.
        float peerAnch[12];
        unsigned int nPeerAnch = peerAnchors(gw, peerAnch);
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
            // Same ownership rule as the near band: we drive what we author.
            if (cellAuth_ && !weAuthor(gw, ownerId, states[i].x, states[i].z)) continue;
            if (cellAuth_ && nPeerAnch > 0 &&
                !observedByPeer(keyOf(states[i]), peerAnch, nPeerAnch,
                                states[i].x, states[i].y, states[i].z)) continue;
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

    if (auditRows_) notePlatoons(gw, states, n, "host");

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
        // Per-anchor breakdown. This was written expecting to catch the host
        // censusing an UNLOADED block at the peer's anchor and broadcasting
        // "no NPCs here" over a town the peer was standing in. Measured false:
        // every split_far run at ~5,200 u separation reported a1=loaded with a
        // healthy share, because both clients hold copies of all player
        // characters and Kenshi streams zones around the peer's squad as
        // readily as around the local one. The breakdown stays as the standing
        // check that this remains true (and as the input to Phase C's
        // capability veto) - it is no longer evidence of the ghost mechanism.
        const float* anch = rawAnch;    // pre-veto: the veto reads off a?=
        unsigned int na = nRawAnch;
        char det[192]; det[0] = '\0';
        for (unsigned int a = 0; a < na; ++a) {
            unsigned int share = 0;
            for (unsigned int i = 0; i < n; ++i) {
                float best = -1.0f; unsigned int bi = 0;
                for (unsigned int s = 0; s < na; ++s) {
                    float d = dist3(states[i].x, states[i].y, states[i].z,
                                    anch[s * 3 + 0], anch[s * 3 + 1],
                                    anch[s * 3 + 2]);
                    if (best < 0.0f || d < best) { best = d; bi = s; }
                }
                if (bi == a) ++share;
            }
            bool ld = engine::isZoneLoadedAt(gw, anch[a * 3 + 0],
                                             anch[a * 3 + 1], anch[a * 3 + 2]);
            char one[40];
            _snprintf(one, sizeof(one) - 1, " a%u=%s:%u",
                      a, ld ? "loaded" : "UNLOADED", share);
            one[sizeof(one) - 1] = '\0';
            unsigned int used = (unsigned int)strlen(det);
            if (used + strlen(one) + 1 < sizeof(det)) strcat(det, one);
        }
        // n = rows actually published, enum = what the walk found; the
        // per-anchor shares below are shares of enum, so they sum to enum
        // rather than to n whenever the gate held rows back.
        // notmine replaces the old dorm= field, which counted BOTH omission
        // reasons behind one name (attention and authority) and so could not say
        // why anything was left out - it read as "the peer is not watching these"
        // when it mostly meant "another client authors these". Now there is only
        // one reason a row is dropped here, and this names it.
        char b[320];
        _snprintf(b, sizeof(b) - 1,
                  "[census] sent n=%u radius=%.0f mid=%u fast=%u anchors=%u%s"
                  " enum=%u notmine=%u proxyrow=%u attnR=%.0f",
                  m, censusRadius_, (unsigned)midBand_.size(),
                  midFastPromoted_, na, det,
                  n, nNotMine, nProxyRow, attentionRadius_);
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
            // Authority PROVENANCE for this publish. weAuthor returning true is
            // the whole mechanism behind the 2026-08-08 dual drive - the join
            // authored 43 rows while owning no cell - but the verdict alone
            // cannot say whether that is the deliberate vacated-cell rule
            // (cellLastOwner_) or a cell nobody ever claimed falling open. The
            // two need opposite fixes, so count the authored rows by source.
            unsigned int bySrc[AUTHSRC_N];
            for (int s = 0; s < (int)AUTHSRC_N; ++s) bySrc[s] = 0;
            for (unsigned int i = 0; i < n; ++i) {
                int cx = 0, cz = 0, src = 0;
                u32 owner = authoritySrc(gw, states[i].x, states[i].z, &cx, &cz, &src);
                if (owner == ownerId && src >= 0 && src < (int)AUTHSRC_N) ++bySrc[src];
            }
            char ab[256];
            _snprintf(ab, sizeof(ab) - 1,
                      "[census] auth mine=%u claim=%u vacated=%u open=%u nomap=%u "
                      "collapsed=%u cells=%u lastOwner=%u",
                      m, bySrc[AUTHSRC_CLAIM], bySrc[AUTHSRC_VACATE],
                      bySrc[AUTHSRC_OPEN], bySrc[AUTHSRC_NOMAP],
                      bySrc[AUTHSRC_COLLAPSE],
                      (unsigned)claimedCells_.size(), (unsigned)cellLastOwner_.size());
            ab[sizeof(ab) - 1] = '\0'; coop::logLine(ab);
            for (unsigned int i = 0; i < n; ++i) {
                char nm[48];
                engine::charName(chars[i], nm, sizeof(nm));
                int cx = 0, cz = 0, src = 0;
                u32 owner = authoritySrc(gw, states[i].x, states[i].z, &cx, &cz, &src);
                char r[256];
                _snprintf(r, sizeof(r) - 1,
                          "[census] row %u hand=%u,%u pos=%.0f,%.0f,%.0f "
                          "cell=%d,%d owner=%u src=%d mine=%d name='%s'",
                          i, states[i].hIndex, states[i].hSerial,
                          states[i].x, states[i].y, states[i].z,
                          cx, cz, owner, src, owner == ownerId ? 1 : 0, nm);
                r[sizeof(r) - 1] = '\0'; coop::logLine(r);
            }
        }
    }
}

void Replicator::syncCamHint(GameWorld* gw, Inbound& in, NetLink& net, u32 ownerId) {
    if (!gw) return;
    unsigned long now = nowMs();

    // Both sides: publish the LOCAL camera center to the engine's interest
    // layer (never crosses the wire - each client reads its own camera).
    float local[3];
    bool haveLocal = engine::cameraCenter(gw, local);
    engine::setLocalCamAnchor(haveLocal, local[0], local[1], local[2]);

    // Ship the camera center to the peer at ~1 Hz (unreliable, latest wins -
    // a lost hint is replaced a second later).
    //
    // BIDIRECTIONAL as of the attention gate. It used to be join -> host only,
    // which was enough while the hint's only job was widening the host's
    // interest spheres. Dormancy needs more than that: it is a predicate BOTH
    // clients must evaluate the same way, and a client cannot tell whether the
    // peer is watching a region unless the peer publishes where it is looking.
    // With the host's camera private, the join would have to assume the host
    // is always watching - which is exactly the assumption that keeps the
    // ghost churn alive.
    if (haveLocal && (camHintSendMs_ == 0 || (now - camHintSendMs_) >= 1000)) {
        camHintSendMs_ = now;
        CamHintPacket p;
        p.type = (u8)PKT_CAM_HINT;
        p.ownerId = ownerId;
        p.x = local[0]; p.y = local[1]; p.z = local[2];
        net.queueCamHint(p);
    }

    // Drain received hints (latest wins) into peerCam_ + staleness stamp, and
    // publish a FRESH hint to the engine's interest layer. A stale hint
    // (silent peer > 3 s: alt-tabbed, loading, disconnecting) drops out of the
    // anchor set rather than pinning interest forever.
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
