// ReplicatorAuthority.cpp - host-authority enforcement on the join (monolith
// split from Replicator.cpp, 2026-07-12): applyNpcCensus (protocol 36 census
// intake), enforceHostAuthority (suppress/park/cull of local copies the host
// does not corroborate, far-mint requests), parkDivergedCopy, and the debug
// marker HUD plumbing (debugMark/pruneDebugMarkers).
//
// Shared hubs: owns censusHands_ + suppressed_; reads proxyByKey_ (minted by
// the spawn TU); writes life_ via lifeSet.
// Must NOT: change any log string - log phrasing is the API consumed by the
// PowerShell oracles (see resources/CODE_MAP.md, log-tag index).

#include "ReplicatorUtil.h"

namespace coop {

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
                lifeSet(k, LIFE_RESOLVED, "restore");
                { char nm[48]; engine::charName(chars[i], nm, sizeof(nm));
                  char b[160]; _snprintf(b, sizeof(b) - 1,
                    "[authority] restore NPC hand=%u,%u name='%s' (supp=%u churn=%lu/%lu)",
                    states[i].hIndex, states[i].hSerial, nm,
                    (unsigned)suppressed_.size(), authSuppresses_, authRestores_);
                  b[sizeof(b) - 1] = '\0'; coop::logLine(b); }
            }
            if (s == suppressed_.end() && !streamed) {
                // Driven bodies report their tier (and own their marker) from
                // applyTargets; a census-present LOCAL copy is the park-
                // fallback regime.
                lifeSet(k, LIFE_PARKED, "census-local");
                debugMark(chars[i], 2, lifeName(LIFE_PARKED));
            }
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
                    lifeSet(k, LIFE_CULLED, "suppress");
                    debugMark(chars[i], 1, lifeName(LIFE_CULLED));
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
        unsigned long nowR = nowMs(); // recently-driven grace reference
        std::set<Character*> nearSet;
        for (unsigned int i = 0; i < n; ++i) nearSet.insert(chars[i]);
        for (unsigned int i = 0; i < wn; ++i) {
            if (nearSet.find(wChars[i]) != nearSet.end()) continue;
            if (proxyChars.find(wChars[i]) != proxyChars.end()) continue;
            // Phase 2 mid-band tier: a DRIVEN body used to skip this pass
            // entirely - correct for parking/suppression (the stream owns its
            // position, and hiding a driven body is self-defeating), but it
            // also skipped RESTORE: a suppressed NPC whose hand starts
            // arriving on the mid tier is driven every tick (drivenChars_)
            // yet stays hidden+frozen forever - walk orders no-op on a body
            // removed from the update list, the permanent-zombie shape of
            // the old boundary flicker. Let a driven body reach the restore
            // branch (same dwell), and skip only park/suppress for it.
            // "Driven" includes a grace window (drivenSeen_): a mid-tier
            // body's samples ride the round-robin, and a rotation hiccup
            // must not let the cull streak run while the body is between
            // samples (run 103044: 6 cull/restore cycles per hand).
            bool driven = drivenChars_.find(wChars[i]) != drivenChars_.end();
            if (!driven) {
                std::map<Character*, unsigned long>::iterator ds =
                    drivenSeen_.find(wChars[i]);
                driven = ds != drivenSeen_.end() && (nowR - ds->second) < 8000;
            }
            Key k = keyOf(wStates[i]);
            if (driven && suppressed_.find(k) == suppressed_.end()) continue;
            bool exists = censusHands_.find(k) != censusHands_.end() ||
                          keep.find(k) != keep.end() || driven;
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
                    lifeSet(k, LIFE_RESOLVED, "restore-wide");
                    debugMark(wChars[i], 2, lifeName(LIFE_PARKED));
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
                // Driven bodies excluded: the mid/near stream owns them.
                if (s == suppressed_.end() && !driven) {
                    lifeSet(k, LIFE_PARKED, "census-wide");
                    parkDivergedCopy(wChars[i], wStates[i], k);
                }
            } else if (s == suppressed_.end() && ac.unstreamed >= SUPPRESS_AFTER_FRAMES) {
                if (engine::suppressNpc(gw, wChars[i])) {
                    suppressed_[k] = wChars[i];
                    ++authSuppresses_;
                    ++censusCulls_;
                    lifeSet(k, LIFE_CULLED, "cull-wide");
                    debugMark(wChars[i], 1, lifeName(LIFE_CULLED));
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
                Key k = keyOf(sts[i]);
                const char* cls;
                if (proxyChars.find(cs[i]) != proxyChars.end() ||
                    keep.find(k) != keep.end() ||
                    drivenChars_.find(cs[i]) != drivenChars_.end()) {
                    cls = "drv"; ++cDrv;
                } else if (suppressed_.find(k) != suppressed_.end()) {
                    cls = "hid"; ++cHid;
                } else if (censusFresh &&
                           censusHands_.find(k) != censusHands_.end()) {
                    cls = "cen"; ++cCen;
                } else {
                    cls = "ghost"; ++cGhost;
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
                // travel_parity worldstate rows (join side): one row per
                // enumerated NPC with its authority class, same schema as the
                // host's census dump so the oracle can cross-match by hand.
                if (auditRows_) {
                    char nm[40]; engine::charName(cs[i], nm, sizeof(nm));
                    char r[224]; _snprintf(r, sizeof(r) - 1,
                        "SCENARIO WNPC hand=%u,%u,%u,%u,%u pos=%.1f,%.1f,%.1f "
                        "cls=%s name='%s'",
                        sts[i].hIndex, sts[i].hSerial, sts[i].hType,
                        sts[i].hContainer, sts[i].hContainerSerial,
                        sts[i].x, sts[i].y, sts[i].z, cls, nm);
                    r[sizeof(r) - 1] = '\0'; coop::logLine(r);
                }
            }
        }
        if (auditRows_) {
            char w[112]; _snprintf(w, sizeof(w) - 1,
                "SCENARIO WORLD n=%u cls=join drv=%u cen=%u hid=%u ghost=%u fresh=%d",
                (unsigned)counted.size(), cDrv, cCen, cHid, cGhost,
                censusFresh ? 1 : 0);
            w[sizeof(w) - 1] = '\0'; coop::logLine(w);
        }
        char b[192]; _snprintf(b, sizeof(b) - 1,
            "[audit] exist near=%u wide=%u drv=%u cen=%u hid=%u ghost=%u "
            "supp=%u census=%u fresh=%d parks=%lu",
            n, wn, cDrv, cCen, cHid, cGhost,
            (unsigned)suppressed_.size(), (unsigned)censusHands_.size(),
            censusFresh ? 1 : 0, censusParks_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // Phase 3 lifecycle upkeep: prune left-interest records + self-audit
    // mintable hands stuck in DISCOVERED (the invisible-raid failure).
    lifeSweep(gw, now);
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


} // namespace coop
