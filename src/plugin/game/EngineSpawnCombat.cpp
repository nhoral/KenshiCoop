// EngineSpawnCombat.cpp - spawn + combat + medical primitives (monolith split
// from EngineInternal.cpp, 2026-07-12): protocol 21 runtime-spawn proxy replication
// (spawnProxyNpc/despawnProxyNpc/spawnRuntimeSquad + describeCharacter),
// scenario scene setup (seats/beds/cages/machines/duel/down scenes), combat
// reads + melee order levers, and the limb-loss / revive / bandage medical
// primitives (spike 21 field map).
//
// Owner state: section-private statics only (duel/down anchors + hands,
// SEAT_MATCH_DIST, SEH shims for the std::string-holding entry points).
// Must NOT: define g_* engine pointers (EngineInternal.cpp owns them - EngineInternal.h
// declares them), install hooks, or change any log string - log phrasing is
// the API consumed by the PowerShell oracles (see resources/CODE_MAP.md).

#include "EngineInternal.h"
#include "../core/WorkPose.h" // SEAT_MATCH_DIST / WORK_MATCH_DIST / poseMatchDist

namespace coop {
namespace engine {

// ---- Protocol 21: runtime-spawn proxy replication ---------------------------

// SEH shims: the public entry points below hold std::string locals (destructors
// forbid __try in the same frame, the loadSave pattern), so the raw engine
// touches live in these POD-only helpers.
// External linkage (EngineInternal.h): the world TU's relation levers share it.
Faction* factionBySidGuarded(GameWorld* gw, const std::string* sid) {
    __try {
        if (!gw || !gw->factionMgr || !g_facBySidFn) return 0;
        return g_facBySidFn(gw->factionMgr, sid);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

namespace {
Faction* nonPlayerFactionGuarded(GameWorld* gw) {
    __try {
        return findNearbyNonPlayerFaction(gw);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
Character* createCharGuarded(GameWorld* gw, GameData* tmpl, Faction* fac,
                             float x, float y, float z, float age) {
    __try {
        Ogre::Vector3 pos(x, y, z);
        RootObject* r = g_createCharFn(gw->theFactory, fac, pos, 0, tmpl, 0, age);
        return r ? static_cast<Character*>(r) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
} // namespace

bool describeCharacter(Character* c, char* charSid, unsigned int charSidLen,
                       char* facSid, unsigned int facSidLen,
                       float* x, float* y, float* z, float* heading, bool* dead,
                       float* age) {
    if (!c || !charSid || charSidLen == 0 || !facSid || facSidLen == 0) return false;
    charSid[0] = '\0';
    facSid[0]  = '\0';
    if (age) *age = 0.0f;
    __try {
        GameData* gd = c->getGameData();
        if (!gd) return false;
        const char* sid = gd->stringID.c_str();
        if (!sid || !sid[0]) return false;
        strncpy(charSid, sid, charSidLen - 1);
        charSid[charSidLen - 1] = '\0';
        // Faction sid is best-effort: "" tells the join to fall back to a local
        // non-player faction (cosmetic - combat outcomes are host-authoritative).
        Faction* f = c->getFaction();
        if (f && g_facGetDataFn) {
            GameData* fd = g_facGetDataFn(f);
            if (fd) {
                strncpy(facSid, fd->stringID.c_str(), facSidLen - 1);
                facSid[facSidLen - 1] = '\0';
            }
        }
        Ogre::Vector3 p = c->getPosition();
        if (x) *x = p.x;
        if (y) *y = p.y;
        if (z) *z = p.z;
        if (heading) *heading = c->getOrientation().getYaw().valueRadians();
        if (dead) *dead = (g_isDeadCharFn && g_isDeadCharFn(c));
        // Age drives animal body SCALE (CharacterAnimal ageSizeMin/Max) - the
        // join mints its proxy with this value (protocol 39). Virtual call
        // through the vtable; humans return their cosmetic age (no scaling).
        if (age) *age = c->getAge();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// SEH-guarded (join, mint duplicate guard 2026-07-11): return a world NPC with
// the SAME template stringID standing within 'radius' of (x,y,z), or 0. The
// census-missing hand may be THAT body under a hand we cannot correlate
// (engine re-container, baked block just loaded) - minting would stand a
// double on top of it. 'excl'/'exclCount' skips bodies the caller already
// accounts for (bound proxies, suppressed culls).
Character* sameTemplateNear(GameWorld* gw, const char* charSid,
                            float x, float y, float z, float radius,
                            Character* const* excl, unsigned int exclCount) {
    if (!gw || !g_getCharsFn || !charSid || !charSid[0]) return 0;
    __try {
        Ogre::Vector3 center(x, y, z);
        g_npcQuery.clear();
        g_getCharsFn(gw, &g_npcQuery, &center, radius, radius, radius, 96, 96, 0);
        unsigned int total = g_npcQuery.size();
        for (unsigned int i = 0; i < total; ++i) {
            RootObject* obj = g_npcQuery[i];
            if (!obj || isPlayerSquad(gw, obj)) continue;
            Character* ch = static_cast<Character*>(obj);
            bool skip = false;
            for (unsigned int e = 0; e < exclCount && !skip; ++e)
                if (excl[e] == ch) skip = true;
            if (skip) continue;
            GameData* gd = ch->getGameData();
            if (!gd) continue;
            const char* sid = gd->stringID.c_str();
            if (sid && strcmp(sid, charSid) == 0) return ch;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

Character* spawnProxyNpc(GameWorld* gw, const char* charSid, const char* facSid,
                         float x, float y, float z, float heading, float age) {
    if (!gw || !gw->theFactory || !g_createCharFn || !charSid || !charSid[0]) return 0;
    // Creature-size sync (protocol 39): animals scale body size by age, so the
    // proxy must be CREATED at the host's age or it spawns full-grown (the
    // 2026-07-12 "giant goats" session). A non-finite or <= 0 wire value means
    // the host couldn't read it - keep the historical adult default.
    if (!(age > 0.0f && age < 1.0e6f)) age = 25.0f;
    // Template by CHARACTER stringID (the same category-scan lookup the
    // inventory reconstructor uses for items). Animal templates live in the
    // SEPARATE ANIMAL_CHARACTER category (2026-07-11 pack-hidden session:
    // Bonedog sid '3979-gamedata.base' MISSed the CHARACTER scan forever, so
    // host wildlife packs could never appear on the join) - scan it second.
    GameData* tmpl = findItemTemplateImpl(gw, charSid, (unsigned int)CHARACTER);
    if (!tmpl)
        tmpl = findItemTemplateImpl(gw, charSid, (unsigned int)ANIMAL_CHARACTER);
    if (!tmpl) {
        char b[128];
        _snprintf(b, sizeof(b) - 1, "[spawn] proxy template MISS sid='%s'", charSid);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return 0;
    }
    Faction* fac = 0;
    if (facSid && facSid[0]) {
        std::string fs(facSid);
        fac = factionBySidGuarded(gw, &fs);
    }
    // Unknown faction sid (modded faction absent here / "" reply): any live
    // non-player faction keeps the proxy a true world NPC. Cosmetic only.
    if (!fac) fac = nonPlayerFactionGuarded(gw);
    if (!fac) {
        coop::logLine("[spawn] proxy faction MISS (no non-player faction loaded)");
        return 0;
    }
    Character* c = createCharGuarded(gw, tmpl, fac, x, y, z, age);
    if (!c) {
        char b[128];
        _snprintf(b, sizeof(b) - 1, "[spawn] proxy create FAILED sid='%s'", charSid);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return 0;
    }
    // Land exactly on the host transform, facing the host heading (createChar
    // grounds/settles on its own); the stream drives it from here.
    park(c, x, y, z, heading);
    // Quiet its local AI immediately: the host stream is the sole task
    // authority for a proxy (AI-suspend re-asserts this every driven tick).
    detachFromTownAI(c);
    clearGoals(c);
    return c;
}

bool despawnProxyNpc(GameWorld* gw, Character* proxy) {
    if (!gw || !proxy || !g_destroyObjFn) return false;
    __try {
        return g_destroyObjFn(gw, static_cast<RootObject*>(proxy),
                              /*justUnloaded*/false, "coop-proxy-dupe-heal");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

unsigned int spawnRuntimeSquad(GameWorld* gw, unsigned int count,
                               unsigned int (*outHands)[5]) {
    if (!gw || count == 0) return 0;
    Faction* fac = nonPlayerFactionGuarded(gw);
    unsigned int n = 0;
    for (unsigned int i = 0; i < count; ++i) {
        // Spread the squad in a loose rank in front of the leader.
        float fwd  = 6.0f + 2.0f * (float)(i / 2);
        float side = (i & 1) ? 2.0f : -2.0f;
        Character* c = fac ? spawnCharInFaction(gw, fwd, side, fac)
                           : spawnNpcInFront(gw, fwd, side);
        if (!c) continue;
        // Detach from town-AI so the town scheduler doesn't immediately re-task
        // the squad away (the same quieting the duel/craft scenes rely on).
        detachFromTownAI(c);
        unsigned int h[5] = { 0, 0, 0, 0, 0 };
        bool haveHand = readObjectHand(static_cast<RootObject*>(c), h);
        if (outHands) {
            for (int j = 0; j < 5; ++j) outHands[n][j] = h[j];
        }
        char b[160];
        _snprintf(b, sizeof(b) - 1,
                  "[spawn] runtime squad member %u/%u hand=%u,%u,%u,%u,%u haveHand=%d",
                  n + 1, count, h[0], h[1], h[2], h[3], h[4], haveHand ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        ++n;
    }
    char b[96];
    _snprintf(b, sizeof(b) - 1, "[spawn] runtime squad spawned=%u/%u fac=%s",
              n, count, fac ? "nonplayer" : "leader-fallback");
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    return n;
}

bool spawnMachineInFront(GameWorld* gw, float fwd, float side, Faction* owner,
                         RootObject** spawned) {
    if (spawned) *spawned = 0;
    if (!gw || !gw->theFactory || !g_createBldgFn) {
        coop::logLine("SETUP: machine spawn skipped (no factory / createBuilding fn)");
        return false;
    }
    __try {
        GameData* tmpl = findMachineTemplate(gw);
        {
            char d[200];
            _snprintf(d, sizeof(d) - 1, "SETUP: machineTemplate='%s'",
                      tmpl ? tmpl->name.c_str() : "(none)");
            d[sizeof(d) - 1] = '\0';
            coop::logLine(d);
        }
        if (!tmpl) return false;
        Ogre::Vector3 pos; float yaw = 0.0f;
        if (!leaderAnchor(gw, fwd, side, &pos, &yaw)) return false;
        // Face the work face toward the leader so the operating body is visible.
        Ogre::Quaternion rot(Ogre::Radian(yaw + 3.14159265f), Ogre::Vector3::UNIT_Y);
        Ogre::Vector3 placePos(pos.x, 0.0f, pos.z); // y=0: createBuilding re-grounds
        Building* b = g_createBldgFn(
            gw->theFactory, tmpl, placePos, /*town*/0, /*owner*/owner, rot, /*cb*/0,
            /*furnitureOf*/0, /*isDoorOf*/0, /*saveState*/0, /*isIndoorsOf*/0,
            /*invisible*/false, /*completed*/true, /*isFoliage*/false,
            /*floor*/0, /*isOutsideFurniture*/false);
        if (!b) { coop::logLine("SETUP: machine createBuilding returned null"); return false; }
        RootObject* ro = reinterpret_cast<RootObject*>(b); // Building's first base
        Ogre::Vector3 ap = ro->getPosition();
        {
            char d[160];
            _snprintf(d, sizeof(d) - 1, "SETUP: machine actualPos=%.2f,%.2f,%.2f visible=%d",
                      ap.x, ap.y, ap.z, ro->getVisible() ? 1 : 0);
            d[sizeof(d) - 1] = '\0';
            coop::logLine(d);
        }
        if (spawned) *spawned = ro;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("SETUP: machine createBuilding FAULTED");
        return false;
    }
}

// Internal: spawn one building from an already-chosen template at a leader-
// relative offset (the spawnMachineInFront shape, template passed in). Caller
// holds no SEH - this guards itself. Logs the landing position + hand.
static RootObject* spawnTemplateInFront(GameWorld* gw, GameData* tmpl,
                                        float fwd, float side, const char* tag) {
    if (!gw || !gw->theFactory || !g_createBldgFn || !tmpl) return 0;
    __try {
        Ogre::Vector3 pos; float yaw = 0.0f;
        if (!leaderAnchor(gw, fwd, side, &pos, &yaw)) return 0;
        // Face the fixture back toward the leader (same as seat/machine spawns).
        Ogre::Quaternion rot(Ogre::Radian(yaw + 3.14159265f), Ogre::Vector3::UNIT_Y);
        Ogre::Vector3 placePos(pos.x, 0.0f, pos.z); // y=0: createBuilding re-grounds
        Building* b = g_createBldgFn(
            gw->theFactory, tmpl, placePos, /*town*/0, /*owner*/0, rot, /*cb*/0,
            /*furnitureOf*/0, /*isDoorOf*/0, /*saveState*/0, /*isIndoorsOf*/0,
            /*invisible*/false, /*completed*/true, /*isFoliage*/false,
            /*floor*/0, /*isOutsideFurniture*/false);
        if (!b) return 0;
        RootObject* ro = reinterpret_cast<RootObject*>(b); // Building's first base
        Ogre::Vector3 ap = ro->getPosition();
        unsigned int h[5] = { 0, 0, 0, 0, 0 };
        bool haveHand = readObjectHand(ro, h);
        char d[224];
        _snprintf(d, sizeof(d) - 1,
                  "SETUP(bedcage): %s '%s' pos=%.2f,%.2f,%.2f hand=%u,%u,%u,%u,%u(%d)",
                  tag, tmpl->name.c_str(), ap.x, ap.y, ap.z,
                  h[0], h[1], h[2], h[3], h[4], haveHand ? 1 : 0);
        d[sizeof(d) - 1] = '\0';
        coop::logLine(d);
        return ro;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("SETUP(bedcage): createBuilding FAULTED");
        return 0;
    }
}

bool setupBedCageScene(GameWorld* gw) {
    GameData* bedTmpl = 0;
    GameData* cageTmpl = 0;
    __try {
        bedTmpl  = findBedTemplate(gw);
        cageTmpl = findCageTemplate(gw);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("SETUP(bedcage): template lookup FAULTED");
        return false;
    }
    {
        char d[224];
        _snprintf(d, sizeof(d) - 1, "SETUP(bedcage): bedTemplate='%s' cageTemplate='%s'",
                  bedTmpl ? bedTmpl->name.c_str() : "(none)",
                  cageTmpl ? cageTmpl->name.c_str() : "(none)");
        d[sizeof(d) - 1] = '\0';
        coop::logLine(d);
    }
    // Bed left of the leader, cage right - far enough apart that a body placed
    // in one cannot ambiguously read as "at" the other in the oracles.
    RootObject* bed  = spawnTemplateInFront(gw, bedTmpl,  7.0f, -5.0f, "bed");
    RootObject* cage = spawnTemplateInFront(gw, cageTmpl, 7.0f,  5.0f, "cage");
    return bed != 0 && cage != 0;
}

// Prisoner-pole occupancy fixture (protocol 19 kind=4 / engine IN_PRISON): spawn
// a single standing prisoner POLE in front of the leader so both clients load a
// save-stable pole hand. A pole is the same containment SYSTEM as a cage
// (setPrisonMode), just a different model - the pole_put controlled test wants
// this model so the sync is visibly a body ON A POLE. Auto-baked into 'pole1'.
bool setupPoleScene(GameWorld* gw) {
    GameData* poleTmpl = 0;
    __try {
        poleTmpl = findPoleTemplate(gw);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("SETUP(pole): template lookup FAULTED");
        return false;
    }
    {
        char d[224];
        _snprintf(d, sizeof(d) - 1, "SETUP(pole): poleTemplate='%s'",
                  poleTmpl ? poleTmpl->name.c_str() : "(none)");
        d[sizeof(d) - 1] = '\0';
        coop::logLine(d);
    }
    if (!poleTmpl) {
        coop::logLine("SETUP(pole): no prisoner-pole template found (see candidates above)");
        return false;
    }
    // Directly in front of the leader, framed by the default camera.
    RootObject* pole = spawnTemplateInFront(gw, poleTmpl, 7.0f, 0.0f, "pole");
    return pole != 0;
}

bool orderWorkAt(Character* c, RootObject* fixture, int task) {
    if (!c || !fixture) return false;
    if (!g_addOrderFn && !g_addJobFn) return false;
    __try {
        if (g_clearGoalsFn) g_clearGoalsFn(c);
        Ogre::Vector3 loc = fixture->getPosition(); // virtual: safe direct call
        if (g_addOrderFn) {
            Building* dest = reinterpret_cast<Building*>(fixture);
            g_addOrderFn(c, dest, task, fixture, /*shift*/false,
                         /*clear*/true, &loc);
        } else if (g_addJobFn) {
            g_addJobFn(c, task, fixture, /*shift*/false,
                       /*addDontClear*/false, &loc);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Find the BAKED bed (kind=1) or prison cage (kind=2) near the leader by
// scanning loaded BUILDING objects by name - the bedcage1 fixture-relocation
// path (same shape as findWorkFixtureNear: save-stable buildings are simply
// searched for by template name after a reload).
RootObject* findFurnitureNear(GameWorld* gw, int kind) {
    if (!gw || !g_getObjsFn || !gw->player) return 0;
    if (gw->player->playerCharacters.size() == 0) return 0;
    __try {
        Ogre::Vector3 center = gw->player->playerCharacters[0]->getPosition();
        g_npcQuery.clear();
        g_getObjsFn(gw, &g_npcQuery, &center, 60.0f, BUILDING, 256, 0);
        unsigned int total = g_npcQuery.size();
        const char* bedPrefs[]  = { "camp bed", "bedroll", "bed" };
        const char* cagePrefs[] = { "prisoner cage", "cage" };
        // kind==4: a PRISONER POLE. Same engine containment as a cage (kind=2 /
        // setPrisonMode) but a distinct model - prefer pole/shackle names so a
        // world holding both a cage and a pole picks the pole.
        const char* polePrefs[] = { "prisoner pole", "cage pole", "shackle", "pole" };
        const char** prefs = (kind == 4) ? polePrefs
                                          : ((kind == 2) ? cagePrefs : bedPrefs);
        const unsigned int nprefs = (kind == 4) ? 4u : ((kind == 2) ? 2u : 3u);
        for (unsigned int k = 0; k < nprefs; ++k) {
            for (unsigned int i = 0; i < total; ++i) {
                RootObject* o = g_npcQuery[i];
                if (!o) continue;
                GameData* gd = o->getGameData();
                if (gd && ciContains(gd->name.c_str(), prefs[k])) return o;
            }
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool orderUseBed(GameWorld* gw, const unsigned int subjHand[5],
                 int* orderedTask, int* useBedTask) {
    // Report the numeric task ids so scenarios can log them for the oracle
    // (the oracle never hardcodes TaskType values).
    if (orderedTask) *orderedTask = (int)USE_BED_ORDER;
    if (useBedTask)  *useBedTask  = (int)USE_BED;
    Character* c = resolveCharByHand(subjHand[3], subjHand[4],
                                     subjHand[0], subjHand[1], subjHand[2]);
    if (!c) return false;
    // Guarded: already on a bed task -> success without re-clearing goals
    // (re-issuing would stand the sleeper up and restart the approach).
    int cur = readTaskKey(c);
    if (cur == (int)USE_BED || cur == (int)USE_BED_ORDER) return true;
    RootObject* bed = findFurnitureNear(gw, /*kind*/1);
    if (!bed) return false;
    return orderWorkAt(c, bed, (int)USE_BED_ORDER);
}

// Give 'c' a PERSISTENT AI GOAL (not a player order) to work 'task' at 'fixture'.
// addGoal hands the intent to the NPC's OWN AI - it is NOT a squad/player-order
// mechanism, so it does not recruit the NPC into the player squad. The NPC then
// walks to the fixture and operates it autonomously, exactly the natural-task path
// the host captures (the way the bar NPCs naturally sat). Returns true if issued.
bool goalWorkAt(Character* c, RootObject* fixture, int task) {
    if (!c || !fixture || !g_addGoalFn) return false;
    __try {
        if (g_clearGoalsFn) g_clearGoalsFn(c);
        g_addGoalFn(c, task, fixture);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Craft RE-ARM: a worker's addGoal intent does NOT serialise, so a baked craft
// scene (craft1) reloads with an idle worker. Re-find the baked fixture + nearest
// non-squad worker by SEARCH and re-issue the work goal, so the HOST resumes
// streaming the work task each session. Cheap + idempotent: it no-ops when the
// worker is already on task (re-issuing every tick would thrash pathing). Meant to
// be called once on load and then periodically by the host tick. Returns true if a
// goal is active/issued.
bool rearmCraftScene(GameWorld* gw) {
    int task = USE_TRAINING_DUMMY;
    RootObject* fixture = findWorkFixtureNear(gw, &task);
    if (!fixture) return false; // nothing baked nearby - silent (called on a timer)
    Character* worker = findWorkerNear(gw, fixture);
    if (!worker) return false;
    int cur = readCharTaskKey(worker);
    if (cur == task) return true; // already operating - leave it alone (no thrash)
    bool issued = goalWorkAt(worker, fixture, task);
    {
        unsigned int h[5];
        readObjectHand(static_cast<RootObject*>(worker), h);
        char b[160];
        _snprintf(b, sizeof(b) - 1,
                  "REARM: re-issued work goal task=%d worker=%u,%u curTask=%d ok=%d",
                  task, h[3], h[4], cur, issued ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    return issued;
}

// Identify the craft worker to drive for a LIVE-order test: the non-squad NPC
// nearest the baked work fixture, plus the task. Returns the worker's hand (in
// readObjectHand layout: [type,container,containerSerial,index,serial]) so a
// scenario can PIN this exact NPC for the whole run - vs re-picking "nearest now",
// which drifts as other world NPCs wander past the prop and orders the wrong body.
bool pickCraftWorker(GameWorld* gw, unsigned int workerHand[5], int* outTask) {
    int task = USE_TRAINING_DUMMY;
    RootObject* fixture = findWorkFixtureNear(gw, &task);
    if (!fixture) return false;
    Character* w = findWorkerNear(gw, fixture);
    if (!w) return false;
    if (!readObjectHand(static_cast<RootObject*>(w), workerHand)) return false;
    if (outTask) *outTask = task;
    return true;
}

// Hold the pinned worker UNTASKED at the prop during a craft_order baseline: clear
// its faction patrol goal and PARK it at the fixture each tick. An idle world NPC
// otherwise patrols out of the host's capture range (observed: only a handful of
// MEMBER samples, then it is too far to reach the prop when ordered). Parking it at
// the prop is the faithful "untasked NPC standing by a prop" staging. Returns true
// if held.
bool holdWorkerAtFixture(GameWorld* gw, const unsigned int workerHand[5]) {
    Character* w = resolveCharByHand(workerHand[3], workerHand[4], workerHand[0],
                                     workerHand[1], workerHand[2]);
    if (!w) return false;
    RootObject* fixture = findWorkFixtureNear(gw, 0);
    if (!fixture) return false;
    float fxx = 0, fxy = 0, fxz = 0;
    __try {
        Ogre::Vector3 p = fixture->getPosition();
        fxx = p.x; fxy = p.y; fxz = p.z;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    clearGoals(w);
    return park(w, fxx, fxy, fxz + 2.5f, 0.0f); // ~2.5u in front, not inside the prop
}

// Order a SPECIFIC worker (pinned by hand) to work the baked fixture (re-found by
// search - there is only one). This is the runtime EVENT the craft_order scenario
// fires: the host hands the pinned NPC a work goal mid-run so the join's driven copy
// transitions idle -> operating. workerHand is in readObjectHand layout. Guarded:
// if the worker is ALREADY operating, do nothing (re-issuing goalWorkAt every tick
// clears + re-adds the goal, thrashing pathing so it never settles into the pose -
// the same lesson the periodic re-arm encodes).
bool orderCraftWorker(GameWorld* gw, const unsigned int workerHand[5], int task) {
    Character* w = resolveCharByHand(workerHand[3], workerHand[4], workerHand[0],
                                     workerHand[1], workerHand[2]);
    if (!w) return false;
    if (readCharTaskKey(w) == task) return true; // already operating - don't thrash
    RootObject* fixture = findWorkFixtureNear(gw, 0);
    if (!fixture) return false;
    return goalWorkAt(w, fixture, task);
}

// --- down_order (Stage 2, LIVE knockout transition) ------------------------------
// A FIXED world anchor captured once at pin time. The baseline hold parks the subject
// here every tick. It must NOT be recomputed from the live leader each tick: a parked
// body that collides with the leader shoves it, the leader-relative anchor then chases
// the shoved leader, and the feedback flings the leader across the map (observed).
static float g_downAnchor[4]   = { 0, 0, 0, 0 }; // x,y,z,yaw
static bool  g_haveDownAnchor  = false;

// Identify the subject to knock out for a LIVE down-order test: the non-squad NPC
// nearest the LOCAL leader (in down1 that is the baked subject standing in front).
// Returns its hand in readObjectHand layout so the scenario PINS this exact NPC for
// the whole run, so host + join drive the SAME identity across the upright->down
// transition. Also latches a FIXED leader-front anchor (a few metres clear of the
// leader) for the baseline hold. Mirrors pickCraftWorker but anchors on the leader.
bool pickDownSubject(GameWorld* gw, unsigned int subjHand[5]) {
    if (!g_getCharsFn || !gw || !gw->player ||
        gw->player->playerCharacters.size() == 0) return false;
    Character* best = 0;
    __try {
        Ogre::Vector3 center = gw->player->playerCharacters[0]->getPosition();
        g_npcQuery.clear();
        g_getCharsFn(gw, &g_npcQuery, &center, 30.0f, 30.0f, 30.0f, 64, 64, 0);
        float bestD = 1e18f;
        for (unsigned int i = 0; i < g_npcQuery.size(); ++i) {
            RootObject* o = g_npcQuery[i];
            if (!o || isPlayerSquad(gw, o)) continue;
            Ogre::Vector3 p = o->getPosition();
            float dx = p.x - center.x, dz = p.z - center.z;
            float d2 = dx * dx + dz * dz;
            if (d2 < bestD) { bestD = d2; best = static_cast<Character*>(o); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!best) return false;
    if (!readObjectHand(static_cast<RootObject*>(best), subjHand)) return false;
    // Latch the hold anchor ONCE: 6 m in front of the leader (well clear of its body
    // so the parked subject never collides with / pushes the leader).
    Ogre::Vector3 pos; float yaw = 0.0f;
    if (leaderAnchor(gw, 6.0f, 0.0f, &pos, &yaw)) {
        g_downAnchor[0] = pos.x; g_downAnchor[1] = pos.y; g_downAnchor[2] = pos.z;
        g_downAnchor[3] = yaw; g_haveDownAnchor = true;
    }
    return true;
}

// Keep the pinned subject UPRIGHT and in capture range during a down_order baseline.
// A baked world NPC's AI paths it AWAY on reload (observed: ~89 u/s off-screen within
// 2 s, so by order-time it is far outside capture and the host streams no down body),
// and clearGoals alone does not hold it - so we PARK it at the FIXED anchor latched at
// pin time (teleport wins over its AI), like holdWorkerAtFixture pins the craft worker.
// The anchor is fixed (not leader-relative) to avoid the parked-body-pushes-leader
// feedback loop. It stays upright (nothing downs it in the baseline) and in range, so
// the join gets a clean upright "before" series. Returns true if held.
bool holdSubjectUpright(GameWorld* gw, const unsigned int subjHand[5]) {
    (void)gw;
    if (!g_haveDownAnchor) return false;
    Character* s = resolveCharByHand(subjHand[3], subjHand[4], subjHand[0],
                                     subjHand[1], subjHand[2]);
    if (!s) return false;
    clearGoals(s);
    return park(s, g_downAnchor[0], g_downAnchor[1], g_downAnchor[2], g_downAnchor[3]);
}

// The runtime EVENT the down_order scenario fires: knock the PINNED subject out so
// the host streams bodyState down and the join's driven copy must transition
// upright -> down. NOT guarded against re-issue: re-applying the forced KO timer on
// a throttle just tops it up (the body is already collapsed, no re-collapse), which
// is how it stays down for the rest of the run. subjHand is readObjectHand layout.
bool orderDownSubject(GameWorld* gw, const unsigned int subjHand[5]) {
    (void)gw;
    Character* s = resolveCharByHand(subjHand[3], subjHand[4], subjHand[0],
                                     subjHand[1], subjHand[2]);
    if (!s) return false;
    return knockDown(s, true);
}

// death_order: the runtime EVENT that KILLS the pinned subject. Test scaffold (no
// combat yet): collapse the body (ragdoll + KO) so it lies down, then mark the
// medical system dead + drain blood so Character::isDead() flips true. That sets
// BODY_DEAD in the host's bodyState capture, which publishOwned turns into a reliable
// EVT_DEATH. Re-assertable on a throttle (idempotent: already-dead stays dead).
bool killSubject(GameWorld* gw, const unsigned int subjHand[5]) {
    (void)gw;
    Character* s = resolveCharByHand(subjHand[3], subjHand[4], subjHand[0],
                                     subjHand[1], subjHand[2]);
    if (!s) return false;
    __try {
        knockDown(s, true); // collapse + hold the body on the ground
        MedicalSystem* med = &s->medical;
        med->blood     = 0.0f; // past the point of no return
        med->unconcious = true;
        med->dead      = true; // Character::isDead() reads this -> BODY_DEAD
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// combat_kill bias (NOT a kill): lower the subject's blood so an ongoing REAL melee
// downs it decisively within the test window, without collapsing it ourselves (no
// unconcious/dead set, no ragdoll) - the opponent's hits + bleeding do the takedown,
// so the down edge comes from genuine combat. Idempotent-ish (clamps to >=0). Returns
// true if applied. subjHand is readObjectHand layout.
bool woundSubject(GameWorld* gw, const unsigned int subjHand[5], float blood) {
    (void)gw;
    Character* s = resolveCharByHand(subjHand[3], subjHand[4], subjHand[0],
                                     subjHand[1], subjHand[2]);
    if (!s) return false;
    __try {
        MedicalSystem* med = &s->medical;
        if (blood < 0.0f) blood = 0.0f;
        if (med->blood > blood) med->blood = blood; // only weaken, never heal
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- Combat (Phase 3c, L5) -------------------------------------------------

bool readCombat(Character* c, CombatRead* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->swordState = -1;
    if (!c) return false;
    __try {
        if (g_inCombatModeFn) out->inCombat   = g_inCombatModeFn(c, true, true);
        if (g_inRangedModeFn) out->ranged     = g_inRangedModeFn(c) ? true : false;
        if (g_underMeleeFn)   out->underMelee  = g_underMeleeFn(c) ? true : false;
        if (g_fleeingFn)      out->fleeing     = g_fleeingFn(c) ? true : false;
        // Stance: the sword state distinguishes an ACTIVE attacker (attacking /
        // blocking / pathing in) from one QUEUED by the AttackSlotManager
        // (circling / waiting / hesitating - most of any crowd). Direct member
        // reads (CombatClass, header layout) inside this SEH frame.
        // combatModeActive is the STABLE engaged signal: isInCombatMode()
        // flickers off between slot rotations / combo sections (measured: a
        // crowd hand's streamed task alternated combat/none for the whole
        // fight), and every flicker used to disarm+reset the peer's copy.
        if (g_getCombatClassFn) {
            CombatClass* cc = g_getCombatClassFn(c);
            if (cc) {
                out->modeActive = cc->combatModeActive;
                out->swordState = (int)cc->combatState;
                out->waiting = (cc->combatState == CIRCLE_MENACINGLY ||
                                cc->combatState == WAIT_MENACINGLY ||
                                cc->combatState == HESITATE);
            }
        }
        if (g_getAttackTargetFn) {
            // getAttackTarget returns a hand by value into our buffer (this=RCX,
            // retbuf=RDX). Read its POD fields into readObjectHand layout.
            char hbuf[sizeof(hand) + 16];
            memset(hbuf, 0, sizeof(hbuf));
            hand* th = reinterpret_cast<hand*>(hbuf);
            g_getAttackTargetFn(c, th);
            out->target[0] = (unsigned int)th->type;
            out->target[1] = th->container;
            out->target[2] = th->containerSerial;
            out->target[3] = th->index;
            out->target[4] = th->serial;
            out->hasTarget = (th->index != 0 || th->serial != 0);
        }
        out->valid = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- duel scene (Phase 3c probe) ------------------------------------------------
static unsigned int g_duelHandA[5] = { 0, 0, 0, 0, 0 };
static unsigned int g_duelHandB[5] = { 0, 0, 0, 0, 0 };
static bool         g_haveDuel     = false;
// Fixed baseline anchors (x,y,z,yaw) for the two pinned duelists, latched once at pin
// time so combat_order can hold them peaceful + in range before the live attack order
// (same fixed-anchor reasoning as g_downAnchor: leader-relative parking causes a
// parked-body-pushes-leader feedback loop).
static float g_duelAnchorA[4] = { 0, 0, 0, 0 };
static float g_duelAnchorB[4] = { 0, 0, 0, 0 };
static bool  g_haveDuelAnchors = false;

// Order 'attacker' to focus-melee 'target' (UNPROVOKED so it engages regardless of
// faction relations). addGoal hands the intent to the NPC's own AI, like the work
// goal, so it is NOT a squad/player-order recruit. SEH-guarded.
static bool orderMeleeAttack(Character* attacker, Character* target) {
    if (!attacker || !target || !g_addGoalFn) return false;
    __try {
        if (g_clearGoalsFn) g_clearGoalsFn(attacker);
        g_addGoalFn(attacker, (int)UNPROVOKED_FOCUSED_MELEE_ATTACK,
                    static_cast<RootObject*>(target));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Route the SAME attack through the player-ORDER path with clear=true. A driven
// copy that was seat-INJECTED (applyTaskOrder) holds a player order at the stool,
// and player orders OUTRANK AI goals - orderMeleeAttack's addGoal is dead on
// arrival (run 014713: the pre-seated window-A striker re-ordered 15x with
// localFight=0 the whole window; only never-seated restrike picks ever engaged).
// clear=true flushes the seat order and makes the attack the new current order.
// dest=NULL mirrors the player's own right-click attack (no destination building).
static bool orderMeleeAttackViaOrder(Character* attacker, Character* target) {
    if (!attacker || !target || !g_addOrderFn) return false;
    __try {
        Ogre::Vector3 loc = static_cast<RootObject*>(target)->getPosition();
        g_addOrderFn(attacker, 0, (int)UNPROVOKED_FOCUSED_MELEE_ATTACK,
                     static_cast<RootObject*>(target), /*shift*/false,
                     /*clear*/true, &loc);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Spawn two non-squad NPCs in front of the leader from the SAME nearby faction (so
// they are PEACEFUL on spawn - mutually non-hostile) and detach each into its own
// platoon for a stable, save-survivable hand. NO attack is issued here: this is the
// neutral baseline both a bake ('duel1') and the live combat_order test start from.
// startDuel()/rearmDuelScene() trigger the fight at runtime so the join sees a live
// peaceful->fighting transition (not just a baked load state). Returns true if both
// spawned and their hands were read.
bool setupDuelScene(GameWorld* gw) {
    Faction* fac = findNearbyNonPlayerFaction(gw);
    coop::logLine(fac ? "SETUP(duel): using nearby non-player faction (peaceful world NPCs)"
                      : "SETUP(duel): NO nearby non-player faction - falling back to "
                        "player faction (duelists WILL be squad members; load near a town)");
    Character* a = fac ? spawnCharInFaction(gw, 5.0f, -2.0f, fac)
                       : spawnNpcInFront(gw, 5.0f, -2.0f);
    Character* b = fac ? spawnCharInFaction(gw, 5.0f,  2.0f, fac)
                       : spawnNpcInFront(gw, 5.0f,  2.0f);
    if (!a || !b) { coop::logLine("SETUP(duel): duelist spawn FAILED"); return false; }
    detachFromTownAI(a);
    detachFromTownAI(b);
    g_haveDuel = readObjectHand(static_cast<RootObject*>(a), g_duelHandA) &&
                 readObjectHand(static_cast<RootObject*>(b), g_duelHandB);
    {
        char m[200];
        _snprintf(m, sizeof(m) - 1,
                  "SETUP(duel): spawned PEACEFUL A=%u,%u B=%u,%u (no attack issued)",
                  g_duelHandA[3], g_duelHandA[4], g_duelHandB[3], g_duelHandB[4]);
        m[sizeof(m) - 1] = '\0'; coop::logLine(m);
    }
    return g_haveDuel;
}

// combat_order LIVE-transition pin (Stage 3c): re-find the two baked duelists after a
// reload (the spawn-time globals are gone in a fresh process) by picking the two
// non-squad NPCs nearest the leader, and stash their hands into the duel globals so
// startDuel/rearmDuelScene/holdDuelistsPeaceful all operate on them by hand. Also
// latches a fixed front-left / front-right anchor for each so the baseline hold keeps
// them peaceful + in capture range. Returns true if TWO distinct subjects were pinned.
bool pickDuelSubjects(GameWorld* gw, unsigned int outA[5], unsigned int outB[5]) {
    if (!g_getCharsFn || !gw || !gw->player ||
        gw->player->playerCharacters.size() == 0) return false;
    Character* best1 = 0; Character* best2 = 0;
    __try {
        Ogre::Vector3 center = gw->player->playerCharacters[0]->getPosition();
        g_npcQuery.clear();
        g_getCharsFn(gw, &g_npcQuery, &center, 30.0f, 30.0f, 30.0f, 64, 64, 0);
        float d1 = 1e18f, d2 = 1e18f;
        for (unsigned int i = 0; i < g_npcQuery.size(); ++i) {
            RootObject* o = g_npcQuery[i];
            if (!o || isPlayerSquad(gw, o)) continue;
            Ogre::Vector3 p = o->getPosition();
            float dx = p.x - center.x, dz = p.z - center.z;
            float dd = dx * dx + dz * dz;
            if (dd < d1) { d2 = d1; best2 = best1; d1 = dd; best1 = static_cast<Character*>(o); }
            else if (dd < d2) { d2 = dd; best2 = static_cast<Character*>(o); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (!best1 || !best2 || best1 == best2) return false;
    if (!readObjectHand(static_cast<RootObject*>(best1), g_duelHandA)) return false;
    if (!readObjectHand(static_cast<RootObject*>(best2), g_duelHandB)) return false;
    for (int i = 0; i < 5; ++i) { outA[i] = g_duelHandA[i]; outB[i] = g_duelHandB[i]; }
    g_haveDuel = true;
    Ogre::Vector3 pa, pb; float ya = 0.0f, yb = 0.0f;
    if (leaderAnchor(gw, 6.0f, -1.5f, &pa, &ya) &&
        leaderAnchor(gw, 6.0f,  1.5f, &pb, &yb)) {
        g_duelAnchorA[0] = pa.x; g_duelAnchorA[1] = pa.y; g_duelAnchorA[2] = pa.z; g_duelAnchorA[3] = ya;
        g_duelAnchorB[0] = pb.x; g_duelAnchorB[1] = pb.y; g_duelAnchorB[2] = pb.z; g_duelAnchorB[3] = yb;
        g_haveDuelAnchors = true;
    }
    return true;
}

// Hold both pinned duelists at their latched anchors + clear goals each baseline tick,
// so they stay peaceful and in capture range until the live attack order. Returns true
// if held. No-op (false) until pickDuelSubjects has latched the anchors.
bool holdDuelistsPeaceful(GameWorld* gw) {
    (void)gw;
    if (!g_haveDuel || !g_haveDuelAnchors) return false;
    Character* a = resolveCharByHand(g_duelHandA[3], g_duelHandA[4], g_duelHandA[0],
                                     g_duelHandA[1], g_duelHandA[2]);
    Character* b = resolveCharByHand(g_duelHandB[3], g_duelHandB[4], g_duelHandB[0],
                                     g_duelHandB[1], g_duelHandB[2]);
    bool any = false;
    if (a) { clearGoals(a); park(a, g_duelAnchorA[0], g_duelAnchorA[1], g_duelAnchorA[2], g_duelAnchorA[3]); any = true; }
    if (b) { clearGoals(b); park(b, g_duelAnchorB[0], g_duelAnchorB[1], g_duelAnchorB[2], g_duelAnchorB[3]); any = true; }
    return any;
}

// Trigger the fight between the two pinned duelists (order each to melee the other).
// Used by the live probe/test to start combat AFTER a peaceful baseline. Returns the
// number of attack orders issued.
int startDuel(GameWorld* gw) {
    (void)gw;
    if (!g_haveDuel) return 0;
    Character* a = resolveCharByHand(g_duelHandA[3], g_duelHandA[4], g_duelHandA[0],
                                     g_duelHandA[1], g_duelHandA[2]);
    Character* b = resolveCharByHand(g_duelHandB[3], g_duelHandB[4], g_duelHandB[0],
                                     g_duelHandB[1], g_duelHandB[2]);
    if (!a || !b) return 0;
    int n = 0;
    if (orderMeleeAttack(a, b)) ++n;
    if (orderMeleeAttack(b, a)) ++n;
    return n;
}

int rearmDuelScene(GameWorld* gw) {
    (void)gw;
    if (!g_haveDuel) return -1;
    Character* a = resolveCharByHand(g_duelHandA[3], g_duelHandA[4], g_duelHandA[0],
                                     g_duelHandA[1], g_duelHandA[2]);
    Character* b = resolveCharByHand(g_duelHandB[3], g_duelHandB[4], g_duelHandB[0],
                                     g_duelHandB[1], g_duelHandB[2]);
    if (!a || !b) return -1;
    int n = 0;
    // Only re-issue to a duelist that has DISENGAGED (no combat mode), so we don't
    // thrash the AI of one that is already actively fighting.
    CombatRead ca, cb;
    if (readCombat(a, &ca) && !ca.inCombat) { if (orderMeleeAttack(a, b)) ++n; }
    if (readCombat(b, &cb) && !cb.inCombat) { if (orderMeleeAttack(b, a)) ++n; }
    return n;
}

bool getDuelHands(unsigned int outA[5], unsigned int outB[5]) {
    if (!g_haveDuel) return false;
    for (int i = 0; i < 5; ++i) { outA[i] = g_duelHandA[i]; outB[i] = g_duelHandB[i]; }
    return true;
}

int logDuelCombat(GameWorld* gw) {
    (void)gw;
    if (!g_haveDuel) return 0;
    const unsigned int* hands[2] = { g_duelHandA, g_duelHandB };
    const char* names[2] = { "A", "B" };
    int n = 0;
    for (int k = 0; k < 2; ++k) {
        Character* c = resolveCharByHand(hands[k][3], hands[k][4], hands[k][0],
                                         hands[k][1], hands[k][2]);
        if (!c) continue;
        CombatRead cr;
        if (!readCombat(c, &cr)) continue;
        char b[200];
        _snprintf(b, sizeof(b) - 1,
            "COMBAT %s hand=%u,%u inCombat=%d ranged=%d underMelee=%d fleeing=%d "
            "hasTarget=%d target=%u,%u",
            names[k], hands[k][3], hands[k][4],
            cr.inCombat ? 1 : 0, cr.ranged ? 1 : 0, cr.underMelee ? 1 : 0,
            cr.fleeing ? 1 : 0, cr.hasTarget ? 1 : 0, cr.target[3], cr.target[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        ++n;
    }
    return n;
}

// 'craft' setup scene: spawn a save-stable work fixture + a NON-squad world NPC,
// then give the NPC a persistent AI GOAL to work it (NOT a player order, which
// would recruit it into the squad and bypass the host-authoritative world-NPC
// path). Its own AI then operates the station, so the HOST captures the natural
// work task and the join reproduces it once the save is baked. Task selection
// lives here where the TaskType enum is in scope. Returns true if a fixture spawned.
bool setupCraftScene(GameWorld* gw) {
    // Reloading a baked scene (craft1): a work fixture already exists nearby. Don't
    // spawn a duplicate - just re-arm the worker's goal (the goal didn't persist).
    {
        int rt = USE_TRAINING_DUMMY;
        if (findWorkFixtureNear(gw, &rt)) {
            coop::logLine("SETUP: existing work fixture found - re-arming (no spawn)");
            return rearmCraftScene(gw);
        }
    }

    // Pick the task from the chosen fixture name: a dummy is "used", everything
    // else is "operated". (findMachineTemplate prioritises a training dummy.)
    int task = OPERATE_MACHINERY;
    {
        GameData* tmpl = findMachineTemplate(gw);
        if (tmpl && (ciContains(tmpl->name.c_str(), "dummy") ||
                     ciContains(tmpl->name.c_str(), "bag")))
            task = USE_TRAINING_DUMMY;
    }
    // Borrow a live non-player faction from a nearby NPC so the worker is NOT a
    // squad member and the fixture has a legitimate owner (an ownerless fixture in
    // the player faction enlists the worker into the squad - the bug we observed).
    Faction* fac = findNearbyNonPlayerFaction(gw);
    coop::logLine(fac ? "SETUP: using nearby non-player faction (world NPC owner)"
                      : "SETUP: NO nearby non-player faction - falling back to player "
                        "faction (worker WILL be a squad member; load near a town)");

    RootObject* mach = 0;
    bool ok = spawnMachineInFront(gw, 6.0f, 0.0f, fac, &mach);
    if (ok && mach) {
        unsigned int h[5];
        if (readObjectHand(mach, h)) {
            char b[160];
            _snprintf(b, sizeof(b) - 1, "SETUP: spawned machine hand=%u,%u,%u,%u,%u task=%d owned=%d",
                      h[3], h[4], h[0], h[1], h[2], task, fac ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        } else coop::logLine("SETUP: spawned machine (hand unread)");
    } else {
        coop::logLine("SETUP: machine spawn FAILED (no machine template or createBuilding faulted)");
    }

    // Spawn the worker in the borrowed faction (non-squad) when we have one; else
    // fall back to the player-faction spawn so the scene still produces something.
    Character* npc = fac ? spawnCharInFaction(gw, 4.0f, 1.0f, fac)
                         : spawnNpcInFront(gw, 4.0f, 1.0f);
    if (npc && mach) {
        // Detach the worker into its OWN platoon BEFORE baking, so its hand is fixed
        // by the save and is IDENTICAL on both clients. The join's runtime quieting
        // also detaches reproducible-pose NPCs (separateIntoMyOwnSquad); a worker
        // that SHARES a faction platoon gets re-containered there (hand 14->1,..),
        // breaking the host<->join hand pairing the pose oracle relies on. Baking it
        // pre-separated makes that runtime detach a no-op, so the hand stays stable.
        bool det = detachFromTownAI(npc);
        coop::logLine(det ? "SETUP: worker detached into own platoon (stable hand)"
                          : "SETUP: worker detach skipped/failed");
        // GOAL, not player order: addGoal hands intent to the NPC's own AI without
        // recruiting it (a player order would pull it into the squad).
        bool issued = goalWorkAt(npc, mach, task);
        coop::logLine(issued ? "SETUP: gave NPC a work GOAL at machine"
                             : "SETUP: work goal FAILED (no addGoal fn)");
        // Diagnostic: confirm the worker is NOT in the player squad (the screenshot
        // gate's machine-readable counterpart). Enlistment can be deferred a tick,
        // so this is an early indicator; the host-log [taskkey] world-capture + the
        // squad-bar screenshot are the authoritative checks.
        coop::logLine(isPlayerSquad(gw, static_cast<RootObject*>(npc))
                          ? "SETUP: WARN worker IS in player squad (enlisted)"
                          : "SETUP: worker is NON-squad (good)");
    } else {
        coop::logLine(npc ? "SETUP: no machine to assign NPC onto"
                          : "SETUP: NPC spawn FAILED");
    }
    return ok && mach != 0;
}

// Drop a Character into (on=true) or out of (on=false) full-body ragdoll. The host
// uses this to manufacture a "down" subject; the join uses it to reproduce one.
// SEH-guarded; no-op if the engine fn didn't resolve.
bool knockDown(Character* c, bool on) {
    if (!c) return false;
    __try {
        if (on) {
            // Prefer a real knockout: it collapses the body AND the medical KO timer
            // suppresses the get-up AI, so a loaded world NPC stays cleanly down (a
            // bare ragdoll it just fights out of - the twitch we saw). Top the timer
            // well past the re-arm interval. Fall back to ragdoll if unresolved.
            if (g_knockoutFn || g_knockoutForceFn) {
                MedicalSystem* med = &c->medical;
                if (g_knockoutFn)      g_knockoutFn(med, 1.0f);
                if (g_knockoutForceFn) g_knockoutForceFn(med, 8.0f);
                return true;
            }
            if (g_ragdollModeFn) { g_ragdollModeFn(c, true, RagdollPart::WHOLE); return true; }
            return false;
        }
        // Wake/stand: clear the forced KO timer and release any ragdoll.
        if (g_knockoutForceFn) g_knockoutForceFn(&c->medical, 0.0f);
        if (g_ragdollModeFn)   g_ragdollModeFn(c, false, RagdollPart::WHOLE);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Maintain an already-down body WITHOUT re-triggering the collapse. knockDown()
// calls knockout(1.0), which re-initiates the ragdoll fall every time - calling
// that each tick produces the visible get-up/flop/ragdoll-spike flicker. The
// real cause of the get-up is the medical KO timer lapsing: once it hits 0 the
// wake AI stands the body, the replicator notices (too late), and re-collapses
// it. So instead top the force timer EVERY tick: the timer never reaches 0, the
// wake AI never fires, and the body stays cleanly down with no re-collapse.
bool holdDown(Character* c) {
    if (!c) return false;
    __try {
        if (g_knockoutForceFn) { g_knockoutForceFn(&c->medical, 8.0f); return true; }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The down subject pinned at bake time (readObjectHand layout), so the host can keep
// re-applying ragdoll to THAT exact NPC (a healthy body fights to stand back up).
static unsigned int g_downHand[5] = { 0, 0, 0, 0, 0 };
static bool         g_haveDownHand = false;

// 'down' setup scene: spawn a NON-squad world NPC and drop it into ragdoll, so the
// host streams bodyState != 0 and (later) the join reproduces a body on the ground.
// Mirrors the craft scene's faction borrow + pre-detach so the subject's hand is
// stable across save/reload and identical on both clients. Returns true if spawned.
bool setupDownScene(GameWorld* gw) {
    Faction* fac = findNearbyNonPlayerFaction(gw);
    coop::logLine(fac ? "SETUP(down): using nearby non-player faction (world NPC)"
                      : "SETUP(down): NO nearby non-player faction - falling back to "
                        "player faction (subject WILL be a squad member; load near a town)");
    Character* npc = fac ? spawnCharInFaction(gw, 4.0f, 0.0f, fac)
                         : spawnNpcInFront(gw, 4.0f, 0.0f);
    if (!npc) { coop::logLine("SETUP(down): NPC spawn FAILED"); return false; }
    bool det = detachFromTownAI(npc);
    coop::logLine(det ? "SETUP(down): subject detached into own platoon (stable hand)"
                      : "SETUP(down): subject detach skipped/failed");
    g_haveDownHand = readObjectHand(static_cast<RootObject*>(npc), g_downHand);
    if (g_haveDownHand) {
        char b[160];
        _snprintf(b, sizeof(b) - 1, "SETUP(down): subject hand=%u,%u,%u,%u,%u",
                  g_downHand[3], g_downHand[4], g_downHand[0], g_downHand[1], g_downHand[2]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    bool kd = knockDown(npc, true);
    coop::logLine(kd ? "SETUP(down): subject knocked into ragdoll"
                     : "SETUP(down): knockDown FAILED (no ragdollMode fn)");
    return true;
}

// 'squad' setup scene (Phase 3.5 bake): build a SECOND player squad tab so the
// bidirectional ownership partition (host owns tab 0, join owns tab 1) has two tabs
// to split. A Kenshi squad tab is a Platoon, and a player member's tab identity is
// its hand CONTAINER. We recruit two world bodies into the player squad, then
// separateIntoMyOwnSquad ONE of them into its OWN player platoon (a distinct
// container = a distinct tab). The faction stays the player's, so the separated
// body is still a controllable squad member - just in tab 2. The user then SAVEs
// (e.g. 'squad1') and both clients load it. The member dump makes the bake
// machine-verifiable: 2+ distinct containers == 2+ squad tabs. Returns true if at
// least one recruit took.
bool setupSquadScene(GameWorld* gw) {
    if (!gw || !gw->player) { coop::logLine("SETUP(squad): no player interface"); return false; }
    PlayerInterface* pl = gw->player;

    Character* a = spawnNpcInFront(gw, 4.0f, -1.5f);
    Character* b = spawnNpcInFront(gw, 4.0f,  1.5f);
    bool ra = a && recruitNpc(gw, a);
    bool rb = b && recruitNpc(gw, b);
    coop::logLine(ra ? "SETUP(squad): recruited A into player squad"
                     : "SETUP(squad): recruit A FAILED");
    coop::logLine(rb ? "SETUP(squad): recruited B into player squad"
                     : "SETUP(squad): recruit B FAILED");
    // Separate B into its OWN player platoon -> a SECOND squad tab (distinct container).
    bool sep = b && detachFromTownAI(b);
    coop::logLine(sep ? "SETUP(squad): separated B into its own platoon (tab 2)"
                      : "SETUP(squad): separate B FAILED");

    // Dump the tab partition (distinct hand-containers across player chars) so the
    // bake is verifiable from the host log: 2+ distinct containers == 2+ squad tabs.
    __try {
        unsigned int n = pl->playerCharacters.size();
        char hdr[96]; _snprintf(hdr, sizeof(hdr) - 1, "SETUP(squad): playerChars=%u", n);
        hdr[sizeof(hdr) - 1] = '\0'; coop::logLine(hdr);
        for (unsigned int i = 0; i < n; ++i) {
            Character* c = pl->playerCharacters[i]; if (!c) continue;
            unsigned int h[5];
            if (readObjectHand(static_cast<RootObject*>(c), h)) {
                char b2[160];
                _snprintf(b2, sizeof(b2) - 1,
                    "SETUP(squad): member[%u] idx=%u,%u container(tab)=%u,%u",
                    i, h[3], h[4], h[1], h[2]);
                b2[sizeof(b2) - 1] = '\0'; coop::logLine(b2);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("SETUP(squad): member dump faulted");
    }
    return ra || rb;
}

// Keep down bodies down. A healthy ragdolled body recovers and stands back up, and
// ragdoll state does not survive save/load, so the host re-applies ragdoll on an
// interval. Rather than guess WHICH nearby NPC is "the subject" (the pin is empty
// after a reload, and the nearest-NPC heuristic mis-fired when the donor/a re-spawn
// sat at the same spot), we simply re-knock EVERY non-squad NPC within a modest
// radius of the leader. The baked subject is always covered, and any neighbour that
// goes down is a world NPC present in the save on BOTH clients, so the join
// reproduces it too. Re-applying to an already-ragdolled body is harmless. Returns
// the number of bodies (re-)knocked, or -1 if the query is unavailable.
int rearmDownScene(GameWorld* gw) {
    if (!g_getCharsFn || !gw || !gw->player ||
        gw->player->playerCharacters.size() == 0) return -1;
    const float R = 30.0f; // horizontal reach for "down bodies in this scene"
    int n = 0;
    __try {
        Ogre::Vector3 center = gw->player->playerCharacters[0]->getPosition();
        g_npcQuery.clear();
        g_getCharsFn(gw, &g_npcQuery, &center, R, R, R, 64, 64, 0);
        for (unsigned int i = 0; i < g_npcQuery.size(); ++i) {
            RootObject* o = g_npcQuery[i];
            if (!o || isPlayerSquad(gw, o)) continue;
            if (knockDown(static_cast<Character*>(o), true)) ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return n;
    }
    return n;
}

// SEH-guarded bone read. MUST live in its own function with only POD locals: a
// __try cannot coexist with C++ objects that need unwinding (the std::string for
// the bone name is therefore owned by the caller and passed by pointer). Returns
// the pelvis-bone height above the logical root, or false on fault.
static bool readPelvisDelta(Character* c, const std::string* boneName, float* outH) {
    __try {
        Ogre::Vector3 ground = c->getPosition();
        Ogre::Vector3 bone(0, 0, 0);
        g_getBoneWorldFn(c, &bone, boneName);   // this=RCX, retbuf=RDX, name=R8
        *outH = bone.y - ground.y;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool readCharBodyFlags(Character* c, int* idle, int* crouched, int* task) {
    __try {
        CharBody* b = c->body;
        if (!b) return false;
        if (idle && g_isIdleFn)         *idle     = g_isIdleFn(b) ? 1 : 0;
        if (crouched && g_isCrouchedFn) *crouched = g_isCrouchedFn(b) ? 1 : 0;
        if (task && g_taskerKeyFn) {
            Tasker* t = b->currentAction;
            if (t) *task = g_taskerKeyFn(t);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Read the host's body-state bit-flags off a rendered Character (BODY_* in Wire.h).
// SEH-guarded, POD-only: a fault returns 0 (treated as upright). 0 = normal/upright.
unsigned short readBodyState(Character* c) {
    if (!c) return 0;
    unsigned short s = 0;
    __try {
        if (g_isDownFn    && g_isDownFn(c))    s |= BODY_DOWN;
        if (g_isRagdollFn && g_isRagdollFn(c)) s |= BODY_RAGDOLL;
        if (g_isDeadCharFn && g_isDeadCharFn(c)) s |= BODY_DEAD;
        if (g_isCrawlFn   && g_isCrawlFn(c))   s |= BODY_CRAWL;
        if (g_isBeingCarriedFn && g_isBeingCarriedFn(c)) s |= BODY_CARRIED;
        // Furniture occupancy (protocol 19): direct member read (0x2F8).
        if (c->inSomething == IN_BED)         s |= BODY_IN_BED;
        else if (c->inSomething == IN_PRISON) s |= BODY_IN_CAGE;
        // Chained/pole prisoner (protocol 41): direct member read (0x320). A
        // captive shackled to a prisoner pole is chained, not caged.
        if (c->isChained) s |= BODY_CHAINED;
        // Stealth (protocol 20): the mode bool exactly (0xD4), NOT the
        // crawl-inclusive isStealthModeOrCrawling that feeds BODY_CRAWL.
        if (c->stealthMode) s |= BODY_SNEAK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return s;
}

bool readPoseState(Character* c, float* pelvis, int* idle, int* crouched, int* task) {
    if (pelvis) *pelvis = -1.0f;
    if (idle) *idle = -1; if (crouched) *crouched = -1; if (task) *task = TASK_NONE;
    if (!c) return false;
    bool any = false;
    // Safe reads (crouch/idle/task) - isolated so a pelvis fault cannot wipe them.
    if (readCharBodyFlags(c, idle, crouched, task)) any = true;
    // Pelvis bone height above root - the animated-skeleton signal that drops when
    // seated. The std::string lives HERE (outside the SEH frame in the helper).
    if (g_getBoneWorldFn && pelvis) {
        std::string boneName("Bip01 Pelvis");
        float h = 0.0f;
        if (readPelvisDelta(c, &boneName, &h)) {
            // pelvis-above-root, in Kenshi units. Magnitude varies a LOT by race
            // (Greenlander/Shek/Hiver/Skeleton heights differ), so the oracle
            // compares the SAME NPC host-vs-join rather than an absolute seat line.
            // Keep any plausibly-real value; -99 flags a clearly-bad read.
            *pelvis = (h > 0.5f && h < 25.0f) ? h : -99.0f;
            any = true;
        }
    }
    return any;
}

// The fixture-acceptance policy (SEAT_MATCH_DIST / poseFixtureAcceptedSq) lives in
// core/WorkPose.h so the no-game prototest layer can lock it. See that header: seats
// are distance-gated (they mis-resolve to a wrong prop), work fixtures are trusted
// by their reliable cross-client hand (a large mine's origin can sit 50-100 m from
// its operate spot, so distance-gating wrongly rejects a correct mine).

// True if a reproduced task pins the body at a WORK fixture (mine/machine/dummy) -
// these are trusted by identity, not distance-gated (see core/WorkPose.h).
static bool isWorkFixtureTask(int task) {
    switch (task) {
        case OPERATE_MACHINERY:
        case OPERATE_AUTOMATIC_MACHINERY:
        case USE_TRAINING_DUMMY:
        case PRETEND_TO_OPERATE_MACHINERY:
            return true;
        default:
            return false;
    }
}

// True if a reproduced task is a medic/first-aid action (2026-07-15 medic sync).
// The subject is the PATIENT (a character), not a building: its hand resolves
// cross-client so it is identity-trusted (not distance-gated), and it is ordered
// with dest=NULL (a patient is not a destination building - see applyTaskOrder).
static bool isMedicTask(int task) {
    switch (task) {
        case FIRST_AID_ORDER:
        case JOB_MEDIC:
        case FIRST_AID_ROBOT:
        case JOB_REPAIR_ROBOT:
            return true;
        default:
            return false;
    }
}

int applyTask(Character* c, const EntityState& e) {
    if (e.task == TASK_NONE) return 0;
    if (!c || !g_handGetRootFn || !g_handCtorFn) return 0;
    if (!g_addGoalFn && !g_setActionFn) return 0;
    __try {
        // Build the subject hand and resolve it to a local fixture.
        char buf[sizeof(hand) + 16];
        memset(buf, 0, sizeof(buf));
        hand* h = reinterpret_cast<hand*>(buf);
        g_handCtorFn(h, e.sIndex, e.sSerial, (itemType)e.sType,
                     e.sContainer, e.sContainerSerial);
        RootObject* target = g_handGetRootFn(h);
        // DIAGNOSTIC: where does THIS (join) client resolve the same seat handle?
        // Compare the "[seatres] JOIN" seatpos to the host's for the same seat to
        // see whether furniture identity correlates across clients.
        logSeatResolveOnce("JOIN", (int)e.task, e.hIndex, e.hSerial, e.sIndex, e.sSerial,
                           e.sType, e.sContainer, e.sContainerSerial, e.x, e.y, e.z);
        if (!target) return 1; // fixture not loaded here -> caller idle-parks
        // PROXIMITY GATE: cross-client furniture identity is NOT reliable - the same
        // subject handle frequently resolves to a DIFFERENT stool tens of metres away
        // on the join. Issuing the seat goal then walks the body ~50 m to that wrong
        // stool while our position-drive teleports it back => the "walking in place,
        // repeatedly teleported" loop. So verify the resolved fixture is actually at
        // the host's streamed transform before committing; a far match is rejected so
        // the caller idle-parks in place (no walk, no loop) instead.
        {
            Ogre::Vector3 sp = target->getPosition(); // virtual: safe direct call
            float dx = sp.x - e.x, dz = sp.z - e.z;
            // Seats: reject a far (mis-resolved) prop. Work fixtures + medic (patient)
            // subjects: trusted by identity - a large mine's origin sits far from its
            // operate spot, and a patient's driven copy may be mid-motion.
            bool identityTrusted = isWorkFixtureTask((int)e.task) || isMedicTask((int)e.task);
            if (!coop::poseFixtureAcceptedSq(identityTrusted, dx * dx + dz * dz))
                return 3; // fixture resolved but it's the WRONG (far) one -> park
        }
        // Clear any local intent first, then issue a PERSISTENT AI goal to perform
        // the task AT the resolved fixture. addGoal is the most stable mechanism
        // tried: it sits ~88% of NPCs correctly. SIT_AROUND does not hard-pin our
        // target seat (it re-runs a local seat search), so a few NPCs still settle
        // on a different free stool - that residual is the open problem. (Tried and
        // rejected: setCurrentAction-primary made MORE NPCs wander; snap-then-pose
        // crashed and still wandered.)
        if (g_clearGoalsFn) g_clearGoalsFn(c);
        if (g_addGoalFn) {
            g_addGoalFn(c, (int)e.task, target);
        } else if (g_setActionFn && c->body) {
            g_setActionFn(c->body, (int)e.task, target);
        }
        return 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// I9: detach an NPC from its town/faction so the town-AI stops auto-assigning it
// tasks - a "squad-like" inert puppet whose only intent source is the host order.
// separateIntoMyOwnSquad(true) ejects it into its own platoon. SEH-guarded; the
// caller invokes this once per driven NPC. Returns true if the call was made.
bool detachFromTownAI(Character* c) {
    if (!c || !g_separateSquadFn) return false;
    __try {
        g_separateSquadFn(c, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// I10: end the body's current action so a suspended node-stander stops executing
// its residual walk-to-node (the "walk in place" when we hold it at the host
// transform) and drops to idle. SEH-guarded; returns true if the call was made.
bool endAction(Character* c) {
    if (!c || !g_endActionFn || !c->body) return false;
    __try {
        g_endActionFn(c->body);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// I9: reproduce a rest pose via the PLAYER-ORDER path instead of the autonomous
// SIT_AROUND goal. addOrder/addJob carry an explicit world LOCATION, so the body
// is ordered to THIS fixture at THIS spot (what a player click-to-sit issues) -
// the engine does not re-run its own seat search and wander to a different stool.
// Same resolution + proximity gate + return codes as applyTask.
int applyTaskOrder(Character* c, const EntityState& e) {
    if (e.task == TASK_NONE) return 0;
    if (!c || !g_handGetRootFn || !g_handCtorFn) return 0;
    if (!g_addOrderFn && !g_addJobFn) return 0;
    __try {
        char buf[sizeof(hand) + 16];
        memset(buf, 0, sizeof(buf));
        hand* h = reinterpret_cast<hand*>(buf);
        g_handCtorFn(h, e.sIndex, e.sSerial, (itemType)e.sType,
                     e.sContainer, e.sContainerSerial);
        RootObject* target = g_handGetRootFn(h);
        if (!target) return 1; // fixture not loaded here -> caller idle-parks
        Ogre::Vector3 loc = target->getPosition(); // virtual: safe direct call
        bool medic = isMedicTask((int)e.task);
        {
            float dx = loc.x - e.x, dz = loc.z - e.z;
            // Seats: reject a far (mis-resolved) prop. Work fixtures + medic (patient)
            // subjects: trusted by identity - a large mine's origin sits far from its
            // operate spot, and a patient's driven copy may be mid-motion.
            if (!coop::poseFixtureAcceptedSq(isWorkFixtureTask((int)e.task) || medic,
                                             dx * dx + dz * dz))
                return 3; // resolved the WRONG (far) fixture -> caller parks in place
        }
        if (g_clearGoalsFn) g_clearGoalsFn(c);
        // Player-order to the EXACT fixture + location. A seat/machine IS a Building,
        // so it doubles as the order destination. A medic subject is the PATIENT (a
        // character, not a building), so order it with dest=NULL - the same form a
        // player's right-click First Aid issues (mirrors orderMeleeAttackViaOrder).
        // clear=true drops prior orders.
        if (g_addOrderFn) {
            Building* dest = medic ? 0 : reinterpret_cast<Building*>(target);
            g_addOrderFn(c, dest, (int)e.task, target, /*shift*/false,
                         /*clear*/true, &loc);
        } else if (g_addJobFn) {
            g_addJobFn(c, (int)e.task, target, /*shift*/false,
                       /*addDontClear*/false, &loc);
        }
        return 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// Stage 3c (join-side): reproduce a streamed combat intent. e.task is a combat
// stance (TASK_COMBAT_MELEE or TASK_COMBAT_WAIT) and the subject hand is the attack
// target; resolve it to a local Character and order THIS body to focus-melee it
// (UNPROVOKED so it engages regardless of faction). The join's own engine then
// animates the fight (draw/swing/footwork) - we replicate the CAUSE, not the
// animation. A WAITING copy gets the same goal (it enters combat stance and menaces
// the target); the difference is the CALLER never timer-re-issues it. Returns 2
// ordered / 1 target not loaded here / 0 no-op / -1 fault. orderMeleeAttack clears
// prior goals so a re-issue cleanly re-targets. breakOrder additionally flushes a
// committed player order (seat inject) via the order-path attack - without it a
// seated copy ignores the goal (see the helper).
int applyCombat(Character* c, const EntityState& e, bool breakOrder) {
    if (!c || !coop::taskIsCombat(e.task)) return 0;
    Character* target = resolveCharByHand(e.sIndex, e.sSerial, e.sType,
                                          e.sContainer, e.sContainerSerial);
    if (!target) return 1; // opponent not loaded on this client -> caller just holds
    if (breakOrder && !orderMeleeAttackViaOrder(c, target)) return -1;
    return orderMeleeAttack(c, target) ? 2 : -1;
}

// Escalation past applyCombat: Character::attackTarget is the engine AI's own
// commit-an-attack entry. The goal/order paths above are ACCEPTED but then
// dropped by the copy's running AI when the victim is a locally player-owned
// body of a non-hostile faction (world_parity camp: host Holy Sentinels beat
// the escaped-prisoner PC, join sentinels idle at task 65535 through every
// reissue). attackTarget skips that re-decision and starts the fight directly.
int forceAttack(Character* c, const EntityState& e) {
    if (!c || !coop::taskIsCombat(e.task)) return 0;
    if (!g_attackTargetFn) return 0;
    Character* target = resolveCharByHand(e.sIndex, e.sSerial, e.sType,
                                          e.sContainer, e.sContainerSerial);
    if (!target) return 1;
    __try {
        g_attackTargetFn(c, target);
        return 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// ---- Player-combat / medical validation primitives (spike 21 field map) ----

bool readMedical(Character* c, MedicalRead* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < 4; ++i) {
        out->limbFlesh[i] = -1.0f; out->limbBand[i] = -1.0f; out->limbMax[i] = -1.0f;
        out->limbState[i] = 0xFF;
    }
    out->hunger = -1.0f; out->fed = -1.0f; out->dazed = -1.0f;
    if (!c) return false;
    __try {
        MedicalSystem* med = &c->medical;
        out->blood       = med->blood;
        out->bleedRate   = med->currentBleedRate;
        out->unconscious = med->unconcious;
        out->dead        = med->dead;
        // Protocol 29: the hunger scalars ride the same snapshot; dazedOrAlert
        // is read for probe diagnostics only (unlabeled semantics).
        out->hunger = med->hunger;
        out->fed    = med->fed;
        out->dazed  = med->dazedOrAlert;
        // Limb order = RobotLimbs::Limb (LEFT_ARM, RIGHT_ARM, LEFT_LEG, RIGHT_LEG).
        MedicalSystem::HealthPartStatus* parts[4] = {
            med->leftArm, med->rightArm, med->leftLeg, med->rightLeg
        };
        for (int i = 0; i < 4; ++i) {
            if (!parts[i]) continue;
            out->limbFlesh[i] = parts[i]->flesh;
            out->limbBand[i]  = parts[i]->bandaging;
            out->limbMax[i]   = parts[i]->_maxHealth;
        }
        // Protocol 16: the FULL anatomy (head/chest/stomach + limbs), keyed by
        // anatomy index - deterministic across clients on the same save. An
        // amputated limb's HealthPartStatus stays in the lektor (the medical
        // GUI still lists it), so indices remain stable across a limb loss.
        unsigned int n = med->anatomy.count;
        if (n > 12) n = 12;
        for (unsigned int i = 0; i < n; ++i) {
            MedicalSystem::HealthPartStatus* p =
                med->anatomy.stuff ? med->anatomy.stuff[i] : 0;
            MedPartRead& pr = out->parts[i];
            if (!p) { pr.used = false; continue; }
            pr.used      = true;
            pr.partType  = (unsigned char)p->whatAmI;
            pr.side      = (unsigned char)p->side;
            pr.flesh     = p->flesh;
            pr.fleshStun = p->fleshStun;
            pr.bandaging = p->bandaging;
            pr.juryRig   = p->juryRigging;
            pr.maxHealth = p->_maxHealth;
        }
        out->nParts = n;
        // Limb-loss state + robotic replacement template ids (Phase C/D).
        // limbStateOf handles the LAZY robotLimbs allocation: null means no
        // limb was ever lost == ORIGINAL (reporting 0xFF here made every
        // healthy character's limb states "unknown" and broke replication).
        RobotLimbs* rl = med->robotLimbs;
        for (int i = 0; i < 4; ++i) {
            out->limbState[i] = limbStateOf(med, i);
            Item* it = rl ? rl->items[i] : 0;
            if (out->limbState[i] == (unsigned char)LIMB_REPLACED && it) {
                GameData* gd = it->getGameData();
                if (gd) {
                    const char* s = gd->stringID.c_str();
                    strncpy(out->limbSid[i], s ? s : "", sizeof(out->limbSid[i]) - 1);
                }
            }
        }
        out->valid = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readMedicalByHand(const unsigned int hand[5], MedicalRead* out) {
    Character* c = resolveCharByHand(hand[3], hand[4], hand[0], hand[1], hand[2]);
    return readMedical(c, out);
}

bool readCombatByHand(const unsigned int hand[5], CombatRead* out) {
    Character* c = resolveCharByHand(hand[3], hand[4], hand[0], hand[1], hand[2]);
    return readCombat(c, out);
}

bool writeMedical(Character* c, const MedicalRead& in) {
    if (!c) return false;
    __try {
        MedicalSystem* med = &c->medical;
        med->blood            = in.blood;
        med->currentBleedRate = in.bleedRate;
        if (in.nParts > 0) {
            // Protocol 16: full-anatomy write by anatomy index. Only write a
            // slot when the LOCAL part at that index agrees on partType+side
            // (same save + race data -> always agrees; the check is a guard
            // against a modded-race mismatch silently corrupting vitals).
            unsigned int n = in.nParts;
            if (n > 12) n = 12;
            if (n > med->anatomy.count) n = med->anatomy.count;
            for (unsigned int i = 0; i < n; ++i) {
                const MedPartRead& pr = in.parts[i];
                if (!pr.used) continue;
                MedicalSystem::HealthPartStatus* p =
                    med->anatomy.stuff ? med->anatomy.stuff[i] : 0;
                if (!p) continue;
                if ((unsigned char)p->whatAmI != pr.partType ||
                    (unsigned char)p->side    != pr.side) continue;
                // -1 = the OWNER's field is unreadable; never write that.
                if (pr.flesh     >= 0.0f) p->flesh       = pr.flesh;
                if (pr.fleshStun >= 0.0f) p->fleshStun   = pr.fleshStun;
                if (pr.bandaging >= 0.0f) p->bandaging   = pr.bandaging;
                if (pr.juryRig   >= 0.0f) p->juryRigging = pr.juryRig;
            }
        } else {
            // Legacy 4-limb write (pre-16 callers / scaffolds).
            MedicalSystem::HealthPartStatus* parts[4] = {
                med->leftArm, med->rightArm, med->leftLeg, med->rightLeg
            };
            for (int i = 0; i < 4; ++i) {
                if (!parts[i]) continue;
                if (in.limbFlesh[i] >= 0.0f) parts[i]->flesh     = in.limbFlesh[i];
                if (in.limbBand[i]  >= 0.0f) parts[i]->bandaging = in.limbBand[i];
            }
        }
        // Protocol 29: hunger scalars (-1 = not carried / owner unreadable -
        // never write that). dazedOrAlert is deliberately NOT written (its
        // semantics are unconfirmed; it rides the read for diagnostics only).
        if (in.hunger >= 0.0f) med->hunger = in.hunger;
        if (in.fed    >= 0.0f) med->fed    = in.fed;
        // unconscious/dead deliberately NOT written: the reliable KO/death/revive
        // event channel (+ bodyState latches) owns those transitions.
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool writeHungerByHand(const unsigned int hand[5], float hunger, float fed) {
    if (!hand) return false;
    Character* c = resolveCharByHand(hand[3], hand[4], hand[0], hand[1], hand[2]);
    if (!c) return false;
    __try {
        MedicalSystem* med = &c->medical;
        if (hunger >= 0.0f) med->hunger = hunger;
        if (fed    >= 0.0f) med->fed    = fed;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int applyBandageLevels(Character* c, const float band[4]) {
    if (!c || !band) return 0;
    int n = 0;
    __try {
        MedicalSystem* med = &c->medical;
        MedicalSystem::HealthPartStatus* parts[4] = {
            med->leftArm, med->rightArm, med->leftLeg, med->rightLeg
        };
        for (int i = 0; i < 4; ++i) {
            if (!parts[i] || band[i] < 0.0f) continue;
            // Raise-only (idempotent): a stale/duplicate delta can never undo a
            // higher local bandage, and the owner's own first aid always wins ties.
            float lvl = band[i];
            if (lvl > parts[i]->_maxHealth) lvl = parts[i]->_maxHealth;
            if (parts[i]->bandaging < lvl) { parts[i]->bandaging = lvl; ++n; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return n;
}

int applyBandageParts(Character* c, const float band[12]) {
    if (!c || !band) return 0;
    int n = 0;
    __try {
        MedicalSystem* med = &c->medical;
        unsigned int cnt = med->anatomy.count;
        if (cnt > 12) cnt = 12;
        for (unsigned int i = 0; i < cnt; ++i) {
            if (band[i] < 0.0f) continue;
            MedicalSystem::HealthPartStatus* p =
                med->anatomy.stuff ? med->anatomy.stuff[i] : 0;
            if (!p) continue;
            // Raise-only (idempotent) - see applyBandageLevels.
            float lvl = band[i];
            if (lvl > p->_maxHealth) lvl = p->_maxHealth;
            if (p->bandaging < lvl) { p->bandaging = lvl; ++n; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return n;
}

// SEH-guarded: fabricate a robotic-limb item from its LIMB_REPLACEMENT template
// stringID (blank handle - the factory owns the id; the same pattern as
// createItemAndAdd, but the item goes to setRobotLimbItem, not an inventory).
static Item* createLimbItem(GameWorld* gw, const char* sid) {
    if (!gw || !gw->theFactory || !g_createItemFn || !sid || !sid[0]) return 0;
    __try {
        GameData* tmpl = findItemTemplateImpl(gw, sid, (unsigned int)LIMB_REPLACEMENT);
        if (!tmpl) return 0;
        char buf[sizeof(hand) + 16];
        memset(buf, 0, sizeof(buf));
        hand* h = reinterpret_cast<hand*>(buf);
        g_handCtorFn(h, 0, 0, (itemType)LIMB_REPLACEMENT, 0, 0);
        return g_createItemFn(gw->theFactory, tmpl, h, 0, 0, -1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int applyLimbStates(GameWorld* gw, Character* c, const unsigned char states[4],
                    const char sid[4][48], bool createSeveredItem) {
    if (!c || !states) return 0;
    int changed = 0;
    __try {
        MedicalSystem* med = &c->medical;
        // NOTE: robotLimbs is lazily allocated - do NOT bail on null (a healthy
        // character has none until the first amputation; the engine's own
        // amputate/crushLimb paths allocate it on demand).
        for (int i = 0; i < 4; ++i) {
            unsigned char want = states[i];
            if (want == 0xFF) continue; // owner couldn't read it
            unsigned char have = limbStateOf(med, i);
            if (want == have) continue;
            RobotLimbs::Limb limb = (RobotLimbs::Limb)i;
            if (want == (unsigned char)LIMB_CRUSHED) {
                // ORIGINAL -> CRUSHED (a crushed robotic limb also lands here;
                // crushLimb handles both).
                if (g_medCrushLimbFn) { g_medCrushLimbFn(med, limb); changed |= (1 << i); }
            } else if (want == (unsigned char)LIMB_STUMP ||
                       want == (unsigned char)LIMB_REPLACED) {
                // Sever when the local limb is still attached. createSeveredItem
                // only on the WORLD AUTHORITY (host) - its real ground item then
                // streams to everyone via the world-item channel; a peer creating
                // one too would duplicate it.
                if (have == (unsigned char)LIMB_ORIGINAL ||
                    have == (unsigned char)LIMB_CRUSHED) {
                    if (g_medAmputateFn) {
                        Ogre::Vector3 zero(0.0f, 0.0f, 0.0f);
                        g_medAmputateFn(med, limb, createSeveredItem, &zero);
                        changed |= (1 << i);
                        have = limbStateOf(med, i);
                    }
                }
                // Fit the prosthetic (Phase D): fabricate the same template and
                // attach it via the engine's own fit path (isLoadingASave=true =
                // the save-load reconstruction path, no side-effect chatter).
                if (want == (unsigned char)LIMB_REPLACED &&
                    have == (unsigned char)LIMB_STUMP &&
                    sid && sid[i][0] && g_medSetRobotLimbFn) {
                    Item* it = createLimbItem(gw, sid[i]);
                    if (it) {
                        g_medSetRobotLimbFn(med, limb, it, /*isLoadingASave*/true);
                        changed |= (1 << i);
                    }
                    // Feasibility research (weapon lesson, protocol 9): log the
                    // fabricate outcome so a factory-null template is visible.
                    char rb[120]; _snprintf(rb, sizeof(rb) - 1,
                        "[med] LIMB-FIT limb=%d sid='%s' created=%d", i, sid[i],
                        it ? 1 : 0);
                    rb[sizeof(rb) - 1] = '\0'; coop::logLine(rb);
                }
            }
            // want == LIMB_ORIGINAL with a severed local limb cannot be healed
            // back (no engine lever); the mismatch is benign (fresh save load
            // resolves it) and writeMedical keeps the vitals honest meanwhile.
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return changed;
}

int destroySeveredLimbsNear(GameWorld* gw, const unsigned int cHand[5], float radius) {
    if (!gw || !g_getObjsFn || !g_destroyObjFn) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    int destroyed = 0;
    __try {
        Ogre::Vector3 center = ro->getPosition();
        g_npcQuery.clear();
        g_getObjsFn(gw, &g_npcQuery, &center, radius, ITEM, 64, 0);
        unsigned int n = g_npcQuery.size();
        RootObject* victims[16]; unsigned int nv = 0;
        for (unsigned int i = 0; i < n && nv < 16; ++i) {
            RootObject* o = g_npcQuery[i]; if (!o) continue;
            Item* it = reinterpret_cast<Item*>(o);
            if (it->isInInventory) continue; // only FREE ground copies
            if (it->itemFunction != ITEM_SEVERED_LIMB) continue;
            victims[nv++] = o;
        }
        for (unsigned int i = 0; i < nv; ++i) {
            if (g_destroyObjFn(gw, victims[i], /*justUnloaded*/false,
                               "coop-severed-limb-dedupe"))
                ++destroyed;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return destroyed; }
    return destroyed;
}

int countSeveredLimbsNear(GameWorld* gw, const unsigned int cHand[5], float radius) {
    if (!gw || !g_getObjsFn) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    int count = 0;
    __try {
        Ogre::Vector3 center = ro->getPosition();
        g_npcQuery.clear();
        g_getObjsFn(gw, &g_npcQuery, &center, radius, ITEM, 64, 0);
        unsigned int n = g_npcQuery.size();
        for (unsigned int i = 0; i < n; ++i) {
            RootObject* o = g_npcQuery[i]; if (!o) continue;
            Item* it = reinterpret_cast<Item*>(o);
            if (it->isInInventory) continue;
            if (it->itemFunction != ITEM_SEVERED_LIMB) continue;
            ++count;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return count; }
    return count;
}

bool amputateSubjectLimb(GameWorld* gw, const unsigned int subjHand[5], int limb) {
    (void)gw;
    if (limb < 0 || limb > 3 || !g_medAmputateFn) return false;
    Character* s = resolveCharByHand(subjHand[3], subjHand[4], subjHand[0],
                                     subjHand[1], subjHand[2]);
    if (!s) return false;
    __try {
        MedicalSystem* med = &s->medical;
        // limbStateOf, NOT robotLimbs->states: robotLimbs is lazily allocated
        // (null on any character that never lost a limb - i.e. exactly the
        // scenario subject), and bailing on null made the scaffold a no-op.
        if (limbStateOf(med, limb) != (unsigned char)LIMB_ORIGINAL)
            return false; // already gone
        Ogre::Vector3 zero(0.0f, 0.0f, 0.0f);
        // createSeveredItem=true: this runs on the body's OWNER, mirroring what
        // a real severing hit does (the peer paths never create the item).
        g_medAmputateFn(med, (RobotLimbs::Limb)limb, true, &zero);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool woundSubjectLimbs(GameWorld* gw, const unsigned int subjHand[5],
                       float flesh, float blood) {
    (void)gw;
    Character* s = resolveCharByHand(subjHand[3], subjHand[4], subjHand[0],
                                     subjHand[1], subjHand[2]);
    if (!s) return false;
    __try {
        MedicalSystem* med = &s->medical;
        // Protocol 16: wound the FULL anatomy (head/chest/stomach + limbs), and
        // the STUN track too (flesh+15, above the cut so the collapse behaviour
        // stays the same as the original 4-limb scaffold) - so the medic_order
        // oracle genuinely exercises every field the wire now carries.
        float stun = flesh + 15.0f;
        unsigned int n = med->anatomy.count;
        for (unsigned int i = 0; i < n; ++i) {
            MedicalSystem::HealthPartStatus* p =
                med->anatomy.stuff ? med->anatomy.stuff[i] : 0;
            if (!p) continue;
            if (p->flesh > flesh)    p->flesh = flesh;      // only lower, never heal
            if (p->fleshStun > stun) p->fleshStun = stun;
        }
        if (blood < 0.0f) blood = 0.0f;
        if (med->blood > blood) med->blood = blood; // only weaken, never heal
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int healSubjectBandage(GameWorld* gw, const unsigned int subjHand[5]) {
    (void)gw;
    Character* s = resolveCharByHand(subjHand[3], subjHand[4], subjHand[0],
                                     subjHand[1], subjHand[2]);
    if (!s) return 0;
    int n = 0;
    __try {
        MedicalSystem* med = &s->medical;
        // Protocol 16: bandage the FULL anatomy (matches the extended wound
        // scaffold), so per-PART treatment forwarding is exercised end to end.
        unsigned int cnt = med->anatomy.count;
        for (unsigned int i = 0; i < cnt; ++i) {
            MedicalSystem::HealthPartStatus* p =
                med->anatomy.stuff ? med->anatomy.stuff[i] : 0;
            if (!p) continue;
            // Bandage only what is damaged, and only RAISE (the same field a real
            // applyFirstAid pass raises incrementally toward _maxHealth; the flesh
            // then self-heals up to the bandaged level via the local medical sim).
            if (p->flesh < p->_maxHealth &&
                p->bandaging < p->_maxHealth) {
                p->bandaging = p->_maxHealth;
                ++n;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return n;
}

bool reviveSubject(GameWorld* gw, const unsigned int subjHand[5]) {
    (void)gw;
    Character* s = resolveCharByHand(subjHand[3], subjHand[4], subjHand[0],
                                     subjHand[1], subjHand[2]);
    if (!s) return false;
    // knockDown(off) clears the forced KO timer + releases the ragdoll (own SEH).
    if (!knockDown(s, false)) return false;
    __try {
        MedicalSystem* med = &s->medical;
        med->unconcious = false;
        if (med->blood < 80.0f) med->blood = 80.0f; // healthy floor (raise only)
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool vetoLocalDeath(Character* c) {
    if (!c) return false;
    // Only act on a body the local sim has actually killed - the caller gates
    // on the OWNER stream reporting alive, so this un-does a purely-local death.
    if (!(g_isDeadCharFn && g_isDeadCharFn(c))) return false;
    // Release the forced KO + ragdoll first (own SEH), same lever as revive.
    knockDown(c, false);
    __try {
        MedicalSystem* med = &c->medical;
        med->dead       = false; // Character::isDead() reads this -> clears BODY_DEAD
        med->unconcious = false;
        if (med->blood < 80.0f) med->blood = 80.0f; // healthy floor (raise only)
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool pickCombatVictim(GameWorld* gw, const unsigned int refHand[5],
                      const unsigned int excludeHand[5], unsigned int outHand[5],
                      const unsigned int excludeHand2[5]) {
    if (!g_getCharsFn || !gw) return false;
    Character* ref = resolveCharByHand(refHand[3], refHand[4], refHand[0],
                                       refHand[1], refHand[2]);
    if (!ref) return false;
    // Gather nearby non-squad candidates by distance inside one SEH frame, then
    // apply the upright filter (readBodyState has its own SEH) on the shortlist.
    const unsigned int MAXC = 16;
    Character* cand[MAXC]; float candD[MAXC]; unsigned int nc = 0;
    __try {
        Ogre::Vector3 center = ref->getPosition();
        g_npcQuery.clear();
        g_getCharsFn(gw, &g_npcQuery, &center, 30.0f, 30.0f, 30.0f, 64, 64, 0);
        for (unsigned int i = 0; i < g_npcQuery.size() && nc < MAXC; ++i) {
            RootObject* o = g_npcQuery[i];
            if (!o || isPlayerSquad(gw, o)) continue;
            Ogre::Vector3 p = o->getPosition();
            float dx = p.x - center.x, dz = p.z - center.z;
            cand[nc] = static_cast<Character*>(o);
            candD[nc] = dx * dx + dz * dz;
            ++nc;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    Character* best = 0; float bestD = 1e18f;
    for (unsigned int i = 0; i < nc; ++i) {
        if (readBodyState(cand[i]) != 0) continue; // down/dead/ragdoll = not a victim
        CombatRead cr; // already fighting = someone else's victim (window A's)
        if (readCombat(cand[i], &cr) && cr.inCombat) continue;
        unsigned int h[5];
        if (!readObjectHand(static_cast<RootObject*>(cand[i]), h)) continue;
        if (excludeHand && h[3] == excludeHand[3] && h[4] == excludeHand[4]) continue;
        if (excludeHand2 && h[3] == excludeHand2[3] && h[4] == excludeHand2[4]) continue;
        if (candD[i] < bestD) { bestD = candD[i]; best = cand[i]; }
    }
    if (!best) return false;
    return readObjectHand(static_cast<RootObject*>(best), outHand);
}

bool orderAttackByHand(GameWorld* gw, const unsigned int atkHand[5],
                       const unsigned int vicHand[5]) {
    (void)gw;
    Character* a = resolveCharByHand(atkHand[3], atkHand[4], atkHand[0],
                                     atkHand[1], atkHand[2]);
    Character* v = resolveCharByHand(vicHand[3], vicHand[4], vicHand[0],
                                     vicHand[1], vicHand[2]);
    if (!a || !v) return false;
    CombatRead cr;
    if (readCombat(a, &cr) && cr.inCombat) return true; // already fighting - don't thrash
    return orderMeleeAttack(a, v);
}


} // namespace engine
} // namespace coop
