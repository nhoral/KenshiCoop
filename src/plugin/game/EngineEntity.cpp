// EngineEntity.cpp - entity capture / resolve / apply (monolith split from
// EngineInternal.cpp, 2026-07-12): EntityState capture (captureOne/captureSquad/
// captureNpcs + interest centers), hand resolve (resolve/resolveCharByHand),
// motion apply (applyRaw/orderMoveTo/walkTo/park), NPC suppression, and the
// seat/bed/cage/machine template + work-fixture finders shared by scenario
// scenes. (The debug marker HUD + co-op panel/overlay moved to EngineUi.cpp in
// Phase 5e.)
//
// Owner state: section-private statics/anon-namespace helpers only (pose
// classifiers, marker SEH shims, capture hysteresis buffers).
// Must NOT: define g_* engine pointers (EngineInternal.cpp owns them - EngineInternal.h
// declares them), install hooks, or change any log string - log phrasing is
// the API consumed by the PowerShell oracles (see resources/CODE_MAP.md).

#include "EngineInternal.h"

// The co-op session panel + status overlay (the DatapanelGUI/Win32/clipboard
// surface) moved to EngineUi.cpp in Phase 5e, taking its <kenshi/gui/...>,
// <mygui/MyGUI_Delegate.h>, <windows.h> and SteamId.h includes with it.

namespace coop {
namespace engine {

// ---- Entity capture / resolve / apply --------------------------------------

namespace {
// Anchored rest poses worth reproducing: the body stays put AT a fixture (a stool,
// throne, bed, machine), so committing the same task on the join seats/poses it in
// place. We deliberately EXCLUDE movement tasks (WANDER_TOWN, GO_TO_THE_BAR...) and
// plain standing (STAND_STILL/IDLE): reproducing a wander task walks the body away
// (tens of metres of drift), and a standing pose is visually identical to a park.
bool isReproduciblePose(int t) {
    switch (t) {
        case SIT_AROUND:
        case SIT_ON_THRONE:
        case REST:
        case RELAX_IN_TOWN_PACKAGE:
        case USE_BED:
        case USE_BED_ORDER:
        case SLEEP_ON_FLOOR:
        // Crafting / gathering / work poses (Stage 3a). All of these pin the body
        // AT a work fixture whose subject hand resolves cross-client, exactly like
        // sitting, so the player-order path (applyTaskOrder) reproduces them in
        // place. Mining drills, farm plots, research benches and smithies all run
        // through OPERATE_MACHINERY; the others cover automatic machines, training
        // dummies and the ambient "pretend to work" town pose.
        case OPERATE_MACHINERY:
        case OPERATE_AUTOMATIC_MACHINERY:
        case USE_TRAINING_DUMMY:
        case PRETEND_TO_OPERATE_MACHINERY:
        // Medic / first-aid poses (2026-07-15 medic sync). A player treating a
        // wounded ally runs one of these; unlike a seat/machine the SUBJECT is the
        // PATIENT (a character), not a building. Its hand resolves cross-client (both
        // clients loaded the same squad), so the player-order path reproduces the
        // bandaging animation on the peer. The healing EFFECT already replicates via
        // the owner-authoritative PKT_MEDICAL snapshot; this is animation parity only.
        // FIRST_AID_ORDER = right-click First Aid; JOB_MEDIC = the medic job;
        // FIRST_AID_ROBOT / JOB_REPAIR_ROBOT = the skeleton-repair equivalents.
        case FIRST_AID_ORDER:
        case JOB_MEDIC:
        case FIRST_AID_ROBOT:
        case JOB_REPAIR_ROBOT:
            return true;
        default:
            return false;
    }
}

// Node-anchored rest poses: the body sits/idles AT an AI node. The node subject is
// not a resolvable RootObject, so applyTask cannot reproduce it - only the body's
// own local AI can, by executing the node. Used to decide NOT to suspend/park these.
bool isNodeAnchoredPoseImpl(int t) {
    switch (t) {
        case STAND_AT_NODE:
        case STAND_AT_SHOPKEEPER_NODE:
            return true;
        default:
            return false;
    }
}

// DEBUG (host-side): log each distinct task key seen among captured bodies once,
// with whether we treat it as a reproducible rest pose. Reveals exactly which
// tasks the bar's seated NPCs use so the allowlist can be widened. Lives outside
// any __try so the std::set's allocations don't violate MSVC's SEH/unwind rule.
void logTaskKeyOnce(int k, bool hasSubject, const char* desc) {
    static std::set<int> seen;
    if (seen.insert(k).second) {
        char b[160];
        _snprintf(b, sizeof(b) - 1, "[taskkey] key=%d desc='%s' repro=%d subject=%d",
                  k, (desc && desc[0]) ? desc : "?",
                  isReproduciblePose(k) ? 1 : 0, hasSubject ? 1 : 0);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
    }
}

// DIAGNOSTIC: resolve a subject (seat) hand to its world position. POD-only locals
// in its own SEH frame (no C++ unwinding objects) so a bad handle degrades to false.
bool resolveSubjectPos(u32 idx, u32 ser, u32 type, u32 cont, u32 contSer,
                       float* x, float* y, float* z) {
    if (!g_handGetRootFn || !g_handCtorFn) return false;
    __try {
        char buf[sizeof(hand) + 16];
        memset(buf, 0, sizeof(buf));
        hand* h = reinterpret_cast<hand*>(buf);
        g_handCtorFn(h, idx, ser, (itemType)type, cont, contSer);
        RootObject* r = g_handGetRootFn(h);
        if (!r) return false;
        Ogre::Vector3 p = r->getPosition();
        *x = p.x; *y = p.y; *z = p.z;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

// DIAGNOSTIC (host-side): once per (npc,fixture) pair, log where THIS client
// resolves the subject handle (seat, machine, dummy, bed...) vs where the NPC
// actually is. Comparing the host's and join's "[seatres]" lines for the same
// handle answers whether the fixture identity correlates across clients (same
// pos) or not. The task= field distinguishes seated poses from crafting/gathering
// work stations (OPERATE_MACHINERY etc.), so the same line doubles as the
// craft-subject ([craftres]) diagnostic for Stage 3a.
// External linkage (EngineInternal.h): also emitted from the spawn/combat TU.
void logSeatResolveOnce(const char* side, int task, u32 npcIdx, u32 npcSer,
                        u32 sIdx, u32 sSer, u32 sType, u32 sCont, u32 sContSer,
                        float npx, float npy, float npz) {
    static std::set<std::pair<u32, u32> > seen;
    if (!seen.insert(std::make_pair(npcIdx, sIdx)).second) return;
    float sx = 0, sy = 0, sz = 0;
    bool ok = resolveSubjectPos(sIdx, sSer, sType, sCont, sContSer, &sx, &sy, &sz);
    float dx = sx - npx, dz = sz - npz;
    float d = ok ? (float)sqrt((double)(dx * dx + dz * dz)) : -1.0f;
    char b[240];
    _snprintf(b, sizeof(b) - 1,
              "[seatres] %s task=%d npc=%u,%u subj=%u,%u ok=%d npcpos=%.1f,%.1f subjpos=%.1f,%.1f d=%.1f",
              side, task, npcIdx, npcSer, sIdx, sSer, ok ? 1 : 0, npx, npz, sx, sz, d);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}

namespace {
// SEH-guarded capture of a single Character into an EntityState. Kept in its own
// __try frame (no C++ unwinding objects) so a bad pointer degrades to a skip.
bool captureOne(Character* c, EntityState* e) {
    __try {
        const hand& h = c->handle;
        e->hType            = (u32)h.type;
        e->hContainer       = h.container;
        e->hContainerSerial = h.containerSerial;
        e->hIndex           = h.index;
        e->hSerial          = h.serial;

        Ogre::Vector3 p = c->getPosition();
        e->x = p.x; e->y = p.y; e->z = p.z;
        e->heading = c->getOrientation().getYaw().valueRadians();

        CharMovement* mv = c->movement;
        if (mv) {
            e->cSpeed   = mv->currentSpeed;
            e->cMotionX = mv->currentMotion.x;
            e->cMotionY = mv->currentMotion.y;
            e->cMotionZ = mv->currentMotion.z;
            e->cMoving  = mv->currentlyMoving ? 1 : 0;
        } else {
            e->cSpeed = 0; e->cMotionX = e->cMotionY = e->cMotionZ = 0; e->cMoving = 0;
        }
        // Stage 5 pose: capture the current task + the object it targets, so the
        // receiver can adopt the same pose at the same fixture (sit/operate). No
        // current action -> TASK_NONE (the receiver idle-parks).
        e->task = TASK_NONE;
        e->rawTask = TASK_NONE;
        e->sType = e->sContainer = e->sContainerSerial = e->sIndex = e->sSerial = 0;
        if (g_taskerKeyFn) {
            CharBody* b = c->body;
            Tasker* t = b ? b->currentAction : 0;
            if (t) {
                int k = g_taskerKeyFn(t);
                e->rawTask = (u16)k; // diagnostic: stream the raw key for divergence checks
                const char* desc = 0;
                if (g_taskerDescFn) {
                    const std::string* ds = g_taskerDescFn(t);
                    if (ds) desc = ds->c_str();
                }
                logTaskKeyOnce(k, t->subject.index != 0 || t->subject.serial != 0, desc);
                // Only stream anchored rest poses; everything else stays TASK_NONE
                // so the receiver parks instead of reproducing a moving task.
                if (isReproduciblePose(k)) {
                    e->task = (u16)k;
                    const hand& s = t->subject;
                    e->sType            = (u32)s.type;
                    e->sContainer       = s.container;
                    e->sContainerSerial = s.containerSerial;
                    e->sIndex           = s.index;
                    e->sSerial          = s.serial;
                    // DIAGNOSTIC: where does THIS (host) client resolve the fixture?
                    logSeatResolveOnce("HOST", k, e->hIndex, e->hSerial,
                                       e->sIndex, e->sSerial, e->sType,
                                       e->sContainer, e->sContainerSerial,
                                       e->x, e->y, e->z);
                }
            }
        }
        // Stage 2: body-state flags (down/KO/ragdoll/dead/crawl). 0 = upright. Read
        // last so a fault here can't lose the transform we already captured.
        e->bodyState = readBodyState(c);
        // Carried-body sync (protocol 18): a body carrying someone streams the
        // synthetic TASK_CARRY_BODY with the carried hand as the subject - the
        // receiver's SELF-HEAL for a lost pickup event. Sits above rest poses
        // (clobbers a sit/work task) and below combat (the combat override just
        // after clobbers it - you drop the body to fight).
        if (c->isCarryingSomething) {
            const hand& ch = c->carryingObject;
            if (ch.index != 0 || ch.serial != 0) {
                e->task             = TASK_CARRY_BODY;
                e->sType            = (u32)ch.type;
                e->sContainer       = ch.container;
                e->sContainerSerial = ch.containerSerial;
                e->sIndex           = ch.index;
                e->sSerial          = ch.serial;
            }
        }
        // Stage 3c combat: if the body is fighting a resolvable target, OVERRIDE the
        // pose task with the synthetic combat intent and stash the target's hand in the
        // subject fields. Combat outranks any rest pose (you can't sit and fight), so
        // this clobbers a sit/work task set above. The join reproduces the cause by
        // ordering its local copy to melee the same target. (Read inside this __try so
        // a combat-read fault can't lose the transform/body-state already captured.)
        {
            CombatRead cr;
            // combatModeActive, not isInCombatMode(): the latter flickers OFF
            // between combo sections / slot rotations, and a flickering stream
            // disarm-reset the peer's copies every gap (measured in combat_crowd:
            // a fighting hand's streamed task alternated combat/none all fight).
            if (readCombat(c, &cr) && (cr.inCombat || cr.modeActive) &&
                cr.hasTarget && (cr.target[3] != 0 || cr.target[4] != 0)) {
                // Stance split (protocol 15): an attack-slot-QUEUED combatant
                // streams TASK_COMBAT_WAIT so the join holds its copy in the
                // menace ring instead of timer-re-issuing the focused attack.
                e->task = cr.waiting ? TASK_COMBAT_WAIT : TASK_COMBAT_MELEE;
                e->sType            = cr.target[0];
                e->sContainer       = cr.target[1];
                e->sContainerSerial = cr.target[2];
                e->sIndex           = cr.target[3];
                e->sSerial          = cr.target[4];
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        noteFault(FAULT_CAPTURE);
        return false;
    }
}
} // namespace

unsigned int captureSquad(GameWorld* gw, bool leaderOnly,
                          EntityState* out, unsigned int maxOut) {
    if (!gw || !out || maxOut == 0) return 0;
    unsigned int n = 0;
    __try {
        if (!gw->player) return 0;
        unsigned int size = (unsigned int)gw->player->playerCharacters.size();
        for (unsigned int i = 0; i < size && n < maxOut; ++i) {
            Character* c = gw->player->playerCharacters[i];
            if (!c) continue;
            // captureOne has its own __try; calling it here is fine.
            if (captureOne(c, &out[n])) ++n;
            if (leaderOnly) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return n;
    }
    return n;
}

// ---- Phase 5b: canonical typed hand capture/resolve -------------------------
// The ONE place that maps a typed ObjectHand onto the engine's native `hand`.
// The 5-arg ctor's arg order (index, serial, type, container, containerSerial =
// CHAR-KEY order) lives HERE and nowhere else - every resolve path funnels
// through this, so the ctor order can never drift between call sites. `buf` is
// caller-owned (>= sizeof(hand)) to keep this a leaf usable inside an SEH frame.
static hand* buildNativeHand(const ObjectHand& oh, char* buf, unsigned int bufLen) {
    memset(buf, 0, bufLen);
    hand* h = reinterpret_cast<hand*>(buf);
    g_handCtorFn(h, oh.index, oh.serial, (itemType)oh.type,
                 oh.container, oh.containerSerial);
    return h;
}

bool handOf(RootObject* obj, ObjectHand& out) {
    if (!obj) return false;
    __try {
        const hand& h = obj->handle;
        out.type            = (u32)h.type;
        out.container       = h.container;
        out.containerSerial = h.containerSerial;
        out.index           = h.index;
        out.serial          = h.serial;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        noteFault(FAULT_HAND_READ);
        return false;
    }
}

bool charHandOf(Character* c, ObjectHand& out) {
    if (!c) return false;
    __try {
        const hand& h = c->handle;
        out.type            = (u32)h.type;
        out.container       = h.container;
        out.containerSerial = h.containerSerial;
        out.index           = h.index;
        out.serial          = h.serial;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        noteFault(FAULT_HAND_READ);
        return false;
    }
}

Character* resolveChar(const ObjectHand& oh) {
    if (!g_handGetCharFn || !g_handCtorFn) return 0;
    __try {
        char buf[sizeof(hand) + 16];
        return g_handGetCharFn(buildNativeHand(oh, buf, sizeof(buf)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        noteFault(FAULT_RESOLVE_CHAR);
        return 0;
    }
}

RootObject* resolveObject(const ObjectHand& oh) {
    if (!g_handGetRootFn || !g_handCtorFn) return 0;
    __try {
        char buf[sizeof(hand) + 16];
        return g_handGetRootFn(buildNativeHand(oh, buf, sizeof(buf)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        noteFault(FAULT_RESOLVE_OBJECT);
        return 0;
    }
}

// SEH-guarded RootObject::getInventory() - the one place the getInventory()
// swallow pattern lives (Phase 5c). Its own SEH frame, so it is safe to call
// from another guarded scope (the __except just returns 0 up to the caller).
Inventory* invOf(RootObject* ro) {
    if (!ro) return 0;
    __try {
        return ro->getInventory();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        noteFault(FAULT_INV_OF);
        return 0;
    }
}

Character* resolve(const EntityState& e) {
    ObjectHand oh;
    oh.type            = e.hType;
    oh.container       = e.hContainer;
    oh.containerSerial = e.hContainerSerial;
    oh.index           = e.hIndex;
    oh.serial          = e.hSerial;
    return resolveChar(oh);
}

// Resolve a Character* from raw hand fields (same path as resolve(EntityState)).
// Adapter kept for not-yet-migrated call sites; the arg order IS char-key order.
Character* resolveCharByHand(unsigned int idx, unsigned int ser, unsigned int type,
                             unsigned int cont, unsigned int contSer) {
    ObjectHand oh;
    oh.index           = idx;
    oh.serial          = ser;
    oh.type            = type;
    oh.container       = cont;
    oh.containerSerial = contSer;
    return resolveChar(oh);
}

bool applyRaw(Character* c, const EntityState& e) {
    if (!c) return false;
    __try {
        CharMovement* mv = c->movement;
        if (!mv) return false;
        Ogre::Vector3 pos(e.x, e.y, e.z);
        Ogre::Quaternion rot(Ogre::Radian(e.heading), Ogre::Vector3::UNIT_Y);
        mv->_setPositionDirectionAndTeleport(pos, rot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readPos(Character* c, float* x, float* y, float* z) {
    if (!c) return false;
    __try {
        Ogre::Vector3 p = c->getPosition();
        if (x) *x = p.x; if (y) *y = p.y; if (z) *z = p.z;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readHand(Character* c, unsigned int out[5]) {
    // char-key adapter over the typed capture: out = {index, serial, type,
    // container, containerSerial}. charHandOf carries the SEH.
    ObjectHand oh;
    if (!charHandOf(c, oh)) return false;
    oh.toCharKey(out);
    return true;
}

bool orderMoveTo(Character* c, float x, float y, float z) {
    if (!c) return false;
    __try {
        Ogre::Vector3 dest(x, y, z);
        // Prefer the player move-order path (moves a player-controlled leader).
        if (g_charSetDestFn) {
            g_charSetDestFn(c, &dest, false);
            return true;
        }
        // Fallback: drive the movement controller directly (AI/proxy bodies).
        CharMovement* mv = c->movement;
        if (!mv) return false;
        mv->setDesiredSpeed(RUN);
        mv->setDestination(dest, HIGH_PRIORITY, false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool walkTo(Character* c, float x, float y, float z, float speed) {
    if (!c) return false;
    __try {
        Ogre::Vector3 dest(x, y, z);
        // Dual-path so ONE call drives both kinds of body:
        //   * player-controlled (squad): the player move-order path; a player char
        //     ignores a bare CharMovement::setDestination (proved in Stage 1).
        //   * AI-controlled (NPC): a CharMovement HIGH_PRIORITY destination, which
        //     overrides the NPC's autonomous movement goals (proved in the monolith).
        // Issuing both is safe: each body obeys the one that applies to it.
        if (g_charSetDestFn) g_charSetDestFn(c, &dest, false);

        CharMovement* mv = c->movement;
        if (mv) {
            mv->setDestination(dest, HIGH_PRIORITY, false);
            float s = speed;
            if (s < 1.0f) s = (float)RUN; // unknown/tiny: default to a run pace
            mv->setDesiredSpeed(s);        // override the order's default speed (catch-up)
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool park(Character* c, float x, float y, float z, float heading) {
    if (!c) return false;
    __try {
        CharMovement* mv = c->movement;
        if (!mv) return false;
        Ogre::Vector3 pos(x, y, z);
        Ogre::Quaternion rot(Ogre::Radian(heading), Ogre::Vector3::UNIT_Y);
        mv->halt(); // clean stop (resets path AND clip phase - only at settle)
        mv->_setPositionDirectionAndTeleport(pos, rot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Census-freeze support: stop an in-flight movement goal WITHOUT teleporting.
// The AI-suspend hook only blocks new periodic decisions - a destination the
// slave AI committed before the freeze keeps the body running (run 014948: a
// FROZEN slave re-pathed ~600 u between parks). Called per tick while frozen.
bool haltMovement(Character* c) {
    if (!c) return false;
    __try {
        CharMovement* mv = c->movement;
        if (!mv) return false;
        mv->halt();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool applyMotion(Character* c, bool moving, float speed, float mx, float my, float mz) {
    if (!c) return false;
    __try {
        CharMovement* mv = c->movement;
        if (!mv) return false;
        mv->currentlyMoving = moving;
        mv->currentSpeed    = speed;
        mv->desiredSpeed    = speed; // keep accel logic from re-deciding to idle
        mv->currentMotion   = Ogre::Vector3(mx, my, mz);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readMotion(Character* c, bool* moving, float* speed) {
    if (!c) return false;
    __try {
        CharMovement* mv = c->movement;
        if (!mv) return false;
        if (moving) *moving = mv->currentlyMoving;
        if (speed)  *speed  = mv->currentSpeed;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int readTaskKey(Character* c) {
    if (!c || !g_taskerKeyFn) return -1;
    __try {
        CharBody* b = c->body;
        Tasker* t = b ? b->currentAction : 0;
        if (!t) return (int)TASK_NONE;
        return g_taskerKeyFn(t);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool isNodeAnchoredPose(int taskKey) { return isNodeAnchoredPoseImpl(taskKey); }

bool recruitNpc(GameWorld* gw, Character* c) {
    if (!gw || !c || !g_recruitFn) return false;
    __try {
        if (!gw->player) return false;
        return g_recruitFn(gw->player, c, false);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
Character* leader(GameWorld* gw) {
    if (!gw) return 0;
    __try {
        if (!gw->player || gw->player->playerCharacters.size() == 0) return 0;
        return gw->player->playerCharacters[0];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// True if 'obj' is one of the local player's squad members (we never stream our
// own controllable squad as a host NPC). Caller holds the SEH frame.
// External linkage (EngineInternal.h): also used by the spawn/combat TU.
bool isPlayerSquad(GameWorld* gw, RootObject* obj) {
    PlayerInterface* pl = gw->player;
    if (!pl) return false;
    unsigned int pc = (unsigned int)pl->playerCharacters.size();
    for (unsigned int j = 0; j < pc; ++j) {
        if (static_cast<RootObject*>(pl->playerCharacters[j]) == obj) return true;
    }
    return false;
}

// Read a live NON-player Faction* off a nearby world NPC (the first non-squad
// character within the interest radius whose faction differs from the player's).
// This avoids FactionManager (no header): spawning into this faction yields a true
// world NPC that is NOT in the player squad, and owning the work fixture with the
// same faction gives that NPC a legitimate reason to operate it. Caller holds SEH.
// Returns 0 if no non-player NPC is nearby (e.g. an empty/blank save).
Faction* findNearbyNonPlayerFaction(GameWorld* gw) {
    if (!gw || !g_getCharsFn || !gw->player) return 0;
    if (gw->player->playerCharacters.size() == 0) return 0;
    Faction* playerFac = gw->player->getFaction();
    Ogre::Vector3 center = gw->player->playerCharacters[0]->getPosition();
    // Wide radius: we only need ANY loaded world NPC to read a faction pointer off,
    // not a close one. A blank-start save can sit just outside a town, so the bar
    // crowd is well beyond the 200u capture radius - reach the whole loaded block.
    g_npcQuery.clear();
    g_getCharsFn(gw, &g_npcQuery, &center, 6000.0f, 6000.0f, 6000.0f, 512, 512, 0);
    unsigned int total = g_npcQuery.size();
    {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SETUP: faction scan found %u loaded NPC(s) within 6000u", total);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    for (unsigned int i = 0; i < total; ++i) {
        RootObject* obj = g_npcQuery[i];
        if (!obj || isPlayerSquad(gw, obj)) continue;
        Faction* f = static_cast<Character*>(obj)->getFaction();
        if (f && f != playerFac) return f;
    }
    return 0;
}

// Camera-anchored interest (spike 35): read the LOCAL camera's world center.
// gw->player->camera is the live per-client CameraClass; getCenter() returns
// Ogre::Vector3 by value (hidden-return-pointer ABI, resolved by RVA in
// engine::resolve). isInitialised() rejects the pre-load camera. Purely local
// - never touches the wire here (the join's hint crosses via PKT_CAM_HINT).
// Throttled [cam] debug line (~5s) so manual logs show the anchor moving.
bool cameraCenter(GameWorld* gw, float out[3]) {
    if (!gw || !out || !g_camGetCenterFn || !g_camIsInitFn) return false;
    static unsigned long lastLog = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    __try {
        PlayerInterface* pl = gw->player;
        if (!pl) return false;
        CameraClass* cam = pl->camera;
        if (!cam || !g_camIsInitFn(cam)) return false;
        Ogre::Vector3 c;
        g_camGetCenterFn(cam, &c);
        x = c.x; y = c.y; z = c.z;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    out[0] = x; out[1] = y; out[2] = z;
    unsigned long now = GetTickCount();
    if (now - lastLog >= 5000) {
        lastLog = now;
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[cam] center=%.1f,%.1f,%.1f", x, y, z);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
    }
    return true;
}

// Camera anchor stores (protocol 43, camera-anchored interest). Main-thread
// only: the sync layer publishes these each tick (syncCamHint), and
// interestCenters reads them in the same tick.
static bool  s_camInterest    = true;  // KENSHICOOP_CAM_INTEREST master enable
static bool  s_localCamValid  = false;
static float s_localCam[3]    = { 0.0f, 0.0f, 0.0f };
static bool  s_peerCamValid   = false;
static float s_peerCam[3]     = { 0.0f, 0.0f, 0.0f };

void setCamInterest(bool on) { s_camInterest = on; }

void setLocalCamAnchor(bool valid, float x, float y, float z) {
    s_localCamValid = valid;
    if (valid) { s_localCam[0] = x; s_localCam[1] = y; s_localCam[2] = z; }
}

void setPeerCamHint(bool valid, float x, float y, float z) {
    s_peerCamValid = valid;
    if (valid) { s_peerCam[0] = x; s_peerCam[1] = y; s_peerCam[2] = z; }
}

// DUAL-INTEREST centers (step 5): one interest sphere per squad TAB leader, up
// to two. Both clients load the same save, so the shared playerCharacters list
// (and its tab/container partition) is identical on each machine - the first
// member of each distinct hand-container IS the other player's leader as seen
// locally. A single host-leader-centered sphere meant the shared world degraded
// the moment the players split up (spike 16); with one sphere per tab leader,
// NPCs around EACH player stay streamed (spike 19's validated design).
//
// Protocol 43 grows the set to up to FOUR anchors: the two tab-leader spheres
// plus the LOCAL camera center and the peer's fresh CAMERA HINT - so NPCs
// where a player is LOOKING (but no PC is standing) stay streamed/listed.
// Camera anchors within ~100u of an existing anchor are dropped (the common
// camera-follows-leader case adds no reach, only query cost). Writes up to
// four centers; returns the count. Caller holds the SEH frame.
unsigned int interestCenters(GameWorld* gw, Ogre::Vector3 outC[4]) {
    PlayerInterface* pl = gw->player;
    if (!pl || pl->playerCharacters.size() == 0) return 0;
    unsigned int pairs[2][2];
    unsigned int nc = 0;
    unsigned int total = pl->playerCharacters.size();
    for (unsigned int i = 0; i < total && nc < 2; ++i) {
        Character* m = pl->playerCharacters[i];
        if (!m) continue;
        unsigned int h[5];
        if (!readObjectHand(static_cast<RootObject*>(m), h)) continue;
        bool seen = false;
        for (unsigned int k = 0; k < nc; ++k)
            if (pairs[k][0] == h[1] && pairs[k][1] == h[2]) { seen = true; break; }
        if (seen) continue;
        pairs[nc][0] = h[1]; pairs[nc][1] = h[2];
        outC[nc] = m->getPosition();
        ++nc;
    }
    if (nc == 0 || !s_camInterest) return nc;
    // Fold in the camera anchors (local first, then the peer hint), deduped
    // against everything already in the set. nc==0 stays 0: no players in
    // gameplay means no streaming at all (the camera alone must not stream).
    const float DEDUPE_DIST_SQ = 100.0f * 100.0f;
    const float* cams[2] = { s_localCamValid ? s_localCam : 0,
                             s_peerCamValid  ? s_peerCam  : 0 };
    for (unsigned int ci = 0; ci < 2 && nc < 4; ++ci) {
        if (!cams[ci]) continue;
        bool dup = false;
        for (unsigned int k = 0; k < nc && !dup; ++k) {
            float dx = outC[k].x - cams[ci][0];
            float dy = outC[k].y - cams[ci][1];
            float dz = outC[k].z - cams[ci][2];
            dup = (dx * dx + dy * dy + dz * dz) <= DEDUPE_DIST_SQ;
        }
        if (dup) continue;
        outC[nc] = Ogre::Vector3(cams[ci][0], cams[ci][1], cams[ci][2]);
        ++nc;
    }
    return nc;
}

// SEH wrapper over interestCenters for callers outside the engine layer
// (interestCenters itself relies on the caller's SEH frame).
unsigned int interestAnchors(GameWorld* gw, float out[12]) {
    if (!gw || !out) return 0;
    unsigned int nc = 0;
    __try {
        Ogre::Vector3 centers[4];
        nc = interestCenters(gw, centers);
        for (unsigned int i = 0; i < nc; ++i) {
            out[i * 3 + 0] = centers[i].x;
            out[i * 3 + 1] = centers[i].y;
            out[i * 3 + 2] = centers[i].z;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return nc;
}

unsigned int captureNpcs(GameWorld* gw, EntityState* out, unsigned int maxOut) {
    if (!g_getCharsFn || !gw || !out || maxOut == 0) return 0;
    unsigned int n = 0;
    // Capture-bubble hysteresis (2026-07-11 choppiness fix): a hard 200 u
    // edge flapped boundary NPCs in/out of the streamed set as the two
    // clients disagreed slightly on their positions, starving the join's
    // interp buffer right when the NPC was visibly walking away. ACQUIRE at
    // 200 u, RETAIN out to 260 u for NPCs captured on the previous call, so
    // a body must genuinely leave interest (not jitter across the line) to
    // stop streaming. Previous-set keyed by (hIndex, hSerial); static
    // arrays, main-thread only (no C++ unwinding inside __try).
    const float NPC_CAPTURE_ACQUIRE = 200.0f;
    const float NPC_CAPTURE_KEEP    = 260.0f;
    static unsigned int prevKeys[512][2];
    static unsigned int prevN = 0;
    static unsigned int newKeys[512][2];
    unsigned int newN = 0;
    __try {
        // Interest: one sphere per squad-tab leader (dual-interest, step 5). The
        // query radii approximate a town-block footprint (~200u far) per sphere.
        Ogre::Vector3 centers[4];
        unsigned int nc = interestCenters(gw, centers);
        if (nc == 0) { prevN = 0; return 0; }

        // Dedupe across the (possibly overlapping) spheres by object pointer.
        static RootObject* appended[512]; // main-thread only
        unsigned int nApp = 0;
        for (unsigned int ci = 0; ci < nc; ++ci) {
            g_npcQuery.clear();
            g_getCharsFn(gw, &g_npcQuery, &centers[ci], NPC_CAPTURE_KEEP,
                         120.0f, 30.0f, 96, 96, 0);
            unsigned int total = g_npcQuery.size();
            for (unsigned int i = 0; i < total && n < maxOut; ++i) {
                RootObject* obj = g_npcQuery[i];
                if (!obj) continue;
                if (isPlayerSquad(gw, obj)) continue; // never stream our own squad here
                bool dup = false;
                for (unsigned int k = 0; k < nApp; ++k)
                    if (appended[k] == obj) { dup = true; break; }
                if (dup) continue;
                // getCharactersWithinSphere returns Characters as RootObject* bases.
                if (captureOne(static_cast<Character*>(obj), &out[n])) {
                    float dx = out[n].x - centers[ci].x;
                    float dy = out[n].y - centers[ci].y;
                    float dz = out[n].z - centers[ci].z;
                    float distSq = dx * dx + dy * dy + dz * dz;
                    if (distSq > NPC_CAPTURE_ACQUIRE * NPC_CAPTURE_ACQUIRE) {
                        // Retention band: only NPCs already streaming stay in.
                        // Skipping without appending lets an overlapping
                        // second sphere still acquire it inside ITS 200 u.
                        bool held = false;
                        for (unsigned int k = 0; k < prevN && !held; ++k)
                            held = prevKeys[k][0] == out[n].hIndex &&
                                   prevKeys[k][1] == out[n].hSerial;
                        if (!held) continue;
                    }
                    if (nApp < 512) appended[nApp++] = obj;
                    if (newN < 512) {
                        newKeys[newN][0] = out[n].hIndex;
                        newKeys[newN][1] = out[n].hSerial;
                        ++newN;
                    }
                    ++n;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // fall through: publish whatever was captured before the fault
    }
    prevN = newN;
    for (unsigned int k = 0; k < newN; ++k) {
        prevKeys[k][0] = newKeys[k][0];
        prevKeys[k][1] = newKeys[k][1];
    }
    return n;
}

void clearGoals(Character* c) {
    if (!c || !g_clearGoalsFn) return;
    __try {
        g_clearGoalsFn(c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool isLocalPlayerChar(GameWorld* gw, Character* c) {
    if (!gw || !c) return false;
    __try {
        return isPlayerSquad(gw, static_cast<RootObject*>(c));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool suppressNpc(GameWorld* gw, Character* c) {
    if (!gw || !c || !g_removeUpdateFn) return false;
    __try {
        g_removeUpdateFn(gw, c);
        if (g_clearGoalsFn) g_clearGoalsFn(c);
        // Freezing alone leaves the body standing/visible at its seat; hide it too
        // so a host-unstreamed NPC fully disappears (no standing-on-the-seat double).
        static_cast<RootObject*>(c)->setVisible(false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void restoreNpc(GameWorld* gw, Character* c) {
    if (!gw || !c || !g_addUpdateFn) return;
    __try {
        g_addUpdateFn(gw, c);
        static_cast<RootObject*>(c)->setVisible(true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool captureNpcByHand(GameWorld* gw, unsigned int hIndex, unsigned int hSerial,
                      unsigned int hType, unsigned int hContainer,
                      unsigned int hContainerSerial, EntityState* out) {
    if (!gw || !out) return false;
    Character* c = resolveCharByHand(hIndex, hSerial, hType, hContainer,
                                     hContainerSerial);
    if (!c) return false;
    __try {
        if (isPlayerSquad(gw, static_cast<RootObject*>(c))) return false;
        return captureOne(c, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Stage 6 host authority: enumerate nearby WORLD NPCs (excluding our own squad),
// yielding both the live Character* and its hand-bearing EntityState so the join
// can decide which are NOT in the host's streamed set and suppress them. Mirrors
// captureNpcs' interest query so the join's local set matches the host's.
unsigned int listNpcs(GameWorld* gw, Character** outChars, EntityState* outStates,
                      unsigned int maxOut) {
    if (!g_getCharsFn || !gw || !outChars || !outStates || maxOut == 0) return 0;
    unsigned int n = 0;
    __try {
        // Same dual-interest spheres as captureNpcs, so the join's suppression
        // view matches what the host is willing to stream.
        Ogre::Vector3 centers[4];
        unsigned int nc = interestCenters(gw, centers);
        if (nc == 0) return 0;
        for (unsigned int ci = 0; ci < nc; ++ci) {
            g_npcQuery.clear();
            g_getCharsFn(gw, &g_npcQuery, &centers[ci], 200.0f, 120.0f, 30.0f, 96, 96, 0);
            unsigned int total = g_npcQuery.size();
            for (unsigned int i = 0; i < total && n < maxOut; ++i) {
                RootObject* obj = g_npcQuery[i];
                if (!obj) continue;
                if (isPlayerSquad(gw, obj)) continue; // never suppress our own squad
                Character* ch = static_cast<Character*>(obj);
                bool dup = false;
                for (unsigned int k = 0; k < n; ++k)
                    if (outChars[k] == ch) { dup = true; break; }
                if (dup) continue;
                if (captureOne(ch, &outStates[n])) { outChars[n] = ch; ++n; }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return n;
    }
    return n;
}

unsigned int listNpcsWide(GameWorld* gw, float radius, Character** outChars,
                          EntityState* outStates, unsigned int maxOut) {
    if (!g_getCharsFn || !gw || !outChars || !outStates || maxOut == 0) return 0;
    if (radius <= 0.0f) return 0;
    unsigned int n = 0;
    __try {
        // Same dual-interest centers as the stream bubble, but the query
        // reaches the census radius (uniform in all axes, like the proven
        // findNearbyNonPlayerFaction whole-block scan) with wide limits.
        Ogre::Vector3 centers[4];
        unsigned int nc = interestCenters(gw, centers);
        if (nc == 0) return 0;
        for (unsigned int ci = 0; ci < nc; ++ci) {
            g_npcQuery.clear();
            g_getCharsFn(gw, &g_npcQuery, &centers[ci], radius, radius, radius,
                         512, 512, 0);
            unsigned int total = g_npcQuery.size();
            for (unsigned int i = 0; i < total && n < maxOut; ++i) {
                RootObject* obj = g_npcQuery[i];
                if (!obj) continue;
                if (isPlayerSquad(gw, obj)) continue; // never census our own squad
                Character* ch = static_cast<Character*>(obj);
                bool dup = false;
                for (unsigned int k = 0; k < n; ++k)
                    if (outChars[k] == ch) { dup = true; break; }
                if (dup) continue;
                if (captureOne(ch, &outStates[n])) { outChars[n] = ch; ++n; }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return n;
    }
    return n;
}

namespace {
// getName() returns std::string by value (needs C++ unwinding), so the copy
// lives in a callee; the SEH guard in charName only has POD locals (C2712).
void charNameCopy(Character* c, char* out, unsigned int cap) {
    std::string nm = static_cast<RootObjectBase*>(c)->getName();
    strncpy(out, nm.c_str(), cap - 1);
    out[cap - 1] = '\0';
}
} // namespace

void charName(Character* c, char* out, unsigned int cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!c) return;
    __try {
        charNameCopy(c, out, cap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
    }
}

// The debug marker HUD labels (markerColour/markerCreateSeh/markerUpdateSeh/
// markerDestroySeh + the public markerCreate/markerUpdate/markerDestroy) moved to
// EngineUi.cpp (Phase 5e code motion) alongside the co-op panel + status overlay
// that reuse the same ScreenLabel SEH shims. Their public declarations stay in
// Engine.h (the Replicator uses them for KENSHICOOP_DEBUG_MARKERS).

// The helpers from here to readObjectHand have EXTERNAL linkage (declared in
// EngineInternal.h): the inventory, spawn/combat and world TUs share them.

// Case-insensitive substring test on raw C strings (no C++ temporaries -> SEH
// legal). Returns true if 'needle' appears anywhere in 'hay'.
bool ciContains(const char* hay, const char* needle) {
    if (!hay || !needle || !needle[0]) return false;
    for (const char* h = hay; *h; ++h) {
        const char* a = h; const char* b = needle;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) break;
            ++a; ++b;
        }
        if (!*b) return true;
    }
    return false;
}

// Find a furniture BUILDING template that is actually a SEAT. Caller holds SEH.
// Priority avoids matching crafting stations ("Engineering Bench" etc.): we want
// stools/chairs/thrones, NOT generic "bench"/"seat" substrings.
GameData* findSeatTemplate(GameWorld* gw) {
    if (!gw || !g_getDataOfTypeFn) return 0;
    g_dataScratch.clear();
    g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, BUILDING);
    unsigned int n = g_dataScratch.size();
    // Ordered keyword preference; first present keyword that any template matches.
    const char* prefs[] = { "bar stool", "stool", "chair", "throne" };
    for (unsigned int k = 0; k < 4; ++k) {
        for (unsigned int i = 0; i < n; ++i) {
            GameData* gd = g_dataScratch[i];
            if (gd && ciContains(gd->name.c_str(), prefs[k])) return gd;
        }
    }
    return 0;
}

// Find a BUILDING template that is a BED (bed/cage occupancy sync). Caller holds
// SEH. "camp bed" is the buildable outdoor bed (no walls/power needed); plain
// "bed" is the fallback (may match indoor variants - still a UseableStuff bed).
GameData* findBedTemplate(GameWorld* gw) {
    if (!gw || !g_getDataOfTypeFn) return 0;
    g_dataScratch.clear();
    g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, BUILDING);
    unsigned int n = g_dataScratch.size();
    const char* prefs[] = { "camp bed", "bedroll", "bed" };
    for (unsigned int k = 0; k < 3; ++k) {
        for (unsigned int i = 0; i < n; ++i) {
            GameData* gd = g_dataScratch[i];
            if (gd && ciContains(gd->name.c_str(), prefs[k])) return gd;
        }
    }
    return 0;
}

// Find a BUILDING template that is a PRISON CAGE. Caller holds SEH.
GameData* findCageTemplate(GameWorld* gw) {
    if (!gw || !g_getDataOfTypeFn) return 0;
    g_dataScratch.clear();
    g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, BUILDING);
    unsigned int n = g_dataScratch.size();
    const char* prefs[] = { "prisoner cage", "cage" };
    for (unsigned int k = 0; k < 2; ++k) {
        for (unsigned int i = 0; i < n; ++i) {
            GameData* gd = g_dataScratch[i];
            if (gd && ciContains(gd->name.c_str(), prefs[k])) return gd;
        }
    }
    return 0;
}

// Find a BUILDING template that is a PRISONER POLE (the standing shackle post a
// captive is tied to). A pole is still IN_PRISON containment (setPrisonMode), a
// DIFFERENT model from the boxed cage - the pole_put fixture wants that model so
// the controlled test visibly shows a body ON A POLE, not in a cage. Caller
// holds SEH. Ordered keyword preference; the bare "pole"/"beam" fallbacks are
// last so a named "prisoner pole" always wins first. Excludes the cage box so a
// world with both a cage and a pole picks the pole. Logs every prison-ish
// candidate so a bake run reveals the exact base-game names.
GameData* findPoleTemplate(GameWorld* gw) {
    if (!gw || !g_getDataOfTypeFn) return 0;
    g_dataScratch.clear();
    g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, BUILDING);
    unsigned int n = g_dataScratch.size();
    // Diagnostic: dump prison/cage/pole-ish template names (bake-time discovery).
    for (unsigned int i = 0; i < n; ++i) {
        GameData* gd = g_dataScratch[i];
        if (!gd) continue;
        const char* nm = gd->name.c_str();
        if (ciContains(nm, "pole") || ciContains(nm, "cage") ||
            ciContains(nm, "prison") || ciContains(nm, "shackle") ||
            ciContains(nm, "beam")) {
            char d[160];
            _snprintf(d, sizeof(d) - 1, "SETUP(pole): candidate building '%s'", nm);
            d[sizeof(d) - 1] = '\0';
            coop::logLine(d);
        }
    }
    // A pole/shackle keyword (never a bare "cage") is what marks the standing
    // post; "cage pole" still qualifies because it contains "pole".
    const char* prefs[] = { "prisoner pole", "cage pole", "shackle", "pole" };
    const unsigned int nprefs = sizeof(prefs) / sizeof(prefs[0]);
    for (unsigned int k = 0; k < nprefs; ++k) {
        for (unsigned int i = 0; i < n; ++i) {
            GameData* gd = g_dataScratch[i];
            if (gd && ciContains(gd->name.c_str(), prefs[k])) return gd;
        }
    }
    return 0;
}

// Find a furniture BUILDING template that is an OPERABLE work fixture an NPC can
// stand at and work (crafting/gathering class). Caller holds SEH. Ordered keyword
// preference: a training dummy is the most deterministic (no inputs/power/recipe -
// the user can just order "train" and the work pose plays), then common crafting
// machines. Mining/farming need terrain resources, so they're not spawned here.
GameData* findMachineTemplate(GameWorld* gw) {
    if (!gw || !g_getDataOfTypeFn) return 0;
    g_dataScratch.clear();
    g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, BUILDING);
    unsigned int n = g_dataScratch.size();
    const char* prefs[] = {
        "training dummy", "combat dummy", "punching bag", "research bench",
        "engineering bench", "weapon smithy", "spinning wheel", "loom"
    };
    const unsigned int nprefs = sizeof(prefs) / sizeof(prefs[0]);
    for (unsigned int k = 0; k < nprefs; ++k) {
        for (unsigned int i = 0; i < n; ++i) {
            GameData* gd = g_dataScratch[i];
            if (gd && ciContains(gd->name.c_str(), prefs[k])) return gd;
        }
    }
    return 0;
}

// Compute a world point 'fwd' metres ahead of the leader's facing and 'side' to
// its right, plus the leader's yaw. Caller holds SEH.
bool leaderAnchor(GameWorld* gw, float fwd, float side,
                  Ogre::Vector3* outPos, float* outYaw) {
    if (!gw || !gw->player || gw->player->playerCharacters.size() == 0) return false;
    Character* ld = gw->player->playerCharacters[0];
    if (!ld) return false;
    Ogre::Vector3 p = ld->getPosition();
    float yaw = ld->getOrientation().getYaw().valueRadians();
    // Kenshi faces -Z at yaw 0; forward = (sin yaw, 0, cos yaw) is a good-enough
    // "ahead of the character" for placing a prop the user then fine-tunes.
    float fx = (float)sin((double)yaw), fz = (float)cos((double)yaw);
    float rx = fz, rz = -fx; // right = forward rotated -90deg about Y
    outPos->x = p.x + fx * fwd + rx * side;
    outPos->y = p.y; // character ground Y; building placement re-grounds via terrain
    outPos->z = p.z + fz * fwd + rz * side;
    if (outYaw) *outYaw = yaw;
    return true;
}

// Read a character's CURRENT task key (TASK_NONE if idle / unreadable). Mirrors the
// capture path; used by re-arm to avoid re-issuing a goal a worker is already doing
// (clearAllAIGoals + addGoal every tick would thrash pathing and never animate).
int readCharTaskKey(Character* c) {
    if (!c || !g_taskerKeyFn) return TASK_NONE;
    __try {
        CharBody* b = c->body;
        Tasker* t = b ? b->currentAction : 0;
        if (!t) return TASK_NONE;
        return g_taskerKeyFn(t);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return TASK_NONE;
    }
}

// Find a BAKED work fixture (training dummy / crafting machine) near the leader by
// scanning loaded BUILDING objects (NOT templates). Returns the live fixture and the
// task to issue at it. This is how craft re-arm relocates the dummy after a reload
// without any sidecar - the save-stable building is simply searched for by name.
RootObject* findWorkFixtureNear(GameWorld* gw, int* outTask) {
    if (!gw || !g_getObjsFn || !gw->player) return 0;
    if (gw->player->playerCharacters.size() == 0) return 0;
    __try {
        Ogre::Vector3 center = gw->player->playerCharacters[0]->getPosition();
        g_npcQuery.clear();
        g_getObjsFn(gw, &g_npcQuery, &center, 60.0f, BUILDING, 256, 0);
        unsigned int total = g_npcQuery.size();
        const char* prefs[] = {
            "training dummy", "combat dummy", "punching bag", "research bench",
            "engineering bench", "weapon smithy", "spinning wheel", "loom"
        };
        const unsigned int nprefs = sizeof(prefs) / sizeof(prefs[0]);
        for (unsigned int k = 0; k < nprefs; ++k) {
            for (unsigned int i = 0; i < total; ++i) {
                RootObject* o = g_npcQuery[i];
                if (!o) continue;
                GameData* gd = o->getGameData();
                if (gd && ciContains(gd->name.c_str(), prefs[k])) {
                    if (outTask)
                        *outTask = (ciContains(gd->name.c_str(), "dummy") ||
                                    ciContains(gd->name.c_str(), "bag"))
                                       ? USE_TRAINING_DUMMY : OPERATE_MACHINERY;
                    return o;
                }
            }
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Nearest NON-squad character to a fixture (the worker that should operate it).
Character* findWorkerNear(GameWorld* gw, RootObject* fixture) {
    if (!gw || !g_getCharsFn || !fixture || !gw->player) return 0;
    __try {
        Ogre::Vector3 at = fixture->getPosition();
        g_npcQuery.clear();
        g_getCharsFn(gw, &g_npcQuery, &at, 40.0f, 30.0f, 10.0f, 64, 64, 0);
        unsigned int total = g_npcQuery.size();
        Character* best = 0; float bestD2 = 1e18f;
        for (unsigned int i = 0; i < total; ++i) {
            RootObject* o = g_npcQuery[i];
            if (!o || isPlayerSquad(gw, o)) continue;
            Ogre::Vector3 p = o->getPosition();
            float dx = p.x - at.x, dz = p.z - at.z;
            float d2 = dx * dx + dz * dz;
            if (d2 < bestD2) { bestD2 = d2; best = static_cast<Character*>(o); }
        }
        return best;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool readObjectHand(RootObject* obj, unsigned int out[5]) {
    // object-order adapter over the typed capture: out = {type, container,
    // containerSerial, index, serial}. handOf carries the SEH.
    ObjectHand oh;
    if (!handOf(obj, oh)) return false;
    oh.toObjOrder(out);
    return true;
}

// ---- In-game co-op session panel + status overlay --------------------------
// Moved to EngineUi.cpp (Phase 5e code motion): the DatapanelGUI panel (clipboard
// helpers, CoopPanelUi state, button callbacks, panelBuildSeh/uiPanelArmSeh/
// panelDestroySeh, coopPanelTick) and the persistent status overlay
// (coopOverlayTick). Their public declarations live in EngineUi.h.

} // namespace engine
} // namespace coop
