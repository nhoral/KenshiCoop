// ZoneQuery.cpp - the zone-loaded query (Phase 1 spawn parity), quarantined in
// its own TU because kenshi/ZoneManager.h redefines ParticlePool (also defined
// by kenshi/CombatClass.h, which EngineInternal.cpp needs) - the two vendored headers
// cannot share a translation unit.
//
// Why the query exists: within a locally LOADED world block, every baked
// (shared-save) NPC resolves by hand. So an unresolvable census hand whose
// host-reported position sits in a loaded block is a genuine host RUNTIME
// spawn - safe to proxy-mint at any distance without duplicate risk. This
// generalizes the fixed spawnMintRadius_ gate (a cheap stand-in for "the
// block here is certainly loaded") to the engine's own answer.

// ZoneManager.h pulls boost/thread headers (shared_mutex members), whose
// auto-link pragma demands libboost_thread-*.lib. We only form member-function
// pointers on the class (never instantiate it), so no boost code is generated -
// suppress the auto-link instead of shipping the library.
#define BOOST_ALL_NO_LIB

#include <windows.h>

#include <core/Functions.h>     // KenshiLib::GetRealAddress
#include <kenshi/GameWorld.h>   // GameWorld::zoneMgr
#include <kenshi/ZoneManager.h> // ZoneManager::_NV_isZoneLoadedT / _NV_isZoneBeingLoadedT

namespace coop {
namespace engine {

namespace {
// this=RCX, const Ogre::Vector3& = pointer in RDX.
typedef bool (__fastcall* ZoneLoadedFn)(ZoneManager* self, const Ogre::Vector3* pos);
ZoneLoadedFn g_zoneLoadedFn      = 0;
ZoneLoadedFn g_zoneBeingLoadedFn = 0;
} // namespace

// Called from engine::resolve(). Resolved via the _NV_ non-virtual aliases
// (concrete RVAs; the virtual names resolve to vtable thunks).
void resolveZoneQuery() {
    g_zoneLoadedFn = (ZoneLoadedFn)KenshiLib::GetRealAddress(
        &ZoneManager::_NV_isZoneLoadedT);
    g_zoneBeingLoadedFn = (ZoneLoadedFn)KenshiLib::GetRealAddress(
        &ZoneManager::_NV_isZoneBeingLoadedT);
}

bool isZoneLoadedAt(GameWorld* gw, float x, float y, float z) {
    if (!gw || !g_zoneLoadedFn) return false;
    __try {
        ZoneManager* zm = gw->zoneMgr;
        if (!zm) return false;
        Ogre::Vector3 p(x, y, z);
        if (!g_zoneLoadedFn(zm, &p)) return false;
        // A block MID-LOAD hasn't materialized its baked NPCs yet - treating it
        // as loaded would far-mint a duplicate of a body about to appear.
        if (g_zoneBeingLoadedFn && g_zoneBeingLoadedFn(zm, &p)) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace engine
} // namespace coop
