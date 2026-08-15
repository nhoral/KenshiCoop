// ReplicatorCore.cpp - Replicator construction + session lifecycle (monolith
// split from Replicator.cpp, 2026-07-12): the ctor defaults, the Phase 3
// unified entity-lifecycle audit (lifeName/lifeSet/lifeSweep + life_),
// resetSession (the ONE place every cross-tick hub resets on session swap),
// inbound ingest (ingest/ingestInv), tab latching/ranking, and the
// end-of-session smoothness summary.
//
// Shared hubs written here: ALL of them (resetSession clears every map);
// life_ is owned here (lifeSet is the only writer, called from every TU).
// Must NOT: change any log string - log phrasing is the API consumed by the
// PowerShell oracles (see resources/CODE_MAP.md, log-tag index).

#include "ReplicatorUtil.h"

namespace coop {

Replicator::Replicator()
    : catchupK_(CATCHUP_K), snapDist_(SNAP_DIST), snapSeconds_(SNAP_SECONDS),
      combatSoftDist_(COMBAT_SOFT_DIST), combatSnapDist_(COMBAT_SNAP_DIST),
      combatBigSnapDist_(COMBAT_BIG_SNAP_DIST), combatSlideMax_(COMBAT_SLIDE_MAX),
      combatConvergeMs_(COMBAT_CONVERGE_MS),
      sendStamp_(true),
      starveHoldMs_(10000), starveHeldNow_(0),
      leaderOnly_(true), streamNpcs_(false),
      activeFrames_(0), zeroWhileActive_(0), maxStep_(0.0f), slewSkipFrames_(0),
      zpDown_(0), zpCarried_(0), zpFurn_(0), zpChain_(0), zpCrawl_(0),
      zpSneak_(0), zpSquadIdle_(0), zpAlias_(0), zpStall_(0),
      onsetReentries_(0),
      onActiveEarly_(0), onActiveMid_(0), onActiveSteady_(0),
      onZeroEarly_(0), onZeroMid_(0), onZeroSteady_(0),
      interpLerp_(0), interpSingle_(0), interpClampOld_(0),
      interpExtrap_(0), interpSegSnap_(0),
      hardSnapSquad_(0), hardSnapNpc_(0), hardSnapMid_(0),
      walkReissueSquad_(0), walkReissueNpc_(0), restFlipNpc_(0), restFlipMid_(0),
      combatSnapTotal_(0), combatSoftWalk_(0), combatSlide_(0), combatOrder_(0),
      combatWrongTgt_(0), combatLogTick_(0),
      interpLogTick_(0),
      translateFrames_(0), walkTruthFrames_(0),
      restSampleFrames_(0), marchFrames_(0),
      marchHold_(0), marchSettle_(0), marchRelapse_(0),
      marchHoldDip_(0), marchHoldStop_(0),
      gateSamples_(0), gateAgree_(0), gateLogTick_(0),
      probeRecruit_(false), probedCount_(0),
      aiSuspend_(false), aiLogTick_(0), nextEventId_(1),
      nextWorldNetId_(1), worldSeeded_(false), nextNativeTakeId_(0),
      nextDropId_(1), nextPickupId_(1), nextXferId_(1),
      xferScanMs_(0), nextTreatId_(1),
      quietRelapse_(0), crawlPhysRestore_(0),
      sitOrders_(0), detachUses_(0), noDetach_(false),
      dmgGuard_(false), reportCombat_(false), nextHitId_(1),
      carrySync_(true), furnSync_(true), chainSync_(true),
      stealthSync_(true), proneSync_(true),
      gateAuthority_(false), trustLogTick_(0),
      trustGrants_(0), trustRevokes_(0),
      authSuppresses_(0), authRestores_(0), authReassertMs_(0), authPruned_(0),
      censusRadius_(0.0f), censusSendMs_(0), censusRecvMs_(0), censusCulls_(0),
      censusOffCell_(0), cellYields_(0), localId_(0xFFFFFFFFu), hostDriveRefusals_(0),
      censusPubTrunc_(false), censusFreshPrev_(false), censusFreshChkMs_(0),
      censusStaleMs_(0), censusStaleEdges_(0), proxyDriftLogMs_(0),
      camHintSendMs_(0), peerCamMs_(0),
      midCursor_(0), midSliceMs_(0), midFastPromoted_(0),
      censusParkDist_(0.0f), censusParks_(0),
      censusPrevMs_(0), censusWalkDist_(0.0f), censusWalks_(0),
      censusFreezeAi_(true),
      attentionRadius_(0.0f),
      attnFlips_(0), attnWinMs_(0), attnBaseSupp_(0), attnBaseCull_(0),
      attnBaseProxy_(0), attnVetoMs_(0), attnVetoRawN_(0), attnVetoMask_(0),
      auditRows_(false), jailProbe_(false), jailObserve_(false),
      speedLastApplied_(-1.0f), speedMyReq_(-1.0f), speedPeerReq_(-1.0f),
      speedCombatCap_(true),
      speedMyCombat_(false), speedPeerCombat_(false), speedLastSet_(-1.0f),
      speedSeqOut_(1), speedSeqSeen_(0),
      speedLastSendMs_(0), speedCombatSampleMs_(0), speedCombatHoldMs_(0),
      spawnSync_(false), spawnPosLogMs_(0),
      spawnMintRadius_(0.0f), adoptRadius_(0.0f), censusAdopts_(0),
      censusScanMs_(0),
      poolSeen_(-1), poolSent_(-1), poolSentMs_(0), poolTotal_(-1),
      poolSeq_(0), poolAcked_(0),
      moneySync_(true), recruitSync_(true),
      squadSync_(true), tabsSeeded_(0),
      cellAuth_(false), cellCollapse_(false), collapsed_(false),
      claimSendMs_(0), claimAssertMs_(0), claimMapMs_(0),
      facSeqOut_(1), facSampleMs_(0), factionSync_(true),
      doorSeqOut_(1), doorSampleMs_(0), doorSync_(true),
      buildSeqOut_(1), buildSampleMs_(0), buildSync_(true),
      bdoorSeqOut_(1), bdoorSampleMs_(0), bdoorSync_(true),
      hungerSync_(true),
      prodSeqOut_(1), prodSampleMs_(0), prodSync_(true),
      researchSeqOut_(1), researchSampleMs_(0), researchSync_(true),
      deedSeqOut_(1), deedSampleMs_(0), deedAuditMs_(0), deedSync_(true),
      fixtureSeqOut_(1), fixtureSampleMs_(0), fixtureSync_(true),
      storeSync_(false), contCensusMs_(0),
      timeSync_(true), timeBrake_(true),
      timeSlew_(1.0f), timeSeqOut_(1), timeSeqSeen_(0),
      timeLastSendMs_(0), timeLastLogMs_(0), timeSlewApplied_(-1.0f),
      platoonT0_(0),
      lifeSweepMs_(0) {
    peerCam_[0] = peerCam_[1] = peerCam_[2] = 0.0f;
}

// ---- Phase 3: unified entity lifecycle ---------------------------------------
// The AUDIT layer over the authority/mint/drive machinery: every decision
// point reports the state it just put a hand into, lifeSet logs the edge, and
// the lifecycle oracle judges the journeys. Mechanics live where they were
// validated; this is the one place their OUTCOMES meet.

const char* Replicator::lifeName(int s) {
    switch (s) {
    case LIFE_DISCOVERED: return "DISCOVERED";
    case LIFE_RESOLVED:   return "RESOLVED";
    case LIFE_HI:         return "HI";
    case LIFE_MID:        return "MID";
    case LIFE_PARKED:     return "PARKED";
    case LIFE_CULLED:     return "CULLED";
    default:              return "UNKNOWN";
    }
}

int Replicator::lifeSet(const Key& k, int to, const char* reason) {
    unsigned long now = nowMs();
    Lifecycle& lc = life_[k];
    int from = lc.state;
    lc.touchMs = now;
    if (from == to) return from;
    lc.state = (u8)to;
    lc.sinceMs = now;
    lc.stuckLogMs = 0;
    // Silent seed: every census-band wilderness NPC is born PARKED, and
    // logging hundreds of those at session start buries the real journeys.
    if (from == LIFE_UNKNOWN && to == LIFE_PARKED) return from;
    char b[176];
    _snprintf(b, sizeof(b) - 1,
              "[life] hand=%u,%u,%u,%u,%u from=%s to=%s reason=%s",
              k.t, k.c, k.cs, k.i, k.s,
              lifeName(from), lifeName(to), reason ? reason : "-");
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    return from;
}

void Replicator::lifeSweep(GameWorld* gw, unsigned long now) {
    if ((now - lifeSweepMs_) < 5000) return;
    lifeSweepMs_ = now;
    // A hand not confirmed for a while left interest (or its body despawned):
    // drop the record so the map tracks the live world, and a re-appearance
    // starts a fresh journey from UNKNOWN.
    const unsigned long PRUNE_MS = 30000;
    // Self-audit: DISCOVERED means "the host swears this exists and we have
    // no body" - the mint pipeline should resolve that within its dwell +
    // round-trip budget WHEN the hand's census position sits in a locally
    // loaded zone (an unloaded far zone defers legitimately, forever if need
    // be). Older than STUCK_MS is the invisible-raid failure (the join
    // fights nothing); one line per hand per STUCK_LOG_MS so the lifecycle
    // oracle sees it without the log flooding.
    const unsigned long STUCK_MS     = 30000;
    const unsigned long STUCK_LOG_MS = 15000;
    for (std::map<Key, Lifecycle>::iterator it = life_.begin();
         it != life_.end(); ) {
        Lifecycle& lc = it->second;
        if ((now - lc.touchMs) > PRUNE_MS) { life_.erase(it++); continue; }
        if (lc.state == LIFE_DISCOVERED && (now - lc.sinceMs) > STUCK_MS &&
            (lc.stuckLogMs == 0 || (now - lc.stuckLogMs) > STUCK_LOG_MS)) {
            std::map<Key, CensusPos>::iterator cp = censusPos_.find(it->first);
            bool mintable = cp != censusPos_.end() &&
                            engine::isZoneLoadedAt(gw, cp->second.x,
                                                   cp->second.y, cp->second.z);
            if (mintable) {
                lc.stuckLogMs = now;
                char b[160];
                _snprintf(b, sizeof(b) - 1,
                          "[life] STUCK hand=%u,%u,%u,%u,%u state=DISCOVERED age=%lus (mintable)",
                          it->first.t, it->first.c, it->first.cs, it->first.i,
                          it->first.s, (now - lc.sinceMs) / 1000);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            } else {
                lc.stuckLogMs = now; // re-check the zone on the next cadence
            }
        }
        ++it;
    }
}

void Replicator::resetSession() {
    // Pointer caches (dangling after the swap) + everything keyed off them.
    targets_.clear();          // interp buffers + per-body drive state
    drivenChars_.clear();
    drivenSeen_.clear();       // recently-driven grace (pointers dangle after swap)
    canonicalOf_.clear();      // capture-translation reverse map (same pointers)
    rekeyLogged_.clear();      // and the change-detector for its [rekey] line
    jailObs_.clear();          // jail-observe spike per-captive last sample
    proxyByKey_.clear();
    suppressed_.clear();
    midBand_.clear();          // host mid-band round-robin (rebuilt by next census)
    midCursor_ = 0; midSliceMs_ = 0;
    life_.clear();             // Phase 3 lifecycle: the OLD world's journeys
    lifeSweepMs_ = 0;
    // Debug markers hold raw Character* from the OLD world plus GUI label
    // objects we own - destroy the labels and drop the map before either
    // dangles into the new session.
    for (std::map<Character*, DebugMarker>::iterator mi = debugMarkers_.begin();
         mi != debugMarkers_.end(); ++mi)
        engine::markerDestroy(mi->second.label);
    debugMarkers_.clear();
    hostBody_.clear();
    attackerOf_.clear();
    combatCapMs_.clear();
    authCount_.clear();
    ownHands_.clear();
    // Protocol 36: the existence census describes the OLD world's hands; the
    // host re-publishes within a second of the new world going live.
    censusHands_.clear();
    censusPos_.clear();
    censusPrev_.clear();
    censusPrevMs_ = 0;
    censusFix_.clear();
    parkMs_.clear();
    censusRecvMs_ = 0;
    censusSendMs_ = 0;
    // Protocol 43: the camera hint describes the OLD world's coordinates.
    camHintSendMs_ = 0;
    peerCamMs_ = 0;
    // Attention latches are per-hand state about the OLD world's geometry.
    attnObs_.clear();
    attnObsPeer_.clear();
    // Claims describe where bodies stand in the OLD world.
    claimSlots_.clear();
    claimedCells_.clear();
    cellLastOwner_.clear();
    claimDwell_.clear();
    cellYield_.clear();
    claimSendMs_   = 0;
    claimAssertMs_ = 0;
    claimMapMs_    = 0;
    attnFlips_ = 0;
    attnWinMs_ = 0;
    // The zone verdict describes the OLD world's streaming state.
    attnVetoMs_ = 0;
    attnVetoRawN_ = 0;
    attnVetoMask_ = 0;
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
    deedRows_.clear();     // protocol 54: re-baselined from the new world's owned set
    facRows_.clear();
    invPub_.clear();
    invRecv_.clear();
    ownedContainers_.clear();
    censusContainers_.clear(); // protocol 34: re-censused in the new world
    worldTrack_.clear();
    worldProxies_.clear();
    worldSeeded_ = false; // re-census the reloaded world's save-native items
    pendingDrops_.clear(); // unresolved drop edges belong to the old world
    // Protocol 56: the reloaded world re-mints its natives; stale watches and
    // received-take dedupe belong to the old world. (nextNativeTakeId_ keeps
    // counting so a late reliable resend from before the swap can't collide.)
    nativeWatch_.clear();
    appliedNativeTakes_.clear();
    weaponCensus_.clear();
    appliedDrops_.clear();
    appliedPickups_.clear();
    groundedWeapons_.clear();
    pendingPickups_.clear(); // the objects those intents named belong to the old world
    // Protocol 37: every container hand and Item* baseline is stale in the new world.
    xferBase_.clear();
    xferSeeded_.clear();
    xferPend_.clear();
    xferLatch_.clear();
    xferDefer_.clear();
    xferOut_.clear();
    appliedXfers_.clear();
    wdSuppress_.clear();
    xferScanMs_ = 0;
    medPub_.clear();
    medRecv_.clear();
    medNpc_.clear();
    statsPub_.clear();
    // Protocol 52: the pool baseline describes the OLD world's wallet, so a
    // reload must re-seed it - otherwise the newly loaded save's cats read as
    // one giant local purchase. Pending deltas die with the session they were
    // measured in.
    poolSeen_ = -1; poolSent_ = -1; poolSentMs_ = 0; poolTotal_ = -1;
    poolSeq_ = 0; poolAcked_ = 0;
    poolPending_.clear();
    stealthPub_.clear();
    pinOwned_.clear();
    pinPeer_.clear();
    moveEcho_.clear();
    exitedOwn_.clear();
    // Protocol 35: the rank latch + the engine's pointer->hand baseline both
    // describe the OLD world (containers and Character* dangle after a swap);
    // the reloaded save re-seeds them at first census/poll.
    tabRank_.clear();
    tabOwned_.clear();
    tabsSeeded_ = 0;
    rekeyedOld_.clear();
    engine::clearSquadRoster();
    probed_.clear();
    spawnReq_.clear();
    unresolvedHands_.clear();
    forceReqHands_.clear();
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
    speedCombatHoldMs_ = 0;
    timeSlew_         = 1.0f;
    timeSeqSeen_      = 0;
    timeLastSendMs_   = 0;
    timeSlewApplied_  = -1.0f;
    // Sample-cadence clocks restart.
    facSampleMs_ = doorSampleMs_ = buildSampleMs_ = bdoorSampleMs_ = 0;
    prodSampleMs_ = 0;
    researchSampleMs_ = 0;
    deedSampleMs_ = 0;
    deedAuditMs_ = 0;
    // A coordinated load re-instantiates every runtime fixture, so both halves
    // of the protocol-55 pairing are stale: re-announce and re-match from
    // scratch rather than translate a hand into a destroyed object.
    fixtureSampleMs_ = 0;
    fixtureOut_.clear();
    fixtureMap_.clear();
    contCensusMs_ = 0;
    authReassertMs_ = 0;
    // Config gates, ownRanks_ and every OUTBOUND seq counter are deliberately
    // preserved (see the header comment).
    coop::logLine("[load] session reset: pointer caches, session maps, change gates cleared");
}

void Replicator::clearPeerReplicationState(GameWorld* gw) {
    // Destroy every MINTED proxy body before resetSession() drops the map that
    // owns the pointers. The engine owns the bodies; a proxy left standing after
    // the peer that authored it is gone is a permanent ghost, and any map still
    // pointing at it (targets_, drivenChars_) would drive a body with no fresh
    // authority - or a freed pointer once the engine reaps it. SEH-guarded via
    // despawnProxyNpc so a single bad pointer can't take down the leave path.
    //
    // ADOPTED entries are released, not destroyed (destroyIfMinted): a body we
    // adopted is one this client generated itself, so the peer leaving makes it
    // ours again rather than making it garbage. Destroying them here would empty a
    // whole town on disconnect, and bake that emptiness into the next save.
    unsigned int cleared = 0, released = 0;
    for (std::map<Key, Character*>::iterator it = proxyByKey_.begin();
         it != proxyByKey_.end(); ++it) {
        if (!gw || !it->second) continue;
        if (destroyIfMinted(gw, it->second)) ++cleared; else ++released;
    }
    mintedBodies_.clear();
    char b[128];
    _snprintf(b, sizeof(b) - 1, "[leave] cleared proxies=%u released=%u",
              cleared, released);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
    // World-item proxies (Phase 3): the world stays LIVE across a peer leave /
    // reconnect (no engine world swap), so the proxy RootObjects we minted for the
    // departed peer's ground items must be destroyed here - otherwise they linger
    // as duplicate items and, worse, get baked into the save on the next write
    // (becoming natives that re-stream on reload). resetSession() below only clears
    // the map, not the bodies, so despawn first. The world surviving does NOT mean
    // the proxies did: the peer may have left after a long trek, and anything whose
    // block unloaded on the way is already destroyed, so each one is re-resolved.
    unsigned int wcleared = 0, wstale = 0;
    for (std::map<std::pair<u32, u32>, WorldProxy>::iterator wi = worldProxies_.begin();
         wi != worldProxies_.end(); ++wi) {
        RootObject* live = gw ? liveWorldProxy(wi->second) : 0;
        if (!live) { ++wstale; continue; }
        if (engine::removeWorldItemProxy(gw, live)) ++wcleared;
    }
    _snprintf(b, sizeof(b) - 1, "[leave] cleared worldProxies=%u stale=%u",
              wcleared, wstale);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
    // Now drop every map (proxyByKey_ included) back to freshly-launched state.
    resetSession();
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
        r.ownerId   = it->ownerId;
        r.items     = it->items; // latest snapshot supersedes
        r.dirty     = true;
        r.truncated = (it->flags & INV_FLAG_TRUNCATED) != 0;
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

void Replicator::decideTabs(const EntityState* raw, unsigned int nSquad,
                            const std::vector<std::pair<u32, u32> >& ctnrs) {
    if (!squadSync_) return;   // legacy per-tick ranking owns nothing durably
    bool seeding = (tabsSeeded_ == 0);
    for (unsigned int i = 0; i < ctnrs.size(); ++i) {
        if (tabOwned_.find(ctnrs[i]) != tabOwned_.end()) continue;
        unsigned int rank = tabRankFor(ctnrs[i], ctnrs);
        bool owned;
        const char* why;
        if (seeding) {
            // The save's own tabs, ranked identically on both clients: the
            // historical rule, so nothing about a normal session changes.
            owned = ownRanks_.empty() ? (rank == 0u) : (ownRanks_.count(rank) != 0);
            why = "seed";
        } else {
            // A tab that did not exist at session start. Its rank is a local
            // number the peer does not share, so the rank rule cannot answer -
            // ask who authored the bodies standing in it. The pins are still
            // present at this moment (the move/recruit that created the tab is
            // what put them there); capturing the answer HERE is what makes it
            // survive the later EXIT that clears them.
            int verdict = 0;   // +1 ours, -1 peer's, 0 nobody claims it
            for (unsigned int m = 0; m < nSquad; ++m) {
                if (raw[m].hContainer != ctnrs[i].first ||
                    raw[m].hContainerSerial != ctnrs[i].second) continue;
                Key k = keyOf(raw[m]);
                if (pinOwned_.count(k)) { verdict = 1; break; }
                if (pinPeer_.count(k))  verdict = -1;
            }
            owned = (verdict == 1) ? true
                                   : ((verdict == -1) ? false : isHostRole());
            why = (verdict == 1) ? "pin-own"
                                 : ((verdict == -1) ? "pin-peer" : "host-fallback");
        }
        tabOwned_[ctnrs[i]] = owned;
        if (!seeding) {
            char b[128];
            _snprintf(b, sizeof(b) - 1, "[squad] TABOWN cont=%u,%u rank=%u own=%d via=%s",
                      ctnrs[i].first, ctnrs[i].second, rank, owned ? 1 : 0, why);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
    // Freeze the shared-rank prefix at the end of the seeding pass.
    if (seeding && !ctnrs.empty()) tabsSeeded_ = (unsigned int)tabRank_.size();
}

bool Replicator::ownsTab(const std::pair<u32, u32>& key, unsigned int rank) const {
    std::map<std::pair<u32, u32>, bool>::const_iterator it = tabOwned_.find(key);
    if (it != tabOwned_.end()) return it->second;
    // Not yet decided (squadSync_ off, or the first tick of a fresh container):
    // the historical rule, so this is never MORE permissive than before.
    return ownRanks_.empty() ? (rank == 0u) : (ownRanks_.count(rank) != 0);
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

    // zeroFrac population audit (see the zp* declarations): what the frozen
    // frames actually WERE. Everything but `free` is a body that could not have
    // walked that frame, so it bounds how much of zeroFrac is even addressable.
    char zp[208];
    _snprintf(zp, sizeof(zp) - 1,
              "SCENARIO ZEROPOP zero=%lu down=%lu carried=%lu furn=%lu chain=%lu "
              "crawl=%lu sneak=%lu squadIdle=%lu alias=%lu stall=%lu",
              zeroWhileActive_, zpDown_, zpCarried_, zpFurn_, zpChain_,
              zpCrawl_, zpSneak_, zpSquadIdle_, zpAlias_, zpStall_);
    zp[sizeof(zp) - 1] = '\0';
    coop::logLine(zp);

    // Motion-onset audit (see the onset* declarations): is zeroFrac reporting
    // stutter, or is it reporting how often the body re-entered scoring? Each
    // bucket's fraction is what to read - if early runs far above steady, the
    // metric is charging render catch-up at motion onset, and the low-sample
    // runs fail because bursty motion pays that toll repeatedly.
    float eF = (onActiveEarly_  > 0) ? (float)onZeroEarly_  / (float)onActiveEarly_  : 0.0f;
    float mF = (onActiveMid_    > 0) ? (float)onZeroMid_    / (float)onActiveMid_    : 0.0f;
    float sF = (onActiveSteady_ > 0) ? (float)onZeroSteady_ / (float)onActiveSteady_ : 0.0f;
    char on[240];
    _snprintf(on, sizeof(on) - 1,
              "SCENARIO ONSET reentries=%lu earlyN=%lu earlyZero=%lu earlyFrac=%.3f "
              "midN=%lu midZero=%lu midFrac=%.3f steadyN=%lu steadyZero=%lu steadyFrac=%.3f",
              onsetReentries_, onActiveEarly_, onZeroEarly_, eF,
              onActiveMid_, onZeroMid_, mF,
              onActiveSteady_, onZeroSteady_, sF);
    on[sizeof(on) - 1] = '\0';
    coop::logLine(on);

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
    // hold/settle/rlps attribute those march frames (see the counter decls). They
    // are appended AFTER marchFrac so Test-MarchInPlace's regex still matches.
    char m[224];
    _snprintf(m, sizeof(m) - 1,
              "SCENARIO MARCH restSamples=%lu march=%lu marchFrac=%.3f "
              "hold=%lu settle=%lu rlps=%lu holdDip=%lu holdStop=%lu",
              restSampleFrames_, marchFrames_, marchFrac,
              marchHold_, marchSettle_, marchRelapse_,
              marchHoldDip_, marchHoldStop_);
    m[sizeof(m) - 1] = '\0';
    coop::logLine(m);

    // Step-2 pruning evidence: how often the legacy quieting patchwork actually
    // fired this run. Sustained relapse=0 across regressions = the I11 re-quiet is
    // dead code under default AI-suspend and can be deleted; detach counts feed the
    // KENSHICOOP_NO_DETACH A/B decision.
    char q[160];
    _snprintf(q, sizeof(q) - 1,
              "SCENARIO QUIET relapse=%lu sitOrders=%lu detach=%lu noDetach=%d "
              "crawlPhys=%lu",
              quietRelapse_, sitOrders_, detachUses_, noDetach_ ? 1 : 0,
              crawlPhysRestore_);
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
