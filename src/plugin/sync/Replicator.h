// Replicator - the per-tick sync orchestrator.
//
// Owns the flow between the engine and the network for replicated entities:
//   * publishOwned(): capture the locally-owned squad and hand it to the net
//     thread (called BEFORE the engine tick so the snapshot is current).
//   * ingest(): drain received entity transforms into per-entity interpolation
//     buffers (called BEFORE the engine tick so apply targets are current).
//   * applyTargets(): sample each buffer at the render time and apply the
//     interpolated pose to the resolved local Character (called AFTER the engine
//     tick so our write is the last word the renderer samples, beating the local
//     AI that re-decides each frame).
//
// Stage 2 routes apply through the interpolation buffer (smooth gliding); the
// transform is still committed via a raw teleport (no walk animation yet - that
// is Stage 3).

#ifndef KENSHICOOP_REPLICATOR_H
#define KENSHICOOP_REPLICATOR_H

#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "Interp.h"
#include "../../netproto/Wire.h"
#include "../core/Inbound.h"
#include "../net/NetLink.h"

class GameWorld;
class Character;
class RootObject;

namespace coop {

class Replicator {
public:
    Replicator();

    // Stage 1/2 stream only the squad leader; later stages stream the whole squad.
    void setLeaderOnly(bool v) { leaderOnly_ = v; }

    // Stage 4: also stream nearby host-authoritative world NPCs (host side).
    void setStreamNpcs(bool v) { streamNpcs_ = v; }

    // Bidirectional presence: the SQUAD-TAB ranks (distinct hand-containers, sorted)
    // this client OWNS (controls locally + streams). The peer owns the other tabs and
    // we drive those from its stream. Host defaults to {0}, join to {1}. Runs on BOTH
    // clients now, so each streams a disjoint set of squad tabs from the one shared
    // squad. On a single-tab save only rank 0 exists -> the join owns nothing and the
    // prior one-directional behaviour is preserved.
    void setOwnRanks(const std::set<unsigned int>& r) { ownRanks_ = r; }

    // AI-gating probe (join side): recruit diverged NPCs into the player squad to
    // validate the "inhabit" lever (stops self-tasking + becomes drivable).
    void setProbeRecruit(bool v) { probeRecruit_ = v; }

    // AI-suspend (join side, default ON): suspend the AI decision layer for
    // host-driven NPCs (faction-safe) so they stop self-tasking but keep
    // animating, and let the host stream be the sole task authority. The
    // universal quieting layer; per-class apply levers sit on top.
    void setAiSuspend(bool v) { aiSuspend_ = v; }

    // Step-2 experiment (KENSHICOOP_NO_DETACH=1): skip the sitter detachFromTownAI
    // in applyRest, betting that AI-suspend alone stops town-AI re-tasking. Off by
    // default; flip for a manual A/B before deleting the detach lever for good.
    void setNoDetach(bool v) { noDetach_ = v; }

    // Damage guard (join side, default ON): every driven (non-owned) body joins the
    // hitByMeleeAttack-suppressed set each tick, so the join's cosmetic fights apply
    // no local damage - outcomes stay host-authoritative (KO/death events).
    void setDamageGuard(bool v) { dmgGuard_ = v; }

    // Step 4 (KENSHICOOP_GATE_AUTHORITY=1, default off): divergence-gated authority.
    // A world NPC whose LOCAL AI has agreed with the host's rawTask for a sustained
    // streak while staying in position enters TRUSTED mode - no suspend, no drive,
    // cheap monitor only; re-engaged instantly on divergence or drift (doctrine 18).
    void setGateAuthority(bool v) { gateAuthority_ = v; }

    // Carried-body sync (protocol 18, default ON): reliable pickup/drop edges +
    // self-healing carried state for player-squad members, executed engine-
    // native on each machine's local pair. KENSHICOOP_CARRY_SYNC=0 disables.
    void setCarrySync(bool v) { carrySync_ = v; }

    // Peer-left sweep (carried-body sync): any driven (peer-owned) copy still
    // carrying a body after its owner disconnected gets a local drop, so the
    // carried body is released back to the ordinary KO/down channels.
    void sweepCarries(GameWorld* gw);

    // Phase 4a: register a container (by hand) this client is AUTHORITATIVE for, so
    // publishInventories streams its contents and applyInventories never reconciles
    // it back onto us. hand layout = [type, container, containerSerial, index, serial].
    // v1 registers a single host-owned baked storage container; clears prior set.
    void setOwnedContainerHand(const unsigned int hand[5]);
    void clearOwnedContainers() { ownedContainers_.clear(); }

    // BEFORE engine: drain received entities into their interpolation buffers.
    void ingest(Inbound& in);

    // BEFORE engine: drain received container-contents snapshots (Phase 4a) into the
    // per-container latest-snapshot cache (marks them dirty for applyInventories).
    void ingestInv(Inbound& in);

    // BEFORE engine (join side): drain reliable transition events and latch them
    // onto the matching tracked body (death = held down permanently; revive clears).
    void applyEvents(Inbound& in);

    // BEFORE engine: capture the locally-owned squad and publish it (host side).
    // Also detects per-entity bodyState transitions (KO/death/revive) and queues the
    // matching reliable event on 'net'.
    void publishOwned(GameWorld* gw, NetLink& net, u32 ownerId);

    // BEFORE engine: for each owned container, capture its contents and queue a
    // reliable snapshot when the content fingerprint changed (or a periodic safety
    // resend elapsed). No-op when no owned container is registered / resolves.
    void publishInventories(GameWorld* gw, NetLink& net, u32 ownerId);

    // BEFORE engine (host only): scan the interest sphere for free ground items, assign/
    // reuse a netId per item (keyed by its local engine hand), and queue a reliable
    // snapshot for new/changed items + a reliable cull for items that vanished. A settled
    // world produces no traffic (change-detected), with a slow periodic safety resend.
    void publishWorldItems(GameWorld* gw, NetLink& net, u32 ownerId);

    // AFTER engine (join only): drain received world-item snapshots/culls and reconcile
    // local proxies - spawn a proxy for a new netId, move it if it changed, destroy it on
    // cull. The host skips this (it authors the world stream).
    void applyWorldItems(GameWorld* gw, Inbound& in);

    // BEFORE engine (Phase W2/W3, runs on EVERY client): diff each OWNED character's WEAPON
    // census. A sustained count DECREASE is a DROP (the weapon left the bag; we never mutate
    // owned inventories ourselves) - author a reliable DROP intent at the owner position (the
    // engine's spatial item query can't find town drops, so we don't require it). A count
    // INCREASE is a PICKUP - author a reliable PICKUP intent. Tracks the real dropped Item*
    // handle per sid so the conservation channel can re-home the exact object on pickup.
    void detectAndPublishWeaponDrops(GameWorld* gw, NetLink& net, u32 ownerId);

    // AFTER engine (Phase W2): drain received DROP intents and relocate THIS client's own
    // copy of the weapon to the ground (Inventory::dropItem, no fabrication). Idempotent by
    // (ownerId,dropId); never acts on a character we own (we already dropped it locally).
    // Runs BEFORE applyInventories so the relocation beats the debounced removal reconcile.
    // Tracks the relocated Item* so a later pickup can re-home that exact object.
    void applyWeaponDrops(GameWorld* gw, Inbound& in);

    // AFTER engine (Phase W3): drain received PICKUP intents and re-home THIS client's tracked
    // ground copy of the weapon back into the picking character's bag (no fabrication, no
    // spatial re-query - uses the remembered Item* handle). Idempotent by (ownerId,pickupId);
    // never acts on a character we own (we already picked it up locally). Runs BEFORE
    // applyInventories so the re-home beats the inventory reconcile.
    void applyWeaponPickups(GameWorld* gw, Inbound& in);

    // BEFORE engine, after publishOwned (phase 2 vitals sync): stream each OWNED
    // player-squad member's medical model (blood, bleed, limb flesh/bandaging,
    // flags) on the RELIABLE channel - change-gated by a quantized fingerprint,
    // ~2 Hz sampling, periodic safety resend. World NPCs are never included
    // (events-only model). Uses the ownHands_ set publishOwned just refreshed.
    void publishMedical(GameWorld* gw, NetLink& net, u32 ownerId);

    // BEFORE engine (phase 2): drain received medical snapshots and write them
    // onto our DRIVEN copies (never our own bodies), remembering the last
    // received bandage levels per body. Also runs the treatment DETECTOR: local
    // bandaging risen ABOVE the last received level means first aid happened on
    // this machine on a driven copy - forward the levels reliably to the owner.
    void applyMedical(GameWorld* gw, Inbound& in, NetLink& net, u32 ownerId);

    // BEFORE engine (phase 2): drain received treatment deltas; each subject we
    // OWN gets the bandage levels applied raise-only (idempotent). The vitals
    // stream then mirrors the healed state back to everyone.
    void applyTreatments(GameWorld* gw, Inbound& in);

    // AFTER publishOwned (protocol 17, both clients): stream each OWNED
    // player-squad member's CharStats (attributes/skills/xp) on the RELIABLE
    // channel - change-gated by a quantized fingerprint, ~1 Hz floor, periodic
    // safety resend. The peer's engine resolves REAL fights with its copy of
    // these numbers, so staleness is a gameplay bug, not a display one.
    void publishStats(GameWorld* gw, NetLink& net, u32 ownerId);

    // BEFORE engine (protocol 17): drain received stats snapshots and write
    // them onto our DRIVEN copies (never our own bodies) + recalculate the
    // derived caches. Also self-heals the junk XP a driven copy's cosmetic
    // fights accumulate locally (the owner's stream overwrites it).
    void applyStats(GameWorld* gw, Inbound& in);

    // BEFORE engine (consensus game-speed sync, runs on BOTH clients):
    //  * detect a LOCAL user speed click (current state != what WE last applied)
    //    and turn it into a request (pause = speed 0);
    //  * join: send PKT_SPEED_REQ (change-gated + 3 s safety resend) and apply
    //    any received PKT_SPEED_SET;
    //  * host: consume its own request locally, drain peer requests, arbitrate
    //    effective = min(requests) capped at 1x while either player squad is in
    //    combat (own flag from ownHands_+readCombat, peer flag from its REQ),
    //    apply locally and broadcast PKT_SPEED_SET on change (+ safety resend).
    void syncSpeed(GameWorld* gw, Inbound& in, NetLink& net, u32 ownerId, bool isHost);

    // AFTER engine: sample + apply the interpolated pose for every tracked entity.
    void applyTargets(GameWorld* gw);

    // AFTER engine: reconcile each peer-owned container we received a (dirty) snapshot
    // for to its desired contents. Skips containers we own (we author those).
    void applyInventories(GameWorld* gw);

    // AFTER applyTargets (join only): make the host authoritative for world NPCs.
    // Any nearby NPC the host is NOT streaming this tick is hidden + frozen so the
    // join's local AI can't run a divergent copy (the standing-on-a-host-seat
    // double / extra-NPC problem). Suppressed NPCs are restored if the host later
    // streams them.
    void enforceHostAuthority(GameWorld* gw);

    unsigned int targetCount() const { return (unsigned int)targets_.size(); }

    // Stage 2 smoothness oracle: emit a "SCENARIO SMOOTH ..." summary describing
    // how often the driven body moved per frame while its source was in motion.
    // Per-frame interpolation keeps zeroFrac low; raw 20 Hz stepping makes it high.
    void logSmoothSummary();

private:
    struct Key {
        u32 t, c, cs, i, s;
        bool operator<(const Key& o) const {
            if (t != o.t) return t < o.t;
            if (c != o.c) return c < o.c;
            if (cs != o.cs) return cs < o.cs;
            if (i != o.i) return i < o.i;
            return s < o.s;
        }
    };
    static Key keyOf(const EntityState& e) {
        Key k; k.t = e.hType; k.c = e.hContainer; k.cs = e.hContainerSerial;
        k.i = e.hIndex; k.s = e.hSerial; return k;
    }

    struct Driven {
        EntityInterp interp;
        bool         fresh;          // host streamed a non-stale sample this tick
        bool         haveActual;     // lx/ly/lz hold a valid previous actual pos
        float        lx, ly, lz;     // last actual (rendered) position
        bool         parked;         // settled at rest (avoid re-halting the clip)
        bool         haveDest;       // dx/dy/dz hold a previously issued destination
        float        dx, dy, dz;     // last issued walk destination
        bool         suppressed;     // NPC pulled off the local AI update list
        unsigned long lastSeenMs;    // last ingest for this hand (stale-entry pruning)
        // Stage 5 rest-pose reproduction.
        u16          issuedTask;     // task we last committed (TASK_NONE = none)
        bool         taskApplied;    // a fixture-resolved pose is currently held
        bool         taskBad;        // task not reproducible here (fixture missing/drift)
        unsigned long taskTick;      // when the task was issued (drift grace timer)
        bool         detached;       // I9: detached from town-AI (separateIntoMyOwnSquad) once
        bool         downApplied;     // Stage 2: body is currently held in ragdoll (host says down)
        bool         koLatched;       // a reliable EVT_KNOCKOUT pinned this body down
        bool         deathLatched;    // a reliable EVT_DEATH pinned this body down PERMANENTLY
        bool         combatArmed;     // Stage 3c: a melee-attack order is currently issued
        unsigned long combatTick;     // when the attack order was (re-)issued (re-arm throttle)
        unsigned int combatOrders;    // orders issued this combat episode (re-issue backoff)
        u32          combatTgtIdx;    // target hand (index/serial) of the last issued order,
        u32          combatTgtSer;    //   so a host-side retarget re-issues immediately
        unsigned long combatSeenTick; // last tick the host stream reported combat (the
                                      //   disarm DEBOUNCE: a 1-batch gap must not reset the AI)
        unsigned long combatSnapTick; // last hard snap (cooldown; a failed teleport must not
                                      //   re-fire every frame - walk-converge between snaps)
        bool         goalsCleared;    // rest-entry AI-goal clear done (once per rest episode;
                                      // re-cleared only after the body genuinely moves again)
        // Step 4 divergence-gated authority (world NPCs, behind gateAuthority_):
        bool         trusted;         // local AI agreed long enough -> not driven/suspended
        unsigned int agreeStreak;     // consecutive agreeing+in-position samples
        // Carried-body sync (protocol 18):
        unsigned long carryHealTick;  // last self-heal pickup attempt (throttle)
        unsigned long carryNoSeeTick; // first tick a locally-carrying copy's stream
                                      //   stopped reporting TASK_CARRY_BODY (the
                                      //   debounced host-side-drop detector)
        Driven() : fresh(false), haveActual(false), lx(0), ly(0), lz(0), parked(false),
                   haveDest(false), dx(0), dy(0), dz(0),
                   suppressed(false), lastSeenMs(0),
                   issuedTask(TASK_NONE), taskApplied(false), taskBad(false),
                   taskTick(0), detached(false), downApplied(false),
                   koLatched(false), deathLatched(false),
                   combatArmed(false), combatTick(0), combatOrders(0),
                   combatTgtIdx(0), combatTgtSer(0),
                   combatSeenTick(0), combatSnapTick(0),
                   goalsCleared(false),
                   trusted(false), agreeStreak(0),
                   carryHealTick(0), carryNoSeeTick(0) {}
    };

    // Reproduce the host's rest pose on a driven body: if it carries a task whose
    // fixture resolves locally, commit it ONCE (seated/idle at the same object);
    // otherwise quiet the AI and park at the host transform. Drift-guarded; re-arms
    // when the host task changes. Used by both the NPC and squad at-rest branches.
    void applyRest(Character* c, Driven& d, const EntityState& out,
                   bool haveActual, float ax, float ay, float az, unsigned long now);

    std::map<Key, Driven> targets_;
    // Host side: last bodyState we published per owned entity (+ when it was last
    // captured, so long-gone entities age out), so we can emit a reliable event on
    // the rising/falling edge (KO/death/revive) exactly once.
    struct HostBody {
        u16 bs; unsigned long seenMs;
        // Carried-body sync (protocol 18): last published carry relationship of
        // an OWNED member, so publishOwned can emit the reliable pickup/drop
        // edge exactly once per transition. carried = the carried body's hand
        // (readObjectHand layout), meaningful only while carrying.
        bool carrying; unsigned int carried[5];
        HostBody() : bs(0), seenMs(0), carrying(false) {
            carried[0] = carried[1] = carried[2] = carried[3] = carried[4] = 0;
        }
    };
    std::map<Key, HostBody> hostBody_;
    // Host side: who is attacking whom (victim key -> {attacker key, lastSeenMs}), built
    // from the combat intents in the captured set (task==TASK_COMBAT_MELEE, subject =
    // the target). STICKY with a recency window: the attacker drops its target the instant
    // the victim falls, so the down/death edge would otherwise see no current attacker -
    // we keep the last attacker for ATTR_WINDOW_MS and stamp it as the event's ACTOR
    // (causality: "downed BY"), so combat KO/death events carry who-did-it.
    std::map<Key, std::pair<Key, unsigned long> > attackerOf_;
    u32                   nextEventId_;
    // Stage 6: world NPCs we've hidden+frozen on the join because the host isn't
    // streaming them. Keyed by hand so we restore the exact body when it re-enters
    // the host's streamed set.
    std::map<Key, Character*> suppressed_;
    // Bodies applyTargets actually drove this tick, by POINTER: a combat-driven
    // world NPC is detached into its own squad (container changes), so its local
    // enumeration key no longer matches the host's streamed key - the hand-keyed
    // `keep` set can't recognise it and enforceHostAuthority would hide+freeze a
    // body mid-fight. Pointer identity survives the re-containering.
    std::set<Character*>      drivenChars_;
    // Step-5 hysteresis: consecutive-frame counters per hand so a brief stream
    // hiccup doesn't suppress (needs ~1 s unstreamed) and a boundary NPC doesn't
    // flicker back (needs ~2 s streamed dwell to restore). Spike 18: the hard
    // interest edge had no dwell band, so boundary patrollers churned.
    struct AuthCount { unsigned int unstreamed; unsigned int streamed;
                       AuthCount() : unstreamed(0), streamed(0) {} };
    std::map<Key, AuthCount>  authCount_;
    unsigned long             authSuppresses_; // churn counters (split_interest metric)
    unsigned long             authRestores_;
    InterpConfig          cfg_;
    bool                  leaderOnly_;
    bool                  streamNpcs_;

    // Bidirectional ownership partition. ownRanks_ = the squad-TAB ranks (distinct
    // sorted hand-containers) we own; ownHands_ = the resolved owned hand keys,
    // refreshed every publishOwned. applyTargets skips any tracked hand in ownHands_
    // (never drive a body we own + simulate locally), defense-in-depth on the partition.
    std::set<unsigned int> ownRanks_;
    std::set<Key>          ownHands_;

    // Phase 4a inventory state. ownedContainers_ = containers we author (publish, never
    // reconcile back onto us). invPub_ = last published content fingerprint + send time
    // per owned container (resend only on change / periodic). invRecv_ = latest received
    // snapshot per peer-owned container, applied (reconciled) when dirty.
    // hash      = last fingerprint actually SENT to peers.
    // lastSendMs = when we last sent (for the periodic safety resend).
    // pendingHash/pendingSince = the most recent captured fingerprint and when it first
    //   appeared; a change is only SENT once it has been STABLE for INV_SETTLE_MS. This
    //   debounce suppresses sub-second mid-drag transients (e.g. a weapon held by the
    //   cursor briefly leaves the inventory entirely) that would otherwise tell the peer
    //   to DESTROY a worn item it cannot refabricate (createItemAndAdd fails for weapons),
    //   losing it permanently. Settled states (the only ones a user actually rests at)
    //   still replicate, just ~one settle-window later.
    // lastSentN = entry count of the last SENT snapshot. A capture with FEWER entries is a
    //   REMOVAL (something left the inventory) and must settle far longer than an addition
    //   or an equip<->loose MOVE (same count): mid-drag the cursor holds the item OUT of the
    //   inventory for up to ~1 s, a transient "gone" the peer would act on by DESTROYING a
    //   worn item it cannot refabricate. Equip/unequip-to-bag keep the count, so they still
    //   replicate fast; only genuine removals (and the cursor flicker) wait.
    struct InvPub { u32 hash; unsigned long lastSendMs; u32 pendingHash; unsigned long pendingSince; unsigned int lastSentN; };
    struct InvRecv { u32 ownerId; std::vector<InvItemEntry> items; bool dirty; };
    std::set<Key>          ownedContainers_;
    std::map<Key, InvPub>  invPub_;
    std::map<Key, InvRecv> invRecv_;

    // Phase W1 world-item state.
    // HOST: worldTrack_ maps a ground item's LOCAL engine hand (Key) to its assigned
    //   netId + last-sent content+pos hash + last send time. nextWorldNetId_ hands out
    //   fresh ids. Items in the map but not seen this scan have left the world -> culled.
    // JOIN: worldProxies_ maps a host netId to the LOCAL proxy object we spawned for it,
    //   plus the last applied pos/hash so we only re-place it on real change.
    struct WorldTrack { u32 netId; u32 hash; unsigned long lastSendMs; float x, y, z; bool seen; };
    struct WorldProxy { RootObject* obj; float x, y, z; u32 hash; };
    std::map<Key, WorldTrack> worldTrack_;
    std::map<u32, WorldProxy>  worldProxies_;
    u32                        nextWorldNetId_;

    // Phase W2 conservation-drop state.
    // weaponCensus_: per OWNED character hand, the last-tick set of WEAPON copies it held,
    //   keyed by "sid|type" -> {provenance + count}. A count DECREASE that coincides with a
    //   free ground weapon near the character is a drop. seeded=false on first sight (we
    //   record the baseline without emitting a spurious drop). nextDropId_ hands out per-
    //   sender monotonic ids. appliedDrops_ dedupes received intents by (ownerId,dropId).
    struct WCensusItem { char manufacturer[48]; char material[48]; u16 quality; int count; unsigned int itemType; };
    struct WCensus {
        std::map<std::string, WCensusItem> items;
        std::map<std::string, int>         retries; // per-sid ground-correlation retry budget
        std::map<std::string, std::deque<void*> > ptrs;        // current weapon Item* per sid
        bool seeded;
    };
    std::map<Key, WCensus>            weaponCensus_;
    u32                               nextDropId_;
    std::set<std::pair<u32, u32> >    appliedDrops_;

    // Phase W3 conservation-pickup state.
    // groundedWeapons_: the REAL ground Item* handles THIS client is tracking per sid (from
    //   its own UI drop OR from relocating a peer's drop). A received PICKUP re-homes one of
    //   these into the picking character's bag; a local pickup (census increase) consumes one.
    // nextPickupId_ hands out per-sender monotonic ids; appliedPickups_ dedupes received ones.
    std::map<std::string, std::deque<void*> > groundedWeapons_;
    u32                               nextPickupId_;
    std::set<std::pair<u32, u32> >    appliedPickups_;

    // Smoothness accumulators (measured from the body's actual motion while its
    // source is moving): how often did the rendered body advance per frame?
    unsigned long activeFrames_;
    unsigned long zeroWhileActive_;
    float         maxStep_;

    // Anim-truth accumulators (the float-bug oracle): of the frames where the
    // body's actual position translated, how many reported a real walk state
    // (currentlyMoving && currentSpeed>0)? A high floatFrac == translating while
    // reporting idle == the float bug.
    unsigned long translateFrames_;
    unsigned long walkTruthFrames_;

    // March-in-place oracle (the INVERSE of the float bug): of the frames where the
    // host is AT REST (not translating), how many had the driven body reporting a
    // walk state (currentlyMoving && currentSpeed>0) while NOT actually moving? A
    // high marchFrac == the body is playing a walk clip in place == the "walking on
    // the spot where the host sits" bug. This is the failure anim-truth cannot see.
    unsigned long restSampleFrames_;
    unsigned long marchFrames_;

    // AI-gating spike: how often does a driven NPC's LOCAL task match the host's
    // streamed rawTask? High agreement => we can gate the local AI (freeze when it
    // matches, release when it diverges) instead of replicating animation data.
    unsigned long gateSamples_;
    unsigned long gateAgree_;
    unsigned long gateLogTick_;

    // AI-gating probe state: recruit each diverged NPC at most once, capped so the
    // experiment stays observable rather than absorbing the whole bar.
    bool                 probeRecruit_;
    std::set<Key>        probed_;
    unsigned int         probedCount_;

    // AI-suspend state (default-on quieting layer).
    bool                 aiSuspend_;
    unsigned long        aiLogTick_;

    // Quieting-patchwork counters (step-2 pruning evidence). With AI-suspend as the
    // default quieting layer these mechanisms should be dead code; the counters
    // MEASURE that claim per run ("SCENARIO QUIET ..." in logSmoothSummary) so the
    // eventual deletion is evidence-gated rather than hopeful.
    unsigned long        quietRelapse_;   // I11: endAction re-fired on a march relapse
    unsigned long        sitOrders_;      // applyTaskOrder issues (the sit/work APPLY lever)
    unsigned long        detachUses_;     // detachFromTownAI calls from applyRest (sitter path)
    bool                 noDetach_;       // KENSHICOOP_NO_DETACH=1: skip sitter detach (A/B experiment)

    // Damage-guard state (join side): suppress local melee damage on driven bodies.
    bool                 dmgGuard_;

    // Carried-body sync (protocol 18): master enable (KENSHICOOP_CARRY_SYNC).
    bool                 carrySync_;

    // Phase-2 medical channel state.
    // medPub_: per OWNED member, the last SENT quantized fingerprint + send time
    //   (change gate + periodic safety resend, the inventory-snapshot pattern).
    // medRecv_: per DRIVEN body, the last RECEIVED bandage levels + when we last
    //   forwarded a treatment (throttle). The detector compares the copy's LOCAL
    //   bandaging against these: local > received means first aid happened HERE
    //   (the stream can only set it back to the owner's level, so the comparison
    //   has no race). sentBand[] remembers what we already forwarded so an
    //   unacknowledged rise isn't re-sent every tick.
    // limbPrev[]: the last PUBLISHED LimbStates (0xFF = never read) - an edge to
    //   STUMP/CRUSHED authors the reliable EVT_AMPUTATE/EVT_CRUSH transition
    //   (doctrine 16; the packet's limbState[] is the self-heal).
    struct MedPub  {
        u32 hash; unsigned long lastSendMs; unsigned char limbPrev[4];
        MedPub() : hash(0), lastSendMs(0) {
            for (int i = 0; i < 4; ++i) limbPrev[i] = 0xFF;
        }
    };
    // Protocol 16: bandage levels are tracked per ANATOMY PART (12 slots), not
    // per limb - first aid on a head wound forwards too.
    struct MedRecv {
        float recvBand[12]; float sentBand[12]; unsigned long lastFwdMs; bool have;
        MedRecv() : lastFwdMs(0), have(false) {
            for (int i = 0; i < 12; ++i) { recvBand[i] = -1.0f; sentBand[i] = -1.0f; }
        }
    };
    std::map<Key, MedPub>  medPub_;
    std::map<Key, MedRecv> medRecv_;
    u32                    nextTreatId_;
    // Protocol 17: per OWNED member, the last SENT quantized stats fingerprint
    // + send time (the medPub_ pattern; stats creep slowly so the channel is
    // near-silent between level-ups).
    struct StatsPub {
        u32 hash; unsigned long lastSendMs;
        StatsPub() : hash(0), lastSendMs(0) {}
    };
    std::map<Key, StatsPub> statsPub_;
    // Phase B: combat-scoped world-NPC vitals (host side). Keys of streamed
    // NPCs that are fighting / being fought / down, with last-qualified time;
    // publishMedical streams their vitals at ~1 Hz while fresh. The join's
    // driven copies are damage-guarded, so the write has nothing local to
    // fight. Also the authority set for TREATMENTS on world NPCs (a join medic
    // bandaging a downed host NPC forwards here).
    std::map<Key, unsigned long> medNpc_;

    // Consensus game-speed sync state. Requests and applied values use ONE
    // number: the multiplier, with 0 meaning paused (min() then gives "either
    // can pause, both must raise"). -1 = not yet known.
    float         speedLastApplied_;   // what WE last wrote (own-write vs user-click detector)
    float         speedMyReq_;         // this client's current request
    float         speedPeerReq_;       // host only: the join's latest request (-1 = none yet)
    bool          speedMyCombat_;      // own-squad in-combat flag (~1 Hz sample)
    bool          speedPeerCombat_;    // host only: the join's reported combat bit
    float         speedLastSet_;       // host: last broadcast effective; join: last received
    u32           speedSeqOut_;        // per-sender monotonic seq for REQ/SET we send
    u32           speedSeqSeen_;       // newest seq accepted from the peer (stale guard)
    unsigned long speedLastSendMs_;    // last REQ (join) / SET (host) send, safety resend
    unsigned long speedCombatSampleMs_;// last own-combat sample time

    // Divergence-gated authority state (step 4).
    bool                 gateAuthority_;
    unsigned long        trustLogTick_;
    unsigned long        trustGrants_;   // trusted-mode entries this run
    unsigned long        trustRevokes_;  // trusted-mode exits (divergence/drift)
};

} // namespace coop

#endif // KENSHICOOP_REPLICATOR_H
