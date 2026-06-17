// Engine implementation. See Engine.h. Built for the VS2010 (v100) toolchain.
//
// The Boost auto-link guards must precede any KenshiLib header that transitively
// pulls in <boost/thread/...> (e.g. RootObjectFactory.h), exactly as the legacy
// monolith does, or MSBuild tries to link Boost libs we neither ship nor use.

#define _CRT_SECURE_NO_WARNINGS 1
#define BOOST_ALL_NO_LIB 1
#define BOOST_ERROR_CODE_HEADER_ONLY 1
#define BOOST_SYSTEM_NO_DEPRECATED 1

#include "Engine.h"
#include "../CoopLog.h"

#include <core/Functions.h>         // KenshiLib::GetRealAddress
#include <kenshi/GameWorld.h>       // GameWorld::player
#include <kenshi/PlayerInterface.h> // PlayerInterface::playerCharacters
#include <kenshi/SaveManager.h>     // SaveManager::getSingleton/load/savesExist
#include <kenshi/Character.h>       // Character (handle/getPosition/getOrientation/movement)
#include <kenshi/CharMovement.h>    // CharMovement::_setPositionDirectionAndTeleport/setDestination
#include <kenshi/Enums.h>           // itemType, MoveSpeed { RUN }
#include <kenshi/util/hand.h>       // hand (5-field identity)
#include <kenshi/util/lektor.h>     // lektor<Character*> (playerCharacters)
#include <ogre/OgreVector3.h>
#include <ogre/OgreQuaternion.h>

namespace coop {
namespace engine {
namespace {

// SaveManager entry points, resolved at load. getSingleton is a static member;
// load(name) and savesExist are __thiscall (passed via __fastcall self in RCX).
typedef SaveManager* (__fastcall* SaveMgrGetFn)();
typedef void         (__fastcall* SaveMgrLoadNameFn)(SaveManager* self, const std::string* name);
typedef bool         (__fastcall* SaveMgrSavesExistFn)(SaveManager* self);

SaveMgrGetFn        g_getFn        = 0;
SaveMgrLoadNameFn   g_loadFn       = 0;
SaveMgrSavesExistFn g_savesExistFn = 0;

// hand::getCharacter resolves a save-stable handle back to the local Character*.
// The 5-arg hand constructor builds a proper hand (sets the vptr) from received
// fields so getCharacter() can be called on it.
typedef Character* (__fastcall* HandGetCharFn)(const hand* self);
typedef hand*      (__fastcall* HandCtorFn)(hand* self, unsigned int index,
                                            unsigned int serial, itemType type,
                                            unsigned int container,
                                            unsigned int containerSerial);

HandGetCharFn g_handGetCharFn = 0;
HandCtorFn    g_handCtorFn    = 0;

// Character::setDestination(pos, shift) is the PLAYER move-order path (what
// click-to-move issues); unlike CharMovement::setDestination it actually moves a
// player-controlled character. Non-virtual, so resolved at runtime.
typedef void (__fastcall* CharSetDestFn)(Character* self, const Ogre::Vector3* pos, bool shift);
CharSetDestFn g_charSetDestFn = 0;

} // namespace

void resolve() {
    g_getFn  = (SaveMgrGetFn)KenshiLib::GetRealAddress(&SaveManager::getSingleton);
    g_loadFn = (SaveMgrLoadNameFn)KenshiLib::GetRealAddress(
        static_cast<void (SaveManager::*)(const std::string&)>(&SaveManager::load));
    g_savesExistFn = (SaveMgrSavesExistFn)KenshiLib::GetRealAddress(&SaveManager::savesExist);
    if (!g_getFn || !g_loadFn)
        coop::logErrLine("engine: could not resolve SaveManager load functions");

    // Entity resolve path: hand->Character lookup and the 5-arg hand ctor
    // (overloaded, so disambiguate via an explicit member-pointer cast).
    g_handGetCharFn = (HandGetCharFn)KenshiLib::GetRealAddress(&hand::getCharacter);
    g_handCtorFn = (HandCtorFn)KenshiLib::GetRealAddress(
        static_cast<hand* (hand::*)(unsigned int, unsigned int, itemType,
                                    unsigned int, unsigned int)>(&hand::_CONSTRUCTOR));
    if (!g_handGetCharFn || !g_handCtorFn)
        coop::logErrLine("engine: could not resolve hand resolve functions");

    g_charSetDestFn = (CharSetDestFn)KenshiLib::GetRealAddress(
        static_cast<void (Character::*)(const Ogre::Vector3&, bool)>(
            &Character::setDestination));
}

bool gameplayLive(GameWorld* gw) {
    __try {
        return gw && gw->player && gw->player->playerCharacters.size() > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool loadSave(const std::string& name) {
    if (!g_getFn || !g_loadFn) return false;
    __try {
        SaveManager* mgr = g_getFn();
        if (!mgr) return false;
        g_loadFn(mgr, &name); // deferred: sets LOADGAME, engine loads a few frames later
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool savesReady() {
    if (!g_getFn || !g_savesExistFn) return true; // unresolved: don't block auto-load
    __try {
        SaveManager* mgr = g_getFn();
        if (!mgr) return false;
        return g_savesExistFn(mgr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- Entity capture / resolve / apply --------------------------------------

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
        // Pose fields unused until Stage 5.
        e->task = TASK_NONE;
        e->sType = e->sContainer = e->sContainerSerial = e->sIndex = e->sSerial = 0;
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
        // Player move-order path first (a player-controlled leader ignores a bare
        // CharMovement::setDestination, but obeys this - same path click-to-move
        // uses, which we proved drives the leader in Stage 1).
        if (g_charSetDestFn) g_charSetDestFn(c, &dest, false);

        CharMovement* mv = c->movement;
        if (mv) {
            if (!g_charSetDestFn) mv->setDestination(dest, HIGH_PRIORITY, false);
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

Character* leader(GameWorld* gw) {
    if (!gw) return 0;
    __try {
        if (!gw->player || gw->player->playerCharacters.size() == 0) return 0;
        return gw->player->playerCharacters[0];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

} // namespace engine
} // namespace coop
