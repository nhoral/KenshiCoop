// ScenarioWorldItems.cpp - ground-item / gear-conservation scenarios
// (monolith split from Scenario.cpp, 2026-07-12): world_weapon_drop,
// world_armor_drop, weapon_loot, drop_probe, world_item_sync,
// world_item_join. Classes are TU-private (anonymous namespace); only
// makeWorldItemScenario (ScenarioSupport.h) is exported.
// Must NOT: change any SCENARIO log string (oracle API, resources/CODE_MAP.md).

#include "ScenarioSupport.h"

namespace coop {
namespace {


// world_weapon_drop / world_armor_drop (Phase W2 oracle): CROSS-CLIENT conservation drop.
// The HOST owns the leader (tab 0) and DROPS a piece of its gear (the real action a player
// performs - for the armor variant an EQUIPPED piece, mirroring the 2026-07-07 session's
// "drag equipped pants to ground"); the conservation channel authors a DROP intent and the
// JOIN - which does NOT own the leader - must RELOCATE its own copy of that gear to the
// ground (Inventory::dropItem), NOT destroy it. The join asserts the gear both LEFT its
// leader's bag AND APPEARED as a free ground item (proving it crossed over by conservation,
// not by the inv-reconcile deleting an unreconstructable item). Roles split on isHost; both
// load the same save so they target the same leader. Parameterized on the gear category
// (2 = WEAPON, 3 = ARMOUR); both variants share the WDROP log contract and oracle.
class WorldGearDropScenario : public Scenario {
public:
    WorldGearDropScenario(const char* scenarioName, unsigned int gearCat)
        : passed_(false), have_(false), isHost_(false), baseType_(0), step_(0),
          invBase_(0), invAfter_(-1), grndAfter_(-1), invMin_(999), grndMax_(0),
          scenarioName_(scenarioName), gearCat_(gearCat) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        baseSid_[0] = '\0';
    }
    virtual const char* name() const { return scenarioName_; }

    virtual void onStart(const ScenarioContext& ctx) {
        isHost_ = ctx.isHost;
        have_ = findLeaderGear(ctx.gw, gearCat_, hand_, baseSid_, sizeof(baseSid_), &baseType_);
        if (have_) {
            invBase_ = invCount(ctx.gw);
            invMin_  = invBase_;
            grndMax_ = engine::countFreeGroundItemsNear(ctx.gw, hand_, baseSid_, baseType_, radius());
        }
        char b[200];
        _snprintf(b, sizeof(b) - 1, "WDROP start role=%s have=%d sid='%s' type=%u invBase=%d",
                  isHost_ ? "host" : "join", have_ ? 1 : 0,
                  baseSid_[0] ? baseSid_ : "(none)", baseType_, invBase_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!have_ || !baseSid_[0]) { if (ctx.elapsedMs >= 6000) { passed_ = false; return true; } return false; }

        if (isHost_) {
            // ACTOR: drop the leader's weapon once (unequip to loose first if worn).
            if (step_ == 0 && ctx.elapsedMs >= 8000) {
                step_ = 1;
                int dr = engine::dropItemFromInventory(ctx.gw, hand_, baseSid_, baseType_, 1);
                if (dr == 0) {
                    int un = engine::unequipItemToLoose(ctx.gw, hand_, baseSid_, baseType_, 1);
                    if (un > 0) dr = engine::dropItemFromInventory(ctx.gw, hand_, baseSid_, baseType_, 1);
                }
                invAfter_  = invCount(ctx.gw);
                grndAfter_ = engine::countFreeGroundItemsNear(ctx.gw, hand_, baseSid_, baseType_, radius());
                char b[160]; _snprintf(b, sizeof(b) - 1,
                    "WDROP host-dropped n=%d inv=%d ground=%d", dr, invAfter_, grndAfter_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        } else {
            // OBSERVER: each tick, track the min bag count + max ground count for the weapon.
            // A successful conservation relocation shows the bag LOSE it AND the ground GAIN it.
            int inv  = invCount(ctx.gw);
            int grnd = engine::countFreeGroundItemsNear(ctx.gw, hand_, baseSid_, baseType_, radius());
            if (inv  < invMin_) invMin_  = inv;
            if (grnd > grndMax_) grndMax_ = grnd;
        }

        if (ctx.elapsedMs >= 22000) {
            if (isHost_) {
                bool dropped = (invAfter_ >= 0) && (invAfter_ <= invBase_ - 1) && (grndAfter_ >= 1);
                passed_ = have_ && (invBase_ >= 1) && dropped;
                char b[200]; _snprintf(b, sizeof(b) - 1,
                    "WDROP verdict role=host pass=%d sid='%s' invBase=%d invAfter=%d grndAfter=%d",
                    passed_ ? 1 : 0, baseSid_, invBase_, invAfter_, grndAfter_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            } else {
                bool leftBag   = (invMin_ <= invBase_ - 1);
                bool onGround  = (grndMax_ >= 1);
                passed_ = have_ && (invBase_ >= 1) && leftBag && onGround;
                char b[220]; _snprintf(b, sizeof(b) - 1,
                    "WDROP verdict role=join pass=%d sid='%s' invBase=%d invMin=%d grndMax=%d relocated=%d",
                    passed_ ? 1 : 0, baseSid_, invBase_, invMin_, grndMax_, (leftBag && onGround) ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            return true;
        }
        return false;
    }
    virtual bool passed() const { return passed_; }

private:
    static const unsigned int MAX_SQUAD = 32;
    static float radius() { return 18.0f; }

    int invCount(GameWorld* gw) {
        InvItemEntry it[INV_ITEMS_MAX];
        unsigned int n = engine::captureContainerContents(gw, hand_, it, INV_ITEMS_MAX, 0);
        int c = 0;
        for (unsigned int i = 0; i < n; ++i)
            if (it[i].itemType == baseType_ && strcmp(it[i].stringID, baseSid_) == 0) ++c;
        return c;
    }
    // The squad LEADER (index 0 = tab 0, host-owned) and its first gear item of the
    // requested category (2 = WEAPON, 3 = ARMOUR), preferring an EQUIPPED piece.
    // Deterministic across clients (same save), so host (owner) drops it and join
    // (non-owner) mirrors.
    static bool findLeaderGear(GameWorld* gw, unsigned int gearCat, unsigned int out[5],
                               char* outSid, unsigned int outLen, unsigned int* outType) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ true, sq, MAX_SQUAD);
        if (n == 0) return false;
        unsigned int h[5] = { sq[0].hType, sq[0].hContainer, sq[0].hContainerSerial,
                              sq[0].hIndex, sq[0].hSerial };
        InvItemEntry it[INV_ITEMS_MAX];
        unsigned int cnt = engine::captureContainerContents(gw, h, it, INV_ITEMS_MAX, 0);
        for (unsigned int pass = 0; pass < 2; ++pass) {
            for (unsigned int i = 0; i < cnt; ++i) {
                if (it[i].itemType != gearCat) continue;
                if (pass == 0 && !it[i].equipped) continue; // prefer an equipped piece
                for (int k = 0; k < 5; ++k) out[k] = h[k];
                strncpy(outSid, it[i].stringID, outLen - 1); outSid[outLen - 1] = '\0';
                if (outType) *outType = it[i].itemType;
                return true;
            }
        }
        return false;
    }

    bool         passed_;
    bool         have_;
    bool         isHost_;
    unsigned int hand_[5];
    char         baseSid_[48];
    unsigned int baseType_;
    int          step_;
    int          invBase_, invAfter_, grndAfter_;
    int          invMin_, grndMax_;
    const char*  scenarioName_;
    unsigned int gearCat_;
};

// weapon_loot (weapon-fabrication sync validation): a weapon that exists in NO shared-save
// inventory enters play mid-session on the ACQUIRING client - the loot / vendor-buy /
// container-grab shape that used to exist ONLY on that client (the last trading loss
// vector). The HOST fabricates one NOVEL-sid weapon into its OWN leader's bag through the
// engine primitive (the same end-state mutation a UI acquisition produces: a brand-new
// Item in an owned inventory); the acquisition must cross to the JOIN through the
// per-character inventory snapshot channel + the peer-side weapon CREATE (spike-451
// recipe) with EXACTLY one copy on each side - fabrication racing the W2 conservation
// channel or the snapshot echo into dupes is the design risk this scenario gates.
// Both sides pick the sid deterministically (same gamedata + same save), so the join
// gates on the exact template appearing. WLOOT log contract; judged by Test-WeaponLoot.
class WeaponLootScenario : public TimedScenario {
public:
    WeaponLootScenario()
        : TimedScenario("weapon_loot", 0), have_(false), isHost_(false),
          step_(0), added_(0), maxCount_(0), finalCount_(0), qual_(-1) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        sid_[0] = '\0';
    }
    virtual void onStart(const ScenarioContext& ctx) {
        isHost_ = ctx.isHost;
        // The squad LEADER (index 0 = tab 0, host-owned) - deterministic across
        // clients (same save), so the host acquires and the join mirrors.
        EntityState sq[32];
        unsigned int n = engine::captureSquad(ctx.gw, /*leaderOnly*/ true, sq, 32);
        if (n > 0) {
            hand_[0] = sq[0].hType; hand_[1] = sq[0].hContainer;
            hand_[2] = sq[0].hContainerSerial; hand_[3] = sq[0].hIndex;
            hand_[4] = sq[0].hSerial;
            have_ = engine::commonNovelWeaponSid(ctx.gw, hand_, sid_, sizeof(sid_)) != 0;
        }
        char b[220];
        _snprintf(b, sizeof(b) - 1,
            "WLOOT start role=%s have=%d sid='%s' hand=%u,%u,%u,%u,%u",
            isHost_ ? "host" : "join", have_ ? 1 : 0, sid_[0] ? sid_ : "(none)",
            hand_[0], hand_[1], hand_[2], hand_[3], hand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!have_) { if (ctx.elapsedMs >= 6000) { passed_ = false; return true; } return false; }

        // ACQUIRE (host @8s): fabricate ONE novel weapon into the OWNED leader's bag.
        if (isHost_ && step_ == 0 && ctx.elapsedMs >= 8000) {
            step_ = 1;
            added_ = engine::addItemsToContainerBySid(ctx.gw, hand_, sid_,
                                                      /*WEAPON*/ 2u, 1, 0, "", "");
            char b[160]; _snprintf(b, sizeof(b) - 1,
                "WLOOT host-acquired sid='%s' added=%d", sid_, added_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // Census the leader's copies of the sid each tick (arrival + dupe watch).
        {
            int c = 0, q = -1;
            InvItemEntry it[INV_ITEMS_MAX];
            unsigned int n = engine::captureContainerContents(ctx.gw, hand_, it, INV_ITEMS_MAX, 0);
            for (unsigned int i = 0; i < n; ++i)
                if (it[i].itemType == 2u && strcmp(it[i].stringID, sid_) == 0) {
                    int qty = it[i].quantity; if (qty < 1) qty = 1;
                    c += qty; q = (int)it[i].quality;
                }
            if (c > maxCount_) maxCount_ = c;
            finalCount_ = c;
            if (q >= 0) qual_ = q;
        }

        if (ctx.elapsedMs >= 30000) {
            if (isHost_) {
                // Acquisition landed, persisted, and nothing (peer echo, W2 census,
                // reconcile churn) ever duplicated or destroyed it locally.
                passed_ = (added_ == 1) && (finalCount_ == 1) && (maxCount_ == 1);
                char b[240]; _snprintf(b, sizeof(b) - 1,
                    "WLOOT verdict role=host pass=%d sid='%s' added=%d final=%d max=%d qual=%d",
                    passed_ ? 1 : 0, sid_, added_, finalCount_, maxCount_, qual_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            } else {
                // The peer copy appeared (snapshot -> weapon CREATE), exactly once,
                // and never transiently duplicated.
                passed_ = (finalCount_ == 1) && (maxCount_ == 1);
                char b[240]; _snprintf(b, sizeof(b) - 1,
                    "WLOOT verdict role=join pass=%d sid='%s' final=%d max=%d qual=%d",
                    passed_ ? 1 : 0, sid_, finalCount_, maxCount_, qual_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            return true;
        }
        return false;
    }
private:
    bool         have_, isHost_;
    unsigned int hand_[5];
    char         sid_[48];
    int          step_, added_, maxCount_, finalCount_, qual_;
};

// drop_probe (Phase W0, DIAGNOSTIC): characterize what a player DROP produces, with no
// protocol changes. The host seeds a known loose item into its leader's bag, enumerates
// nearby world items (WEAPON/ARMOUR/ITEM/CONTAINER) as a BEFORE baseline, drops the item,
// then re-enumerates as AFTER. The log IS the deliverable: it tells us the dropped
// object's itemType/hand/pos and whether getObjectsWithinSphere enumerates it (the
// `enumerated=` verdict) - the facts the W1 interest-scan + proxy design depends on. The
// join is a passive observer (the drop is host-authored; W0 has no world-item channel).
class DropProbeScenario : public TimedScenario {
public:
    DropProbeScenario()
        : TimedScenario("drop_probe", 0), have_(false), step_(0), seeded_(0), dropped_(0),
          nearBefore_(0), nearAfter_(0), dropType_(0) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        sid_[0] = '\0';
    }

    virtual void onStart(const ScenarioContext& ctx) {
        have_ = engine::pickInventoryContainer(ctx.gw, hand_);
        char b[160];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO DROP anchor host=%d have=%d hand=%u,%u,%u,%u,%u",
            ctx.isHost ? 1 : 0, have_ ? 1 : 0,
            hand_[0], hand_[1], hand_[2], hand_[3], hand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Only the host drives the probe; the join just lives out its window.
        if (!ctx.isHost) {
            if (ctx.elapsedMs >= JOIN_DURATION_MS) { passed_ = true; return true; }
            return false;
        }
        if (!have_) { if (ctx.elapsedMs >= 6000) { passed_ = false; return true; } return false; }

        // @4s: seed a known loose item and read back its real itemType from the bag.
        if (step_ == 0 && ctx.elapsedMs >= 4000) {
            step_ = 1;
            seeded_ = engine::addTestItemsToContainer(ctx.gw, hand_, 1, sid_, sizeof(sid_));
            InvItemEntry items[INV_ITEMS_MAX];
            unsigned int n = engine::captureContainerContents(ctx.gw, hand_, items, INV_ITEMS_MAX, 0);
            for (unsigned int i = 0; i < n; ++i)
                if (!items[i].equipped && strcmp(items[i].stringID, sid_) == 0) {
                    dropType_ = items[i].itemType; break;
                }
            char b[200];
            _snprintf(b, sizeof(b) - 1, "SCENARIO DROP SEEDED added=%d sid='%s' type=%u",
                      seeded_, sid_[0] ? sid_ : "(none)", dropType_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // @6s: BEFORE enumeration baseline.
        if (step_ == 1 && ctx.elapsedMs >= 6000) {
            step_ = 2;
            coop::logLine("SCENARIO DROP BEFORE-scan:");
            engine::dumpWorldItems(ctx.gw);
            nearBefore_ = engine::countWorldItemsNear(ctx.gw, 30.0f);
            char b[120];
            _snprintf(b, sizeof(b) - 1, "SCENARIO DROP BEFORE near=%d", nearBefore_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // @8s: perform the drop.
        if (step_ == 2 && ctx.elapsedMs >= 8000) {
            step_ = 3;
            dropped_ = engine::dropItemFromInventory(ctx.gw, hand_, sid_, dropType_, 1);
            char b[200];
            _snprintf(b, sizeof(b) - 1, "SCENARIO DROP DROPPED dropped=%d sid='%s' type=%u",
                      dropped_, sid_[0] ? sid_ : "(none)", dropType_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // @10s: AFTER enumeration (let the engine settle the new ground object a couple s).
        if (step_ == 3 && ctx.elapsedMs >= 10000) {
            step_ = 4;
            coop::logLine("SCENARIO DROP AFTER-scan:");
            engine::dumpWorldItems(ctx.gw);
            nearAfter_ = engine::countWorldItemsNear(ctx.gw, 30.0f);
            int enumerated = (nearAfter_ > nearBefore_) ? 1 : 0;
            char b[200];
            _snprintf(b, sizeof(b) - 1,
                "SCENARIO DROP RESULT dropped=%d before=%d after=%d enumerated=%d",
                dropped_, nearBefore_, nearAfter_, enumerated);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (ctx.elapsedMs >= HOST_DURATION_MS) { passed_ = (step_ == 4 && dropped_ > 0); return true; }
        return false;
    }

private:
    static const unsigned long HOST_DURATION_MS = 14000;
    static const unsigned long JOIN_DURATION_MS = 12000;
    bool         have_;
    int          step_;
    int          seeded_;
    int          dropped_;
    int          nearBefore_;
    int          nearAfter_;
    unsigned int dropType_;
    unsigned int hand_[5];
    char         sid_[48];
};

// world_item_sync (Phase W1): host-authored ground-item visual sync. The HOST seeds a
// known item, DROPS it (a real free world item), then later DESPAWNS it. Both clients
// sample the interest sphere every 500 ms via captureWorldItems and log a machine-checkable
// "SCENARIO WI <HOST|JOIN> t=.. n=.. pos=.. hash=.." line (n = ground items seen; pos/hash
// from the first). The host's drop streams a netId-keyed snapshot to the join, which spawns
// a LOCAL proxy ground item (so the join's OWN captureWorldItems then enumerates it). The
// oracle asserts the join's observed item matches the host's pos (within tolerance) + CONTENT
// hash (exactly), then that the host's despawn culls the join's proxy cleanly (n -> 0 on both).
//
// world_item_join (W1 BIDIR): the same script with the JOIN as the author - the
// direction that never existed before the bidirectional W1 fix (join drops of
// materials/food were invisible on the host). The join drops + despawns; the HOST
// must spawn/cull the proxy. Same log contract, same oracle logic (roles swapped).
class WorldItemSyncScenario : public Scenario {
public:
    explicit WorldItemSyncScenario(bool joinAuthor = false)
        : joinAuthor_(joinAuthor), passed_(false), have_(false), step_(0), lastLogMs_(0),
          seeded_(0), dropped_(0), despawned_(0), dropType_(0),
          peakN_(0), lastN_(0) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        sid_[0] = '\0';
    }

    virtual const char* name() const { return joinAuthor_ ? "world_item_join" : "world_item_sync"; }

    virtual void onStart(const ScenarioContext& ctx) {
        have_ = engine::pickInventoryContainer(ctx.gw, hand_);
        char b[160];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO WI anchor host=%d have=%d hand=%u,%u,%u,%u,%u",
            ctx.isHost ? 1 : 0, have_ ? 1 : 0,
            hand_[0], hand_[1], hand_[2], hand_[3], hand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Sample the interest sphere on BOTH clients every 500 ms.
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            engine::WorldItemRaw raw[16];
            unsigned int n = engine::captureWorldItems(ctx.gw, raw, 16, 60.0f);
            lastN_ = n; if (n > peakN_) peakN_ = n;
            float x = (n > 0) ? raw[0].x : 0.0f, y = (n > 0) ? raw[0].y : 0.0f, z = (n > 0) ? raw[0].z : 0.0f;
            unsigned int hash = (n > 0) ? raw[0].hash : 0u;
            char b[200];
            _snprintf(b, sizeof(b) - 1,
                "SCENARIO WI %s t=%lu n=%u pos=%.2f,%.2f,%.2f hash=%u",
                ctx.isHost ? "HOST" : "JOIN", (unsigned long)ctx.elapsedMs, n, x, y, z, hash);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        const bool author = (ctx.isHost != joinAuthor_);
        if (author && have_) {
            // @5s: seed a known item, read its real itemType, then DROP it to the ground.
            if (step_ == 0 && ctx.elapsedMs >= 5000) {
                step_ = 1;
                seeded_ = engine::addTestItemsToContainer(ctx.gw, hand_, 1, sid_, sizeof(sid_));
                InvItemEntry items[INV_ITEMS_MAX];
                unsigned int n = engine::captureContainerContents(ctx.gw, hand_, items, INV_ITEMS_MAX, 0);
                for (unsigned int i = 0; i < n; ++i)
                    if (!items[i].equipped && strcmp(items[i].stringID, sid_) == 0) { dropType_ = items[i].itemType; break; }
                dropped_ = engine::dropItemFromInventory(ctx.gw, hand_, sid_, dropType_, 1);
                char b[200];
                _snprintf(b, sizeof(b) - 1, "SCENARIO WI DROP seeded=%d dropped=%d sid='%s' type=%u",
                          seeded_, dropped_, sid_[0] ? sid_ : "(none)", dropType_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            // @16s: DESPAWN the dropped item so the host culls it and the join removes its proxy.
            if (step_ == 1 && ctx.elapsedMs >= 16000) {
                step_ = 2;
                despawned_ = engine::destroyWorldItemsNear(ctx.gw, 60.0f);
                char b[120];
                _snprintf(b, sizeof(b) - 1, "SCENARIO WI DESPAWN destroyed=%d", despawned_);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            if (ctx.isHost != joinAuthor_)
                passed_ = have_ && (dropped_ > 0) && (despawned_ > 0); // the author's legs
            else
                passed_ = true; // observer; the runner's WI oracle is authoritative
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long HOST_DURATION_MS = 26000; // outlive the join's window
    static const unsigned long JOIN_DURATION_MS = 22000;
    const bool    joinAuthor_;
    bool          passed_;
    bool          have_;
    int           step_;
    unsigned long lastLogMs_;
    int           seeded_;
    int           dropped_;
    int           despawned_;
    unsigned int  dropType_;
    unsigned int  peakN_;
    unsigned int  lastN_;
    unsigned int  hand_[5];
    char          sid_[48];
};

// world_item_drop (query-free NON-gear drop-and-APPEAR): the town-hardening path.
// The AUTHOR (host by default) seeds a common NON-gear item, then DROPS it via
// engine::dropItemFromInventory -> Inventory::dropItem -> the hooked _NV_dropItem,
// so the drop is captured QUERY-FREE ([wi] DROP-CAP) rather than re-found by the
// spatial scan. The peer must SPAWN a proxy at the captured spot, and - because
// culling is now handle-based (real Item* gone), not spatial-scan-miss - the item
// must STAY put without flicker for the rest of the run. Both clients sample the
// interest sphere every 500 ms; BOTH assert the item APPEARED (n>=1 after the drop
// settles) and PERSISTED (never dropped back to 0 once seen) - a flicker or a
// premature cull fails the persistence leg. No despawn here (world_item_sync
// already covers the cull leg); this isolates appear + stay.
class WorldItemDropScenario : public TimedScenario {
public:
    WorldItemDropScenario()
        : TimedScenario("world_item_drop", 0), have_(false), lastLogMs_(0), step_(0),
          seeded_(0), dropped_(0), dropType_(0),
          sawItem_(false), persistOk_(true) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        sid_[0] = '\0';
    }

    virtual void onStart(const ScenarioContext& ctx) {
        have_ = engine::pickInventoryContainer(ctx.gw, hand_);
        char b[160];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO WID anchor host=%d have=%d hand=%u,%u,%u,%u,%u",
            ctx.isHost ? 1 : 0, have_ ? 1 : 0,
            hand_[0], hand_[1], hand_[2], hand_[3], hand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            engine::WorldItemRaw raw[16];
            unsigned int n = engine::captureWorldItems(ctx.gw, raw, 16, 60.0f);
            // Appear + persist tracking (both clients). Only judge AFTER the drop
            // has had time to fire and replicate.
            if (ctx.elapsedMs >= APPEAR_MS) {
                if (n >= 1) sawItem_ = true;
                else if (sawItem_) persistOk_ = false; // vanished after being seen = flicker/premature cull
            }
            float x = (n > 0) ? raw[0].x : 0.0f, y = (n > 0) ? raw[0].y : 0.0f, z = (n > 0) ? raw[0].z : 0.0f;
            unsigned int hash = (n > 0) ? raw[0].hash : 0u;
            char b[200];
            _snprintf(b, sizeof(b) - 1,
                "SCENARIO WID %s t=%lu n=%u seen=%d persist=%d pos=%.2f,%.2f,%.2f hash=%u",
                ctx.isHost ? "HOST" : "JOIN", (unsigned long)ctx.elapsedMs, n,
                sawItem_ ? 1 : 0, persistOk_ ? 1 : 0, x, y, z, hash);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // HOST authors the drop through the hooked inventory drop path.
        if (ctx.isHost && have_ && step_ == 0 && ctx.elapsedMs >= 5000) {
            step_ = 1;
            seeded_ = engine::addTestItemsToContainer(ctx.gw, hand_, 1, sid_, sizeof(sid_));
            InvItemEntry items[INV_ITEMS_MAX];
            unsigned int n = engine::captureContainerContents(ctx.gw, hand_, items, INV_ITEMS_MAX, 0);
            for (unsigned int i = 0; i < n; ++i)
                if (!items[i].equipped && strcmp(items[i].stringID, sid_) == 0) { dropType_ = items[i].itemType; break; }
            dropped_ = engine::dropItemFromInventory(ctx.gw, hand_, sid_, dropType_, 1);
            char b[200];
            _snprintf(b, sizeof(b) - 1, "SCENARIO WID DROP seeded=%d dropped=%d sid='%s' type=%u",
                      seeded_, dropped_, sid_[0] ? sid_ : "(none)", dropType_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            // Both sides must have SEEN the ground item and kept it (no flicker).
            // The host additionally proves it actually authored the drop.
            bool appeared = sawItem_ && persistOk_;
            passed_ = ctx.isHost ? (have_ && dropped_ > 0 && appeared) : appeared;
            char m[160];
            _snprintf(m, sizeof(m) - 1,
                "SCENARIO WID verdict host=%d dropped=%d seen=%d persist=%d pass=%d",
                ctx.isHost ? 1 : 0, dropped_, sawItem_ ? 1 : 0, persistOk_ ? 1 : 0, passed_ ? 1 : 0);
            m[sizeof(m) - 1] = '\0'; coop::logLine(m);
            return true;
        }
        return false;
    }

private:
    static const unsigned long HOST_DURATION_MS = 24000; // outlive the join's window
    static const unsigned long JOIN_DURATION_MS = 20000;
    static const unsigned long APPEAR_MS        = 9000;  // drop@5s + replication slack
    bool          have_;
    unsigned long lastLogMs_;
    int           step_;
    int           seeded_;
    int           dropped_;
    unsigned int  dropType_;
    bool          sawItem_;
    bool          persistOk_;
    unsigned int  hand_[5];
    char          sid_[48];
};

// rejoin_items (Phase 3 item-dup fix): a reload must NOT duplicate save-native
// ground items. The HOST drops K test items (both clients reach n0+K), issues a
// coordinated saveGameAs (the saveSync half streams the join a byte-identical
// copy; its PKT_SAVE_ACK is the "join holds my copy" gate) so the drops bake
// into the shared save, then loads it MID-SESSION. After the swap those drops
// are save-natives on BOTH clients: the first-scan baseline (worldSeeded_) must
// record them as never-emit so the host does NOT re-stream them and the join
// does NOT mint a duplicate proxy on top of its own native. WITHOUT the fix the
// re-scan re-streams all n0+K and the join layers n0+K proxies -> ~2*(n0+K).
// Both clients census the interest sphere every 500 ms (SCENARIO RI rows); the
// oracle gates that the POST-reload count did not grow past the PRE-reload count
// on either side (no +K layer) and that a WORLD-RELOAD edge actually occurred.
// Reuses the load_sync save/ACK/load coordination + world-swap detection.
class RejoinItemsScenario : public TimedScenario {
public:
    RejoinItemsScenario()
        : TimedScenario("rejoin_items", 0), have_(false), lastLogMs_(0), step_(0),
          baselineN_(-1), preReloadN_(-1), postReloadN_(-1), lastN_(0),
          dropped_(0), saveIssued_(false), saveOk_(false), ackSeen_(false),
          ackOk_(false), loadIssued_(false), loadOk_(false),
          dropStartMs_(0), swapSeen_(false), swapDone_(false), censusAtMs_(0),
          postCensused_(false), sigWas2_(false), sigClearedMs_(0), lastStatusMs_(0) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        sid_[0] = '\0';
    }

    virtual void onStart(const ScenarioContext& ctx) {
        have_ = engine::pickInventoryContainer(ctx.gw, hand_);
        char b[160];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO RI anchor host=%d have=%d hand=%u,%u,%u,%u,%u",
            ctx.isHost ? 1 : 0, have_ ? 1 : 0,
            hand_[0], hand_[1], hand_[2], hand_[3], hand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        bool live = engine::gameplayLive(ctx.gw);

        // Census the interest sphere on BOTH clients every 500 ms (when live).
        if (live && (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0)) {
            lastLogMs_ = ctx.elapsedMs;
            engine::WorldItemRaw raw[MAXW];
            unsigned int n = engine::captureWorldItems(ctx.gw, raw, MAXW, 60.0f);
            lastN_ = (int)n;
            char b[176];
            _snprintf(b, sizeof(b) - 1,
                "SCENARIO RI %s t=%lu n=%u step=%d swapDone=%d",
                ctx.isHost ? "HOST" : "JOIN", (unsigned long)ctx.elapsedMs, n,
                step_, swapDone_ ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // ---- Host script: baseline -> drop K -> coordinated save -> load ----
        if (ctx.isHost && have_) {
            if (step_ == 0 && live && ctx.elapsedMs >= 4000) {
                step_ = 1;
                baselineN_ = lastN_;
                char b[96]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO RI BASELINE n=%d t=%lu", baselineN_, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            if (step_ == 1 && ctx.elapsedMs >= 6000) {
                step_ = 2;
                for (int k = 0; k < DROP_K; ++k) {
                    char s[48]; s[0] = '\0';
                    int added = engine::addTestItemsToContainer(ctx.gw, hand_, 1, s, sizeof(s));
                    if (added <= 0 || !s[0]) continue;
                    unsigned int dtype = 0;
                    InvItemEntry items[INV_ITEMS_MAX];
                    unsigned int n = engine::captureContainerContents(ctx.gw, hand_, items, INV_ITEMS_MAX, 0);
                    for (unsigned int i = 0; i < n; ++i)
                        if (!items[i].equipped && strcmp(items[i].stringID, s) == 0) { dtype = items[i].itemType; break; }
                    if (engine::dropItemFromInventory(ctx.gw, hand_, s, dtype, 1) > 0) ++dropped_;
                }
                char b[128]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO RI DROP k=%d dropped=%d t=%lu", DROP_K, dropped_, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            // @14s: the drops have replicated to the join; issue the coordinated save.
            if (step_ == 2 && ctx.elapsedMs >= 14000) {
                step_ = 3;
                saveIssued_ = true;
                saveOk_ = engine::saveGameAs(SAVE_NAME);
                char b[112]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO RI SAVE name='%s' ok=%d t=%lu", SAVE_NAME, saveOk_ ? 1 : 0, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            if (step_ == 3 && saveIssued_ && !ackSeen_ && savexfer::lastAckXferId() != 0) {
                ackSeen_ = true; ackOk_ = (savexfer::lastAckOk() == 1);
                char b[96]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO RI ACK ok=%d t=%lu", ackOk_ ? 1 : 0, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
            if (step_ == 3 && ackSeen_ && ackOk_ && !loadIssued_ && live) {
                step_ = 4;
                loadIssued_ = true;
                loadOk_ = engine::loadSave(SAVE_NAME);
                char b[96]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO RI LOAD ok=%d t=%lu", loadOk_ ? 1 : 0, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        // The JOIN is reactive: the Plugin's coordinated-load GO handler issues
        // its own bypass-once load; the scenario just watches for the swap.

        // ---- Both sides: world-swap tracking (mirrors LoadSyncScenario) -------
        if (!live && dropStartMs_ == 0) {
            dropStartMs_ = ctx.elapsedMs;
            if (!swapSeen_) {
                swapSeen_ = true;
                preReloadN_ = lastN_; // last count seen while live = the pre-reload total
                char b[112]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO RI PRERELOAD n=%d t=%lu", preReloadN_, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        } else if (live && dropStartMs_ != 0) {
            unsigned long ms = ctx.elapsedMs - dropStartMs_;
            dropStartMs_ = 0;
            if (ms >= SWAP_MIN_MS && !swapDone_) {
                swapDone_ = true;
                censusAtMs_ = ctx.elapsedMs + CENSUS_SETTLE_MS;
                char b[112]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO RI SWAPDONE swapMs=%lu t=%lu", ms, ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        // Synchronous swap path: execute() rebuilds inside one call (no visible
        // live drop) - latch off the LOADGAME signal being consumed.
        {
            int sig = engine::saveMgrSignal(0);
            if (sig == 2) { sigWas2_ = true; sigClearedMs_ = 0; }
            else if (sigWas2_ && sigClearedMs_ == 0) sigClearedMs_ = ctx.elapsedMs;
            if (!swapDone_ && sigWas2_ && sigClearedMs_ != 0 && live &&
                dropStartMs_ == 0 && ctx.elapsedMs >= sigClearedMs_ + SYNC_CONFIRM_MS) {
                if (preReloadN_ < 0) preReloadN_ = lastN_;
                swapDone_ = true;
                censusAtMs_ = ctx.elapsedMs + CENSUS_SETTLE_MS;
                char b[128]; _snprintf(b, sizeof(b) - 1,
                    "SCENARIO RI SWAPDONE t=%lu (synchronous inside execute)", ctx.elapsedMs);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }

        // ---- Both sides: post-reload census ---------------------------------
        if (swapDone_ && !postCensused_ && live && ctx.elapsedMs >= censusAtMs_) {
            postCensused_ = true;
            engine::WorldItemRaw raw[MAXW];
            unsigned int n = engine::captureWorldItems(ctx.gw, raw, MAXW, 60.0f);
            postReloadN_ = (int)n;
            char b[160]; _snprintf(b, sizeof(b) - 1,
                "SCENARIO RI POSTRELOAD n=%d pre=%d delta=%d t=%lu",
                postReloadN_, preReloadN_, postReloadN_ - preReloadN_, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        if (ctx.elapsedMs - lastStatusMs_ >= 5000) {
            lastStatusMs_ = ctx.elapsedMs;
            char b[200]; _snprintf(b, sizeof(b) - 1,
                "SCENARIO RI STATE host=%d dropped=%d save=%d ack=%d load=%d "
                "swapDone=%d post=%d base=%d pre=%d postN=%d live=%d t=%lu",
                ctx.isHost ? 1 : 0, dropped_, saveOk_ ? 1 : 0, ackOk_ ? 1 : 0,
                loadOk_ ? 1 : 0, swapDone_ ? 1 : 0, postCensused_ ? 1 : 0,
                baselineN_, preReloadN_, postReloadN_, live ? 1 : 0, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        bool localDone = ctx.isHost
            ? (dropped_ > 0 && saveOk_ && ackOk_ && loadOk_ && swapDone_ && postCensused_)
            : (swapDone_ && postCensused_);
        if ((localDone && ctx.elapsedMs >= censusAtMs_ + TAIL_HOLD_MS) ||
            ctx.elapsedMs >= DURATION_MS) {
            // The cross-client no-duplication verdict (join count must not EXCEED
            // the host's authoritative native count post-reload) needs BOTH logs,
            // so the runner's Test-RejoinItems oracle owns it. Locally each side
            // only proves it drove its own legs: the HOST authored drop+save+load
            // and did not balloon its OWN count (host mints no proxy for its own
            // stream, so host post == pre is the fixed behavior); the JOIN merely
            // survived the reload and censused (the join CANNOT see the host count
            // to judge parity). The join count legitimately grows past its pre-
            // reload value when the drops were outside its interest sphere until
            // the reload co-located it - which is why parity, not pre-vs-post, is
            // the gate.
            bool hostNoDup = (postReloadN_ >= 0 && preReloadN_ >= 0 && postReloadN_ <= preReloadN_);
            passed_ = ctx.isHost
                ? (dropped_ > 0 && saveOk_ && ackOk_ && loadOk_ && swapDone_ && postCensused_ && hostNoDup)
                : (swapDone_ && postCensused_);
            char b[200]; _snprintf(b, sizeof(b) - 1,
                "SCENARIO RI verdict host=%d pass=%d base=%d pre=%d post=%d hostNoDup=%d "
                "dropped=%d save=%d ack=%d load=%d",
                ctx.isHost ? 1 : 0, passed_ ? 1 : 0, baselineN_, preReloadN_, postReloadN_,
                hostNoDup ? 1 : 0, dropped_, saveOk_ ? 1 : 0, ackOk_ ? 1 : 0, loadOk_ ? 1 : 0);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return true;
        }
        return false;
    }

private:
    static const unsigned int  MAXW            = 64;
    static const int           DROP_K          = 3;
    static const unsigned long SWAP_MIN_MS     = 400;    // Plugin's flicker floor
    static const unsigned long SYNC_CONFIRM_MS = 3000;   // no-drop window after sig clear
    static const unsigned long CENSUS_SETTLE_MS= 6000;   // let the reloaded world settle
    static const unsigned long TAIL_HOLD_MS    = 8000;
    static const unsigned long DURATION_MS     = 150000;

    bool          have_;
    unsigned long lastLogMs_;
    int           step_;
    int           baselineN_, preReloadN_, postReloadN_, lastN_;
    int           dropped_;
    bool          saveIssued_, saveOk_, ackSeen_, ackOk_, loadIssued_, loadOk_;
    unsigned long dropStartMs_;
    bool          swapSeen_, swapDone_;
    unsigned long censusAtMs_;
    bool          postCensused_, sigWas2_;
    unsigned long sigClearedMs_, lastStatusMs_;
    unsigned int  hand_[5];
    char          sid_[48];

    static const char* const SAVE_NAME;
};
const char* const RejoinItemsScenario::SAVE_NAME = "coopresume";

} // namespace

Scenario* makeWorldItemScenario(const std::string& name) {
    if (name == "drop_probe")   return new DropProbeScenario();
    if (name == "world_item_sync") return new WorldItemSyncScenario();
    if (name == "world_item_drop") return new WorldItemDropScenario();
    if (name == "world_item_join") return new WorldItemSyncScenario(/*joinAuthor*/ true);
    if (name == "world_weapon_drop") return new WorldGearDropScenario("world_weapon_drop", 2);
    if (name == "world_armor_drop")  return new WorldGearDropScenario("world_armor_drop", 3);
    if (name == "weapon_loot")  return new WeaponLootScenario();
    if (name == "rejoin_items") return new RejoinItemsScenario();
    return 0;
}

} // namespace coop
