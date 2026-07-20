// EngineWorld.cpp - world economy + structures (monolith split from
// EngineInternal.cpp, 2026-07-12): protocol 22 money/wallet + vendor trading,
// protocol 23 recruitment probe, protocol 24 faction relations read/write,
// protocol 26 door/gate state, protocols 27/28 placed-building sync +
// dismantle, protocol 33 production machines, protocol 34 storage/machine
// containers.
//
// Owner state: section-private statics/anon-namespace helpers only (wallet/
// trader resolvers, fill*Read shims, class-type predicates).
// Must NOT: define g_* engine pointers (EngineInternal.cpp owns them - EngineInternal.h
// declares them; the faction/shop hook BODIES live in EngineInternal.cpp), or change
// any log string - log phrasing is the API consumed by the PowerShell oracles
// (see resources/CODE_MAP.md).

#include "EngineInternal.h"

namespace coop {
namespace engine {

// ---- Protocol 22 groundwork: money + vendor trading -------------------------

namespace {
// Ownerships block (the wallet) of the platoon containing 'c', or 0. The chain
// Character -> ActivePlatoon (getPlatoon) -> Platoon (::me) -> Ownerships is all
// engine-owned pointers; caller holds SEH.
Ownerships* walletOf(Character* c) {
    if (!c || !g_getPlatoonFn) return 0;
    ActivePlatoon* ap = g_getPlatoonFn(c);
    if (!ap) return 0;
    Platoon* p = ap->me;
    if (!p) return 0;
    return &p->ownerships;
}

// The vendor's trader Character: the raw member first, the engine accessor as
// fallback (the member read null on every enumerated vendor, run 103547).
// Caller holds SEH.
Character* traderOf(ShopTrader* st) {
    if (!st) return 0;
    Character* t = st->trader;
    if (!t && g_shopGetTraderFn) t = g_shopGetTraderFn(st);
    return t;
}
} // namespace

unsigned int listVendorsNear(GameWorld* gw, VendorRead* out, unsigned int maxOut,
                             float radius) {
    if (!gw || !gw->player || !out || maxOut == 0 || !g_getObjsFn) return 0;
    unsigned int n = 0;
    __try {
        if (gw->player->playerCharacters.size() == 0) return 0;
        Character* ld = gw->player->playerCharacters[0];
        if (!ld) return 0;
        Ogre::Vector3 center = ld->getPosition();
        g_npcQuery.clear();
        g_getObjsFn(gw, &g_npcQuery, &center, radius, SHOP_TRADER_CLASS, 64, 0);
        unsigned int total = g_npcQuery.size();
        for (unsigned int i = 0; i < total && n < maxOut; ++i) {
            RootObject* o = g_npcQuery[i]; if (!o) continue;
            VendorRead& v = out[n];
            memset(&v, 0, sizeof(v));
            if (!readObjectHand(o, v.hand)) continue;
            v.money = -1; v.stock = -1; v.qty = -1; v.src = 0;
            __try { v.money = o->getMoney(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            __try {
                Ogre::Vector3 p = o->getPosition();
                v.x = p.x; v.y = p.y; v.z = p.z;
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            __try {
                // Two candidate stock containers (run 101952/103018 finding): the
                // ShopTrader's own aggregated Inventory is LAZY (null until the
                // trade UI opens), while the trader CHARACTER's inventory is a
                // regular, always-present container. Read whichever answers; the
                // trader's save-stable hand is also the cross-client vendor
                // identity (the ShopTrader wrapper's serial is runtime-minted).
                ShopTrader* st = static_cast<ShopTrader*>(o);
                GameData* gd = 0;
                __try { gd = o->getGameData(); } __except (EXCEPTION_EXECUTE_HANDLER) { gd = 0; }
                if (gd) {
                    strncpy(v.sid, gd->stringID.c_str(), sizeof(v.sid) - 1);
                    v.sid[sizeof(v.sid) - 1] = '\0';
                }
                Inventory* inv = invOf(o);
                if (inv) v.src = 1;
                Character* trader = traderOf(st);
                if (trader) {
                    readObjectHand(static_cast<RootObject*>(trader), v.traderHand);
                    if (!inv) {
                        inv = invOf(static_cast<RootObject*>(trader));
                        if (inv) v.src = 2;
                    }
                }
                if (inv) {
                    InvItemEntry ent[96];
                    unsigned int ne = readInvItems(inv, ent, 0, 96);
                    int q = 0;
                    for (unsigned int e = 0; e < ne; ++e)
                        q += (ent[e].quantity < 1) ? 1 : (int)ent[e].quantity;
                    v.stock = (int)ne; v.qty = q;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return n; }
    return n;
}

bool readWalletByHand(const unsigned int mHand[5], int* outMoney) {
    if (outMoney) *outMoney = -1;
    if (!mHand || !outMoney || !g_ownGetMoneyFn) return false;
    Character* c = resolveCharByHand(mHand[3], mHand[4], mHand[0], mHand[1], mHand[2]);
    if (!c) return false;
    __try {
        Ownerships* ow = walletOf(c);
        if (!ow) return false;
        *outMoney = g_ownGetMoneyFn(ow);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool writeWalletByHand(const unsigned int mHand[5], int money) {
    if (!mHand || money < 0 || !g_ownSetMoneyFn) return false;
    Character* c = resolveCharByHand(mHand[3], mHand[4], mHand[0], mHand[1], mHand[2]);
    if (!c) return false;
    __try {
        Ownerships* ow = walletOf(c);
        if (!ow) return false;
        g_ownSetMoneyFn(ow, money);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- Protocol 23 phase 0: recruitment probe ---------------------------------

int probeRecruit(GameWorld* gw, bool runtimeSubject,
                 unsigned int outHandBefore[5], unsigned int outHandAfter[5],
                 float radius) {
    if (outHandBefore) memset(outHandBefore, 0, 5 * sizeof(unsigned int));
    if (outHandAfter)  memset(outHandAfter,  0, 5 * sizeof(unsigned int));
    if (!gw || !gw->player) return -1;
    Character* subject = 0;
    if (runtimeSubject) {
        // Runtime leg: a freshly-minted NPC (host-only hand - the spawn-sync
        // regime). Offset to the side so the two legs don't stack bodies.
        subject = spawnNpcInFront(gw, 5.0f, 2.5f);
    } else {
        // Baked leg: the nearest world NPC (save-stable hand both clients
        // share - the bar-recruit regime). First non-player body in 60 u.
        if (!g_getCharsFn) return -1;
        __try {
            if (gw->player->playerCharacters.size() == 0) return -1;
            Character* ld = gw->player->playerCharacters[0];
            if (!ld) return -1;
            Ogre::Vector3 center = ld->getPosition();
            g_npcQuery.clear();
            g_getCharsFn(gw, &g_npcQuery, &center, radius, radius, 30.0f, 32, 32, 0);
            unsigned int total = g_npcQuery.size();
            for (unsigned int i = 0; i < total; ++i) {
                RootObject* o = g_npcQuery[i];
                if (!o || isPlayerSquad(gw, o)) continue;
                // Never pick a PEER-DRIVEN body (it sits in the AI-suspend set
                // every driven tick): with recruit sync on, the deterministic
                // nearest-NPC pick would otherwise select the same bar NPC the
                // peer already recruited - its local copy is re-keyed to the
                // peer's stream hand here, and double-recruiting it collides
                // both sides' ownership on one hand (run 120738).
                if (g_aiSuspended.find(static_cast<Character*>(o)) != g_aiSuspended.end())
                    continue;
                subject = static_cast<Character*>(o);
                break;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    }
    if (!subject) {
        if (runtimeSubject) {
            coop::logLine("[recruit] probe no-subject (spawn failed)");
        } else {
            char b[80]; _snprintf(b, sizeof(b) - 1,
                "[recruit] probe no-subject (no world NPC in %.0fu)", radius);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        return 0;
    }
    __try {
        if (outHandBefore) readObjectHand(static_cast<RootObject*>(subject), outHandBefore);
        bool ok = recruitNpc(gw, subject);
        // The AFTER hand is the identity evidence: recruit re-containers the
        // body into a player platoon, so the CONTAINER fields change - the
        // question is whether index/serial survive (peer-resolvable) or not.
        if (outHandAfter) readObjectHand(static_cast<RootObject*>(subject), outHandAfter);
        return ok ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("[recruit] probe SEH-except");
        return -1;
    }
}

bool joinPlayerSquadAt(GameWorld* gw, Character* c, const unsigned int newHand[5]) {
    if (!gw || !gw->player || !c || !newHand ||
        !g_playerFactionFn || !g_getPlatoonFn)
        return false;
    __try {
        // Idempotent, but tab-aware: skip only when the body is ALREADY a member
        // AND already sits in the target tab (echoed edge / already inserted). A
        // transfer moves an existing member to a DIFFERENT tab, so a bare
        // "already a member" guard would wrongly skip the re-container.
        unsigned int ch[5];
        if (readObjectHand(static_cast<RootObject*>(c), ch) &&
            ch[1] == newHand[1] && ch[2] == newHand[2] &&
            isPlayerSquad(gw, static_cast<RootObject*>(c)))
            return true;
        Faction* f = g_playerFactionFn(gw->player);
        if (!f) return false;
        // Case A (the norm - recruit_sync showed tabKnown=1): resolve the target
        // platoon by the reported container serial among existing squad members,
        // so the inserted body lands in the SAME tab the recruiter used and its
        // hand container matches the streamed key.
        ActivePlatoon* target = 0;
        unsigned int pc = gw->player->playerCharacters.size();
        for (unsigned int i = 0; i < pc; ++i) {
            Character* m = gw->player->playerCharacters[i];
            if (!m) continue;
            unsigned int mh[5];
            if (!readObjectHand(static_cast<RootObject*>(m), mh)) continue;
            if (mh[1] == newHand[1] && mh[2] == newHand[2]) {
                target = g_getPlatoonFn(m);
                break;
            }
        }
        // Case B fallback: the reported tab is a runtime-minted platoon not
        // present locally - drop into the leader's default tab so the body is
        // still a panel member (control still follows the peer-owned pin).
        if (!target && pc > 0 && gw->player->playerCharacters[0])
            target = g_getPlatoonFn(gw->player->playerCharacters[0]);
        if (!target) return false;
        // setFaction (probe lever 1, the header-documented re-platoon path):
        // moves 'c' into the player faction + target platoon WITHOUT touching
        // PlayerInterface::recruit, so the recruit detour never fires and no
        // spurious edge echoes back to the author. PlayerInterface::
        // playerCharacters refreshes on a later engine tick, so DON'T re-check
        // isPlayerSquad here (it reads false the same frame - a misleading
        // false negative); reaching setFaction with a resolved platoon IS the
        // success signal.
        c->setFaction(f, target);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

unsigned int listPlayerRelations(GameWorld* gw, FactionRead* out, unsigned int maxOut) {
    if (!gw || !out || maxOut == 0 || !g_relGetFn) return 0;
    unsigned int n = 0;
    __try {
        if (!gw->player || !gw->factionMgr) return 0;
        Faction* pf = gw->player->getFaction();
        if (!pf || !pf->relations) return 0;
        lektor<Faction*>& all = gw->factionMgr->participants;
        unsigned int total = all.size();
        for (unsigned int i = 0; i < total && n < maxOut; ++i) {
            Faction* f = all[i];
            if (!f || f == pf || f->notARealFaction || !f->relations) continue;
            FactionRead& r = out[n];
            memset(&r, 0, sizeof(r));
            facSidOf(f, r.sid, sizeof(r.sid));
            if (r.sid[0] == '\0') continue; // no GameData = not addressable cross-client
            strncpy(r.name, f->name.c_str(), sizeof(r.name) - 1);
            r.name[sizeof(r.name) - 1] = '\0';
            // BOTH rows: the player's table toward them AND their table toward
            // the player - the probe must show which side guard aggro consults
            // (and whether the engine keeps them mirrored).
            r.usToThem = g_relGetFn(pf->relations, f);
            r.themToUs = g_relGetFn(f->relations, pf);
            r.enemy      = g_relIsEnemyFn ? (g_relIsEnemyFn(pf->relations, f) ? 1 : 0) : -1;
            r.enemyRecip = g_relIsEnemyFn ? (g_relIsEnemyFn(f->relations, pf) ? 1 : 0) : -1;
            r.ally       = g_relIsAllyFn  ? (g_relIsAllyFn(pf->relations, f)  ? 1 : 0) : -1;
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return n; }
    return n;
}

// std::string construction cannot share a frame with __try (C2712), so the
// sid -> Faction* lookup lives in this unguarded shim (the callee is guarded).
static Faction* factionBySidC(GameWorld* gw, const char* sid) {
    std::string s(sid);
    return factionBySidGuarded(gw, &s);
}

bool readRelationBySid(GameWorld* gw, const char* sid, float* outUs, float* outThem) {
    if (outUs)   *outUs   = -999.0f;
    if (outThem) *outThem = -999.0f;
    if (!gw || !sid || !g_relGetFn) return false;
    __try {
        if (!gw->player) return false;
        Faction* pf = gw->player->getFaction();
        if (!pf || !pf->relations) return false;
        Faction* f = factionBySidC(gw, sid);
        if (!f || !f->relations) return false;
        if (outUs)   *outUs   = g_relGetFn(pf->relations, f);
        if (outThem) *outThem = g_relGetFn(f->relations, pf);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool writeRelationBySid(GameWorld* gw, const char* sid, float value, bool reciprocal,
                        float* outBefore, float* outAfter) {
    if (outBefore) *outBefore = -999.0f;
    if (outAfter)  *outAfter  = -999.0f;
    if (!gw || !sid || !g_relGetFn || !g_relSetFn) return false;
    __try {
        if (!gw->player) return false;
        Faction* pf = gw->player->getFaction();
        if (!pf || !pf->relations) return false;
        Faction* f = factionBySidC(gw, sid);
        if (!f || !f->relations) return false;
        if (outBefore) *outBefore = g_relGetFn(pf->relations, f);
        g_relSetFn(pf->relations, f, value);
        // The reciprocal row (their table toward the player) is what THEIR AI
        // consults; the probe writes both so the evidence covers either design.
        if (reciprocal) g_relSetFn(f->relations, pf, value);
        if (outAfter) *outAfter = g_relGetFn(pf->relations, f);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("[fac] write SEH-except");
        return false;
    }
}

bool installFactionHooks() {
    typedef void (FactionRelations::*EvMemFn)(Faction*, FactionRelations::FactionEvent, float);
    typedef void (FactionRelations::*AmtMemFn)(Faction*, float, float);
    intptr_t evAddr = KenshiLib::GetRealAddress(
        static_cast<EvMemFn>(&FactionRelations::_NV_affectRelations));
    intptr_t amtAddr = KenshiLib::GetRealAddress(
        static_cast<AmtMemFn>(&FactionRelations::_NV_affectRelations));
    bool ok = true;
    if (!evAddr || KenshiLib::AddHook(evAddr, (void*)&affectRelEv_hook,
                                      (void**)&g_affectEvOrig) != KenshiLib::SUCCESS)
        ok = false;
    if (!amtAddr || KenshiLib::AddHook(amtAddr, (void*)&affectRelAmt_hook,
                                       (void**)&g_affectAmtOrig) != KenshiLib::SUCCESS)
        ok = false;
    return ok;
}

unsigned int drainFactionDeltas(FactionDelta* out, unsigned int maxOut) {
    unsigned int n = 0;
    for (unsigned int i = 0; i < g_facDeltas.size() && n < maxOut; ++i, ++n) {
        const FactionDeltaRec& r = g_facDeltas[i];
        memcpy(out[n].meSid,   r.meSid,   sizeof(out[n].meSid));
        memcpy(out[n].whomSid, r.whomSid, sizeof(out[n].whomSid));
        out[n].isEvent = r.isEvent; out[n].ev = r.ev;
        out[n].amount = r.amount; out[n].mult = r.mult; out[n].after = r.after;
    }
    g_facDeltas.clear();
    return n;
}

// ---- Protocol 26: door/gate state --------------------------------------------

// Fill a DoorRead from a live door Building. Callers hold the SEH frame.
// `open` collapses the animated DoorState: a door in motion reports its
// DESTINATION (OPENING = open, CLOSING = closed) so the channel never
// publishes the transient mid-swing state.
static void fillDoorRead(DoorStuff* d, DoorRead* r) {
    memset(r, 0, sizeof(*r));
    readObjectHand(static_cast<RootObject*>(d), r->hand);
    Ogre::Vector3 p = d->getPosition();
    r->x = p.x; r->y = p.y; r->z = p.z;
    DoorState st = d->state;
    r->state   = (int)st;
    r->open    = (st == DOORSTATE_OPEN || st == DOORSTATE_OPENING) ? 1 : 0;
    r->hasLock = d->doorLock ? 1 : 0;
    r->locked  = (r->hasLock && g_doorIsLockedFn && g_doorIsLockedFn(d)) ? 1 : 0;
    r->gate    = 0; // filled by the caller (isGate is a virtual we avoid here)
    // Protocol 28: the door-to-building link. parent + the index in the
    // parent's ordered `doors` list give a placed door its translatable wire
    // identity (the runtime door hand itself never crosses usefully).
    r->doorIndex = -1;
    Building* par = d->parent;
    if (par) {
        readObjectHand(static_cast<RootObject*>(par), r->parentHand);
        unsigned int nd = par->doors.size();
        for (unsigned int i = 0; i < nd; ++i) {
            if (par->doors[i] == static_cast<Building*>(d)) {
                r->doorIndex = (int)i;
                break;
            }
        }
    }
    GameData* gd = d->getGameData();
    if (gd) {
        strncpy(r->name, gd->name.c_str(), sizeof(r->name) - 1);
        r->name[sizeof(r->name) - 1] = '\0';
        // Town gates are DoorStuff too; the name carries the distinction well
        // enough for diagnostics without touching the isGate vtable slot.
        r->gate = ciContains(r->name, "gate") ? 1 : 0;
    }
}

unsigned int enumDoorsNear(GameWorld* gw, float radius, DoorRead* out, unsigned int maxOut) {
    if (!gw || !out || maxOut == 0 || !g_getObjsFn || !g_doorIsOpenFn) return 0;
    unsigned int n = 0;
    __try {
        Ogre::Vector3 centers[4];
        unsigned int nc = interestCenters(gw, centers);
        if (nc == 0) return 0;
        for (unsigned int ci = 0; ci < nc; ++ci) {
            g_npcQuery.clear();
            g_getObjsFn(gw, &g_npcQuery, &centers[ci], radius, BUILDING, 256, 0);
            unsigned int total = g_npcQuery.size();
            for (unsigned int i = 0; i < total && n < maxOut; ++i) {
                RootObject* o = g_npcQuery[i];
                if (!o) continue;
                Building* b = static_cast<Building*>(o);
                if (!b->imADoor) continue;
                DoorStuff* d = static_cast<DoorStuff*>(b);
                unsigned int h[5];
                if (!readObjectHand(o, h)) continue;
                bool dup = false; // the two interest spheres can overlap
                for (unsigned int k = 0; k < n; ++k)
                    if (out[k].hand[3] == h[3] && out[k].hand[4] == h[4] &&
                        out[k].hand[1] == h[1]) { dup = true; break; }
                if (dup) continue;
                fillDoorRead(d, &out[n]);
                ++n;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return n; }
    return n;
}

bool readDoorByHand(const unsigned int dHand[5], DoorRead* out) {
    if (!dHand || !out) return false;
    RootObject* ro = resolveObjectByHand(dHand);
    if (!ro) return false;
    __try {
        Building* b = static_cast<Building*>(ro);
        if (!b->imADoor) return false;
        fillDoorRead(static_cast<DoorStuff*>(b), out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool writeDoorByHand(const unsigned int dHand[5], int wantOpen, int wantLocked,
                     DoorRead* outAfter) {
    if (outAfter) memset(outAfter, 0, sizeof(*outAfter));
    if (!dHand || !g_doorOpenFn || !g_doorCloseFn) return false;
    RootObject* ro = resolveObjectByHand(dHand);
    if (!ro) return false;
    __try {
        Building* b = static_cast<Building*>(ro);
        if (!b->imADoor) return false;
        DoorStuff* d = static_cast<DoorStuff*>(b);
        DoorState st = d->state;
        int curOpen = (st == DOORSTATE_OPEN || st == DOORSTATE_OPENING) ? 1 : 0;
        if (wantOpen != curOpen) {
            // Polite path first (animation + navmesh + sound - what a local
            // click does); the blunt UT force paths only when it refuses.
            bool ok = wantOpen ? g_doorOpenFn(d) : g_doorCloseFn(d);
            if (!ok) {
                if (wantOpen && g_doorForceOpenFn)       g_doorForceOpenFn(d);
                else if (!wantOpen && g_doorForceCloseFn) g_doorForceCloseFn(d);
            }
        }
        if (wantLocked >= 0 && d->doorLock && g_doorLockFn && g_doorUnlockFn) {
            int curLocked = (g_doorIsLockedFn && g_doorIsLockedFn(d)) ? 1 : 0;
            if (wantLocked != curLocked) {
                if (wantLocked) g_doorLockFn(d); else g_doorUnlockFn(d);
            }
        }
        if (outAfter) fillDoorRead(d, outAfter);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("[door] write SEH-except");
        return false;
    }
}

// ---- Protocol 27: placed-building sync -----------------------------------

// Fill a BuildRead from a live Building. Callers hold the SEH frame.
static void fillBuildRead(Building* b, BuildRead* r) {
    memset(r, 0, sizeof(*r));
    RootObject* ro = static_cast<RootObject*>(b);
    readObjectHand(ro, r->hand);
    Ogre::Vector3 p = ro->getPosition();
    r->x = p.x; r->y = p.y; r->z = p.z;
    r->progress = b->_buildState.constructionProgress;
    r->complete = b->_buildState.isComplete ? 1 : 0;
    GameData* gd = ro->getGameData();
    if (gd) {
        strncpy(r->sid, gd->stringID.c_str(), sizeof(r->sid) - 1);
        r->sid[sizeof(r->sid) - 1] = '\0';
        strncpy(r->name, gd->name.c_str(), sizeof(r->name) - 1);
        r->name[sizeof(r->name) - 1] = '\0';
    }
}

unsigned int enumSitesNear(GameWorld* gw, float radius, BuildRead* out, unsigned int maxOut) {
    if (!gw || !out || maxOut == 0 || !g_getObjsFn) return 0;
    unsigned int n = 0;
    __try {
        Ogre::Vector3 centers[4];
        unsigned int nc = interestCenters(gw, centers);
        if (nc == 0) return 0;
        for (unsigned int ci = 0; ci < nc; ++ci) {
            g_npcQuery.clear();
            g_getObjsFn(gw, &g_npcQuery, &centers[ci], radius, BUILDING, 256, 0);
            unsigned int total = g_npcQuery.size();
            for (unsigned int i = 0; i < total && n < maxOut; ++i) {
                RootObject* o = g_npcQuery[i];
                if (!o) continue;
                Building* b = static_cast<Building*>(o);
                // Only sites UNDER CONSTRUCTION: complete/baked buildings are
                // legion (whole towns) and never stream through this channel.
                if (b->_buildState.isComplete) continue;
                unsigned int h[5];
                if (!readObjectHand(o, h)) continue;
                bool dup = false; // the two interest spheres can overlap
                for (unsigned int k = 0; k < n; ++k)
                    if (out[k].hand[3] == h[3] && out[k].hand[4] == h[4] &&
                        out[k].hand[1] == h[1]) { dup = true; break; }
                if (dup) continue;
                fillBuildRead(b, &out[n]);
                ++n;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return n; }
    return n;
}

bool readBuildingByHand(const unsigned int bHand[5], BuildRead* out) {
    if (!bHand || !out) return false;
    RootObject* ro = resolveObjectByHand(bHand);
    if (!ro) return false;
    __try {
        fillBuildRead(static_cast<Building*>(ro), out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool writeBuildProgressByHand(const unsigned int bHand[5], float progress,
                              BuildRead* outAfter) {
    if (outAfter) memset(outAfter, 0, sizeof(*outAfter));
    if (!bHand || !g_buildSetProgFn) return false;
    RootObject* ro = resolveObjectByHand(bHand);
    if (!ro) return false;
    __try {
        Building* b = static_cast<Building*>(ro);
        bool wasComplete = b->_buildState.isComplete;
        if (!wasComplete) {
            g_buildSetProgFn(b, progress);
            // If the engine's setter does not self-complete at the threshold,
            // fire the completion notification exactly once ourselves so the
            // site finishes NATIVELY (scaffold off, navmesh, materials).
            if (progress >= 1.0f && !b->_buildState.isComplete && g_buildNotifyDoneFn)
                g_buildNotifyDoneFn(b);
        }
        if (outAfter) fillBuildRead(b, outAfter);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("[build] progress-write SEH-except");
        return false;
    }
}

int placeBuildingAt(GameWorld* gw, const char* sid, float x, float y, float z,
                    float heading, bool completed, unsigned int outHand[5]) {
    if (outHand) memset(outHand, 0, 5 * sizeof(unsigned int));
    if (!gw || !gw->theFactory || !g_createBldgFn || !sid || !sid[0]) return 0;
    GameData* tmpl = findItemTemplateImpl(gw, sid, (unsigned int)BUILDING);
    if (!tmpl) {
        char b[128];
        _snprintf(b, sizeof(b) - 1, "[build] mint template MISS sid='%s'", sid);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return 0;
    }
    __try {
        Ogre::Quaternion rot(Ogre::Radian(heading), Ogre::Vector3::UNIT_Y);
        Ogre::Vector3 pos(x, y, z); // createBuilding re-grounds vertically
        Building* bld = g_createBldgFn(
            gw->theFactory, tmpl, pos, /*town*/0, /*owner*/0, rot, /*cb*/0,
            /*furnitureOf*/0, /*isDoorOf*/0, /*saveState*/0, /*isIndoorsOf*/0,
            /*invisible*/false, completed, /*isFoliage*/false,
            /*floor*/0, /*isOutsideFurniture*/false);
        if (!bld) {
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                      "[build] mint REFUSED sid='%s' pos=%.1f,%.1f,%.1f (factory null)",
                      sid, x, y, z);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return 0;
        }
        RootObject* ro = static_cast<RootObject*>(bld);
        unsigned int h[5] = { 0, 0, 0, 0, 0 };
        bool haveHand = readObjectHand(ro, h);
        if (outHand && haveHand) memcpy(outHand, h, sizeof(h));
        Ogre::Vector3 ap = ro->getPosition();
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "[build] mint OK sid='%s' completed=%d hand=%u.%u.%u.%u.%u(%d) "
                  "pos=%.1f,%.1f,%.1f prog=%.3f",
                  sid, completed ? 1 : 0, h[0], h[1], h[2], h[3], h[4],
                  haveHand ? 1 : 0, ap.x, ap.y, ap.z,
                  bld->_buildState.constructionProgress);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("[build] mint FAULTED");
        return -1;
    }
}

// Deterministic small BUILDING template for the probe's programmatic
// placement: buildable, tiny footprint, no power/inputs. Caller holds SEH.
// wantDoor selects the shack class instead (a real walk-in building whose
// template mints DoorStuff children - the protocol-28 subject).
static GameData* findBuildTemplate(GameWorld* gw, bool wantDoor) {
    if (!gw || !g_getDataOfTypeFn) return 0;
    g_dataScratch.clear();
    g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, BUILDING);
    unsigned int n = g_dataScratch.size();
    const char* fixturePrefs[] = { "training dummy", "camp bed", "storage", "well" };
    const char* doorPrefs[]    = { "small shack", "shack", "storm house", "small house" };
    const char** prefs = wantDoor ? doorPrefs : fixturePrefs;
    for (unsigned int k = 0; k < 4; ++k) {
        for (unsigned int i = 0; i < n; ++i) {
            GameData* gd = g_dataScratch[i];
            if (gd && ciContains(gd->name.c_str(), prefs[k])) return gd;
        }
    }
    return 0;
}

int probePlaceBuilding(GameWorld* gw, float fwd, float side, bool wantDoor,
                       unsigned int outHand[5], char* outSid, unsigned int sidLen,
                       float* outX, float* outY, float* outZ, float* outYaw) {
    if (outSid && sidLen) outSid[0] = '\0';
    if (!gw) return 0;
    GameData* tmpl = 0;
    Ogre::Vector3 pos(0, 0, 0); float yaw = 0.0f; bool anchored = false;
    __try {
        tmpl = findBuildTemplate(gw, wantDoor);
        anchored = leaderAnchor(gw, fwd, side, &pos, &yaw);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (!tmpl || !anchored) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[build] probe-place no %s",
                  tmpl ? "leader anchor" : "template");
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return 0;
    }
    const char* sid = 0;
    __try { sid = tmpl->stringID.c_str(); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (outSid && sidLen) {
        strncpy(outSid, sid, sidLen - 1);
        outSid[sidLen - 1] = '\0';
    }
    if (outX) *outX = pos.x;
    if (outY) *outY = pos.y;
    if (outZ) *outZ = pos.z;
    if (outYaw) *outYaw = yaw;
    unsigned int localHand[5] = { 0, 0, 0, 0, 0 };
    int rc = placeBuildingAt(gw, sid, pos.x, pos.y, pos.z, yaw,
                             /*completed*/false, localHand);
    if (outHand) memcpy(outHand, localHand, sizeof(localHand));
    // A successful PROGRAMMATIC placement queues the same edge the UI detour
    // would (fromUi=0), so the sync layer announces it. Peer-side MINTS call
    // placeBuildingAt directly and never land here - echo-free.
    if (rc == 1) {
        BuildEdgeRec e;
        memset(&e, 0, sizeof(e));
        memcpy(e.hand, localHand, sizeof(e.hand));
        strncpy(e.sid, sid, sizeof(e.sid) - 1);
        e.sid[sizeof(e.sid) - 1] = '\0';
        e.x = pos.x; e.y = pos.y; e.z = pos.z; e.yaw = yaw;
        e.fromUi = 0;
        if (g_buildEdges.size() < 32) g_buildEdges.push_back(e);
    }
    return rc;
}

// ---- Protocol 28: placed-building doors + dismantle ------------------------

bool readDoorOfBuilding(const unsigned int bHand[5], unsigned int doorIndex,
                        DoorRead* out) {
    if (!bHand || !out) return false;
    RootObject* ro = resolveObjectByHand(bHand);
    if (!ro) return false;
    __try {
        Building* b = static_cast<Building*>(ro);
        if (doorIndex >= b->doors.size()) return false;
        Building* db = b->doors[doorIndex];
        if (!db || !db->imADoor) return false;
        fillDoorRead(static_cast<DoorStuff*>(db), out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool destroyBuildingByHand(GameWorld* gw, const unsigned int bHand[5]) {
    if (!gw || !bHand || !g_destroyObjFn) return false;
    RootObject* ro = resolveObjectByHand(bHand);
    if (!ro) return false;
    __try {
        return g_destroyObjFn(gw, ro, /*justUnloaded*/false, "coop-build-remove");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("[build] destroy FAULTED");
        return false;
    }
}

// ---- Protocol 33: production machine sync ---------------------------------

// Machine-class filter for the census. STORAGE is excluded (containers ride
// the container-inventory channel); TURRET/WALL/DOOR/FLUFF are not machines.
static bool isMachineClassType(int t) {
    return t == (int)BCTYPE_PRODUCTION || t == (int)BCTYPE_CRAFTING ||
           t == (int)BCTYPE_ITEM_FURNACE || t == (int)BCTYPE_FARM ||
           t == (int)BCTYPE_RESEARCH;
}

// True when the classType is ProductionBuilding-derived (has productionState /
// output buffer / input lektor). RESEARCH is UseableStuff-derived - power and
// tech level only.
static bool isProductionClassType(int t) {
    return t == (int)BCTYPE_PRODUCTION || t == (int)BCTYPE_CRAFTING ||
           t == (int)BCTYPE_ITEM_FURNACE || t == (int)BCTYPE_FARM;
}

// Fill a ProdRead from a live machine Building. Callers hold the SEH frame
// and have already verified isMachineClassType(b->classType).
static void fillProdRead(Building* b, ProdRead* r) {
    memset(r, 0, sizeof(*r));
    RootObject* ro = static_cast<RootObject*>(b);
    readObjectHand(ro, r->hand);
    Ogre::Vector3 p = ro->getPosition();
    r->x = p.x; r->y = p.y; r->z = p.z;
    r->classType = (int)b->classType;
    r->complete  = b->_buildState.isComplete ? 1 : 0;
    // sentinels: "not this class / no buffer"
    r->powerOn = -1; r->powerOutput = -1.0f; r->productionState = -1;
    r->miningLevel = -1.0f; r->outAmount = -1.0f; r->outCap = -1;
    r->nInputs = 0; r->inAmount[0] = -1.0f; r->inAmount[1] = -1.0f;
    r->techLevel = -1;
    r->grown = -1.0f; r->died = -1.0f; r->growStart = -1.0f; r->harvested = -1;
    // Every machine class derives from UseableStuff: power bit is a plain
    // member (0x3B5), output watts via the engine's own getter (generator
    // override reports production, the base reports the consumer draw).
    UseableStuff* us = static_cast<UseableStuff*>(b);
    r->powerOn = us->powerOn ? 1 : 0;
    if (g_machPowerOutBaseFn) {
        bool gen = g_machIsGenFn ? g_machIsGenFn(us) : false;
        r->powerOutput = (gen && g_machPowerOutGenFn)
            ? g_machPowerOutGenFn(us) : g_machPowerOutBaseFn(us);
    }
    if (r->classType == (int)BCTYPE_RESEARCH && g_machTechLvlFn)
        r->techLevel = g_machTechLvlFn(static_cast<ResearchBuilding*>(b));
    if (isProductionClassType(r->classType)) {
        ProductionBuilding* pb = static_cast<ProductionBuilding*>(b);
        r->productionState = (int)pb->productionState;
        r->miningLevel     = pb->_resourceMiningLevel;
        StorageBuilding::ConsumptionItem* outBuf = pb->productionItem;
        if (outBuf) {
            r->outAmount = outBuf->amount;
            r->outCap    = outBuf->maxCapacity;
            if (outBuf->item) {
                strncpy(r->outSid, outBuf->item->stringID.c_str(),
                        sizeof(r->outSid) - 1);
                r->outSid[sizeof(r->outSid) - 1] = '\0';
            }
        }
        // The buffer only materializes on the first production tick; until
        // then report the TEMPLATE the machine produces (run-151948 finding:
        // out='' outAmt=-1 on a freshly minted bench).
        if (r->outSid[0] == '\0') {
            MachProdItemDataFn pf = (r->classType == (int)BCTYPE_CRAFTING &&
                                     g_machProdDataCraftFn)
                ? g_machProdDataCraftFn : g_machProdDataBaseFn;
            GameData* pd = pf ? pf(b) : 0;
            if (pd) {
                strncpy(r->outSid, pd->stringID.c_str(), sizeof(r->outSid) - 1);
                r->outSid[sizeof(r->outSid) - 1] = '\0';
            }
        }
        unsigned int ni = pb->consumptionItems.size();
        for (unsigned int i = 0; i < ni && i < 2; ++i) {
            StorageBuilding::ConsumptionItem& ci = pb->consumptionItems[i];
            r->inAmount[i] = ci.amount;
            if (ci.item) {
                strncpy(r->inSid[i], ci.item->stringID.c_str(),
                        sizeof(r->inSid[i]) - 1);
                r->inSid[i][sizeof(r->inSid[i]) - 1] = '\0';
            }
            r->nInputs = (int)(i + 1);
        }
    }
    if (r->classType == (int)BCTYPE_FARM) {
        FarmBuilding* fb = static_cast<FarmBuilding*>(b);
        r->grown     = fb->grown;
        r->died      = fb->died;
        r->growStart = fb->growStart;
        r->harvested = fb->harvested;
    }
    GameData* gd = ro->getGameData();
    if (gd) {
        strncpy(r->sid, gd->stringID.c_str(), sizeof(r->sid) - 1);
        r->sid[sizeof(r->sid) - 1] = '\0';
        strncpy(r->name, gd->name.c_str(), sizeof(r->name) - 1);
        r->name[sizeof(r->name) - 1] = '\0';
    }
}

unsigned int enumMachinesNear(GameWorld* gw, float radius, ProdRead* out,
                              unsigned int maxOut) {
    if (!gw || !out || maxOut == 0 || !g_getObjsFn) return 0;
    unsigned int n = 0;
    __try {
        Ogre::Vector3 centers[4];
        unsigned int nc = interestCenters(gw, centers);
        if (nc == 0) return 0;
        for (unsigned int ci = 0; ci < nc; ++ci) {
            g_npcQuery.clear();
            g_getObjsFn(gw, &g_npcQuery, &centers[ci], radius, BUILDING, 256, 0);
            unsigned int total = g_npcQuery.size();
            for (unsigned int i = 0; i < total && n < maxOut; ++i) {
                RootObject* o = g_npcQuery[i];
                if (!o) continue;
                Building* b = static_cast<Building*>(o);
                if (!isMachineClassType((int)b->classType)) continue;
                // Incomplete sites ride protocol 27 until finished.
                if (!b->_buildState.isComplete) continue;
                unsigned int h[5];
                if (!readObjectHand(o, h)) continue;
                bool dup = false; // the two interest spheres can overlap
                for (unsigned int k = 0; k < n; ++k)
                    if (out[k].hand[3] == h[3] && out[k].hand[4] == h[4] &&
                        out[k].hand[1] == h[1]) { dup = true; break; }
                if (dup) continue;
                fillProdRead(b, &out[n]);
                ++n;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return n; }
    return n;
}

bool readMachineByHand(const unsigned int mHand[5], ProdRead* out) {
    if (!mHand || !out) return false;
    RootObject* ro = resolveObjectByHand(mHand);
    if (!ro) return false;
    __try {
        Building* b = static_cast<Building*>(ro);
        if (!isMachineClassType((int)b->classType)) return false;
        fillProdRead(b, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool writeMachineByHand(const unsigned int mHand[5], int wantPower,
                        float outAmount, bool useSetItem,
                        const float inAmount[2], const float farm[4],
                        ProdRead* outAfter) {
    if (outAfter) memset(outAfter, 0, sizeof(*outAfter));
    if (!mHand) return false;
    RootObject* ro = resolveObjectByHand(mHand);
    if (!ro) return false;
    __try {
        Building* b = static_cast<Building*>(ro);
        int ct = (int)b->classType;
        if (!isMachineClassType(ct)) return false;
        UseableStuff* us = static_cast<UseableStuff*>(b);
        if (wantPower >= 0 && g_machPowerFn) {
            int cur = us->powerOn ? 1 : 0;
            if (wantPower != cur) g_machPowerFn(us, wantPower != 0);
        }
        if (isProductionClassType(ct)) {
            ProductionBuilding* pb = static_cast<ProductionBuilding*>(b);
            StorageBuilding::ConsumptionItem* outBuf = pb->productionItem;
            if (outAmount >= 0.0f) {
                if (useSetItem && g_machSetProdItemFn) {
                    // The native lever: item template stays the machine's own
                    // (buffer item when materialized, else the machine's
                    // production template), amount splits into whole stack +
                    // fractional progress. setProductionItem is also the
                    // buffer-materializing path.
                    GameData* item = (outBuf && outBuf->item) ? outBuf->item : 0;
                    if (!item) {
                        MachProdItemDataFn pf = (ct == (int)BCTYPE_CRAFTING &&
                                                 g_machProdDataCraftFn)
                            ? g_machProdDataCraftFn : g_machProdDataBaseFn;
                        item = pf ? pf(b) : 0;
                    }
                    if (item) {
                        int stack = (int)outAmount;
                        float prog = outAmount - (float)stack;
                        g_machSetProdItemFn(pb, item, stack, prog);
                    }
                } else if (outBuf) {
                    outBuf->amount = outAmount; // direct write (clamp probe)
                }
            }
            if (inAmount) {
                unsigned int ni = pb->consumptionItems.size();
                for (unsigned int i = 0; i < ni && i < 2; ++i)
                    if (inAmount[i] >= 0.0f)
                        pb->consumptionItems[i].amount = inAmount[i];
            }
        }
        if (ct == (int)BCTYPE_FARM && farm) {
            FarmBuilding* fb = static_cast<FarmBuilding*>(b);
            if (farm[0] >= 0.0f) fb->grown     = farm[0];
            if (farm[1] >= 0.0f) fb->died      = farm[1];
            if (farm[2] >= 0.0f) fb->growStart = farm[2];
            if (farm[3] >= 0.0f) fb->harvested = (int)farm[3];
        }
        if (outAfter) fillProdRead(b, outAfter);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("[prod] write SEH-except");
        return false;
    }
}

bool operateMachineByHand(GameWorld* gw, const unsigned int mHand[5], float amount) {
    if (!gw || !mHand) return false;
    RootObject* ro = resolveObjectByHand(mHand);
    if (!ro) return false;
    __try {
        if (!gw->player || gw->player->playerCharacters.size() == 0) return false;
        Character* worker = gw->player->playerCharacters[0];
        if (!worker) return false;
        Building* b = static_cast<Building*>(ro);
        int ct = (int)b->classType;
        if (!isMachineClassType(ct)) return false;
        // The vtable's dispatch, done by hand: we call resolved NON-virtual
        // addresses, so classType picks the right override.
        MachOperateFn fn = g_machOperateProdFn;
        if (ct == (int)BCTYPE_CRAFTING && g_machOperateCraftFn)  fn = g_machOperateCraftFn;
        if (ct == (int)BCTYPE_FARM && g_machOperateFarmFn)       fn = g_machOperateFarmFn;
        if (ct == (int)BCTYPE_RESEARCH && g_machOperateResearchFn) fn = g_machOperateResearchFn;
        if (!fn) return false;
        fn(b, worker, amount);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("[prod] operate SEH-except");
        return false;
    }
}

// Deterministic machine template for the probe's programmatic placement.
// kind 0 = power generator (BCTYPE_PRODUCTION, has getPowerOutput), kind 1 =
// crafting bench (BCTYPE_CRAFTING, has input/output buffers), kind 2 =
// storage container (BCTYPE_STORAGE - protocol 34's chest subject), kind 3 =
// research bench (BCTYPE_RESEARCH - spike 401's subject; NOT reachable through
// findMachineTemplate, whose preference order tops out at training dummies). `skip`
// selects the skip-th DISTINCT candidate in preference order: a template's
// NAME does not reveal its BuildingClassType (run-170508: the first
// "general storage" match placed as a non-STORAGE class), so the kind-2
// caller places candidates in turn and verifies the LIVE building's class,
// destroying rejects. All candidates buildable anywhere - drills/farms need
// terrain resources so the probe never spawns them. Caller holds SEH.
static GameData* findProdTemplate(GameWorld* gw, int kind, int skip) {
    if (!gw || !g_getDataOfTypeFn) return 0;
    g_dataScratch.clear();
    g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, BUILDING);
    unsigned int n = g_dataScratch.size();
    const char* genPrefs[]   = { "small wind generator", "wind generator",
                                 "small generator", "generator" };
    const char* craftPrefs[] = { "armour crafting bench", "weapon smithing bench",
                                 "weapon smith", "engineering bench" };
    const char* storePrefs[] = { "general storage", "storage box", "storage chest",
                                 "chest", "storage" };
    const char* resPrefs[]   = { "small research bench", "research bench",
                                 "research" };
    const char** prefs;
    unsigned int nPrefs;
    if (kind == 0)      { prefs = genPrefs;   nPrefs = 4; }
    else if (kind == 2) { prefs = storePrefs; nPrefs = 5; }
    else if (kind == 3) { prefs = resPrefs;   nPrefs = 3; }
    else                { prefs = craftPrefs; nPrefs = 4; }
    GameData* seen[16];
    unsigned int nSeen = 0;
    for (unsigned int k = 0; k < nPrefs; ++k) {
        for (unsigned int i = 0; i < n; ++i) {
            GameData* gd = g_dataScratch[i];
            if (!gd || !ciContains(gd->name.c_str(), prefs[k])) continue;
            bool dup = false; // a name can match several prefs
            for (unsigned int s = 0; s < nSeen; ++s)
                if (seen[s] == gd) { dup = true; break; }
            if (dup) continue;
            if ((int)nSeen == skip) return gd;
            if (nSeen < 16) seen[nSeen++] = gd;
        }
    }
    return 0;
}

// SEH-guarded: the live Building's classType at the hand, or -1.
static int readBuildingClassByHand(const unsigned int h[5]) {
    RootObject* ro = resolveObjectByHand(h);
    if (!ro) return -1;
    __try {
        return (int)static_cast<Building*>(ro)->classType;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

int probePlaceMachine(GameWorld* gw, float fwd, float side, int kind,
                      unsigned int outHand[5], char* outSid, unsigned int sidLen) {
    if (outSid && sidLen) outSid[0] = '\0';
    if (!gw) return 0;
    Ogre::Vector3 pos(0, 0, 0); float yaw = 0.0f; bool anchored = false;
    __try {
        anchored = leaderAnchor(gw, fwd, side, &pos, &yaw);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (!anchored) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[prod] probe-place kind=%d no leader anchor",
                  kind);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return 0;
    }
    // A template NAME does not reveal its BuildingClassType (run-170508: the
    // first "general storage" name-match placed as a non-STORAGE class the
    // container census rightly skipped), so walk the preference-ordered
    // candidates: place, verify the LIVE building's class, destroy + retry on
    // mismatch. kinds 0/1 accept any machine class (the original behaviour);
    // kind 2 demands BCTYPE_STORAGE, kind 3 demands BCTYPE_RESEARCH.
    const int MAX_CANDIDATES = 8;
    for (int cand = 0; cand < MAX_CANDIDATES; ++cand) {
        GameData* tmpl = 0;
        __try {
            tmpl = findProdTemplate(gw, kind, cand);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
        if (!tmpl) {
            char b[96];
            _snprintf(b, sizeof(b) - 1,
                      "[prod] probe-place kind=%d no template (tried %d)",
                      kind, cand);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return 0;
        }
        const char* sid = 0;
        __try { sid = tmpl->stringID.c_str(); } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
        unsigned int localHand[5] = { 0, 0, 0, 0, 0 };
        int rc = placeBuildingAt(gw, sid, pos.x, pos.y, pos.z, yaw,
                                 /*completed*/false, localHand);
        if (rc != 1) {
            if (outHand) memcpy(outHand, localHand, sizeof(localHand));
            if (outSid && sidLen) {
                strncpy(outSid, sid, sidLen - 1);
                outSid[sidLen - 1] = '\0';
            }
            return rc;
        }
        int liveClass = readBuildingClassByHand(localHand);
        bool classOk = true;
        if (kind == 2) classOk = (liveClass == (int)BCTYPE_STORAGE);
        if (kind == 3) classOk = (liveClass == (int)BCTYPE_RESEARCH);
        if (!classOk) {
            char b[160];
            _snprintf(b, sizeof(b) - 1,
                      "[prod] probe-place kind=%d REJECT sid='%s' class=%d "
                      "(want %s) - destroy + next candidate",
                      kind, sid, liveClass,
                      kind == 3 ? "RESEARCH" : "STORAGE");
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            destroyBuildingByHand(gw, localHand);
            continue;
        }
        if (outHand) memcpy(outHand, localHand, sizeof(localHand));
        if (outSid && sidLen) {
            strncpy(outSid, sid, sidLen - 1);
            outSid[sidLen - 1] = '\0';
        }
        // Queue the protocol-27 edge so the peer mints the same site (the
        // probe then ramps it complete through writeBuildProgressByHand).
        BuildEdgeRec e;
        memset(&e, 0, sizeof(e));
        memcpy(e.hand, localHand, sizeof(e.hand));
        strncpy(e.sid, sid, sizeof(e.sid) - 1);
        e.sid[sizeof(e.sid) - 1] = '\0';
        e.x = pos.x; e.y = pos.y; e.z = pos.z; e.yaw = yaw;
        e.fromUi = 0;
        if (g_buildEdges.size() < 32) g_buildEdges.push_back(e);
        return 1;
    }
    char b[96];
    _snprintf(b, sizeof(b) - 1,
              "[prod] probe-place kind=%d no class-matching candidate", kind);
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    return 0;
}

// ---- Protocol 34: storage/machine container sync ---------------------------

// Container-bearing filter for the census: STORAGE chests plus the machine
// classes - a machine's crafted whole ITEMS land in the same Building
// inventory the chest uses, so both ride the container channel.
static bool isContainerClassType(int t) {
    return t == (int)BCTYPE_STORAGE || isMachineClassType(t);
}

// Fill a ContRead from a live container-bearing Building. Callers hold the
// SEH frame and have already verified isContainerClassType(b->classType).
// The contents summary reads up to 64 entries (readInvItems carries its own
// SEH) - the census's capacity measuring stick vs INV_ITEMS_MAX.
static void fillContRead(Building* b, ContRead* r) {
    memset(r, 0, sizeof(*r));
    RootObject* ro = static_cast<RootObject*>(b);
    readObjectHand(ro, r->hand);
    Ogre::Vector3 p = ro->getPosition();
    r->x = p.x; r->y = p.y; r->z = p.z;
    r->classType = (int)b->classType;
    r->complete  = b->_buildState.isComplete ? 1 : 0;
    Inventory* inv = ro->getInventory();
    r->hasInv = inv ? 1 : 0;
    if (inv) {
        InvItemEntry ent[64];
        unsigned int n = readInvItems(inv, ent, 0, 64);
        r->nEntries = (int)n;
        unsigned int h = 0; int qt = 0;
        for (unsigned int i = 0; i < n; ++i) {
            h += invEntryHash(ent[i]);
            qt += (int)ent[i].quantity;
        }
        r->hash = h; r->qtyTotal = qt;
        if (n > 0) {
            strncpy(r->firstSid, ent[0].stringID, sizeof(r->firstSid) - 1);
            r->firstSid[sizeof(r->firstSid) - 1] = '\0';
            r->firstQty = (int)ent[0].quantity;
        }
    }
    GameData* gd = ro->getGameData();
    if (gd) {
        strncpy(r->sid, gd->stringID.c_str(), sizeof(r->sid) - 1);
        r->sid[sizeof(r->sid) - 1] = '\0';
        strncpy(r->name, gd->name.c_str(), sizeof(r->name) - 1);
        r->name[sizeof(r->name) - 1] = '\0';
    }
}

unsigned int enumContainersNear(GameWorld* gw, float radius, ContRead* out,
                                unsigned int maxOut) {
    if (!gw || !out || maxOut == 0 || !g_getObjsFn) return 0;
    unsigned int n = 0;
    __try {
        Ogre::Vector3 centers[4];
        unsigned int nc = interestCenters(gw, centers);
        if (nc == 0) return 0;
        for (unsigned int ci = 0; ci < nc; ++ci) {
            g_npcQuery.clear();
            g_getObjsFn(gw, &g_npcQuery, &centers[ci], radius, BUILDING, 256, 0);
            unsigned int total = g_npcQuery.size();
            for (unsigned int i = 0; i < total && n < maxOut; ++i) {
                RootObject* o = g_npcQuery[i];
                if (!o) continue;
                Building* b = static_cast<Building*>(o);
                if (!isContainerClassType((int)b->classType)) continue;
                // Incomplete sites ride protocol 27 until finished.
                if (!b->_buildState.isComplete) continue;
                unsigned int h[5];
                if (!readObjectHand(o, h)) continue;
                bool dup = false; // the two interest spheres can overlap
                for (unsigned int k = 0; k < n; ++k)
                    if (out[k].hand[3] == h[3] && out[k].hand[4] == h[4] &&
                        out[k].hand[1] == h[1]) { dup = true; break; }
                if (dup) continue;
                fillContRead(b, &out[n]);
                ++n;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return n; }
    return n;
}

bool readContainerByHand(const unsigned int cHand[5], ContRead* out) {
    if (!cHand || !out) return false;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return false;
    __try {
        Building* b = static_cast<Building*>(ro);
        if (!isContainerClassType((int)b->classType)) return false;
        fillContRead(b, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int ensureVendorStock(GameWorld* gw, const unsigned int vHand[5]) {
    (void)gw;
    if (!vHand) return -1;
    RootObject* ro = resolveObjectByHand(vHand);
    if (!ro) return -1;
    __try {
        Inventory* inv = invOf(ro);
        if (inv) return 1;
        if (!g_getPlatoonFn || !g_platoonRefreshInvFn) {
            coop::logLine("[shop] ensure-stock fns-unresolved"); return 0;
        }
        // The hand enumerated under SHOP_TRADER_CLASS, so this IS a ShopTrader.
        ShopTrader* st = static_cast<ShopTrader*>(ro);
        Character* trader = traderOf(st);
        if (!trader) { coop::logLine("[shop] ensure-stock trader-null"); return 0; }
        ActivePlatoon* ap = g_getPlatoonFn(trader);
        if (!ap) { coop::logLine("[shop] ensure-stock platoon-null"); return 0; }
        g_platoonRefreshInvFn(ap, /*firstTime=*/true);
        inv = invOf(ro);
        if (!inv) coop::logLine("[shop] ensure-stock refresh-ran inv-still-null");
        return inv ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

int probeVendorBuy(GameWorld* gw, const unsigned int vHand[5],
                   const unsigned int buyerHand[5],
                   char* outSid, unsigned int sidLen) {
    if (outSid && sidLen) outSid[0] = '\0';
    if (!gw || !vHand || !buyerHand || !g_buyItemFn) return -1;
    RootObject* vendor = resolveObjectByHand(vHand);
    Character*  buyer  = resolveCharByHand(buyerHand[3], buyerHand[4], buyerHand[0],
                                           buyerHand[1], buyerHand[2]);
    if (!vendor || !buyer) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "[shop] probe-buy unresolved vendor=%d buyer=%d",
                  vendor ? 1 : 0, buyer ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return -1;
    }
    __try {
        // Same stock-container pick as listVendorsNear: the ShopTrader's own
        // (lazy) Inventory first, else the trader CHARACTER's - whether buyItem
        // works against the character container is itself probe evidence.
        int src = 0;
        Inventory* vinv = invOf(vendor);
        if (vinv) src = 1;
        if (!vinv) {
            Character* trader = traderOf(static_cast<ShopTrader*>(vendor));
            if (trader) {
                vinv = invOf(static_cast<RootObject*>(trader));
                if (vinv) src = 2;
            }
        }
        if (!vinv) { coop::logLine("[shop] probe-buy vendor-inv-null"); return -1; }
        {
            char sb[64];
            _snprintf(sb, sizeof(sb) - 1, "[shop] probe-buy inv-src=%d", src);
            sb[sizeof(sb) - 1] = '\0'; coop::logLine(sb);
        }
        // Pick the first LOOSE stack in the vendor's stock as the purchase subject.
        InvItemEntry ent[96];
        Item* items[96];
        unsigned int ne = readInvItems(vinv, ent, items, 96);
        Item* pick = 0; int pickIdx = -1;
        for (unsigned int i = 0; i < ne; ++i) {
            if (!items[i] || ent[i].equipped) continue;
            pick = items[i]; pickIdx = (int)i; break;
        }
        if (!pick) { coop::logLine("[shop] probe-buy no-stock"); return 0; }
        if (outSid && sidLen) {
            strncpy(outSid, ent[pickIdx].stringID, sidLen - 1);
            outSid[sidLen - 1] = '\0';
        }
        // BEFORE / AFTER evidence: vendor cash + stock, buyer tab wallet. What one
        // local buyItem mutates (and what, if anything, crosses today) IS the probe
        // deliverable - the oracle only requires the pair of lines to exist.
        int vMoney0 = -1, vMoney1 = -1, w0 = -1, w1 = -1;
        __try { vMoney0 = vendor->getMoney(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Ownerships* ow = walletOf(buyer);
        if (ow && g_ownGetMoneyFn) w0 = g_ownGetMoneyFn(ow);
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "[shop] BUY-BEFORE sid='%s' type=%u qty=%u vendorMoney=%d stock=%u wallet=%d",
                  ent[pickIdx].stringID, ent[pickIdx].itemType,
                  (unsigned int)ent[pickIdx].quantity, vMoney0, ne, w0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        Item* got = g_buyItemFn(vinv, pick, static_cast<RootObject*>(buyer));
        unsigned int ne1 = readInvItems(vinv, ent, 0, 96);
        __try { vMoney1 = vendor->getMoney(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (ow && g_ownGetMoneyFn) w1 = g_ownGetMoneyFn(ow);
        _snprintf(b, sizeof(b) - 1,
                  "[shop] BUY-AFTER ok=%d vendorMoney=%d stock=%u wallet=%d",
                  got ? 1 : 0, vMoney1, ne1, w1);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return got ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("[shop] probe-buy SEH-except");
        return -1;
    }
}


} // namespace engine
} // namespace coop
