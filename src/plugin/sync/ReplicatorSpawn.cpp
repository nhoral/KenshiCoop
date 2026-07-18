// ReplicatorSpawn.cpp - runtime-spawn proxy + event replication (monolith
// split from Replicator.cpp, 2026-07-12): syncSpawns (protocol 21 spawn
// describe/request/mint incl. far minting + creature-size/age sync),
// applyEvents (reliable EVT_* intake: KO/death/limb/recruit/treatment edges)
// and rekeyPeerBody (EVT_RECRUIT / squad-move re-keying).
//
// Shared hubs: owns proxyByKey_/spawnReq_ (mint bookkeeping); rekeys
// targets_/life_ entries in place; writes life_ via lifeSet.
// Must NOT: change any log string - log phrasing is the API consumed by the
// PowerShell oracles (see resources/CODE_MAP.md, log-tag index).

#include "ReplicatorUtil.h"

namespace coop {

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
                lifeSet(it->first, LIFE_UNKNOWN, "proxy-despawned");
                proxyByKey_.erase(it++);
                continue;
            }
            // Baked-duplicate self-heal (Phase 1 spawn parity): the proxy's
            // BOUND key was unresolvable when we minted - if it resolves NOW
            // (to a body that isn't the proxy; proxies carry their own minted
            // hands), the shared-save original materialized under it (its
            // block finished loading, or an engine re-container restored the
            // hand). The original is authoritative and resolve-driven by the
            // stream directly; the proxy is a standing double - destroy it.
            const Key& bk = it->first;
            Character* orig = engine::resolveCharByHand(bk.i, bk.s, bk.t, bk.c, bk.cs);
            if (orig && orig != it->second) {
                engine::despawnProxyNpc(gw, it->second);
                char b[176]; _snprintf(b, sizeof(b) - 1,
                    "[spawn] proxy DUPE-HEAL hand=%u,%u,%u,%u,%u (original resolved; "
                    "proxy destroyed, proxies=%u)",
                    bk.t, bk.c, bk.cs, bk.i, bk.s, (unsigned)proxyByKey_.size() - 1);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                spawnReq_.erase(bk); // hand resolves now - no REQ needed again
                lifeSet(bk, LIFE_RESOLVED, "dupe-heal");
                proxyByKey_.erase(it++);
                continue;
            }
            ++it;
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
            float age = 0.0f;
            bool found = c && engine::describeCharacter(
                c, pkt.charSid, sizeof(pkt.charSid), pkt.facSid, sizeof(pkt.facSid),
                &pkt.x, &pkt.y, &pkt.z, &pkt.heading, &dead, &age);
            pkt.found = found ? 1 : 0;
            pkt.dead  = dead ? 1 : 0;
            pkt.age   = age; // animals scale body size by age (protocol 39)
            net.queueSpawnInfo(pkt);
            char b[224]; _snprintf(b, sizeof(b) - 1,
                "[spawn] INFO send hand=%u,%u,%u,%u,%u found=%d dead=%d age=%.2f sid='%s' fac='%s'",
                k.t, k.c, k.cs, k.i, k.s, pkt.found, pkt.dead, pkt.age,
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
            // Phase 1b (phantom fix): a hand we PIN owned is ours - a census
            // vouch for it is our own echo or a stale tail after a control-flip
            // claim, never a mintable peer body.
            if (pinOwned_.find(k) != pinOwned_.end()) continue;
            {
                // Already recorded unresolved by applyTargets. Phase 2 mid-band
                // regression (run 113712): the mid tier now STREAMS a distant
                // runtime spawn before this scan ever sees it, and skipping
                // here left the hand stream-classed (cen=0) - proximity-gated
                // REQs, no far-mint - so raids again materialized on top of
                // the join player (mints at 237-249 u vs Phase 1's ~574 u).
                // The hand is census-vouched regardless of which path noticed
                // it first: grant the census mark + arm the far-mint dwell.
                std::map<Key, UnresolvedHand>::iterator uh = unresolvedHands_.find(k);
                if (uh != unresolvedHands_.end()) {
                    uh->second.fromCensus = true;
                    SpawnReqState& ar = spawnReq_[k];
                    if (ar.firstMissMs == 0) ar.firstMissMs = now;
                    lifeSet(k, LIFE_DISCOVERED, "census-miss");
                    continue;
                }
            }
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
            if (engine::resolveCharByHand(k.i, k.s, k.t, k.c, k.cs)) {
                // Resolvable again: disarm the far-mint dwell so a LATER real
                // despawn restarts the sustained-miss clock from zero.
                std::map<Key, SpawnReqState>::iterator ar = spawnReq_.find(k);
                if (ar != spawnReq_.end()) ar->second.firstMissMs = 0;
                continue;
            }
            // Arm/continue the sustained-miss clock (Phase 1 far mint): a zone
            // reports loaded a few seconds before its baked bodies resolve, so
            // the far mint waits FAR_MINT_ARM_MS of continuous missing-ness.
            {
                SpawnReqState& ar = spawnReq_[k];
                if (ar.firstMissMs == 0) ar.firstMissMs = now;
            }
            UnresolvedHand& u = unresolvedHands_[k];
            u.fromCensus = true;
            lifeSet(k, LIFE_DISCOVERED, "census-miss");
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

    const unsigned int MINT_BUDGET_PER_TICK = 4;
    unsigned int mintedThisTick = 0;
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
        // Phase 1b (phantom fix): a hand we PIN owned must never be minted - it
        // is ours (a control-flip claim), and a reply for it is a stale tail.
        if (pinOwned_.find(k) != pinOwned_.end()) continue;
        // A hand re-keyed away (recruit/squad move) between our REQ and this
        // reply is DEAD - minting it would resurrect the duplicate the re-key
        // just cleaned up (protocol 35 run 192211).
        std::map<Key, unsigned long>::iterator rko = rekeyedOld_.find(k);
        if (rko != rekeyedOld_.end() && (now - rko->second) < REKEYED_GRACE_MS)
            continue;
        // Mint gate (census-mint fix + Phase 1 spawn parity): the reply
        // position is the host's authority. Inside spawnMintRadius_ of our
        // own squad, always mint (a baked NPC this close sits in a loaded
        // block and would have resolved locally, so an unresolvable hand
        // here is a genuine host runtime spawn). BEYOND the radius, a
        // census-sourced hand may FAR-MINT when its position sits in a
        // locally LOADED zone - the same "baked would have resolved" logic,
        // generalized from a fixed radius to the true zone-loaded query -
        // so host spawns appear on the join at the same distance they
        // appeared on the host (the "NPCs spawn on top of the join player"
        // report). Anything else: soft-defer (the NPC may be walking toward
        // us - retry on the FAR_RETRY_MS cadence, and reset the send cap so
        // a long approach can't exhaust it).
        float mintDist = -1.0f;
        for (unsigned int i = 0; i < nSquad; ++i) {
            float d = dist3(p.x, p.y, p.z, squad[i].x, squad[i].y, squad[i].z);
            if (mintDist < 0.0f || d < mintDist) mintDist = d;
        }
        if (spawnMintRadius_ > 0.0f &&
            (mintDist < 0.0f || mintDist > spawnMintRadius_)) {
            // Sustained-miss dwell: the zone-loaded signal precedes baked-body
            // materialization by a few seconds (run 20260712_092921 healed
            // 8/12 far mints), so a far mint additionally requires the hand
            // to have been continuously unresolvable for FAR_MINT_ARM_MS.
            const unsigned long FAR_MINT_ARM_MS = 10000;
            bool farOk = rq.fromCensus && mintDist >= 0.0f &&
                         rq.firstMissMs != 0 &&
                         (now - rq.firstMissMs) >= FAR_MINT_ARM_MS &&
                         (censusRadius_ <= 0.0f || mintDist <= censusRadius_ * 1.25f) &&
                         engine::isZoneLoadedAt(gw, p.x, p.y, p.z);
            if (!farOk) {
                rq.farMs = now;
                rq.sends = 0;
                char b[176]; _snprintf(b, sizeof(b) - 1,
                    "[spawn] INFO deferred (far) hand=%u,%u,%u,%u,%u dist=%.0f mint=%.0f cen=%d",
                    k.t, k.c, k.cs, k.i, k.s, mintDist, spawnMintRadius_,
                    rq.fromCensus ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                continue;
            }
        } else if (mintDist >= 0.0f && !engine::isZoneLoadedAt(gw, p.x, p.y, p.z)) {
            // Phase 2 crash hardening (near-mint zone guard): a NEAR mint normally
            // lands in a loaded block (a body this close would have baked-resolved),
            // but during approach/zone churn the destination block can be mid-load
            // or not yet streamed - creating a body there is the mint-into-unloaded-
            // zone crash vector. Defer (re-judge on the far cadence) until the block
            // is fully loaded. The far branch already requires isZoneLoadedAt; this
            // extends the same proof to near mints. Near-the-player zones are loaded
            // in normal play, so this rarely fires; spawn_sync is the regression gate.
            rq.farMs = now;
            rq.sends = 0;
            char b[176]; _snprintf(b, sizeof(b) - 1,
                "[spawn] INFO deferred (near-unloaded) hand=%u,%u,%u,%u,%u dist=%.0f",
                k.t, k.c, k.cs, k.i, k.s, mintDist);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            continue;
        }
        // Per-tick mint budget: a raid arriving all at once answers a burst
        // of REQs with a burst of INFOs - creating many bodies in one engine
        // tick is a visible hitch. Overflow re-judges in ~1 s via farMs.
        if (mintedThisTick >= MINT_BUDGET_PER_TICK) {
            rq.farMs = (now > FAR_RETRY_MS) ? (now - FAR_RETRY_MS + 1000) : now;
            rq.sends = 0;
            // Phase 0 crash breadcrumb: town/camp approach can answer a burst of
            // REQs and defer many mints per tick. The overflow was SILENT; log it
            // (throttled ~1 Hz/hand by the farMs re-judge above) so a pre-crash
            // mint storm is visible in the flushed trail.
            char bd[176]; _snprintf(bd, sizeof(bd) - 1,
                "[spawn] INFO deferred (budget) hand=%u,%u,%u,%u,%u minted=%u/%u",
                k.t, k.c, k.cs, k.i, k.s, (unsigned)mintedThisTick,
                (unsigned)MINT_BUDGET_PER_TICK);
            bd[sizeof(bd) - 1] = '\0'; coop::logLine(bd);
            continue;
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
        //
        // CENSUS-ONLY scope (2026-07-16 spawn_sync fix): the guard is for an
        // UNCORRELATED census hand (its exact identity is unknown, so a nearby
        // same-template body is likely it). A direct STREAM REQ (rq.fromCensus
        // == false) is fully correlated - the host explicitly streamed THIS hand
        // at THIS position - so it must mint even where same-template world NPCs
        // stand nearby. Applying the guard to stream REQs regressed spawn_sync:
        // near spawns into a POPULATED area (common modded template, twins within
        // 20u) deferred forever (near 0/4), while far spawns into empty terrain
        // bound fine (far 4/4). Restores pre-guard stream-REQ minting; the census
        // path keeps the dupe protection it was built for.
        if (rq.fromCensus && !rq.forceReq) {
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
                char b[200]; _snprintf(b, sizeof(b) - 1,
                    "[spawn] INFO deferred (dupe) hand=%u,%u,%u,%u,%u sid='%s' "
                    "twin within %.0fu cen=%d force=%d",
                    k.t, k.c, k.cs, k.i, k.s, p.charSid, MINT_DUPE_RADIUS,
                    rq.fromCensus ? 1 : 0, rq.forceReq ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                continue;
            }
        }
        Character* proxy = engine::spawnProxyNpc(gw, p.charSid, p.facSid,
                                                 p.x, p.y, p.z, p.heading, p.age);
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
        // Phase 2 crash hardening (post-mint liveness): prove the just-minted
        // body is live and engine-registered by round-tripping its OWN hand
        // BEFORE we bind it into proxyByKey_ and start driving it. A create that
        // returns a pointer the engine cannot resolve back is a body we would
        // drive into a UAF. On failure, destroy it and treat as a failed mint.
        // Mirrors the ~1 Hz syncSpawns sweep, closing the gap at mint time.
        {
            unsigned int ph[5];
            Character* back = 0;
            if (engine::readHand(proxy, ph))
                back = engine::resolveCharByHand(ph[0], ph[1], ph[2], ph[3], ph[4]);
            if (back != proxy) {
                engine::despawnProxyNpc(gw, proxy);
                rq.deniedMs = now;
                char b[176]; _snprintf(b, sizeof(b) - 1,
                    "[spawn] proxy FAILED hand=%u,%u,%u,%u,%u sid='%s' (post-mint liveness)",
                    k.t, k.c, k.cs, k.i, k.s, p.charSid);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                continue;
            }
        }
        proxyByKey_[k] = proxy;
        ++mintedThisTick;
        lifeSet(k, LIFE_RESOLVED, "mint");
        // Dead on arrival: latch the down state now (the same reliable-latch
        // path an EVT_DEATH would take) so the proxy spawns INTO ragdoll
        // instead of standing up for a frame. Latched entries never age out.
        if (p.dead) targets_[k].deathLatched = true;
        // mintDist (Phase 1 telemetry): how far from our squad the proxy
        // appeared - the spawn-parity oracle gates its distribution.
        char b[224]; _snprintf(b, sizeof(b) - 1,
            "[spawn] proxy BOUND hand=%u,%u,%u,%u,%u sid='%s' fac='%s' dead=%d "
            "age=%.2f pos=%.1f,%.1f,%.1f mintDist=%.0f cen=%d (proxies=%u)",
            k.t, k.c, k.cs, k.i, k.s, p.charSid, p.facSid, p.dead ? 1 : 0,
            p.age, p.x, p.y, p.z, mintDist, rq.fromCensus ? 1 : 0,
            (unsigned)proxyByKey_.size());
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        // Phase 1b: a recruit/move whose ok=0 rekey enrolled a force-REQ (the
        // interest-split recruit) now has its proxy body - make it a real squad
        // member on this client too (same as the ok=1 path) and retire the
        // force-REQ. Only force-REQ hands become members: an ordinary world-NPC
        // census mint must NOT be dropped into the player squad.
        if (forceReqHands_.erase(k))
            insertPeerMember(gw, proxy, k, "recruit");
    }

    // Force-REQ injection (protocol 23 interest-split recruit fix): a recruit/
    // move whose rekey landed ok=0 has no local body and would be proximity-
    // gated out of the REQ path when it sits far from OUR squad. Re-seed those
    // hands into unresolvedHands_ as fromCensus (position-less, proximity-
    // bypassed) every pass until they bind a proxy or resolve locally, so the
    // recruited body appears regardless of distance.
    if (!forceReqHands_.empty()) {
        for (std::set<Key>::iterator it = forceReqHands_.begin();
             it != forceReqHands_.end(); ) {
            const Key& k = *it;
            bool bound = proxyByKey_.find(k) != proxyByKey_.end();
            Character* local = bound ? 0
                : engine::resolveCharByHand(k.i, k.s, k.t, k.c, k.cs);
            if (bound || local) { forceReqHands_.erase(it++); continue; }
            UnresolvedHand& uh = unresolvedHands_[k];
            uh.fromCensus = true; // bypass send-side proximity
            uh.forceReq   = true; // correlated by the reliable edge: skip dupe guard
            ++it;
        }
    }

    // Author requests for the hands applyTargets recorded unresolved last
    // tick (proximity-gated to our own squad members) plus this tick's
    // census-missing hands (no position - the reply-side mint gate judges).
    if (!unresolvedHands_.empty()) {
        for (std::map<Key, UnresolvedHand>::iterator it = unresolvedHands_.begin();
             it != unresolvedHands_.end(); ++it) {
            const Key& k = it->first;
            if (proxyByKey_.find(k) != proxyByKey_.end()) continue;
            // Phase 1b (phantom fix): never REQ a hand we PIN owned - a
            // control-flip claimed it, so asking the peer about it would only
            // re-open the phantom-mint window it exists to close.
            if (pinOwned_.find(k) != pinOwned_.end()) continue;
            // Never ask about a hand a re-key just retired (protocol 35): a
            // stale batch of the OLD key may still be in flight, and a REQ
            // for it would mint the duplicate the re-key exists to prevent.
            std::map<Key, unsigned long>::iterator rko = rekeyedOld_.find(k);
            if (rko != rekeyedOld_.end() && (now - rko->second) < REKEYED_GRACE_MS)
                continue;
            SpawnReqState& rq = spawnReq_[k];
            // Census-sourced hands stay census-marked across retries (the
            // far-mint privilege follows the EXISTENCE authority, not the
            // path a later stream sample happened to arrive by).
            if (it->second.fromCensus) rq.fromCensus = true;
            if (it->second.forceReq)   rq.forceReq   = true;
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
    // Phase 1b (control follows the squad transfer): does newK's destination tab
    // resolve to a rank THIS client owns? Uses the SAME predicate publishOwned
    // partitions on (ownRanks_ over the latched tabRank_). When true, the move
    // handed the body to US - we must CONTROL it (own + stream), not drive it.
    //
    // TRANSFERS ONLY (tag "squad" = EVT_SQUAD_MOVE). A fresh RECRUIT is
    // author-owned regardless of which tab it lands in: both sides recruit into
    // the shared rank-0 player tab, so a rank-owned claim here would make the
    // peer seize the OTHER side's recruit and mint a duplicate (recruit_sync run
    // 110524: join/baked rekeyed AND proxy-minted on the host). A squad-move is
    // the only edge that legitimately re-homes an existing unit across the
    // ownership partition. Only meaningful with squadSync_ (tabRank_ is latched
    // only then); a move into a tab not yet ranked locally reads destOwned=false
    // and self-corrects on a later edge.
    bool destOwned = false;
    if (squadSync_ && tag && tag[0] == 's') {
        std::map<std::pair<u32, u32>, unsigned int>::const_iterator rit =
            tabRank_.find(std::make_pair((u32)newK.c, (u32)newK.cs));
        if (rit != tabRank_.end())
            destOwned = ownRanks_.empty() ? (rit->second == 0u)
                                          : (ownRanks_.count(rit->second) != 0);
    }
    // The author owns a peer-tab hand even if a local tab census would rank it
    // into a tab we own; but a transfer INTO a tab we own is exactly the control
    // hand-off, so we claim it instead of pinning it peer.
    if (!destOwned) pinPeer_.insert(newK);
    // A chained edge (recruit then move, move then move) leaves the OLD key's
    // pin dead - drop it so the pin sets track only live hands.
    pinPeer_.erase(oldK);
    if (destOwned) pinPeer_.erase(newK);
    // Carry the down/death LATCH across the re-key. A body that dies (or is
    // KO'd) and then re-containers (host un-squads a corpse, tab move) streams
    // its EVT_DEATH/EVT_KNOCKOUT under the OLD hand, so deathLatched/koLatched
    // live on targets_[oldK]. Erasing that record below (without this) drops
    // the pin, and the join's local AI/KO-timer stands the corpse back up under
    // the new key - the "dead on one game, alive on the other" desync
    // (2026-07-15 bone-dog fight: serial 3332275456 died @container121, then an
    // EVT_SQUAD_MOVE re-keyed it and un-pinned the body). Snapshot the latches
    // here and re-seed them onto targets_[newK] so the corpse stays down.
    bool carryDeath = false, carryKo = false, carryDown = false;
    {
        std::map<Key, Driven>::iterator oldT = targets_.find(oldK);
        if (oldT != targets_.end()) {
            carryDeath = oldT->second.deathLatched;
            carryKo    = oldT->second.koLatched;
            carryDown  = oldT->second.downApplied;
        }
    }
    // Drop the old key's stream state too (run 192211: the interp TAIL of a
    // re-keyed hand kept replaying after the migration, went unresolved and
    // REQ'd a duplicate proxy). The grace stamp suppresses spawn REQs/mints
    // from any batch or reply still in flight for the dead key.
    targets_.erase(oldK);
    spawnReq_.erase(oldK);
    rekeyedOld_[oldK] = nowMs();
    if (carryDeath || carryKo) {
        Driven& nd = targets_[newK]; // stream fills interp; we seed only the latch
        coop::LatchState merged = coop::rekeyCarryLatch(
            coop::LatchState(carryDeath, carryKo, carryDown),
            coop::LatchState(nd.deathLatched, nd.koLatched, nd.downApplied));
        nd.deathLatched = merged.death;
        nd.koLatched    = merged.ko;
        nd.downApplied  = merged.down;
        char lb[200]; _snprintf(lb, sizeof(lb) - 1,
            "[event] REKEY-LATCH old=%u,%u,%u,%u,%u new=%u,%u,%u,%u,%u death=%d ko=%d",
            oldK.t, oldK.c, oldK.cs, oldK.i, oldK.s,
            newK.t, newK.c, newK.cs, newK.i, newK.s,
            carryDeath ? 1 : 0, carryKo ? 1 : 0);
        lb[sizeof(lb) - 1] = '\0'; coop::logLine(lb);
    }
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
        // Phase 2 crash hardening: this is an NPC proxy body, not a ground-item
        // proxy - destroy it with the NPC-correct SEH-guarded despawnProxyNpc
        // (removeWorldItemProxy is for world Item* proxies and mis-handles a
        // Character body; the same rekey path already uses despawnProxyNpc for
        // its control-flip phantom cull below).
        culled = engine::despawnProxyNpc(gw, mint) ? 1 : 0;
        repaired = 1;
    }
    if (c) {
        // Host-authority suppression may have already hidden the old copy
        // (its hand left the peer's stream the moment the edge re-containered
        // it) - bring the body back first.
        bool wasSuppressed = false;
        std::map<Key, Character*>::iterator sit = suppressed_.find(oldK);
        if (sit != suppressed_.end()) {
            engine::restoreNpc(gw, c);
            suppressed_.erase(sit);
            wasSuppressed = true;
        }
        // A body transferred into a tab WE own is ours to control now, not
        // drive - do NOT bind it as a proxy (that would make applyTargets drive
        // our own owned body). insertPeerMember below re-containers it into the
        // destination tab and pins the actual local hand OWNED so publishOwned
        // streams it; the drive teardown after that clears any residual stream
        // state for an immediate control hand-off.
        if (!destOwned) proxyByKey_[newK] = c;
        forceReqHands_.erase(newK); // bound - no force-REQ needed
        if (!destOwned) lifeSet(newK, LIFE_RESOLVED, "rekey");
        // Recruit audit (Ruka diagnosis): the body is now bound + host-driven,
        // but the two field symptoms ("join didn't see the new member" /
        // "transferring did nothing") are about SQUAD MEMBERSHIP, not the body.
        // Log whether the re-bound copy is actually in THIS client's player
        // squad and whether authority had it hidden - the decisive evidence for
        // whether the gap is membership/ownership (a follow-up feature) vs a
        // body-visibility bug. Recruit edges only (squad-move poll would spam).
        if (tag && tag[0] == 'r') {
            int inSquad = -1;
            __try {
                inSquad = engine::isPlayerSquad(gw,
                            reinterpret_cast<RootObject*>(c)) ? 1 : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) { inSquad = -1; }
            // Tab identity de-risk (Phase 1b step 1): is the recruiter-reported
            // target tab a container this client already knows as a player tab
            // (Case A, shared save-stable platoon - rank resolvable) or a NEW
            // one (Case B, runtime-minted platoon this client can't rank)? The
            // latch map is refreshed each publishOwned from OUR own squad, so a
            // hit means the target platoon exists locally and membership
            // insertion can target it by serial.
            std::pair<u32, u32> tk((u32)newK.c, (u32)newK.cs);
            std::map<std::pair<u32, u32>, unsigned int>::const_iterator tit =
                tabRank_.find(tk);
            int tabKnown = (tit != tabRank_.end()) ? 1 : 0;
            long tabRank  = (tit != tabRank_.end()) ? (long)tit->second : -1;
            char ab[208]; _snprintf(ab, sizeof(ab) - 1,
                "[recruit] REKEY-BIND new=%u,%u,%u,%u,%u playerSquad=%d "
                "wasSuppressed=%d tabKnown=%d rank=%ld c=%p",
                newK.t, newK.c, newK.cs, newK.i, newK.s, inSquad,
                wasSuppressed ? 1 : 0, tabKnown, tabRank, (void*)c);
            ab[sizeof(ab) - 1] = '\0'; coop::logLine(ab);
        }
        // Phase 1b membership: make the re-keyed body a REAL member of THIS
        // client's squad, in the tab the author reported, so a recruit/transfer
        // shows in the panel on BOTH games. For a PEER-owned tab, pinPeer_ (set
        // above) keeps it peer-owned so publishOwned never streams it and
        // applyTargets drives it via the isSquad walk-drive regime; a peer-owned
        // squad FOLLOWER is AI-suspended while driven (see applyTargets) so it
        // stops self-following its local leader and the walk-drive alone
        // reproduces the owner's run. For a tab WE own (destOwned - a transfer
        // into our squad), insertPeerMember pins the actual local hand OWNED so
        // publishOwned streams it and the local player controls it. Idempotent +
        // tab-aware, so a squad-move re-containers an existing member too.
        insertPeerMember(gw, c, newK, tag, destOwned);
        if (destOwned) {
            // Control hand-off: drop every residual DRIVE artifact so publishOwned
            // streams the body immediately and applyTargets never fights our own
            // now-owned copy. canonicalOf_ is keyed by Character* (the stamp that
            // otherwise keeps the drive-exclusion guard active for a full
            // drivenSeen_ horizon); targets_/spawnReq_ hold the peer stream state.
            canonicalOf_.erase(c);
            targets_.erase(oldK);  targets_.erase(newK);
            spawnReq_.erase(oldK); spawnReq_.erase(newK);
            // Phantom-mint suppression: stamp the CLAIMED hand into the re-keyed
            // grace so any batch/INFO reply for newK already in flight (the host
            // was streaming it until this instant) neither REQs nor mints. The
            // pinOwned_ vetoes above are the primary guard; this covers the
            // window before the pin is first consulted next tick.
            rekeyedOld_[newK] = nowMs();
            // Heal an already-minted phantom: a prior flip (or a lost race) may
            // have bound a proxy under newK. We own the hand now, so despawn the
            // stray proxy and drop its binding (manual 2026-07-17: Squint).
            std::map<Key, Character*>::iterator px = proxyByKey_.find(newK);
            if (px != proxyByKey_.end()) {
                if (px->second && px->second != c)
                    engine::despawnProxyNpc(gw, px->second);
                proxyByKey_.erase(px);
            }
            char cf[176]; _snprintf(cf, sizeof(cf) - 1,
                "[%s] CONTROL-FLIP claim new=%u,%u,%u,%u,%u (now owned)",
                (tag ? tag : "squad"), newK.t, newK.c, newK.cs, newK.i, newK.s);
            cf[sizeof(cf) - 1] = '\0'; coop::logLine(cf);
        }
    } else {
        // ok=0: no local body at the OLD hand and no proxy to migrate. The
        // recruit/move fired while this hand was OUTSIDE our interest (the
        // interest-split "join never saw Ruka" report). The host still streams
        // the NEW hand as an owned member, but the ordinary spawn-REQ path
        // proximity-gates it to OUR squad, so a recruit far from the join's
        // squad never REQs and the body never mints. Enroll the new hand in the
        // force-REQ set: the reliable edge PROVES it is a legit host body, so we
        // REQ it regardless of distance (the reply-side mint gate still judges).
        forceReqHands_.insert(newK);
        char fb[176]; _snprintf(fb, sizeof(fb) - 1,
            "[%s] REKEY-FALLBACK new=%u,%u,%u,%u,%u force-REQ (no local body)",
            tag, newK.t, newK.c, newK.cs, newK.i, newK.s);
        fb[sizeof(fb) - 1] = '\0'; coop::logLine(fb);
    }
    life_.erase(oldK); // the old hand's journey ends with the re-key
    char rb[224]; _snprintf(rb, sizeof(rb) - 1,
        "[%s] REKEY old=%u,%u,%u,%u,%u new=%u,%u,%u,%u,%u ok=%d repaired=%d culled=%d",
        tag, oldK.t, oldK.c, oldK.cs, oldK.i, oldK.s,
        newK.t, newK.c, newK.cs, newK.i, newK.s, c ? 1 : 0, repaired, culled);
    rb[sizeof(rb) - 1] = '\0'; coop::logLine(rb);
}

void Replicator::insertPeerMember(GameWorld* gw, Character* c, const Key& newK,
                                  const char* tag, bool ownIt) {
    if (!c) return;
    unsigned int nh[5] = { newK.t, newK.c, newK.cs, newK.i, newK.s };
    bool ok = engine::joinPlayerSquadAt(gw, c, nh);
    // Pin the body's ACTUAL local hand. setFaction assigns a local platoon index
    // that usually DIFFERS from the owner's streamed hand (each engine numbers
    // its platoon independently), and publishOwned keys ownership by the captured
    // LOCAL hand - so we must pin THAT hand, not just newK. ownIt selects the
    // side of the partition:
    //   * ownIt=false (recruit / move into the PEER's tab): pin PEER-owned. Without
    //     it the re-containered body escapes the owner-hand pin (newK), streams as
    //     owned, and the owner mints a DUPLICATE proxy of its own recruit
    //     (recruit_sync run 095843: join squad 8 vs host 6).
    //   * ownIt=true (transfer INTO a tab we own - the control hand-off): pin
    //     OWNED so publishOwned streams the body and the local player controls it.
    // readObjectHand re-reads post-move in Key order [type,container,
    // containerSerial,index,serial].
    if (ownIt) { pinOwned_.insert(newK); pinPeer_.erase(newK); }
    unsigned int lh[5] = { 0, 0, 0, 0, 0 };
    bool haveLh = false;
    __try {
        haveLh = engine::readObjectHand(reinterpret_cast<RootObject*>(c), lh);
    } __except (EXCEPTION_EXECUTE_HANDLER) { haveLh = false; }
    Key lk; lk.t = lh[0]; lk.c = lh[1]; lk.cs = lh[2]; lk.i = lh[3]; lk.s = lh[4];
    bool sameAsNew = (lk.t == newK.t && lk.c == newK.c && lk.cs == newK.cs &&
                      lk.i == newK.i && lk.s == newK.s);
    bool pinnedLocal = false;
    if (haveLh && (lh[0] | lh[1] | lh[2] | lh[3] | lh[4]) && !sameAsNew) {
        if (ownIt) { pinPeer_.erase(lk); pinOwned_.insert(lk); }
        else       { pinOwned_.erase(lk); pinPeer_.insert(lk); }
        pinnedLocal = true;
    }
    int inSquad = -1;
    __try {
        inSquad = engine::isPlayerSquad(gw, reinterpret_cast<RootObject*>(c)) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { inSquad = -1; }
    char b[240]; _snprintf(b, sizeof(b) - 1,
        "[%s] MEMBER new=%u,%u,%u,%u,%u insert=%d playerSquad=%d "
        "localHand=%u,%u,%u,%u,%u pinnedLocal=%d ownIt=%d",
        (tag ? tag : "recruit"), newK.t, newK.c, newK.cs, newK.i, newK.s,
        ok ? 1 : 0, inSquad, lk.t, lk.c, lk.cs, lk.i, lk.s,
        pinnedLocal ? 1 : 0, ownIt ? 1 : 0);
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
}


} // namespace coop
