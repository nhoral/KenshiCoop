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
class WeaponLootScenario : public Scenario {
public:
    WeaponLootScenario()
        : passed_(false), have_(false), isHost_(false),
          step_(0), added_(0), maxCount_(0), finalCount_(0), qual_(-1) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        sid_[0] = '\0';
    }
    virtual const char* name() const { return "weapon_loot"; }

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
    virtual bool passed() const { return passed_; }

private:
    bool         passed_, have_, isHost_;
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
class DropProbeScenario : public Scenario {
public:
    DropProbeScenario()
        : passed_(false), have_(false), step_(0), seeded_(0), dropped_(0),
          nearBefore_(0), nearAfter_(0), dropType_(0) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        sid_[0] = '\0';
    }

    virtual const char* name() const { return "drop_probe"; }

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

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long HOST_DURATION_MS = 14000;
    static const unsigned long JOIN_DURATION_MS = 12000;
    bool         passed_;
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

} // namespace

Scenario* makeWorldItemScenario(const std::string& name) {
    if (name == "drop_probe")   return new DropProbeScenario();
    if (name == "world_item_sync") return new WorldItemSyncScenario();
    if (name == "world_item_join") return new WorldItemSyncScenario(/*joinAuthor*/ true);
    if (name == "world_weapon_drop") return new WorldGearDropScenario("world_weapon_drop", 2);
    if (name == "world_armor_drop")  return new WorldGearDropScenario("world_armor_drop", 3);
    if (name == "weapon_loot")  return new WeaponLootScenario();
    return 0;
}

} // namespace coop
