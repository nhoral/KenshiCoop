// EngineProbe.cpp - raw-RVA diagnostic probes (Phase 5e code motion, 2026-07-19).
// Split out of EngineInventory.cpp (spike-451 weapon-mint recipe trace + the
// diagWeaponCreate diagnostic) and EngineInternal.cpp (spike-402 native-snapshot
// round-trip) so the shipping Release DLL no longer carries the mkspy detour
// trampolines or the probe entry points. This TU is HARNESS-ONLY: it is excluded
// from the Release build in KenshiCoop.vcxproj (like the Scenario*.cpp TUs) - its
// only callers are the probe scenarios (ScenarioProbes.cpp / ScenarioWorldItems.cpp),
// which are themselves Release-excluded.
//
// The public declarations live in EngineProbe.h; the spike-401 research tech-tree
// store surface STAYS in EngineInventory.cpp/Engine.h because three of its entry
// points (researchEnumKnown / researchQueryBySid / researchStartBySid) are
// load-bearing protocol-38 SYNC calls (interleaved sync+probe).
//
// Owner state: section-private anon-namespace mkspy trace state (incl. g_mkspyBase)
// only. Shared inventory helpers (createItemAndAdd, fallbackWeaponManufacturer,
// resolveObjectByHand, captureContainerContents) and the engine fn-ptrs
// (g_createItemFn/g_handCtorFn/g_getDataOfTypeFn/g_dataScratch) are external,
// declared in EngineInternal.h / Engine.h and defined in their owning TUs.

#include "EngineInternal.h"

namespace coop {
namespace engine {

// ---- Spike 451: weapon-mint recipe trace ------------------------------------
// The engine mints weapons at runtime constantly (armed NPC spawns, vendor stock,
// crafting) while OUR 6-arg createItem returns null for every weapon template
// (diagWeaponCreate: 0/24 even with a live weapon's exact man/mat pointers). These
// detours watch the ENGINE do it: both createItem overloads + copyItem + the
// Sword constructor + chooseDataFromList (list picks scoped to weapon mints), each
// logging full args, the caller's RVA and the result as "[mkspy] ..." lines. The
// last SUCCESSFUL engine weapon mint's args are captured so probeReplayWeaponMint
// can re-issue the identical call from plugin context - proving (or refuting) the
// recipe in the same run.
namespace {

typedef Item*     (__fastcall* CreateItemStateFn)(RootObjectFactory* self, GameData* itemState);
typedef Item*     (__fastcall* CopyItemFn)(RootObjectFactory* self, Item* from);
typedef GameData* (__fastcall* ChooseDataFn)(RootObjectFactory* self, GameData* dataList,
                                             const std::string* listName,
                                             itemType materialDataType, int useVal012);
typedef Sword*    (__fastcall* SwordCtorFn)(Sword* self, GameData* baseData,
                                            GameData* companyData, GameData* materialData,
                                            hand* handle, int level);

CreateItemFn      g_mk6Orig   = 0;
CreateItemStateFn g_mkSOrig   = 0;
CopyItemFn        g_cpyOrig   = 0;
ChooseDataFn      g_cdlOrig   = 0;
SwordCtorFn       g_swordOrig = 0;

// Module base for caller RVAs: g_createItemFn is base + 0x57FFD0 (RootObjectFactory.h).
unsigned __int64 g_mkspyBase = 0;
// Depth flag: >0 while inside a WEAPON-template createItem6, so the (very hot)
// chooseDataFromList detour only logs the picks belonging to a weapon mint.
// Main-thread only (the factory runs on the game's main thread).
int g_inWeaponMk = 0;

// Last successful ENGINE weapon mint (template pointers persist for the session).
GameData* g_wmGd = 0; GameData* g_wmMesh = 0; GameData* g_wmMat = 0;
int g_wmLevel = 0; Faction* g_wmFaction = 0; bool g_wmHave = false;

void mkspySid(GameData* gd, char* out, unsigned int len) {
    if (!out || !len) return;
    out[0] = '\0';
    if (!gd) { strncpy(out, "(null)", len - 1); out[len - 1] = '\0'; return; }
    __try {
        strncpy(out, gd->stringID.c_str(), len - 1);
        out[len - 1] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) { strncpy(out, "(fault)", len - 1); out[len - 1] = '\0'; }
}

int mkspyType(GameData* gd) {
    if (!gd) return -1;
    __try { return (int)gd->type; } __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}

unsigned __int64 mkspyRva(void* retAddr) {
    if (!g_mkspyBase) return 0;
    unsigned __int64 a = (unsigned __int64)retAddr;
    return (a > g_mkspyBase) ? (a - g_mkspyBase) : 0;
}

// Line caps so an all-calls trace can't flood a long session (spike runs only).
int g_mk6Logged = 0;
int g_cdlLogged = 0;
const int MK6_LOG_CAP = 300;

// SEH-guarded: is this freshly created Item actually a weapon (virtual isWeapon)?
bool mkspyIsWeapon(Item* it) {
    if (!it) return false;
    __try { return it->isWeapon() != 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

Item* __fastcall mk6_hook(RootObjectFactory* self, GameData* gd, const hand* handle,
                          GameData* weaponMesh, GameData* matData, int levelOverride,
                          Faction* flagUniform) {
    void* caller = _ReturnAddress();
    const int gdt = mkspyType(gd);
    const bool isWpn = (gdt == (int)WEAPON);
    // Run-1 finding: the engine's weapon mints call the Sword ctor from INSIDE
    // createItem6's address range (+0x5805F2) yet no WEAPON-typed gd ever entered
    // this hook - the entry gd must be typed differently (state record?). So run 2
    // traces EVERY entry (capped) and scopes the chooseDataFromList trace to any
    // in-flight createItem6.
    ++g_inWeaponMk;
    Item* it = g_mk6Orig(self, gd, handle, weaponMesh, matData, levelOverride, flagUniform);
    --g_inWeaponMk;
    if (g_mk6Logged < MK6_LOG_CAP) {
        ++g_mk6Logged;
        __try {
            char gsid[48], msid[48], xsid[48];
            mkspySid(gd, gsid, sizeof(gsid));
            mkspySid(weaponMesh, msid, sizeof(msid));
            mkspySid(matData, xsid, sizeof(xsid));
            unsigned int hi = 0, hs = 0, ht = 0, hc = 0, hcs = 0;
            if (handle) { hi = handle->index; hs = handle->serial; ht = (unsigned int)handle->type;
                          hc = handle->container; hcs = handle->containerSerial; }
            char b[440];
            _snprintf(b, sizeof(b) - 1,
                "[mkspy] mk6 gd='%s'/%d mesh='%s'/%d mat='%s'/%d lvl=%d fac=%d "
                "hand={i=%u s=%u t=%u c=%u cs=%u} ret=%d caller=+0x%I64X",
                gsid, gdt, msid, mkspyType(weaponMesh), xsid, mkspyType(matData),
                levelOverride, flagUniform ? 1 : 0, hi, hs, ht, hc, hcs,
                it ? 1 : 0, mkspyRva(caller));
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            // Capture by RESULT type, not entry type: run 1 proved the engine's
            // weapon mints reach the Sword ctor from inside createItem6 while NO
            // WEAPON-typed gd ever entered the hook - the entry record is typed
            // differently, so classify off the constructed item itself.
            if (it && (isWpn || mkspyIsWeapon(it))) {
                g_wmGd = gd; g_wmMesh = weaponMesh; g_wmMat = matData;
                g_wmLevel = levelOverride; g_wmFaction = flagUniform; g_wmHave = true;
                char c[100];
                _snprintf(c, sizeof(c) - 1, "[mkspy] CAPTURED weapon recipe (entry gdType=%d)", gdt);
                c[sizeof(c) - 1] = '\0'; coop::logLine(c);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return it;
}

Item* __fastcall mkS_hook(RootObjectFactory* self, GameData* itemState) {
    void* caller = _ReturnAddress();
    Item* it = g_mkSOrig(self, itemState);
    __try {
        if (mkspyType(itemState) == (int)WEAPON || g_inWeaponMk) {
            char gsid[48];
            mkspySid(itemState, gsid, sizeof(gsid));
            char b[200];
            _snprintf(b, sizeof(b) - 1, "[mkspy] mkState gd='%s'/%d ret=%d caller=+0x%I64X",
                      gsid, mkspyType(itemState), it ? 1 : 0, mkspyRva(caller));
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return it;
}

Item* __fastcall cpy_hook(RootObjectFactory* self, Item* from) {
    void* caller = _ReturnAddress();
    Item* it = g_cpyOrig(self, from);
    __try {
        GameData* gd = from ? from->getGameData() : 0;
        if (mkspyType(gd) == (int)WEAPON) {
            char gsid[48];
            mkspySid(gd, gsid, sizeof(gsid));
            char b[200];
            _snprintf(b, sizeof(b) - 1, "[mkspy] copyItem gd='%s' ret=%d caller=+0x%I64X",
                      gsid, it ? 1 : 0, mkspyRva(caller));
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return it;
}

GameData* __fastcall cdl_hook(RootObjectFactory* self, GameData* dataList,
                              const std::string* listName, itemType materialDataType,
                              int useVal012) {
    GameData* r = g_cdlOrig(self, dataList, listName, materialDataType, useVal012);
    if (g_inWeaponMk > 0 && g_cdlLogged < MK6_LOG_CAP) {
        ++g_cdlLogged;
        __try {
            char lsid[48], rsid[48], lname[48];
            mkspySid(dataList, lsid, sizeof(lsid));
            mkspySid(r, rsid, sizeof(rsid));
            lname[0] = '\0';
            if (listName) { strncpy(lname, listName->c_str(), sizeof(lname) - 1); lname[sizeof(lname) - 1] = '\0'; }
            char b[280];
            _snprintf(b, sizeof(b) - 1,
                "[mkspy] chooseList list='%s' name='%s' matType=%d use=%d -> '%s'/%d",
                lsid, lname, (int)materialDataType, useVal012, rsid, mkspyType(r));
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return r;
}

Sword* __fastcall swordCtor_hook(Sword* self, GameData* baseData, GameData* companyData,
                                 GameData* materialData, hand* handle, int level) {
    void* caller = _ReturnAddress();
    Sword* r = g_swordOrig(self, baseData, companyData, materialData, handle, level);
    __try {
        char gsid[48], csid[48], msid[48];
        mkspySid(baseData, gsid, sizeof(gsid));
        mkspySid(companyData, csid, sizeof(csid));
        mkspySid(materialData, msid, sizeof(msid));
        unsigned int hi = 0, hs = 0, ht = 0, hc = 0, hcs = 0;
        if (handle) { hi = handle->index; hs = handle->serial; ht = (unsigned int)handle->type;
                      hc = handle->container; hcs = handle->containerSerial; }
        char b[340];
        _snprintf(b, sizeof(b) - 1,
            "[mkspy] swordCtor gd='%s' company='%s'/%d mat='%s'/%d lvl=%d "
            "hand={i=%u s=%u t=%u c=%u cs=%u} ret=%d caller=+0x%I64X",
            gsid, csid, mkspyType(companyData), msid, mkspyType(materialData),
            level, hi, hs, ht, hc, hcs, r ? 1 : 0, mkspyRva(caller));
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}

} // namespace

bool installCreateItemTraceHook() {
    // Base for caller RVAs: createItem6's documented RVA anchors the module base.
    intptr_t a6 = KenshiLib::GetRealAddress(
        static_cast<Item* (RootObjectFactory::*)(GameData*, const hand&, GameData*,
                                                 GameData*, int, Faction*)>(
            &RootObjectFactory::createItem));
    if (a6) g_mkspyBase = (unsigned __int64)a6 - 0x57FFD0ull;
    intptr_t aS = KenshiLib::GetRealAddress(
        static_cast<Item* (RootObjectFactory::*)(GameData*)>(&RootObjectFactory::createItem));
    intptr_t aC = KenshiLib::GetRealAddress(&RootObjectFactory::copyItem);
    intptr_t aL = KenshiLib::GetRealAddress(&RootObjectFactory::chooseDataFromList);
    intptr_t aW = KenshiLib::GetRealAddress(
        static_cast<Sword* (Sword::*)(GameData*, GameData*, GameData*, hand, int)>(
            &Sword::_CONSTRUCTOR));
    int ok = 0, want = 0;
    if (a6) { ++want; if (KenshiLib::AddHook(a6, (void*)&mk6_hook,  (void**)&g_mk6Orig)   == KenshiLib::SUCCESS) ++ok; }
    if (aS) { ++want; if (KenshiLib::AddHook(aS, (void*)&mkS_hook,  (void**)&g_mkSOrig)   == KenshiLib::SUCCESS) ++ok; }
    if (aC) { ++want; if (KenshiLib::AddHook(aC, (void*)&cpy_hook,  (void**)&g_cpyOrig)   == KenshiLib::SUCCESS) ++ok; }
    if (aL) { ++want; if (KenshiLib::AddHook(aL, (void*)&cdl_hook,  (void**)&g_cdlOrig)   == KenshiLib::SUCCESS) ++ok; }
    if (aW) { ++want; if (KenshiLib::AddHook(aW, (void*)&swordCtor_hook, (void**)&g_swordOrig) == KenshiLib::SUCCESS) ++ok; }
    char b[160];
    _snprintf(b, sizeof(b) - 1,
        "[mkspy] hooks installed %d/%d (mk6=%d mkState=%d copy=%d chooseList=%d swordCtor=%d)",
        ok, want, g_mk6Orig ? 1 : 0, g_mkSOrig ? 1 : 0, g_cpyOrig ? 1 : 0,
        g_cdlOrig ? 1 : 0, g_swordOrig ? 1 : 0);
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    return ok == want && want >= 1;
}

int probeReplayWeaponMint(GameWorld* gw, const unsigned int cHand[5]) {
    if (!g_wmHave) { coop::logLine("[mkspy] replay: nothing captured"); return 0; }
    if (!gw || !gw->theFactory || !g_createItemFn || !g_handCtorFn) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    Inventory* inv = invOf(ro);
    if (!inv) { coop::logLine("[mkspy] replay: no inv"); return 0; }
    __try {
        char buf[sizeof(hand) + 16]; memset(buf, 0, sizeof(buf));
        hand* h = reinterpret_cast<hand*>(buf);
        g_handCtorFn(h, 0, 0, WEAPON, 0, 0);
        Item* it = g_createItemFn(gw->theFactory, g_wmGd, h, g_wmMesh, g_wmMat,
                                  g_wmLevel, g_wmFaction);
        int added = 0;
        if (it) added = inv->tryAddItem(it, 1) ? 1 : 0;
        char gsid[48];
        mkspySid(g_wmGd, gsid, sizeof(gsid));
        char b[220];
        _snprintf(b, sizeof(b) - 1,
            "[mkspy] replay gd='%s' lvl=%d fac=%d -> created=%d added=%d",
            gsid, g_wmLevel, g_wmFaction ? 1 : 0, it ? 1 : 0, added);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return (it && added) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("[mkspy] replay SEH-except");
        return -1;
    }
}

int probeFabricateWeaponLoose(GameWorld* gw, const unsigned int cHand[5],
                              char* outSid, unsigned int outLen) {
    if (outSid && outLen) outSid[0] = '\0';
    if (!gw || !g_getDataOfTypeFn) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    char sid[48]; sid[0] = '\0';
    __try {
        g_dataScratch.clear();
        g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, WEAPON);
        unsigned int n = g_dataScratch.size();
        for (unsigned int i = 0; i < n; ++i) {
            GameData* t = g_dataScratch[i];
            if (!t) continue;
            const char* s = t->stringID.c_str();
            if (!s || !s[0]) continue;
            if (strcmp(s, "FISTS") == 0) continue; // natural weapon - not a real fabrication test
            strncpy(sid, s, sizeof(sid) - 1);
            sid[sizeof(sid) - 1] = '\0';
            break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!sid[0]) return 0;
    if (outSid && outLen) { strncpy(outSid, sid, outLen - 1); outSid[outLen - 1] = '\0'; }
    return createItemAndAdd(gw, inv, sid, (unsigned int)WEAPON, 1, 0, false) ? 1 : 0;
}

int commonNovelWeaponSid(GameWorld* gw, const unsigned int cHand[5],
                         char* outSid, unsigned int outLen) {
    if (outSid && outLen) outSid[0] = '\0';
    if (!gw || !g_getDataOfTypeFn || !outSid || outLen == 0) return 0;
    // Current contents first (own SEH guard): the pick must EXCLUDE any weapon the
    // container already holds, so the scenario's arrival gate can't false-pass on a
    // baked copy of the same template.
    const unsigned int MAXC = 64;
    InvItemEntry cur[64];
    unsigned int nc = captureContainerContents(gw, cHand, cur, MAXC, 0);
    __try {
        g_dataScratch.clear();
        g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, WEAPON);
        unsigned int n = g_dataScratch.size();
        for (unsigned int i = 0; i < n; ++i) {
            GameData* t = g_dataScratch[i];
            if (!t) continue;
            const char* s = t->stringID.c_str();
            if (!s || !s[0]) continue;
            if (strcmp(s, "FISTS") == 0) continue; // natural weapon - never a real acquisition
            bool held = false;
            for (unsigned int j = 0; j < nc && !held; ++j)
                if (cur[j].itemType == (unsigned int)WEAPON &&
                    strcmp(cur[j].stringID, s) == 0) held = true;
            if (held) continue;
            strncpy(outSid, s, outLen - 1); outSid[outLen - 1] = '\0';
            return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 0;
}

// DIAGNOSTIC: prove weapon fabrication over the template set. Walks the first `maxTry`
// WEAPON base templates and instantiates each with the spike-451 recipe (manufacturer
// GameData FIRST, weapon template in the third "weaponMesh" slot, blank NULL_ITEM hand,
// lvl 0) - the engine's own mint shape. Logs per-template result + the success count
// (historically 0/24 with the old template-first args). Trials are added to `inv` and
// immediately destroyed so nothing leaks.
void diagWeaponCreate(GameWorld* gw, const unsigned int cHand[5], int maxTry) {
    if (maxTry <= 0) maxTry = 24;
    if (!gw || !gw->theFactory || !g_createItemFn || !g_handCtorFn || !g_getDataOfTypeFn) {
        coop::logLine("[wpndiag] missing fns"); return;
    }
    RootObject* ro = resolveObjectByHand(cHand);
    Inventory* inv = invOf(ro);
    if (!inv) { coop::logLine("[wpndiag] no inv"); return; }
    GameData* man = fallbackWeaponManufacturer(gw);
    if (!man) { coop::logLine("[wpndiag] no WEAPON_MANUFACTURER record"); return; }
    unsigned int n = 0;
    __try {
        g_dataScratch.clear();
        g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, WEAPON);
        n = g_dataScratch.size();
    } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
    int tried = 0, okCount = 0;
    for (unsigned int i = 0; i < n && tried < maxTry; ++i) {
        GameData* tmpl = 0;
        __try { tmpl = g_dataScratch[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { tmpl = 0; }
        if (!tmpl) continue;
        ++tried;
        Item* it = 0;
        char sid[48]; sid[0] = '\0';
        __try {
            strncpy(sid, tmpl->stringID.c_str(), sizeof(sid) - 1); sid[sizeof(sid) - 1] = '\0';
            char buf[sizeof(hand) + 16]; memset(buf, 0, sizeof(buf));
            hand* h = reinterpret_cast<hand*>(buf);
            g_handCtorFn(h, 0, 0, NULL_ITEM, 0, 0);
            // Spike-451 shape: manufacturer first, template as "weaponMesh", lvl 0.
            it = g_createItemFn(gw->theFactory, man, h, tmpl, 0, 0, 0);
            if (it) { ++okCount; inv->removeItemAutoDestroy(it, 1); }
        } __except (EXCEPTION_EXECUTE_HANDLER) { it = (Item*)0; }
        char e[140];
        _snprintf(e, sizeof(e) - 1, "[wpndiag] tmpl[%d] sid='%s' -> %s", tried - 1, sid, it ? "OK" : "null");
        e[sizeof(e) - 1] = '\0'; coop::logLine(e);
    }
    char s[120];
    _snprintf(s, sizeof(s) - 1, "[wpndiag] RESULT ok=%d/%d (spike-451 manufacturer-first recipe)",
              okCount, tried);
    s[sizeof(s) - 1] = '\0'; coop::logLine(s);
}

// ---- Spike 402: native-snapshot round-trip probe ----------------------------
// Serialise the local leader through the engine's own GameDataContainer save/load
// pipeline into a temp .mod, reload it, and compare record/instance counts - a
// zero-serialiser-of-our-own proof that native object serialisation round-trips.
// Moved here from EngineInternal.cpp (Phase 5e). Uses the external leader().
int probeNativeSnapshot(GameWorld* gw) {
    Character* c = leader(gw);
    if (!c) return 0;

    // Keep every allocation in probe-owned containers. Character::serialise
    // writes state GameData records into `output`'s source container; nothing
    // is inserted into GameWorld::savedata or the live platoon container.
    GameDataContainer output;
    output.setName("KenshiCoop r402 native snapshot");
    const std::string sid("kenshicoop-r402-instances");
    const std::string displayName("KenshiCoop r402 instances");
    GameData* instances =
        output.createNewData(INSTANCE_COLLECTION, sid, displayName);
    if (!instances) {
        coop::logErrLine("[r402] native snapshot: INSTANCE_COLLECTION create failed");
        return 0;
    }

    GameSaveState state = c->serialise(&output, instances, 0);
    const unsigned int stateCount = (unsigned int)state.states.size();
    const unsigned int instanceCount =
        (unsigned int)instances->instances.size();
    const unsigned int recordCount =
        (unsigned int)output._getAllData().size();

    char tempDir[MAX_PATH];
    tempDir[0] = '\0';
    if (!GetTempPathA(MAX_PATH, tempDir)) {
        coop::logErrLine("[r402] native snapshot: GetTempPath failed");
        return -1;
    }
    char filename[MAX_PATH];
    _snprintf(filename, sizeof(filename) - 1,
              "%sKenshiCoop-r402-%lu.mod", tempDir,
              (unsigned long)GetCurrentProcessId());
    filename[sizeof(filename) - 1] = '\0';

    const std::string file(filename);
    const bool saved = output.save(file, 0);
    WIN32_FILE_ATTRIBUTE_DATA attr;
    memset(&attr, 0, sizeof(attr));
    const bool fileExists =
        GetFileAttributesExA(filename, GetFileExInfoStandard, &attr) != 0;
    const unsigned __int64 fileBytes = fileExists
        ? (((unsigned __int64)attr.nFileSizeHigh << 32) |
           (unsigned __int64)attr.nFileSizeLow)
        : 0;

    GameDataContainer loaded;
    const std::string modName("KenshiCoop-r402");
    const bool loadedOk = saved &&
        loaded.load(file, modName, 0, 0, true);
    GameData* loadedInstances =
        loadedOk ? loaded.getData(sid, INSTANCE_COLLECTION) : 0;
    const unsigned int loadedInstanceCount = loadedInstances
        ? (unsigned int)loadedInstances->instances.size() : 0;
    const unsigned int loadedRecordCount = loadedOk
        ? (unsigned int)loaded._getAllData().size() : 0;

    char line[480];
    _snprintf(line, sizeof(line) - 1,
              "[r402] native snapshot states=%u instances=%u records=%u "
              "save=%d bytes=%llu load=%d loadedInstances=%u "
              "loadedRecords=%u file='%s'",
              stateCount, instanceCount, recordCount, saved ? 1 : 0,
              fileBytes, loadedOk ? 1 : 0, loadedInstanceCount,
              loadedRecordCount, filename);
    line[sizeof(line) - 1] = '\0';
    coop::logLine(line);

    if (!saved || !loadedOk) return -1;
    return stateCount > 0 && instanceCount == 1 &&
           loadedInstanceCount == instanceCount &&
           loadedRecordCount == recordCount ? 1 : 0;
}

} // namespace engine
} // namespace coop
