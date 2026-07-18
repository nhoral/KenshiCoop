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
    // Phase 6b: the shackle LOCK state is content: a locked vs unlocked shackle must
    // hash differently so a lock toggle registers as a content change (triggers a
    // resend) and the peer re-learns the owner-authoritative lock state.
    h ^= (unsigned int)(e.locked ? 0x85EBCA6Bu : 0u);
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

// Protocol 31 (coordinated save): incremental FNV-1a-32 over raw bytes - the
// per-file CRC in the PKT_SAVE_DONE table. Seed with fnv1aInit(), fold each
// chunk with fnv1aUpdate() as it is read/written, so neither sender nor
// receiver ever needs the whole file in memory. Same function on both ends
// (and locked by prototest) = the transfer's integrity proof.
inline unsigned int fnv1aInit() { return 2166136261u; }
inline unsigned int fnv1aUpdate(unsigned int h, const void* data, unsigned int len) {
    const unsigned char* p = (const unsigned char*)data;
    for (unsigned int i = 0; i < len; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

// Protocol 32 (coordinated load): the FOLDER FINGERPRINT the host attaches to
// PKT_LOAD_GO and the join compares against its on-disk copy. FNV-1a folded
// over each file's LOWER-CASED relative path (the NUL rides along as the
// path/CRC separator) followed by its content CRC (fnv1a over the file
// bytes), in ascending lower-cased-path order. Sorting inside makes the
// result invariant to directory enumeration order (FindFirstFile order is
// filesystem-dependent); lower-casing matches Windows path case-
// insensitivity. Byte-identical folders agree on both machines; any file
// added/removed/renamed/rewritten changes the value. 0 is reserved for
// "missing/unreadable folder", so a real fingerprint landing on 0 is nudged
// to 1. Locked by prototest (determinism + order-invariance + sensitivity).
inline unsigned int folderFingerprintOf(const char* const* relPaths,
                                        const unsigned int* crcs,
                                        unsigned int count) {
    if (count == 0) return 0;
    enum { FP_MAX_FILES = 4096 };
    if (count > FP_MAX_FILES) return 0;
    // Sort an index array by lower-cased path (insertion sort: save folders
    // are tens of files; no <algorithm>/functor plumbing needed for v100).
    unsigned int idx[FP_MAX_FILES];
    for (unsigned int i = 0; i < count; ++i) idx[i] = i;
    for (unsigned int i = 1; i < count; ++i) {
        unsigned int k = idx[i];
        int j = (int)i - 1;
        while (j >= 0) {
            const char* a = relPaths[idx[j]];
            const char* b = relPaths[k];
            int cmp = 0;
            for (;; ++a, ++b) {
                int ca = (unsigned char)*a, cb = (unsigned char)*b;
                if (ca >= 'A' && ca <= 'Z') ca += 32;
                if (cb >= 'A' && cb <= 'Z') cb += 32;
                if (ca != cb) { cmp = ca - cb; break; }
                if (ca == 0) break;
            }
            if (cmp <= 0) break;
            idx[j + 1] = idx[j];
            --j;
        }
        idx[(unsigned int)(j + 1)] = k;
    }
    unsigned int h = fnv1aInit();
    for (unsigned int i = 0; i < count; ++i) {
        const char* p = relPaths[idx[i]];
        for (; *p; ++p) {
            int c = (unsigned char)*p;
            if (c >= 'A' && c <= 'Z') c += 32;
            h ^= (unsigned int)c; h *= 16777619u;
        }
        h ^= 0u; h *= 16777619u; // the path's NUL separator
        unsigned int crc = crcs[idx[i]];
        h = fnv1aUpdate(h, &crc, sizeof(crc));
    }
    return h ? h : 1;
}

} // namespace coop

#endif // KENSHICOOP_CONTENTHASH_H
