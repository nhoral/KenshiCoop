// EngineInventory.cpp - inventory + world items (monolith split from
// EngineInternal.cpp, 2026-07-12): Phase 4a container-contents capture/reconcile,
// item template finders + createItemAndAdd, equip/unequip levers, Phase W0/W1
// world-item (ground drop) diagnostics + replication proxies, and the spike-401
// research tech-tree store probe. (The spike-451 weapon-mint recipe trace +
// diagWeaponCreate moved to the harness-only EngineProbe.cpp in Phase 5e.)
//
// Owner state: section-private statics/anon-namespace helpers only (r401 lever
// cache).
// Must NOT: define g_* engine pointers (EngineInternal.cpp owns them - EngineInternal.h
// declares them), install hooks, or change any log string - log phrasing is
// the API consumed by the PowerShell oracles (see resources/CODE_MAP.md).

#include "EngineInternal.h"

namespace coop {
namespace engine {

// ---- Phase 4a: container-contents (inventory) capture / reconcile ----------

RootObject* resolveObjectByHand(const unsigned int cHand[5]) {
    // object-order adapter over the typed resolve: cHand = {type, container,
    // containerSerial, index, serial}. resolveObject (EngineEntity.cpp) owns the
    // native hand-ctor arg order + SEH; this just converts the legacy array.
    if (!cHand) return 0;
    return resolveObject(ObjectHand::fromObjOrder(cHand));
}

namespace {

// invEntryHash / sectionNameHash moved to ../../netproto/ContentHash.h (shared
// with the prototest unit layer, which locks their cross-client stability).
using coop::sectionNameHash;
using coop::invEntryHash;

// SEH-guarded helper: copy an item's manufacturer + material GameData stringIDs into the
// snapshot entry. WEAPONS need them to be reconstructable on the peer (createItem requires
// the manufacturer/mesh GameData); armour/items leave them empty (the pointers are null).
void fillItemProvenance(Item* it, InvItemEntry& e) {
    __try {
        GameData* man = it->manufacturerData;
        GameData* mat = it->materialData;
        if (man) { const char* s = man->stringID.c_str(); strncpy(e.manufacturer, s ? s : "", sizeof(e.manufacturer) - 1); }
        if (mat) { const char* s = mat->stringID.c_str(); strncpy(e.material,     s ? s : "", sizeof(e.material) - 1); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

} // namespace

// SEH-guarded: read a container's contents - both LOOSE items and EQUIPPED gear
// (each tagged with its equipped flag + slot) - into out[] and, when outItems != 0,
// the matching Item* for each entry (used by the reconcile to remove excess stacks).
// Reads template stringID + type + stack quantity + quality bucket off each Item.
// Returns the count written.
// External linkage (EngineInternal.h): the world TU's vendor probe shares it.
unsigned int readInvItems(Inventory* inv, InvItemEntry* out, Item** outItems,
                          unsigned int maxOut) {
    if (!inv || !out || maxOut == 0) return 0;
    unsigned int n = 0;
    __try {
        lektor<Item*>& all = inv->_allItems;
        unsigned int total = all.size();
        for (unsigned int i = 0; i < total && n < maxOut; ++i) {
            Item* it = all[i];
            if (!it) continue;
            GameData* gd = it->getGameData();
            if (!gd) continue;
            memset(&out[n], 0, sizeof(InvItemEntry));
            const char* sid = gd->stringID.c_str();
            strncpy(out[n].stringID, sid ? sid : "", sizeof(out[n].stringID) - 1);
            out[n].itemType = (unsigned int)gd->type;
            int q = it->quantity; if (q < 1) q = 1;
            out[n].quantity = (q > 65535) ? (unsigned short)65535 : (unsigned short)q;
            float ql = it->quality; if (ql < 0.0f) ql = 0.0f;
            out[n].quality = (unsigned short)(ql * 100.0f);
            if (it->isEquipped) continue; // worn gear is appended below, from equip sections
            out[n].equipped = 0;
            out[n].slot     = (unsigned char)((unsigned int)it->slotType & 0xFFu);
            out[n].section  = 0; // loose: no equip section
            // Phase 6b: locked shackle bit (LockedArmour with a live lock). Direct
            // virtual dispatch (like getGameData above) discriminates the item; the
            // outer __try covers a fault.
            { LockedArmour* la = it->isLockedArmour(); out[n].locked = (la && la->lock) ? 1 : 0; }
            fillItemProvenance(it, out[n]); // weapon manufacturer/material (empty otherwise)
            if (outItems) outItems[n] = it;
            ++n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return n;
    }
    // Append EQUIPPED gear (Phase 4a equipment sync). Worn items - EVERY equippable
    // slot: weapons, all armour pieces, belt, worn backpack, ... - live in the
    // inventory's equip SECTIONS, not the loose _allItems list. Walk every section and
    // capture the ones flagged isAnEquippedItemSection, tagging each item equipped=1
    // with the section's slot. This covers all slots uniformly (the prior armour/weapon
    // getters silently missed the weapon holster, so weapon unequips never replicated).
    if (g_getSectionsFn) {
        __try {
            lektor<InventorySection*>* secs = g_getSectionsFn(inv);
            unsigned int ns = secs ? secs->size() : 0;
            for (unsigned int s = 0; s < ns && n < maxOut; ++s) {
                InventorySection* sec = (*secs)[s];
                if (!sec || !sec->isAnEquippedItemSection) continue;
                const Ogre::vector<InventorySection::SectionItem>::type& its = sec->items;
                unsigned int ni = (unsigned int)its.size();
                for (unsigned int i = 0; i < ni && n < maxOut; ++i) {
                    Item* it = its[i].item;
                    if (!it) continue;
                    GameData* gd = it->getGameData();
                    if (!gd) continue;
                    memset(&out[n], 0, sizeof(InvItemEntry));
                    const char* sid = gd->stringID.c_str();
                    strncpy(out[n].stringID, sid ? sid : "", sizeof(out[n].stringID) - 1);
                    out[n].itemType = (unsigned int)gd->type;
                    int q = it->quantity; if (q < 1) q = 1;
                    out[n].quantity = (q > 65535) ? (unsigned short)65535 : (unsigned short)q;
                    float ql = it->quality; if (ql < 0.0f) ql = 0.0f;
                    out[n].quality = (unsigned short)(ql * 100.0f);
                    out[n].equipped = 1;
                    out[n].slot     = (unsigned char)((unsigned int)sec->limitedSlot & 0xFFu);
                    // Phase 6b: locked shackle bit on the equipped LockedArmour.
                    { LockedArmour* la = it->isLockedArmour(); out[n].locked = (la && la->lock) ? 1 : 0; }
                    // Carry the SECTION identity so the two weapon slots ('hip' vs
                    // 'back', both ATTACH_WEAPON) replicate to the SAME slot on the peer.
                    out[n].section  = sectionNameHash(sec->name.c_str());
                    fillItemProvenance(it, out[n]); // weapon manufacturer/material
                    if (outItems) outItems[n] = it;
                    ++n;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return n;
        }
    }
    // Worn WEAPONS may not live in any section: a HELD/primary weapon is tracked only by
    // getPrimaryWeapon()/getSecondaryWeapon(). A sheathed weapon often ALSO appears in a
    // 'hip'/'back' equip section (captured above), so dedup by (stringID, itemType) below.
    // Capturing both pointers guarantees equipping/unequipping ANY weapon replicates -
    // without this a re-equipped weapon (routed to the held slot, which is in no section)
    // disappears from the snapshot and the peer never sees it come back.
    if (g_getPrimaryWeaponFn || g_getSecondaryWeaponFn) {
        __try {
            Item* wpns[2];
            wpns[0] = g_getPrimaryWeaponFn ? g_getPrimaryWeaponFn(inv) : 0;
            wpns[1] = g_getSecondaryWeaponFn ? g_getSecondaryWeaponFn(inv) : 0;
            for (int w = 0; w < 2 && n < maxOut; ++w) {
                Item* it = wpns[w];
                if (!it) continue;
                GameData* gd = it->getGameData();
                if (!gd) continue;
                const char* sid = gd->stringID.c_str();
                unsigned int wtype = (unsigned int)gd->type;
                // Dedup by (stringID, itemType) against already-captured EQUIPPED entries:
                // a sheathed weapon is exposed BOTH as a section item and via this pointer,
                // and the two are distinct Item* objects, so pointer identity won't catch
                // it. Matching on template avoids double-counting the same worn weapon.
                bool dup = false;
                for (unsigned int j = 0; j < n; ++j)
                    if (out[j].equipped && out[j].itemType == wtype &&
                        strncmp(out[j].stringID, sid ? sid : "",
                                sizeof(out[j].stringID)) == 0) { dup = true; break; }
                if (dup) continue;
                memset(&out[n], 0, sizeof(InvItemEntry));
                strncpy(out[n].stringID, sid ? sid : "", sizeof(out[n].stringID) - 1);
                out[n].itemType = (unsigned int)gd->type;
                int q = it->quantity; if (q < 1) q = 1;
                out[n].quantity = (q > 65535) ? (unsigned short)65535 : (unsigned short)q;
                float ql = it->quality; if (ql < 0.0f) ql = 0.0f;
                out[n].quality = (unsigned short)(ql * 100.0f);
                out[n].equipped = 1;
                out[n].slot     = (unsigned char)((unsigned int)it->slotType & 0xFFu);
                // A pointer-only weapon isn't in a getAllSections() section, so there is
                // no section name to hash - leave 0 (the peer equips it to the default
                // weapon slot; sheathed weapons are captured WITH a section above).
                out[n].section  = 0;
                fillItemProvenance(it, out[n]); // weapon manufacturer/material (required to recreate)
                if (outItems) outItems[n] = it;
                ++n;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return n;
        }
    }
    return n;
}

// SEH-guarded: find an item template by stringID within its itemType category.
// External linkage (EngineInternal.h): the spawn/combat TU's limb-item mint shares it.
GameData* findItemTemplateImpl(GameWorld* gw, const char* sid, unsigned int typeCat) {
    if (!gw || !g_getDataOfTypeFn || !sid || !sid[0]) return 0;
    __try {
        g_dataScratch.clear();
        g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, (itemType)typeCat);
        unsigned int n = g_dataScratch.size();
        for (unsigned int i = 0; i < n; ++i) {
            GameData* gd = g_dataScratch[i];
            if (gd && strcmp(gd->stringID.c_str(), sid) == 0) return gd;
        }
        static int dbg = -1;
        if (dbg < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dbg = (e && e[0] == '1') ? 1 : 0; }
        if (dbg) {
            char b[200];
            _snprintf(b, sizeof(b) - 1, "[tmpl] MISS sid='%s' type=%u scanned=%u sample0='%s'",
                sid, typeCat, n, (n > 0 && g_dataScratch[0]) ? g_dataScratch[0]->stringID.c_str() : "(none)");
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 0;
}

// SEH-guarded: a generic WEAPON_MANUFACTURER GameData for weapon fabrication when the
// wire carried no manufacturer sid (spike 451: the manufacturer record is the REQUIRED
// first arg of a weapon createItem - without one the weapon cannot fabricate at all,
// so a generic maker beats a lost weapon). First enumerated record, cached per session.
// External linkage (Phase 5e): declared in EngineInternal.h so the weapon-mint probes
// (now in EngineProbe.cpp) can reuse it.
GameData* fallbackWeaponManufacturer(GameWorld* gw) {
    static GameData* cached = 0;
    if (cached) return cached;
    if (!gw || !g_getDataOfTypeFn) return 0;
    __try {
        g_dataScratch.clear();
        g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, WEAPON_MANUFACTURER);
        if (g_dataScratch.size() > 0) cached = g_dataScratch[0];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return cached;
}

// SEH-guarded: create `qty` of the template (sid, typeCat) and add it to inv. The
// join reconstructs items locally (their hands are host-only / unresolvable), so a
// fresh blank handle is fine - the host stays authoritative for the contents. When
// `equip` is set, the created item is moved into its equipment slot (equipItem) so
// the reconstructed item is WORN, matching the author's equipped state.
// External linkage (Phase 5e): declared in EngineInternal.h (default args live on
// that declaration) so probeFabricateWeaponLoose (now in EngineProbe.cpp) can reuse it.
bool createItemAndAdd(GameWorld* gw, Inventory* inv, const char* sid,
                      unsigned int typeCat, int qty, int qualityBucket, bool equip,
                      const char* manufacturer, const char* material) {
    if (!gw || !gw->theFactory || !g_createItemFn || !inv || !sid || qty <= 0) return false;
    static int dbg = -1;
    if (dbg < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dbg = (e && e[0] == '1') ? 1 : 0; }
    __try {
        GameData* tmpl = findItemTemplateImpl(gw, sid, typeCat);
        if (!tmpl) { if (dbg) coop::logLine("[mk] tmpl-null"); return false; }
        // A WEAPON needs its manufacturer (company) GameData; resolve it (and the material
        // spec) by the replicated stringIDs. Armour and items pass null for both (their
        // templates instantiate directly).
        GameData* man = (manufacturer && manufacturer[0])
                        ? findItemTemplateImpl(gw, manufacturer, (unsigned int)WEAPON_MANUFACTURER) : 0;
        GameData* mat = (material && material[0])
                        ? findItemTemplateImpl(gw, material, (unsigned int)MATERIAL_SPECS_WEAPON) : 0;
        Item* it = 0;
        if ((itemType)typeCat == WEAPON) {
            // KENSHICOOP_WEAPON_FAB=0: escape hatch back to the pre-spike-451 behaviour
            // (weapons never fabricate - conservation-only gear sync). Covers EVERY
            // weapon-fabrication site at once: reconcile CREATE, xfer shortfall, probes.
            static int fabOn = -1;
            if (fabOn < 0) { const char* e = getenv("KENSHICOOP_WEAPON_FAB"); fabOn = (e && e[0] == '0') ? 0 : 1; }
            if (!fabOn) {
                if (dbg) coop::logLine("[mk] weapon-fab disabled (KENSHICOOP_WEAPON_FAB=0)");
                return false;
            }
            // Spike 451: for WEAPONS the 6-arg createItem's first two GameData roles
            // are SWAPPED - the engine passes the WEAPON_MANUFACTURER record FIRST and
            // the weapon template THIRD (the header's misleading "weaponMesh" slot).
            // Template-first returns null for every weapon template (the old 0/24);
            // manufacturer-first fabricates (replay proved created=1 added=1). The
            // engine's own mint shape: blank NULL_ITEM hand, levelOverride 0, no faction.
            if (!man) man = fallbackWeaponManufacturer(gw);
            if (man) {
                char wb[sizeof(hand) + 16];
                memset(wb, 0, sizeof(wb));
                hand* wh = reinterpret_cast<hand*>(wb);
                g_handCtorFn(wh, 0, 0, NULL_ITEM, 0, 0);
                it = g_createItemFn(gw->theFactory, man, wh, tmpl, mat, 0, 0);
            } else if (dbg) {
                coop::logLine("[mk] weapon manufacturer unresolved (no provenance, no fallback)");
            }
        } else {
            char buf[sizeof(hand) + 16];
            memset(buf, 0, sizeof(buf));
            hand* h = reinterpret_cast<hand*>(buf);
            g_handCtorFn(h, 0, 0, (itemType)typeCat, 0, 0); // blank handle (factory owns id)
            it = g_createItemFn(gw->theFactory, tmpl, h, man, mat, -1, 0);
        }
        if (!it) { if (dbg) { char b[140]; _snprintf(b,sizeof(b)-1,"[mk] createItem-null sid='%s' type=%u man=%d mat=%d",sid,typeCat,man?1:0,mat?1:0); b[sizeof(b)-1]='\0'; coop::logLine(b);} return false; }
        if (qualityBucket > 0) it->quality = (float)qualityBucket / 100.0f;
        if (!inv->tryAddItem(it, qty)) { if (dbg) { char b[120]; _snprintf(b,sizeof(b)-1,"[mk] tryAddItem-fail sid='%s' type=%u equip=%d",sid,typeCat,equip?1:0); b[sizeof(b)-1]='\0'; coop::logLine(b);} return false; } // virtual
        // Equipment is non-stackable (qty 1); move the just-added item into its slot.
        if (equip && g_equipItemFn) g_equipItemFn(inv, it);
        if (dbg) { char b[120]; _snprintf(b,sizeof(b)-1,"[mk] OK sid='%s' type=%u equip=%d qty=%d",sid,typeCat,equip?1:0,qty); b[sizeof(b)-1]='\0'; coop::logLine(b); }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (dbg) coop::logLine("[mk] SEH-except");
        return false;
    }
}

namespace {

// SEH-guarded: remove up to `qty` units of (sid, typeCat, equipped) by walking the
// captured stacks. removeItemAutoDestroy frees the stack, so items[i] is never
// touched again. `wantEquipped` keeps loose and worn copies of the same item distinct
// (so unequipping/dropping a worn item doesn't accidentally remove the loose one).
int removeByKey(Inventory* inv, Item** items, InvItemEntry* meta, unsigned int n,
                const char* sid, unsigned int typeCat, int qty, int wantEquipped) {
    if (!inv || !items || !meta || !sid || qty <= 0) return 0;
    int removed = 0;
    __try {
        for (unsigned int i = 0; i < n && removed < qty; ++i) {
            if (!items[i]) continue;
            if (meta[i].itemType != typeCat) continue;
            if ((int)meta[i].equipped != wantEquipped) continue;
            if (strcmp(meta[i].stringID, sid) != 0) continue;
            int have = meta[i].quantity; if (have < 1) have = 1;
            int take = qty - removed; if (take > have) take = have;
            inv->removeItemAutoDestroy(items[i], take); // virtual; destroys the stack
            removed += take;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return removed;
    }
    return removed;
}

// SEH-guarded: EQUIP an item the container ALREADY holds loose (move loose -> slot).
// Unlike createItemAndAdd(equip=true) - which fabricates a blank-handle item that the
// engine discards within a tick (d25) - this equips a REAL, already-resolved item, so it
// PERSISTS. This is what makes the re-equip (up) path replicate. Returns 1 on success.
int equipExisting(Inventory* inv, Item* it) {
    if (!inv || !it || !g_equipItemFn) return 0;
    __try {
        return g_equipItemFn(inv, it) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// SEH-guarded: UNEQUIP a worn item back to loose inventory (move slot -> loose) WITHOUT
// destroying it, so its identity/quality survive for a later re-equip. removeItemDontDestroy
// detaches it from its equip section; then we place it into a LOOSE (non-equip) storage
// section DIRECTLY - the inventory-level add (tryAddItem) auto-routes an equippable item
// straight back into its slot (re-equipping the thing we just took off), so we must target
// a loose section's own addItem to keep it off. Returns 1 on success.
int unequipToLoose(Inventory* inv, Item* it) {
    if (!inv || !it) return 0;
    __try {
        Item* taken = inv->removeItemDontDestroy_returnsItem(it, 1, false); // virtual
        if (!taken) return 0;
        GameData* gd = taken->getGameData();
        if (g_getSectionsFn) {
            lektor<InventorySection*>* secs = g_getSectionsFn(inv);
            unsigned int ns = secs ? secs->size() : 0;
            for (unsigned int s = 0; s < ns; ++s) {
                InventorySection* sec = (*secs)[s];
                if (!sec || sec->isAnEquippedItemSection || sec->containerSlot) continue;
                if (gd && !sec->hasRoomForItem(gd, 1)) continue;  // virtual
                if (sec->addItem(taken, 1)) return 1;             // virtual; loose placement
            }
        }
        // Fallback: generic add (may re-equip, but never leak the item).
        return inv->tryAddItem(taken, 1) ? 1 : 0;                 // virtual
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// SEH-guarded: ensure a WORN weapon of (sid,type) sits in the weapon section whose name
// hashes to wantSection. The two weapon slots ('hip'=Weapon II, 'back'=Weapon I) share
// AttachSlot ATTACH_WEAPON, so equipItem auto-routes to a default slot - it cannot honour
// the author's slot choice. This moves the REAL worn item between the two weapon sections
// (detach + section-level addItem, the same direct placement unequipToLoose uses) so the
// peer mirrors Weapon I vs II. No-op unless wantSection names an EXISTING weapon section,
// the item is worn in a DIFFERENT weapon section, and the target slot doesn't already hold
// it. Armour is never touched (its sections are unique per slot, so they never mismatch).
// Returns 1 if it moved an item, else 0.
int correctWeaponSlot(Inventory* inv, const char* sid, unsigned int type,
                      unsigned short wantSection) {
    if (!inv || !sid || !sid[0] || wantSection == 0 || !g_getSectionsFn) return 0;
    __try {
        lektor<InventorySection*>* secs = g_getSectionsFn(inv);
        unsigned int ns = secs ? secs->size() : 0;
        InventorySection* target = 0;
        Item* srcItem = 0;
        for (unsigned int s = 0; s < ns; ++s) {
            InventorySection* sec = (*secs)[s];
            if (!sec || !sec->isAnEquippedItemSection) continue;
            if ((unsigned int)sec->limitedSlot != (unsigned int)ATTACH_WEAPON) continue;
            unsigned short h = sectionNameHash(sec->name.c_str());
            const Ogre::vector<InventorySection::SectionItem>::type& its = sec->items;
            unsigned int ni = (unsigned int)its.size();
            Item* found = 0;
            for (unsigned int i = 0; i < ni; ++i) {
                Item* it = its[i].item; if (!it) continue;
                GameData* gd = it->getGameData(); if (!gd) continue;
                if ((unsigned int)gd->type != type) continue;
                if (strcmp(gd->stringID.c_str(), sid) != 0) continue;
                found = it; break;
            }
            if (h == wantSection) {
                target = sec;
                if (found) return 0;            // already in the desired slot - nothing to do
            } else if (found && !srcItem) {
                srcItem = found;                // worn in the OTHER weapon slot - candidate to move
            }
        }
        if (!target || !srcItem) return 0;       // target slot unknown, or item not worn elsewhere
        Item* taken = inv->removeItemDontDestroy_returnsItem(srcItem, 1, false); // virtual
        if (!taken) return 0;
        if (target->addItem(taken, 1)) return 1;                  // virtual: direct slot placement
        // Targeted placement failed - re-equip generically so the weapon is never leaked.
        if (g_equipItemFn) g_equipItemFn(inv, taken);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

} // namespace

unsigned int captureContainerContents(GameWorld* gw, const unsigned int cHand[5],
                                      InvItemEntry* out, unsigned int maxOut,
                                      unsigned int* outHash) {
    if (outHash) *outHash = 0;
    if (!gw || !out || maxOut == 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    unsigned int n = readInvItems(inv, out, 0, maxOut);
    if (outHash) {
        unsigned int h = 0;
        for (unsigned int i = 0; i < n; ++i) h += invEntryHash(out[i]);
        *outHash = h; // 0 == empty container (a meaningful, distinct fingerprint)
    }
    return n;
}

bool applyContainerContents(GameWorld* gw, const unsigned int cHand[5],
                            const InvItemEntry* items, unsigned int count) {
    if (!gw) return false;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return false;
    Inventory* inv = invOf(ro);
    if (!inv) return false;

    // Snapshot current contents (with parallel Item* for removal).
    const unsigned int MAXC = 64;
    InvItemEntry cur[64];
    Item* curItems[64];
    unsigned int ncur = readInvItems(inv, cur, curItems, MAXC);

    // Group desired + current by TEMPLATE (stringID,itemType), tracking the EQUIPPED vs
    // LOOSE split separately. A change in the split while the template's TOTAL count is
    // unchanged is an in-place MOVE (equip/unequip an existing item): we transition the
    // REAL item rather than destroy+recreate, so (a) identity/quality survive and (b) the
    // equip PERSISTS - fabricated blank-handle equips are discarded by the engine (d25),
    // which is why the re-equip (up) path used to vanish. Only a genuine total-count
    // change creates (loose) or destroys items. Counts are tiny, so O(k^2) PODs is fine
    // (SEH-safe: engine mutation is confined to the equip/unequip/create/remove helpers).
    struct Grp {
        char sid[48]; unsigned int type;
        int desiredEq, desiredLoose, curEq, curLoose; int qualEq, qualLoose;
        char manufacturer[48]; char material[48]; // weapon provenance (needed to recreate)
    };
    Grp g[128];
    unsigned int ng = 0;
    for (unsigned int i = 0; i < count; ++i) {
        unsigned int j = 0;
        for (; j < ng; ++j)
            if (g[j].type == items[i].itemType && strcmp(g[j].sid, items[i].stringID) == 0) break;
        if (j == ng && ng < 128) {
            memset(&g[ng], 0, sizeof(Grp));
            strncpy(g[ng].sid, items[i].stringID, sizeof(g[ng].sid) - 1);
            g[ng].type = items[i].itemType; j = ng; ++ng;
        }
        if (j < ng) {
            int q = (items[i].quantity < 1) ? 1 : items[i].quantity;
            if (items[i].equipped) { g[j].desiredEq += q; g[j].qualEq = items[i].quality; }
            else                   { g[j].desiredLoose += q; g[j].qualLoose = items[i].quality; }
            // Carry the weapon's manufacturer/material (first desired entry of the group);
            // empty for non-weapons. Needed when the create path fabricates the item.
            if (!g[j].manufacturer[0] && items[i].manufacturer[0])
                strncpy(g[j].manufacturer, items[i].manufacturer, sizeof(g[j].manufacturer) - 1);
            if (!g[j].material[0] && items[i].material[0])
                strncpy(g[j].material, items[i].material, sizeof(g[j].material) - 1);
        }
    }
    for (unsigned int i = 0; i < ncur; ++i) {
        unsigned int j = 0;
        for (; j < ng; ++j)
            if (g[j].type == cur[i].itemType && strcmp(g[j].sid, cur[i].stringID) == 0) break;
        if (j == ng && ng < 128) {
            memset(&g[ng], 0, sizeof(Grp));
            strncpy(g[ng].sid, cur[i].stringID, sizeof(g[ng].sid) - 1);
            g[ng].type = cur[i].itemType; j = ng; ++ng;
        }
        if (j < ng) {
            int q = (cur[i].quantity < 1) ? 1 : cur[i].quantity;
            if (cur[i].equipped) g[j].curEq += q; else g[j].curLoose += q;
        }
    }

    static int dbg = -1;
    if (dbg < 0) { const char* e = getenv("KENSHICOOP_INV_DUMP"); dbg = (e && e[0] == '1') ? 1 : 0; }

    bool changed = false;
    for (unsigned int k = 0; k < ng; ++k) {
        if (dbg) {
            char b[200];
            _snprintf(b, sizeof(b) - 1,
                "[recon] grp type=%u sid='%s' desire(eq=%d,loose=%d) cur(eq=%d,loose=%d)",
                g[k].type, g[k].sid, g[k].desiredEq, g[k].desiredLoose, g[k].curEq, g[k].curLoose);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // 1) MOVE UP: equip an existing LOOSE copy when we need more worn AND have loose
        //    to spare. Equipping a real item persists (unlike a fabricated one).
        int upMoves;
        { int needEq = g[k].desiredEq - g[k].curEq, surpLoose = g[k].curLoose - g[k].desiredLoose;
          upMoves = (needEq < surpLoose) ? needEq : surpLoose; if (upMoves < 0) upMoves = 0; }
        for (int m = 0; m < upMoves; ++m) {
            int f = -1;
            for (unsigned int i = 0; i < ncur; ++i) {
                if (!curItems[i] || cur[i].equipped || cur[i].itemType != g[k].type) continue;
                if (strcmp(cur[i].stringID, g[k].sid) != 0) continue;
                f = (int)i; break;
            }
            int ok = (f < 0) ? -1 : equipExisting(inv, curItems[f]);
            if (dbg) { char b[96]; _snprintf(b, sizeof(b)-1, "[recon]   MOVE-UP f=%d ok=%d", f, ok); b[sizeof(b)-1]='\0'; coop::logLine(b); }
            if (f < 0 || !ok) break;
            curItems[f] = 0;                 // consumed: now worn, not loose
            g[k].curLoose--; g[k].curEq++; changed = true;
        }
        // 2) MOVE DOWN: unequip an existing WORN copy to loose (preserve it) when we need
        //    more loose AND have worn to spare.
        int downMoves;
        { int needLoose = g[k].desiredLoose - g[k].curLoose, surpEq = g[k].curEq - g[k].desiredEq;
          downMoves = (needLoose < surpEq) ? needLoose : surpEq; if (downMoves < 0) downMoves = 0; }
        for (int m = 0; m < downMoves; ++m) {
            int f = -1;
            for (unsigned int i = 0; i < ncur; ++i) {
                if (!curItems[i] || !cur[i].equipped || cur[i].itemType != g[k].type) continue;
                if (strcmp(cur[i].stringID, g[k].sid) != 0) continue;
                f = (int)i; break;
            }
            int ok = (f < 0) ? -1 : unequipToLoose(inv, curItems[f]);
            if (dbg) { char b[96]; _snprintf(b, sizeof(b)-1, "[recon]   MOVE-DOWN f=%d ok=%d", f, ok); b[sizeof(b)-1]='\0'; coop::logLine(b); }
            if (f < 0 || !ok) break;
            curItems[f] = 0;                 // consumed: now loose, not worn
            g[k].curEq--; g[k].curLoose++; changed = true;
        }
        // 3) RESIDUAL CREATE (genuine additions). ALWAYS create LOOSE - a fabricated loose
        //    item persists, but a fabricated-AND-equipped one is discarded within a tick by
        //    the engine's equipment validation (d25; this is the "picked-up weapon flickers
        //    into slot 1 then vanishes" bug). So any EQUIP shortfall for which we have no
        //    real copy to MOVE-UP is created loose here and equipped on a LATER reconcile
        //    tick: by then the loose copy is a real, established factory item, and MOVE-UP's
        //    equipExisting moves it into its slot persistently (the inv_reequip path). The
        //    freshly-created loose items are NOT in curItems[], so step-4 REMOVE-LOOSE (which
        //    only scans captured pre-existing items) can never strip them this tick.
        int createLoose = 0;
        if (g[k].desiredLoose > g[k].curLoose) createLoose += g[k].desiredLoose - g[k].curLoose;
        if (g[k].desiredEq    > g[k].curEq)    createLoose += g[k].desiredEq    - g[k].curEq;
        if (createLoose > 0) {
            int qb = (g[k].desiredEq > g[k].curEq) ? g[k].qualEq : g[k].qualLoose;
            bool ok = createItemAndAdd(gw, inv, g[k].sid, g[k].type, createLoose, qb, false,
                                       g[k].manufacturer, g[k].material);
            if (dbg) { char b[120]; _snprintf(b, sizeof(b)-1, "[recon]   CREATE-LOOSE n=%d (loose+eqDefer) ok=%d", createLoose, ok?1:0); b[sizeof(b)-1]='\0'; coop::logLine(b); }
            if (ok) changed = true;
        }
        // 4) RESIDUAL REMOVE (genuine removals; destroy surplus). Moved items were nulled
        //    in curItems above, so removeByKey won't touch them.
        if (g[k].curLoose > g[k].desiredLoose) {
            int r = removeByKey(inv, curItems, cur, ncur, g[k].sid, g[k].type,
                            g[k].curLoose - g[k].desiredLoose, 0);
            if (dbg) { char b[96]; _snprintf(b, sizeof(b)-1, "[recon]   REMOVE-LOOSE n=%d got=%d", g[k].curLoose-g[k].desiredLoose, r); b[sizeof(b)-1]='\0'; coop::logLine(b); }
            if (r > 0) changed = true;
        }
        if (g[k].curEq > g[k].desiredEq) {
            int r = removeByKey(inv, curItems, cur, ncur, g[k].sid, g[k].type,
                            g[k].curEq - g[k].desiredEq, 1);
            if (dbg) { char b[96]; _snprintf(b, sizeof(b)-1, "[recon]   REMOVE-EQ n=%d got=%d", g[k].curEq-g[k].desiredEq, r); b[sizeof(b)-1]='\0'; coop::logLine(b); }
            if (r > 0) changed = true;
        }
    }
    // SLOT-FIDELITY pass (weapons). The count reconcile above equips the right NUMBER of
    // each weapon, but equipItem auto-routes to a default weapon slot, so a weapon the
    // author wears in Weapon I ('back') can land in Weapon II ('hip') on the peer. Both
    // weapon sections share AttachSlot ATTACH_WEAPON (identical `slot`), so we steer by the
    // replicated SECTION hash and move each worn weapon into the slot the author chose.
    for (unsigned int i = 0; i < count; ++i) {
        if (!items[i].equipped || items[i].section == 0) continue;
        if (correctWeaponSlot(inv, items[i].stringID, items[i].itemType, items[i].section)) {
            changed = true;
            if (dbg) { char b[160]; _snprintf(b, sizeof(b)-1,
                "[recon]   SLOT-MOVE sid='%s' -> section=%u", items[i].stringID, items[i].section);
                b[sizeof(b)-1]='\0'; coop::logLine(b); }
        }
    }
    return changed;
}

namespace {
// Find a "common", general-inventory-friendly item template (stackable trade goods /
// food the player squad's backpack always accepts). Falls back to the first ITEM
// template. Caller holds SEH. Returns the template + writes its itemType category.
GameData* findCommonItemTemplate(GameWorld* gw, unsigned int* outType) {
    if (!gw || !g_getDataOfTypeFn) return 0;
    const char* prefs[] = {
        "iron plate", "copper", "building materials", "raw meat", "dustwich",
        "foodcube", "ration", "rock", "cotton", "fabric"
    };
    const unsigned int np = sizeof(prefs) / sizeof(prefs[0]);
    __try {
        g_dataScratch.clear();
        g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, ITEM);
        unsigned int n = g_dataScratch.size();
        for (unsigned int k = 0; k < np; ++k)
            for (unsigned int i = 0; i < n; ++i) {
                GameData* gd = g_dataScratch[i];
                if (gd && ciContains(gd->name.c_str(), prefs[k])) {
                    if (outType) *outType = (unsigned int)ITEM;
                    return gd;
                }
            }
        for (unsigned int i = 0; i < n; ++i)
            if (g_dataScratch[i]) { if (outType) *outType = (unsigned int)ITEM; return g_dataScratch[i]; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return 0;
}
} // namespace

bool pickInventoryContainer(GameWorld* gw, unsigned int out[5]) {
    if (out) { for (int i = 0; i < 5; ++i) out[i] = 0; }
    if (!gw || !gw->player) return false;
    __try {
        if (gw->player->playerCharacters.size() == 0) return false;
        Character* ld = gw->player->playerCharacters[0];
        if (!ld) return false;
        // v1 anchor: the leader's own inventory - a container that EXISTS in every save
        // (stable character hand, resolves cross-client) and accepts arbitrary items
        // (unlike resource-limited storage buildings). Both setup + resolve agree on it.
        return readObjectHand(static_cast<RootObject*>(ld), out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int addTestItemsToContainer(GameWorld* gw, const unsigned int cHand[5], int qty,
                            char* outStringID, unsigned int outLen) {
    if (outStringID && outLen) outStringID[0] = '\0';
    if (!gw || qty <= 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    unsigned int typeCat = 0;
    GameData* gd = findCommonItemTemplate(gw, &typeCat);
    if (!gd) return 0;
    char sid[48]; sid[0] = '\0';
    __try {
        const char* s = gd->stringID.c_str();
        strncpy(sid, s ? s : "", sizeof(sid) - 1); sid[sizeof(sid) - 1] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (!sid[0]) return 0;
    if (outStringID && outLen) { strncpy(outStringID, sid, outLen - 1); outStringID[outLen - 1] = '\0'; }
    // createItemAndAdd is SEH-guarded; it re-resolves the template by stringID+type.
    bool ok = createItemAndAdd(gw, inv, sid, typeCat, qty, 0, /*equip=*/false);
    return ok ? qty : 0;
}

int probeAddAnyToContainer(GameWorld* gw, const unsigned int cHand[5], int qty,
                           char* outStringID, unsigned int outLen) {
    if (outStringID && outLen) outStringID[0] = '\0';
    if (!gw || qty <= 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    // Storage buildings are usually item-type-LIMITED (run-171231: a Fabric
    // Chest refused the iron-plate sentinel), so walk the common stackables
    // until tryAddItem accepts one. This mirrors the protocol-34 apply path,
    // which only ever fabricates items the AUTHOR's copy of the container
    // already holds - i.e. items the container accepts by definition.
    const char* prefs[] = {
        "iron plate", "copper", "building materials", "raw meat", "dustwich",
        "foodcube", "ration", "rock", "cotton", "fabric"
    };
    const unsigned int np = sizeof(prefs) / sizeof(prefs[0]);
    for (unsigned int k = 0; k < np; ++k) {
        char sid[48]; sid[0] = '\0';
        __try {
            g_dataScratch.clear();
            g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, ITEM);
            unsigned int n = g_dataScratch.size();
            for (unsigned int i = 0; i < n; ++i) {
                GameData* gd = g_dataScratch[i];
                if (gd && ciContains(gd->name.c_str(), prefs[k])) {
                    const char* s = gd->stringID.c_str();
                    strncpy(sid, s ? s : "", sizeof(sid) - 1);
                    sid[sizeof(sid) - 1] = '\0';
                    break;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { sid[0] = '\0'; }
        if (!sid[0]) continue;
        // createItemAndAdd is SEH-guarded; false = template miss OR the
        // container refused it (type filter) - try the next candidate.
        if (createItemAndAdd(gw, inv, sid, (unsigned int)ITEM, qty, 0,
                             /*equip=*/false)) {
            if (outStringID && outLen) {
                strncpy(outStringID, sid, outLen - 1);
                outStringID[outLen - 1] = '\0';
            }
            return qty;
        }
    }
    return 0;
}

int removeTestItemsFromContainer(GameWorld* gw, const unsigned int cHand[5], int qty) {
    if (!gw || qty <= 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    unsigned int typeCat = 0;
    GameData* gd = findCommonItemTemplate(gw, &typeCat);
    if (!gd) return 0;
    char sid[48]; sid[0] = '\0';
    __try {
        const char* s = gd->stringID.c_str();
        strncpy(sid, s ? s : "", sizeof(sid) - 1); sid[sizeof(sid) - 1] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (!sid[0]) return 0;
    // Snapshot current contents (with parallel Item* for removal), then remove by key.
    const unsigned int MAXC = 64;
    InvItemEntry cur[64];
    Item* curItems[64];
    unsigned int ncur = readInvItems(inv, cur, curItems, MAXC);
    return removeByKey(inv, curItems, cur, ncur, sid, typeCat, qty, /*wantEquipped=*/0);
}

int commonTestItemSid(GameWorld* gw, char* outSid, unsigned int outLen,
                      unsigned int* outType) {
    if (outSid && outLen) outSid[0] = '\0';
    if (outType) *outType = 0;
    if (!gw || !outSid || outLen == 0) return 0;
    unsigned int typeCat = 0;
    GameData* gd = findCommonItemTemplate(gw, &typeCat);
    if (!gd) return 0;
    __try {
        const char* s = gd->stringID.c_str();
        if (!s || !s[0]) return 0;
        strncpy(outSid, s, outLen - 1); outSid[outLen - 1] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (outType) *outType = typeCat;
    return 1;
}

int addItemsToContainerBySid(GameWorld* gw, const unsigned int cHand[5],
                             const char* sid, unsigned int typeCat, int qty,
                             int qualityBucket, const char* manufacturer,
                             const char* material) {
    if (!gw || !sid || !sid[0] || qty <= 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    bool ok = createItemAndAdd(gw, inv, sid, typeCat, qty, qualityBucket,
                               /*equip=*/false, manufacturer, material);
    return ok ? qty : 0;
}

int moveItemBetweenContainers(GameWorld* gw, const unsigned int srcHand[5],
                              const unsigned int dstHand[5],
                              const char* sid, unsigned int typeCat, int qty,
                              bool suspendVeto) {
    if (!gw || !sid || !sid[0] || qty <= 0) return 0;
    RootObject* srcRo = resolveObjectByHand(srcHand);
    RootObject* dstRo = resolveObjectByHand(dstHand);
    if (!srcRo || !dstRo || srcRo == dstRo) return 0;
    Inventory* src = invOf(srcRo);
    Inventory* dst = invOf(dstRo);
    if (!src || !dst) return 0;
    const unsigned int MAXC = 64;
    InvItemEntry cur[64];
    Item* curItems[64];
    unsigned int ncur = readInvItems(src, cur, curItems, MAXC);
    int moved = 0;
    // This is normally a SANCTIONED cross-owner relocation (Protocol 37
    // conservation), so suspend the trade veto for its duration - the veto only
    // exists to refuse genuine UI drags, never our own reconciled moves. The
    // xfer_block test passes suspendVeto=false to drive it AS a UI drag would
    // (veto active), so it can assert the refusal + item conservation.
    bool vetoSav = g_invVetoSuspend; if (suspendVeto) g_invVetoSuspend = true;
    __try {
        // Loose stacks first (the ordinary bag-to-bag drag), then worn copies (the
        // "drag straight out of an equip slot" trade the field report describes).
        for (int pass = 0; pass < 2 && moved < qty; ++pass) {
            for (unsigned int i = 0; i < ncur && moved < qty; ++i) {
                if (!curItems[i]) continue;
                if (cur[i].itemType != typeCat) continue;
                if ((int)cur[i].equipped != pass) continue;
                if (strcmp(cur[i].stringID, sid) != 0) continue;
                int have = cur[i].quantity; if (have < 1) have = 1;
                int take = qty - moved; if (take > have) take = have;
                Item* taken = src->removeItemDontDestroy_returnsItem(curItems[i], take, false); // virtual
                if (!taken) continue;
                if (!dst->tryAddItem(taken, take)) {          // virtual: real-object relocation
                    src->tryAddItem(taken, take);             // destination refused - put it back
                    g_invVetoSuspend = vetoSav;
                    return moved;
                }
                curItems[i] = 0; // detached; never touch this stack again
                moved += take;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_invVetoSuspend = vetoSav;
        return moved;
    }
    g_invVetoSuspend = vetoSav;
    return moved;
}

// SEH-guarded DIAGNOSTIC: log the full inventory of the object at cHand - every loose
// _allItems entry and every section (name/slot/equipFlag/containerFlag) with its items
// (type/eqFlag/sid). Lets us see exactly where a worn WEAPON lives: a section flagged
// isAnEquippedItemSection (the snapshot captures it) vs. a dedicated weapon pointer the
// section walk misses (type 0 = WEAPON, 1 = ARMOUR, 2 = ITEM).
void dumpInventory(GameWorld* gw, const unsigned int cHand[5]) {
    if (!gw) return;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return;
    Inventory* inv = invOf(ro);
    if (!inv) return;
    __try {
        lektor<Item*>& all = inv->_allItems;
        unsigned int na = all.size();
        char h[120]; _snprintf(h, sizeof(h) - 1, "DUMP _allItems n=%u", na); h[sizeof(h) - 1] = '\0';
        coop::logLine(h);
        for (unsigned int i = 0; i < na; ++i) {
            Item* it = all[i]; if (!it) continue;
            GameData* gd = it->getGameData(); if (!gd) continue;
            char b[200];
            _snprintf(b, sizeof(b) - 1, "DUMP   loose type=%u eq=%d slot=%u sid='%s'",
                      (unsigned int)gd->type, it->isEquipped ? 1 : 0,
                      (unsigned int)it->slotType, gd->stringID.c_str());
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try {
        Item* pw = g_getPrimaryWeaponFn ? g_getPrimaryWeaponFn(inv) : 0;
        Item* sw = g_getSecondaryWeaponFn ? g_getSecondaryWeaponFn(inv) : 0;
        GameData* pgd = pw ? pw->getGameData() : 0;
        GameData* sgd = sw ? sw->getGameData() : 0;
        char b[220];
        _snprintf(b, sizeof(b) - 1, "DUMP weapons primary=%s sid='%s' secondary=%s sid='%s'",
                  pw ? "Y" : "n", pgd ? pgd->stringID.c_str() : "",
                  sw ? "Y" : "n", sgd ? sgd->stringID.c_str() : "");
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!g_getSectionsFn) return;
    __try {
        lektor<InventorySection*>* secs = g_getSectionsFn(inv);
        unsigned int ns = secs ? secs->size() : 0;
        char h[120]; _snprintf(h, sizeof(h) - 1, "DUMP sections n=%u", ns); h[sizeof(h) - 1] = '\0';
        coop::logLine(h);
        for (unsigned int s = 0; s < ns; ++s) {
            InventorySection* sec = (*secs)[s]; if (!sec) continue;
            const Ogre::vector<InventorySection::SectionItem>::type& its = sec->items;
            unsigned int ni = (unsigned int)its.size();
            char b[220];
            _snprintf(b, sizeof(b) - 1, "DUMP sec='%s' slot=%u equip=%d ctnr=%d items=%u",
                      sec->name.c_str(), (unsigned int)sec->limitedSlot,
                      sec->isAnEquippedItemSection ? 1 : 0, sec->containerSlot ? 1 : 0, ni);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            for (unsigned int i = 0; i < ni; ++i) {
                Item* it = its[i].item; if (!it) continue;
                GameData* gd = it->getGameData(); if (!gd) continue;
                char c[200];
                _snprintf(c, sizeof(c) - 1, "DUMP     type=%u eq=%d sid='%s'",
                          (unsigned int)gd->type, it->isEquipped ? 1 : 0, gd->stringID.c_str());
                c[sizeof(c) - 1] = '\0'; coop::logLine(c);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---- Phase W0: world-item (ground drop) diagnostic hooks -------------------
// These let a scenario drive a DROP and enumerate nearby world items WITHOUT any UI,
// so we can characterize exactly what a Kenshi drop produces (object kind/itemType,
// position, whether getObjectsWithinSphere enumerates it, hand value) before designing
// the world-item replication channel. Mirrors addTestItemsToContainer / dumpInventory.

// SEH-guarded: drop `qty` of the LOOSE (sid,type) the object at cHand holds onto the
// ground. Finds the matching loose Item* and calls Inventory::dropItem (virtual), which
// removes it from the bag and places it as a world object. Returns the number dropped.
int dropItemFromInventory(GameWorld* gw, const unsigned int cHand[5],
                          const char* sid, unsigned int typeCat, int qty,
                          void** outLastDropped) {
    if (outLastDropped) *outLastDropped = 0;
    if (!gw || !sid || !sid[0] || qty <= 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    int dropped = 0;
    __try {
        // Re-snapshot each iteration: dropItem mutates _allItems, so a cached Item*
        // could dangle. Find the first LOOSE entry matching (sid,type) and drop it.
        for (int d = 0; d < qty; ++d) {
            const unsigned int MAXC = 64;
            InvItemEntry cur[64];
            Item* curItems[64];
            unsigned int ncur = readInvItems(inv, cur, curItems, MAXC);
            Item* victim = 0;
            for (unsigned int i = 0; i < ncur; ++i) {
                if (cur[i].equipped) continue;                 // loose only
                if (cur[i].itemType != typeCat) continue;
                if (strcmp(cur[i].stringID, sid) != 0) continue;
                victim = curItems[i]; break;
            }
            if (!victim) break;
            inv->dropItem(victim);                             // virtual: bag -> ground
            if (outLastDropped) *outLastDropped = victim;      // the now-grounded object
            ++dropped;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return dropped;
}

namespace {
// Enumerate world objects of `type` within `radius` of `center` and log one line per
// object (itemType / sid / hand / pos). Returns the count enumerated. Caller holds SEH.
unsigned int dumpWorldItemsOfType(GameWorld* gw, const Ogre::Vector3& center,
                                  float radius, itemType type, const char* label) {
    if (!g_getObjsFn) return 0;
    g_npcQuery.clear();
    g_getObjsFn(gw, &g_npcQuery, &center, radius, type, 256, 0);
    unsigned int n = g_npcQuery.size();
    char h[120];
    _snprintf(h, sizeof(h) - 1, "WORLDITEM scan type=%s(%d) n=%u r=%.1f",
              label, (int)type, n, radius);
    h[sizeof(h) - 1] = '\0'; coop::logLine(h);
    for (unsigned int i = 0; i < n; ++i) {
        RootObject* o = g_npcQuery[i]; if (!o) continue;
        unsigned int hd[5] = {0,0,0,0,0};
        readObjectHand(o, hd);
        Ogre::Vector3 p(0,0,0);
        __try { p = o->getPosition(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        GameData* gd = 0;
        __try { gd = o->getGameData(); } __except (EXCEPTION_EXECUTE_HANDLER) { gd = 0; }
        char b[260];
        _snprintf(b, sizeof(b) - 1,
            "WORLDITEM   %s sid='%s' gdtype=%u hand=%u,%u,%u,%u,%u pos=%.2f,%.2f,%.2f",
            label, gd ? gd->stringID.c_str() : "(no gd)",
            gd ? (unsigned int)gd->type : 0u,
            hd[0], hd[1], hd[2], hd[3], hd[4], p.x, p.y, p.z);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    return n;
}
} // namespace

// SEH-guarded DIAGNOSTIC: enumerate WEAPON/ARMOUR/ITEM/CONTAINER world objects near the
// local leader and log each (so we can see what a dropped item became and whether the
// interest query enumerates it). The log IS the deliverable for the W0 drop_probe.
void dumpWorldItems(GameWorld* gw) {
    if (!gw || !gw->player) return;
    __try {
        if (gw->player->playerCharacters.size() == 0) return;
        Character* ld = gw->player->playerCharacters[0];
        if (!ld) return;
        Ogre::Vector3 center = ld->getPosition();
        const float r = 30.0f;
        dumpWorldItemsOfType(gw, center, r, WEAPON,    "WEAPON");
        dumpWorldItemsOfType(gw, center, r, ARMOUR,    "ARMOUR");
        dumpWorldItemsOfType(gw, center, r, ITEM,      "ITEM");
        dumpWorldItemsOfType(gw, center, r, CONTAINER, "CONTAINER");
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// SEH-guarded: count loose world items (WEAPON+ARMOUR+ITEM) within `radius` of the
// leader. Oracle cross-check that a drop actually produced an enumerable ground item.
int countWorldItemsNear(GameWorld* gw, float radius) {
    if (!gw || !gw->player || !g_getObjsFn) return 0;
    int total = 0;
    __try {
        if (gw->player->playerCharacters.size() == 0) return 0;
        Character* ld = gw->player->playerCharacters[0];
        if (!ld) return 0;
        Ogre::Vector3 center = ld->getPosition();
        const itemType kinds[] = { WEAPON, ARMOUR, ITEM };
        for (int k = 0; k < 3; ++k) {
            g_npcQuery.clear();
            g_getObjsFn(gw, &g_npcQuery, &center, radius, kinds[k], 256, 0);
            total += (int)g_npcQuery.size();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return total; }
    return total;
}

// ---- Phase W1: world-item (ground drop) replication ------------------------
namespace {
// CONTENT fingerprint of one world item: stringID + type + qty + quality. Position is
// DELIBERATELY excluded - the join may re-ground a spawned proxy's Y slightly, so a
// pos-inclusive hash would never match host vs join. The caller tracks position
// separately (the WorldItemEntry carries x/y/z; the oracle matches pos within tolerance).
unsigned int worldItemHash(const char* sid, unsigned int type, unsigned short qty,
                           unsigned short quality) {
    unsigned int h = 2166136261u;
    if (sid) for (const char* p = sid; *p; ++p) { h ^= (unsigned char)*p; h *= 16777619u; }
    h ^= (unsigned int)type    * 2654435761u;
    h ^= (unsigned int)qty     * 40503u;
    h ^= (unsigned int)quality * 2246822519u;
    return h ? h : 1u;
}
} // namespace

unsigned int captureWorldItems(GameWorld* gw, WorldItemRaw* out, unsigned int maxOut,
                               float radius) {
    if (!gw || !gw->player || !out || maxOut == 0 || !g_getObjsFn) return 0;
    unsigned int n = 0;
    __try {
        if (gw->player->playerCharacters.size() == 0) return 0;
        Character* ld = gw->player->playerCharacters[0];
        if (!ld) return 0;
        Ogre::Vector3 center = ld->getPosition();
        // A dropped item enumerates under multiple category queries (W0 finding), so
        // scan WEAPON/ARMOUR/ITEM and DEDUPE by hand (index+serial) to avoid triples.
        const itemType kinds[] = { ITEM, WEAPON, ARMOUR };
        for (int k = 0; k < 3 && n < maxOut; ++k) {
            g_npcQuery.clear();
            g_getObjsFn(gw, &g_npcQuery, &center, radius, kinds[k], 256, 0);
            unsigned int total = g_npcQuery.size();
            for (unsigned int i = 0; i < total && n < maxOut; ++i) {
                RootObject* o = g_npcQuery[i]; if (!o) continue;
                unsigned int hd[5] = {0,0,0,0,0};
                if (!readObjectHand(o, hd)) continue;
                // Dedupe by (index, serial) against what we've already collected.
                bool dup = false;
                for (unsigned int j = 0; j < n; ++j)
                    if (out[j].hand[3] == hd[3] && out[j].hand[4] == hd[4]) { dup = true; break; }
                if (dup) continue;
                GameData* gd = 0;
                __try { gd = o->getGameData(); } __except (EXCEPTION_EXECUTE_HANDLER) { gd = 0; }
                if (!gd) continue;
                Ogre::Vector3 p(0,0,0);
                __try { p = o->getPosition(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                Item* it = reinterpret_cast<Item*>(o);
                int qty = 1; float ql = 0.0f;
                __try { qty = it->quantity; ql = it->quality; } __except (EXCEPTION_EXECUTE_HANDLER) { qty = 1; ql = 0.0f; }
                if (qty < 1) qty = 1;
                WorldItemRaw& w = out[n];
                for (int t = 0; t < 5; ++t) w.hand[t] = hd[t];
                strncpy(w.stringID, gd->stringID.c_str(), sizeof(w.stringID) - 1);
                w.stringID[sizeof(w.stringID) - 1] = '\0';
                w.itemType = (unsigned int)gd->type;
                w.quantity = (unsigned short)(qty > 0xFFFF ? 0xFFFF : qty);
                w.quality  = (unsigned short)(ql > 0.0f ? (int)(ql * 100.0f) : 0);
                w.x = p.x; w.y = p.y; w.z = p.z;
                w.hash = worldItemHash(w.stringID, w.itemType, w.quantity, w.quality);
                ++n;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return n;
    }
    return n;
}

RootObject* spawnWorldItemProxy(GameWorld* gw, const char* sid, unsigned int typeCat,
                                int qty, float x, float y, float z) {
    if (!gw || !gw->theFactory || !g_createObjFn || !sid || !sid[0]) return 0;
    __try {
        GameData* tmpl = findItemTemplateImpl(gw, sid, typeCat);
        if (!tmpl) return 0;
        Ogre::Vector3 pos(x, y, z);
        Ogre::Quaternion rot = Ogre::Quaternion::IDENTITY;
        // owner=0 (unowned ground item), invisible=false (must render), no home building.
        RootObjectBase* ro = g_createObjFn(gw->theFactory, tmpl, pos, /*fromMod*/false,
                                           /*owner*/0, rot, /*cb*/0, /*container*/0,
                                           /*state*/0, /*invisible*/false, /*home*/0,
                                           /*age*/0.0f);
        if (!ro) return 0;
        // RootObjectBase is the topmost base (offset 0), so this is an address no-op
        // (same pattern as createBuilding -> RootObject*).
        RootObject* obj = reinterpret_cast<RootObject*>(ro);
        // Stacks: the factory mints a single unit; set the visible stack quantity.
        if (qty > 1) { __try { reinterpret_cast<Item*>(obj)->quantity = qty; } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        return obj;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void updateWorldItemProxy(RootObject* proxy, float x, float y, float z) {
    if (!proxy) return;
    __try {
        Ogre::Vector3 pos(x, y, z);
        Ogre::Quaternion rot = Ogre::Quaternion::IDENTITY;
        // setPositionRotation is virtual on Item; the proxy IS an Item.
        reinterpret_cast<Item*>(proxy)->setPositionRotation(pos, rot, /*fixedPosition*/true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool removeWorldItemProxy(GameWorld* gw, RootObject* proxy) {
    if (!gw || !proxy || !g_destroyObjFn) return false;
    __try {
        return g_destroyObjFn(gw, proxy, /*justUnloaded*/false, "coop-worlditem-cull");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int destroyWorldItemsNear(GameWorld* gw, float radius) {
    if (!gw || !gw->player || !g_getObjsFn || !g_destroyObjFn) return 0;
    int destroyed = 0;
    __try {
        if (gw->player->playerCharacters.size() == 0) return 0;
        Character* ld = gw->player->playerCharacters[0];
        if (!ld) return 0;
        Ogre::Vector3 center = ld->getPosition();
        // Collect distinct ground items first (the query enumerates one object under
        // multiple category filters), THEN destroy - destroying mutates the world, so we
        // must not destroy mid-enumeration of the shared scratch buffer.
        RootObject* victims[64]; unsigned int nv = 0;
        const itemType kinds[] = { ITEM, WEAPON, ARMOUR };
        for (int k = 0; k < 3 && nv < 64; ++k) {
            g_npcQuery.clear();
            g_getObjsFn(gw, &g_npcQuery, &center, radius, kinds[k], 256, 0);
            unsigned int total = g_npcQuery.size();
            for (unsigned int i = 0; i < total && nv < 64; ++i) {
                RootObject* o = g_npcQuery[i]; if (!o) continue;
                bool dup = false;
                for (unsigned int j = 0; j < nv; ++j) if (victims[j] == o) { dup = true; break; }
                if (!dup) victims[nv++] = o;
            }
        }
        for (unsigned int i = 0; i < nv; ++i) {
            if (g_destroyObjFn(gw, victims[i], /*justUnloaded*/false, "coop-worlditem-test-despawn"))
                ++destroyed;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return destroyed; }
    return destroyed;
}

// SEH-guarded DIAGNOSTIC: dump every nearby item (across ITEM/WEAPON/ARMOUR queries) so we
// can see what the engine actually reports for a UI-dropped weapon - its gd->type, its
// isInInventory flag, and distance. Used once when a drop-decrease can't be correlated to a
// ground copy, to learn WHY (not enumerated at all? enumerated but flagged in-inventory?).
void diagGroundScan(GameWorld* gw, const unsigned int cHand[5], const char* sid, float radius) {
    if (!gw || !g_getObjsFn) return;
    RootObject* ro = resolveObjectByHand(cHand);
    __try {
        // Resolve both candidate query centers: the OWNED character (what the detector uses)
        // and the player leader (what the working W1 captureWorldItems uses). Log both so we
        // can tell whether the owned-hand position is the problem.
        Ogre::Vector3 cpos(0,0,0); bool haveC = false;
        if (ro) { cpos = ro->getPosition(); haveC = true; }
        Ogre::Vector3 lpos(0,0,0); bool haveL = false;
        if (gw->player && gw->player->playerCharacters.size() > 0 && gw->player->playerCharacters[0]) {
            lpos = gw->player->playerCharacters[0]->getPosition(); haveL = true;
        }
        float dCL = (haveC && haveL)
            ? (float)sqrt((cpos.x-lpos.x)*(cpos.x-lpos.x) + (cpos.y-lpos.y)*(cpos.y-lpos.y) + (cpos.z-lpos.z)*(cpos.z-lpos.z))
            : -1.0f;
        char hb[220]; _snprintf(hb, sizeof(hb) - 1,
            "[wd] scan ownedPos=%.1f,%.1f,%.1f leaderPos=%.1f,%.1f,%.1f apart=%.1f wideR=%.0f",
            cpos.x, cpos.y, cpos.z, lpos.x, lpos.y, lpos.z, dCL, radius * 4.0f);
        hb[sizeof(hb) - 1] = '\0'; coop::logLine(hb);
        // Scan a WIDE radius around the LEADER (the center that works for W1) so a far/cursor
        // drop is still caught. Log every candidate with its distance from BOTH centers.
        Ogre::Vector3 center = haveL ? lpos : cpos;
        float wideR = radius * 4.0f;
        const itemType kinds[] = { ITEM, WEAPON, ARMOUR };
        const char* knm[] = { "ITEM", "WEAPON", "ARMOUR" };
        int logged = 0;
        for (int k = 0; k < 3; ++k) {
            g_npcQuery.clear();
            g_getObjsFn(gw, &g_npcQuery, &center, wideR, kinds[k], 256, 0);
            unsigned int n = g_npcQuery.size();
            char qb[120]; _snprintf(qb, sizeof(qb) - 1, "[wd] scan query=%s found=%u (leader-centered, R=%.0f)", knm[k], n, wideR);
            qb[sizeof(qb) - 1] = '\0'; coop::logLine(qb);
            for (unsigned int i = 0; i < n && logged < 30; ++i) {
                RootObject* o = g_npcQuery[i]; if (!o) continue;
                Item* it = reinterpret_cast<Item*>(o);
                GameData* gd = 0; __try { gd = it->getGameData(); } __except (EXCEPTION_EXECUTE_HANDLER) { gd = 0; }
                if (!gd) continue;
                int inInv = 0; __try { inInv = it->isInInventory ? 1 : 0; } __except (EXCEPTION_EXECUTE_HANDLER) {}
                Ogre::Vector3 p(0,0,0); __try { p = it->getPosition(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                float dL = (float)sqrt((p.x-lpos.x)*(p.x-lpos.x)+(p.y-lpos.y)*(p.y-lpos.y)+(p.z-lpos.z)*(p.z-lpos.z));
                float dC = (float)sqrt((p.x-cpos.x)*(p.x-cpos.x)+(p.y-cpos.y)*(p.y-cpos.y)+(p.z-cpos.z)*(p.z-cpos.z));
                char b[240]; _snprintf(b, sizeof(b) - 1,
                    "[wd]   cand sid='%s' type=%u inInv=%d dLeader=%.1f dOwned=%.1f%s",
                    gd->stringID.c_str(), (unsigned)gd->type, inInv, dL, dC,
                    (sid && sid[0] && strcmp(gd->stringID.c_str(), sid) == 0) ? " <== MATCH" : "");
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                ++logged;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// SEH-guarded: count FREE ground items (isInInventory==false) of (sid,type) within radius
// of the object at cHand. "Free" excludes the character's own worn/held copies, which the
// interest query also enumerates - so this isolates the item actually lying on the ground.
int countFreeGroundItemsNear(GameWorld* gw, const unsigned int cHand[5],
                             const char* sid, unsigned int typeCat, float radius) {
    if (!gw || !sid || !sid[0] || !g_getObjsFn) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    int count = 0;
    __try {
        Ogre::Vector3 center = ro->getPosition();
        // A dropped item enumerates under multiple category queries (W0 finding) - a
        // UI-dropped weapon often shows up only under ITEM, not WEAPON. Scan all three
        // and DEDUPE by object pointer so the same ground item isn't counted twice.
        const itemType kinds[] = { ITEM, WEAPON, ARMOUR };
        RootObject* seen[64]; unsigned int ns = 0;
        for (int k = 0; k < 3; ++k) {
            g_npcQuery.clear();
            g_getObjsFn(gw, &g_npcQuery, &center, radius, kinds[k], 256, 0);
            unsigned int n = g_npcQuery.size();
            for (unsigned int i = 0; i < n; ++i) {
                RootObject* o = g_npcQuery[i]; if (!o) continue;
                Item* it = reinterpret_cast<Item*>(o);   // WEAPON/ARMOUR/ITEM objects are Items
                GameData* gd = it->getGameData(); if (!gd) continue;
                if ((unsigned int)gd->type != typeCat) continue;
                if (strcmp(gd->stringID.c_str(), sid) != 0) continue;
                if (it->isInInventory) continue;          // skip the char's own worn/held copies
                bool dup = false;
                for (unsigned int j = 0; j < ns; ++j) if (seen[j] == o) { dup = true; break; }
                if (dup) continue;
                if (ns < 64) seen[ns++] = o;
                ++count;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}

// SEH-guarded SPIKE: pick up a FREE ground item of (sid,type) near cHand by RELOCATING the
// REAL object into the character's inventory (tryAddItem) - NO createItem. This is the
// conservation primitive that makes weapons work where fabrication fails: the dropped
// weapon already exists as a real object, so we just re-home it. Returns 1 on success.
int pickupWorldItemIntoInventory(GameWorld* gw, const unsigned int cHand[5],
                                 const char* sid, unsigned int typeCat, float radius) {
    if (!gw || !sid || !sid[0] || !g_getObjsFn) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    int picked = 0;
    __try {
        Ogre::Vector3 center = ro->getPosition();
        // Scan all categories (a dropped weapon often enumerates under ITEM, not WEAPON).
        const itemType kinds[] = { ITEM, WEAPON, ARMOUR };
        for (int k = 0; k < 3 && !picked; ++k) {
            g_npcQuery.clear();
            g_getObjsFn(gw, &g_npcQuery, &center, radius, kinds[k], 256, 0);
            unsigned int n = g_npcQuery.size();
            for (unsigned int i = 0; i < n; ++i) {
                RootObject* o = g_npcQuery[i]; if (!o) continue;
                Item* it = reinterpret_cast<Item*>(o);
                GameData* gd = it->getGameData(); if (!gd) continue;
                if ((unsigned int)gd->type != typeCat) continue;
                if (strcmp(gd->stringID.c_str(), sid) != 0) continue;
                if (it->isInInventory) continue;          // only a free ground item
                if (inv->tryAddItem(it, 1)) { picked = 1; break; } // relocate real object: bag <- ground
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return picked;
}

// SEH-guarded: position of the FIRST free ground item of (sid,type) within radius of cHand.
// Fills out[3] and returns 1 if found, else 0. The drop detector uses this to put the
// dropped weapon's world position on the wire so the peer mirrors it.
int firstFreeGroundItemPos(GameWorld* gw, const unsigned int cHand[5],
                           const char* sid, unsigned int typeCat, float radius, float out[3]) {
    if (!gw || !sid || !sid[0] || !g_getObjsFn || !out) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    int found = 0;
    __try {
        Ogre::Vector3 center = ro->getPosition();
        // A UI-dropped weapon often enumerates only under the ITEM category, not WEAPON
        // (W0 finding) - scan all three and filter by gd->type so we still locate it.
        const itemType kinds[] = { ITEM, WEAPON, ARMOUR };
        for (int k = 0; k < 3 && !found; ++k) {
            g_npcQuery.clear();
            g_getObjsFn(gw, &g_npcQuery, &center, radius, kinds[k], 256, 0);
            unsigned int n = g_npcQuery.size();
            for (unsigned int i = 0; i < n; ++i) {
                RootObject* o = g_npcQuery[i]; if (!o) continue;
                Item* it = reinterpret_cast<Item*>(o);
                GameData* gd = it->getGameData(); if (!gd) continue;
                if ((unsigned int)gd->type != typeCat) continue;
                if (strcmp(gd->stringID.c_str(), sid) != 0) continue;
                if (it->isInInventory) continue;
                Ogre::Vector3 p = it->getPosition();
                out[0] = p.x; out[1] = p.y; out[2] = p.z; found = 1; break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
}

// SEH-guarded: world position of the object at hand. Fills out[3], returns 1 on success.
// The drop detector uses this as the drop position when the engine's spatial item query
// can't locate the dropped weapon on the ground (e.g. in towns) - the weapon was dropped at
// the owner's feet anyway, so the owner's position is a faithful mirror target.
int objectWorldPos(const unsigned int hand[5], float out[3]) {
    if (!out) return 0;
    RootObject* ro = resolveObjectByHand(hand);
    if (!ro) return 0;
    int ok = 0;
    __try {
        Ogre::Vector3 p = ro->getPosition();
        out[0] = p.x; out[1] = p.y; out[2] = p.z; ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
}

// SEH-guarded (Phase W1b): handle-based liveness of a tracked ground item. Resolve the
// item's local engine hand and decide whether it is still a FREE ground item, WITHOUT
// the getObjectsWithinSphere query that fails in towns. Returns 1 (and fills out[3] with
// the current world position) if the item still exists and is NOT inside an inventory;
// returns 0 if it was destroyed (hand no longer resolves) or picked up (isInInventory).
// This replaces the "vanished from the spatial scan" cull, which false-culled town drops.
int groundItemLiveness(const unsigned int itemHand[5], float out[3]) {
    RootObject* ro = resolveObjectByHand(itemHand);
    if (!ro) return 0; // destroyed / no longer resolvable
    int live = 0;
    __try {
        Item* it = reinterpret_cast<Item*>(ro);
        if (it->isInInventory) return 0; // picked up into some inventory
        Ogre::Vector3 p = ro->getPosition();
        if (out) { out[0] = p.x; out[1] = p.y; out[2] = p.z; }
        live = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { live = 0; }
    return live;
}

// SEH-guarded (Phase W3): world position of a tracked Item* (as void*). The drop detector
// reads this off the REAL just-dropped object to author the exact cursor-drop location -
// far more accurate than the owner's feet, and it never relies on the (town-unreliable)
// spatial query. Fills out[3], returns 1 on success.
int itemWorldPos(void* item, float out[3]) {
    if (!out || !item) return 0;
    int ok = 0;
    __try {
        Ogre::Vector3 p = reinterpret_cast<RootObject*>(item)->getPosition();
        out[0] = p.x; out[1] = p.y; out[2] = p.z; ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
}

// SEH-guarded CONSERVATION primitive (Phase W2): relocate the owner's OWN copy of a weapon
// from its bag to the GROUND at (x,y,z) - the mirror of a drop performed on another client.
// Never fabricates: it finds the real item the peer already owns (shared save) and moves it
// (Inventory::dropItem). If only an EQUIPPED copy exists, it is unequipped to loose first
// (dropItem acts on loose items). Returns the number relocated (0 if the peer had no copy).
int relocateWeaponToGround(GameWorld* gw, const unsigned int ownerHand[5],
                           const char* sid, unsigned int typeCat,
                           float x, float y, float z, void** outDropped) {
    if (outDropped) *outDropped = 0;
    if (!gw || !sid || !sid[0]) return 0;
    RootObject* ro = resolveObjectByHand(ownerHand);
    if (!ro) return 0;
    // 1) Move a real copy to the ground. Try a loose copy first; if none, unequip a worn
    //    one to loose and drop that. dropItemFromInventory drops loose items only. Capture
    //    the dropped Item* so the conservation channel can re-home this exact object later
    //    (the spatial query can't re-find it in towns).
    void* droppedItem = 0;
    int dropped = dropItemFromInventory(gw, ownerHand, sid, typeCat, 1, &droppedItem);
    if (dropped == 0) {
        int un = unequipItemToLoose(gw, ownerHand, sid, typeCat, 1);
        if (un > 0) dropped = dropItemFromInventory(gw, ownerHand, sid, typeCat, 1, &droppedItem);
    }
    if (dropped == 0) return 0;
    if (outDropped) *outDropped = droppedItem;
    // 2) Reposition the just-dropped object to the mirrored world position so it lands in the
    //    same spot the dropping client placed it. We move the EXACT dropped Item* by handle
    //    (no spatial query - that fails in towns and would leave the item at the owner's feet).
    __try {
        if (droppedItem) {
            Ogre::Vector3 pos(x, y, z);
            Ogre::Quaternion rot = Ogre::Quaternion::IDENTITY;
            reinterpret_cast<Item*>(droppedItem)->setPositionRotation(pos, rot, /*fixedPosition*/true);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return dropped;
}

// SEH-guarded (auto-revert mitigation, W1 non-gear pickup): re-drop a ground proxy
// that a PEER picked up back onto the GROUND at (x,y,z). The peer must not RETAIN a
// non-gear W1 proxy (the authoring client still holds the real object - retaining
// the picked-up copy duplicates it), so we move it bag->ground via the engine's own
// Inventory::dropItem (the same virtual the drop hook wraps) - a clean relocation,
// NEVER a destroy. Then reposition the exact object to the tracked ground spot (the
// vanilla drop lands it at the character's feet). Returns 1 if it was in an inventory
// and got dropped, else 0.
int dropProxyItemToGround(GameWorld* gw, void* item, float x, float y, float z) {
    if (!gw || !item) return 0;
    int ok = 0;
    __try {
        Item* it = reinterpret_cast<Item*>(item);
        if (!it->isInInventory) return 0;    // still on the ground - nobody picked it up
        // The PARENT inventory holding this item. NOTE: Item::getInventory() returns the
        // item's OWN inventory (a container item's) - null for a plain item - so resolve
        // the holding character from _whosInventoryWeAreIn and take ITS inventory.
        const hand& h = it->_whosInventoryWeAreIn;
        unsigned int cHand[5];
        cHand[0] = (unsigned int)h.type; cHand[1] = h.container; cHand[2] = h.containerSerial;
        cHand[3] = h.index; cHand[4] = h.serial;
        RootObject* holder = resolveObjectByHand(cHand);
        if (!holder) return 0;
        Inventory* inv = holder->getInventory(); // the inventory currently holding it
        if (!inv) return 0;
        inv->dropItem(it);                   // virtual Inventory::dropItem: clean bag -> ground
        Ogre::Vector3 pos(x, y, z);
        Ogre::Quaternion rot = Ogre::Quaternion::IDENTITY;
        it->setPositionRotation(pos, rot, /*fixedPosition*/true);
        ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
}

// SEH-guarded (Phase W3): capture the WEAPON items the object at cHand currently holds, with
// their REAL Item* handles. The drop detector remembers these per tick so that when a weapon
// leaves the bag (drop), the prior tick's handle IS the now-grounded object - trackable for a
// later pickup without re-querying the world (the spatial query fails in towns). Fills
// sids[i] (NUL-terminated, <48) and items[i] (Item* as void*); returns the count.
unsigned int captureWeaponPtrs(GameWorld* gw, const unsigned int cHand[5],
                               char (*sids)[48], void** items, unsigned int maxOut) {
    if (!gw || !sids || !items || maxOut == 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    const unsigned int MAXC = 64;
    InvItemEntry ent[64];
    Item* its[64];
    unsigned int ncur = readInvItems(inv, ent, its, MAXC);
    unsigned int n = 0;
    for (unsigned int i = 0; i < ncur && n < maxOut; ++i) {
        // Gear only (itemType 2 = WEAPON, 3 = ARMOUR/clothing): the conservation channel tracks
        // these by real Item* handle. Stackable items stay on the W1 proxy stream.
        if (ent[i].itemType != 2u && ent[i].itemType != 3u) continue;
        strncpy(sids[n], ent[i].stringID, 47); sids[n][47] = '\0';
        items[n] = its[i];
        ++n;
    }
    return n;
}

// SEH-guarded (Phase W3): re-home a tracked ground Item* back into the inventory of the
// object at targetHand (Inventory::tryAddItem of the EXISTING object - no fabrication). This
// is the pickup mirror of relocateWeaponToGround. Returns 1 on success. The caller guarantees
// `item` is a real, still-live object it dropped earlier (conservation); SEH guards a stale
// handle from crashing, but a destroyed object should never be passed.
int addItemPtrToInventory(GameWorld* gw, const unsigned int targetHand[5], void* item) {
    if (!gw || !item) return 0;
    RootObject* ro = resolveObjectByHand(targetHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    int ok = 0;
    // Sanctioned conservation pickup (W3): re-homing a REAL tracked ground item
    // whose stale _whosInventoryWeAreIn could otherwise look cross-owner to the
    // trade veto. Suspend it so the sanctioned relocation is never refused.
    bool vetoSav = g_invVetoSuspend; g_invVetoSuspend = true;
    __try {
        ok = inv->tryAddItem(reinterpret_cast<Item*>(item), 1) ? 1 : 0; // virtual: ground -> bag
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    g_invVetoSuspend = vetoSav;
    return ok;
}

// SEH-guarded: report the first EQUIPPED item worn by the object at cHand. Writes its
// template stringID + itemType + equipped count, returns 1 if any worn item exists.
// The inv_equip scenario uses this to drive equip/unequip on a KNOWN, race-compatible
// template (the gear the character already wears), so no template-hunting is needed.
int findEquippedItemKey(GameWorld* gw, const unsigned int cHand[5],
                        char* outSid, unsigned int outLen, unsigned int* outType,
                        int* outEquippedCount) {
    if (outEquippedCount) *outEquippedCount = 0;
    if (!gw) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    const unsigned int MAXC = 64;
    InvItemEntry cur[64];
    unsigned int ncur = readInvItems(inv, cur, 0, MAXC);
    int found = 0; int eqCount = 0;
    for (unsigned int i = 0; i < ncur; ++i) {
        if (!cur[i].equipped) continue;
        ++eqCount;
        if (!found) {
            if (outSid && outLen) { strncpy(outSid, cur[i].stringID, outLen - 1); outSid[outLen - 1] = '\0'; }
            if (outType) *outType = cur[i].itemType;
            found = 1;
        }
    }
    if (outEquippedCount) *outEquippedCount = eqCount;
    return found;
}

// SEH-guarded: remove `qty` of the EQUIPPED (sid, type) worn by the object at cHand
// (the "drop/unequip armour" action). Returns the number removed. Used by inv_equip
// to prove that unequipping/removing worn gear propagates cross-client.
int removeEquippedItem(GameWorld* gw, const unsigned int cHand[5],
                       const char* sid, unsigned int typeCat, int qty) {
    if (!gw || !sid || !sid[0] || qty <= 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    const unsigned int MAXC = 64;
    InvItemEntry cur[64];
    Item* curItems[64];
    unsigned int ncur = readInvItems(inv, cur, curItems, MAXC);
    return removeByKey(inv, curItems, cur, ncur, sid, typeCat, qty, /*wantEquipped=*/1);
}

// SEH-guarded: create and EQUIP `qty` of (sid, type) onto the object at cHand (the
// "equip armour" action). Returns the number equipped. Used by inv_equip to prove
// that equipping worn gear propagates cross-client.
int addEquippedItem(GameWorld* gw, const unsigned int cHand[5],
                    const char* sid, unsigned int typeCat, int qty) {
    if (!gw || !sid || !sid[0] || qty <= 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    return createItemAndAdd(gw, inv, sid, typeCat, qty, 0, /*equip=*/true) ? qty : 0;
}

// SEH-guarded: UNEQUIP `qty` of the worn (sid,type) on the object at cHand to LOOSE
// inventory WITHOUT destroying it (move slot -> loose; the real "drag worn item into
// the bag" action). Unlike removeEquippedItem (which destroys), this PRESERVES the item
// so it can be re-equipped later. Returns how many moved. Used by inv_reequip.
int unequipItemToLoose(GameWorld* gw, const unsigned int cHand[5],
                       const char* sid, unsigned int typeCat, int qty) {
    if (!gw || !sid || !sid[0] || qty <= 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    const unsigned int MAXC = 64;
    InvItemEntry cur[64]; Item* curItems[64];
    unsigned int ncur = readInvItems(inv, cur, curItems, MAXC);
    int moved = 0;
    for (unsigned int i = 0; i < ncur && moved < qty; ++i) {
        if (!curItems[i] || !cur[i].equipped || cur[i].itemType != typeCat) continue;
        if (strcmp(cur[i].stringID, sid) != 0) continue;
        if (unequipToLoose(inv, curItems[i])) ++moved;
    }
    return moved;
}

// SEH-guarded: EQUIP `qty` of the LOOSE (sid,type) the object at cHand already holds
// (move loose -> slot; the real "drag item from bag onto the paperdoll" action). Equips
// a REAL item so it persists (fabricated equips are discarded, d25). Returns how many
// equipped. Used by inv_reequip to drive the UP path on save-loaded gear.
int reequipLooseItem(GameWorld* gw, const unsigned int cHand[5],
                     const char* sid, unsigned int typeCat, int qty) {
    if (!gw || !sid || !sid[0] || qty <= 0) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;
    const unsigned int MAXC = 64;
    InvItemEntry cur[64]; Item* curItems[64];
    unsigned int ncur = readInvItems(inv, cur, curItems, MAXC);
    int moved = 0;
    for (unsigned int i = 0; i < ncur && moved < qty; ++i) {
        if (!curItems[i] || cur[i].equipped || cur[i].itemType != typeCat) continue;
        if (strcmp(cur[i].stringID, sid) != 0) continue;
        if (equipExisting(inv, curItems[i])) ++moved;
    }
    return moved;
}

// The spike-451 weapon-mint recipe trace (the mkspy createItem/copyItem/Sword-ctor/
// chooseDataFromList detours + installCreateItemTraceHook, probeReplayWeaponMint,
// probeFabricateWeaponLoose, commonNovelWeaponSid) moved to EngineProbe.cpp (Phase
// 5e) - a HARNESS-ONLY TU, so the shipping Release DLL no longer carries the detour
// trampolines. diagWeaponCreate moved there too. createItemAndAdd +
// fallbackWeaponManufacturer stay here (external linkage) since inventory needs them.

// ---- Spike 401: research tech-tree store probe ------------------------------
// Real-RVA surfaces recovered from the on-disk 1.0.65 exe (string-xref +
// disassembly of the "Research already known" caller at 0x2b65d0 - the research
// UI's click handler: canResearch -> isKnown -> startResearch):
//   0x2134690  .data global whose +0x38 slot is the engine's LIVE Research*
//              (matches PlayerInterface::technology @ 0x38 - the global is the
//              PlayerInterface singleton the UI code reads)
//   0x82f300   Research::isKnown(GameData*) -> bool (special path for type 0x15,
//              else walks a vector reached through the container at this+0x10)
//   0x832fa0   Research::canResearch(GameData*, bool, bool) -> bool (the UI
//              passes false,false)
//   0x834550   Research::startResearch(GameData*) (dedupes the queue at
//              +0x38/+0x58, activates, fires the UI toast natively)
// These are ON-DISK 1.0.65 RVAs from disassembling the installed exe, so the
// only correct base is GetModuleHandle(NULL). Deriving the base from a
// KenshiLib-resolved neighbour (run 211124) crashed BOTH clients through the
// SEH guards - KenshiLib's GetRealAddress space need not be base+headerRVA
// (version remap), and a mid-instruction jump can land on __fastfail, which
// no SEH frame catches. The two bases are logged once for the evidence trail.

namespace {

const unsigned int R401_WIN = 0x800; // Research-object snapshot window (bytes)
unsigned char g_r401Snap[0x800];
bool  g_r401Have = false;
void* g_r401Ptr  = 0;

typedef bool (__fastcall* R401BoolGdFn)(void* research, GameData* gd);
typedef bool (__fastcall* R401CanFn)(void* research, GameData* gd,
                                     bool a, bool b);
typedef void (__fastcall* R401StartFn)(void* research, GameData* gd);

// SEH-guarded prologue compare - a raw-RVA call is only allowed when the live
// bytes are EXACTLY the disassembled function's prologue. A wrong base (or a
// different exe build) fails the compare instead of jumping into garbage,
// where a mid-instruction landing can hit __fastfail - unrecoverable by SEH
// (run 211124 killed both clients that way).
bool r401SigOk(unsigned __int64 addr, const unsigned char* sig,
               unsigned int n) {
    __try {
        return memcmp((const void*)addr, sig, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct R401Levers {
    R401BoolGdFn isKnown;
    R401CanFn    can;
    R401StartFn  start;
};

unsigned __int64 r401Base() {
    static unsigned __int64 base = 0;
    if (!base) base = (unsigned __int64)GetModuleHandleA(NULL);
    return base;
}

// Scan the running module's .text for a unique byte signature. The on-disk exe
// RVAs do NOT map to the live image by base+RVA (run 212346: klib's getTechLevel
// resolved to base+0x2ADE00, 0x470 past the on-disk 0x2AD990, and the on-disk
// bytes there are unrelated - the executing image differs from the file on
// disk). A prologue scan finds each function at its TRUE runtime address no
// matter the skew (each signature was verified count==1 in the file's .text).
// SEH-guarded page walk; returns 0 if not found.
unsigned __int64 r401ScanText(const unsigned char* sig, unsigned int n) {
    unsigned __int64 base = r401Base();
    if (!base || !sig || n == 0) return 0;
    __try {
        const unsigned char* p = (const unsigned char*)base;
        // PE -> optional header -> section table; find .text.
        unsigned int peOff = *(unsigned int*)(p + 0x3c);
        const unsigned char* pe = p + peOff;
        unsigned short nsec = *(unsigned short*)(pe + 6);
        unsigned short optSz = *(unsigned short*)(pe + 20);
        const unsigned char* sec = pe + 24 + optSz;
        unsigned __int64 txtVa = 0, txtSz = 0;
        for (unsigned short i = 0; i < nsec; ++i) {
            const unsigned char* s = sec + i * 40;
            if (memcmp(s, ".text", 5) == 0) {
                txtVa = *(unsigned int*)(s + 12);
                txtSz = *(unsigned int*)(s + 8);
                break;
            }
        }
        if (!txtVa || txtSz < n) return 0;
        const unsigned char* start = p + txtVa;
        const unsigned char* end = start + txtSz - n;
        unsigned char first = sig[0];
        for (const unsigned char* q = start; q <= end; ++q) {
            if (*q != first) continue;
            if (memcmp(q, sig, n) == 0) return (unsigned __int64)q;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 0;
}

// Locate the three lever entry points once by prologue scan. All-or-nothing:
// any miss disables every raw call this spike makes.
const R401Levers* r401GetLevers() {
    static R401Levers lv = { 0, 0, 0 };
    static int state = 0; // 0 unresolved, 1 ok, -1 refused
    if (state == 0) {
        state = -1;
        // 24-byte prologues (verified unique in the 1.0.65 .text).
        static const unsigned char sigKnown[] = {
            0x48,0x89,0x54,0x24,0x10,0x53,0x48,0x83,0xEC,0x20,0x48,0x8B,
            0xDA,0x48,0x83,0xC2,0x50,0x83,0x3A,0x15,0x74,0x39,0x48,0x83 };
        static const unsigned char sigCan[] = {
            0x40,0x55,0x56,0x57,0x48,0x8D,0x6C,0x24,0xB9,0x48,0x81,0xEC,
            0xA0,0x00,0x00,0x00,0x48,0xC7,0x45,0xFF,0xFE,0xFF,0xFF,0xFF };
        static const unsigned char sigStart[] = {
            0x48,0x8B,0xC4,0x55,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x48,
            0x8D,0xA8,0x38,0xFE,0xFF,0xFF,0x48,0x81,0xEC,0xA0,0x02,0x00 };
        unsigned __int64 a = r401ScanText(sigKnown, sizeof(sigKnown));
        unsigned __int64 b = r401ScanText(sigCan, sizeof(sigCan));
        unsigned __int64 c = r401ScanText(sigStart, sizeof(sigStart));
        char msg[200];
        _snprintf(msg, sizeof(msg) - 1,
                  "[r401] lever scan isKnown=%016llx can=%016llx start=%016llx",
                  a, b, c);
        msg[sizeof(msg) - 1] = '\0'; coop::logLine(msg);
        if (a && b && c) {
            lv.isKnown = (R401BoolGdFn)a;
            lv.can     = (R401CanFn)b;
            lv.start   = (R401StartFn)c;
            state = 1;
            coop::logLine("[r401] levers located by prologue scan");
        } else {
            coop::logLine("[r401] levers REFUSED (prologue not found)");
        }
    }
    return state == 1 ? &lv : 0;
}

// The live Research* is gw->player->technology (the KenshiLib header member at
// PlayerInterface+0x38). The on-disk-RVA global chain was dropped: the running
// image is base-skewed from the file (see r401ScanText), so a base+RVA global
// read returned null (run 212346). The header member resolved to a real object.
void* r401Research(GameWorld* gw) {
    __try {
        if (gw && gw->player) return gw->player->technology;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

// SEH-guarded lookup of a RESEARCH GameData by sid (main-thread scratch).
GameData* r401FindResearch(GameWorld* gw, const char* sid) {
    if (!gw || !sid || !sid[0] || !g_getDataOfTypeFn) return 0;
    __try {
        g_dataScratch.clear();
        g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, RESEARCH);
        unsigned int n = g_dataScratch.size();
        for (unsigned int i = 0; i < n; ++i) {
            GameData* t = g_dataScratch[i];
            if (!t) continue;
            const char* s = t->stringID.c_str();
            if (s && strcmp(s, sid) == 0) return t;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 0;
}

// SEH-guarded: true when p reads as a plausible GameData record (small type
// enum + short printable stringID). Bogus pointers fault into the handler or
// fail the plausibility checks - the mkspy classification trick.
bool r401GdProbe(void* p, int* outType, char* outSid, unsigned int outLen) {
    if (!p || ((ULONG_PTR)p & 0x7) || outLen < 2) return false;
    __try {
        GameData* gd = (GameData*)p;
        int t = (int)gd->type;
        if (t < 0 || t > 90) return false;
        const char* s = gd->stringID.c_str();
        if (!s || !s[0]) return false;
        unsigned int i = 0;
        for (; i < outLen - 1 && s[i]; ++i) {
            if ((unsigned char)s[i] < 0x20 || (unsigned char)s[i] > 0x7e)
                return false;
            outSid[i] = s[i];
        }
        outSid[i] = '\0';
        *outType = t;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

} // namespace

int probeResearchStore(GameWorld* gw, int phase) {
    if (!gw) return 0;
    unsigned char cur[R401_WIN];
    void* tech = r401Research(gw);
    if (!tech) return 0;
    // The interim out-of-process probing of this object CRASHED the host, so
    // every read stays in-process under its own SEH frame.
    __try {
        memcpy(cur, tech, R401_WIN);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("[r401] store read FAULTED");
        return -1;
    }
    char b[240];
    if (tech != g_r401Ptr) { g_r401Ptr = tech; g_r401Have = false; }
    if (phase == 0 || !g_r401Have) {
        memcpy(g_r401Snap, cur, R401_WIN);
        g_r401Have = true;
        _snprintf(b, sizeof(b) - 1, "[r401] store ptr=%p win=0x%x",
                  tech, R401_WIN);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        for (unsigned int off = 0; off < 0x100; off += 32) {
            unsigned __int64 q[4];
            memcpy(q, cur + off, sizeof(q));
            _snprintf(b, sizeof(b) - 1,
                      "[r401] hex 0x%03x: %016llx %016llx %016llx %016llx",
                      off, q[0], q[1], q[2], q[3]);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        for (unsigned int off = 0; off + 8 <= 0x400; off += 8) {
            void* p = 0;
            memcpy(&p, cur + off, sizeof(p));
            int t = -1; char sid[48];
            if (r401GdProbe(p, &t, sid, sizeof(sid))) {
                _snprintf(b, sizeof(b) - 1,
                          "[r401] slot 0x%03x -> GameData type=%d sid='%s'",
                          off, t, sid);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }
        }
        return 1;
    }
    int nd = 0;
    for (unsigned int off = 0; off + 4 <= R401_WIN; off += 4) {
        unsigned int a, c;
        memcpy(&a, g_r401Snap + off, 4);
        memcpy(&c, cur + off, 4);
        if (a == c) continue;
        float fa, fc;
        memcpy(&fa, &a, 4); memcpy(&fc, &c, 4);
        _snprintf(b, sizeof(b) - 1,
                  "[r401] diff 0x%03x %08x->%08x (f %.5g->%.5g)",
                  off, a, c, fa, fc);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (++nd >= 40) { coop::logLine("[r401] diff TRUNCATED"); break; }
    }
    if (nd == 0) coop::logLine("[r401] diff none");
    memcpy(g_r401Snap, cur, R401_WIN);
    return 1;
}

int probeCurrentResearchSid(char* outSid, unsigned int outLen) {
    if (outSid && outLen) outSid[0] = '\0';
    // Unresolvable for now: ManagementScreen.h does not compile under VC100
    // (its inline ReorderableList template trips C2065), and the KenshiLib
    // header RVAs (getSingleton 0x2967F0 / singleton 0x212C428) live in
    // GetRealAddress's REMAPPED space, not at base+RVA on the 1.0.65 exe -
    // the on-disk bytes at those offsets are unrelated code (spike-401 run
    // 211124 finding). The current-research identity comes from the store
    // diff instead; a UI read can be re-added if a real on-disk anchor for
    // the ManagementScreen singleton is ever recovered.
    return 0;
}

int researchQueryBySid(GameWorld* gw, const char* sid, int* outKnown,
                       int* outCan) {
    if (outKnown) *outKnown = -1;
    if (outCan)   *outCan   = -1;
    void* res = r401Research(gw);
    const R401Levers* lv = r401GetLevers();
    if (!res || !lv) return 0;
    GameData* gd = r401FindResearch(gw, sid);
    if (!gd) return 0;
    __try {
        if (outKnown) *outKnown = lv->isKnown(res, gd) ? 1 : 0;
        // The research UI's click handler passes (gd, false, false).
        if (outCan)   *outCan   = lv->can(res, gd, false, false) ? 1 : 0;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

int researchStartBySid(GameWorld* gw, const char* sid) {
    void* res = r401Research(gw);
    const R401Levers* lv = r401GetLevers();
    if (!res || !lv) return 0;
    GameData* gd = r401FindResearch(gw, sid);
    if (!gd) return 0;
    __try {
        lv->start(res, gd);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

int researchPickSubject(GameWorld* gw, char* outSid, unsigned int outLen) {
    if (outSid && outLen) outSid[0] = '\0';
    void* res = r401Research(gw);
    const R401Levers* lv = r401GetLevers();
    if (!gw || !res || !lv || !g_getDataOfTypeFn || !outSid || outLen < 2)
        return 0;
    __try {
        g_dataScratch.clear();
        g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, RESEARCH);
        unsigned int n = g_dataScratch.size();
        for (unsigned int i = 0; i < n; ++i) {
            GameData* t = g_dataScratch[i];
            if (!t) continue;
            if (lv->isKnown(res, t)) continue;
            if (!lv->can(res, t, false, false)) continue;
            const char* s = t->stringID.c_str();
            if (!s || !s[0]) continue;
            strncpy(outSid, s, outLen - 1);
            outSid[outLen - 1] = '\0';
            return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return 0;
}

unsigned int probeResearchEnum(GameWorld* gw, unsigned int maxLog) {
    void* res = r401Research(gw);
    const R401Levers* lv = r401GetLevers();
    if (!gw || !res || !lv || !g_getDataOfTypeFn) return 0;
    unsigned int logged = 0, total = 0, known = 0;
    __try {
        g_dataScratch.clear();
        g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, RESEARCH);
        unsigned int n = g_dataScratch.size();
        total = n;
        char b[220];
        for (unsigned int i = 0; i < n; ++i) {
            GameData* t = g_dataScratch[i];
            if (!t) continue;
            bool k = lv->isKnown(res, t);
            if (k) ++known;
            if (logged < maxLog) {
                bool c = lv->can(res, t, false, false);
                _snprintf(b, sizeof(b) - 1,
                          "[r401] research[%u] sid='%s' name='%s' known=%d can=%d",
                          i, t->stringID.c_str(), t->name.c_str(),
                          k ? 1 : 0, c ? 1 : 0);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                ++logged;
            }
        }
        _snprintf(b, sizeof(b) - 1, "[r401] research total=%u known=%u",
                  total, known);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("[r401] enum FAULTED");
        return logged;
    }
    return total;
}

unsigned int researchEnumKnown(GameWorld* gw, char* outSids,
                               unsigned int sidCap, unsigned int maxN) {
    void* res = r401Research(gw);
    const R401Levers* lv = r401GetLevers();
    if (!gw || !res || !lv || !g_getDataOfTypeFn || !outSids || sidCap < 2)
        return 0;
    unsigned int written = 0;
    __try {
        g_dataScratch.clear();
        g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, RESEARCH);
        unsigned int n = g_dataScratch.size();
        for (unsigned int i = 0; i < n && written < maxN; ++i) {
            GameData* t = g_dataScratch[i];
            if (!t) continue;
            if (!lv->isKnown(res, t)) continue;
            const char* s = t->stringID.c_str();
            if (!s || !s[0]) continue;
            char* dst = outSids + (size_t)written * sidCap;
            strncpy(dst, s, sidCap - 1);
            dst[sidCap - 1] = '\0';
            ++written;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return written; }
    return written;
}

int probeResearchBenchRead(const unsigned int mHand[5], int* outTech,
                           float* outProg, int* outPower) {
    if (outTech)  *outTech  = -1;
    if (outProg)  *outProg  = -1.0f;
    if (outPower) *outPower = -1;
    if (!mHand) return 0;
    RootObject* ro = resolveObjectByHand(mHand);
    if (!ro) return 0;
    __try {
        Building* bd = static_cast<Building*>(ro);
        if ((int)bd->classType != (int)BCTYPE_RESEARCH) return 0;
        UseableStuff* us = static_cast<UseableStuff*>(bd);
        if (outProg)  *outProg  = us->progressBarLevel;
        if (outPower) *outPower = us->powerOn ? 1 : 0;
        if (outTech && g_machTechLvlFn)
            *outTech = g_machTechLvlFn(static_cast<ResearchBuilding*>(bd));
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// diagWeaponCreate (the spike-451 weapon-fabrication diagnostic) moved to
// EngineProbe.cpp (Phase 5e, HARNESS-ONLY). It uses the now-external
// fallbackWeaponManufacturer + createItemAndAdd defined above.

// SEH-guarded: find a template the character at cHand can actually WEAR and equip it,
// so the inv_equip scenario has a known worn item to cycle even on a save whose squad
// members start naked. Walks ARMOUR then WEAPON templates, creating+equipping each and
// keeping the first that actually lands in a slot (it->isEquipped); non-wearable trials
// are destroyed so nothing leaks loose. Writes the winning stringID + itemType.
int seedEquippedItem(GameWorld* gw, const unsigned int cHand[5],
                     char* outSid, unsigned int outLen, unsigned int* outType) {
    if (outSid && outLen) outSid[0] = '\0';
    if (outType) *outType = 0;
    if (!gw || !gw->theFactory || !g_createItemFn || !g_handCtorFn ||
        !g_equipItemFn || !g_getDataOfTypeFn) return 0;
    RootObject* ro = resolveObjectByHand(cHand);
    if (!ro) return 0;
    Inventory* inv = invOf(ro);
    if (!inv) return 0;

    const itemType cats[2] = { ARMOUR, WEAPON };
    for (int c = 0; c < 2; ++c) {
        unsigned int n = 0;
        __try {
            g_dataScratch.clear();
            g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, cats[c]);
            n = g_dataScratch.size();
        } __except (EXCEPTION_EXECUTE_HANDLER) { n = 0; }
        unsigned int tried = 0;
        for (unsigned int i = 0; i < n && tried < 120; ++i) {
            GameData* tmpl = 0;
            __try { tmpl = g_dataScratch[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { tmpl = 0; }
            if (!tmpl) continue;
            ++tried;
            int ok = 0;
            char sidbuf[48]; sidbuf[0] = '\0';
            __try {
                char buf[sizeof(hand) + 16];
                memset(buf, 0, sizeof(buf));
                hand* h = reinterpret_cast<hand*>(buf);
                g_handCtorFn(h, 0, 0, cats[c], 0, 0);
                Item* it = g_createItemFn(gw->theFactory, tmpl, h, 0, 0, -1, 0);
                if (it) {
                    if (inv->tryAddItem(it, 1)) {                  // virtual
                        bool eq = g_equipItemFn(inv, it);
                        if (eq && it->isEquipped) {
                            const char* s = tmpl->stringID.c_str();
                            strncpy(sidbuf, s ? s : "", sizeof(sidbuf) - 1);
                            sidbuf[sizeof(sidbuf) - 1] = '\0';
                            ok = sidbuf[0] ? 1 : 0;
                        } else {
                            inv->removeItemAutoDestroy(it, 1);     // not wearable here
                        }
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
            if (ok) {
                if (outSid && outLen) { strncpy(outSid, sidbuf, outLen - 1); outSid[outLen - 1] = '\0'; }
                if (outType) *outType = (unsigned int)cats[c];
                return 1;
            }
        }
    }
    return 0;
}

bool setupInventoryScene(GameWorld* gw, unsigned int outHand[5]) {
    if (outHand) { for (int i = 0; i < 5; ++i) outHand[i] = 0; }
    if (!gw || !gw->player) { coop::logLine("SETUP(inventory): no player interface"); return false; }
    unsigned int ch[5];
    if (!pickInventoryContainer(gw, ch)) {
        coop::logLine("SETUP(inventory): could not resolve leader container");
        return false;
    }
    char sid[48]; sid[0] = '\0';
    int added = addTestItemsToContainer(gw, ch, 2, sid, sizeof(sid));
    char b[220];
    _snprintf(b, sizeof(b) - 1,
        "SETUP(inventory): anchor=leader hand=%u,%u,%u,%u,%u seeded=%d x '%s'",
        ch[0], ch[1], ch[2], ch[3], ch[4], added, sid[0] ? sid : "(none)");
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    if (outHand) { for (int i = 0; i < 5; ++i) outHand[i] = ch[i]; }
    return true;
}

bool spawnSeatInFront(GameWorld* gw, float fwd, float side, RootObject** spawned) {
    if (spawned) *spawned = 0;
    if (!gw || !gw->theFactory || !g_createBldgFn) {
        coop::logLine("SETUP: seat spawn skipped (no factory / createBuilding fn)");
        return false;
    }
    __try {
        unsigned int nTemplates = 0;
        if (g_getDataOfTypeFn) {
            g_dataScratch.clear();
            g_getDataOfTypeFn(&gw->gamedata, &g_dataScratch, BUILDING);
            nTemplates = g_dataScratch.size();
        }
        GameData* tmpl = findSeatTemplate(gw);
        {
            char d[200];
            _snprintf(d, sizeof(d) - 1, "SETUP: BUILDING templates=%u seatTemplate='%s'",
                      nTemplates, tmpl ? tmpl->name.c_str() : "(none)");
            d[sizeof(d) - 1] = '\0';
            coop::logLine(d);
        }
        if (!tmpl) return false;
        Ogre::Vector3 pos; float yaw = 0.0f;
        if (!leaderAnchor(gw, fwd, side, &pos, &yaw)) return false;
        {
            char d[160];
            _snprintf(d, sizeof(d) - 1, "SETUP: seat reqPos=%.2f,%.2f,%.2f yaw=%.2f",
                      pos.x, pos.y, pos.z, yaw);
            d[sizeof(d) - 1] = '\0';
            coop::logLine(d);
        }
        // Face the seat back toward the leader so a seated body looks outward.
        Ogre::Quaternion rot(Ogre::Radian(yaw + 3.14159265f), Ogre::Vector3::UNIT_Y);
        // createBuilding adds the terrain height to position.y (a seat passed at the
        // absolute ground Y floats ~terrain-height up). Pass y=0 so it re-grounds.
        Ogre::Vector3 placePos(pos.x, 0.0f, pos.z);
        Building* b = g_createBldgFn(
            gw->theFactory, tmpl, placePos, /*town*/0, /*owner*/0, rot, /*cb*/0,
            /*furnitureOf*/0, /*isDoorOf*/0, /*saveState*/0, /*isIndoorsOf*/0,
            /*invisible*/false, /*completed*/true, /*isFoliage*/false,
            /*floor*/0, /*isOutsideFurniture*/false);
        if (!b) { coop::logLine("SETUP: createBuilding returned null"); return false; }
        // Building's first base is RootObject (offset 0), so this reinterpret is a
        // no-op address-wise - lets us avoid pulling in the heavy Building.h.
        RootObject* ro = reinterpret_cast<RootObject*>(b);
        Ogre::Vector3 ap = ro->getPosition(); // virtual: where it actually landed
        {
            char d[160];
            _snprintf(d, sizeof(d) - 1, "SETUP: seat actualPos=%.2f,%.2f,%.2f visible=%d",
                      ap.x, ap.y, ap.z, ro->getVisible() ? 1 : 0);
            d[sizeof(d) - 1] = '\0';
            coop::logLine(d);
        }
        if (spawned) *spawned = ro;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        coop::logLine("SETUP: createBuilding FAULTED");
        return false;
    }
}

Character* spawnNpcInFront(GameWorld* gw, float fwd, float side) {
    if (!gw || !gw->theFactory || !g_createCharFn) return 0;
    __try {
        Character* ld = (gw->player && gw->player->playerCharacters.size())
                            ? gw->player->playerCharacters[0] : 0;
        if (!ld) return 0;
        Faction* fac = ld->getFaction();
        GameData* tmpl = ld->getGameData();
        if (!fac || !tmpl) return 0;
        Ogre::Vector3 pos; float yaw = 0.0f;
        if (!leaderAnchor(gw, fwd, side, &pos, &yaw)) return 0;
        // owner=0: createRandomCharacter makes its own container; NOT enlisted in
        // the player squad, so the receiver drives it via the NPC path.
        RootObject* r = g_createCharFn(gw->theFactory, fac, pos, 0, tmpl, 0, 25.0f);
        return r ? static_cast<Character*>(r) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Spawn a world character in a GIVEN faction (not the player's), so it is NOT a
// squad member and flows through the host-authoritative world-NPC path. Body uses
// the leader's race template (a valid mesh); only the faction differs. SEH-guarded.
Character* spawnCharInFaction(GameWorld* gw, float fwd, float side, Faction* fac) {
    if (!gw || !gw->theFactory || !g_createCharFn || !fac) return 0;
    __try {
        Character* ld = (gw->player && gw->player->playerCharacters.size())
                            ? gw->player->playerCharacters[0] : 0;
        if (!ld) return 0;
        GameData* tmpl = ld->getGameData();
        if (!tmpl) return 0;
        Ogre::Vector3 pos; float yaw = 0.0f;
        if (!leaderAnchor(gw, fwd, side, &pos, &yaw)) return 0;
        RootObject* r = g_createCharFn(gw->theFactory, fac, pos, 0, tmpl, 0, 25.0f);
        return r ? static_cast<Character*>(r) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}


} // namespace engine
} // namespace coop
