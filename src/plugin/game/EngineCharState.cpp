// EngineCharState.cpp - character state channels + game speed (monolith split
// from EngineInternal.cpp, 2026-07-12): protocol 17 stats sync, protocol 18
// carried-body sync, protocol 19 furniture occupancy (beds + cages),
// protocol 20 stealth + detection indicators, and the consensus game-speed
// plane (read/write/quiet-write, speed-intent consume, vote-button reads).
//
// Owner state: section-private statics/anon-namespace helpers only.
// Must NOT: define g_* engine pointers (EngineInternal.cpp owns them - EngineInternal.h
// declares them; the speed-intent hook BODIES live in EngineInternal.cpp), or change
// any log string - log phrasing is the API consumed by the PowerShell oracles
// (see resources/CODE_MAP.md).

#include "EngineInternal.h"

namespace coop {
namespace engine {

// ---- Protocol 17: character stats sync --------------------------------------

bool readStats(Character* c, StatsRead* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < 40; ++i) out->stats[i] = -1.0f;
    out->xp = -1.0f; out->freeAttribPts = -1.0f;
    if (!c || !g_statsGetRefFn) return false;
    __try {
        CharStats* st = c->stats;
        if (!st) return false;
        // Raw stat slots by the engine's own accessor (STAT_STRENGTH..STAT_END-1).
        for (int i = (int)STAT_STRENGTH; i < (int)STAT_END; ++i) {
            float* p = g_statsGetRefFn(st, i);
            if (p) out->stats[i] = *p;
        }
        out->nStats = (unsigned int)STAT_END; // slots [1, nStats) filled
        out->xp = st->xp;
        out->freeAttribPts = (float)st->freeAttributePoints;
        out->valid = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readStatsByHand(const unsigned int hand[5], StatsRead* out) {
    Character* c = resolveCharByHand(hand[3], hand[4], hand[0], hand[1], hand[2]);
    return readStats(c, out);
}

bool writeStats(Character* c, const StatsRead& in) {
    if (!c || !g_statsGetRefFn) return false;
    __try {
        CharStats* st = c->stats;
        if (!st) return false;
        unsigned int n = in.nStats;
        if (n > 40u) n = 40u;
        if (n > (unsigned int)STAT_END) n = (unsigned int)STAT_END;
        for (unsigned int i = (unsigned int)STAT_STRENGTH; i < n; ++i) {
            if (in.stats[i] < 0.0f) continue; // -1 = unreadable on the owner
            float* p = g_statsGetRefFn(st, (int)i);
            if (p) *p = in.stats[i];
        }
        if (in.xp >= 0.0f)            st->xp = in.xp;
        if (in.freeAttribPts >= 0.0f) st->freeAttributePoints = (int)in.freeAttribPts;
        // Refresh the derived caches now (attack/block speed, run speed,
        // encumbrance) instead of waiting for the engine's next periodic pass.
        if (g_statsRecalcFn) g_statsRecalcFn(st);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool raiseSubjectStat(GameWorld* gw, const unsigned int subjHand[5],
                      int statId, float value) {
    (void)gw;
    if (statId <= (int)STAT_NONE || statId >= (int)STAT_END || !g_statsGetRefFn)
        return false;
    Character* s = resolveCharByHand(subjHand[3], subjHand[4], subjHand[0],
                                     subjHand[1], subjHand[2]);
    if (!s) return false;
    __try {
        CharStats* st = s->stats;
        if (!st) return false;
        float* p = g_statsGetRefFn(st, statId);
        if (!p) return false;
        if (*p < value) *p = value; // raise-only (idempotent scaffold)
        if (g_statsRecalcFn) g_statsRecalcFn(st);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Raise EVERY stat (STAT_STRENGTH..STAT_END-1) on the body at subjHand to at least
// 'value' (raise-only, same accessor path as raiseSubjectStat) and recalc ONCE.
// Returns the count of stats actually raised (0 = unresolved / no engine hooks).
// combat_win scenario: buff a PC squad so it reliably WINS the fight.
unsigned int raiseAllStats(GameWorld* gw, const unsigned int subjHand[5], float value) {
    (void)gw;
    if (!g_statsGetRefFn) return 0;
    Character* s = resolveCharByHand(subjHand[3], subjHand[4], subjHand[0],
                                     subjHand[1], subjHand[2]);
    if (!s) return 0;
    __try {
        CharStats* st = s->stats;
        if (!st) return 0;
        unsigned int n = 0;
        for (int i = (int)STAT_STRENGTH; i < (int)STAT_END; ++i) {
            float* p = g_statsGetRefFn(st, i);
            if (!p) continue;
            if (*p < value) { *p = value; ++n; }
        }
        if (n && g_statsRecalcFn) g_statsRecalcFn(st); // single recalc after the batch
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Raise EVERY player-squad member (all tabs) to at least 'value' in every stat.
// Returns the count of members buffed. Used by the "buffpc" setup scene to bake a
// maxed-out save on a single client (no coop peer needed).
unsigned int buffAllPlayerStats(GameWorld* gw, float value) {
    EntityState sq[64];
    unsigned int n = captureSquad(gw, false, sq, 64);
    unsigned int total = 0;
    for (unsigned int i = 0; i < n; ++i) {
        unsigned int h[5] = { sq[i].hType, sq[i].hContainer, sq[i].hContainerSerial,
                              sq[i].hIndex, sq[i].hSerial };
        if (raiseAllStats(gw, h, value) > 0) ++total;
    }
    return total;
}

// ---- Protocol 18: carried-body sync ------------------------------------------

bool readCarry(Character* c, CarryRead* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!c) return false;
    __try {
        out->carrying     = c->isCarryingSomething;
        out->beingCarried = g_isBeingCarriedFn ? g_isBeingCarriedFn(c)
                                               : c->_isBeingCarried;
        if (out->carrying) {
            const hand& h = c->carryingObject;
            out->carried[0] = (unsigned int)h.type;
            out->carried[1] = h.container;
            out->carried[2] = h.containerSerial;
            out->carried[3] = h.index;
            out->carried[4] = h.serial;
        }
        out->valid = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool applyPickup(GameWorld* gw, Character* carrier, const unsigned int carriedHand[5]) {
    (void)gw;
    if (!carrier || !g_pickupObjectFn) return false;
    Character* who = resolveCharByHand(carriedHand[3], carriedHand[4], carriedHand[0],
                                       carriedHand[1], carriedHand[2]);
    if (!who) return false;
    __try {
        // Idempotent: already carrying exactly this body -> success no-op;
        // carrying something ELSE -> refuse (never stack / steal a carry).
        if (carrier->isCarryingSomething) {
            const hand& h = carrier->carryingObject;
            return (h.index == carriedHand[3] && h.serial == carriedHand[4]);
        }
        // The body is already on someone's shoulder locally (e.g. the real
        // carry raced the event on the owner side) - treat as done.
        if (g_isBeingCarriedFn && g_isBeingCarriedFn(who)) return true;
        g_pickupObjectFn(carrier, who);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool applyDrop(Character* carrier, bool ragdoll) {
    if (!carrier || !g_dropCarriedFn) return false;
    __try {
        if (!carrier->isCarryingSomething) return true; // idempotent no-op
        g_dropCarriedFn(carrier, ragdoll, /*removeOnly*/false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool carrySubject(GameWorld* gw, const unsigned int carrierHand[5],
                  const unsigned int carriedHand[5]) {
    Character* carrier = resolveCharByHand(carrierHand[3], carrierHand[4], carrierHand[0],
                                           carrierHand[1], carrierHand[2]);
    if (!carrier) return false;
    return applyPickup(gw, carrier, carriedHand);
}

bool dropSubject(GameWorld* gw, const unsigned int carrierHand[5], bool ragdoll) {
    (void)gw;
    Character* carrier = resolveCharByHand(carrierHand[3], carrierHand[4], carrierHand[0],
                                           carrierHand[1], carrierHand[2]);
    if (!carrier) return false;
    return applyDrop(carrier, ragdoll);
}

// ---- Protocol 19: furniture occupancy (beds + prison cages) ------------------

bool readFurniture(Character* c, FurnitureRead* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!c) return false;
    __try {
        if (c->inSomething == IN_BED)         out->kind = 1;
        else if (c->inSomething == IN_PRISON) out->kind = 2;
        else                                  out->kind = 0;
        if (out->kind != 0) {
            const hand& h = c->inWhat;
            out->furn[0] = (unsigned int)h.type;
            out->furn[1] = h.container;
            out->furn[2] = h.containerSerial;
            out->furn[3] = h.index;
            out->furn[4] = h.serial;
        }
        out->valid = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool applyFurniture(GameWorld* gw, Character* occupant,
                    const unsigned int furnHand[5], int kind, bool on) {
    (void)gw;
    if (!occupant || (kind != 1 && kind != 2)) return false;
    SetFurnModeFn fn = (kind == 1) ? g_setBedModeFn : g_setPrisonModeFn;
    if (!fn) return false;
    // Resolve the furniture OUTSIDE the SEH frame (resolveObjectByHand guards
    // itself). A null is only fatal for ENTER; an EXIT can fall back to the
    // occupant's own inWhat below.
    RootObject* ro = furnHand ? resolveObjectByHand(furnHand) : 0;
    __try {
        const int desired = on ? kind : 0;
        int cur = 0;
        if (occupant->inSomething == IN_BED)         cur = 1;
        else if (occupant->inSomething == IN_PRISON) cur = 2;
        if (cur == desired) return true;         // idempotent no-op
        if (on && cur != 0) return false;        // already in OTHER furniture: refuse
        // UseableStuff's first base chain starts at RootObject (offset 0), the
        // same address-preserving reinterpret the Building spawns use.
        UseableStuff* us = reinterpret_cast<UseableStuff*>(ro);
        if (!on && !us) {
            // Exit without a resolvable hand: release from whatever it is in.
            const hand& h = occupant->inWhat;
            unsigned int own[5];
            own[0] = (unsigned int)h.type; own[1] = h.container;
            own[2] = h.containerSerial;    own[3] = h.index; own[4] = h.serial;
            us = reinterpret_cast<UseableStuff*>(resolveObjectByHand(own));
        }
        if (on && !us) return false;             // enter needs the real fixture
        fn(occupant, on, us);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool putSubjectInFurniture(GameWorld* gw, const unsigned int subjHand[5], int kind, bool on) {
    Character* c = resolveCharByHand(subjHand[3], subjHand[4], subjHand[0],
                                     subjHand[1], subjHand[2]);
    if (!c) return false;
    RootObject* furn = findFurnitureNear(gw, kind);
    if (!furn) return false;
    unsigned int fh[5] = { 0, 0, 0, 0, 0 };
    if (!readObjectHand(furn, fh)) return false;
    return applyFurniture(gw, c, fh, kind, on);
}

bool taskIsBedPose(int t) {
    return t == (int)USE_BED || t == (int)USE_BED_ORDER || t == (int)SLEEP_ON_FLOOR;
}

// ---- Protocol 20: stealth mode + detection indicators ----------------------

bool readStealth(Character* c, StealthRead* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!c) return false;
    __try {
        out->sneaking = c->stealthMode;
        out->unseen   = (unsigned char)c->stealthUnseen.key;
        // whoSeesMeSneaking is a boost-1.60 unordered_map (kenshi/util/
        // OgreUnordered.h) and we compile against the same in-tree boost the
        // game shipped with, so in-process iteration is layout-correct. The
        // iterators are raw node pointers (trivially destructible - SEH-legal).
        ogre_unordered_map<hand, Character::WhoSeesMe>::type::iterator it =
            c->whoSeesMeSneaking.begin();
        ogre_unordered_map<hand, Character::WhoSeesMe>::type::iterator end =
            c->whoSeesMeSneaking.end();
        for (; it != end; ++it) {
            ++out->mapSize;
            if (out->nSeers >= 16) continue;
            StealthSeer& s = out->seers[out->nSeers];
            const hand& h = it->first;
            s.npc[0] = (unsigned int)h.type;
            s.npc[1] = h.container;
            s.npc[2] = h.containerSerial;
            s.npc[3] = h.index;
            s.npc[4] = h.serial;
            s.see  = (unsigned char)it->second.seeState.key;
            s.prog = it->second.progressOfMaybe;
            ++out->nSeers;
        }
        out->valid = true;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(out, 0, sizeof(*out));
        return false;
    }
}

int readStealthMode(Character* c) {
    if (!c) return -1;
    __try {
        return c->stealthMode ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool applyStealth(Character* c, bool on) {
    if (!c || !g_setStealthModeFn) return false;
    __try {
        if (c->stealthMode == on) return true; // idempotent no-op
        g_setStealthModeFn(c, on);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool applyStealthSeer(GameWorld* gw, Character* sneaker,
                      const unsigned int npcHand[5], unsigned char see, float prog) {
    (void)gw;
    if (!sneaker || !g_notifySeeSneakFn || !npcHand) return false;
    Character* seer = resolveCharByHand(npcHand[3], npcHand[4], npcHand[0],
                                        npcHand[1], npcHand[2]);
    if (!seer) return false;
    __try {
        int ynm = (int)see;
        g_notifySeeSneakFn(sneaker, seer, &ynm, prog);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool sneakSubject(GameWorld* gw, const unsigned int subjHand[5], bool on) {
    (void)gw;
    Character* c = resolveCharByHand(subjHand[3], subjHand[4], subjHand[0],
                                     subjHand[1], subjHand[2]);
    if (!c) return false;
    return applyStealth(c, on);
}

bool enterFurnitureNearPos(GameWorld* gw, Character* occupant, int kind,
                           float x, float y, float z, float radius) {
    if (!gw || !occupant || !g_getObjsFn || (kind != 1 && kind != 2)) return false;
    RootObject* best = 0;
    __try {
        Ogre::Vector3 center(x, y, z);
        g_npcQuery.clear();
        g_getObjsFn(gw, &g_npcQuery, &center, radius, BUILDING, 64, 0);
        unsigned int total = g_npcQuery.size();
        const char* bedPrefs[]  = { "camp bed", "bedroll", "bed" };
        const char* cagePrefs[] = { "prisoner cage", "cage" };
        const char** prefs = (kind == 2) ? cagePrefs : bedPrefs;
        const unsigned int nprefs = (kind == 2) ? 2u : 3u;
        float bestD2 = 1e18f;
        for (unsigned int i = 0; i < total; ++i) {
            RootObject* o = g_npcQuery[i];
            if (!o) continue;
            GameData* gd = o->getGameData();
            if (!gd) continue;
            bool match = false;
            for (unsigned int k = 0; k < nprefs && !match; ++k)
                if (ciContains(gd->name.c_str(), prefs[k])) match = true;
            if (!match) continue;
            Ogre::Vector3 p = o->getPosition();
            float dx = p.x - x, dz = p.z - z;
            float d2 = dx * dx + dz * dz;
            if (d2 < bestD2) { bestD2 = d2; best = o; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!best) return false;
    unsigned int fh[5] = { 0, 0, 0, 0, 0 };
    if (!readObjectHand(best, fh)) return false;
    return applyFurniture(gw, occupant, fh, kind, true);
}

bool readGameSpeed(GameWorld* gw, float* mult, bool* paused) {
    if (!gw || !mult || !paused) return false;
    __try {
        *mult   = gw->frameSpeedMult;
        *paused = gw->paused;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool readGameClock(GameWorld* gw, double* outHours, float* outHourLenSec) {
    if (outHours)      *outHours      = -1.0;
    if (outHourLenSec) *outHourLenSec = -1.0f;
    if (!gw || !g_getTimeHoursFn || !g_getHourLenFn) return false;
    __try {
        double hours = -1.0;
        g_getTimeHoursFn(gw, &hours); // retbuf ABI: this in RCX, retbuf in RDX
        if (outHours)      *outHours      = hours;
        if (outHourLenSec) *outHourLenSec = g_getHourLenFn(gw);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// NOTE: a clock STEP lever was prototyped here (write GameWorld::timeStamper's
// CPerfTimer base, self-verifying with revert-on-mismatch) and REJECTED: the
// verify never passed - getTimeStamp_inGameHours does not derive from that
// timer (run 150001), the absolute calendar is advanced elsewhere by the frame
// tick. The Replicator's slew (scale the join's sim speed until the offset
// closes) is the correction path; no engine clock writer exists.

bool writeGameSpeed(GameWorld* gw, float mult, bool paused) {
    if (!gw || !g_setGameSpeedFn || !g_userPauseFn) return false;
    if (mult <= 0.0f) paused = true; // wire convention: speed 0 == paused
    __try {
        g_userPauseFn(gw, paused);
        // Leave the multiplier untouched while paused, so unpausing restores
        // the pre-pause speed (matching the engine's own pause behaviour).
        if (!paused) g_setGameSpeedFn(gw, mult, /*click*/false);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool writeGameSpeedQuiet(GameWorld* gw, float mult, bool paused) {
    if (!gw || !g_setFrameSpeedMultFn || !g_userPauseFn) return false;
    if (mult <= 0.0f) paused = true; // wire convention: speed 0 == paused
    g_speedGuardWrite = true; // our own write: the intent hooks must not see it
    bool ok;
    __try {
        g_userPauseFn(gw, paused);
        // The bare multiplier setter: drives the sim WITHOUT the UI-button
        // notify setGameSpeed does - the buttons keep showing the local vote.
        if (!paused && mult > 0.0f) g_setFrameSpeedMultFn(gw, mult);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    g_speedGuardWrite = false;
    if (ok) {
        // Remember what we established: the intent poll treats any LATER
        // deviation from this as a user action.
        g_quietHave   = true;
        g_quietPaused = paused;
        if (!paused && mult > 0.0f) g_quietMult = mult;
    }
    // userPause re-highlights the buttons from the EFFECTIVE state (spike
    // finding); put the player's VOTE highlight back.
    restoreVoteButtons();
    return ok;
}

bool consumeSpeedIntent(GameWorld* gw, float* mult, bool* paused) {
    if (!mult || !paused) return false;
    // 1) Hook-captured intent (simulated writeGameSpeed clicks and any code
    //    path that does route through the public setters).
    if (g_speedIntentFresh) {
        g_speedIntentFresh = false;
        *mult   = g_speedIntentMult;
        *paused = g_speedIntentPaused;
        // Mark the current engine state EXPLAINED so the poll below doesn't
        // re-report this same action next tick.
        float em = 0.0f; bool ep = false;
        if (readGameSpeed(gw, &em, &ep)) {
            g_quietHave = true; g_quietPaused = ep;
            if (!ep && em > 0.0f) g_quietMult = em;
        }
        snapshotVoteButtons();
        return true;
    }
    // 2) Poll fallback (manual-session finding 2026-07-08: the MainBar click
    //    handler writes the speed INLINE - real UI clicks never reach the
    //    setGameSpeed detour). Two complementary signals:
    //      engine diff - the engine left the state WE last wrote: a click, a
    //        keyboard pause, or RE_Kenshi acted. The engine state IS the
    //        user's request.
    //      button diff - the highlight moved but the engine DIDN'T: a click
    //        on the speed equal to the current effective (the same-value
    //        vote). Its value also equals the engine state, by definition.
    float em = 0.0f; bool ep = false;
    if (!readGameSpeed(gw, &em, &ep)) return false;
    bool engineDiff = g_quietHave &&
        (ep != g_quietPaused ||
         (!ep && em > 0.0f && fabsf(em - g_quietMult) > 0.01f));
    bool btnDiff = false;
    char btn[16]; btn[0] = '\0';
    int nBtn = readSpeedButtons(btn, sizeof(btn));
    if (nBtn > 0 && g_voteBtnN == nBtn && memcmp(btn, g_voteBtn, nBtn) != 0)
        btnDiff = true;
    if (!engineDiff && !btnDiff) return false;
    if (engineDiff) {
        // Which multiplier is the request? A moved engine mult = the user
        // picked a new speed. A pause-only flip (space bar) PRESERVES the
        // requested mult - the engine's own model - so unpausing restores
        // the vote, not the effective.
        bool multMoved = (!ep && em > 0.0f && fabsf(em - g_quietMult) > 0.01f);
        *mult   = multMoved ? em : g_speedIntentMult;
        *paused = ep;
    } else {
        // Button-only diff: the engine state did NOT move, so the click's
        // meaning comes from WHICH button lit up. A speed click while PAUSED
        // is the big one (manual finding 2026-07-08): the click doesn't flip
        // the engine's pause flag, so reading *paused = engine would keep
        // voting pause forever (both clients stuck paused). Button 0 is the
        // pause button (probe-verified '1000' while paused); any other
        // selection is an UNPAUSE vote at that speed.
        int newIdx = -1;
        for (int i = 0; i < nBtn; ++i)
            if (btn[i] == '1' && g_voteBtn[i] != '1') { newIdx = i; break; }
        if (newIdx == 0) {
            *paused = true;
            *mult   = g_speedIntentMult; // pause preserves the requested mult
        } else if (newIdx > 0) {
            *paused = false;
            // The clicked speed: the engine mult when it equals the click (the
            // same-value case), else the best value the engine holds.
            *mult   = (em > 0.0f) ? em : g_speedIntentMult;
        } else {
            // Selection only turned OFF (shouldn't happen) - engine state wins.
            *paused = ep;
            *mult   = (em > 0.0f) ? em : g_speedIntentMult;
        }
    }
    g_speedIntentMult   = *mult;
    g_speedIntentPaused = ep;
    // Explain the state + adopt the clicked highlight as the new vote display.
    g_quietHave = true; g_quietPaused = ep;
    if (!ep && em > 0.0f) g_quietMult = em;
    if (nBtn > 0) { memcpy(g_voteBtn, btn, nBtn); g_voteBtnN = nBtn; }
    return true;
}

bool installSpeedIntentHooks(GameWorld* gw) {
    // Seed the intent state from the live engine BEFORE hooking, so the first
    // consume reflects the save's starting speed instead of the defaults.
    float m = 0.0f; bool p = false;
    if (readGameSpeed(gw, &m, &p)) {
        if (m > 0.0f) g_speedIntentMult = m;
        g_speedIntentPaused = p;
    }
    // Baseline vote highlight = the save's starting button state, so the first
    // quiet apply has something to restore before any real click happens.
    snapshotVoteButtons();
    intptr_t aSet    = KenshiLib::GetRealAddress(&GameWorld::setGameSpeed);
    intptr_t aPause  = KenshiLib::GetRealAddress(&GameWorld::userPause);
    intptr_t aToggle = KenshiLib::GetRealAddress(&GameWorld::togglePause);
    if (!aSet || !aPause || !aToggle) return false;
    if (KenshiLib::AddHook(aSet, (void*)&setGameSpeed_hook,
                           (void**)&g_setGameSpeedOrig) != KenshiLib::SUCCESS)
        return false;
    if (KenshiLib::AddHook(aPause, (void*)&userPause_hook,
                           (void**)&g_userPauseOrig) != KenshiLib::SUCCESS)
        return false;
    if (KenshiLib::AddHook(aToggle, (void*)&togglePause_hook,
                           (void**)&g_togglePauseOrig) != KenshiLib::SUCCESS)
        return false;
    return true;
}

int readSpeedButtons(char* out, int n) {
    if (!out || n < 2) return -1;
    out[0] = '\0';
    __try {
        if (!gui || !gui->mainbar) return -1;
        Ogre::FastArray<MyGUI::Button*>& btns = gui->mainbar->speedButtons;
        int cnt = (int)btns.size();
        if (cnt > n - 1) cnt = n - 1;
        for (int i = 0; i < cnt; ++i) {
            MyGUI::Button* b = btns[i];
            out[i] = (b && b->getStateSelected()) ? '1' : '0';
        }
        out[cnt] = '\0';
        return cnt;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        return -1;
    }
}


} // namespace engine
} // namespace coop
