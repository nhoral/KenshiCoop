// KenshiCoop shared wire protocol.
//
// IMPORTANT: this header is compiled by BOTH the VS2010 (v100) plugin and the
// modern-compiler nettest app. Keep it plain C++03: no <cstdint> guarantees on
// the old toolchain, no constexpr, no scoped enums, no STL containers.
//
// Wire format is little-endian. x86-64 is little-endian on both ends, so we send
// the packed struct bytes directly. If a big-endian peer is ever introduced,
// swap to the explicit read/write helpers below.

#ifndef KENSHICOOP_PROTOCOL_H
#define KENSHICOOP_PROTOCOL_H

#include <string.h> // memcpy

namespace coop {

// Fixed-width types without relying on <cstdint> (VS2010 has it, but be safe).
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef float              f32;

// Bump when the wire format changes incompatibly. Checked during handshake.
// v3: NpcStateEntry gains the NPC's current task + the task's subject object, so
// the client can reproduce the host's pose (sit/operate/etc.) at the same fixture.
// v4: NpcStateEntry gains the host's CharMovement state (currentSpeed, currentMotion
// vector, currentlyMoving). The engine's AnimationClass selects walk/idle/run from
// these, so mirroring them onto the client makes the locomotion animation match the
// host (fixes "walk-in-place" on held NPCs) without guessing clip names.
// v5: adds PKT_SQUAD_STATE - a bidirectional, owner-tagged batch of a peer's OWN
// player-squad members (reuses NpcStateEntry). Unlike the host-only NPC stream,
// BOTH peers stream their own squad; the receiver tags those hands by ownerId so
// it can apply the right authority rule (and, later, exclude them from NPC pick).
const u16 PROTOCOL_VERSION = 5;

// Packet type tags (first byte of every packet).
enum PacketType {
    PKT_HELLO        = 1, // client -> host on connect: protocol version + name
    PKT_WELCOME      = 2, // host -> client: assigned playerId
    PKT_PLAYER_STATE = 3, // either direction: a player's transform for this tick
    PKT_PING         = 4, // liveness / RTT probe
    PKT_PONG         = 5,
    PKT_PLAYER_LEFT  = 6, // net thread -> game thread: a player disconnected
    PKT_NPC_STATE    = 7, // host -> client: a batch of nearby NPC transforms
    PKT_SQUAD_STATE  = 8  // either direction: a batch of the sender's OWN squad
};

// Sentinel playerId meaning "all remote players" (used on local disconnect, when
// every remote ghost should be cleaned up at once).
const u32 PLAYER_ID_ALL = 0xFFFFFFFFu;

// ---- Packets (packed; sent as raw bytes) -------------------------------------

#pragma pack(push, 1)

struct HelloPacket {
    u8  type;        // = PKT_HELLO
    u16 version;     // = PROTOCOL_VERSION
    u8  nameLen;     // bytes of name following this struct (0..63)
    // char name[nameLen] follows
};

struct WelcomePacket {
    u8  type;        // = PKT_WELCOME
    u32 playerId;    // id the host assigned to this client
};

// One player's position+heading at a given simulation tick. This is the core
// Milestone 5 payload. Keep it small; it is sent frequently.
struct PlayerStatePacket {
    u8  type;        // = PKT_PLAYER_STATE
    u32 playerId;    // who this state belongs to
    u32 tick;        // sender's monotonically increasing tick counter
    f32 x;
    f32 y;
    f32 z;
    f32 heading;     // radians
};

struct PingPacket {
    u8  type;        // = PKT_PING / PKT_PONG
    u32 nonce;
};

// One NPC's identity + transform. Identity is the save-stable Kenshi `hand`
// (its five fields), which is identical across machines that load the same
// save, so the client resolves it back to its own local Character. Phase 2.
struct NpcStateEntry {
    u32 htype;            // hand.type (itemType)
    u32 hcontainer;       // hand.container
    u32 hcontainerSerial; // hand.containerSerial
    u32 hindex;           // hand.index
    u32 hserial;          // hand.serial
    f32 x;
    f32 y;
    f32 z;
    f32 heading;          // radians
    // Pose replication: the NPC's current task + the object that task targets.
    // task == 0xFFFF means "no current task" (client falls back to position hold).
    u16 task;             // host NPC's current TaskType (engine enum), or 0xFFFF
    u32 stype;            // subject hand.type
    u32 scontainer;       // subject hand.container
    u32 scontainerSerial; // subject hand.containerSerial
    u32 sindex;           // subject hand.index
    u32 sserial;          // subject hand.serial
    // Locomotion-animation state: the engine picks walk/idle/run from these, so
    // mirroring them makes the client copy animate like the host.
    f32 cspeed;           // CharMovement.currentSpeed
    f32 cmotionX;         // CharMovement.currentMotion.x (world-space motion)
    f32 cmotionY;         // CharMovement.currentMotion.y
    f32 cmotionZ;         // CharMovement.currentMotion.z
    u8  cmoving;          // CharMovement.currentlyMoving (0/1)
}; // 75 bytes

// Sentinel task value meaning "the host NPC had no current task this tick".
const u16 NPC_TASK_NONE = 0xFFFFu;

// An NPC batch packet is: [u8 type=PKT_NPC_STATE][u8 count][NpcStateEntry*count].
// Capped so a full batch stays comfortably inside one unreliable datagram.
const unsigned int NPC_BATCH_MAX = 18; // 18*75 + 2 = 1352 bytes

struct NpcBatchHeader {
    u8 type;  // = PKT_NPC_STATE
    u8 count; // number of NpcStateEntry that follow
};

// A squad batch packet is:
//   [SquadBatchHeader][NpcStateEntry * count]
// ownerId identifies the streaming peer (its network player id). The receiver
// tags every contained hand as owned by that peer. Same per-datagram cap as the
// NPC batch (a player squad is small, so one batch is plenty).
struct SquadBatchHeader {
    u8  type;    // = PKT_SQUAD_STATE
    u32 ownerId; // network player id of the squad's owner
    u8  count;   // number of NpcStateEntry that follow
};

#pragma pack(pop)

// ---- Helpers -----------------------------------------------------------------

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

#endif // KENSHICOOP_PROTOCOL_H
