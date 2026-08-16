// ReplicatorChannels.cpp - the periodic state channels (monolith split from
// Replicator.cpp, 2026-07-12): medical (+treatments), stats, the money pool,
// faction relations, doors, production machines, research, placed builds
// (+build doors), recruits, squad moves, stealth, consensus game speed,
// game-clock sync, and onPeerConnected (the join-time snapshot burst).
//
// Shared hubs: each channel owns its own seq/sample-throttle members
// (fooSeqOut_/fooSampleMs_); reads ownHands_/tabRank_ (stamped by the
// publish TU) for tab-scoped channels.
// Must NOT: change any log string - log phrasing is the API consumed by the
// PowerShell oracles (see resources/CODE_MAP.md, log-tag index).

#include "ReplicatorUtil.h"
#include "../core/BuildOwnership.h"

namespace coop {

// Quantized fingerprint of a medical snapshot (FNV-1a over rounded fields):
// sub-noise wobble (blood regen ticks at ~0.01) must not chatter the reliable
// channel, so blood/flesh/bandaging quantize to 0.5 units and bleed to 0.05.
// Protocol 16: covers the FULL anatomy (flesh+stun+bandage+juryRig per part)
// plus the 4 LimbStates - a stun hit or a limb loss re-fingerprints too.
static coop::u32 medicalHash(const engine::MedicalRead& m) {
    long q[4 + 12 * 4 + 4 + 3];
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
    // Protocol 53: crippled must be IN the fingerprint or the cripple/heal
    // transition is a change no send is triggered by - limb flesh moves with it
    // in the usual case, but a jury-rig or a heal can flip the flag while the
    // quantized part values stay in their buckets.
    q[4 + 12 * 4 + 4 + 2] = m.crippled ? 1 : 0;
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
    pkt.flags = (mr.unconscious ? MED_UNCONSCIOUS : 0) | (mr.dead ? MED_DEAD : 0) |
                (mr.crippled ? MED_CRIPPLED : 0);
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
        // Protocol 53: unlike its two neighbours in this byte, crippled is
        // AUTHORITATIVE rather than advisory - no event channel owns the
        // transition, and writeMedical applies it so the copy's crawl gait can
        // engage (the posture apply in applyTargets is the other half).
        w.crippled    = (p.flags & MED_CRIPPLED) != 0;
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

// JOIN only (protocol 45): forward the join-dealt damage that applyTargets drained
// into pendingHits_ (per driven world-NPC copy, keyed by canonical hand). The
// join's own melee is guarded (cosmetic), so this reliable report is the ONLY path
// by which the join PC actually wounds the host's authoritative NPC. Sent as it
// accumulates; the map is cleared each publish (unsent-on-drop is acceptable - the
// next swing re-accumulates, and RELIABLE delivery covers a queued send).
void Replicator::publishCombatHits(GameWorld* gw, NetLink& net, u32 ownerId) {
    (void)gw;
    if (pendingHits_.empty()) return;
    for (std::map<Key, PendingHit>::iterator it = pendingHits_.begin();
         it != pendingHits_.end(); ++it) {
        const Key&       k  = it->first;
        const PendingHit& ph = it->second;
        if (ph.flesh <= 0.0f && ph.blood <= 0.0f) continue;
        CombatHitPacket chp;
        memset(&chp, 0, sizeof(chp));
        chp.type    = (u8)PKT_COMBAT_HIT;
        chp.ownerId = ownerId;
        chp.hitId   = nextHitId_++;
        chp.sType = k.t; chp.sContainer = k.c; chp.sContainerSerial = k.cs;
        chp.sIndex = k.i; chp.sSerial = k.s;
        chp.flesh = ph.flesh; chp.blood = ph.blood;
        net.queueCombatHit(chp);
        char b[160]; _snprintf(b, sizeof(b) - 1,
            "[combat] HIT SEND id=%u hand=%u,%u flesh=%.1f blood=%.1f",
            chp.hitId, k.i, k.s, ph.flesh, ph.blood);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    pendingHits_.clear();
}

// HOST only (protocol 45): drain received join-dealt damage reports and wound the
// authoritative world NPC (blood loss + a frontal flesh wound). The host owns +
// simulates world NPCs, so this is the authoritative application; the vitals
// stream then mirrors the new state back to the join's cosmetic copy. A report for
// a body the host OWNS as a player-squad member is skipped (PvP is out of scope
// and would be a partition error, mirroring applyTreatments' authority guard).
void Replicator::applyCombatHits(GameWorld* gw, Inbound& in) {
    std::deque<InboundCombatHit> got;
    in.drainCombatHits(got);
    for (std::deque<InboundCombatHit>::iterator it = got.begin(); it != got.end(); ++it) {
        const CombatHitPacket& p = it->pkt;
        Key k; k.t = p.sType; k.c = p.sContainer; k.cs = p.sContainerSerial;
        k.i = p.sIndex; k.s = p.sSerial;
        if (ownHands_.find(k) != ownHands_.end()) {
            char b[160]; _snprintf(b, sizeof(b) - 1,
                "[combat] HIT RECV id=%u hand=%u,%u SKIP (own body)",
                p.hitId, k.i, k.s);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            continue;
        }
        unsigned int hand[5] = { k.t, k.c, k.cs, k.i, k.s };
        bool applied = engine::applyReportedDamage(gw, hand, p.flesh, p.blood);
        // A wounded world NPC is definitionally combat-scoped: mark it so the NPC
        // vitals stream (publishMedical, Phase B) mirrors the authoritative drop
        // back to the join's cosmetic copy (link 6 - "damage reached the join").
        // The join's own copy is damage-guarded, so without this stream it would
        // never reflect the host-applied wound.
        if (applied && streamNpcs_) medNpc_[k] = nowMs();
        char b[160]; _snprintf(b, sizeof(b) - 1,
            "[combat] HIT RECV id=%u hand=%u,%u flesh=%.1f blood=%.1f applied=%d",
            p.hitId, k.i, k.s, p.flesh, p.blood, applied ? 1 : 0);
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

// ---- Protocol 52: shared money pool -----------------------------------------
//
// Kenshi keeps ONE player wallet per save, so both players spend from one pool
// and the HOST's engine wallet is the authority. Two writers on one number rule
// out absolute snapshots (they lose concurrent spends: from 1000, a 200 and a
// 300 purchase exchanged as totals leave 800 or 700, so one purchase is free),
// so the join ships the CHANGE and the host ships the authoritative TOTAL.
//
// Detection is a single wallet read per tick against poolSeen_: whatever moved
// the wallet - trade UI, sale, loot, bounty, hire, bar tab - shows up as a
// delta, so no per-path hooking is needed. poolSeen_ is updated on every write
// we make ourselves, which is what keeps our own apply from being re-detected
// as a fresh local change (the echo guard the sampled channels all need).

void Replicator::publishMoneyPool(const SyncContext& ctx) {
    GameWorld* gw = ctx.gw; NetLink& net = *ctx.net; u32 ownerId = ctx.localId;
    if (!moneySync_) return;
    int cur = -1;
    if (!engine::readPlayerWallet(gw, &cur) || cur < 0) return;
    const bool hostRole = isHostRole();
    unsigned long now = nowMs();

    // First sample of a session (or the first after a reload): adopt the wallet
    // as the baseline instead of reporting it as a change. Without this the
    // save's own starting cats would look like a purchase the size of the whole
    // wallet on whichever side sampled first.
    if (poolSeen_ < 0) {
        poolSeen_ = cur;
        if (hostRole) poolTotal_ = cur;
        char b[112];
        _snprintf(b, sizeof(b) - 1, "[wallet] POOL SEED t=%d role=%s",
                  cur, hostRole ? "host" : "join");
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return;
    }

    if (hostRole) {
        // Our engine wallet IS the pool. A local move is already authoritative;
        // just keep the mirror and let the change gate publish it.
        if (cur != poolSeen_) {
            char b[128];
            _snprintf(b, sizeof(b) - 1, "[wallet] POOL LOCAL t=%d was=%d delta=%d",
                      cur, poolSeen_, cur - poolSeen_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            poolSeen_ = cur;
        }
        poolTotal_ = cur;
        bool changed = (poolTotal_ != poolSent_);
        if (!sync::gateShouldSend(changed, now, poolSentMs_, tuning_.moneyMinSendMs,
                                  tuning_.moneyResendMs, /*resendUnsent*/ true))
            return;
        poolSent_ = poolTotal_; poolSentMs_ = now;
        MoneyPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type    = (u8)PKT_MONEY;
        pkt.ownerId = ownerId;
        pkt.ackSeq  = poolAcked_;
        pkt.money   = poolTotal_;
        net.queueMoney(pkt);
        if (changed) { // resends stay silent; the change is the signal
            char b[112];
            _snprintf(b, sizeof(b) - 1, "[wallet] POOL SEND t=%d ack=%u",
                      poolTotal_, poolAcked_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        return;
    }

    // Join: report the movement, never the total. The delta stays pending until
    // the host acks it so applyMoneyPool can re-apply it on top of a total that
    // does not include it yet.
    if (cur == poolSeen_) return;
    int delta = cur - poolSeen_;
    poolSeen_ = cur;
    ++poolSeq_;
    poolPending_.push_back(PoolDelta(poolSeq_, delta));
    MoneyDeltaPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type    = (u8)PKT_MONEY_DELTA;
    pkt.ownerId = ownerId;
    pkt.seq     = poolSeq_;
    pkt.delta   = delta;
    net.queueMoneyDelta(pkt);
    char b[128];
    _snprintf(b, sizeof(b) - 1, "[wallet] POOL DELTA seq=%u delta=%d local=%d",
              poolSeq_, delta, cur);
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
}

void Replicator::applyMoneyPool(const SyncContext& ctx) {
    GameWorld* gw = ctx.gw; Inbound& in = *ctx.in; NetLink& net = *ctx.net;
    std::deque<InboundMoney> totals;
    std::deque<InboundMoneyDelta> deltas;
    in.drainMoney(totals);
    in.drainMoneyDeltas(deltas);
    if (totals.empty() && deltas.empty()) return;
    if (!moneySync_) return;
    const bool hostRole = isHostRole();

    if (hostRole) {
        // Fold the join's deltas into the pool. Clamped at 0: both players can
        // commit purchases against the same cats before either delta lands, and
        // the goods are already handed over locally, so the pool absorbs the
        // shortfall rather than going negative.
        bool moved = false;
        for (std::deque<InboundMoneyDelta>::iterator it = deltas.begin();
             it != deltas.end(); ++it) {
            const MoneyDeltaPacket& p = it->pkt;
            if (p.seq <= poolAcked_) continue; // already folded (defensive)
            if (poolTotal_ < 0) {              // no seed yet - adopt the wallet first
                int cur = -1;
                if (!engine::readPlayerWallet(gw, &cur) || cur < 0) return;
                poolTotal_ = cur; poolSeen_ = cur;
            }
            int want = poolTotal_ + p.delta;
            if (want < 0) {
                char b[128];
                _snprintf(b, sizeof(b) - 1,
                          "[wallet] OVERDRAFT want=%d clamped=0 seq=%u delta=%d",
                          want, p.seq, p.delta);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                want = 0;
            }
            poolTotal_ = want;
            poolAcked_ = p.seq;
            moved = true;
            char b[144];
            _snprintf(b, sizeof(b) - 1, "[wallet] POOL FOLD seq=%u delta=%d t=%d",
                      p.seq, p.delta, poolTotal_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (!moved) return;
        if (engine::writePlayerWallet(gw, poolTotal_)) {
            poolSeen_ = poolTotal_;
        } else {
            // The fold is acked but the engine refused the write, so the join's
            // cats are about to be dropped when the next sample re-reads the
            // unchanged wallet. Never silent: this is money going missing.
            char b[112];
            _snprintf(b, sizeof(b) - 1, "[wallet] WARN pool write FAILED t=%d ack=%u",
                      poolTotal_, poolAcked_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // Publish immediately rather than waiting on the send floor: the join is
        // holding a pending delta and we now know the answer.
        poolSent_ = poolTotal_; poolSentMs_ = nowMs();
        MoneyPacket out;
        memset(&out, 0, sizeof(out));
        out.type    = (u8)PKT_MONEY;
        out.ownerId = ctx.localId;
        out.ackSeq  = poolAcked_;
        out.money   = poolTotal_;
        net.queueMoney(out);
        return;
    }

    // Join: adopt the newest authoritative total, then re-apply whatever the
    // host has not acked yet so a purchase made moments ago is not reverted for
    // a round trip and then re-applied.
    if (totals.empty()) return;
    const MoneyPacket& p = totals.back().pkt;
    if (p.money < 0) return;
    while (!poolPending_.empty() && poolPending_.front().seq <= p.ackSeq)
        poolPending_.pop_front();
    poolAcked_ = p.ackSeq;
    int want = p.money;
    for (std::deque<PoolDelta>::iterator it = poolPending_.begin();
         it != poolPending_.end(); ++it)
        want += it->delta;
    if (want < 0) want = 0;
    int cur = -1;
    if (!engine::readPlayerWallet(gw, &cur)) return;
    if (cur == want) { poolSeen_ = cur; return; } // already converged
    bool ok = engine::writePlayerWallet(gw, want);
    if (ok) poolSeen_ = want;
    char b[176];
    _snprintf(b, sizeof(b) - 1,
              "[wallet] POOL RECV t=%d ack=%u pending=%u want=%d was=%d ok=%d",
              p.money, p.ackSeq, (unsigned)poolPending_.size(), want, cur, ok ? 1 : 0);
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
}

void Replicator::publishFactions(const SyncContext& ctx) {
    GameWorld* gw = ctx.gw; NetLink& net = *ctx.net; u32 ownerId = ctx.localId;
    if (!factionSync_) return;
    const unsigned long SAMPLE_MS = tuning_.factionSampleMs; // relations move in bursts; 1 Hz is plenty
    const unsigned long RESEND_MS = tuning_.factionResendMs; // safety resend for rows we ever sent
    const float         EPS       = 0.5f;  // engine values are whole-ish numbers
    unsigned long now = nowMs();

    // The affectRelations detour saw a REAL mutation this tick: sample NOW so
    // the row crosses within a tick instead of up to a full sample period
    // later. The deltas themselves are evidence (already logged); the value
    // diff below is what actually replicates.
    engine::FactionDelta deltas[16];
    unsigned int nDeltas = engine::drainFactionDeltas(deltas, 16);
    // A real mutation this tick (nDeltas > 0) forces a sample regardless of the
    // 1 Hz throttle so the row crosses within a tick.
    if (nDeltas == 0 && !sync::gateSampleDue(now, facSampleMs_, SAMPLE_MS)) return;
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
        // Seeded silently above, so a never-sent row holds (resendUnsent=false);
        // only a real move or a post-send safety resend crosses.
        if (!sync::gateShouldSend(changed, now, fr.lastSendMs, /*minSendMs*/ 0,
                                  RESEND_MS, /*resendUnsent*/ false))
            continue;
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

void Replicator::applyFactions(const SyncContext& ctx) {
    GameWorld* gw = ctx.gw; Inbound& in = *ctx.in;
    std::deque<InboundFaction> got;
    in.drainFaction(got);
    if (got.empty()) return;
    if (!factionSync_) return;
    const float EPS = 0.5f;
    for (std::deque<InboundFaction>::iterator it = got.begin(); it != got.end(); ++it) {
        const FactionPacket& p = it->pkt;
        if (p.sid[0] == '\0') continue;
        FacRow& fr = facRows_[std::string(p.sid)];
        if (!sync::gateSeqAccept(fr.seqSeen, p.seq)) continue; // stale/dup row
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

void Replicator::publishDoors(const SyncContext& ctx) {
    GameWorld* gw = ctx.gw; NetLink& net = *ctx.net; u32 ownerId = ctx.localId;
    if (!doorSync_) return;
    const unsigned long SAMPLE_MS = tuning_.doorSampleMs;  // doors move in clicks; 1 Hz is plenty
    const unsigned long RESEND_MS = tuning_.doorResendMs;  // safety resend for rows we ever sent
    unsigned long now = nowMs();
    if (!sync::gateSampleDue(now, doorSampleMs_, SAMPLE_MS)) return;
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
        // Doors seed their baseline silently above, so a never-sent row holds
        // (resendUnsent = false); only a real change or a post-send safety
        // resend crosses. No burst throttle - the 1 Hz sample gate paces it.
        if (!sync::gateShouldSend(changed, now, dr.lastSendMs, /*minSendMs*/ 0,
                                  RESEND_MS, /*resendUnsent*/ false))
            continue;
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

void Replicator::applyDoors(const SyncContext& ctx) {
    Inbound& in = *ctx.in;
    std::deque<InboundDoor> got;
    in.drainDoor(got);
    if (got.empty()) return;
    if (!doorSync_) return;
    for (std::deque<InboundDoor>::iterator it = got.begin(); it != got.end(); ++it) {
        const DoorPacket& p = it->pkt;
        Key k; k.t = p.hand[0]; k.c = p.hand[1]; k.cs = p.hand[2];
        k.i = p.hand[3]; k.s = p.hand[4];
        DoorRow& dr = doorRows_[k];
        if (!sync::gateSeqAccept(dr.seqSeen, p.seq)) continue; // stale/dup row
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

void Replicator::publishProd(const SyncContext& ctx) {
    GameWorld* gw = ctx.gw; NetLink& net = *ctx.net; u32 ownerId = ctx.localId;
    if (!prodSync_) return;
    const unsigned long SAMPLE_MS = tuning_.prodSampleMs;  // machines tick slowly; 1 Hz is plenty
    const unsigned long RESEND_MS = tuning_.prodResendMs;  // safety resend = the join drift corrector
    unsigned long now = nowMs();
    if (!sync::gateSampleDue(now, prodSampleMs_, SAMPLE_MS)) return;
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
        // `changed` already folds in first-sight (!pr.sent), so a never-sent row
        // always crosses; resendUnsent is moot here. Safety resend after RESEND_MS.
        if (!sync::gateShouldSend(changed, now, pr.lastSendMs, /*minSendMs*/ 0,
                                  RESEND_MS, /*resendUnsent*/ false))
            continue;
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

void Replicator::applyProd(const SyncContext& ctx) {
    Inbound& in = *ctx.in;
    std::deque<InboundProd> got;
    in.drainProd(got);
    if (got.empty()) return;
    if (!prodSync_) return;
    for (std::deque<InboundProd>::iterator it = got.begin(); it != got.end(); ++it) {
        const ProdPacket& p = it->pkt;
        Key wk; wk.t = p.key[0]; wk.c = p.key[1]; wk.cs = p.key[2];
        wk.i = p.key[3]; wk.s = p.key[4];
        ProdRow& pr = prodRows_[std::make_pair((int)p.keyKind, wk)];
        if (!sync::gateSeqAccept(pr.seqSeen, p.seq)) continue; // stale/dup row
        pr.seqSeen = p.seq;
        // Resolve the wire key to OUR machine's hand: baked hands resolve
        // directly; a placer key is either a building WE placed (our own
        // hand) or one we MINTED for the host's placement (translation map).
        unsigned int hand[5];
        if (p.keyKind == 0) {
            // A baked hand resolves directly, but a MINE's does not: it is a
            // per-client instance for a terrain node, so the host's key names
            // nothing here and every buffer row for it used to be dropped -
            // the "mine inventory does not sync" half of the report. Prefer the
            // protocol-55 pairing, fall back to the raw hand.
            if (!localHandForFixtureKey(wk, hand))
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

void Replicator::publishResearch(const SyncContext& ctx) {
    GameWorld* gw = ctx.gw; NetLink& net = *ctx.net; u32 ownerId = ctx.localId;
    if (!researchSync_) return;
    const unsigned long SAMPLE_MS = tuning_.researchSampleMs; // unlocks are rare; 1 Hz is plenty
    const unsigned long RESEND_MS = tuning_.researchResendMs; // lost-row / late-prereq corrector
    unsigned long now = nowMs();
    if (!sync::gateSampleDue(now, researchSampleMs_, SAMPLE_MS)) return;
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
        // Known-set membership is monotonic (no value to diff): send on first
        // sight then resend periodically. No silent seed -> resendUnsent=true.
        if (!sync::gateShouldSend(/*changed*/ false, now, rr.lastSendMs,
                                  /*minSendMs*/ 0, RESEND_MS, /*resendUnsent*/ true))
            continue;
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

void Replicator::applyResearch(const SyncContext& ctx) {
    GameWorld* gw = ctx.gw; Inbound& in = *ctx.in;
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
        if (!sync::gateSeqAccept(rr.seqSeen, p.seq)) continue; // stale/dup row
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

void Replicator::publishDeeds(const SyncContext& ctx) {
    GameWorld* gw = ctx.gw; NetLink& net = *ctx.net; u32 ownerId = ctx.localId;
    if (!deedSync_) return;
    const unsigned long SAMPLE_MS = tuning_.deedSampleMs; // purchases are rare
    const unsigned long RESEND_MS = tuning_.deedResendMs; // lost-row AND not-yet-resolvable corrector
    unsigned long now = nowMs();
    if (!sync::gateSampleDue(now, deedSampleMs_, SAMPLE_MS)) return;
    deedSampleMs_ = now;

    // A party owning more than this many buildings is far past the two-player
    // design target; the cap only bounds the scratch, and the set is read off
    // Ownerships rather than a spatial query so an unloaded deed still ships.
    const unsigned int MAX_DEEDS = 128;
    static engine::DeedRead rows[MAX_DEEDS]; // main-thread only
    unsigned int n = engine::enumOwnedBuildings(gw, rows, MAX_DEEDS);

    // Pass 1 - OWN. Set membership is the authority, so a hand in the set
    // publishes owned=1 on first sight and on the safety resend (no silent
    // seed, hence resendUnsent=true - the research shape).
    std::set<Key> live;
    for (unsigned int i = 0; i < n; ++i) {
        const engine::DeedRead& r = rows[i];
        Key k; k.t = r.hand[0]; k.c = r.hand[1]; k.cs = r.hand[2];
        k.i = r.hand[3]; k.s = r.hand[4];
        // Raw deed hands are cross-client identities only for save-resident
        // buildings.  A protocol-27 placement has a different runtime hand on
        // each client and its ownership is established when the peer copy is
        // minted below.  Publishing that local hand as a deed can resolve to
        // nothing (or, worse, an unrelated runtime object) on the peer.
        Key buildWire;
        const bool sessionPlaced = buildKeyForLocalHand(k, buildWire);
        if (!deedMayPublishRawHand(sessionPlaced)) {
            deedRows_.erase(k); // also remove a row seeded by older code
            continue;
        }
        live.insert(k);
        DeedRow& dr = deedRows_[k];
        bool changed = (dr.knownOwned != 1);
        if (!sync::gateShouldSend(changed, now, dr.lastSendMs, /*minSendMs*/ 0,
                                  RESEND_MS, /*resendUnsent*/ true))
            continue;
        dr.sent = true; dr.knownOwned = 1; dr.lastSendMs = now;
        DeedPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type    = (u8)PKT_DEED;
        pkt.ownerId = ownerId;
        pkt.seq     = deedSeqOut_++;
        for (unsigned int h = 0; h < 5; ++h) pkt.hand[h] = r.hand[h];
        pkt.owned   = 1;
        strncpy(pkt.ownerSid, r.ownerSid, sizeof(pkt.ownerSid) - 1);
        net.queueDeed(pkt);
        if (changed) { // resends stay silent; the new deed is the signal
            char b[208];
            _snprintf(b, sizeof(b) - 1,
                      "[deed] SEND hand=%u.%u.%u.%u.%u owned=1 owner='%s' "
                      "name='%s' seq=%u",
                      pkt.hand[0], pkt.hand[1], pkt.hand[2], pkt.hand[3],
                      pkt.hand[4], pkt.ownerSid, r.name, pkt.seq);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    // Pass 2 - UN-OWN, additive-only. A row we have latched at owned=1 that is
    // no longer in the set is only a SALE if the building still resolves here
    // and the engine agrees it is not ours. A hand that merely stopped
    // resolving means "that zone is unloaded", never "sold", and publishing
    // owned=0 for it would DELETE the partner's deed.
    for (std::map<Key, DeedRow>::iterator it = deedRows_.begin();
         it != deedRows_.end(); ++it) {
        DeedRow& dr = it->second;
        if (dr.knownOwned != 1) continue;
        if (live.find(it->first) != live.end()) continue;
        unsigned int hand[5];
        hand[0] = it->first.t; hand[1] = it->first.c; hand[2] = it->first.cs;
        hand[3] = it->first.i; hand[4] = it->first.s;
        engine::DeedRead cur;
        if (!engine::readDeedByHand(gw, hand, &cur)) continue; // unloaded: stay latched
        if (cur.owned != 0) continue;                          // engine disagrees
        dr.sent = true; dr.knownOwned = 0; dr.lastSendMs = now;
        DeedPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type    = (u8)PKT_DEED;
        pkt.ownerId = ownerId;
        pkt.seq     = deedSeqOut_++;
        for (unsigned int h = 0; h < 5; ++h) pkt.hand[h] = hand[h];
        pkt.owned   = 0;
        // The seller, carried explicitly: the receiver has no baseline town
        // owner to restore for a building that was already ours in the save.
        strncpy(pkt.ownerSid, cur.ownerSid, sizeof(pkt.ownerSid) - 1);
        net.queueDeed(pkt);
        char b[208];
        _snprintf(b, sizeof(b) - 1,
                  "[deed] SEND hand=%u.%u.%u.%u.%u owned=0 owner='%s' "
                  "name='%s' seq=%u",
                  pkt.hand[0], pkt.hand[1], pkt.hand[2], pkt.hand[3],
                  pkt.hand[4], pkt.ownerSid, cur.name, pkt.seq);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    // The AUDIT pass. Moving the owner faction is the part of a purchase the
    // MARKET sees, and the field report is that it is not the whole of what
    // makes a building a base: the buyer gets the datapanel, the access and the
    // research level, the peer only gets "no longer for sale". Both sides dump
    // the same wide state for the same hand, so the buyer-vs-peer DIFF names
    // exactly which of those states the apply still has to write. Diagnostic
    // only - nothing here writes.
    const unsigned long AUDIT_MS = 5000;
    if (!sync::gateSampleDue(now, deedAuditMs_, AUDIT_MS)) return;
    deedAuditMs_ = now;
    int audited = 0;
    for (std::map<Key, DeedRow>::iterator it = deedRows_.begin();
         it != deedRows_.end() && audited < 8; ++it) {
        if (it->second.knownOwned != 1) continue;
        unsigned int hand[5];
        hand[0] = it->first.t; hand[1] = it->first.c; hand[2] = it->first.cs;
        hand[3] = it->first.i; hand[4] = it->first.s;
        engine::DeedAudit a;
        if (!engine::auditDeed(gw, hand, &a)) continue; // unloaded here
        ++audited;
        char b[400];
        _snprintf(b, sizeof(b) - 1,
                  "[deed] AUDIT hand=%u.%u.%u.%u.%u isPlayer=%d set=%d sale=%d "
                  "shop=%d public=%d desig=%d resident=%d/%d town=%d/%d "
                  "interior=%d internals=%d doors=%d/%d/%d furn=%d/%d/%d "
                  "canUse=%d home=%d/%d/%d stuff=%d wallet=%d "
                  "squads=%d/%d/%d cur=%d/%d base=%d/%d owner='%s' name='%s'",
                  hand[0], hand[1], hand[2], hand[3], hand[4],
                  a.isPlayer, a.ownedSet, a.forSale, a.shop, a.isPublic,
                  a.designation, a.residentHand, a.residentLeader,
                  a.townHand, a.townMgr, a.interior, a.internals,
                  a.doors, a.doorsPlayer, a.doorsSet,
                  a.furn, a.furnPlayer, a.furnSet, a.canUse,
                  a.homeIsThis, a.homeSet, a.homeTown, a.stuffSize, a.wallet,
                  a.squads, a.squadsOwn, a.squadsHome, a.curOwn, a.curHome,
                  a.townPlayer, a.townLevel, a.ownerSid, a.name);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::applyDeeds(const SyncContext& ctx) {
    GameWorld* gw = ctx.gw; Inbound& in = *ctx.in;
    std::deque<InboundDeed> got;
    in.drainDeed(got);
    if (got.empty()) return;
    if (!deedSync_) return;
    unsigned long now = nowMs();
    for (std::deque<InboundDeed>::iterator it = got.begin(); it != got.end(); ++it) {
        const DeedPacket& p = it->pkt;
        Key k; k.t = p.hand[0]; k.c = p.hand[1]; k.cs = p.hand[2];
        k.i = p.hand[3]; k.s = p.hand[4];
        // A mixed-version peer may still publish a runtime placed-building
        // hand through the deed channel.  Protocol 27 owns that identity; do
        // not let a raw-hand deed collide with a local runtime object.
        if (ownBuilds_.find(k) != ownBuilds_.end() ||
            peerBuilds_.find(k) != peerBuilds_.end())
            continue;
        DeedRow& dr = deedRows_[k];
        if (!sync::gateSeqAccept(dr.seqSeen, p.seq)) continue; // stale/dup row
        dr.seqSeen = p.seq;
        int want = p.owned ? 1 : 0;
        if (dr.applied && dr.knownOwned == want) continue; // converged; resend no-op
        engine::DeedRead cur;
        if (!engine::readDeedByHand(gw, p.hand, &cur)) {
            // THE difference from the door channel: an unresolvable hand is not
            // an accepted edge here, it is the common case (the buyer is at the
            // building, we may be a continent away). Leave the row unapplied so
            // the sender's safety resend brings it back once the zone loads.
            continue;
        }
        if (cur.owned == want) { // already converged (our own echo, or the save)
            dr.applied = true; dr.knownOwned = want;
            continue;
        }
        // Baseline BEFORE the engine write, and count it as sent: the ownership
        // change this causes must not be re-detected as a local purchase and
        // bounced straight back at the author.
        dr.knownOwned = want; dr.sent = true; dr.lastSendMs = now;
        engine::DeedRead post;
        bool ok = engine::writeDeedByHand(gw, p.hand, want, p.ownerSid, &post);
        if (ok && post.owned == want) dr.applied = true;
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "[deed] RECV hand=%u.%u.%u.%u.%u owned=%d->%d ok=%d "
                  "forSale=%d->%d owner='%s' name='%s' seq=%u",
                  p.hand[0], p.hand[1], p.hand[2], p.hand[3], p.hand[4],
                  cur.owned, ok ? post.owned : cur.owned, ok ? 1 : 0,
                  cur.forSale, ok ? post.forSale : cur.forSale,
                  p.ownerSid, cur.name, p.seq);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

// ---- Protocol 55: runtime-fixture identity ----------------------------------
// See the FixturePacket comment in Wire.h for the measurement that motivates
// this channel (the same mine carrying hand 50.2399902464 on the host and
// 104.1377319296 on the join, at an identical world position).

// The census both halves share: the container/machine classes within the
// interest radius, i.e. exactly the set protocol 33 and 34 key their rows to.
// Seats and beds are save-baked and deliberately outside it - restricting what
// is ANNOUNCED is what keeps the furniture protocols on their existing path.
static const float FIXTURE_CENSUS_RADIUS = 100.0f; // matches prod/store census
static const unsigned int FIXTURE_MAX_ROWS = 48;

void Replicator::publishFixtures(const SyncContext& ctx) {
    GameWorld* gw = ctx.gw; NetLink& net = *ctx.net; u32 ownerId = ctx.localId;
    if (!fixtureSync_) return;
    const unsigned long SAMPLE_MS = 1000;  // census cadence
    const unsigned long RESEND_MS = 15000; // lost-row AND not-yet-loadable retry
    unsigned long now = nowMs();
    if (!sync::gateSampleDue(now, fixtureSampleMs_, SAMPLE_MS)) return;
    fixtureSampleMs_ = now;

    static engine::ContRead rows[FIXTURE_MAX_ROWS]; // main-thread only
    unsigned int n = engine::enumContainersNear(gw, FIXTURE_CENSUS_RADIUS, rows,
                                                FIXTURE_MAX_ROWS);
    for (unsigned int i = 0; i < n; ++i) {
        const engine::ContRead& r = rows[i];
        if (!r.sid[0]) continue; // no template = nothing the peer can match on
        Key k; k.t = r.hand[0]; k.c = r.hand[1]; k.cs = r.hand[2];
        k.i = r.hand[3]; k.s = r.hand[4];
        FixOut& fo = fixtureOut_[k];
        bool first = !fo.sent;
        if (!first && (now - fo.lastSendMs) < RESEND_MS) continue;
        fo.sent = true; fo.lastSendMs = now;
        FixturePacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type    = (u8)PKT_FIXTURE;
        pkt.ownerId = ownerId;
        pkt.seq     = fixtureSeqOut_++;
        for (unsigned int h = 0; h < 5; ++h) pkt.hand[h] = r.hand[h];
        pkt.x = r.x; pkt.y = r.y; pkt.z = r.z;
        pkt.classType = (u8)r.classType;
        strncpy(pkt.sid, r.sid, sizeof(pkt.sid) - 1);
        net.queueFixture(pkt);
        if (first) { // resends stay silent; a newly seen fixture is the signal
            char b[224];
            _snprintf(b, sizeof(b) - 1,
                      "[fixture] SEND hand=%u.%u.%u.%u.%u cls=%d pos=%.1f,%.1f "
                      "sid='%s' name='%s' seq=%u",
                      pkt.hand[0], pkt.hand[1], pkt.hand[2], pkt.hand[3],
                      pkt.hand[4], r.classType, r.x, r.z, r.sid, r.name, pkt.seq);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }
}

void Replicator::applyFixtures(const SyncContext& ctx) {
    GameWorld* gw = ctx.gw; Inbound& in = *ctx.in;
    std::deque<InboundFixture> got;
    in.drainFixture(got);
    if (got.empty()) return;
    if (!fixtureSync_) return;
    unsigned long now = nowMs();
    for (std::deque<InboundFixture>::iterator it = got.begin(); it != got.end(); ++it) {
        const FixturePacket& p = it->pkt;
        Key k; k.t = p.hand[0]; k.c = p.hand[1]; k.cs = p.hand[2];
        k.i = p.hand[3]; k.s = p.hand[4];
        FixtureRow& fr = fixtureMap_[k];
        if (!sync::gateSeqAccept(fr.seqSeen, p.seq)) continue; // stale/dup row
        fr.seqSeen = p.seq;
        if (fr.resolved) continue; // identity is permanent; resends are no-ops

        // Save-baked machines DO cross by hand, and those rows are the majority
        // in a town. Take the sender's hand as-is when it resolves here to the
        // same template at the same place - the position/sid agreement is what
        // makes it a proven pairing rather than a coincidental resolve.
        engine::ContRead cur;
        if (engine::readContainerByHand(p.hand, &cur) &&
            strcmp(cur.sid, p.sid) == 0 &&
            (cur.x - p.x) * (cur.x - p.x) + (cur.z - p.z) * (cur.z - p.z) <= 25.0f) {
            memcpy(fr.hand, p.hand, sizeof(unsigned int) * 5);
            fr.resolved = true;
            continue; // identity pairing: consumers keep using the wire hand
        }

        unsigned int lh[5];
        if (!engine::findFixtureByPosSid(gw, p.x, p.y, p.z, p.sid,
                                         (int)p.classType, lh)) {
            // Not loaded here yet. Record nothing and let the safety resend
            // retry; consumers meanwhile fall back to the raw hand.
            if (fr.lastMissMs == 0 || (now - fr.lastMissMs) >= 10000) {
                fr.lastMissMs = now;
                char b[208];
                _snprintf(b, sizeof(b) - 1,
                          "[fixture] RECV unmatched wire=%u.%u.%u.%u.%u cls=%u "
                          "pos=%.1f,%.1f sid='%s' seq=%u",
                          p.hand[0], p.hand[1], p.hand[2], p.hand[3], p.hand[4],
                          (unsigned)p.classType, p.x, p.z, p.sid, p.seq);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            continue;
        }
        memcpy(fr.hand, lh, sizeof(unsigned int) * 5);
        fr.resolved = true;
        char b[240];
        _snprintf(b, sizeof(b) - 1,
                  "[fixture] RECV pair wire=%u.%u.%u.%u.%u -> local=%u.%u.%u.%u.%u "
                  "cls=%u pos=%.1f,%.1f sid='%s' seq=%u",
                  p.hand[0], p.hand[1], p.hand[2], p.hand[3], p.hand[4],
                  lh[0], lh[1], lh[2], lh[3], lh[4], (unsigned)p.classType,
                  p.x, p.z, p.sid, p.seq);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        unparkForNewFixture();
    }
}

// A pose can lose the race with its fixture's pairing: the miner's task arrives
// on the 20 Hz entity stream, applyTaskOrder cannot resolve the unpaired hand,
// returns 1 and latches taskBad - which is a PERMANENT park until the task
// changes. The pairing that would have made it work then lands a moment later
// and nothing retries it, so the body stays parked for the whole mining job.
// Clearing the latch here is what closes that window.
//
// The sweep is deliberately not narrowed to the bodies using this fixture (a
// Driven does not record its subject): a pairing is a first-sight event, a few
// per session, and the cost of over-clearing is one extra apply attempt on the
// next tick for a body that was going to stay bad anyway.
void Replicator::unparkForNewFixture() {
    unsigned int n = 0;
    for (std::map<Key, Driven>::iterator it = targets_.begin();
         it != targets_.end(); ++it) {
        Driven& d = it->second;
        if (!d.taskBad || d.taskApplied) continue;
        d.taskBad = false; d.taskRetries = 0;
        ++n;
    }
    if (n) {
        char b[96];
        _snprintf(b, sizeof(b) - 1,
                  "[fixture] pairing cleared %u parked pose(s) for retry", n);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

bool Replicator::localHandForFixtureKey(const Key& wire, unsigned int out[5]) const {
    if (!out) return false;
    std::map<Key, FixtureRow>::const_iterator it = fixtureMap_.find(wire);
    if (it == fixtureMap_.end() || !it->second.resolved) return false;
    memcpy(out, it->second.hand, sizeof(unsigned int) * 5);
    return true;
}

// Build-site pose subject translation. A construction site is created at RUNTIME,
// so unlike every other pose fixture its hand is client-local: a placement logged
// 0.3409.11111.2.3348877056 on the placer and ...2871948288 on the peer. A streamed
// BUILD/JOB_BUILDER pose therefore has to cross in the placer's key space, or the
// peer resolves nothing, parks the builder, and the body just jitters under the
// position drive with no construction animation.
bool Replicator::buildKeyForLocalHand(const Key& local, Key& outWire) const {
    // A site WE placed already IS the key - nothing to rewrite.
    if (ownBuilds_.find(local) != ownBuilds_.end()) {
        outWire = local;
        return true;
    }
    // A site we MINTED for the peer's placement: express it as the placer's key.
    std::map<Key, Key>::const_iterator mit = mintByLocal_.find(local);
    if (mit != mintByLocal_.end()) {
        outWire = mit->second;
        return true;
    }
    return false; // not a placed site (save-resident building) - no translation
}

bool Replicator::localHandForBuildKey(const Key& wire, unsigned int out[5]) const {
    if (!out) return false;
    std::map<Key, OwnBuild>::const_iterator ob = ownBuilds_.find(wire);
    if (ob != ownBuilds_.end()) {
        if (ob->second.removed) return false;
        memcpy(out, ob->second.hand, sizeof(unsigned int) * 5);
        return true;
    }
    std::map<Key, PeerBuild>::const_iterator pb = peerBuilds_.find(wire);
    if (pb != peerBuilds_.end() && pb->second.minted == 1 && !pb->second.removed) {
        memcpy(out, pb->second.localHand, sizeof(unsigned int) * 5);
        return true;
    }
    return false; // unknown / refused / tombstoned key
}

bool Replicator::ensurePeerBuildOwnership(GameWorld* gw, const Key& wire,
                                          PeerBuild& pb) {
    if (!peerBuildNeedsOwnership(pb.minted, pb.removed,
                                 pb.ownershipApplied))
        return pb.ownershipApplied;

    // The native factory was given the local player faction.  Prefer that
    // result and avoid inserting every placed chair/bench into the separate
    // property-deed set when the normal creation path already made it owned.
    engine::DeedRead cur;
    if (engine::readDeedByHand(gw, pb.localHand, &cur) && cur.owned == 1) {
        pb.ownershipApplied = true;
        return true;
    }

    // Backstop for engine/template variants where the factory sets only part
    // of the ownership state.  This is idempotent and uses the same proven
    // state write as protocol 54 without replaying a purchase or charging cats.
    engine::DeedRead post;
    bool ok = engine::writeDeedByHand(gw, pb.localHand, /*wantOwned*/1,
                                      /*ownerSid*/"", &post);
    pb.ownershipApplied = ok && post.owned == 1;

    char b[224];
    _snprintf(b, sizeof(b) - 1,
              "[build] OWNER-CLAIM key=%u.%u.%u.%u.%u "
              "local=%u.%u.%u.%u.%u ok=%d owned=%d",
              wire.t, wire.c, wire.cs, wire.i, wire.s,
              pb.localHand[0], pb.localHand[1], pb.localHand[2],
              pb.localHand[3], pb.localHand[4], ok ? 1 : 0,
              ok ? post.owned : -1);
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    return pb.ownershipApplied;
}

void Replicator::publishBuilds(const SyncContext& ctx) {
    NetLink& net = *ctx.net; u32 ownerId = ctx.localId;
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
    const unsigned long SAMPLE_MS = tuning_.buildSampleMs;
    const unsigned long RESEND_MS = tuning_.buildResendMs;
    unsigned long now = nowMs();
    if (!sync::gateSampleDue(now, buildSampleMs_, SAMPLE_MS)) return;
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
        // The PLACE seeds lastProg=-1, so the first sample always diverges;
        // thereafter a real progress move or a post-send resend crosses.
        if (!sync::gateShouldSend(changed, now, ob.lastSendMs, /*minSendMs*/ 0,
                                  RESEND_MS, /*resendUnsent*/ false))
            continue;
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

void Replicator::applyBuilds(const SyncContext& ctx) {
    GameWorld* gw = ctx.gw; Inbound& in = *ctx.in;
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
        std::map<Key, PeerBuild>::iterator existing = peerBuilds_.find(k);
        if (existing != peerBuilds_.end()) {
            // A PLACE safety resend is also an ownership retry.  The factory
            // mint may succeed one tick before the ownership subsystem is
            // ready; deduping the packet must not leave a permanent orphan.
            ensurePeerBuildOwnership(gw, k, existing->second);
            continue; // already minted (or mint already refused) - dedupe
        }
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
            ensurePeerBuildOwnership(gw, k, pb);
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
        // Retry before the seq gate: even a duplicate safety row is useful if
        // the previous ownership write ran before the engine was ready.
        ensurePeerBuildOwnership(gw, k, pb);
        if (!sync::gateSeqAccept(pb.seqSeen, p.seq)) continue; // stale/dup row
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
        // The placer's own isComplete drives completion here - see the note on
        // writeBuildProgressByHand. Inferring it from progress crossing 1.0
        // finished the peer's copy long before the placer's.
        bool ok = engine::writeBuildProgressByHand(pb.localHand, p.progress,
                                                   p.complete != 0, &post);
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

// Phase 6c: the channel-descriptor registry for the change-gated SAMPLED
// channels. Each row names a channel and how the tick drives it - its master
// enable (a Replicator bool member), an optional SECOND enable (build doors
// ride buildSync_ AND bdoorSync_), its publish/apply members (uniform
// SyncContext signature), and its DIRECTION: BOTH = symmetric detector on both
// clients (the applied-row baseline keeps it echo-free); HOST_AUTH = the host
// publishes and the join applies (world-simulation authority, no echo path).
// The array ORDER is the wire cadence order and MUST match the historical
// publish sequence the explicit tick blocks had. Adding a sampled channel is
// one row here plus its publish/apply pair - no edit to the Plugin tick.
// Built inside a member function so the pointer-to-private-members resolve.
void Replicator::driveSampledChannels(const SyncContext& ctx) {
    typedef void (Replicator::*ChFn)(const SyncContext&);
    struct Desc {
        bool Replicator::* enable;   // master gate (== the old g_cfg.* gate)
        bool Replicator::* enable2;  // second gate (0 = none)
        ChFn               publish;
        ChFn               apply;
        bool               hostAuth; // true = host publishes / join applies
    };
    static const Desc kCh[] = {
        { &Replicator::factionSync_,  0,                     &Replicator::publishFactions,   &Replicator::applyFactions,   false },
        { &Replicator::doorSync_,     0,                     &Replicator::publishDoors,      &Replicator::applyDoors,      false },
        { &Replicator::buildSync_,    0,                     &Replicator::publishBuilds,     &Replicator::applyBuilds,     false },
        { &Replicator::buildSync_,    &Replicator::bdoorSync_, &Replicator::publishBuildDoors, &Replicator::applyBuildDoors, false },
        // Ahead of prod deliberately: prod rows resolve THROUGH the fixture map,
        // so a pairing that arrives this tick is usable by the very next row.
        { &Replicator::fixtureSync_,  0,                     &Replicator::publishFixtures,   &Replicator::applyFixtures,   false },
        { &Replicator::prodSync_,     0,                     &Replicator::publishProd,       &Replicator::applyProd,       true  },
        { &Replicator::researchSync_, 0,                     &Replicator::publishResearch,   &Replicator::applyResearch,   true  },
        { &Replicator::deedSync_,     0,                     &Replicator::publishDeeds,      &Replicator::applyDeeds,      false }
    };
    const int n = (int)(sizeof(kCh) / sizeof(kCh[0]));
    for (int i = 0; i < n; ++i) {
        const Desc& d = kCh[i];
        if (!(this->*(d.enable))) continue;
        if (d.enable2 && !(this->*(d.enable2))) continue;
        if (d.hostAuth) {
            if (ctx.isHost) (this->*(d.publish))(ctx);
            else            (this->*(d.apply))(ctx);
        } else {
            (this->*(d.publish))(ctx);
            (this->*(d.apply))(ctx);
        }
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
                 nMoney = 0, nInv = 0, nWorld = 0, nProd = 0, nDeed = 0;
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
    // The money pool is a single value, not a row cache: age its send stamp so
    // publishMoneyPool re-broadcasts the authoritative total on its next sample.
    if (poolSentMs_ != 0) { poolSentMs_ = 1; nMoney = 1; }
    for (std::map<Key, InvPub>::iterator it = invPub_.begin();
         it != invPub_.end(); ++it)
        if (it->second.lastSendMs != 0) { it->second.lastSendMs = 1; ++nInv; }
    for (std::map<Key, WorldTrack>::iterator it = worldTrack_.begin();
         it != worldTrack_.end(); ++it)
        if (it->second.lastSendMs != 0) { it->second.lastSendMs = 1; ++nWorld; }
    for (std::map<std::pair<int, Key>, ProdRow>::iterator it = prodRows_.begin();
         it != prodRows_.end(); ++it)
        if (it->second.lastSendMs != 0) { it->second.lastSendMs = 1; ++nProd; }
    // Deeds send with resendUnsent, so a fresh peer would learn the owned set
    // on the next 1 Hz sample regardless; ageing the stamps just means it does
    // not wait out the 15 s resend window for deeds already published.
    for (std::map<Key, DeedRow>::iterator it = deedRows_.begin();
         it != deedRows_.end(); ++it)
        if (it->second.lastSendMs != 0) { it->second.lastSendMs = 1; ++nDeed; }

    char b[240];
    _snprintf(b, sizeof(b) - 1,
              "[latejoin] RESYNC place=%u remove=%u fac=%u door=%u bdoor=%u "
              "med=%u stats=%u money=%u inv=%u world=%u prod=%u deed=%u",
              nPlace, nRemove, nFac, nDoor, nBdoor, nMed, nStats, nMoney,
              nInv, nWorld, nProd, nDeed);
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
}

void Replicator::publishBuildDoors(const SyncContext& ctx) {
    NetLink& net = *ctx.net; u32 ownerId = ctx.localId;
    if (!bdoorSync_) return;
    const unsigned long SAMPLE_MS = tuning_.bdoorSampleMs; // the protocol-26 door cadence
    const unsigned long RESEND_MS = tuning_.bdoorResendMs; // safety resend for rows ever sent
    unsigned long now = nowMs();
    if (!sync::gateSampleDue(now, bdoorSampleMs_, SAMPLE_MS)) return;
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
                // Seeded silently above (resendUnsent=false); like protocol-26
                // doors, only a real toggle or a post-send resend crosses.
                if (!sync::gateShouldSend(changed, now, row.lastSendMs,
                                          /*minSendMs*/ 0, RESEND_MS,
                                          /*resendUnsent*/ false))
                    continue;
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

void Replicator::applyBuildDoors(const SyncContext& ctx) {
    Inbound& in = *ctx.in;
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
        if (!sync::gateSeqAccept(row.seqSeen, p.seq)) continue; // stale/dup row
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
    unsigned long nowSq = nowMs();
    // Retire hands we already announced as gone (bounded; see exitedOwn_).
    for (std::map<Key, unsigned long>::iterator xt = exitedOwn_.begin();
         xt != exitedOwn_.end(); ) {
        if ((nowSq - xt->second) > (unsigned long)SQUAD_EXIT_GRACE_MS) exitedOwn_.erase(xt++);
        else ++xt;
    }
    for (unsigned int i = 0; i < n; ++i) {
        // Our own insertPeerMember re-container, not a user action: publishing
        // it would claim the peer's character as ours (see moveEcho_).
        u32 serial = edges[i].before[4] ? edges[i].before[4] : edges[i].after[4];
        std::map<u32, unsigned long>::iterator me = moveEcho_.find(serial);
        if (me != moveEcho_.end()) {
            bool fresh = (nowSq - me->second) <= (unsigned long)SQUAD_ECHO_MS;
            moveEcho_.erase(me);
            if (fresh) {
                char eb[176]; _snprintf(eb, sizeof(eb) - 1,
                    "[squad] EVT skip echo old=%u,%u,%u,%u,%u new=%u,%u,%u,%u,%u",
                    edges[i].before[0], edges[i].before[1], edges[i].before[2],
                    edges[i].before[3], edges[i].before[4],
                    edges[i].after[0], edges[i].after[1], edges[i].after[2],
                    edges[i].after[3], edges[i].after[4]);
                eb[sizeof(eb) - 1] = '\0'; coop::logLine(eb);
                continue;
            }
        }
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
        // ...and if it LEFT, retire the hand so the peer's in-flight tail cannot
        // turn our departed body into a driven one (see exitedOwn_).
        else exitedOwn_[ok] = nowSq;
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
        // An EMPTY snapshot is the authority saying "no one sees you anymore".
        // The entries do NOT age out on their own: the engine only decays
        // whoSeesMeSneaking while its stealth update runs, so after the sneak
        // ends the last replayed set stays frozen on the owner. Clear it here or
        // the detection arrows/status linger for the rest of the session.
        unsigned int cleared = 0;
        if (n == 0 && engine::clearStealthSeers(c)) cleared = 1;
        char b[176]; _snprintf(b, sizeof(b) - 1,
            "[sneak] DETECT RECV hand=%u,%u seers=%u applied=%u unseen=%u cleared=%u",
            k.i, k.s, n, applied, (unsigned)p.unseen, cleared);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
}

void Replicator::syncSpeed(GameWorld* gw, Inbound& in, NetLink& net, u32 ownerId,
                           bool isHost) {
    const unsigned long RESEND_MS        = 3000; // safety resend (late join / lost state)
    const unsigned long COMBAT_SAMPLE_MS = 1000; // own-squad combat poll cadence
    const unsigned long COMBAT_HOLD_MS   = 4000; // cap hysteresis: hold past brief gaps
    const float         EPS              = 0.01f;
    unsigned long now = nowMs();

    // Own-squad combat flag, ~1 Hz (readCombat per owned member; the cap only
    // needs second-level reactivity). An edge forces a REQ send below so the
    // host's cap reacts faster than the safety resend.
    //
    // Hysteresis (Phase 5 spike 2026-07-17): readCombat drops to false in the
    // gap between an enemy being KO'd and the next attacker engaging, so a raw
    // per-sample flag makes the consensus cap FLAP (SET bounced 1x<->3x mid
    // fight, only 0.667 of the combat window sat at 1x). Hold the cap for
    // COMBAT_HOLD_MS past the last TRUE read so a momentary lull never releases
    // the speed - the indicator/effective stay steady while fighting continues.
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
        if (fighting) speedCombatHoldMs_ = now;
        bool held = (speedCombatHoldMs_ != 0) && ((now - speedCombatHoldMs_) < COMBAT_HOLD_MS);
        combatEdge     = (held != speedMyCombat_);
        speedMyCombat_ = held;
    }
    // Phase 5 spike: expose the combat-cap state so the speed-setter
    // diagnostics (KENSHICOOP_DEBUG_SPEED) can distinguish an engine-forced
    // combat cap from a user click by context.
    engine::setSpeedCombatHint(speedMyCombat_ || speedPeerCombat_);

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
        if (combat && speedCombatCap_ && eff > 1.0f) eff = 1.0f;
        bool changed = (speedLastSet_ < 0.0f || fabs(eff - speedLastSet_) > EPS);
        // userActed with an UNCHANGED effective = a denied raise (consensus
        // holdback): re-apply immediately so the host engine doesn't run fast
        // until the enforcement below - a click is a request, not an override.
        if (changed || userActed || speedLastSendMs_ == 0 ||
            (now - speedLastSendMs_) >= RESEND_MS) {
            bool effPaused = (eff <= EPS);
            // slewedEffective carries the clock brake on the host exactly as it
            // carries the slew on the join (protocol 25). What goes out on the
            // wire below is the UNSLEWED eff: the brake is the host's own sim
            // running slow to let the join close a gap, and broadcasting it
            // would slow the join by the same factor and close nothing.
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

    // Phase 5 continuous indicator reconcile: the buttons show the VOTE, but
    // engine-forced changes (dialog auto-pause re-highlight, the combat cap, a
    // denied write's dehighlight) drag the MyGUI highlight off it. Unless the
    // player acted THIS tick (in which case the new click owns the highlight),
    // snap the buttons back to the captured vote so the indicator self-heals
    // within a frame and always reflects the player's request.
    if (!userActed) engine::reconcileVoteButtons();
}

void Replicator::syncTime(GameWorld* gw, Inbound& in, NetLink& net, u32 ownerId,
                          bool isHost) {
    std::deque<InboundTime> got;
    in.drainTime(got);
    if (!timeSync_) return;
    unsigned long now = nowMs();

    if (isHost) {
        // THE BRAKE (2026-08-06). The host used to be a pure clock reference -
        // it broadcast its hours and never corrected, leaving the join to close
        // every offset by running FASTER. That works only while there is room
        // above the consensus speed, and slewedEffective clamps at 5x, which is
        // also where a session that wants to travel sits. Measured at that
        // ceiling (run 20260806_153111): the join held slew=2.00 for all 168 s
        // and the two clocks advanced at 137.3 and 137.1 game-seconds per real
        // second - the same rate - with the offset flat at 0.16 gh throughout.
        // The join was asking for 10x, receiving 5x, and the controller was
        // saturated and open-loop for the entire run.
        //
        // So give the correction its other direction. Down has room at every
        // speed: the host slows ITSELF while the join is behind and cannot
        // catch up on its own. What is broadcast stays the unslewed consensus,
        // exactly as the join's own slew is local to the join - if the brake
        // went out on the wire both sims would slow together and the gap would
        // never close.
        //
        // This matters well beyond the clock reading right. 0.16 gh is about
        // ten game-minutes, and a townsperson walks a few hundred units in
        // that: the host's copy is that much further along its route than the
        // join's, which is the same magnitude as the divergence the census park
        // and walk-converge bands spend the whole town correcting. A clock gap
        // is a position gap everywhere at once.
        const TimePacket* jn = 0;
        for (std::deque<InboundTime>::iterator it = got.begin();
             it != got.end(); ++it) {
            if (timeSeqSeen_ != 0 && (long)(it->pkt.seq - timeSeqSeen_) <= 0)
                continue;
            timeSeqSeen_ = it->pkt.seq;
            jn = &it->pkt;
        }
        if (jn && timeBrake_) {
            double localH = -1.0;
            if (engine::readGameClock(gw, &localH, 0) && localH >= 0.0) {
                double lag = localH - jn->gameHours; // >0 = the join is BEHIND
                // How much catch-up the join can still muster on its own,
                // computed rather than reported: its slew caps at 2x and
                // slewedEffective clamps the product at 5x, so the consensus
                // speed alone says whether it has anywhere left to go. At 2.5x
                // and below it can still double; at 5x it has nothing. Braking
                // when the join can fix the gap itself would slow the host
                // player's game for no reason.
                float eff  = (speedLastSet_ > 0.01f) ? speedLastSet_ : 1.0f;
                float head = 5.0f / eff;          // ceiling / consensus
                if (head > 2.0f) head = 2.0f;     // ...but its own cap first
                const double B_ENGAGE_GH    = 0.01;   // as the join's
                const double B_DISENGAGE_GH = 0.002;
                const double B_GAIN         = 15.0;   // gentler: the slow loop
                const double B_FLOOR        = -0.40;  // never below 0.6x
                bool braking = (timeSlew_ < 0.999f);
                bool engage  = head < 1.10f &&
                               (braking ? (lag > B_DISENGAGE_GH)
                                        : (lag > B_ENGAGE_GH));
                float newSlew = 1.0f;
                if (engage) {
                    double d = -lag * B_GAIN;
                    if (d < B_FLOOR) d = B_FLOOR;
                    newSlew = (float)(1.0 + d);
                }
                bool slewChanged = fabs(newSlew - timeSlew_) > 0.01f;
                timeSlew_ = newSlew;
                if (slewChanged && speedLastSet_ > 0.01f) {
                    float want = slewedEffective(speedLastSet_);
                    if (engine::writeGameSpeedQuiet(gw, want, false))
                        timeSlewApplied_ = want;
                }
                if (slewChanged || timeLastLogMs_ == 0 ||
                    (now - timeLastLogMs_) >= 5000) {
                    timeLastLogMs_ = now;
                    char b[176]; _snprintf(b, sizeof(b) - 1,
                        "[time] BRAKE lag=%.4fgh slew=%.2f head=%.2f eff=%.2f "
                        "local=%.5f join=%.5f",
                        lag, timeSlew_, head, eff, localH, jn->gameHours);
                    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                }
            }
        }
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

    // Report our own clock back, on the same channel and cadence. The host
    // cannot brake for a lag it cannot see, and this is the whole of what it
    // needs: it derives the join's remaining headroom from the consensus speed
    // it set itself. Sent from inside the sample handler, so the report is
    // naturally paced by the host's own 1 Hz broadcast and a join that is
    // hearing nothing says nothing.
    const unsigned long REPORT_MS = 1000;
    if (timeLastSendMs_ == 0 || (now - timeLastSendMs_) >= REPORT_MS) {
        timeLastSendMs_ = now;
        TimePacket mine;
        memset(&mine, 0, sizeof(mine));
        mine.type      = (u8)PKT_TIME;
        mine.ownerId   = ownerId;
        mine.seq       = timeSeqOut_++;
        mine.gameHours = local;
        net.queueTime(mine);
    }
}


} // namespace coop
