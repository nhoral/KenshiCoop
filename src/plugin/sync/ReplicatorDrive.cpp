// ReplicatorDrive.cpp - the join-side DRIVE path (monolith split from
// Replicator.cpp, 2026-07-12): applyTargets (the per-frame walk/park/combat/
// carry/furniture/stealth drive of every peer-authoritative body, incl. the
// interp buffer consumption and hard-snap gates), applyRest (rest placement +
// pose reproduction), sweepCarries (carry/furniture self-heal teardown) and
// logHardSnap (throttled snap evidence).
//
// Shared hubs: owns targets_ consumption and drivenChars_/drivenSeen_/
// canonicalOf_ stamping (pruned on drivenSeen_'s horizon; pointers compared,
// never dereferenced after prune); writes suppressed_, life_ via lifeSet.
// Must NOT: change any log string - log phrasing is the API consumed by the
// PowerShell oracles (see resources/CODE_MAP.md, log-tag index).

#include "ReplicatorUtil.h"

namespace coop {

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
    // Own-squad positions, for scoping the smoothness/march oracles to the
    // historical near-bubble population (Phase 2). The mid tier drives bodies
    // far outside it, where Kenshi throttles offscreen character updates -
    // their stepwise rendering is an engine LOD fact, not an interp fault,
    // and it buried the gates' real signal (run 111445: one boundary walker
    // charged 341 zero-frames of 365 active).
    static EntityState oracleSquad[16]; // main-thread only
    unsigned int oracleSquadN = engine::captureSquad(gw, false, oracleSquad, 16);
    for (std::map<Key, Driven>::iterator it = targets_.begin(); it != targets_.end(); ++it) {
        // Never drive a body WE own: we control + stream it locally, the peer drives
        // its copy from our stream. The disjoint partition + no local loopback means
        // our own hand shouldn't appear in targets_, but guard regardless (a stray
        // self-owned sample would otherwise fight our own control every frame).
        if (ownHands_.find(it->first) != ownHands_.end()) continue;
        // Phase 1b (phantom "Squint" fix): also never drive/seed a hand we PIN
        // owned. ownHands_ is rebuilt each publish from the LOCAL captured hand;
        // a control-flip claim pins the OWNER's streamed hand (newK) owned too,
        // and the host's last in-flight batches for newK arrive after the flip.
        // Without this veto they seed unresolved (newK no longer resolves - the
        // body moved to a new local index), REQ, and mint a phantom proxy that
        // chases the real body (manual 2026-07-17: Squint following Adi).
        if (pinOwned_.find(it->first) != pinOwned_.end()) continue;
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
                        canonicalOf_[c] = it->first;
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
        bool viaProxy = false;
        if (!c && (spawnSync_ || recruitSync_)) {
            std::map<Key, Character*>::iterator pit = proxyByKey_.find(it->first);
            if (pit != proxyByKey_.end()) { c = pit->second; viaProxy = true; }
        }
        // Phase 2 crash hardening: a minted proxy pointer can be freed by the
        // engine in the window between the ~1 Hz syncSpawns liveness sweep and
        // this 20 Hz drive (mint/zone churn on town/camp approach). Prove the
        // pointer is still live with a cheap SEH-guarded hand read BEFORE we
        // dereference it below; a dead read means the body was reaped, so unbind
        // and let the census/REQ machinery re-mint if the host still streams it.
        // Only minted proxies need this - engine::resolve() returns bodies from
        // the engine's own live lookup. targets_ is being iterated here, so we
        // only touch the OTHER maps and let the 30 s targets_ prune (or a clean
        // next-tick re-resolve) handle this key.
        if (viaProxy) {
            unsigned int lh[5];
            if (!engine::readHand(c, lh)) {
                char sb[160]; _snprintf(sb, sizeof(sb) - 1,
                    "[drive] STALE unbind hand=%u,%u,%u,%u,%u c=%p",
                    it->first.t, it->first.c, it->first.cs, it->first.i,
                    it->first.s, (void*)c);
                sb[sizeof(sb) - 1] = '\0'; coop::logLine(sb);
                proxyByKey_.erase(it->first);
                spawnReq_.erase(it->first);   // allow a fresh REQ/mint cycle
                lifeSet(it->first, LIFE_UNKNOWN, "drive-stale");
                continue;
            }
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
        // Phase 0 crash breadcrumb (KENSHICOOP_DEBUG_DRIVE_TRAIL=1, OFF by
        // default = zero cost): name the PROXY-driven body just before we drive
        // it. CoopLog flushes every line, so the last flushed [drive] proxy line
        // before a hard crash identifies the body we touched - the UAF-on-stale-
        // proxy hypothesis (approach-town/camp mint churn). Native resolves are
        // save-stable and excluded to keep the trail focused on minted bodies.
        if (viaProxy) {
            static int driveTrail = -1;
            if (driveTrail < 0) {
                const char* e = getenv("KENSHICOOP_DEBUG_DRIVE_TRAIL");
                driveTrail = (e && e[0] == '1') ? 1 : 0;
            }
            if (driveTrail) {
                char tb[160]; _snprintf(tb, sizeof(tb) - 1,
                    "[drive] proxy hand=%u,%u,%u,%u,%u c=%p",
                    it->first.t, it->first.c, it->first.cs, it->first.i,
                    it->first.s, (void*)c);
                tb[sizeof(tb) - 1] = '\0'; coop::logLine(tb);
            }
        }
        drivenChars_.insert(c);
        drivenSeen_[c] = now; // recently-driven grace for the authority passes
        canonicalOf_[c] = it->first; // capture translation (combat subjects)
        debugMark(c, 0, "DRV");

        // Every driven body is damage-guarded (locally-simulated hits must not
        // mutate the local-only medical model; outcomes arrive as host events).
        if (dmgGuard_) engine::addDamageGuard(c);

        // Owner-authoritative death veto (2026-07-15). The damage guard blocks
        // NEW melee wounds, but a lethal frame in an unguarded window (stream
        // stall past the starve-hold, above) or a non-melee source can still
        // flip this copy's local medical.dead - and the medical model is
        // local-only, so nothing reconciles it while the OWNER still reports the
        // body alive (no BODY_DEAD in the stream, no latched EVT_DEATH). That is
        // the "dead on one game, alive on the other" desync. Un-kill it: death
        // may only take hold on the peer via the owner's reliable EVT_DEATH.
        if (dmgGuard_ && !(out.bodyState & BODY_DEAD) && !d.deathLatched &&
            (engine::readBodyState(c) & BODY_DEAD)) {
            if (engine::vetoLocalDeath(c)) {
                char b[128]; _snprintf(b, sizeof(b) - 1,
                    "[death] veto hand=%u,%u", out.hIndex, out.hSerial);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        float ax, ay, az;
        bool haveActual = engine::readPos(c, &ax, &ay, &az);
        bool hostMoving = (out.cMoving != 0) || (out.cSpeed > MOVE_EPS);
        // A conscious bed pose (USE_BED / USE_BED_ORDER / SLEEP_ON_FLOOR) is a
        // STATIONARY anchored pose, but a sleeper streams currentlyMoving=1 (the
        // climb-in / in-bed idle sets the movement flag while cSpeed stays 0).
        // For a DRIVEN SQUAD member genuinelyMoving == hostMoving, so that flag
        // routes the sleeper down the walk/snap path - it gets position-snapped
        // to the streamed transform instead of reproducing the bed pose via
        // applyRest, so it STANDS on the bed instead of lying in it (manual
        // 2026-07-17, ~4/5 tries; the 1/5 that worked caught cMoving==0). A
        // bedded body is never walking: anchor it so the rest/pose path runs.
        if (engine::taskIsBedPose((int)out.task)) hostMoving = false;

        // Two drive regimes (see Engine::isLocalPlayerChar):
        //   * SQUAD member - a player-controlled body, inert when uncontrolled, so
        //     the engine obeys our move-order: true grounded walk-drive (Stage 3).
        //   * world NPC - fully AI-simulated locally, so a move-order gets fought;
        //     drive it kinematically (teleport wins as the last word) + mirror the
        //     host locomotion so it still animates. Grounded engine-walk + real
        //     sit/idle poses for NPCs arrive in Stage 5 (AI quiet-in-place).
        bool isSquad = engine::isLocalPlayerChar(gw, c);

        // ---- Phase A jail-observe (KENSHICOOP_JAIL_OBSERVE, read-only spike) ----
        // For a peer-owned captive (the join's jailed PC as driven on the host),
        // temporarily let the host's LOCAL sim run it unopposed: skip drive,
        // AI-suspend AND furniture self-heal, and log the full trajectory. This
        // reveals what the host guard's "put to work" actually does to the body
        // (does it relocate to a fixed work spot -> B-R, or walk a job round ->
        // B-W). The body is still damage-guarded (harmless) and in drivenChars_
        // (keeps host-authority suppression off) from above. Knob OFF by default.
        if (jailObserve_) {
            engine::FurnitureRead ofr;
            bool ofrOk = engine::readFurniture(c, &ofr) && ofr.valid;
            int localKindO = ofrOk ? ofr.kind : 0;
            int slaveO = engine::readSlaveState(c);
            bool captive = localKindO != 0 || slaveO > 0 ||
                           (out.bodyState & (BODY_IN_CAGE | BODY_CHAINED | BODY_IN_BED));
            if (captive) {
                int streamKindO = (out.bodyState & BODY_IN_BED) ? 1
                                : ((out.bodyState & BODY_IN_CAGE) ? 2
                                : ((out.bodyState & BODY_CHAINED) ? 3 : 0));
                // Log on any kind change, a >5u move, or every 500ms - enough to
                // reconstruct the cage -> work trajectory without flooding.
                JailObs& jo = jailObs_[it->first];
                float dx = ax - jo.x, dy = ay - jo.y, dz = az - jo.z;
                float moved2 = haveActual ? (dx*dx + dy*dy + dz*dz) : 0.0f;
                bool first = (jo.ms == 0);
                if (first || localKindO != jo.kind || moved2 > 25.0f ||
                    (now - jo.ms) >= 500) {
                    float step = (haveActual && !first) ? std::sqrt(moved2) : 0.0f;
                    jo.kind = localKindO; jo.ms = now;
                    if (haveActual) { jo.x = ax; jo.y = ay; jo.z = az; }
                    char b[256];
                    _snprintf(b, sizeof(b) - 1,
                        "[jail] OBSERVE hand=%u,%u localKind=%d streamKind=%d chained=%d "
                        "inWhat=%u,%u slave=%d task=%u raw=%u pos=%.1f,%.1f,%.1f step=%.1f",
                        out.hIndex, out.hSerial, localKindO, streamKindO,
                        (out.bodyState & BODY_CHAINED) ? 1 : 0,
                        ofrOk ? ofr.furn[3] : 0u, ofrOk ? ofr.furn[4] : 0u,
                        slaveO, out.task, out.rawTask,
                        haveActual ? ax : 0.0f, haveActual ? ay : 0.0f,
                        haveActual ? az : 0.0f, step);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
                // Do NOT drive / suspend / self-heal this body: let the host sim run it.
                d.parked = false; d.haveDest = false;
                if (haveActual) { d.haveActual = true; d.lx = ax; d.ly = ay; d.lz = az; }
                continue;
            }
        }

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
        // Non-owner unlock guard (protocol 42): a shackled prisoner (isChained ->
        // BODY_CHAINED) can ALSO be in a cage/bed (IN_PRISON/IN_BED). readFurniture's
        // kind priority reports the cage (kind=2), so the occupancy self-heal below
        // only ever re-asserts the CAGE - never the shackle. A peer PC's local
        // lockpick (or AI break-out) then leaves the owner's prisoner UNLOCKED on
        // this peer while the owner still streams BODY_CHAINED (the reported "the
        // other client's PC unlocked the shackle" desync, cage2). Re-assert
        // setChainedMode INDEPENDENTLY of the furniture kind: remember the owner
        // (slaveOwner) while the driven copy is locally chained, and re-apply if it
        // has lost the chain. Scoped to the masked case (chained AND in a cage/bed);
        // a pole-only chained body is handled by the kind=3 self-heal below.
        if (chainSync_ && (out.bodyState & BODY_CHAINED) &&
            (out.bodyState & (BODY_IN_CAGE | BODY_IN_BED))) {
            engine::ShackleRead lsr;
            bool haveSr = engine::readShackle(c, &lsr) && lsr.valid;
            if (haveSr && lsr.chained &&
                (lsr.owner[3] != 0 || lsr.owner[4] != 0)) {
                for (int fi = 0; fi < 5; ++fi) d.chainOwner[fi] = lsr.owner[fi];
                d.haveChainOwner = true;
            }
            if (haveSr && !lsr.chained &&
                (now - d.chainHealTick) >= FURN_HEAL_MS) {
                d.chainHealTick = now;
                // Remembered owner if we have one, else the body's own slaveOwner
                // (furnHand=0) - covers a caged slave that activated unshackled.
                bool ok = d.haveChainOwner
                    ? engine::applyFurniture(gw, c, d.chainOwner, 3, true)
                    : engine::applyFurniture(gw, c, 0, 3, true);
                engine::endAction(c);
                char b[160]; _snprintf(b, sizeof(b) - 1,
                    "[furn] SHACKLE RELOCK occ=%u,%u owner=%u,%u src=%s ok=%d",
                    out.hIndex, out.hSerial, d.chainOwner[3], d.chainOwner[4],
                    d.haveChainOwner ? "remembered" : "slaveOwner", ok ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        if (furnSync_ && !engine::taskIsBedPose((int)out.task)) {
            // Chained/pole prisoner (protocol 41) rides this carve-out as
            // kind=3 (Character::isChained). Gated by chainSync_ so it can be
            // turned off without disabling bed/cage occupancy.
            int streamKind = (out.bodyState & BODY_IN_BED) ? 1
                           : ((out.bodyState & BODY_IN_CAGE) ? 2
                           : ((chainSync_ && (out.bodyState & BODY_CHAINED)) ? 3 : 0));
            engine::FurnitureRead lfr;
            bool haveFr = engine::readFurniture(c, &lfr);
            int localKind = (haveFr && lfr.valid) ? lfr.kind : 0;
            if (localKind == 3 && !chainSync_) localKind = 0;
            // Jail put-to-work desync spike (KENSHICOOP_JAIL_PROBE, read-only):
            // the DRIVEN view of a peer-owned captive (the host's copy of the
            // join's jailed PC). streamKind is what the owner reports;
            // localKind is where our copy actually sits. A streamKind=2/3 with
            // localKind=0 (or vice-versa) is the twitch. Pairs with side=own.
            if (jailProbe_ && (streamKind != 0 || localKind != 0)) {
                static std::map<Key, unsigned long> s_drvJailMs;
                Key jk = keyOf(out);
                std::map<Key, unsigned long>::iterator jt = s_drvJailMs.find(jk);
                if (jt == s_drvJailMs.end() || (now - jt->second) >= 250) {
                    s_drvJailMs[jk] = now;
                    int slave = engine::readSlaveState(c);
                    char jb[224];
                    _snprintf(jb, sizeof(jb) - 1,
                              "[jail] STATE side=drv hand=%u,%u streamKind=%d localKind=%d "
                              "chained=%d slaveOwner=%u,%u isSlave=%d task=%u raw=%u "
                              "pos=%.1f,%.1f,%.1f mv=%d",
                              out.hIndex, out.hSerial, streamKind, localKind,
                              (out.bodyState & BODY_CHAINED) ? 1 : 0,
                              lfr.furn[3], lfr.furn[4], slave, out.task, out.rawTask,
                              out.x, out.y, out.z, out.cMoving ? 1 : 0);
                    jb[sizeof(jb) - 1] = '\0'; coop::logLine(jb);
                }
            }
            // Remember the owner hand while locally chained, so a lost/late
            // reliable ENTER (or an AI break-out) can be re-applied below (the
            // continuous BODY_CHAINED bit carries no owner).
            if (localKind == 3) {
                for (int fi = 0; fi < 5; ++fi) d.chainOwner[fi] = lfr.furn[fi];
                d.haveChainOwner = (lfr.furn[3] != 0 || lfr.furn[4] != 0);
            }
            // Chained fall-through (world_parity 2026-07-17): kind=3 (chained)
            // is an EQUIP state, not a transform anchor - a working slave
            // walks its mining round while shackled, and the camp save starts
            // the PCs chained, so the hold below froze every chained body at
            // its entry position (the host's PC rendered 1600 u away on the
            // join) and - at rest - left the local copy's own queued job
            // running (the join's Leaf walked its local mining round while
            // the host mined in place: the hold skips applyRest, so the
            // host's task was never reproduced and the stale local task never
            // cleared). Keep only the CHAINED-STATE heal here (re-chain a
            // locally-unchained copy, throttled) and fall through: the
            // unified drive owns transform AND task for chained bodies (it
            // AI-suspends driven bodies itself; applyRest reproduces the
            // host's work pose at rest). Cage/bed (kinds 1-2) remain true
            // transform anchors below.
            if (streamKind == 3) {
                if (haveFr && localKind != 3 &&
                    (now - d.furnHealTick) >= FURN_HEAL_MS) {
                    d.furnHealTick = now;
                    // A local bed/cage the stream does NOT vouch for is a
                    // transform anchor at the wrong spot (the host's guard
                    // jailed its copy locally / a stale attach) - the drive's
                    // parks and walks all no-op against it (Flashbox held a
                    // 33 u offset for 60 s). Break it before re-chaining.
                    if (localKind == 1 || localKind == 2) {
                        engine::applyFurniture(gw, c, lfr.furn, localKind, false);
                        engine::endAction(c);
                    }
                    bool ok = d.haveChainOwner
                        ? engine::applyFurniture(gw, c, d.chainOwner, 3, true)
                        : engine::applyFurniture(gw, c, 0, 3, true);
                    engine::endAction(c);
                    char b[160]; _snprintf(b, sizeof(b) - 1,
                        "[furn] HEAL ENTER occ=%u,%u kind=3 was=%d ok=%d (fallthrough)",
                        out.hIndex, out.hSerial, localKind, ok ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    // Jail spike (KENSHICOOP_JAIL_PROBE, read-only): quantify the
                    // re-seat as the user sees it. divergence = how far the local
                    // copy had drifted from the owner's streamed pos when we
                    // re-chained it (the visible teleport magnitude); localStep =
                    // how far our copy moved since last tick while nominally
                    // seated (>0 => the driven copy's OWN local AI is walking it -
                    // the "exit cage to run" half of the oscillation).
                    if (jailProbe_) {
                        float dvg = haveActual ? std::sqrt(
                            (ax-out.x)*(ax-out.x)+(ay-out.y)*(ay-out.y)+(az-out.z)*(az-out.z)) : 0.0f;
                        float lstep = (haveActual && d.haveActual) ? std::sqrt(
                            (ax-d.lx)*(ax-d.lx)+(ay-d.ly)*(ay-d.ly)+(az-d.lz)*(az-d.lz)) : 0.0f;
                        char s[192]; _snprintf(s, sizeof(s) - 1,
                            "[jail] SNAP hand=%u,%u kind=3 was=%d divergence=%.1f localStep=%.1f ok=%d",
                            out.hIndex, out.hSerial, localKind, dvg, lstep, ok ? 1 : 0);
                        s[sizeof(s) - 1] = '\0'; coop::logLine(s);
                    }
                }
            } else if (streamKind != 0) {
                d.furnNoSeeTick = 0;
                // A jailed/bedded DRIVEN body must not run its own decision layer.
                // A CONSCIOUS caged squad member (an arrested player) otherwise
                // "releases from jail", fights the guards and walks out of the cage
                // while the self-heal re-seats it every FURN_HEAL_MS - the "teleported
                // in and out of jail" oscillation (2026-07-15). Squad members are
                // normally never AI-suspended (the !isSquad gate on the locomotion
                // path below), but a body the host is holding IN furniture is the
                // exception: suspend its decisions so it stays put. The suspend set is
                // rebuilt every drive tick, so this self-clears the moment the host
                // stops streaming the furniture bit (body released) and its AI resumes.
                if (aiSuspend_) engine::addAiSuspend(c);
                if (haveFr && localKind != streamKind &&
                    (now - d.furnHealTick) >= FURN_HEAL_MS) {
                    d.furnHealTick = now;
                    // Chain (kind 3) has no searchable building and needs the
                    // OWNER: re-apply setChainedMode with the remembered owner,
                    // or - for a prisoner that spawned into interest already
                    // UNCHAINED (no owner ever remembered) - via its OWN
                    // slaveOwner (furnHand=0, set from the shared save). Without
                    // this fallback an obedient working slave that activated
                    // unshackled+jobless on the join was never re-locked and its
                    // local AI fled, drawing the join guards into a chase the host
                    // never saw (manual 2026-07-17, camp working prisoners).
                    // Cages/beds re-find the nearest matching fixture by name.
                    bool ok = (streamKind == 3)
                        ? (d.haveChainOwner
                           ? engine::applyFurniture(gw, c, d.chainOwner, 3, true)
                           : engine::applyFurniture(gw, c, 0, 3, true))
                        : engine::enterFurnitureNearPos(
                            gw, c, streamKind, out.x, out.y, out.z, FURN_MATCH_DIST);
                    // Drop the in-progress escape/attack action so the body doesn't
                    // finish breaking out before the suspend takes hold. endAction is
                    // SEH-guarded (same call the rest-park path uses).
                    engine::endAction(c);
                    char b[160]; _snprintf(b, sizeof(b) - 1,
                        "[furn] HEAL ENTER occ=%u,%u kind=%d ok=%d",
                        out.hIndex, out.hSerial, streamKind, ok ? 1 : 0);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                    { char q[160]; _snprintf(q, sizeof(q) - 1,
                        "[furn] cage-quiet occ=%u,%u kind=%d",
                        out.hIndex, out.hSerial, streamKind);
                      q[sizeof(q) - 1] = '\0'; coop::logLine(q); }
                    // Jail spike (see kind=3 SNAP above): divergence = teleport
                    // magnitude of this re-seat; localStep = drift since last tick
                    // while nominally in the cage/bed (>0 => local AI walked it).
                    if (jailProbe_) {
                        float dvg = haveActual ? std::sqrt(
                            (ax-out.x)*(ax-out.x)+(ay-out.y)*(ay-out.y)+(az-out.z)*(az-out.z)) : 0.0f;
                        float lstep = (haveActual && d.haveActual) ? std::sqrt(
                            (ax-d.lx)*(ax-d.lx)+(ay-d.ly)*(ay-d.ly)+(az-d.lz)*(az-d.lz)) : 0.0f;
                        char s[192]; _snprintf(s, sizeof(s) - 1,
                            "[jail] SNAP hand=%u,%u kind=%d was=%d divergence=%.1f localStep=%.1f ok=%d",
                            out.hIndex, out.hSerial, streamKind, localKind, dvg, lstep, ok ? 1 : 0);
                        s[sizeof(s) - 1] = '\0'; coop::logLine(s);
                    }
                }
                d.parked = false; d.haveDest = false;
                if (haveActual) { d.haveActual = true; d.lx = ax; d.ly = ay; d.lz = az; }
                continue;
            } else if (localKind == 1 && hostMoving) {
                // Bed fast-exit (conscious sleep wake): a bed pose has NO
                // reliable EXIT edge (publishOwned suppresses furniture edges
                // while taskIsBedPose), so a host that wakes and WALKS would
                // otherwise leave the join copy frozen in bed until the
                // FURN_EXIT_MS debounce - and a conscious bed sleeper is never
                // AI-suspended, so its local AI can re-sleep it in the gap
                // ("stays sleeping" desync, pole save 2026-07-17). The host
                // genuinely moving is an unambiguous "left the bed" signal (a
                // caged/chained body - kind 2/3 - never moves, so this can't
                // false-trigger there and they stay on the debounce): eject NOW,
                // drop the in-progress sleep action, release the held pose, and
                // FALL THROUGH (no continue) so the unified drive follows the
                // host this same tick.
                bool ok = engine::applyFurniture(gw, c, lfr.furn, 1, false);
                engine::endAction(c);
                d.furnNoSeeTick = 0;
                d.taskApplied = false; d.issuedTask = TASK_NONE; d.taskNoneTick = 0;
                char bfe[160]; _snprintf(bfe, sizeof(bfe) - 1,
                    "[furn] BED FAST-EXIT occ=%u,%u ok=%d hostMoving=1",
                    out.hIndex, out.hSerial, ok ? 1 : 0);
                bfe[sizeof(bfe) - 1] = '\0'; coop::logLine(bfe);
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
        // A latched EVT_DEATH/EVT_KNOCKOUT forces the down treatment every tick,
        // which is what keeps a corpse pinned. That latch lives on this Driven
        // record, so it MUST survive a hand re-key - rekeyPeerBody carries
        // deathLatched/koLatched from the old key onto the new one (2026-07-15);
        // without that carry a dead body that re-containers would fall through
        // to the drive path below and the local AI would stand it back up.
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
                // Engagement escalation (world_parity camp, run 005538): a
                // driven guard beating a locally PLAYER-OWNED body (escaped
                // prisoner PC) accepted the goal-path attack (r=2) every
                // reissue but never engaged - its running local AI re-decides
                // against fighting the player squad and drops the goal. After
                // a failed goal-path episode, flush via the ORDER path too
                // (player orders outrank AI goals - the seat-injection
                // precedent). Backoff-throttled like every reissue.
                if (!breakSeat && !hostWaiting && !localFighting &&
                    d.combatOrders >= 1)
                    breakSeat = true;
                // Wrong-target divergence (Phase 3, 2026-07-16): the local brawl
                // grabbed a DIFFERENT body than the host reports. Drop the wrong
                // lock before re-ordering so the engine re-acquires the host's
                // target instead of re-diverging every episode (the maxPersist /
                // wrongTgt driver - a snap back doesn't fix the cause, the local
                // AI just re-locks the wrong enemy). Throttled by the same re-issue
                // backoff, so this is not a per-frame clearGoals thrash.
                if (wrongLocalTgt) engine::clearGoals(c);
                int r = engine::applyCombat(c, out, breakSeat);
                if (breakSeat && r == 2) {
                    d.taskApplied = false; d.taskBad = false;
                    d.issuedTask  = TASK_NONE;
                }
                // Final escalation (world_parity camp, run 011417): the copy
                // accepted BOTH the goal and order attacks yet still never
                // engaged (localFight=0, task 65535 all window) - its running
                // AI validates the victim (locally player-owned, non-hostile
                // faction: the escaped-prisoner recapture) and drops the
                // committed goal. attackTarget is that AI's own commit entry,
                // past the validation. Only after two failed ordered episodes
                // so ordinary NPC-vs-NPC fights never hit this path.
                int fr = -2; // -2 = not attempted
                if (r == 2 && !hostWaiting && !localFighting &&
                    d.combatOrders >= 2) {
                    fr = engine::forceAttack(c, out);
                }
                d.combatArmed = true; d.combatTick = now;
                if (d.combatOrders < 1000000u) ++d.combatOrders;
                ++combatOrder_;
                d.combatTgtIdx = out.sIndex; d.combatTgtSer = out.sSerial;
                { char b[192]; _snprintf(b, sizeof(b) - 1,
                    "[combat] order hand=%u,%u tgt=%u,%u localFight=%d r=%d wait=%d n=%u%s%s",
                    out.hIndex, out.hSerial, out.sIndex, out.sSerial,
                    localFighting ? 1 : 0, r, hostWaiting ? 1 : 0, d.combatOrders,
                    breakSeat ? " seatbrk=1" : "", fr == -2 ? "" :
                    (fr == 2 ? " forced=1" : (fr == 1 ? " forced=notgt" :
                     (fr == 0 ? " forced=nofn" : " forced=fault"))));
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
                // Source (host) speed estimate: a leave on a FAST source is a real
                // chase (a teleport is correct there); a big drift on a STATIONARY
                // source (srcVel~0) is melee churn or a wrong-place body - converge,
                // never warp. Same 2-sample estimate the locomotion gate uses.
                float srcVel = 0.0f;
                { EntityState nn; float cvx = 0.0f, cvy = 0.0f, cvz = 0.0f;
                  if (d.interp.latest(&nn, &cvx, &cvy, &cvz))
                      srcVel = std::sqrt(cvx * cvx + cvy * cvy + cvz * cvz); }
                // Convergence-first correction (2026-07-16 smoothness pass). A
                // correctly-engaged fight owns its footwork up to the churn ceiling
                // (COMBAT_SNAP_DIST); every other copy (arming / idle / WAITING /
                // wrong-target) converges above a soft band - tighter for a waiting
                // stance that should not wander. Above the leave band the body
                // FAST-SLIDES to the host pose (a quick walk, gait preserved); an
                // INSTANT teleport is reserved for a true LEAVE only (very far, a
                // source teleport, or a drift that SAT over the band for
                // COMBAT_CONVERGE_MS on a moving source). Momentary interp/footwork
                // spikes converge - they never warp.
                bool correctFight = localFighting && !wrongLocalTgt;
                float softBand  = hostWaiting ? COMBAT_WAIT_DIST : combatSoftDist_;
                float leaveBand = correctFight ? combatSnapDist_ : softBand;
                if (drift > combatSnapDist_) {
                    if (d.combatOverTick == 0) d.combatOverTick = now;
                } else {
                    d.combatOverTick = 0;
                }
                bool sustained = d.combatOverTick != 0 &&
                                 (now - d.combatOverTick) >= combatConvergeMs_;
                bool srcTeleport = (d.interp.lastMode() == EntityInterp::SM_SEG_SNAP);
                // A WAITING stance has no chase to justify a warp - it only converges.
                bool trueLeave = !hostWaiting &&
                                 (drift > combatBigSnapDist_ || srcTeleport ||
                                  (sustained && srcVel >= COMBAT_SNAP_VEL));
                if (trueLeave &&
                    (now - d.combatSnapTick) >= COMBAT_SNAP_COOL_MS) {
                    engine::applyRaw(c, out);
                    d.combatSnapTick = now;
                    d.combatOverTick = 0;
                    d.haveDest = false; // position jumped: force a fresh slide dest
                    ++d.combatSnapCount; ++combatSnapTotal_;
                    if (wrongLocalTgt) ++combatWrongTgt_;
                    { char b[224]; _snprintf(b, sizeof(b) - 1,
                        "[combat] snap hand=%u,%u drift=%.1f srcVel=%.1f "
                        "localFight=%d wrongTgt=%d arming=%d wait=%d seg=%lu n=%lu",
                        out.hIndex, out.hSerial, drift, srcVel,
                        localFighting ? 1 : 0, wrongLocalTgt ? 1 : 0,
                        arming ? 1 : 0, hostWaiting ? 1 : 0,
                        d.interp.lastSegMs(), d.combatSnapCount);
                      b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
                } else if (drift > leaveBand) {
                    // Fast catch-up slide: speed scales with drift (~1 s to close),
                    // clamped so a big gap glides quickly without a teleport. walkTo
                    // floors sub-1 speeds to RUN, so small converges stay a walk.
                    // Re-issue ONLY when the host pose moved past REISSUE_DIST since
                    // the last slide dest: a per-frame walkTo restarts the path and
                    // renders as stutter (the locomotion-drive lesson) - the exact
                    // smoothness regression this throttle removes.
                    float moved = d.haveDest
                        ? dist3(out.x, out.y, out.z, d.dx, d.dy, d.dz)
                        : (REISSUE_DIST + 1.0f);
                    if (moved > REISSUE_DIST) {
                        // Speed must EXCEED the source's own pace or a chase never
                        // closes: the copy would trail at a fixed gap, stay over the
                        // band, and eventually teleport (the maxPersist=9 driver at
                        // N=40). Match the streamed locomotion speed and ADD a drift-
                        // proportional catch-up, capped at 2.5x the source pace (the
                        // locomotion-drive envelope), with COMBAT_SLIDE_MAX as a
                        // floor so a stationary-source gap still closes quickly.
                        float base = (out.cSpeed > 1.0f) ? out.cSpeed : 12.0f;
                        float spd = base + drift;
                        float cap = base * 2.5f;
                        if (cap < combatSlideMax_) cap = combatSlideMax_;
                        if (spd > cap) spd = cap;
                        engine::walkTo(c, out.x, out.y, out.z, spd);
                        ++combatSoftWalk_;
                        if (drift > combatSnapDist_) ++combatSlide_;
                        d.haveDest = true; d.dx = out.x; d.dy = out.y; d.dz = out.z;
                    }
                } else {
                    // Converged inside the leave band: release the slide dest so the
                    // next genuine drift re-issues a fresh walk (and so a body that
                    // exits combat doesn't inherit a stale combat destination).
                    d.haveDest = false;
                }
            }
            d.parked = false;
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
        // Phase 2 mid-band tier: the per-entity stream cadence. Near-tier
        // bodies see ~50 ms segments; a round-robin mid-tier body sees the
        // rotation period (~500 ms +, growing with the mid population). The
        // walk-hold and the lead point both scale with it, so a sparsely
        // sampled walker neither flips to rest between samples nor reaches
        // its lead point and idles waiting for the next one. Cadence = the
        // LARGER of the newest segment and the smoothed inter-arrival EMA:
        // the newest segment alone reads ~50 ms whenever the 1 Hz census
        // rebuild reshuffles the rotation and a hand lands two slices in a
        // row (run 105155: mid bodies misclassified as near-tier, dragging
        // their sparse-stream rendering into the near gates).
        unsigned long segMs = d.interp.lastSegMs();
        {
            unsigned long avgMs = (unsigned long)d.interp.avgInterval();
            if (avgMs > segMs) segMs = avgMs;
        }
        unsigned long moveHold = NPC_MOVE_HOLD_MS;
        if (segMs * 5 > moveHold) moveHold = segMs * 5; // survive sample droughts
        if (moveHold > 5000) moveHold = 5000;
        float vlen = std::sqrt(vx * vx + vy * vy + vz * vz);
        if (vlen > d.velPeak) d.velPeak = vlen;
        else                  d.velPeak *= 0.99f; // ~1 s half-life at 75 fps
        if (haveNewest && vlen > NPC_MOVE_VEL) d.moveSeenMs = now;
        bool npcMoving = haveNewest && d.moveSeenMs != 0 &&
                         (now - d.moveSeenMs) <= moveHold;
        // Mid-tier body = sparse stream cadence (the round-robin period).
        // Its drive shares the near-tier code below, but its counters/oracles
        // are tracked apart: the near-tier gates guard the validated 20 Hz
        // pipeline, the mid tier is judged by the anti-zombie oracle.
        bool midTier = !isSquad && segMs > 250;
        if (midTier) d.midSeenMs = now;
        if (!isSquad) {
            if (d.wasMoving && !npcMoving) {
                if (midTier) ++restFlipMid_;
                else         ++restFlipNpc_;
            }
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
        // The trailing allowance grows with the stream cadence (Phase 2
        // mid-band tier): against ~500 ms+ samples the body legitimately
        // trails newest by a full segment of source travel (the lead point
        // it walks toward was computed a segment ago), so the near-tier
        // allowance hard-snapped mid walkers chronically (run 103044: 4,259
        // teleports in 30 s, 'Dust Boss' at gap 20-40 u vs gate 18-25 all
        // run).
        float gateSec = snapSeconds_ + (float)segMs / 1000.0f;
        if (gateSec > 2.5f) gateSec = 2.5f;
        if (d.velPeak * gateSec > snapGate) snapGate = d.velPeak * gateSec;

        // Mid-tier bodies are only DRIVEN while the host copy genuinely moves
        // (Phase 2): a stationary far NPC goes back to its own local AI and
        // the census-park divergence fallback - the pre-mid-tier regime.
        // Driving all ~36 census-band bodies at once starved Kenshi's own
        // character-update budget and the whole town rendered stepwise
        // (run 112835: near-tier walkers at zeroFrac 0.63 vs the 0.33
        // baseline; snap storms at every host stop because the drive
        // trailed farther). Movers stay driven - the anti-zombie fix - and
        // the release also skips the AI suspend below, so the local AI can
        // idle the body naturally between host movements.
            if (!isSquad && midTier && !npcMoving) {
                drivenChars_.erase(c);
                drivenSeen_.erase(c); // wide pass may census-park it again
                d.parked = false; d.haveDest = false;
                d.taskApplied = false; d.issuedTask = TASK_NONE;
                if (haveActual) { d.haveActual = true; d.lx = ax; d.ly = ay; d.lz = az; }
                lifeSet(it->first, LIFE_PARKED, "mid-rest");
                debugMark(c, 2, lifeName(LIFE_PARKED));
                continue;
            }

        // Lifecycle: a body that stays driven past the release checks lives
        // in the HI (20 Hz) or MID (round-robin) tier by its stream cadence,
        // with a 10 s hold toward MID so cadence jitter at the tier boundary
        // doesn't flap the record (run 145113 baselined a 3 s hold: boundary
        // bodies flapped HI<->MID ~100x/run - classification noise, not tier
        // changes; and the pre-release placement double-logged every
        // stationary mid body PARKED<->MID each tick). PCs classify HI
        // permanently - their 20 Hz stream never opens a mid-sized segment
        // (Phase 3: PCs and NPCs share this record, the drive below, and
        // the classifier).
        {
            bool midish = midTier ||
                          (d.midSeenMs != 0 && (now - d.midSeenMs) < 10000);
            int st = (!isSquad && midish) ? LIFE_MID : LIFE_HI;
            lifeSet(it->first, st, "drive");
            debugMark(c, st == LIFE_MID ? 3 : 0, lifeName(st));
        }

        // AI-suspend: for any body we DRIVE from the peer's stream, suspend its
        // AI decision layer (faction-safe) so it stops self-tasking but keeps
        // animating. The peer's stream is the sole task authority; the body holds
        // + animates its current/injected action instead of the AI re-deciding
        // every tick. (Releasing node-anchored sitters to local AI was tried -
        // Idea I4 - and regressed: the freed AI wandered them off-host,
        // CROSSCHECK 0.5, and it still did not reliably sit them. So we suspend
        // uniformly.)
        //
        // Phase 1b: this now covers driven SQUAD members too (dropped the old
        // !isSquad gate). A peer-owned squad FOLLOWER (e.g. a recruit, or a unit
        // transferred into the peer's tab) otherwise self-tasks "follow my local
        // leader" at walk speed while the walk-drive simultaneously issues the
        // owner's run-speed move order - the two fight, giving the slow-follow +
        // periodic-snap artifact (manual 2026-07-17: Dust Bandit). Suspending it
        // lets the walk-drive alone own the motion, so it reproduces the owner's
        // run cleanly. A driven squad LEADER has nothing to follow, so the
        // suspend is a no-op for it (leaders already rendered correctly). All
        // bodies here are peer-owned (applyTargets only drives what we do NOT
        // own), so this never quiets a locally-controlled character; the set is
        // rebuilt every tick, so it self-clears the instant ownership flips back.
        if (aiSuspend_) engine::addAiSuspend(c);

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
            d.taskNoneTick = 0;     // movement already released the task; clear the streak
            d.goalsCleared = false; // next rest episode gets one fresh goal-clear
        }

        // ---- Unified drive (Phase 3): one walk/rest/snap path for PCs and
        // NPCs. The kinds differ by POLICY, not code:
        //   * moving CLASSIFIER - a PC body is inert when uncontrolled, so
        //     the host's cMoving flag is trustworthy; an NPC's flag flaps on
        //     fidgets/turns, so NPCs classify by debounced stream VELOCITY
        //     (npcMoving, the walk-hold above). Both feed the same tree.
        //   * SNAP permission - PCs keep the validated instant absolute gate
        //     (the other player's characters are the user-facing rubber
        //     banding; falling behind must reconcile immediately). NPCs are
        //     divergence-gated at 3x the velocity-scaled gate (run 115656:
        //     a max-speed chase holds a STABLE trailing gap catch-up can
        //     never close - re-snapping it every few hundred ms was the
        //     storm; a stable trail is faithful-if-late rendering, and every
        //     genuine warp measured >= 3x anyway), with the mid-tier
        //     cooldown on top (far teleports routinely fail to stick - seat
        //     anchors, unloaded terrain - and must not re-fire per frame).
        //   * at REST - both kinds reproduce the host's pose via applyRest
        //     (per-kind inside: squad members are never town-AI-detached).
        // Walk mechanics are IDENTICAL: lead-point walk along the source
        // velocity (cadence-adaptive - at 20 Hz the fixed 0.6 s lead PCs
        // were validated with; stretched to 1.5 stream segments against
        // sparse mid-tier samples so the body never idles between them),
        // gap-proportional catch-up speed capped at 2.5x, re-issued only
        // when the lead point moves (per-frame re-issue = path-restart
        // stutter). No clearGoals while walking: the HIGH_PRIORITY move
        // order already overrides AI movement, and clearGoals would CANCEL
        // our destination. removeFromUpdateList is never used: it freezes
        // the movement controller (walk + teleport both no-op).
        bool snapOk;
        if (isSquad) {
            snapOk = haveNewest;
        } else {
            // (A grow-vs-own-EMA ratio trigger was tried for the divergence
            // gate first and kept firing on melee-lunge jitter - srcVel 0,
            // gap hopping 30% within a couple frames, 10-14 snaps/min in
            // npc_sync run 123101. Marginal trailing under the 3x bound
            // converges through the catch-up walk; a stopped source heals
            // through the rest-path park.)
            snapOk = gapNewest > snapGate * 3.0f &&
                     (!midTier || (now - d.npcSnapTick) >= NPC_SNAP_COOL_MS);
        }
        if (genuinelyMoving && haveActual && gapNewest > snapGate && snapOk) {
            // Fell behind / source warped: hard-snap to the true position
            // (no-halt teleport keeps the clip phase advancing).
            engine::applyRaw(c, newest);
            if (isSquad) {
                ++hardSnapSquad_;
                logHardSnap(c, out, "squad", gapNewest, vlen, snapGate, d.haveDest);
            } else {
                // Accounting: a snap on a YOUNG ring (< 16 samples, ~0.8 s of
                // 20 Hz coverage) is the one-time divergence reconciliation
                // of a newly / re-acquired body (Phase 2 replaces the census
                // park with it) - classed with the mid counter so the
                // snap-rate gate keeps measuring steady-state tracking only.
                // A recent mid->near handoff (raid entering the 20 Hz
                // bubble) is the same reconciliation debt: divergence
                // accrued under sparse mid coverage, paid with one snap
                // right after the cadence flips near (run 123101: 'Fuu' gap
                // 407 on a 20 Hz-classed ring whose history was mid-band).
                // The clock-slew catch-up window is the same class again:
                // while timeSlew_ != 1 the join sim runs at a different
                // wall-clock rate than the host stream, so every divergent
                // copy legitimately needs reconciliation teleports - the
                // smoothness oracle already excludes those frames for the
                // same reason (run 150302: coop_presence spent its whole 25 s
                // at slew=2.00 and 4 session-start catch-up snaps tripped
                // the steady-state npc gate).
                bool slewing = timeSlew_ < 0.99f || timeSlew_ > 1.01f;
                bool coverage = d.interp.samples() < 16 ||
                                (d.midSeenMs != 0 && (now - d.midSeenMs) < 5000) ||
                                slewing;
                if (midTier || coverage) ++hardSnapMid_;
                else                     ++hardSnapNpc_;
                d.npcSnapTick = now;
                logHardSnap(c, out,
                            midTier ? "mid" : (coverage ? "cover" : "npc"),
                            gapNewest, vlen, snapGate, d.haveDest);
            }
            d.parked = false; d.haveDest = false;
        } else if (genuinelyMoving) {
            float tx = newest.x, ty = newest.y, tz = newest.z;
            // Lead only while the instantaneous velocity is meaningful: the
            // debounced classifier keeps the walk verdict through mid-walk
            // velocity dips (vlen ~ 0), where a lead projection would
            // divide by zero - aim at the newest position instead.
            if (vlen > 0.01f) {
                float leadSec = LEAD_SECONDS;
                float segSec  = (float)segMs / 1000.0f * 1.5f;
                if (segSec > leadSec) leadSec = segSec;
                if (leadSec > 3.0f)   leadSec = 3.0f;
                float lead = vlen * leadSec;
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
                if (isSquad) ++walkReissueSquad_;
                else         ++walkReissueNpc_;
                d.haveDest = true; d.dx = tx; d.dy = ty; d.dz = tz;
            }
            d.parked = false;
            // Locomotion mirror for a DRIVEN SQUAD member (Phase 1b gait fix):
            // player-squad bodies take their gait from the player move-order path
            // (Character::setDestination, shift=false) and effectively IGNORE
            // CharMovement::setDesiredSpeed - so walkTo alone renders a WALK clip
            // no matter how fast the host ran (manual 2026-07-17: Adi walks while
            // the host runs). Mirror the host's exact locomotion (currentSpeed +
            // world-space currentMotion, as streamed) so the anim controller
            // blends to the RUN clip from a run-magnitude state. World NPCs obey
            // setDesiredSpeed on the CharMovement path, so they keep the
            // no-mirror behavior (the engine picks their clip from the locomotion
            // it actually performs); mirroring an NPC here would fight that.
            if (isSquad)
                engine::applyMotion(c, true, out.cSpeed,
                                    out.cMotionX, out.cMotionY, out.cMotionZ);
        } else {
            // At rest, task-authoritative: reproduce the host's sit/idle pose
            // at the same fixture, else quiet + park. Bar patrons sit
            // DYNAMICALLY (walk in and SIT_AROUND a stool), so the seat must
            // be actively INJECTED via applyRest->applyTask; AI-suspend is
            // what stops the local AI from standing an NPC back up. For a
            // squad member this is what makes a join squad-mate sit on the
            // same chair instead of standing on it.
            applyRest(c, d, out, haveActual, ax, ay, az, now, isSquad);
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
        // Mid-tier / far bodies are excluded from the smoothness/anim/march
        // scoring entirely (Phase 2): their sparse cadence renders differently
        // by design (long-lead walks, occasional reconciliation snaps), Kenshi
        // itself throttles far/offscreen character updates into stepwise
        // motion (an engine LOD fact, not an interp fault), and the gates
        // were tuned for - and must keep guarding - the CLOSE 20 Hz pipeline.
        // Scope by DISTANCE to the own squad (the historical judged
        // population), not just cadence: a boundary walker flaps between
        // cadences faster than the estimate settles (run 111445). The
        // anti-zombie oracle owns mid-tier quality.
        bool oracleNear = !midTier;
        if (oracleNear && !isSquad && haveActual && oracleSquadN > 0) {
            float best = -1.0f;
            for (unsigned int oi = 0; oi < oracleSquadN; ++oi) {
                float dd = dist3(ax, ay, az, oracleSquad[oi].x,
                                 oracleSquad[oi].y, oracleSquad[oi].z);
                if (best < 0.0f || dd < best) best = dd;
            }
            if (best > 200.0f) oracleNear = false;
        }
        bool oracleActive = oracleNear &&
                            (isSquad ? hostMoving
                                     : (haveNewest && vlen > NPC_MOVE_VEL));
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
                ++d.activeF;
                if (step < 0.01f) { ++zeroWhileActive_; ++d.zeroF; }
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
        } else if (oracleNear && !oracleActive && haveActual && d.haveActual) {
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
    pruneDriveGrace(now);
    logDriveTelemetry(now);
    ageOutStaleTargets(now);
}

// --- Drive-tick epilogue phases (Phase 7 Workstream C) --------------------
// Split verbatim out of applyTargets' post-loop tail: each reads/writes only
// Replicator members + the tick clock, so behavior is identical.

void Replicator::pruneDriveGrace(unsigned long now) {
    // Prune the recently-driven grace map on a horizon well past the grace
    // window (pointers to despawned bodies must not accumulate; they are
    // never dereferenced, only compared, but the map should stay small).
    for (std::map<Character*, unsigned long>::iterator ds = drivenSeen_.begin();
         ds != drivenSeen_.end(); ) {
        if ((now - ds->second) > 30000) {
            canonicalOf_.erase(ds->first); // same lifetime bound (dangling ptr)
            drivenSeen_.erase(ds++);
        }
        else ++ds;
    }
}

void Replicator::logDriveTelemetry(unsigned long now) {
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
        char b[288];
        _snprintf(b, sizeof(b) - 1,
            "[interp] lerp=%lu extrap=%lu clamp=%lu seg=%lu single=%lu "
            "snapSq=%lu snapNpc=%lu reissueSq=%lu reissueNpc=%lu restFlip=%lu "
            "delay=%lu jit=%.1f starve=%u snapMid=%lu restFlipMid=%lu",
            interpLerp_, interpExtrap_, interpClampOld_, interpSegSnap_,
            interpSingle_, hardSnapSquad_, hardSnapNpc_,
            walkReissueSquad_, walkReissueNpc_, restFlipNpc_, maxDelay, maxJit,
            starveHeldNow_, hardSnapMid_, restFlipMid_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        // Worst zero-step contributor (Phase 2 smoothness diagnosis): name the
        // hand charging the most frozen-while-active frames to the oracle.
        {
            const Driven* worst = 0; const Key* wk = 0;
            for (std::map<Key, Driven>::iterator it = targets_.begin();
                 it != targets_.end(); ++it) {
                if (!worst || it->second.zeroF > worst->zeroF) {
                    worst = &it->second; wk = &it->first;
                }
            }
            if (worst && worst->zeroF > 0) {
                char z[144]; _snprintf(z, sizeof(z) - 1,
                    "[interp] worstZero hand=%u,%u zero=%lu active=%lu seg=%lu",
                    wk->i, wk->s, worst->zeroF, worst->activeF,
                    worst->interp.lastSegMs());
                z[sizeof(z) - 1] = '\0'; coop::logLine(z);
            }
        }
    }
    // Combat warp-diagnosis rollup (~5 s, Phase 1). Quiet until combat has
    // happened this session (combatOrder_ > 0), so a peaceful run stays clean.
    // Cumulative counters diff into rates (the combat_snap_rate oracle); armed
    // counts + maxPersist are LIVE (a body persistently snapping is diverging in
    // a directed way - wrong target / wrong place - not occasionally churning).
    if (combatOrder_ > 0 && (now - combatLogTick_) > 5000) {
        combatLogTick_ = now;
        unsigned int armed = 0; unsigned long maxPersist = 0;
        for (std::map<Key, Driven>::iterator it = targets_.begin();
             it != targets_.end(); ++it) {
            if (it->second.combatArmed) ++armed;
            if (it->second.combatSnapCount > maxPersist)
                maxPersist = it->second.combatSnapCount;
        }
        char b[208];
        _snprintf(b, sizeof(b) - 1,
            "[combat] stats snap=%lu slide=%lu softWalk=%lu order=%lu wrongTgt=%lu "
            "armed=%u maxPersist=%lu",
            combatSnapTotal_, combatSlide_, combatSoftWalk_, combatOrder_,
            combatWrongTgt_, armed, maxPersist);
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
}

void Replicator::ageOutStaleTargets(unsigned long now) {
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
    // Debounced task-clear (job removal). The re-arm above deliberately IGNORES
    // host->NONE frames so a transient capture blip can't tear down a committed sit/
    // operate pose. But a genuine un-assign where the body stays STATIONARY (the
    // movement re-arm in applyTargets never fires) streams NONE continuously - hold
    // through blips, but after TASK_CLEAR_MS of sustained NONE release the held pose
    // so the peer stops reproducing the order (falls through to clearGoals+endAction+
    // park below). Carry is synthetic (owned by the carry self-heal) - never clear on
    // it. Mirrors carryNoSeeTick/furnNoSeeTick.
    if (!syntheticCarry && out.task == TASK_NONE &&
        (d.taskApplied || d.issuedTask != TASK_NONE)) {
        if (d.taskNoneTick == 0) {
            d.taskNoneTick = now; // streak starts; hold this frame
        } else if (coop::poseClearElapsed(d.taskNoneTick, now, TASK_CLEAR_MS)) {
            { char b[112]; _snprintf(b, sizeof(b) - 1,
                "[pose] task-clear hand=%u,%u was=%u", out.hIndex, out.hSerial,
                (unsigned)d.issuedTask); b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            d.taskApplied = false; d.taskBad = false; d.taskRetries = 0;
            d.issuedTask = TASK_NONE; d.taskNoneTick = 0;
        }
    } else {
        d.taskNoneTick = 0; // any real task (incl. a genuine re-arm) cancels the streak
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
        // NEVER detach a CHAINED body either (world_parity 2026-07-17): the
        // chained fall-through routes working slaves here, and the detach
        // re-containered each one (Pao 1,42 -> 1,53 etc.) - the old hand
        // stopped resolving (drive dropped it, census couldn't match) and the
        // new-hand body was census-absent, so suppression HID it: the "many
        // units on the host missing on the join" manual finding. AI-suspend
        // (default-on) already quiets the town-AI re-tasker for them.
        bool chained = (out.bodyState & BODY_CHAINED) != 0;
        if (!d.detached && !noDetach_ && !isSquad && !chained) {
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
        // NOTE: do NOT AI-suspend a held bed pose. The engine's decision layer
        // (Character::periodicUpdate) is what plays/maintains the lie-down sleep
        // clip; suspending it leaves the body placed in the bed but STANDING on
        // it (manual test 2026-07-17). The wake-and-move desync is handled by the
        // bed fast-exit in applyTargets (Fix A), so no re-sleep guard is needed
        // here - the driven copy follows the host the instant it moves.
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


} // namespace coop
