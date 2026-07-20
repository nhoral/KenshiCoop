// KenshiCoop unified wire protocol (clean rebuild, v1).
//
// Compiled by the VS2010 (v100) plugin only. Keep it plain C++03: no <cstdint>
// reliance, no constexpr, no scoped enums, no STL on the wire. Wire format is
// packed, little-endian; x86-64 is little-endian on both ends so we send the
// struct bytes directly.

#ifndef KENSHICOOP_WIRE_H
#define KENSHICOOP_WIRE_H

#include <string.h> // memcpy

namespace coop {

typedef unsigned char  u8;
typedef signed char    i8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef float          f32;
typedef double         f64;

// Protocol version. The full version-by-version history (what each bump added
// and why) lives in resources/PROTOCOL_HISTORY.md - keep it there, not here, so
// this header stays a definition file. When you bump PROTOCOL_VERSION, add the
// matching entry at the bottom of that doc. The version is checked at handshake
// and a mismatch is rejected (no back-compat).
const u16 PROTOCOL_VERSION = 44;

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
    PKT_WORLD_PICKUP     = 10,// RELIABLE conservation pickup intent (Phase W3); WorldPickupPacket
    PKT_TIME_PING        = 11,// UNRELIABLE wall-clock sync probe (join -> host); TimePingPacket
    PKT_TIME_PONG        = 12,// UNRELIABLE wall-clock sync echo (host -> join); TimePongPacket
    PKT_MEDICAL          = 13,// RELIABLE owner-authoritative vitals snapshot; MedicalPacket
    PKT_TREATMENT        = 14,// RELIABLE first-aid-on-a-driven-copy delta; TreatmentPacket
    PKT_SPEED_REQ        = 15,// RELIABLE game-speed REQUEST (join -> host); SpeedPacket
    PKT_SPEED_SET        = 16,// RELIABLE arbitrated effective speed (host -> join); SpeedPacket
    PKT_STATS            = 17,// RELIABLE owner-authoritative CharStats snapshot; StatsPacket
    PKT_STEALTH          = 18,// UNRELIABLE detection-map snapshot (host -> owner); StealthPacket
    PKT_SPAWN_REQ        = 19,// RELIABLE unresolved-hand query (join -> host); SpawnReqPacket
    PKT_SPAWN_INFO       = 20,// RELIABLE runtime-spawn description (host -> join); SpawnInfoPacket
    PKT_MONEY            = 21,// RELIABLE owner-authoritative tab wallet (protocol 22); MoneyPacket
    PKT_FACTION          = 22,// RELIABLE player-faction relation row (protocol 24); FactionPacket
    PKT_TIME             = 23,// RELIABLE host-authoritative game clock (protocol 25); TimePacket
    PKT_DOOR             = 24,// RELIABLE baked-door open/lock state row (protocol 26); DoorPacket
    PKT_BUILD_PLACE      = 25,// RELIABLE placed-building describe/mint (protocol 27); BuildPlacePacket
    PKT_BUILD_STATE      = 26,// RELIABLE placer-authoritative construction progress (protocol 27); BuildStatePacket
    PKT_BUILD_DOOR       = 27,// RELIABLE placed-building door row, translated key (protocol 28); BuildDoorPacket
    PKT_BUILD_REMOVE     = 28,// RELIABLE placer-authoritative building removal (protocol 28); BuildRemovePacket
    PKT_SAVE_REQ         = 29,// RELIABLE join save request (join -> host, protocol 31); SaveReqPacket
    PKT_SAVE_BEGIN       = 30,// RELIABLE save-transfer announce (host -> join, protocol 31); SaveBeginPacket
    PKT_SAVE_FILE        = 31,// RELIABLE save-file chunk (host -> join, protocol 31); SaveFileHeader + path + payload
    PKT_SAVE_DONE        = 32,// RELIABLE save-transfer CRC table (host -> join, protocol 31); SaveDoneHeader + u32*count
    PKT_SAVE_ACK         = 33,// RELIABLE commit acknowledgement (join -> host, protocol 31); SaveAckPacket
    PKT_LOAD_GO          = 34,// RELIABLE coordinated-load order (host -> join, protocol 32); LoadGoPacket
    PKT_LOAD_REQ         = 35,// RELIABLE join load request (join -> host, protocol 32); LoadReqPacket
    PKT_LOAD_NACK        = 36,// RELIABLE join copy missing/diverged (join -> host, protocol 32); LoadNackPacket
    PKT_PROD             = 37,// RELIABLE host-authoritative machine state row (protocol 33); ProdPacket
    PKT_NPC_CENSUS       = 38,// RELIABLE wide-radius NPC existence list (protocol 36); NpcCensusHeader
    PKT_INV_XFER         = 39,// RELIABLE cross-owner transfer intent (protocol 37); InvXferPacket
    PKT_RESEARCH         = 40,// RELIABLE host-authoritative known-research row (protocol 38); ResearchPacket
    PKT_CAM_HINT         = 41 // UNRELIABLE join camera center hint (protocol 43, join -> host); CamHintPacket
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
    EVT_REVIVE   = 3, // subject stood back up (down -> upright edge)
    EVT_AMPUTATE = 4, // subject lost a limb (LimbState -> STUMP edge); arg = RobotLimbs::Limb
    EVT_CRUSH    = 5, // subject's limb was crushed (LimbState -> CRUSHED edge); arg = limb
    // Carried-body sync (protocol 18). subject = the CARRIED body, actor = the
    // CARRIER (both resolve locally on each machine; the receiver performs the
    // same pickup/drop between its LOCAL pair). arg: 0 for pickup; for drop,
    // 1 = ragdoll the body on release (the normal ground drop), 0 = gentle.
    EVT_PICKUP_BODY = 6, // carrier lifted the subject onto its shoulder
    EVT_DROP_BODY   = 7, // carrier released the subject
    // Furniture occupancy (protocol 19). subject = the OCCUPANT, actor slots =
    // the FURNITURE's save-stable hand (a building, not a character - both
    // clients loaded the same save, so it resolves locally on each machine).
    // arg: 1 = bed, 2 = prison cage, 3 = chained/pole (protocol 41). For a
    // chain the actor slots carry the OWNER's hand (Character::slaveOwner)
    // instead of a furniture building: the receiver reproduces it with
    // setChainedMode(occupant, on, owner) and the pole position rides the
    // continuous transform stream (no rigid fixture attach needed).
    EVT_ENTER_FURNITURE = 8, // occupant was placed in / climbed into the furniture
    EVT_EXIT_FURNITURE  = 9, // occupant left / was removed from the furniture
    // Recruitment sync (protocol 23). subject = the recruited body's OLD hand
    // (its identity BEFORE PlayerInterface::recruit re-containered it), actor =
    // its NEW hand (the key the recruiter streams it under from now on). The
    // receiver re-keys its local copy of the old hand to the new key (no
    // duplicate body); if the old hand doesn't resolve there (runtime-born
    // subject) the bidirectional describe/mint channel covers it instead.
    EVT_RECRUIT = 10,
    // Squad management sync (protocol 35). Same shape as EVT_RECRUIT:
    // subject = the moved body's OLD hand, actor = its NEW hand after the
    // squad-tab move re-containered it (squad_probe: every move - UI drag,
    // separate-into-new-squad, setFaction move-back - mints a FRESH hand;
    // index/serial do not survive). An all-zero actor = the body LEFT the
    // roster (dismissal). The receiver shares the EVT_RECRUIT re-key path
    // and pins the new hand as peer-owned (rank reshuffle cannot flip it).
    EVT_SQUAD_MOVE = 11
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

// ObjectHand - the canonical identity of a Kenshi engine object (the game's
// `hand` 5-tuple: an itemType + a container ref + a per-object index/serial),
// stable across machines that loaded the same save. This is the ONE typed
// representation the plugin should pass around; it exists to retire the "dual
// hand[5] layout" footgun, where the same five values were packed into a raw
// u32[5] in TWO different orders a call site had to remember by position:
//   * OBJECT order   = {type, container, containerSerial, index, serial}
//       the dominant "readObjectHand layout" - EntityState's hand fields,
//       CombatRead::target[5], and every subj/c/m/itemHand[5] use it.
//   * CHAR-KEY order = {index, serial, type, container, containerSerial}
//       the older "SCENARIO hand-key" layout produced by readHand().
// The engine's own 5-arg hand ctor takes (index, serial, type, container,
// containerSerial) = CHAR-KEY order; the adapter (engine::resolveChar /
// resolveObject) owns that mapping in one place so no caller repeats it.
// Field NAMES (not positions) are the contract: fill/read them by name and the
// order stops mattering. The from*/to* helpers convert to/from the two legacy
// raw-array orders at the few boundaries that still speak u32[5] (the wire and
// not-yet-migrated call sites). Plain C++03 POD - safe to pass by value/copy.
struct ObjectHand {
    u32 type;
    u32 container;
    u32 containerSerial;
    u32 index;
    u32 serial;

    // OBJECT order: a[0..4] = {type, container, containerSerial, index, serial}.
    static ObjectHand fromObjOrder(const u32 a[5]) {
        ObjectHand h;
        h.type = a[0]; h.container = a[1]; h.containerSerial = a[2];
        h.index = a[3]; h.serial = a[4];
        return h;
    }
    void toObjOrder(u32 a[5]) const {
        a[0] = type; a[1] = container; a[2] = containerSerial;
        a[3] = index; a[4] = serial;
    }
    // CHAR-KEY order: a[0..4] = {index, serial, type, container, containerSerial}.
    static ObjectHand fromCharKey(const u32 a[5]) {
        ObjectHand h;
        h.index = a[0]; h.serial = a[1]; h.type = a[2];
        h.container = a[3]; h.containerSerial = a[4];
        return h;
    }
    void toCharKey(u32 a[5]) const {
        a[0] = index; a[1] = serial; a[2] = type;
        a[3] = container; a[4] = containerSerial;
    }
    // A resolvable hand has a non-zero index/serial pair (the engine's null
    // handle is all-zero; type/container alone never identify a live object).
    bool resolvable() const { return index != 0 || serial != 0; }
    bool equals(const ObjectHand& o) const {
        return type == o.type && container == o.container &&
               containerSerial == o.containerSerial &&
               index == o.index && serial == o.serial;
    }
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

// Combat STANCE split (protocol 15). Kenshi's AttackSlotManager grants only a
// couple of attackers an active slot; the rest of an engaged crowd WAITS
// (CIRCLE_MENACINGLY / WAIT_MENACINGLY / HESITATE sword states). A waiting
// combatant is a stance, not a failed attack: the join must keep its copy
// holding the attack goal in the menace ring, NOT re-issue the focused attack
// on a timer (each re-issue clearGoals-resets the local AI, which wanders the
// body until the drift snap teleports it - the exact artifact this fixes).
const u16 TASK_COMBAT_WAIT = 0xFE01u;
inline bool taskIsCombat(u16 t)     { return t == TASK_COMBAT_MELEE || t == TASK_COMBAT_WAIT; }
inline bool taskIsCombatWait(u16 t) { return t == TASK_COMBAT_WAIT; }

// Carried-body sync (protocol 18): synthetic task meaning "this body is
// CARRYING the subject hand on its shoulder". Set by the carrier's owner
// whenever Character::isCarryingSomething holds; the join uses it as the
// SELF-HEAL for a lost EVT_PICKUP_BODY (a driven carrier reporting
// TASK_CARRY_BODY whose local copy is not carrying gets a throttled local
// pickup). Priority sits below combat, above rest poses.
const u16 TASK_CARRY_BODY = 0xFE02u;
inline bool taskIsCarry(u16 t) { return t == TASK_CARRY_BODY; }

// bodyState bit-flags. A body is "down" (on the ground, not upright) when any of
// BODY_DOWN / BODY_RAGDOLL / BODY_DEAD is set; BODY_CRAWL is an upright-ish stealth/
// crawl posture kept separate. Read from Character::isDown/isRagdoll/isDead/
// isStealthModeOrCrawling on the host.
const u16 BODY_DOWN    = 1 << 0; // Character::isDown()  (KO'd / unconscious / collapsed)
const u16 BODY_RAGDOLL = 1 << 1; // Character::isRagdoll()
const u16 BODY_DEAD    = 1 << 2; // Character::isDead()
const u16 BODY_CRAWL   = 1 << 3; // Character::isStealthModeOrCrawling()
// Carried-body sync (protocol 18): Character::isBeingCarried(). A carried body
// still reads down/ragdoll (it is KO'd, in carry-mode ragdoll), but it must be
// EXEMPT from the down enforcement (knockDown/holdDown/co-locate snap) and all
// position driving - its transform is owned by the local shoulder attach.
const u16 BODY_CARRIED = 1 << 4;
// Furniture occupancy (protocol 19): Character::inSomething (IN_BED/IN_PRISON).
// An occupant may also read down (an unconscious body placed in a bed/cage),
// but like BODY_CARRIED it must be EXEMPT from the down enforcement and all
// position driving - the furniture attach owns its transform.
const u16 BODY_IN_BED  = 1 << 5;
const u16 BODY_IN_CAGE = 1 << 6;
// Stealth sync (protocol 20): Character::stealthMode EXACTLY (the mode bool the
// player toggles). Distinct from BODY_CRAWL (isStealthModeOrCrawling), which
// also covers injured crawl - a crawler must never get setStealthMode applied.
const u16 BODY_SNEAK   = 1 << 7;
// Chained/pole prisoner (protocol 41): Character::isChained. A captive on a
// prisoner POLE is shackled via the chained/slave system, NOT the cage's
// inSomething==IN_PRISON. Like the occupancy bits it may also read down (a
// KO'd body just placed on the pole) but is EXEMPT from the down enforcement
// and all position driving - the chained attach + streamed transform own it.
// Rides the furniture pipeline as kind=3 (the actor hand carries the OWNER).
const u16 BODY_CHAINED = 1 << 8;

// True if the body should be treated as lying down (suppress walk-drive / parking).
// Deliberately ignores BODY_CARRIED (and the occupancy bits): the receiver checks
// bodyIsCarried/bodyInFurniture FIRST and skips the down path entirely for them.
inline bool bodyIsDown(u16 s)    { return (s & (BODY_DOWN | BODY_RAGDOLL | BODY_DEAD)) != 0; }
inline bool bodyIsCarried(u16 s) { return (s & BODY_CARRIED) != 0; }
inline bool bodyInFurniture(u16 s) { return (s & (BODY_IN_BED | BODY_IN_CAGE | BODY_CHAINED)) != 0; }
inline bool bodyChained(u16 s)   { return (s & BODY_CHAINED) != 0; }
inline bool bodySneaking(u16 s)  { return (s & BODY_SNEAK) != 0; }

// An entity batch is: [EntityBatchHeader][EntityState * count]. ownerId tags the
// streaming peer; the receiver attributes every contained hand to that owner so
// it applies the right authority rule. Capped so one batch fits a datagram.
// sendMs (v35) is the sender's millisecond clock at capture time: the receiver
// indexes its interp ring on (sendMs + estimated clock offset) instead of the
// arrival time, so path jitter no longer smears into the snapshot spacing.
struct EntityBatchHeader {
    u8  type;    // = PKT_ENTITY_BATCH
    u32 ownerId; // network player id of the batch's owner
    u32 sendMs;  // sender's monotonic ms clock when the batch was captured
    u32 epoch;   // sender session epoch (v44): the receiver drops a batch whose
                 // epoch is older than the newest it accepted from this owner,
                 // so a network-delayed batch from the session that existed
                 // BEFORE a coordinated world reload (it describes a world that
                 // no longer exists) cannot mint a phantom proxy in the new one.
    u8  count;   // number of EntityState that follow
};

// 17 * sizeof(EntityState) + header stays comfortably under a 1400 B datagram
// (v35: one entity of headroom traded for the sendMs stamp; v44: +epoch). This
// is the HARD receive-side bound and the raw-UDP sender chunk size.
const unsigned int ENTITY_BATCH_MAX = 17;

// Steam sender chunk size: the Steam P2P transport clamps ENet's MTU to
// 1200 B, and ENet sends an oversized UNRELIABLE packet as RELIABLE
// fragments - retransmits and ordering stalls on the 20 Hz motion stream,
// on exactly the transport real sessions use (architecture review
// 2026-07-10). 14 * 79 B + 14 B header = 1120 B, inside 1200 with ENet's
// per-packet overhead. Sender-side only - the receiver validates by
// len >= need against the header count, so mixed caps interoperate.
const unsigned int ENTITY_BATCH_MAX_STEAM = 14;

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
    // Phase 6b (protocol 42): 1 when this item is a LockedArmour (shackle) with a
    // live lock (Item::isLockedArmour()->lock != null). The owner is authoritative
    // for the lock state; a peer's local lockpick must not desync it (see the
    // non-owner unlock guard in ReplicatorDrive). Cage occupancy (IN_PRISON) masks
    // the chained furniture kind, so this rides the inventory snapshot as an
    // occupancy-independent lock signal. lockReserved keeps `section` 2-byte aligned.
    u8   locked;
    u8   lockReserved; // reserved (0)
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
// key identifies WHOSE inventory this is (a storage building, a character, a chest).
// count == 0 is a valid "container is now empty" snapshot.
struct InvSnapshotHeader {
    u8  type;    // = PKT_INV_SNAPSHOT
    u32 ownerId; // network player id of the authoritative sender
    // Protocol 34: container identity kind. 0 = the c* fields are the raw
    // (save-stable) hand - characters, baked chests, the previous implicit
    // behaviour. 1 = the c* fields are the protocol-27 PLACER key of a
    // session-placed building: the sender translated its local hand through
    // its build maps (own placement = own hand; a minted proxy = the reverse
    // map) and the receiver resolves through its own (peer key -> minted
    // local hand; own key -> own hand) - the PKT_PROD identity approach.
    u8  keyKind;
    // container key (whose inventory; hand or placer key per keyKind)
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
    u32 ownerId; // authoring sender (W1 bidir: netId spaces are PER-SENDER; the
                 // receiver keys proxies by (ownerId, netId))
    u8  count;   // number of WorldItemEntry that follow
};

// A world-item cull is: [WorldItemRemoveHeader][u32 netId * count]. Sent when a tracked
// ground item leaves the world / interest sphere (picked up, despawned, out of range), so
// the join destroys its matching proxy. Reliable so a despawn is never missed.
struct WorldItemRemoveHeader {
    u8  type;    // = PKT_WORLD_ITEM_REMOVE
    u32 ownerId; // authoring sender (culls are scoped to this owner's netId space)
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
    // EXACT ground instance being re-homed: the identity of the originating DROP that both
    // clients tracked it under. When refDropId != 0 the peer re-homes precisely that (owner,
    // id)-keyed copy (disambiguating two same-sid ground weapons); refDropId == 0 means the
    // picker couldn't correlate an instance and the peer falls back to its oldest same-sid copy.
    u32 refDropOwnerId;
    u32 refDropId;
};

// ---- Protocol 37: cross-owner TRANSFER intent --------------------------------
// A direct UI drag between two squads mutates a PEER-authored container - the one
// write the single-writer container snapshots cannot represent (the owner would
// reconcile it away: dupe on take, wipe on give, weapon-vanish for gear; see the
// PROTOCOL_VERSION 36 note). The dragging client detects the COMPLETED move by
// diffing its tracked containers against their last-known baselines, pairs the
// loss with the matching gain, and authors this intent once, reliably. The
// receiver relocates the REAL Item* between its own copies of the two containers
// (the W2/W3 conservation doctrine applied to bags: no fabrication, no
// destruction - so gear survives). xferId is per-sender monotonic (idempotence).
// Container hands use the raw save-stable layout (characters and baked chests;
// the same keys the PKT_INV_SNAPSHOT channel streams with keyKind 0).
struct InvXferPacket {
    u8  type;        // = PKT_INV_XFER
    u32 ownerId;     // network player id of the sender (the client that saw the drag)
    u32 xferId;      // monotonic per-sender (idempotency)
    // SOURCE container hand (the item left here)
    u32 sType;
    u32 sContainer;
    u32 sContainerSerial;
    u32 sIndex;
    u32 sSerial;
    // DESTINATION container hand (the item landed here)
    u32 dType;
    u32 dContainer;
    u32 dContainerSerial;
    u32 dIndex;
    u32 dSerial;
    // item identity (the receiver locates its own copy in src by these)
    char stringID[48];
    u32  itemType;   // GameData::type category
    u16  quantity;   // units moved
    u16  quality;    // quality*100 of the moved stack (advisory)
    char manufacturer[48];
    char material[48];
};

// Reserved netId meaning "no/invalid world item".
const u32 WORLD_ITEM_NETID_NONE = 0u;

// ---- Wall-clock time sync (remote-play prerequisite) ------------------------
// The validation oracles time-align host/join log samples by their "[HH:MM:SS.mmm]"
// wall-clock stamps - which only works when both clients share a machine. This
// channel measures the wall-clock OFFSET between the two machines, NTP-style:
// the JOIN sends a TIME_PING carrying its wall clock t0 (ms since local midnight,
// coop::wallClockMs()); the HOST immediately echoes a TIME_PONG with t0 plus its
// own wall clock th; the join receives it at t1 and computes
//     rtt    = t1 - t0
//     offset = th + rtt/2 - t1      (host wall clock minus join wall clock)
// keeping the minimum-RTT sample (least queueing noise). The join logs
// "CLOCKSYNC offset=<ms> rtt=<ms> n=<samples>" lines which Get-ScenarioSeries
// applies to normalize its log stamps into the host clock frame. Rides the
// UNRELIABLE channel: a retransmitted (reliable) probe would carry a stale t0
// and poison the RTT estimate; a lost probe just means one missing sample.
// ---- Phase 2 (player combat + medical): owner-authoritative vitals sync -----
// Kenshi's medical model (blood, bleed rate, per-limb flesh + bandaging) is
// entirely LOCAL (spikes 21-23): a driven copy's vitals never move unless we
// move them. For PLAYER-SQUAD characters - the bodies players actually care
// about healing - each client streams its OWNED members' medical model to the
// peer, which writes the fields straight onto its driven copy (killSubject/
// woundSubject direct-write precedent). Change-gated on a quantized fingerprint
// + throttled, with a periodic safety resend, on the RELIABLE channel (a change
// must not be lost; steady state is silent). Unconscious/dead ride ALONG for
// the record but the KO/death/revive EVENT channel remains the transition
// authority (the latches in applyEvents). Protocol 16: the same packet also
// carries combat-scoped WORLD-NPC vitals (host-authoritative) so a battered
// NPC renders true health on the join instead of pristine.
const u8 MED_UNCONSCIOUS = 1 << 0;
const u8 MED_DEAD        = 1 << 1;

// Protocol 16: one anatomy part's full damage model. Parts are keyed by their
// ANATOMY INDEX (MedicalSystem::anatomy order) - both clients load the same
// save/race data, so the ordering is deterministic (the same doctrine that keys
// fixtures by hand). partType/side ride along as a sanity check: the receiver
// verifies its local part at that index agrees before writing.
struct MedPartEntry {
    u8  used;      // 1 = this slot carries a part, 0 = empty tail slot
    u8  partType;  // HealthPartStatus::PartType (TORSO/LEG/ARM/HEAD)
    u8  side;      // LeftRight enum value
    f32 flesh;     // cut HP    (-1 = unreadable; never written)
    f32 fleshStun; // stun HP   (-1 = unreadable; never written)
    f32 bandaging; // bandage level (-1 = unreadable)
    f32 juryRig;   // robotics jury-rig level (-1 = unreadable)
};

// Max anatomy slots on the wire. Humans carry 7 parts (head, chest, stomach +
// 4 limbs); 12 leaves headroom for modded/animal anatomies without a resize.
const u8 MED_PARTS_MAX = 12;

// LimbState wire values match kenshi's enum; 0xFF = unknown/unreadable on the
// owner (never applied).
const u8 LIMB_WIRE_ORIGINAL = 0;
const u8 LIMB_WIRE_STUMP    = 1;
const u8 LIMB_WIRE_REPLACED = 2;
const u8 LIMB_WIRE_CRUSHED  = 3;
const u8 LIMB_STATE_UNKNOWN = 0xFF;
// Robotic replacement template stringID capacity (matches InvItemEntry).
const u8 MED_SID_LEN = 48;

struct MedicalPacket {
    u8  type;    // = PKT_MEDICAL
    u32 ownerId; // network player id of the sender (the subject's owner)
    // subject hand (whose vitals these are)
    u32 sType;
    u32 sContainer;
    u32 sContainerSerial;
    u32 sIndex;
    u32 sSerial;
    f32 blood;
    f32 bleedRate;
    // Protocol 29: hunger scalars (MedicalSystem::hunger/fed, engine scale
    // ~0..3). -1 = not carried (hungerSync off on the sender) - the receiver
    // leaves its local value untouched. dazedOrAlert deliberately NOT on the
    // wire (unconfirmed semantics; probe-diagnostics only).
    f32 hunger;
    f32 fed;
    u8  flags;  // MED_* bits (advisory; events own the transitions)
    u8  nParts; // filled MedPartEntry slots (anatomy order, from index 0)
    MedPartEntry parts[MED_PARTS_MAX];
    // Limb loss (protocol 16): LimbState per RobotLimbs::Limb order
    // (LEFT_ARM, RIGHT_ARM, LEFT_LEG, RIGHT_LEG). Self-heal for the reliable
    // EVT_AMPUTATE/EVT_CRUSH transitions (doctrine 16: state + events).
    u8  limbState[4];
    // Robotic replacement template stringID per limb (empty unless the
    // matching limbState == LIMB_REPLACED). Lets the peer fabricate + fit
    // the same prosthetic (Phase D).
    char limbSid[4][48];
};

// First aid administered ON a driven copy, forwarded to the body's OWNER.
// The healer's machine detects its local bandaging rising ABOVE the last
// RECEIVED medical snapshot for that copy (a stream overwrite can only lower
// it back, so the comparison is race-free) and sends the resulting per-PART
// bandage LEVELS - not the per-frame applyFirstAid call stream (hot path).
// The owner applies them raise-only (max(local, received)), which makes the
// packet idempotent; the vitals stream then mirrors the healed state back to
// everyone. treatId is per-sender monotonic for log correlation. Protocol 16:
// levels are keyed by ANATOMY INDEX (all parts, not just the 4 limbs).
struct TreatmentPacket {
    u8  type;    // = PKT_TREATMENT
    u32 ownerId; // network player id of the sender (the HEALER's machine)
    u32 treatId; // monotonic per-sender (log correlation; apply is idempotent)
    // subject hand (whose body was bandaged - owned by the RECEIVER)
    u32 sType;
    u32 sContainer;
    u32 sContainerSerial;
    u32 sIndex;
    u32 sSerial;
    f32 partBand[MED_PARTS_MAX]; // bandage level per anatomy part (-1 = not raised)
};

// Consensus game speed (pause/1x/2x/3x). As PKT_SPEED_REQ it carries one
// client's REQUESTED speed (what its player last clicked) plus an IN_COMBAT
// bit for that client's own squad; as PKT_SPEED_SET it carries the host's
// arbitrated EFFECTIVE speed both engines must apply. Pause travels as
// speed 0 (so min() gives "either can pause, both must raise"); the PAUSED
// flag is kept explicit for log clarity. seq is per-sender monotonic so a
// late retransmit never rolls back a newer decision.
enum SpeedFlags {
    SPEED_PAUSED    = 1,
    SPEED_IN_COMBAT = 2
};
struct SpeedPacket {
    u8  type;    // = PKT_SPEED_REQ or PKT_SPEED_SET
    u32 ownerId; // network player id of the sender
    u32 seq;     // monotonic per-sender (stale-packet guard)
    f32 speed;   // requested/effective multiplier (0 = paused)
    u8  flags;   // SPEED_* bits
};

// ---- Protocol 17: owner-authoritative character stats -----------------------
// CharStats (attributes, weapon skills, craft skills, xp) is entirely LOCAL,
// like the medical model - but unlike medical it also STEERS authoritative
// outcomes on the peer: a join-owned character fighting a world NPC resolves
// the real damage on the HOST, using the host's copy of that character's
// stats. So each client streams its OWNED player-squad members' stats
// (change-gated, ~1 Hz floor, reliable) and the peer writes them onto its
// driven copy + recalculates derived values. The stream is also the self-heal
// for junk XP a driven copy's cosmetic fights generate locally (the damage
// guard blocks damage, not XP events). World NPCs are excluded: their
// authoritative fights run on the host with the host's own (correct) stats.
//
// Slots are indexed by kenshi's StatsEnumerated (STAT_STRENGTH=1 ..
// STAT_SMITHING_BOW=38, read via CharStats::getStatRef); slot 0 (STAT_NONE)
// is unused. -1 = unreadable on the owner, never written by the receiver
// (the medical convention).
const u8 STATS_SLOT_MAX = 40; // headroom above STAT_END (39)

struct StatsPacket {
    u8  type;    // = PKT_STATS
    u32 ownerId; // network player id of the sender (the subject's owner)
    // subject hand (whose stats these are)
    u32 sType;
    u32 sContainer;
    u32 sContainerSerial;
    u32 sIndex;
    u32 sSerial;
    u8  nStats;  // filled slots (from index 1; receiver clamps to its own max)
    f32 stats[STATS_SLOT_MAX]; // by StatsEnumerated index (-1 = unreadable)
    f32 xp;                    // CharStats::xp (-1 = unreadable)
    f32 freeAttributePoints;   // CharStats::freeAttributePoints (int on wire as f32; -1 = unreadable)
};

// ---- Protocol 22: per-tab wallet snapshot -----------------------------------
// Owner-authoritative money for ONE player squad tab, keyed by the tab's RANK
// among the distinct sorted member containers - the same cross-client-stable
// tab identity the ownership partition uses (a hand key would also work, but
// rank is what both sides already agree on for "whose tab is whose"). Change-
// gated with a floor + safety resend (the PKT_STATS pacing); the receiver
// writes the value via Ownerships::setMoney onto the platoon of that rank's
// tab leader. money is signed on the wire because the engine field is an int.
struct MoneyPacket {
    u8  type;    // = PKT_MONEY
    u32 ownerId; // network player id of the sender (the tab's owner)
    u32 tabRank; // squad-tab rank (0 = host-owned tab, 1 = join-owned, ...)
    int money;   // Ownerships::money for that tab's platoon
};

// ---- Protocol 24: player-faction relation row --------------------------------
// ONE relation row between the shared player faction and a world faction,
// keyed by the faction's GameData stringID (cross-client stable -
// faction_probe run 132239; the same identity protocol 21 round-trips for
// proxy spawns). The probe showed the engine keeps the two per-side tables
// MIRRORED (player->them always equals them->player) and derives the
// enemy/ally flags from the value, so one float is the whole state; the
// receiver writes BOTH local rows via FactionRelations::setRelation. The
// channel is SYMMETRIC: each client streams rows its own table moved
// (change-gated vs a seeded baseline, ~1 Hz sample or immediate on a
// detoured affectRelations mutation) and applies whatever arrives; the
// receiver updates its baseline BEFORE writing, so an applied row is never
// re-detected as a local change (echo-free). seq is per-sender monotonic so
// a stale row never overwrites a newer one on either side.
struct FactionPacket {
    u8  type;      // = PKT_FACTION
    u32 ownerId;   // network player id of the sender
    u32 seq;       // per-sender monotonic (stale-row guard)
    char sid[48];  // faction GameData stringID ("" never sent)
    f32 relation;  // the row value (engine range approx. -100..100)
};

// ---- Protocol 25: host-authoritative game clock -------------------------------
// The host's absolute in-game clock (GameWorld::getTimeStamp_inGameHours, in
// total campaign hours - time_probe run 141509 proved it save-derived and
// advancing at exactly frameSpeedMult x the base rate). ~1 Hz on CH_RELIABLE.
// The join computes offset = hostHours - localHours and corrects by SLEWING:
// it quietly scales its local sim speed on top of the arbitrated consensus
// speed until the offset is inside tolerance (there is no engine setter for
// the clock base; a slew converges without one and never makes the clock
// jump or run backwards). At the default hour length (~109 real-seconds per
// game hour) 50 ms of wire latency is ~0.0005 game hours - ignorable, so no
// RTT compensation. seq is host-monotonic (stale-sample guard).
struct TimePacket {
    u8  type;      // = PKT_TIME
    u32 ownerId;   // network player id of the sender (the host)
    u32 seq;       // per-sender monotonic (stale-sample guard)
    f64 gameHours; // absolute in-game clock, total hours
};

// ---- Protocol 26: baked-door open/lock state ----------------------------------
// One door/gate state row, keyed by the door Building's save-stable hand (the
// furniture/bed identity precedent - door_probe run 160041 confirmed census
// intersection on the shared save). SYMMETRIC change-gated channel: each
// client samples doors near its interest centers ~1 Hz, streams rows whose
// (open, locked) moved vs a seeded per-hand baseline, and applies received
// rows through the engine's own openDoor/closeDoor/lockDoor/unlockDoor -
// updating the baseline BEFORE the write, so an applied row is never
// re-detected as a local change (echo-free). seq is per-sender monotonic so
// a stale row never overwrites a newer one. open is the collapsed DESTINATION
// state (OPENING counts as open, CLOSING as closed) so a door mid-swing never
// publishes a transient. A receiver that cannot resolve the hand skips the
// row silently (out-of-interest or a runtime-placed door - accepted edge).
struct DoorPacket {
    u8  type;      // = PKT_DOOR
    u32 ownerId;   // network player id of the sender
    u32 seq;       // per-sender monotonic (stale-row guard)
    // door hand [type, container, containerSerial, index, serial]
    u32 hand[5];
    u8  open;      // 1 = open/opening
    u8  locked;    // 1 = DoorLock engaged (only applied when the door has a lock)
};

// ---- Protocol 27: placed-building sync --------------------------------------
// A placed building is a RUNTIME object: its hand exists only in the placer's
// session (build_probe: minted-site hand intersection across clients is zero),
// so the wire key is the PLACER's local hand and the receiver keeps a
// key -> local-hand translation map, exactly the protocol-21 proxy precedent
// for structures. The PLACER is the authority for its building's construction
// progress (the describe/mint edge names the authority implicitly - whoever
// announced the key streams its state).
//
// PLACE announces one local placement (the UI commit detour on
// PreviewBuilding::placeFinalPreviewBuilding, or a programmatic scenario
// place). The receiver mints a local construction site with the same
// createBuilding factory the placer used (probe-proven to bypass the UI's
// town-placement verification, so a peer mint always lands where the placer's
// did) and never re-announces it (a factory mint does not pass through the
// placement detour - echo-free by construction). Re-announced keys (safety
// resends) are deduped by the translation map.
struct BuildPlacePacket {
    u8  type;      // = PKT_BUILD_PLACE
    u32 ownerId;   // network player id of the sender (the placer = the authority)
    u32 seq;       // per-sender monotonic (diagnostics/ordering)
    // the placed building's hand IN THE PLACER'S SESSION (the wire key)
    u32 key[5];
    char sid[48];  // building template GameData stringID
    f32 x;         // placement transform (the receiver mints here; the
    f32 y;         //  factory re-grounds vertically itself)
    f32 z;
    f32 yaw;       // radians
    u8  fromUi;    // 1 = real build-mode commit, 0 = programmatic (diagnostics)
};

// One construction-progress row for a building the SENDER placed (keyed by
// the sender's hand = the PLACE key). Change-gated ~1 Hz with a 10 s safety
// resend while incomplete; complete=1 latches (the engine self-completes at
// progress >= 1.0 through its own setter - scaffold off, navmesh updated).
// A receiver whose translation map lacks the key skips the row silently
// (mint refused or PLACE not yet applied - reliable ordering makes the
// latter transient).
struct BuildStatePacket {
    u8  type;      // = PKT_BUILD_STATE
    u32 ownerId;   // network player id of the sender (the placer)
    u32 seq;       // per-sender monotonic (stale-row guard)
    u32 key[5];    // the PLACER's hand for the building (translation-map key)
    f32 progress;  // ConstructionState::constructionProgress (0..1 while building)
    u8  complete;  // 1 = ConstructionState::isComplete (latched)
};

// ---- Protocol 28: placed-building doors + dismantle --------------------------
// A placed building's doors are runtime objects on BOTH clients (the placer's
// original and the peer's minted proxy each mint their own DoorStuff children),
// so no raw door hand ever crosses. bdoor_probe (run 195513) proved the
// translation identity: the factory mints doors in template order, so
// (PLACER's building hand, index in Building::doors) names the same physical
// door on both clients once resolved through the protocol-27 build maps.
// Symmetric change-gated rows, the protocol-26 door shape on the translated
// key: both clients sample their placed/minted buildings' doors ~1 Hz and
// stream rows whose (open, locked) moved vs the seeded baseline; the baseline
// updates BEFORE the apply write (echo-free); per-sender seq drops stale rows.
struct BuildDoorPacket {
    u8  type;      // = PKT_BUILD_DOOR
    u32 ownerId;   // network player id of the sender
    u32 seq;       // per-sender monotonic (stale-row guard)
    u32 bkey[5];   // the PLACER's hand for the owning building (map key)
    u8  doorIndex; // position in the building's ordered doors list
    u8  open;      // 1 = open/opening (collapsed destination state)
    u8  locked;    // 1 = DoorLock engaged (applied only when the door has one)
};

// Placer-authoritative removal of a session-placed building: the dismantle
// detour (UI path) or a programmatic destroy queues the edge; the receiver
// destroys its mapped proxy through the engine's own GameWorld::destroy and
// tombstones the translation entry (later rows for the key skip silently).
// Only buildings in the session's build maps ever stream removal - baked
// buildings are untouched by this channel.
struct BuildRemovePacket {
    u8  type;    // = PKT_BUILD_REMOVE
    u32 ownerId; // network player id of the sender (the placer)
    u32 seq;     // per-sender monotonic
    u32 key[5];  // the PLACER's hand for the removed building
};

// ---- Protocol 20: stealth detection-map snapshot ---------------------------
// The host's authoritative world computes WHO NOTICES a sneaking character
// (Character::whoSeesMeSneaking, filled by the engine's own vision checks -
// spike-proven to fire against driven copies). For a PEER-owned sneaker that
// map lives on the host's driven copy, but the indicators must render on the
// OWNER's screen - so the host streams the map back to the owner, who replays
// each entry between its LOCAL pair via notifyICanSeeYouSneaking. Continuous
// owner-directed FEEDBACK state: unreliable, change-gated + throttled, latest
// snapshot wins; an empty snapshot clears stale arrows (the engine ages
// entries out itself once notifies stop).
const u8 STEALTH_SEER_MAX = 16;

struct StealthSeerEntry {
    // seer hand (the local character who notices the sneaker)
    u32 nType;
    u32 nContainer;
    u32 nContainerSerial;
    u32 nIndex;
    u32 nSerial;
    u8  see;    // YesNoMaybe key: 0 = NO, 1 = YES, 2 = MAYBE
    f32 prog;   // WhoSeesMe::progressOfMaybe (raw engine progress, can exceed 1)
};

struct StealthPacket {
    u8  type;    // = PKT_STEALTH
    u32 ownerId; // network player id of the SENDER (the detection authority)
    // subject hand (the sneaker whose map this is - a member of the RECEIVER)
    u32 sType;
    u32 sContainer;
    u32 sContainerSerial;
    u32 sIndex;
    u32 sSerial;
    u8  unseen;  // Character::stealthUnseen (YesNoMaybe key) on the authority
    u8  nSeers;  // filled entries
    StealthSeerEntry seers[STEALTH_SEER_MAX];
};

// ---- Protocol 21: runtime-spawn proxy replication ---------------------------
// The join asks about a streamed hand it cannot resolve (a host RUNTIME spawn -
// roaming squad, dialog ambush - whose hand exists only in the host's session).
// Debounced per hand + retry-capped by the sender so the reliable channel stays
// quiet; the host caches replies so a retransmitted request costs one lookup.
struct SpawnReqPacket {
    u8  type;    // = PKT_SPAWN_REQ
    u32 ownerId; // network player id of the sender (the join)
    // the unresolvable streamed hand, verbatim from the entity batch
    u32 hType;
    u32 hContainer;
    u32 hContainerSerial;
    u32 hIndex;
    u32 hSerial;
};

// The host's description of the runtime spawn: enough for the join to mint a
// LOCAL proxy body (template + faction + transform). found=0 is the negative
// reply ("that hand doesn't resolve here either" - e.g. the NPC despawned
// between the request and the reply), which stops the join's retries.
// Appearance/equipment are approximated by the template (randomized gear);
// combat outcomes stay host-authoritative + damage-guarded, so this is
// cosmetic (accepted limitation).
struct SpawnInfoPacket {
    u8  type;    // = PKT_SPAWN_INFO
    u32 ownerId; // network player id of the sender (the host)
    // the requested hand, echoed verbatim (the join's proxy-map key)
    u32 hType;
    u32 hContainer;
    u32 hContainerSerial;
    u32 hIndex;
    u32 hSerial;
    char charSid[48]; // character template GameData stringID
    char facSid[48];  // faction GameData stringID ("" = unknown -> join fallback)
    f32 x;            // world transform at reply time
    f32 y;
    f32 z;
    f32 heading;      // radians (yaw)
    u8  found;        // 1 = resolved + described; 0 = negative (stop retrying)
    u8  dead;         // body was dead at reply time (join spawns + death-latches)
    // Host body's age (protocol 39). Animals derive body SCALE from age
    // (CharacterAnimal ageSizeMin/Max), so the join must mint with the host's
    // value or every proxy creature spawns full-grown ("giant goats", manual
    // session 2026-07-12). <= 0 = unreadable -> join uses its adult default.
    f32 age;
};

// ---- Protocol 31: coordinated save + session resume --------------------------
// The HOST's save is authoritative. Any local save on the HOST (menu save,
// quicksave, autosave, programmatic) triggers the coordinated flow: wait for
// folder quiescence, then stream the whole save folder to the join. A save
// initiated on the JOIN is suppressed locally (its engine never writes) and
// forwarded to the host as PKT_SAVE_REQ instead - one authoritative save,
// host-arbitrated. All five packets ride CH_RELIABLE (ENet fragments the
// ~4 KB chunks transparently; ordered-reliable means FILE chunks can never
// arrive before their BEGIN or after their DONE).

// Save-name capacity on the wire (matches the SaveEdge capture buffer).
const u8 SAVE_NAME_LEN = 48;
// One PKT_SAVE_FILE payload cap. 4 KB = ~4 ENet fragments at the 1200 B
// Steam MTU; small enough that pacing (chunks/pump) bounds burst bandwidth.
const u16 SAVE_CHUNK_MAX = 4096;
// Relative-path capacity inside a save folder (e.g. "platoon\\x.platoon").
const u16 SAVE_PATH_MAX = 260;

// Join -> host: "my player pressed save" (the join's local write was
// suppressed). The host runs its own saveGameAs(name) and the coordinated
// transfer follows. reqId is per-sender monotonic (log correlation).
struct SaveReqPacket {
    u8   type;    // = PKT_SAVE_REQ
    u32  ownerId; // network player id of the sender (the join)
    u32  reqId;   // monotonic per-sender
    char name[48]; // requested save name ('\0'-padded)
};

// Host -> join: the transfer is starting. xferId keys every FILE/DONE/ACK of
// this transfer (a per-host monotonic counter - a stale chunk from an
// aborted transfer is dropped by id mismatch). fileCount/totalBytes size the
// join's progress accounting and staging.
struct SaveBeginPacket {
    u8   type;      // = PKT_SAVE_BEGIN
    u32  ownerId;   // network player id of the sender (the host)
    u32  xferId;    // per-host monotonic transfer id
    char name[48];  // save name (the join stages save/<name>__incoming/)
    u16  fileCount; // files that will follow
    unsigned __int64 totalBytes; // sum of file sizes (progress + sanity)
};

// Host -> join: one chunk of one file. Variable length:
//   [SaveFileHeader][char path[pathLen]][u8 payload[dataLen]]
// path is the file's save-folder-relative path ('\\'-separated, NOT
// terminated); it rides every chunk so the receiver is stateless per chunk
// (no separate file-table packet to lose ordering against). offset is the
// write position within the file; fileIdx indexes the DONE CRC table.
struct SaveFileHeader {
    u8  type;     // = PKT_SAVE_FILE
    u32 ownerId;  // network player id of the sender (the host)
    u32 xferId;   // matching SaveBeginPacket.xferId
    u16 fileIdx;  // 0-based index into the transfer's file list
    u16 pathLen;  // bytes of relative path following this header (1..SAVE_PATH_MAX)
    u32 offset;   // byte offset of this chunk within the file
    u16 dataLen;  // payload bytes following the path (0..SAVE_CHUNK_MAX;
                  // 0 is legal for an empty file's single chunk)
};

// Host -> join: end of transfer + the per-file CRC table (FNV-1a-32 over
// each file's full content, fileIdx order): [SaveDoneHeader][u32 * fileCount].
struct SaveDoneHeader {
    u8  type;      // = PKT_SAVE_DONE
    u32 ownerId;   // network player id of the sender (the host)
    u32 xferId;    // matching SaveBeginPacket.xferId
    u16 fileCount; // CRC entries that follow (must equal BEGIN's fileCount)
};

// Join -> host: the staged save verified + committed (ok=1) or failed
// (ok=0: CRC mismatch / missing file / IO error - staging is discarded, the
// join's previous save state is untouched).
struct SaveAckPacket {
    u8  type;     // = PKT_SAVE_ACK
    u32 ownerId;  // network player id of the sender (the join)
    u32 xferId;   // the transfer being acknowledged
    u8  ok;       // 1 = committed; 0 = verify/commit failed
    u16 files;    // files committed
    unsigned __int64 bytes; // bytes committed
};

// ---- Coordinated load (protocol 32) ------------------------------------------
// The HOST is load-authoritative, mirroring the save arbitration. A load on
// the HOST (menu or programmatic - the SaveManager::load detour catches
// them all) broadcasts LOAD_GO before the host's own (never delayed) native
// load. The join compares the fingerprint against its on-disk copy: match
// = load the identical save now; missing/diverged = LOAD_NACK, the host
// streams the folder via the protocol-31 SaveXfer after its own reload and
// the join loads on commit. A join-initiated load is suppressed locally and
// forwarded as LOAD_REQ. loadId is per-host monotonic: a stale NACK from a
// superseded load is dropped by id mismatch.

// Host -> join: "load this save now". fingerprint is FNV-1a-32 over the
// folder's sorted relative paths + per-file content CRCs (savexfer::
// folderFingerprint) - byte-identical folders agree, any divergence differs.
// fingerprint 0 = the host folder was unreadable (the join must NACK).
struct LoadGoPacket {
    u8   type;        // = PKT_LOAD_GO
    u32  ownerId;     // network player id of the sender (the host)
    u32  loadId;      // per-host monotonic load id (stale-NACK guard)
    u32  fingerprint; // folder fingerprint of the host's copy (0 = unknown)
    char name[48];    // save name ('\0'-padded)
};

// Join -> host: "my player pressed load" (the join's local load was
// suppressed). The host runs its own loadSave(name) - whose detour edge
// broadcasts the LOAD_GO - if the save exists on the host.
struct LoadReqPacket {
    u8   type;    // = PKT_LOAD_REQ
    u32  ownerId; // network player id of the sender (the join)
    u32  reqId;   // monotonic per-sender (log correlation)
    char name[48]; // requested save name ('\0'-padded)
};

// Join -> host: "I can't load that - my copy is missing or diverged". The
// host answers with a protocol-31 SaveXfer of the folder (after its own
// reload completes); the join loads after the verified commit.
struct LoadNackPacket {
    u8   type;        // = PKT_LOAD_NACK
    u32  ownerId;     // network player id of the sender (the join)
    u32  loadId;      // the LOAD_GO being refused
    u32  fingerprint; // the join's local fingerprint (0 = missing folder)
    char name[48];    // save name ('\0'-padded)
};

// ---- Protocol 33: production machine sync ------------------------------------
// One machine state row, HOST-authoritative (world-simulation precedent: the
// host's engine is the one whose production/power/farming ticks count; the
// join's copies are quieted by convergence, not suppression - its machines
// still tick, this channel just corrects them ~1 Hz). Change-gated on the
// quantized fields + 10 s safety resend; per-sender seq drops stale rows.
// Identity: BAKED machines have save-stable hands (keyKind=0, the door
// precedent); SESSION-PLACED machines are runtime objects, so the key is the
// protocol-27 PLACER hand (keyKind=1) - the sender translates its local hand
// through its build maps (own placement = own hand; a minted proxy = the
// reverse map), the receiver resolves through its own (peer key -> minted
// local hand; own key -> own hand). -1 sentinels = field not carried (not
// this machine class / no buffer yet) - the hunger fold-in trick.
struct ProdPacket {
    u8  type;      // = PKT_PROD
    u32 ownerId;   // network player id of the sender (the host)
    u32 seq;       // per-sender monotonic (stale-row guard)
    u8  keyKind;   // 0 = baked hand, 1 = protocol-27 placer key
    u32 key[5];
    u8  classType; // BuildingClassType (diagnostics; apply re-reads locally)
    i8  powerOn;   // 0/1; -1 = unreadable (not applied)
    i8  prodState; // ProductionBuilding::ProductionState; -1 = not carried
    f32 outAmount; // output buffer amount; -1 = not carried
    char outSid[48]; // output item template sid ("" = not carried) - lets the
                   // receiver MATERIALIZE a still-null buffer with the same
                   // item via the native setProductionItem lever
    f32 inAmount[2]; // input buffer amounts; -1 = not carried
    f32 grown;     // farm growth floats; -1 = not a farm
    f32 died;
    f32 growStart;
    f32 harvested; // int on the engine side; carried as f32 (-1 = not a farm)
};

// ---- Protocol 38: research tech-tree sync -------------------------------------
// One KNOWN-research row, HOST-authoritative (world-simulation precedent: the
// host's tech tree is the party's). Identity is the RESEARCH GameData stringID
// - cross-client stable (both clients enumerate the identical record set from
// the shared save, spike 401). The host streams a row for every sid its
// Research store reports known (first sight = the session baseline, then a
// safety resend); the join applies via Research::startResearch, which is
// idempotent (already-known sids are skipped by an isKnown pre-check).
// Un-learning does not exist in the engine, so rows only ever ADD knowledge.
struct ResearchPacket {
    u8  type;      // = PKT_RESEARCH
    u32 ownerId;   // network player id of the sender (the host)
    u32 seq;       // per-sender monotonic (stale-row guard)
    char sid[48];  // RESEARCH GameData stringID (the wire key)
};

// NPC existence census (protocol 36): the host's 1 Hz wide-radius hand list.
// The positional stream stays at the ~200 u interest bubble; this list only
// says WHICH world NPCs exist on the host within the census radius, so the
// join can cull local-only ghosts long before they enter the stream bubble
// (the 2026-07-09 field report: join-visible NPCs vanishing on approach).
// v38 adds the host position per row: existence AND whereabouts, so the join
// can park a census-present local copy that wandered off the host's spot.
// Layout: [NpcCensusHeader][u32 hand[5] * count][f32 pos[3] * count] -
// 32 B per NPC, reliable (ENet fragments large lists), a few KB/s at worst.
struct NpcCensusHeader {
    u8  type;    // = PKT_NPC_CENSUS
    u32 ownerId; // network player id of the sender (the host)
    u16 count;   // number of 5xu32 hands (then 3xf32 positions) that follow
};

// Hard cap on hands per census packet (512 * 20 B = ~10 KB, fragmented fine).
const unsigned int NPC_CENSUS_MAX = 512;

// Camera hint (protocol 43, join -> host, ~1 Hz UNRELIABLE latest-wins): the
// join's camera world center, so the host can anchor an interest sphere where
// the join player is LOOKING (its PC may be elsewhere). Loss is harmless -
// the next hint lands a second later; a stale hint (> ~3 s) is dropped.
struct CamHintPacket {
    u8  type;    // = PKT_CAM_HINT
    u32 ownerId; // network player id of the sender (the join)
    f32 x, y, z; // CameraClass::getCenter() world position
};

struct TimePingPacket {
    u8  type;       // = PKT_TIME_PING
    u32 nonce;      // echo-match key
    u32 senderWallMs; // join's wallClockMs() at send
};

struct TimePongPacket {
    u8  type;         // = PKT_TIME_PONG
    u32 nonce;        // echoed from the ping
    u32 echoWallMs;   // the ping's senderWallMs, echoed verbatim
    u32 responderWallMs; // host's wallClockMs() at echo
};

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
