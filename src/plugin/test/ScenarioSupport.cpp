// ScenarioSupport.cpp - shared scenario helpers (monolith split from
// Scenario.cpp, 2026-07-12): the SCENARIO MEMBER/RECV/VITALS line emitters
// and the squad-tab classification used by every domain TU. Declared in
// ScenarioSupport.h; external linkage on purpose.
//
// Must NOT: change any log string - "SCENARIO ..." phrasing and field order
// are the API the PowerShell oracles key on (resources/CODE_MAP.md).

#include "ScenarioSupport.h"

namespace coop {

// Log one "SCENARIO <kind> hand=i,s,t,c,cs pos=x,y,z" line for character 'c'.
// kind is "MEMBER" (authoritative) or "RECV" (observer). Returns false if the
// character's hand/pos couldn't be read.
bool logScenarioLine(const char* kind, Character* c) {
    if (!c) return false;
    unsigned int h[5];
    float x, y, z;
    if (!engine::readHand(c, h)) return false;
    if (!engine::readPos(c, &x, &y, &z)) return false;
    char b[160];
    _snprintf(b, sizeof(b) - 1,
              "SCENARIO %s hand=%u,%u,%u,%u,%u pos=%.2f,%.2f,%.2f",
              kind, h[0], h[1], h[2], h[3], h[4], x, y, z);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
    return true;
}

// Log a SCENARIO line straight from a captured EntityState. The hand field order
// (index,serial,type,container,containerSerial) MUST match logScenarioLine so the
// runner keys MEMBER and RECV lines identically.
void logScenarioEntity(const char* kind, const EntityState& e) {
    // task=<TaskType|65535> lets the runner's pose oracle compare the host's
    // current action against the join's reproduced one for seated NPCs.
    //
    // pelvis/crouch/idle are the AUTHORITATIVE pose oracle: read straight off the
    // rendered skeleton (pelvis = Bip01 height above ground; seated ~0.4-0.6 m,
    // standing ~0.9-1.1 m). We read them HERE (not in captureNpcs) by resolving the
    // hand back to the local Character* and calling readPoseState - this keeps pose
    // reading OFF the streaming-capture path (which broke host capture before). On
    // fault / unresolved, pelvis=-1 and the runner skips that sample.
    float pelvis = -1.0f; int crouch = -1, idle = -1, ptask = (int)e.task;
    Character* c = engine::resolve(e);
    // crouch=-2 is a diagnostic marker meaning the hand did NOT resolve to a local
    // Character here (so we never even called readPoseState) - distinct from -1
    // (resolved but the pose read returned nothing).
    if (c) engine::readPoseState(c, &pelvis, &idle, &crouch, &ptask);
    else   crouch = -2;
    char b[256];
    _snprintf(b, sizeof(b) - 1,
              "SCENARIO %s hand=%u,%u,%u,%u,%u pos=%.2f,%.2f,%.2f task=%u "
              "pelvis=%.2f crouch=%d idle=%d bs=%u",
              kind, e.hIndex, e.hSerial, e.hType, e.hContainer, e.hContainerSerial,
              e.x, e.y, e.z, (unsigned int)e.task, pelvis, crouch, idle,
              (unsigned int)e.bodyState);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}

// Log one extended "SCENARIO VITALS" line for the body at hand h (readObjectHand
// layout: [type,container,containerSerial,index,serial]). The prefix
// "hand=i,s t=.. blood=.." is byte-compatible with the short form combat_kill
// emits (Test-DamageGuard anchors on it); the medical-model fields the player-
// combat/medic oracles need follow after. fl = per-limb flesh, bd = per-limb
// bandaging, order LA,RA,LL,RL (RobotLimbs::Limb). Protocol-16 fields append
// AFTER dead= (older regexes have no $ anchor, so they keep matching): pfl/pst
// = min flesh / min stun across the FULL anatomy (head/chest/stomach included),
// ls = the 4 LimbStates (255 = unknown). Silent no-op if unresolved.
void logVitalsLine(const unsigned int h[5], unsigned long t) {
    engine::MedicalRead mr;
    if (!engine::readMedicalByHand(h, &mr) || !mr.valid) return;
    float pfl = 1e9f, pst = 1e9f;
    for (unsigned int i = 0; i < mr.nParts && i < 12; ++i) {
        if (!mr.parts[i].used) continue;
        if (mr.parts[i].flesh     >= 0.0f && mr.parts[i].flesh     < pfl) pfl = mr.parts[i].flesh;
        if (mr.parts[i].fleshStun >= 0.0f && mr.parts[i].fleshStun < pst) pst = mr.parts[i].fleshStun;
    }
    if (pfl > 1e8f) pfl = -1.0f;
    if (pst > 1e8f) pst = -1.0f;
    char b[320];
    _snprintf(b, sizeof(b) - 1,
              "SCENARIO VITALS hand=%u,%u t=%lu blood=%.1f bleed=%.2f "
              "fl=%.1f,%.1f,%.1f,%.1f bd=%.1f,%.1f,%.1f,%.1f unc=%d dead=%d "
              "pfl=%.1f pst=%.1f ls=%u,%u,%u,%u",
              h[3], h[4], t, mr.blood, mr.bleedRate,
              mr.limbFlesh[0], mr.limbFlesh[1], mr.limbFlesh[2], mr.limbFlesh[3],
              mr.limbBand[0], mr.limbBand[1], mr.limbBand[2], mr.limbBand[3],
              mr.unconscious ? 1 : 0, mr.dead ? 1 : 0,
              pfl, pst,
              (unsigned)mr.limbState[0], (unsigned)mr.limbState[1],
              (unsigned)mr.limbState[2], (unsigned)mr.limbState[3]);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}

// ---- Squad-tab classification (shared by the player_* / medic scenarios) ----
// Mirrors CoopPresenceScenario's partition EXACTLY (and therefore the
// Replicator's): a member's tab identity is its hand CONTAINER, and a tab's
// rank is its position among the distinct containers sorted ascending. Host
// owns rank 0, join owns rank 1.
bool tabHandLess(const EntityState& a, const EntityState& b) {
    if (a.hType != b.hType) return a.hType < b.hType;
    if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
    if (a.hContainerSerial != b.hContainerSerial) return a.hContainerSerial < b.hContainerSerial;
    if (a.hIndex != b.hIndex) return a.hIndex < b.hIndex;
    return a.hSerial < b.hSerial;
}
bool tabCtnrLess(const EntityState& a, const EntityState& b) {
    if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
    return a.hContainerSerial < b.hContainerSerial;
}
bool tabCtnrEq(const EntityState& a, const EntityState& b) {
    return a.hContainer == b.hContainer && a.hContainerSerial == b.hContainerSerial;
}
// Rank of member i's squad tab among the distinct, sorted containers (-1 unknown).
int tabRankOf(const EntityState* sq, unsigned int n, unsigned int i) {
    const unsigned int MAXT = 32;
    EntityState distinct[MAXT]; unsigned int dn = 0;
    for (unsigned int a = 0; a < n; ++a) {
        bool seen = false;
        for (unsigned int b = 0; b < dn; ++b) if (tabCtnrEq(distinct[b], sq[a])) { seen = true; break; }
        if (!seen && dn < MAXT) distinct[dn++] = sq[a];
    }
    for (unsigned int a = 1; a < dn; ++a)
        for (unsigned int b = a; b > 0 && tabCtnrLess(distinct[b], distinct[b-1]); --b) {
            EntityState t = distinct[b]; distinct[b] = distinct[b-1]; distinct[b-1] = t;
        }
    for (unsigned int b = 0; b < dn; ++b) if (tabCtnrEq(distinct[b], sq[i])) return (int)b;
    return -1;
}
// The lowest-hand member of tab 'rank' ("that tab's leader" - the same stable
// pick coop_presence uses for its mover). Returns the index into sq, or -1.
int tabLeaderIdx(const EntityState* sq, unsigned int n, unsigned int rank) {
    int best = -1;
    for (unsigned int i = 0; i < n; ++i) {
        if (tabRankOf(sq, n, i) != (int)rank) continue;
        if (best < 0 || tabHandLess(sq[i], sq[best])) best = (int)i;
    }
    return best;
}
// Fill h[5] (readObjectHand layout) from a captured EntityState's hand fields.
void handFromEntity(const EntityState& e, unsigned int h[5]) {
    h[0] = e.hType; h[1] = e.hContainer; h[2] = e.hContainerSerial;
    h[3] = e.hIndex; h[4] = e.hSerial;
}

} // namespace coop
