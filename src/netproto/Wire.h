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
// in towns, each client tracks the actual dropped Item* handle rather than re-finding it;
// to 12 when the wall-clock TIME-SYNC channel (PKT_TIME_PING/PKT_TIME_PONG) was added -
// the join periodically pings with its wall clock, the host echoes with its own, and the
// join estimates the host-relative clock OFFSET (NTP-style, min-RTT filtered), logging
// CLOCKSYNC lines the validation oracles use to time-align host/join logs across two
// machines whose wall clocks disagree (a remote-play prerequisite);
// to 13 when the owner-authoritative MEDICAL channel was added (phase 2 of the
// player-combat/medical plan): PKT_MEDICAL streams an owned PLAYER-SQUAD
// character's local-only medical model (blood, bleed, per-limb flesh+bandaging,
// unc/dead flags) to the peer, change-gated + throttled, so driven copies render
// true vitals instead of diverging forever (spikes 21-23); and PKT_TREATMENT
// carries first aid administered ON a driven copy back to the body's OWNER
// (per-limb bandage levels, raise-only apply) so cross-player healing lands on
// the authoritative body. World NPCs stay on the events-only model;
// to 14 when the CONSENSUS GAME-SPEED channel was added: each client's UI speed
// (pause/1x/2x/3x) is a REQUEST (PKT_SPEED_REQ, join -> host; the host consumes
// its own locally), the host arbitrates effective = min(requests) capped at 1x
// while either player squad is in combat, and broadcasts PKT_SPEED_SET which
// both sides apply. Divergent speeds would diverge every rate-based local
// simulation (medical, hunger, cosmetic fights), so speed is consensus state;
// to 15 when the combat intent split into ACTIVE vs WAITING stances
// (TASK_COMBAT_WAIT): Kenshi's attack-slot system keeps most of a crowd
// "engaged but queued", and driving those copies with the active-attack
// re-issue loop reset their AI every throttle tick (clearGoals) and teleported
// them around. The host now classifies its combatState (sword state) and the
// join leaves waiting copies holding their goal in a menace ring;
// to 16 when the medical channel grew to FULL anatomy + limb loss (full
// medical/limb sync plan): MedicalPacket now carries every body part (head/
// chest/stomach + limbs; humans have 7, MED_PARTS_MAX slots) with flesh AND
// fleshStun AND bandaging AND juryRigging per part (keyed by anatomy index -
// deterministic across clients loading the same save), plus the 4 LimbStates
// (stump/crushed/replaced) and the robotic replacement template stringIDs;
// TreatmentPacket forwards per-PART bandage levels; EVT_AMPUTATE/EVT_CRUSH
// reliable events carry the limb-loss transitions (medical packet states are
// the self-heal). Combat-scoped world NPCs now stream vitals too (host-
// authoritative), so a battered NPC no longer renders pristine on the join;
// to 17 when the owner-authoritative CHARACTER-STATS channel (PKT_STATS) was
// added: CharStats (strength..smithing, xp) is local-only like medical, so a
// character leveled mid-session stayed stale on the peer - and the peer's
// engine RESOLVES real fights with that stale copy (a join character vs a
// world NPC resolves on the HOST). Owned player-squad members only,
// change-gated + throttled; the stream also self-heals the junk XP a driven
// copy's cosmetic fights accumulate locally;
// to 18 when CARRIED-BODY sync was added: picking up / carrying / dropping a
// KO'd player-squad member. The CARRIER's owner authors reliable
// EVT_PICKUP_BODY/EVT_DROP_BODY edges (subject = carried, actor = carrier);
// continuous state self-heals them (synthetic TASK_CARRY_BODY on the carrier
// + a BODY_CARRIED bodyState bit on the carried). Each machine executes the
// SAME pickup between its LOCAL pair via the engine's own pickupObject, so
// the shoulder attach / carry animation / transform-follow are engine-native
// on both sides; a carried copy is exempt from the down-enforcement hold and
// all position driving (the local attach owns its transform);
// to 19 when FURNITURE OCCUPANCY sync was added (beds + prison cages): the
// carry shape applied to a stateful attach. The OCCUPANT's owner authors
// reliable EVT_ENTER_FURNITURE/EVT_EXIT_FURNITURE edges (subject = occupant,
// actor = the furniture's save-stable hand, arg = 1 bed / 2 cage) off
// Character::inSomething transitions; continuous BODY_IN_BED/BODY_IN_CAGE
// bodyState bits self-heal them. Each machine executes the SAME placement
// between its LOCAL pair via the engine's own setBedMode/setPrisonMode, so
// the in-bed/in-cage pose and transform are engine-native on both sides; an
// occupied copy is exempt from down-enforcement and position driving (the
// furniture owns its transform).
// to 20 when STEALTH sync was added: a BODY_SNEAK bodyState bit streams
// Character::stealthMode exactly (BODY_CRAWL stays isStealthModeOrCrawling -
// it includes injured crawl, which must never trigger setStealthMode), and
// the receiver applies the engine's own setStealthMode to its driven copy so
// the sneak-walk is engine-native. PKT_STEALTH streams the DETECTION map
// (whoSeesMeSneaking: per-seer YesNoMaybe + progress) of a driven sneaker
// back to its OWNER - the first owner-directed FEEDBACK channel: the host's
// authoritative world computes who notices the sneaker (spike: detection DOES
// fire against driven copies), and the owner replays each entry between its
// LOCAL pair via notifyICanSeeYouSneaking so the marker arrows render
// natively on the player's own screen.
// to 21 when RUNTIME-SPAWN proxy replication was added: NPC sync resolves
// bodies by save-stable hand, so a squad the host's spawn manager mints at
// RUNTIME (roaming bandits, dialog ambushes) has a host-only hand the join
// can never resolve - the host fought enemies the join couldn't see (spike
// 01, field report 2026-07-07). PULL-based: the join sends PKT_SPAWN_REQ
// (reliable, debounced per hand) for any streamed hand it cannot resolve;
// the host resolves it locally and replies PKT_SPAWN_INFO (character
// template stringID + faction stringID + transform + alive flag); the join
// spawns a local PROXY body from that description and drives it through the
// SAME world-NPC path as a baked NPC (AI-suspend, damage guard, combat
// rendering all inherit - the hand->proxy translation happens at the single
// applyTargets resolve choke point);
// to 22 when the MONEY channel (PKT_MONEY) was added: Kenshi's wallet is
// per-Platoon (Ownerships::money - no global player wallet, spike 29) and
// nothing about it was on the wire (shop_probe run 104watch: sentinel writes
// never crossed), so any purchase/sale/bounty changed cats on ONE client only.
// Owner-authoritative by squad-tab RANK (the same partition positional/
// inventory sync own): each client publishes the wallet of every tab it OWNS,
// change-gated on the reliable channel with a safety resend; the receiver
// writes the peer tab's wallet via Ownerships::setMoney;
// to 23 when RECRUITMENT sync was added: recruiting re-containers the subject
// into a player platoon (a NEW hand the peer can never resolve), so a recruit
// existed on the recruiting client only (recruit_probe run 114151: the peer
// either minted a DUPLICATE proxy next to its still-standing baked copy, or -
// join -> host - saw nothing at all, the describe channel being join-pull
// only). Three changes: a reliable EVT_RECRUIT edge (subject = the OLD hand,
// actor = the NEW hand) lets the peer RE-KEY its existing local body to the
// recruiter's new stream key instead of duplicating; the describe/mint spawn
// channel (PKT_SPAWN_REQ/INFO) runs BIDIRECTIONALLY so a runtime-born recruit
// resolves in either direction; and recruited hands are owned by their
// RECRUITER regardless of which local tab rank they land in (the probe showed
// a join recruit landing in the HOST-owned rank-0 container).
const u16 PROTOCOL_VERSION = 23;

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
    PKT_MONEY            = 21 // RELIABLE owner-authoritative tab wallet (protocol 22); MoneyPacket
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
    // arg: 1 = bed, 2 = prison cage.
    EVT_ENTER_FURNITURE = 8, // occupant was placed in / climbed into the furniture
    EVT_EXIT_FURNITURE  = 9, // occupant left / was removed from the furniture
    // Recruitment sync (protocol 23). subject = the recruited body's OLD hand
    // (its identity BEFORE PlayerInterface::recruit re-containered it), actor =
    // its NEW hand (the key the recruiter streams it under from now on). The
    // receiver re-keys its local copy of the old hand to the new key (no
    // duplicate body); if the old hand doesn't resolve there (runtime-born
    // subject) the bidirectional describe/mint channel covers it instead.
    EVT_RECRUIT = 10
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

// True if the body should be treated as lying down (suppress walk-drive / parking).
// Deliberately ignores BODY_CARRIED (and the occupancy bits): the receiver checks
// bodyIsCarried/bodyInFurniture FIRST and skips the down path entirely for them.
inline bool bodyIsDown(u16 s)    { return (s & (BODY_DOWN | BODY_RAGDOLL | BODY_DEAD)) != 0; }
inline bool bodyIsCarried(u16 s) { return (s & BODY_CARRIED) != 0; }
inline bool bodyInFurniture(u16 s) { return (s & (BODY_IN_BED | BODY_IN_CAGE)) != 0; }
inline bool bodySneaking(u16 s)  { return (s & BODY_SNEAK) != 0; }

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
