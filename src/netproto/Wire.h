// KenshiCoop unified wire protocol (clean rebuild, v1).
//
// Compiled by the VS2010 (v100) plugin only (the legacy nettest still uses the
// old Protocol.h). Keep it plain C++03: no <cstdint> reliance, no constexpr,
// no scoped enums, no STL on the wire. Wire format is packed, little-endian;
// x86-64 is little-endian on both ends so we send the struct bytes directly.

#ifndef KENSHICOOP_WIRE_H
#define KENSHICOOP_WIRE_H

#include <string.h> // memcpy

namespace coop {

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef float          f32;

// Protocol version. Bumped to 2 when EntityState gained the bodyState field
// (Stage 2 down/dead/ragdoll replication); to 3 when the reliable event channel
// (PKT_EVENT: KO/death/revive transitions) was added; to 4 when the combat intent
// (TASK_COMBAT_MELEE) began riding the existing task+subject fields (Stage 3c); to
// 5 when the inventory/container-contents snapshot (PKT_INV_SNAPSHOT, Phase 4a) was
// added; to 6 when InvItemEntry gained the equipped flag + slot (equipped armour/
// weapon sync); to 7 when the spare pad became `section` (a section-name hash so the
// two weapon slots - both AttachSlot ATTACH_WEAPON, indistinguishable by `slot` - are
// told apart and Weapon I/II placement replicates); to 8 when the world-item channel
// (PKT_WORLD_ITEM snapshot + PKT_WORLD_ITEM_REMOVE, Phase W1) was added - host-
// authoritative, interest-scoped, netId-keyed ground items; to 9 when InvItemEntry gained
// manufacturer + material stringIDs (WEAPONS cannot be reconstructed by the engine factory
// without their manufacturer/mesh GameData - createItem returns null - so a picked-up or
// looted weapon silently failed to appear on the peer; armour/loose items never needed it);
// to 10 when the CONSERVATION drop channel (PKT_WORLD_DROP, Phase W2) was added - a weapon
// can't be fabricated on a peer, so instead of streaming a template the dropper authors a
// reliable DROP intent and each client RELOCATES its own real copy of that weapon to the
// ground (bag -> world). Bidirectional (host or join may drop). Checked during handshake;
// a mismatch is rejected (no back-compat);
// to 11 when the conservation PICKUP channel (PKT_WORLD_PICKUP, Phase W3) was added - the
// mirror of a drop: when a character picks a dropped weapon up, its owner authors a reliable
// PICKUP intent and each peer relocates the REAL ground copy it has been tracking back into
// that character's bag (world -> bag). Because the engine's spatial item query is unreliable
// in towns, each client tracks the actual dropped Item* handle rather than re-finding it.
const u16 PROTOCOL_VERSION = 11;

// Packet type tags (first byte of every packet).
enum PacketType {
    PKT_HELLO            = 1, // client -> host on connect: version + name
    PKT_WELCOME          = 2, // host -> client: version echo + assigned playerId
    PKT_LEAVE            = 3, // net thread -> game thread marker: a peer left
    PKT_ENTITY_BATCH     = 4, // either direction: owner-tagged EntityState batch (20 Hz)
    PKT_EVENT            = 5, // RELIABLE one-shot transition (KO/death/revive); see EventPacket
    PKT_INV_SNAPSHOT     = 6, // RELIABLE container-contents snapshot (Phase 4a); InvSnapshotHeader
    PKT_WORLD_ITEM       = 7, // RELIABLE world-item snapshot (Phase W1); WorldItemSnapshotHeader
    PKT_WORLD_ITEM_REMOVE= 8, // RELIABLE world-item cull by netId (Phase W1); WorldItemRemoveHeader
    PKT_WORLD_DROP       = 9, // RELIABLE conservation drop intent (Phase W2); WorldDropPacket
    PKT_WORLD_PICKUP     = 10 // RELIABLE conservation pickup intent (Phase W3); WorldPickupPacket
};

// One-shot transition events carried on the RELIABLE channel. Continuous state
// (EntityState.bodyState) self-heals at 20 Hz over the unreliable channel, but a
// transition that MUST be observed exactly once - a death, a KO landing, later a
// combat hit - cannot tolerate a dropped datagram. These ride the reliable channel
// so they are never lost or reordered. Doctrine 16: state unreliable, events reliable.
enum EventType {
    EVT_NONE     = 0,
    EVT_KNOCKOUT = 1, // subject went down / unconscious (BODY_DOWN edge)
    EVT_DEATH    = 2, // subject died (BODY_DEAD edge) - permanent, latched on the join
    EVT_REVIVE   = 3  // subject stood back up (down -> upright edge)
};

// Sentinel ownerId meaning "all remote peers" (used on local disconnect to sweep
// every driven body at once).
const u32 OWNER_ID_ALL = 0xFFFFFFFFu;

#pragma pack(push, 1)

struct HelloPacket {
    u8  type;    // = PKT_HELLO
    u16 version; // = PROTOCOL_VERSION
    u8  nameLen; // bytes of name following this struct (0..63)
    // char name[nameLen] follows
};

struct WelcomePacket {
    u8  type;     // = PKT_WELCOME
    u16 version;  // host's PROTOCOL_VERSION (client re-checks)
    u32 playerId; // id the host assigned to this client
};

// A reliable one-shot transition. 'subject' is the hand the event happened TO; the
// 'actor' hand is the cause (attacker) and is all-zero until combat (L5). 'arg' is
// event-specific (e.g. damage) and 0 for KO/death. eventId is a per-sender monotonic
// counter for idempotent apply + log correlation. Subject/actor use the same five
// u32 hand fields as EntityState so the receiver resolves them identically.
struct EventPacket {
    u8  type;    // = PKT_EVENT
    u8  event;   // EventType
    u32 ownerId; // network player id of the sender
    u32 eventId; // monotonic per-sender
    // subject hand (whom it happened to)
    u32 sType;
    u32 sContainer;
    u32 sContainerSerial;
    u32 sIndex;
    u32 sSerial;
    // actor hand (the cause; zeroed until combat)
    u32 aType;
    u32 aContainer;
    u32 aContainerSerial;
    u32 aIndex;
    u32 aSerial;
    f32 arg;     // event-specific payload (damage, etc.); 0 for KO/death
};

// One replicated entity: save-stable hand identity + transform + locomotion +
// task/pose. Identity is the Kenshi `hand` (5 u32 fields), identical across
// machines that load the same save, so the receiver resolves it to its own
// local Character. This single shape carries both player-squad members and NPCs.
struct EntityState {
    // identity (hand)
    u32 hType;
    u32 hContainer;
    u32 hContainerSerial;
    u32 hIndex;
    u32 hSerial;
    // transform
    f32 x;
    f32 y;
    f32 z;
    f32 heading;        // radians (yaw)
    // locomotion (engine selects walk/idle/run from these; mirrored on receiver)
    f32 cSpeed;         // CharMovement.currentSpeed
    f32 cMotionX;       // CharMovement.currentMotion (world-space)
    f32 cMotionY;
    f32 cMotionZ;
    u8  cMoving;        // CharMovement.currentlyMoving (0/1)
    // pose: current task + the object that task targets (subject hand)
    u16 task;           // engine TaskType, or TASK_NONE
    u32 sType;
    u32 sContainer;
    u32 sContainerSerial;
    u32 sIndex;
    u32 sSerial;
    // diagnostic (AI-gating spike): the host's RAW top-level Tasker::key for this
    // body regardless of reproducibility, so the join can compare it to its own
    // local task and detect divergence. TASK_NONE if the body has no current task.
    u16 rawTask;
    // body state (Stage 2): bit-flags (BODY_*) read off the host's rendered Character
    // - down/KO, ragdoll, dead, crawling. 0 = upright/normal. The join reproduces the
    // down/dead posture from these (locomotion/task sync alone can't express a body
    // lying on the ground), and a body that is down must NOT be walk-driven/parked.
    u16 bodyState;
};

// Sentinel task value meaning "no current task this tick".
const u16 TASK_NONE = 0xFFFFu;

// Synthetic task value (NOT a real engine Tasker key) meaning "this body is in melee
// combat with the subject hand as its target" (Stage 3c, L5). Real engine task keys
// are small ints, so 0xFE00 cannot collide with one or with TASK_NONE. The host sets
// task=TASK_COMBAT_MELEE + the subject hand = the attack target when readCombat reports
// in-combat-with-target; the join reproduces the CAUSE by ordering its local copy to
// melee that same resolved target (let its own engine animate the fight). Combat takes
// priority over rest poses, so it overrides any reproducible sit/work task that frame.
const u16 TASK_COMBAT_MELEE = 0xFE00u;
inline bool taskIsCombat(u16 t) { return t == TASK_COMBAT_MELEE; }

// bodyState bit-flags. A body is "down" (on the ground, not upright) when any of
// BODY_DOWN / BODY_RAGDOLL / BODY_DEAD is set; BODY_CRAWL is an upright-ish stealth/
// crawl posture kept separate. Read from Character::isDown/isRagdoll/isDead/
// isStealthModeOrCrawling on the host.
const u16 BODY_DOWN    = 1 << 0; // Character::isDown()  (KO'd / unconscious / collapsed)
const u16 BODY_RAGDOLL = 1 << 1; // Character::isRagdoll()
const u16 BODY_DEAD    = 1 << 2; // Character::isDead()
const u16 BODY_CRAWL   = 1 << 3; // Character::isStealthModeOrCrawling()

// True if the body should be treated as lying down (suppress walk-drive / parking).
inline bool bodyIsDown(u16 s) { return (s & (BODY_DOWN | BODY_RAGDOLL | BODY_DEAD)) != 0; }

// An entity batch is: [EntityBatchHeader][EntityState * count]. ownerId tags the
// streaming peer; the receiver attributes every contained hand to that owner so
// it applies the right authority rule. Capped so one batch fits a datagram.
struct EntityBatchHeader {
    u8  type;    // = PKT_ENTITY_BATCH
    u32 ownerId; // network player id of the batch's owner
    u8  count;   // number of EntityState that follow
};

// 18 * sizeof(EntityState) + header stays comfortably under a 1400 B datagram.
const unsigned int ENTITY_BATCH_MAX = 18;

// ---- Phase 4a: container-contents (inventory) snapshot ---------------------
// World objects (items/buildings) carry the same save-stable `hand` as Characters,
// so a container that EXISTS in the shared save resolves cross-client. But crafting/
// loot mints NEW items at runtime whose hands are host-only and unresolvable on the
// join (same reason we bake saves). So we do NOT drive item objects by hand. Instead
// we stream the container's CONTENTS as a description - template stringID + itemType
// + quantity + quality - keyed by the CONTAINER's stable hand, and the join
// RECONSTRUCTS items locally (createItem/addItem, removeItemAutoDestroy) to match.
// Idempotent + loss-tolerant: a full snapshot re-applied reaches the same multiset.
// Sent on the RELIABLE channel on content-change (doctrine 16: transitions reliable),
// with a periodic safety resend.

// One item line in a container snapshot: the template identity (stringID) + its
// itemType category (for the template lookup) + stack quantity + a quality bucket.
// stringID is a fixed buffer (no STL on the wire); longer ids are truncated (the
// lookup tolerates a name fallback). quality is quality*100 (0 if not applicable).
struct InvItemEntry {
    char stringID[48];
    u32  itemType;   // GameData::type of the template (itemType enum)
    u16  quantity;
    u16  quality;
    u8   equipped;   // 1 if worn in an equipment slot (armour/weapon), else 0 (loose)
    u8   slot;       // AttachSlot the item occupies (advisory; equipItem auto-routes)
    u16  section;    // hash of the equip SECTION name (0 = loose / none). Distinguishes
                     // the two weapon slots ('hip' vs 'back'), which share AttachSlot
                     // ATTACH_WEAPON and so are identical in `slot`; lets the peer place
                     // a worn weapon in the SAME slot (Weapon I vs II) as the author.
    // A WEAPON is generated from a base def PLUS a manufacturer (mesh/company) and a
    // material spec; the engine factory (RootObjectFactory::createItem) REQUIRES the
    // manufacturer GameData (the `weaponMesh` arg) or it returns null, so without these
    // the peer cannot reconstruct a looted/picked-up weapon (it just never appears). They
    // are the GameData stringIDs (resolved on the peer via WEAPON_MANUFACTURER /
    // MATERIAL_SPECS_WEAPON); empty for armour/items, which need neither. They also feed
    // the content hash so a different manufacturer/material registers as a content change.
    char manufacturer[48];
    char material[48];
};

// An inventory snapshot is: [InvSnapshotHeader][InvItemEntry * count]. The container
// hand identifies WHOSE inventory this is (a storage building, a character, a chest).
// count == 0 is a valid "container is now empty" snapshot.
struct InvSnapshotHeader {
    u8  type;    // = PKT_INV_SNAPSHOT
    u32 ownerId; // network player id of the authoritative sender
    // container hand (whose inventory)
    u32 cType;
    u32 cContainer;
    u32 cContainerSerial;
    u32 cIndex;
    u32 cSerial;
    u8  count;   // number of InvItemEntry that follow
};

// A full snapshot ([header][InvItemEntry*count]) can now exceed one datagram (each entry
// carries two 48-byte template ids), but inv snapshots ride the RELIABLE channel, which
// ENet fragments + reassembles transparently. They are change-driven (rare), so the extra
// bytes cost nothing in steady state. 20 worn+loose entries covers a squad member.
const unsigned int INV_ITEMS_MAX = 20;

// ---- Phase W1: world-item (ground drop) snapshot ---------------------------
// Generalizes the Phase 4a content-snapshot/reconcile model from CONTAINERS to the open
// WORLD. The W0 drop_probe proved a player drop produces a free-standing world Item that
// getObjectsWithinSphere enumerates, carrying a host-RUNTIME hand the join cannot resolve
// (same blank-handle problem as crafted items). So world items are NOT hand-keyed: the
// host assigns each tracked ground item a synthetic `netId` (stable while the item lives),
// streams a descriptive snapshot (template + qty + quality + world position) for the items
// inside the players' interest sphere, and the join spawns/updates/culls a LOCAL proxy
// keyed by netId. Host-authoritative (doctrine 8); the join never authors world items.
// Change-detected: a settled world has stable per-item content+pos, so a stream of zero
// traffic, with a slow periodic safety resend. Rides the RELIABLE channel (doctrine 16).

// One ground item in a world snapshot. netId is the host-assigned key (NOT an engine
// hand). state is reserved bit-flags (0 = loose on-ground; future: claimed/pile/corpse).
struct WorldItemEntry {
    u32  netId;        // host-assigned synthetic id (the cross-client key)
    char stringID[48]; // template identity (longer ids truncated; name fallback tolerated)
    u32  itemType;     // GameData::type category (for the template lookup)
    u16  quantity;     // stack size
    u16  quality;      // quality*100 (0 if not applicable)
    f32  x;            // world position
    f32  y;
    f32  z;
    u8   state;        // reserved flags (0 = on-ground loose)
};

// A world-item snapshot is: [WorldItemSnapshotHeader][WorldItemEntry * count]. It is a
// PARTIAL stream (only items in the interest sphere that are new/changed since last send),
// NOT a full-world dump; culls travel separately via PKT_WORLD_ITEM_REMOVE. count == 0 is
// legal (a keep-alive) but normally omitted.
struct WorldItemSnapshotHeader {
    u8  type;    // = PKT_WORLD_ITEM
    u32 ownerId; // authoritative sender (the host)
    u8  count;   // number of WorldItemEntry that follow
};

// A world-item cull is: [WorldItemRemoveHeader][u32 netId * count]. Sent when a tracked
// ground item leaves the world / interest sphere (picked up, despawned, out of range), so
// the join destroys its matching proxy. Reliable so a despawn is never missed.
struct WorldItemRemoveHeader {
    u8  type;    // = PKT_WORLD_ITEM_REMOVE
    u32 ownerId; // authoritative sender (the host)
    u8  count;   // number of u32 netIds that follow
};

// 16 * sizeof(WorldItemEntry)=1168 + header(6) stays under a 1400 B datagram.
const unsigned int WORLD_ITEMS_MAX = 16;

// ---- Phase W2: conservation DROP intent ------------------------------------
// A WEAPON cannot be rebuilt on a peer (RootObjectFactory::createItem returns null for all
// weapons), so the W1 "stream a template, spawn a proxy" path can never show a dropped
// weapon on the other client. The conservation model fixes this WITHOUT creating anything:
// both clients already own the weapon (shared save), so when one client drops it, the OWNER
// of that character authors a reliable DROP intent and EACH client relocates its OWN real
// copy of the weapon from the character's bag to the ground (Inventory::dropItem). No
// fabrication, no destruction - the object is conserved and merely moved. Bidirectional:
// whichever client OWNS the dropping character authors the intent (Doctrine 8 partition),
// and the non-owning peer relocates its copy (so it never echoes its own drop back).
//
// A drop is a single fixed-size POD (like EventPacket), sent once on the RELIABLE channel.
// dropId is a per-sender monotonic id (idempotency + future PICKUP correlation). The owner
// hand identifies WHOSE bag the weapon left; the item identity locates the peer's matching
// copy; x/y/z is the ground position to mirror.
struct WorldDropPacket {
    u8  type;        // = PKT_WORLD_DROP
    u32 ownerId;     // network player id of the sender (the dropping character's owner)
    u32 dropId;      // monotonic per-sender (idempotency / pickup correlation)
    // owning character hand (whose inventory the weapon left)
    u32 oType;
    u32 oContainer;
    u32 oContainerSerial;
    u32 oIndex;
    u32 oSerial;
    // item identity (the peer finds its own matching copy by these)
    char stringID[48];
    u32  itemType;   // GameData::type (WEAPON for now; generalizes later)
    u16  quality;    // quality*100 (0 if n/a)
    char manufacturer[48];
    char material[48];
    // mirrored ground position
    f32 x;
    f32 y;
    f32 z;
};

// ---- Phase W3: conservation PICKUP intent ----------------------------------
// The mirror of PKT_WORLD_DROP. When a character picks a dropped weapon back up, the OWNER
// of that character authors a reliable PICKUP intent, and each peer relocates the REAL
// ground copy it tracked (from the drop) back into that character's bag (world -> bag) -
// again WITHOUT fabricating anything. Because getObjectsWithinSphere can't find dropped
// items in towns, the peer does NOT re-query the ground; it re-homes the exact Item* handle
// it remembered when the weapon was dropped (its own local copy). pickupId is monotonic per
// sender for idempotency. The target hand is the character that gained the weapon.
struct WorldPickupPacket {
    u8  type;        // = PKT_WORLD_PICKUP
    u32 ownerId;     // network player id of the sender (the picking character's owner)
    u32 pickupId;    // monotonic per-sender (idempotency)
    // target character hand (whose bag the weapon entered)
    u32 oType;
    u32 oContainer;
    u32 oContainerSerial;
    u32 oIndex;
    u32 oSerial;
    // item identity (selects which tracked ground copy the peer re-homes)
    char stringID[48];
    u32  itemType;   // GameData::type (WEAPON for now)
    u16  quality;    // quality*100 (0 if n/a)
};

// Reserved netId meaning "no/invalid world item".
const u32 WORLD_ITEM_NETID_NONE = 0u;

#pragma pack(pop)

// Returns the packet type tag (first byte) of a received buffer, or 0 if empty.
inline u8 packetType(const void* data, unsigned int len) {
    if (data == 0 || len < 1) return 0;
    return *reinterpret_cast<const u8*>(data);
}

// Safe typed read: returns true and fills out if the buffer is large enough.
template <typename T>
inline bool readPacket(const void* data, unsigned int len, T* out) {
    if (data == 0 || out == 0 || len < sizeof(T)) return false;
    memcpy(out, data, sizeof(T));
    return true;
}

} // namespace coop

#endif // KENSHICOOP_WIRE_H
