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
// (Stage 2 down/dead/ragdoll replication). Checked during handshake; a mismatch is
// rejected (no attempt at backward compatibility across versions).
const u16 PROTOCOL_VERSION = 2;

// Packet type tags (first byte of every packet).
enum PacketType {
    PKT_HELLO        = 1, // client -> host on connect: version + name
    PKT_WELCOME      = 2, // host -> client: version echo + assigned playerId
    PKT_LEAVE        = 3, // net thread -> game thread marker: a peer left
    PKT_ENTITY_BATCH = 4  // either direction: owner-tagged EntityState batch (20 Hz)
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
