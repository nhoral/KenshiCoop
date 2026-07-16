// EngineEntity.cpp - entity capture / resolve / apply (monolith split from
// EngineInternal.cpp, 2026-07-12): EntityState capture (captureOne/captureSquad/
// captureNpcs + interest centers), hand resolve (resolve/resolveCharByHand),
// motion apply (applyRaw/orderMoveTo/walkTo/park), NPC suppression, the debug
// marker HUD labels, and the seat/bed/cage/machine template + work-fixture
// finders shared by scenario scenes.
//
// Owner state: section-private statics/anon-namespace helpers only (pose
// classifiers, marker SEH shims, capture hysteresis buffers).
// Must NOT: define g_* engine pointers (EngineInternal.cpp owns them - EngineInternal.h
// declares them), install hooks, or change any log string - log phrasing is
// the API consumed by the PowerShell oracles (see resources/CODE_MAP.md).

#include "EngineInternal.h"

// In-game co-op session panel (reconstructed 2026-07-14): the native
// DatapanelGUI window + its interactive rows and Win32 key capture live here.
// EngineInternal.h already pulls Globals.h (::gui), ForgottenGUI.h and
// MyGUI_Button.h; these add the panel row types, the free-function delegate
// factory, and GetAsyncKeyState/VK_* for the F2 toggle + digit entry.
#include <kenshi/gui/DatapanelGUI.h>
#include <kenshi/gui/DataPanelLine.h>
#include <mygui/MyGUI_Delegate.h> // MyGUI::newDelegate + CDelegate* (free-fn callbacks)
#include <windows.h>

#include "../core/SteamId.h" // parseSteamId64 (pure) for the paste-from-clipboard button

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

Character* resolve(const EntityState& e) {
    if (!g_handGetCharFn || !g_handCtorFn) return 0;
    __try {
        char buf[sizeof(hand) + 16];
        memset(buf, 0, sizeof(buf));
        hand* h = reinterpret_cast<hand*>(buf);
        g_handCtorFn(h, e.hIndex, e.hSerial, (itemType)e.hType,
                     e.hContainer, e.hContainerSerial);
        return g_handGetCharFn(h);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Resolve a Character* from raw hand fields (same path as resolve(EntityState)).
Character* resolveCharByHand(unsigned int idx, unsigned int ser, unsigned int type,
                             unsigned int cont, unsigned int contSer) {
    if (!g_handGetCharFn || !g_handCtorFn) return 0;
    __try {
        char buf[sizeof(hand) + 16];
        memset(buf, 0, sizeof(buf));
        hand* h = reinterpret_cast<hand*>(buf);
        g_handCtorFn(h, idx, ser, (itemType)type, cont, contSer);
        return g_handGetCharFn(h);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
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
    if (!c) return false;
    __try {
        const hand& h = c->handle;
        out[0] = h.index;
        out[1] = h.serial;
        out[2] = (unsigned int)h.type;
        out[3] = h.container;
        out[4] = h.containerSerial;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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

// DUAL-INTEREST centers (step 5): one interest sphere per squad TAB leader, up
// to two. Both clients load the same save, so the shared playerCharacters list
// (and its tab/container partition) is identical on each machine - the first
// member of each distinct hand-container IS the other player's leader as seen
// locally. A single host-leader-centered sphere meant the shared world degraded
// the moment the players split up (spike 16); with one sphere per tab leader,
// NPCs around EACH player stay streamed (spike 19's validated design). Writes up
// to two centers; returns the count. Caller holds the SEH frame.
unsigned int interestCenters(GameWorld* gw, Ogre::Vector3 outC[2]) {
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
        Ogre::Vector3 centers[2];
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
        Ogre::Vector3 centers[2];
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
        Ogre::Vector3 centers[2];
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

// ---- Debug marker HUD labels (KENSHICOOP_DEBUG_MARKERS, spike-47 substrate) --
// ForgottenGUI::createScreenLabel + ScreenLabel::setTracking pin a colored text
// label to a character; the engine's own per-frame projection keeps it on the
// body (spike 47 render proof). The Replicator uses these to make join-side
// authority states self-explaining on screen: who is host-driven, who is
// hidden, who is a local-only ghost. C2712 split: the outer fns build the
// std::string/Colour/Vector3 (unwindable), POD-only inner fns hold the SEH.

namespace {

void markerColour(int colorId, MyGUI::Colour* col) {
    switch (colorId) {
    case 0:  *col = MyGUI::Colour(0.30f, 1.00f, 0.30f, 1.0f); break; // driven
    case 1:  *col = MyGUI::Colour(1.00f, 0.25f, 0.25f, 1.0f); break; // hidden
    case 2:  *col = MyGUI::Colour(1.00f, 0.90f, 0.25f, 1.0f); break; // local-only
    default: *col = MyGUI::Colour(0.80f, 0.80f, 0.80f, 1.0f); break;
    }
}

ScreenLabel* markerCreateSeh(ForgottenGUI* g, Character* c,
                             const std::string* text, const MyGUI::Colour* col,
                             const Ogre::Vector3* off) {
    __try {
        ScreenLabel* l = g->createScreenLabel(*text, *col, ScreenLabel::LS_SMALL,
                                              ScreenLabel::RS_STOPPED);
        if (l) {
            l->_NV_setRisingSpeed(ScreenLabel::RS_STOPPED);
            l->_NV_setTracking(c->handle, *off);
        }
        return l;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool markerUpdateSeh(ScreenLabel* l, const std::string* text,
                     const MyGUI::Colour* col) {
    __try {
        l->_NV_setCaption(*text);
        l->_NV_setColor(*col);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool markerDestroySeh(ForgottenGUI* g, ScreenLabel* l) {
    __try {
        g->destroy(l);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

} // namespace

void* markerCreate(Character* c, const char* text, int colorId) {
    if (!c || !text) return 0;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    if (!g) return 0;
    std::string t(text);
    MyGUI::Colour col;
    markerColour(colorId, &col);
    Ogre::Vector3 off(0.0f, 2.2f, 0.0f); // head height (spike 47)
    return markerCreateSeh(g, c, &t, &col, &off);
}

bool markerUpdate(void* label, const char* text, int colorId) {
    if (!label || !text) return false;
    std::string t(text);
    MyGUI::Colour col;
    markerColour(colorId, &col);
    return markerUpdateSeh((ScreenLabel*)label, &t, &col);
}

void markerDestroy(void* label) {
    if (!label) return;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    if (!g) return;
    markerDestroySeh(g, (ScreenLabel*)label);
}

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
    if (!obj) return false;
    __try {
        const hand& h = obj->handle;
        out[0] = (unsigned int)h.type;
        out[1] = h.container;
        out[2] = h.containerSerial;
        out[3] = h.index;
        out[4] = h.serial;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- In-game co-op session panel (config-driven, spike-50 DatapanelGUI stack) -
// A native DatapanelGUI window toggled with F2. The player picks role + transport
// (toggle BUTTONS - the only DatapanelGUI control with a callable RVA callback;
// MyGUI comboboxes/editboxes have no reachable getters and never receive keyboard
// focus during gameplay) and connects/leaves via a bound checkbox. The friend code
// (peer SteamID) + UDP endpoint come from coop_config.json and are shown READ-ONLY;
// a "Copy my Steam ID" button puts the player's own id on the clipboard to share.
// The GUI layer is session-agnostic: live status arrives via *st; the user's
// actions leave via the onConnect/onDisconnect callbacks (the plugin root owns the
// net/session/config wiring).
//
// SEH discipline (spike 47/48): the mutation calls take std::string by const-ref
// or PODs, so they all sit inside one __try, provided NO std::string temporary is
// constructed in that frame. The one exception is createDatapanel's BY-VALUE
// std::string 'layer' arg (an unwindable temporary => C2712), so the window is
// created in the outer, non-SEH function; ::gui is verified non-null first and the
// createScreenLabel/createFloatingLabel factory family is render-proven (46-48).

namespace {

// Write a UTF-8/ANSI string to the Windows clipboard (CF_TEXT). Mirror of the
// paste-read: OpenClipboard -> EmptyClipboard -> GlobalAlloc+copy -> SetClipboardData
// -> CloseClipboard. Used by the "Copy my Steam ID" button. Win32 only (no MyGUI).
bool clipboardSetText(const char* text) {
    if (!text) return false;
    size_t n = strlen(text);
    if (!OpenClipboard(0)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n + 1);
        if (h) {
            char* dst = (char*)GlobalLock(h);
            if (dst) {
                memcpy(dst, text, n);
                dst[n] = '\0';
                GlobalUnlock(h);
                if (SetClipboardData(CF_TEXT, h)) ok = true; // clipboard now owns h
            }
            if (!ok) GlobalFree(h); // ownership not transferred on failure
        }
    }
    CloseClipboard();
    return ok;
}

// Read text from the Windows clipboard into out. Prefers CF_UNICODETEXT (what the
// Steam overlay / browsers usually publish) and falls back to CF_TEXT, converting
// either to a narrow std::string (the SteamID parse keeps only ASCII digits, so a
// lossy WideCharToMultiByte is fine here). Used by the "Paste friend's Steam ID"
// button. Win32 only (no MyGUI). Returns true iff some text was retrieved.
bool clipboardGetText(std::string& out) {
    if (!OpenClipboard(0)) return false;
    bool ok = false;
    HANDLE hw = GetClipboardData(CF_UNICODETEXT);
    if (hw) {
        const wchar_t* src = (const wchar_t*)GlobalLock(hw);
        if (src) {
            int need = WideCharToMultiByte(CP_UTF8, 0, src, -1, 0, 0, 0, 0);
            if (need > 0) {
                std::string tmp((size_t)need, '\0');
                if (WideCharToMultiByte(CP_UTF8, 0, src, -1, &tmp[0], need, 0, 0) > 0) {
                    if (!tmp.empty() && tmp[tmp.size() - 1] == '\0') tmp.resize(tmp.size() - 1);
                    out = tmp;
                    ok = true;
                }
            }
            GlobalUnlock(hw);
        }
    }
    if (!ok) {
        HANDLE ha = GetClipboardData(CF_TEXT);
        if (ha) {
            const char* src = (const char*)GlobalLock(ha);
            if (src) { out = src; ok = true; GlobalUnlock(ha); }
        }
    }
    CloseClipboard();
    return ok;
}

struct CoopPanelUi {
    DatapanelGUI* panel;
    bool          open, built;
    bool          hostFlag;      // true = HOST role armed
    bool          steamFlag;     // true = Steam transport armed (else UDP)
    bool          connectedFlag; // desired connection state (Online/Offline toggle)
    bool          lastConnected; // last observed st->running (external-change sync)
    bool          lastChkVal;    // last toggle value (connect/disconnect edge)
    bool          needsRebuild;
    bool          f2Down;        // F2 held last tick (rising-edge toggle)
    std::string   lastStatus;    // last status text shown (refresh gate)
    CoopPanelUi()
        : panel(0), open(false), built(false), hostFlag(true), steamFlag(true),
          connectedFlag(false), lastConnected(false), lastChkVal(false),
          needsRebuild(false), f2Down(false) {}
};

CoopPanelUi             g_panel;
DataPanelLine_Button*   g_roleBtn      = 0;
DataPanelLine_Button*   g_transBtn     = 0;
DataPanelLine_Button*   g_connBtn      = 0; // Online/Offline toggle (replaces the checkbox)
DataPanelLine_Button*   g_copyIdBtn    = 0;
DataPanelLine_Button*   g_pasteIdBtn   = 0; // "Paste friend's Steam ID" from clipboard
DataPanelLine*          g_debugLine    = 0; // white connection-status debug row
DataPanelLine*          g_peerLine     = 0; // white "Friend's Steam ID" row
DataPanelLine*          g_selfLine     = 0; // white "Your Steam ID" row
std::string             g_selfIdStr;   // self SteamID as digits (set each tick; "" = none)

// Friend's SteamID pasted in-panel this session (0 = none). Per-session by
// design: it lives only in memory, so relaunching Kenshi clears it and the
// friend's id is re-pasted (nothing is written to disk). Passed to onConnect,
// where it overrides the (usually empty) config steamPeer.
unsigned long long      g_pastedPeer   = 0;
bool                    g_pasteFailed  = false; // last paste wasn't a valid Steam ID

// Button callbacks (free functions - MyGUI::newDelegate wraps them without any
// raw-MyGUI link). A press flips the armed flag and requests a rebuild so the
// caption reflects the new choice on the next tick.
void onRoleBtn(DataPanelLine*) {
    g_panel.hostFlag = !g_panel.hostFlag;
    g_panel.needsRebuild = true;
    coop::logLine(g_panel.hostFlag ? "[coop-ui] role -> Host" : "[coop-ui] role -> Join");
}
void onTransBtn(DataPanelLine*) {
    g_panel.steamFlag = !g_panel.steamFlag;
    g_panel.needsRebuild = true;
    coop::logLine(g_panel.steamFlag ? "[coop-ui] transport -> Steam" : "[coop-ui] transport -> UDP");
}
// Online/Offline toggle: flip the desired connection state. The connect/disconnect
// edge (connectedFlag vs lastChkVal) is handled in coopPanelTick, same as before.
void onConnBtn(DataPanelLine*) {
    g_panel.connectedFlag = !g_panel.connectedFlag;
    g_panel.needsRebuild = true;
    coop::logLine(g_panel.connectedFlag ? "[coop-ui] connection -> ONLINE"
                                        : "[coop-ui] connection -> OFFLINE");
}
// Copy the player's own SteamID to the clipboard so they can paste it to a friend
// (who pastes it into their panel via "Paste friend's Steam ID").
void onCopyIdBtn(DataPanelLine*) {
    if (g_selfIdStr.empty()) {
        coop::logLine("[coop-ui] copy Steam ID: none (Steam not running)");
        return;
    }
    bool ok = clipboardSetText(g_selfIdStr.c_str());
    char b[64];
    _snprintf(b, sizeof(b) - 1, "[coop-ui] copied Steam ID to clipboard: %s",
              ok ? "ok" : "FAILED");
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
}
// Paste the friend's SteamID from the clipboard: read text, extract + validate a
// SteamID64, and store it as the session peer (used on the next Connect). No
// typing, no config edit. Rejects arbitrary clipboard junk (g_pasteFailed drives
// the peer-row hint).
void onPasteIdBtn(DataPanelLine*) {
    std::string clip;
    unsigned long long id = 0;
    if (clipboardGetText(clip) && coop::parseSteamId64(clip, id)) {
        g_pastedPeer  = id;
        g_pasteFailed = false;
        char b[64];
        _snprintf(b, sizeof(b) - 1, "[coop-ui] paste friend id=%llu ok=1", id);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
    } else {
        g_pasteFailed = true;
        coop::logLine("[coop-ui] paste friend id=0 ok=0 (clipboard not a Steam ID)");
    }
    g_panel.needsRebuild = true;
}

// POD-only pointer bundle so the row-build SEH frame constructs no std::string.
struct PanelStrings {
    const std::string *title, *roleKey, *roleCap, *transKey, *transCap;
    const std::string *connKey, *connCap;
    const std::string *dbgKey, *dbgVal;
    const std::string *peerKey, *peerVal, *pasteKey, *pasteCap;
    const std::string *selfKey, *selfVal, *copyKey, *copyCap;
    const std::string *empty;
};

void panelBuildSeh(DatapanelGUI* p, const PanelStrings* s) {
    __try {
        p->_NV_clear();
        p->setCaption(*s->title);
        g_roleBtn  = p->setLineButton(*s->roleKey,  *s->roleCap,  0);
        g_transBtn = p->setLineButton(*s->transKey, *s->transCap, 0);
        g_connBtn  = p->setLineButton(*s->connKey,  *s->connCap,  0);
        p->addSpace(0, 0.35f);
        // Connection-status debug line (coloured white below, outside SEH).
        g_debugLine = p->setLine(*s->dbgKey, *s->dbgVal, *s->empty, 0, false, true);
        p->addSpace(0, 0.35f);
        // Friend's SteamID: pasted in-panel (Copy on their side -> Paste here).
        g_peerLine = p->setLine(*s->peerKey, *s->peerVal, *s->empty, 0, false, true);
        g_pasteIdBtn = p->setLineButton(*s->pasteKey, *s->pasteCap, 0);
        p->addSpace(0, 0.35f);
        g_selfLine = p->setLine(*s->selfKey, *s->selfVal, *s->empty, 0, false, true);
        g_copyIdBtn = p->setLineButton(*s->copyKey, *s->copyCap, 0);
        p->_NV_update();
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Colour a line's key + value TextBoxes white for readability. Runs AFTER
// panelBuildSeh's _NV_update (the w1/w2 widgets exist by then). MyGUI::Colour is a
// trivial 4-float struct (no destructor), so it may live in the SEH frame.
void dbgColourSeh(DataPanelLine* line) {
    if (!line) return;
    __try {
        MyGUI::Colour white(1.0f, 1.0f, 1.0f, 1.0f);
        if (line->w1) line->w1->setTextColour(white);
        if (line->w2) line->w2->setTextColour(white);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Arm a freshly-minted panel: register it for ForgottenGUI's per-frame refresh
// AND make it visible. createDatapanel returns a built-but-hidden window; without
// this pair the F2 toggle logs open/close yet nothing ever draws (the render bug
// in the reconstruction). PODs only, so the whole thing sits in one SEH frame.
bool uiPanelArmSeh(ForgottenGUI* g, DatapanelGUI* p) {
    if (!g || !p) return false;
    __try {
        g->addDatapanelToUpdateList(p);
        p->_NV_show(true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void panelDestroySeh(ForgottenGUI* g, DatapanelGUI* p) {
    if (!g || !p) return;
    // Pull it off the refresh list BEFORE destroying so ForgottenGUI never
    // dereferences the freed panel on the next frame.
    __try {
        g->removeDatapanelFromUpdateList(p);
        g->destroy(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

} // namespace

void coopPanelTick(const CoopPanelState* st, CoopConnectFn onConnect,
                   CoopDisconnectFn onDisconnect) {
    if (!st) return;
    ForgottenGUI* g = ::gui; // KenshiLib data export (spike 46)
    { static void* s_last = (void*)-1;
      if ((void*)g != s_last) { s_last = (void*)g;
          char b[64]; _snprintf(b, sizeof(b) - 1, "[coop-ui] gui ptr=%p", (void*)g);
          b[sizeof(b) - 1] = '\0'; coop::logLine(b); } }
    if (!g) return;

    // Cache the self id as a string for the Copy button (used by onCopyIdBtn).
    if (st->selfSteamId) {
        char b[32];
        _snprintf(b, sizeof(b) - 1, "%llu", (unsigned long long)st->selfSteamId);
        b[sizeof(b) - 1] = '\0';
        g_selfIdStr = b;
    } else {
        g_selfIdStr.clear();
    }

    // F2 rising edge toggles the panel open/closed.
    bool f2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    if (f2 && !g_panel.f2Down) {
        if (!g_panel.open) {
            g_panel.hostFlag      = st->isHost;
            g_panel.steamFlag     = (st->transportSel == 0);
            g_panel.connectedFlag = st->running;
            g_panel.lastConnected = st->running;
            g_panel.lastChkVal    = st->running;
            g_panel.open = true;
            g_panel.needsRebuild = true;
            coop::logLine("[coop-ui] panel opened");
        } else {
            panelDestroySeh(g, g_panel.panel);
            g_panel.panel = 0; g_panel.built = false;
            g_roleBtn = 0; g_transBtn = 0; g_connBtn = 0; g_copyIdBtn = 0;
            g_pasteIdBtn = 0;
            g_debugLine = 0; g_peerLine = 0; g_selfLine = 0;
            g_panel.open = false;
            coop::logLine("[coop-ui] panel closed");
        }
    }
    g_panel.f2Down = f2;

    if (!g_panel.open) return;

    // Keep the Online/Offline toggle honest when the session state changes
    // underneath us (a peer-driven connect, a failed connect that stopped, etc):
    // resync the desired flag to the real state and rebuild so the button caption
    // + debug line reflect it.
    if (st->running != g_panel.lastConnected) {
        g_panel.lastConnected = st->running;
        g_panel.connectedFlag = st->running;
        g_panel.lastChkVal    = st->running;
        g_panel.needsRebuild = true;
    }

    std::string detail = st->detail ? std::string(st->detail) : std::string();
    if (detail != g_panel.lastStatus) g_panel.needsRebuild = true;

    // Create the window once (outside SEH - see the header note on C2712).
    // Layer MUST be "Info": spike 48 proved createFloatingLabel renders non-null
    // there. "Windows" is not a visible MyGUI layer here - the panel is minted
    // and armed but attaches to nothing, so F2 logs open/close yet nothing draws.
    if (!g_panel.panel) {
        std::string layer = "Info";
        g_panel.panel = g->createDatapanel(0.22f, 0.30f, 0.30f, 0.44f, false, layer, true);
        g_panel.built = false;
        if (!g_panel.panel) {
            coop::logErrLine("[coop-ui] createDatapanel FAILED");
        } else if (!uiPanelArmSeh(g, g_panel.panel)) {
            coop::logErrLine("[coop-ui] panel arm (update-list/show) FAILED");
        }
    }

    // (Re)populate the rows when anything visible changed.
    if (g_panel.panel && (g_panel.needsRebuild || !g_panel.built)) {
        std::string title    = "Co-op Session    -    F2 to close";
        std::string roleKey  = "role";
        std::string roleCap  = std::string("Role: ") + (g_panel.hostFlag ? "HOST" : "JOIN") + "    (switch)";
        std::string transKey = "trans";
        std::string transCap = std::string("Transport: ") + (g_panel.steamFlag ? "STEAM" : "UDP") + "    (switch)";
        std::string connKey  = "conn";
        std::string connCap  = std::string("Connection: ") + (g_panel.connectedFlag ? "ONLINE" : "OFFLINE") + "    (switch)";

        // White debug line: describes the live connection state + type. Reflects
        // the ACTUAL running session when online; the armed toggles when offline.
        std::string transStr = (st->transportSel == 0) ? "Steam" : "UDP";
        std::string dbgKey   = "Connection status";
        std::string dbgVal;
        if (st->running) {
            if (st->peerPresent)
                dbgVal = (st->isHost ? std::string("Hosting") : std::string("Joining")) +
                         " over " + transStr + " - peer connected";
            else if (st->isHost)
                dbgVal = std::string("Hosting over ") + transStr + " - waiting for peer...";
            else
                dbgVal = std::string("Joining over ") + transStr + " - connecting to host...";
        } else {
            dbgVal = std::string("Offline - will ") + (g_panel.hostFlag ? "host" : "join") +
                     " over " + (g_panel.steamFlag ? "Steam" : "UDP") + " on Connect";
        }

        // Friend's SteamID: prefer the value pasted in-panel this session; fall
        // back to the config (steamPeer, mainly for advanced/back-compat use).
        std::string peerKey = "Friend's Steam ID";
        std::string peerVal;
        unsigned long long peerShown = g_pastedPeer ? g_pastedPeer
                                                     : (unsigned long long)st->peerSteamId;
        if (peerShown != 0) {
            char pb[32];
            _snprintf(pb, sizeof(pb) - 1, "%llu", peerShown);
            pb[sizeof(pb) - 1] = '\0';
            peerVal = pb;
        } else if (g_pasteFailed) {
            peerVal = "(clipboard was not a Steam ID - copy theirs and retry)";
        } else {
            peerVal = "(click Paste friend's Steam ID)";
        }
        std::string pasteKey = "pasteid";
        std::string pasteCap = "Paste friend's Steam ID";

        char selfBuf[40];
        if (st->selfSteamId) {
            _snprintf(selfBuf, sizeof(selfBuf) - 1, "%llu", (unsigned long long)st->selfSteamId);
            selfBuf[sizeof(selfBuf) - 1] = '\0';
        } else {
            strcpy(selfBuf, "(Steam not running)");
        }
        std::string selfKey  = "Your Steam ID";
        std::string selfVal  = selfBuf;
        std::string copyKey  = "copyid";
        std::string copyCap  = "Copy my Steam ID";
        std::string empty    = "";

        PanelStrings ps;
        ps.title = &title; ps.roleKey = &roleKey; ps.roleCap = &roleCap;
        ps.transKey = &transKey; ps.transCap = &transCap;
        ps.connKey = &connKey; ps.connCap = &connCap;
        ps.dbgKey = &dbgKey; ps.dbgVal = &dbgVal;
        ps.peerKey = &peerKey; ps.peerVal = &peerVal;
        ps.pasteKey = &pasteKey; ps.pasteCap = &pasteCap;
        ps.selfKey = &selfKey; ps.selfVal = &selfVal;
        ps.copyKey = &copyKey; ps.copyCap = &copyCap;
        ps.empty = &empty;
        panelBuildSeh(g_panel.panel, &ps);

        // Delegate assignment + white-colouring live OUTSIDE the SEH frame (pointer
        // targets are valid post-build; assignment can't fault) so no delegate
        // temporary lands in it.
        if (g_roleBtn)    g_roleBtn->callback    = MyGUI::newDelegate(&onRoleBtn);
        if (g_transBtn)   g_transBtn->callback   = MyGUI::newDelegate(&onTransBtn);
        if (g_connBtn)    g_connBtn->callback    = MyGUI::newDelegate(&onConnBtn);
        if (g_copyIdBtn)  g_copyIdBtn->callback  = MyGUI::newDelegate(&onCopyIdBtn);
        if (g_pasteIdBtn) g_pasteIdBtn->callback = MyGUI::newDelegate(&onPasteIdBtn);
        dbgColourSeh(g_debugLine);
        dbgColourSeh(g_peerLine);
        dbgColourSeh(g_selfLine);

        g_panel.built = true;
        g_panel.needsRebuild = false;
        g_panel.lastStatus = detail;
    }

    // Connect / disconnect on the Online/Offline toggle edge (edge, not level, so
    // a connect that hasn't reported running yet is not re-fired every tick). The
    // pasted friend id (0 if none) is handed to the plugin, which lets a non-zero
    // value override the config steamPeer; UDP ip/port still come from the config.
    if (g_panel.connectedFlag != g_panel.lastChkVal) {
        g_panel.lastChkVal = g_panel.connectedFlag;
        if (g_panel.connectedFlag && !st->running) {
            char b[80];
            _snprintf(b, sizeof(b) - 1, "[coop-ui] CONNECT role=%s transport=%s",
                      g_panel.hostFlag ? "HOST" : "JOIN",
                      g_panel.steamFlag ? "steam" : "udp");
            b[sizeof(b) - 1] = '\0';
            coop::logLine(b);
            if (onConnect) onConnect(g_panel.hostFlag, g_panel.steamFlag, g_pastedPeer);
        } else if (!g_panel.connectedFlag && st->running) {
            coop::logLine("[coop-ui] DISCONNECT requested");
            if (onDisconnect) onDisconnect();
        }
    }
}

// ---- Persistent co-op status overlay ----------------------------------------
// A ScreenLabel tracked to the local leader (the spike-47/48 render path, reused
// via the marker* SEH shims above), showing live session status colored by state
// (0 = offline/red, 1 = waiting/yellow, 2 = connected/green). Recreated if the
// leader pointer changes (world reload); removed when show=false or no leader.

namespace {
ScreenLabel* g_overlay       = 0;
Character*   g_overlayLeader = 0;
int          g_overlayState  = -1;
std::string  g_overlayText;

int overlayColorId(int state) { return state == 2 ? 0 : (state == 1 ? 2 : 1); }

Character* panelLeaderSeh(GameWorld* gw) {
    __try {
        if (!gw || !gw->player) return 0;
        if (gw->player->playerCharacters.size() == 0) return 0;
        return gw->player->playerCharacters[0];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
} // namespace

void coopOverlayTick(GameWorld* gw, const char* text, int state, bool show) {
    ForgottenGUI* g = ::gui;
    if (!g) return;

    Character* leader = show ? panelLeaderSeh(gw) : 0;
    if (!show || !leader) {
        if (g_overlay) {
            markerDestroySeh(g, g_overlay);
            g_overlay = 0; g_overlayLeader = 0; g_overlayState = -1; g_overlayText.clear();
        }
        return;
    }

    std::string t = text ? std::string(text) : std::string();
    if (!g_overlay || leader != g_overlayLeader) {
        if (g_overlay) markerDestroySeh(g, g_overlay);
        MyGUI::Colour col; markerColour(overlayColorId(state), &col);
        Ogre::Vector3 off(0.0f, 2.8f, 0.0f);
        g_overlay = markerCreateSeh(g, leader, &t, &col, &off);
        g_overlayLeader = leader; g_overlayState = state; g_overlayText = t;
        return;
    }
    if (t != g_overlayText || state != g_overlayState) {
        MyGUI::Colour col; markerColour(overlayColorId(state), &col);
        markerUpdateSeh(g_overlay, &t, &col);
        g_overlayText = t; g_overlayState = state;
    }
}

} // namespace engine
} // namespace coop
