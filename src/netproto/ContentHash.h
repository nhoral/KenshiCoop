// Content-fingerprint hashing shared by the plugin (Engine.cpp change detection)
// and the prototest unit layer. These MUST be identical on both clients (the
// cross-client content hash IS the inventory-sync oracle's convergence key), so
// they live in netproto/ next to the wire structs they fingerprint, with the
// unit test locking their behaviour.
//
// Plain C++03 for the VS2010 (v100) toolchain. Header-only (inline).

#ifndef KENSHICOOP_CONTENTHASH_H
#define KENSHICOOP_CONTENTHASH_H

#include "Wire.h"

namespace coop {

// FNV-1a over a section NAME, folded to 16 bits. The section set is built
// identically from race/inventory data on both clients, so the same name yields
// the same hash - a stable cross-client section identity that survives the wire
// (where STL strings cannot). 0 is reserved for "no section" (loose), so a real
// section that hashes to 0 is nudged to 1 (collision with "loose" would be worse
// than a 1-in-65k name clash).
inline unsigned short sectionNameHash(const char* name) {
    if (!name || !name[0]) return 0;
    unsigned int h = 2166136261u;
    for (const char* p = name; *p; ++p) { h ^= (unsigned char)*p; h *= 16777619u; }
    unsigned short s = (unsigned short)((h ^ (h >> 16)) & 0xFFFFu);
    return s ? s : 1;
}

// Order-independent per-entry hash (FNV-1a over stringID, mixed with type/qty/
// qual/equip/slot/section/manufacturer/material). Summing these across a
// container's entries yields a content fingerprint that is invariant to item
// ordering, so the host only re-sends a snapshot on real change - and the
// inv-sync oracles compare exactly these sums cross-client.
inline unsigned int invEntryHash(const InvItemEntry& e) {
    unsigned int h = 2166136261u;
    for (const char* p = e.stringID; *p; ++p) { h ^= (unsigned char)*p; h *= 16777619u; }
    h ^= (unsigned int)e.itemType * 2654435761u;
    h ^= (unsigned int)e.quantity * 40503u;
    h ^= (unsigned int)e.quality  * 2246822519u;
    // Equipped vs loose is a DISTINCT content state: an item worn in a slot must hash
    // differently from the same item sitting loose, so equipping/unequipping registers
    // as a content change (triggers a resend) and the peer reconciles the slot too.
    h ^= (unsigned int)(e.equipped ? 0x9E3779B9u : 0u);
    h ^= (unsigned int)e.slot * 2716044179u;
    // The SECTION must be part of the fingerprint too: the two weapon slots ('hip' vs
    // 'back') share AttachSlot ATTACH_WEAPON, so `slot` is identical for both - without
    // hashing the section a Weapon I<->II move produces an UNCHANGED fingerprint and is
    // never published, so the peer never learns the slot changed.
    h ^= (unsigned int)e.section * 2475825337u;
    // Manufacturer + material are part of a WEAPON's identity (mesh/company + grade): two
    // otherwise-identical base weapons with different manufacturers are visually distinct,
    // so they must hash differently (a swap registers as a content change + resend).
    for (const char* p = e.manufacturer; *p; ++p) { h ^= (unsigned char)*p; h *= 16777619u; }
    for (const char* p = e.material;     *p; ++p) { h ^= (unsigned char)*p; h *= 16777619u; }
    return h;
}

} // namespace coop

#endif // KENSHICOOP_CONTENTHASH_H
