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

    // Protocol 36 live-tuning knobs (KENSHICOOP_INTERP_* / _CATCHUP_K /
    // _SNAP_DIST): override the interp buffer's delay/extrapolation window and
    // the walk-drive's hard-snap / catch-up gains for WAN A/B runs without a
    // rebuild. Defaults match the historical constants.
    void setInterpConfig(const InterpConfig& c) { cfg_ = c; }
    void setDriveTuning(float catchupK, float snapDist, float snapSeconds) {
        if (catchupK > 0.0f) catchupK_ = catchupK;
        if (snapDist > 0.0f) snapDist_ = snapDist;
        if (snapSeconds > 0.0f) snapSeconds_ = snapSeconds;
    }
    // Combat-drive convergence bands (2026-07-16 smoothness pass). 0 keeps the
    // ReplicatorUtil constant default; > 0 overrides for a sweep without a rebuild.
    void setCombatTuning(float softDist, float snapDist, float bigSnapDist,
                         float slideMax, unsigned long convergeMs) {
        if (softDist > 0.0f)    combatSoftDist_    = softDist;
        if (snapDist > 0.0f)    combatSnapDist_    = snapDist;
        if (bigSnapDist > 0.0f) combatBigSnapDist_ = bigSnapDist;
        if (slideMax > 0.0f)    combatSlideMax_    = slideMax;
        if (convergeMs > 0)     combatConvergeMs_  = convergeMs;
    }
    // KENSHICOOP_SEND_STAMP=0: ignore the batch sendMs and index interp rings
    // on arrival time (the legacy scheme) - a receiver-local A/B lever.
    void setSendStamp(bool v) { sendStamp_ = v; }
    // KENSHICOOP_STARVE_HOLD_MS: how long a driven body whose stream went
    // STALE keeps its mutation guards (AI suspend + damage guard) before it
    // is released to local simulation. A brief WAN stall must not become an
    // authority transfer (architecture review 2026-07-10); 0 restores the
    // legacy release-on-stale behavior.
    void setStarveHold(unsigned long ms) { starveHoldMs_ = ms; }

    // Bidirectional presence: the SQUAD-TAB ranks (distinct hand-containers, sorted)
    // this client OWNS (controls locally + streams). The peer owns the other tabs and
    // we drive those from its stream. Host defaults to {0}, join to {1}. Runs on BOTH
    // clients now, so each streams a disjoint set of squad tabs from the one shared
    // squad. On a single-tab save only rank 0 exists -> the join owns nothing and the
    // prior one-directional behaviour is preserved.
    void setOwnRanks(const std::set<unsigned int>& r) { ownRanks_ = r; }

    // Cross-owner trade veto classifier (engine InvOwnerClassFn). Given a
    // save-stable owner hand (readObjectHand layout [type,container,
    // containerSerial,index,serial]) returns 0 = not a player-squad member,
    // 1 = a squad member owned by THIS client, 2 = a squad member owned by the
    // PEER. Consults the sets publishOwned refreshes each tick (allSquad_ +
    // ownHands_). Const + set-lookup only, so it is safe to call from the engine
    // tick (the UI-drag detour runs on the same main thread).
    int ownerClassForHand(const unsigned int h[5]) const {
        Key k; k.t = h[0]; k.c = h[1]; k.cs = h[2]; k.i = h[3]; k.s = h[4];
        if (allSquad_.find(k) == allSquad_.end()) return 0;
        return (ownHands_.find(k) != ownHands_.end()) ? 1 : 2;
    }

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

    // Furniture occupancy sync (protocol 19, default ON): reliable enter/exit
    // edges + self-healing BODY_IN_BED/BODY_IN_CAGE state, executed engine-
    // native (setBedMode/setPrisonMode) on each machine's local pair.
    // KENSHICOOP_FURN_SYNC=0 disables.
    void setFurnSync(bool v) { furnSync_ = v; }

    // Chained/pole prisoner sync (protocol 41, default ON): rides the furniture
    // pipeline as kind=3 (Character::isChained -> setChainedMode). A captive on
    // a prisoner pole is shackled, not caged, so the cage path never saw it.
    // KENSHICOOP_CHAIN_SYNC=0 disables JUST the chain kind (beds/cages keep
    // working) - the A/B escape hatch if it ever freezes a walking slave.
    void setChainSync(bool v) { chainSync_ = v; }

    // Stealth sync (protocol 20, default ON): continuous BODY_SNEAK posture
    // apply on driven copies (engine-native setStealthMode) + the detection-
    // indicator feedback stream. KENSHICOOP_STEALTH_SYNC=0 disables.
    void setStealthSync(bool v) { stealthSync_ = v; }

    // Runtime-spawn proxy replication (protocol 21, default ON): the join
    // requests a description for any streamed hand it cannot resolve (a host
    // RUNTIME spawn) and mints a local proxy body bound at the applyTargets
    // resolve choke point. KENSHICOOP_SPAWN_SYNC=0 disables (and spawn_probe
    // forces it off to baseline the failure modes).
    void setSpawnSync(bool v) { spawnSync_ = v; }

    // BEFORE engine (protocol 21 -> 23, both clients): the describe/mint channel,
    // BIDIRECTIONAL since protocol 23 (a join RECRUIT of a runtime-born NPC mints
    // a hand the host cannot resolve). Each side both:
    //  * sends debounced PKT_SPAWN_REQ for hands applyTargets recorded as
    //    unresolved near the squad; drains PKT_SPAWN_INFO replies and mints local
    //    proxy bodies (engine::spawnProxyNpc), binding them in proxyByKey_;
    //  * drains PKT_SPAWN_REQ, resolves + describes the body
    //    (engine::describeCharacter) and replies PKT_SPAWN_INFO (throttled per
    //    hand; found=0 = negative reply, stops the peer's retries).
    void syncSpawns(GameWorld* gw, Inbound& in, NetLink& net, u32 ownerId, bool isHost);

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
    // gw is needed by the EVT_RECRUIT re-key (restoring a suppressed local body).
    void applyEvents(GameWorld* gw, Inbound& in);

    // BEFORE engine: capture the locally-owned squad and publish it (host side).
    // Also detects per-entity bodyState transitions (KO/death/revive) and queues the
    // matching reliable event on 'net'.
    void publishOwned(GameWorld* gw, NetLink& net, u32 ownerId);

    // BEFORE engine: for each owned container, capture its contents and queue a
    // reliable snapshot when the content fingerprint changed (or a periodic safety
    // resend elapsed). No-op when no owned container is registered / resolves.
    void publishInventories(GameWorld* gw, NetLink& net, u32 ownerId);

    // BEFORE engine (BOTH clients since the W1 bidir fix): scan the interest sphere for
    // free ground items WE author (peer proxies are filtered by the echo guard), assign/
    // reuse a netId per item (keyed by its local engine hand), and queue a reliable
    // snapshot for new/changed items + a reliable cull for items that vanished. A settled
    // world produces no traffic (change-detected), with a slow periodic safety resend.
    // Known carried-over W1 edge: non-gear proxies have no pickup intent - a pickup on
    // the proxy side leaves the author's real item until the author's own next scan
    // culls it (self-healing); gear rides the W2/W3 conservation intents instead.
    void publishWorldItems(GameWorld* gw, NetLink& net, u32 ownerId);

    // AFTER engine (BOTH clients): drain received world-item snapshots/culls and
    // reconcile local proxies - spawn a proxy for a new (ownerId, netId), move it if it
    // changed, destroy it on cull. netId spaces are per-sender, culls owner-scoped.
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

    // BEFORE engine, after publishInventories (protocol 37, BOTH clients): detect a
    // COMPLETED cross-owner drag. Every tracked container (own + received) keeps a
    // per-item-total baseline; a scan (~2.5 Hz) that finds a LOSS in one container
    // and the matching GAIN in another - stable for a settle window, at least one
    // end peer-authored - is a direct UI trade the single-writer snapshots cannot
    // represent. Author ONE reliable PKT_INV_XFER (src+dst+item+qty), latch the
    // pending transfer so applyInventories cannot reconcile it back while the
    // owner's snapshots are still stale, and (for gear) suppress the W2 weapon-
    // census drop/pickup reading of the same count edges. Own<->own moves and
    // unpaired diffs are folded into the baseline (own snapshots already carry
    // them; a lone loss is a drop/consume, not a trade).
    void detectAndPublishTransfers(GameWorld* gw, NetLink& net, u32 ownerId);

    // AFTER engine (protocol 37): drain received transfer intents and relocate the
    // REAL Item* between our own copies of the two containers (conservation - no
    // fabrication, no destruction, so gear survives; non-gear falls back to
    // fabricate-into-dst if our src copy is missing). Idempotent by
    // (ownerId, xferId); skips intents we authored. Runs BEFORE applyInventories
    // so the relocation beats the reconcile.
    void applyTransfers(GameWorld* gw, Inbound& in, u32 localId);

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

    // AFTER publishOwned (protocol 22, both clients): stream the WALLET
    // (Ownerships::money) of every squad tab this client OWNS, keyed by tab
    // RANK - change-gated on the reliable channel with a ~1 Hz floor and a
    // periodic safety resend (the publishStats pacing). Kenshi's wallet is
    // per-Platoon; nothing else about money is on the wire (shop_probe).
    void publishMoney(GameWorld* gw, NetLink& net, u32 ownerId);

    // BEFORE engine (protocol 22): drain received wallet snapshots and write
    // each PEER-owned tab's money onto our local copy of that tab's platoon
    // via Ownerships::setMoney. Never writes a rank we own.
    void applyMoney(GameWorld* gw, Inbound& in);

    // Per-tab wallet sync master enable (KENSHICOOP_MONEY_SYNC).
    void setMoneySync(bool v) { moneySync_ = v; }

    // BEFORE engine (protocol 23, both clients): drain the engine's recruit-edge
    // queue (the PlayerInterface::recruit detour records every successful LOCAL
    // recruit) and author one reliable EVT_RECRUIT per edge (subject = the OLD
    // hand, actor = the NEW hand). Also pins each recruited hand into
    // pinOwned_ so publishOwned streams it even when the engine parked it
    // in a tab RANK the partition says the peer owns (recruit_probe: a join
    // recruit landed in the host's rank-0 container).
    void publishRecruits(GameWorld* gw, NetLink& net, u32 ownerId);

    // Recruitment sync master enable (KENSHICOOP_RECRUIT_SYNC).
    void setRecruitSync(bool v) { recruitSync_ = v; }

    // BEFORE engine (protocol 35, both clients): poll the roster's pointer ->
    // hand baseline (~2 Hz; the Character* survives a squad-tab move, only its
    // hand changes - squad_probe) and author one reliable EVT_SQUAD_MOVE per
    // detected edge (subject = OLD hand, actor = NEW hand; zeroed actor = the
    // body left the roster). Pins the new hand into pinOwned_ BEFORE the wire
    // so publishOwned keeps streaming the moved member no matter which tab
    // rank its new container latched to. The receive half (shared EVT_RECRUIT
    // re-key) lives in applyEvents.
    void publishSquadMoves(GameWorld* gw, NetLink& net, u32 ownerId);

    // Squad management sync master enable (KENSHICOOP_SQUAD_SYNC). Also gates
    // the container-rank LATCH: with it on, tab ranks are assigned once at
    // first census and newly-seen containers APPEND (a mid-session tab can
    // never reshuffle existing ranks / flip whole-tab ownership); off = the
    // legacy per-tick sorted ranking (the squad_probe baseline).
    void setSquadSync(bool v) { squadSync_ = v; }

    // BEFORE engine (protocol 24, both clients): sample the player-faction
    // relation table (~1 Hz, immediately when the affectRelations detour saw a
    // mutation) and stream every row whose value MOVED since the seeded
    // baseline (change-gated reliable, per-sid safety resend). Both clients
    // run the same detector, so a change replicates from whichever engine it
    // happened on; applying a received row updates the local baseline, which
    // is what makes the channel echo-free.
    void publishFactions(GameWorld* gw, NetLink& net, u32 ownerId);

    // BEFORE engine (protocol 24): drain received relation rows and write each
    // one onto BOTH local table directions via FactionRelations::setRelation
    // (the probe showed the engine keeps them mirrored). Stale rows (per-sid
    // seq guard) and already-converged rows are skipped.
    void applyFactions(GameWorld* gw, Inbound& in);

    // Faction-relation sync master enable (KENSHICOOP_FACTION_SYNC).
    void setFactionSync(bool v) { factionSync_ = v; }

    // BEFORE engine (protocol 26, both clients): sample baked doors near the
    // interest centers (~1 Hz) and stream every row whose (open, locked)
    // moved since the seeded per-hand baseline (change-gated reliable,
    // per-hand safety resend for rows ever sent). Both clients run the same
    // detector, so a door change replicates from whichever engine it
    // happened on (a local click, an NPC, or the write applyDoors made).
    void publishDoors(GameWorld* gw, NetLink& net, u32 ownerId);

    // BEFORE engine (protocol 26): drain received door rows; each one that
    // resolves locally is applied through the engine's own door actions
    // (openDoor/closeDoor + lockDoor/unlockDoor). The baseline updates
    // BEFORE the write (echo-free); stale rows (per-hand seq guard) and
    // already-converged rows are skipped; unresolvable hands are skipped
    // silently (out-of-interest or runtime door - accepted edge).
    void applyDoors(GameWorld* gw, Inbound& in);

    // Door-state sync master enable (KENSHICOOP_DOOR_SYNC).
    void setDoorSync(bool v) { doorSync_ = v; }

    // BEFORE engine (protocol 27, both clients): drain local placement edges
    // (the placeFinalPreviewBuilding detour + programmatic scenario places)
    // into PKT_BUILD_PLACE announcements keyed by OUR hand, then sample every
    // building WE placed (~1 Hz) and stream change-gated PKT_BUILD_STATE
    // progress rows (10 s safety resend while incomplete; the final
    // complete=1 row latches the channel silent for that site).
    void publishBuilds(GameWorld* gw, NetLink& net, u32 ownerId);

    // BEFORE engine (protocol 27): drain received announcements - each new
    // key mints a local construction site through the same createBuilding
    // factory (town-rule-free, probe-proven) and enters the key -> local-hand
    // translation map (a refused mint is remembered so resends don't retry) -
    // then apply received progress rows through the engine's own
    // setConstructionProgress via that map (per-key seq guard drops stale
    // rows; unknown keys are skipped silently).
    void applyBuilds(GameWorld* gw, Inbound& in);

    // Placed-building sync master enable (KENSHICOOP_BUILD_SYNC).
    void setBuildSync(bool v) { buildSync_ = v; }

    // BEFORE engine (protocol 28, both clients): sample the doors of every
    // building in the session build maps (~1 Hz) and stream change-gated
    // PKT_BUILD_DOOR rows on the TRANSLATED identity (placer's building hand
    // + door index; own placements key by our hand, minted proxies through
    // the reverse map) - the protocol-26 symmetric door shape, so a placed
    // door replicates from whichever engine moved it.
    void publishBuildDoors(GameWorld* gw, NetLink& net, u32 ownerId);

    // BEFORE engine (protocol 28): drain received placed-door rows; resolve
    // the key through the build maps (own hand or minted local hand), read
    // door #index of that building, and apply through the engine's own door
    // actions. Baseline updates BEFORE the write (echo-free); per-key seq
    // guard; unknown/tombstoned keys skip silently.
    void applyBuildDoors(GameWorld* gw, Inbound& in);

    // Placed-building door + removal sync master enable (KENSHICOOP_BDOOR_SYNC).
    void setBdoorSync(bool v) { bdoorSync_ = v; }

    // Hunger sync enable (protocol 29, KENSHICOOP_HUNGER_SYNC): whether the
    // hunger/fed scalars ride the medical snapshot (OFF sends/applies them
    // as -1 = not carried; the rest of the stream is untouched).
    void setHungerSync(bool v) { hungerSync_ = v; }

    // BEFORE engine (protocol 33, HOST only - the world-simulation
    // authority): sample machine-class buildings near the interest centers
    // (~1 Hz) and stream a PKT_PROD row per machine - the first sight of a
    // machine SENDS (the host's state IS the baseline, unlike the symmetric
    // door channel), then change-gated on the quantized fields with a 10 s
    // safety resend (which is also what keeps a join whose own engine
    // drifted converging). Identity: baked hand (keyKind=0) or protocol-27
    // placer key through the build maps (keyKind=1).
    void publishProd(GameWorld* gw, NetLink& net, u32 ownerId);

    // BEFORE engine (protocol 33, join side): drain received machine rows;
    // resolve the key (baked hand directly; placer key through ownBuilds_/
    // peerBuilds_), read the local machine, and apply only what actually
    // diverged through the engine's own levers (switchPowerOn / direct
    // amount + farm-float writes; a still-null output buffer is first
    // MATERIALIZED via the native setProductionItem). Per-key seq guard
    // drops stale rows; unresolvable keys skip silently (out-of-interest).
    void applyProd(GameWorld* gw, Inbound& in);

    // Production machine sync master enable (KENSHICOOP_PROD_SYNC).
    void setProdSync(bool v) { prodSync_ = v; }

    // BEFORE engine (protocol 38, HOST only - the tech-tree authority):
    // sample the Research store's known set ~1 Hz (Research::isKnown over
    // the shared RESEARCH GameData enumeration) and stream one PKT_RESEARCH
    // row per known sid - first sight sends (the host's known set IS the
    // session baseline), then a safety resend covers a lost row / a join
    // whose apply lever needed prerequisites that arrived later.
    void publishResearch(GameWorld* gw, NetLink& net, u32 ownerId);

    // BEFORE engine (protocol 38, join side): drain received known-research
    // rows; sids already known locally are skipped (idempotent), the rest
    // apply through Research::startResearch - the exact lever a research-UI
    // click commits (flips isKnown in the same tick, spike 401). Per-sid
    // seq guard drops stale rows.
    void applyResearch(GameWorld* gw, Inbound& in);

    // Research tech-tree sync master enable (KENSHICOOP_RESEARCH_SYNC).
    void setResearchSync(bool v) { researchSync_ = v; }

    // Storage/machine container sync (protocol 34, KENSHICOOP_STORE_SYNC):
    // when set (HOST only - host-authoritative world containers), a ~1 Hz
    // census of container-bearing buildings (STORAGE + the machine classes)
    // in the interest spheres auto-registers each as an AUTHORED container,
    // so publishInventories streams it through the existing change-gated
    // hash+settle logic. Session-placed buildings translate to the wire via
    // the protocol-27 build maps (keyKind=1); the receiver resolves back in
    // ingestInv. Layered on invSync (publishInventories is the carrier).
    void setStoreSync(bool v) { storeSync_ = v; }

    // Connect-edge resync (protocol 30, no wire change): called from the
    // game-thread connect drain (processNetEvents) when a peer appears (host
    // on HELLO, join on WELCOME - covers late joins AND quick reconnects).
    // Re-announces every live placed building's PKT_BUILD_PLACE (+ a
    // PKT_BUILD_REMOVE for removed ones; the receiver's session maps dedupe
    // duplicates) and un-latches their STATE rows, then forces an immediate
    // resend pass over every change-gated channel cache by aging lastSendMs
    // on rows EVER SENT (each channel's own safety-resend condition fires on
    // its next sample). Rows never sent stay silent - the shared-save
    // baseline is not traffic. Edge-only caches (weaponCensus_, hostBody_)
    // are deliberately untouched: re-seeding would author phantom edges.
    void onPeerConnected(NetLink& net, u32 ownerId);

    // AFTER publishOwned (protocol 20, HOST only - the world-detection
    // authority): for every DRIVEN copy currently in stealth mode, read its
    // whoSeesMeSneaking map and stream a PKT_STEALTH snapshot back to the
    // sneaker's OWNER (~4 Hz while active, change-gated; one final empty
    // snapshot when the sneak ends / the map empties so stale arrows clear).
    void publishStealth(GameWorld* gw, NetLink& net, u32 ownerId);

    // BEFORE engine (protocol 20): drain received detection snapshots; each
    // subject we OWN gets the entries replayed between the LOCAL pair via
    // notifyICanSeeYouSneaking, so the marker arrows render natively on the
    // owner's screen. Never applied to driven copies.
    void applyStealthFeedback(GameWorld* gw, Inbound& in);

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

    // BEFORE engine, AFTER syncSpeed (protocol 25 game-clock sync):
    //  * host: broadcast the absolute in-game clock (PKT_TIME, ~1 Hz);
    //  * join: drain samples, measure offset = hostHours - localHours, and
    //    steer timeSlew_ - a multiplier the speed layer's quiet writes apply
    //    ON TOP of the arbitrated consensus speed (proportional, capped, with
    //    a disengage deadband). The slew composes with speed consensus rather
    //    than fighting its continuous enforcement; when speedSync is off the
    //    channel degrades to offset logging (no lever to compose with).
    void syncTime(GameWorld* gw, Inbound& in, NetLink& net, u32 ownerId, bool isHost);

    // Game-clock sync master enable (KENSHICOOP_TIME_SYNC).
    void setTimeSync(bool v) { timeSync_ = v; }

    // Session reset (protocol 32): called on the world-reload edge (gameplay
    // non-live -> live after a mid-session load). The old world is GONE - every
    // raw Character*/RootObject* cache is dangling and every session map
    // (builds, proxies, change-gate baselines, interp buffers) describes a
    // world that no longer exists. Clears all of it back to as-if-freshly-
    // launched state while PRESERVING: the config gates (set* levers), the
    // ownership partition (ownRanks_), and every OUTBOUND sequence counter
    // (facSeqOut_, doorSeqOut_, buildSeqOut_, bdoorSeqOut_, prodSeqOut_,
    // speedSeqOut_, timeSeqOut_, nextEventId_/netId/dropId/pickupId/treatId)
    // - a peer that
    // did NOT reload keeps its per-sender stale-row guards, so restarting a
    // counter would make every new row look stale to it.
    void resetSession();

    // Peer-leave cleanup (Phase 2 crash hardening): called from the transport
    // leave edge when the OTHER player disconnects mid-session (distinct from a
    // world-reload). resetSession() clears proxyByKey_ but never DESTROYS the
    // minted bodies, and the leave handler previously did neither - so after a
    // peer drop the survivor kept its minted proxies standing AND kept driving
    // them off stale maps (the "join crash -> host follow-on crash" chain). This
    // despawns every minted proxy body FIRST (SEH-guarded), then resetSession()
    // to clear the maps that referenced them. Safe if reconnect follows: the new
    // session re-censuses and re-mints from scratch.
    void clearPeerReplicationState(GameWorld* gw);

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

    // HOST, ~1 Hz (protocol 36): publish the wide-radius NPC existence census
    // (hands only) so the join can cull local-only ghosts far beyond the
    // positional stream bubble. No-op unless streamNpcs_ and censusRadius_ > 0.
    void publishNpcCensus(GameWorld* gw, NetLink& net, u32 ownerId);

    // JOIN: drain received censuses into the latest-wins existence set
    // (censusHands_) consumed by enforceHostAuthority's wide-radius pass.
    void applyNpcCensus(Inbound& in);

    // Camera hint channel (protocol 43, camera-anchored interest):
    //  * join: read the local camera center (engine::cameraCenter) and send
    //    it to the host at ~1 Hz (PKT_CAM_HINT, unreliable latest-wins);
    //  * host: drain received hints into peerCam_/peerCamMs_ and publish the
    //    fresh hint to the engine layer (engine::setPeerCamHint) so
    //    interestCenters can anchor an extra sphere on it. Both sides also
    //    publish their LOCAL camera as an anchor (never crosses the wire).
    void syncCamHint(GameWorld* gw, Inbound& in, NetLink& net, u32 ownerId, bool isHost);

    // KENSHICOOP_CENSUS_RADIUS: wide-radius existence culling reach (units);
    // <= 0 disables the census channel on both sides.
    void setCensusRadius(float r) { censusRadius_ = r; }

    // KENSHICOOP_SPAWN_MINT_RADIUS (2026-07-11 "NPCs spawn on top of the join
    // player" fix): how far from our own squad a census-missing host NPC may
    // be PROXY-MINTED. The census-missing scan asks about every census hand
    // with no local body; the host's reply carries the authoritative position
    // and the mint happens only inside this radius (the duplicate guard: a
    // baked NPC in a loaded block always resolves, so a resolve-fail inside
    // render range is a genuine host runtime spawn). <= 0 restores the legacy
    // stream-bubble-only minting.
    void setSpawnMintRadius(float r) { spawnMintRadius_ = r; }

    // KENSHICOOP_CENSUS_PARK (v38 pack-hidden fix): how far a census-PRESENT
    // local copy may drift from the host's census position before the join
    // parks it back onto the host's spot. <= 0 disables parking (existence-
    // only census, the v37 behavior).
    void setCensusParkDist(float d) { censusParkDist_ = d; }

    // KENSHICOOP_CENSUS_FREEZE_AI (default ON): the join freezes the local AI
    // of a census-band body (census-present, unstreamed) that DIVERGES past
    // censusParkDist_ - the position park teleports it back, but its local AI
    // kept re-deciding to flee/fight, so a captive/working slave ran and
    // aggroed the join's guards while the host had it working. Divergence-
    // gated: well-tracking census NPCs never trip it and keep their local AI.
    void setCensusFreezeAi(bool v) { censusFreezeAi_ = v; }

    // travel_parity worldstate rows: when enabled, both sides dump one
    // "SCENARIO WNPC hand=.. pos=.. cls=.. name=.." row per enumerated world
    // NPC every ~5 s - the host from its census walk (cls=host), the join
    // from the existence audit with each NPC's authority class (drv/cen/hid/
    // ghost) - so Test-TravelParity can cross-compare the two worlds while
    // the players travel. Off by default (log volume).
    void setAuditRows(bool on) { auditRows_ = on; }

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
        unsigned int taskRetries;    // far-fixture (r=3) attempts (interp-lag retry)
        unsigned long taskNoneTick;  // first tick of the current sustained host->NONE
                                     //   streak (0 = not in one); after TASK_CLEAR_MS
                                     //   of continuous NONE the held pose is released
                                     //   (debounced job-removal detector - mirrors
                                     //   carryNoSeeTick/furnNoSeeTick)
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
        unsigned long combatSnapCount;// cumulative combat hard-snaps on this hand (Phase 1 warp
                                      //   diagnosis): a body that keeps snapping is PERSISTENTLY
                                      //   divergent (wrong target / wrong place), not occasionally
                                      //   churning - surfaced as n= in the [combat] snap line and
                                      //   as maxPersist in the [combat] stats rollup
        unsigned long combatOverTick; // Phase 1 hysteresis (2026-07-16 smoothness pass): first
                                      //   tick the drift went OVER the snap band this spell
                                      //   (0 = under). A copy only INSTANT-teleports after the
                                      //   drift PERSISTS COMBAT_CONVERGE_MS on a moving source;
                                      //   a momentary interp/footwork spike fast-slides back
                                      //   instead of warping
        unsigned long npcSnapTick;    // locomotion-path hard snap cooldown (Phase 2): a far
                                      //   NPC whose teleport can't stick (seat AI, unloaded
                                      //   terrain) walk-converges between snaps instead of
                                      //   teleporting every frame
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
        // Furniture occupancy sync (protocol 19):
        unsigned long furnHealTick;   // last self-heal enter attempt (throttle)
        unsigned long furnNoSeeTick;  // first tick a locally-occupying copy's stream
                                      //   stopped reporting the occupancy bit (the
                                      //   debounced owner-side-exit detector)
        // Third-party placement (protocol 36): last time the host authored a
        // PEER-ENTER for this peer-owned driven body (re-author throttle).
        unsigned long furnPeerTick;
        // Chained/pole prisoner (protocol 41): the OWNER hand last seen for this
        // body while it was locally chained, so a lost/late reliable ENTER (or
        // an AI break-out) can be self-healed by re-applying setChainedMode -
        // the continuous BODY_CHAINED bit carries no owner (unlike a cage, whose
        // building the self-heal re-finds by name near the streamed position).
        unsigned int  chainOwner[5];
        bool          haveChainOwner;
        // Phase 6b (protocol 42): last non-owner unlock-guard re-assert attempt
        // (throttle). A caged prisoner (streamKind=2) is also shackled, but the
        // furniture kind priority hides the chained state from the occupancy
        // self-heal; this guard re-asserts setChainedMode independently so a peer
        // PC's local lockpick can't leave the owner's prisoner unlocked on the peer.
        unsigned long chainHealTick;
        // Stealth sync (protocol 20):
        unsigned long sneakTick;      // last setStealthMode apply (mode-flap throttle)
        // Velocity-aware snap gate (2026-07-11): slow-decaying peak of the
        // source's wall-clock speed. The instantaneous 2-sample estimate
        // collapses to ~0 the moment the source halts, deflating the snap
        // gate while the driven body is still converging on the stop point.
        float        velPeak;
        // Walk/rest debounce (2026-07-11 choppiness fix): last tick a sample
        // showed genuine translation; the walking verdict holds for a fixed
        // time after it so mid-walk velocity dips can't flap the classifier.
        unsigned long moveSeenMs;
        // The classifier's previous verdict, so genuine walk->rest
        // transitions can be counted (restFlipNpc_) as a flap-rate signal.
        bool         wasMoving;
        // Per-hand smoothness attribution (Phase 2 diagnosis): cumulative
        // active/zero-step frames this body contributed to the oracle, so a
        // zeroFrac regression can name its worst offenders in the stat line.
        unsigned long zeroF;
        unsigned long activeF;
        // Last tick this body's stream cadence classified MID-tier. A body
        // that just handed off mid -> near (raid walking into the 20 Hz
        // bubble) may owe one large reconciliation snap for divergence
        // accrued under sparse mid coverage - classed to the mid ledger
        // (like young-ring coverage snaps), not steady-state near tracking.
        unsigned long midSeenMs;
        Driven() : fresh(false), haveActual(false), lx(0), ly(0), lz(0), parked(false),
                   haveDest(false), dx(0), dy(0), dz(0),
                   suppressed(false), lastSeenMs(0),
                   issuedTask(TASK_NONE), taskApplied(false), taskBad(false),
                   taskTick(0), taskRetries(0), taskNoneTick(0), detached(false), downApplied(false),
                   koLatched(false), deathLatched(false),
                   combatArmed(false), combatTick(0), combatOrders(0),
                   combatTgtIdx(0), combatTgtSer(0),
                   combatSeenTick(0), combatSnapTick(0), combatSnapCount(0),
                   combatOverTick(0), npcSnapTick(0),
                   goalsCleared(false),
                   trusted(false), agreeStreak(0),
                   carryHealTick(0), carryNoSeeTick(0),
                   furnHealTick(0), furnNoSeeTick(0), furnPeerTick(0),
                   haveChainOwner(false), chainHealTick(0),
                   sneakTick(0), velPeak(0.0f), moveSeenMs(0), wasMoving(false),
                   zeroF(0), activeF(0), midSeenMs(0) {
            chainOwner[0] = chainOwner[1] = chainOwner[2] = chainOwner[3] = chainOwner[4] = 0;
        }
    };

    // Reproduce the host's rest pose on a driven body: if it carries a task whose
    // fixture resolves locally, commit it ONCE (seated/idle at the same object);
    // otherwise quiet the AI and park at the host transform. Drift-guarded; re-arms
    // when the host task changes. Used by both the NPC and squad at-rest branches.
    // isSquad: player-squad members are NEVER town-AI-detached (the detach
    // re-containers the body OUT of its squad tab - a new hand - which breaks
    // every hand-keyed protocol on a squad member; observed on bed_pose).
    void applyRest(Character* c, Driven& d, const EntityState& out,
                   bool haveActual, float ax, float ay, float az, unsigned long now,
                   bool isSquad);

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
        // Furniture occupancy (protocol 19): last published occupancy of this
        // entity (0 none / 1 bed / 2 cage) + the furniture's hand (captured
        // from the local Character on the ENTER edge), so publishOwned can
        // emit the reliable enter/exit edge exactly once per transition and
        // still name the furniture on EXIT.
        int furnKind; unsigned int furn[5];
        HostBody() : bs(0), seenMs(0), carrying(false), furnKind(0) {
            carried[0] = carried[1] = carried[2] = carried[3] = carried[4] = 0;
            furn[0] = furn[1] = furn[2] = furn[3] = furn[4] = 0;
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
    // Capture-side combat telemetry: last "[combat] CAP" emit per OWNED hand
    // (throttle). Records what this client SENDS on the wire - the missing
    // half of the audit trail for join-initiated fights.
    std::map<Key, unsigned long> combatCapMs_;
    u32                   nextEventId_;
    // Stage 6: world NPCs we've hidden+frozen on the join because the host isn't
    // streaming them. Keyed by hand so we restore the exact body when it re-enters
    // the host's streamed set.
    std::map<Key, Character*> suppressed_;
    // Phase 2 (runtime-spawn sync): last time the suppressed set was re-hidden.
    // The engine can undo a one-shot hide on its own (ambush dialog/combat
    // re-tasks the body, zone streaming re-adds it to the update list), so the
    // hide is re-asserted on a ~2 s cadence while the hand stays unstreamed.
    // Lifetime guard (2026-07-11 join crash, dump: SensoryData::periodicUpdate
    // walking a freed body during zone streaming with 93 suppressed wildlife
    // booked): the ENGINE owns every suppressed body and is free to despawn it
    // at any time. The re-assert pass re-resolves each entry's hand FIRST; a
    // null/different resolution means the stored Character* is stale, and the
    // entry (plus its marker/counters) is pruned without touching the pointer.
    unsigned long authReassertMs_;
    unsigned long authPruned_;      // stale suppressed entries dropped (despawned bodies)
    // Bodies applyTargets actually drove this tick, by POINTER: a combat-driven
    // world NPC is detached into its own squad (container changes), so its local
    // enumeration key no longer matches the host's streamed key - the hand-keyed
    // `keep` set can't recognise it and enforceHostAuthority would hide+freeze a
    // body mid-fight. Pointer identity survives the re-containering.
    std::set<Character*>      drivenChars_;
    // Recently-driven grace (Phase 2 mid-band tier): last tick each body was
    // driven. drivenChars_ is per-tick, but a mid-tier body's samples arrive
    // on the round-robin cadence and a rotation hiccup empties its interp for
    // a beat - the wide authority pass must not judge it as unstreamed during
    // such a gap (run 20260712_103044: driven bodies culled mid-drive, then
    // restored by the reassert pass's driven exemption, 6 churn cycles per
    // hand). Pointers are compared, never dereferenced; timestamp-pruned.
    std::map<Character*, unsigned long> drivenSeen_;
    // Reverse identity map for CAPTURE translation (join-initiated town combat,
    // run 20260712_180913): body pointer -> the canonical key the PEER streams
    // it under. A driven body's LOCAL hand can diverge from its wire identity
    // (the engine separateIntoMyOwnSquad's a town NPC into a fresh local
    // container when a fight starts; minted proxies never had the peer's hand
    // at all), so a captured combat intent's SUBJECT must be published under
    // the canonical key or the peer's applyCombat can't resolve the target
    // (r=1 forever - the fight only renders on the attacker's client).
    // Refilled every drive tick; pruned alongside drivenSeen_.
    std::map<Character*, Key> canonicalOf_;
    // Step-5 hysteresis: consecutive-frame counters per hand so a brief stream
    // hiccup doesn't suppress (needs ~1 s unstreamed) and a boundary NPC doesn't
    // flicker back (needs ~2 s streamed dwell to restore). Spike 18: the hard
    // interest edge had no dwell band, so boundary patrollers churned.
    struct AuthCount { unsigned int unstreamed; unsigned int streamed;
                       AuthCount() : unstreamed(0), streamed(0) {} };
    std::map<Key, AuthCount>  authCount_;
    unsigned long             authSuppresses_; // churn counters (split_interest metric)
    unsigned long             authRestores_;
    // NPC existence census (protocol 36): the host's 1 Hz wide-radius hand
    // list. On the join, censusHands_ is the latest received existence set and
    // censusRecvMs_ its arrival time - enforceHostAuthority culls local NPCs
    // OUTSIDE the stream bubble that are absent from it (with the same
    // hysteresis), but only while the census is FRESH (staleness disables
    // wide culling rather than mass-suppressing on a silent host).
    float                     censusRadius_;  // 0 = census disabled
    unsigned long             censusSendMs_;  // host: last census publish
    unsigned long             censusRecvMs_;  // join: last census arrival
    // Camera hint channel (protocol 43): join sends its camera center at
    // ~1 Hz; the host keeps the latest hint + arrival stamp (stale hints are
    // dropped from the anchor set rather than pinning interest forever).
    unsigned long             camHintSendMs_; // join: last hint send
    float                     peerCam_[3];    // host: latest peer camera center
    unsigned long             peerCamMs_;     // host: its arrival time (0 = none)
    std::set<Key>             censusHands_;   // join: latest existence set
    unsigned long             censusCulls_;   // join: wide-radius suppress count
    // Phase 2 mid-band streaming tier (HOST): census-walk NPCs OUTSIDE the
    // ~200/260 u stream bubble, nearest-first, refreshed at the 1 Hz census
    // cadence. publishOwned round-robins a small slice of them through the
    // entity batch every frame (quota sized so each NPC hits the 20 Hz wire
    // at ~MID_HZ aggregate) - between census beats a far NPC keeps receiving
    // real positions instead of freezing under divergent local AI (the
    // "zombie NPC" report). Keys only (no Character*): each publish resolves
    // the hand fresh, so a despawn between census walks degrades to a skip.
    struct MidBandEntry {
        Key k; float dist;
        bool operator<(const MidBandEntry& o) const { return dist < o.dist; }
    };
    std::vector<MidBandEntry> midBand_;
    unsigned int              midCursor_;  // start of the CURRENT slice
    unsigned long             midSliceMs_; // last slice advance (50 ms cadence:
                                           // the slice must persist across a
                                           // whole net tick to be sampled)
    // v38 census position parking (pack-hidden investigation, 2026-07-11):
    // the host position per census row. A census-PRESENT NPC is exempt from
    // culling, but its two locally-simulated copies can wander arbitrarily
    // far apart - the join saw a pack somewhere the host's copy wasn't.
    // enforceHostAuthority parks a local copy that diverged past
    // censusParkDist_ back onto the host's spot (0 disables).
    struct CensusPos { float x, y, z; };
    std::map<Key, CensusPos>  censusPos_;     // join: host pos per census row
    float                     censusParkDist_;
    unsigned long             censusParks_;   // join: divergence-park count
    std::map<Key, unsigned long> parkMs_;     // join: per-key park cooldown
    // Census-band AI freeze (KENSHICOOP_CENSUS_FREEZE_AI): join-side flag +
    // per-key last-diverge tick. A census-band body that drifts past
    // censusParkDist_ is added here and AI-suspended; the hold (~5 s) keeps it
    // quiesced after the park zeroes its drift, then re-checks (release if it
    // settled, re-freeze if it diverges again).
    bool                      censusFreezeAi_;
    std::map<Key, unsigned long> censusFrozen_; // join: per-key freeze-hold tick
    bool                      auditRows_;     // travel_parity worldstate rows
    // Third-party furniture placement (protocol 36): ENTER edges detected on
    // peer-owned driven bodies in applyTargets (a guard jailing an arrested
    // player runs purely on the host sim, so the occupant's owner can never
    // author the designed occupant-owner edge). Buffered here because
    // applyTargets has no NetLink; publishOwned drains them onto the wire.
    struct PendFurnEnter { Key occ; unsigned int furn[5]; int kind; };
    std::vector<PendFurnEnter> furnPeerPend_;
    // When WE last authored an owner-side EXIT for an own hand: an in-flight
    // (5 s re-author window) PEER-ENTER must not re-jail a body its owner
    // just freed - the exit-vs-reauthor race guard.
    std::map<Key, unsigned long> ownFurnExit_;
    InterpConfig          cfg_;
    float                 catchupK_;  // walk-drive gap-proportional speed gain
    float                 snapDist_;  // moving-body hard-snap distance floor (u)
    float                 snapSeconds_; // velocity-aware snap gate: teleport only
                                        // when the body trails the newest sample
                                        // by more than this much TRAVEL TIME (the
                                        // fixed distance gate false-fired on
                                        // sprinters and any game speed > 2x)
    float                 combatSoftDist_;    // combat converge soft band (u)
    float                 combatSnapDist_;    // combat churn ceiling / slide trigger (u)
    float                 combatBigSnapDist_; // combat true-leave instant-teleport dist (u)
    float                 combatSlideMax_;    // combat fast-slide speed cap floor (u/s)
    unsigned long         combatConvergeMs_;  // combat snap hysteresis window (ms)
    bool                  sendStamp_; // index interp rings on sender stamps (v35)
    unsigned long         starveHoldMs_;  // guard-hold window past staleness
    unsigned int          starveHeldNow_; // bodies in the hold this tick (stat line)
    bool                  leaderOnly_;
    bool                  streamNpcs_;

    // Bidirectional ownership partition. ownRanks_ = the squad-TAB ranks (distinct
    // sorted hand-containers) we own; ownHands_ = the resolved owned hand keys,
    // refreshed every publishOwned. applyTargets skips any tracked hand in ownHands_
    // (never drive a body we own + simulate locally), defense-in-depth on the partition.
    std::set<unsigned int> ownRanks_;
    std::set<Key>          ownHands_;
    // Every player-squad member key (own + peer), refreshed each publishOwned from
    // the pre-ownership-filter roster. Lets the trade veto tell a squad character
    // apart from a world NPC / container (which are never blocked).
    std::set<Key>          allSquad_;

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
    // Protocol 34: the host's ~1 Hz container census result (LOCAL hands of
    // complete STORAGE/machine-class buildings in the interest spheres).
    // Folded into the authored set each publishInventories pass while
    // storeSync_ is on; per-container change-gate state lives in invPub_ as
    // usual. Refreshed wholesale each census (leaving interest just stops
    // the captures; the invPub_ baseline survives for the return).
    bool           storeSync_;
    unsigned long  contCensusMs_;
    std::set<Key>  censusContainers_;

    // Phase W1 world-item state.
    // HOST: worldTrack_ maps a ground item's LOCAL engine hand (Key) to its assigned
    //   netId + last-sent content+pos hash + last send time. nextWorldNetId_ hands out
    //   fresh ids. Items in the map but not seen this scan have left the world -> culled.
    // JOIN: worldProxies_ maps a host netId to the LOCAL proxy object we spawned for it,
    //   plus the last applied pos/hash so we only re-place it on real change.
    // stringID/itemType/quantity/quality are captured at DISCOVERY (a spatial-scan
    // row or a query-free drop-hook edge) so the item can be streamed and refreshed
    // by HANDLE resolution alone - no rescan needed. `seen` is retained only for the
    // legacy scan path; culling is now handle-based (engine::groundItemLiveness).
    // `baseline` (Phase 3 item-dup fix): a ground item present at the FIRST
    // post-load spatial scan is a SHARED save-native - both clients already hold
    // it identically, so it must never be streamed (authoring it mints a proxy
    // on top of the peer's own native = the rejoin/reload duplication). Baseline
    // tracks are recorded for identity/liveness but skipped in the emit path;
    // only items appearing AFTER the baseline (session drops, runtime spawns)
    // stream. Re-seeded after every reload via worldSeeded_ in resetSession().
    struct WorldTrack {
        u32 netId; u32 hash; unsigned long lastSendMs; float x, y, z; bool seen;
        char stringID[48]; u32 itemType; u16 quantity; u16 quality; bool baseline;
    };
    struct WorldProxy { RootObject* obj; float x, y, z; u32 hash; };
    std::map<Key, WorldTrack> worldTrack_;
    // Spawned proxies for PEER-authored ground items, keyed by (ownerId, netId):
    // W1 is bidirectional (each client streams the free ground items it authors),
    // so netId spaces are per-sender and culls are scoped to their owner.
    std::map<std::pair<u32, u32>, WorldProxy> worldProxies_;
    u32                        nextWorldNetId_;
    // First-scan-baseline latch (Phase 3): false after launch/reload; the first
    // publishWorldItems pass seeds every in-range save-native as a baseline track
    // (never-emit) and sets this true. resetSession() clears it so the reloaded
    // world (which may now contain previously-dropped-then-baked items) re-baselines
    // rather than re-streaming - closing the per-reload duplicate layering.
    bool                       worldSeeded_;

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
    // Each entry also carries the drop's GLOBALLY-SHARED identity (dropOwnerId, dropId) - both
    //   clients agree on it from the DROP packet, so a PICKUP can name the EXACT instance to
    //   re-home. This disambiguates two same-sid weapons on the ground (one dropped by each
    //   client): FIFO-by-sid would otherwise re-home the wrong copy on the peer.
    // nextPickupId_ hands out per-sender monotonic ids; appliedPickups_ dedupes received ones.
    struct GroundWeapon { u32 dropOwnerId; u32 dropId; void* item; };
    std::map<std::string, std::deque<GroundWeapon> > groundedWeapons_;
    u32                               nextPickupId_;
    std::set<std::pair<u32, u32> >    appliedPickups_;

    // Protocol 37 cross-owner transfer state.
    // xferBase_: last-known per-item totals (sid,type)->qty per tracked container - the
    //   reference the drag detector diffs against. Rebased after every mutation WE make
    //   (reconcile, transfer apply, W2/W3 relocation) so only USER actions register.
    // xferPend_: currently-observed unpaired diffs + when each first appeared (a pair of
    //   matching loss/gain older than the settle window becomes an intent; a diff that
    //   never pairs is folded back into the baseline after a timeout).
    // xferLatch_: per PEER container, the pending local delta a sent/applied transfer
    //   implies. While active, applyInventories ADJUSTS the received snapshot by it
    //   (suppressing the reconcile-back that caused the dupe/wipe); cleared when the
    //   owner's snapshot catches up to the local total, or on deadline.
    // wdSuppress_: (character key, sid) -> expiry; a gear transfer must not be read by
    //   the W2 weapon census as a ground drop / pickup of that sid on those characters.
    typedef std::pair<std::string, u32> XKey; // (stringID, itemType)
    struct XferPend  { int delta; unsigned long sinceMs; };
    struct XferLatch { int delta; unsigned long deadlineMs; };
    std::map<Key, std::map<XKey, int> >       xferBase_;
    std::map<Key, bool>                       xferSeeded_;
    std::map<Key, std::map<XKey, XferPend> >  xferPend_;
    std::map<Key, std::map<XKey, XferLatch> > xferLatch_;
    // Per peer container: when applyInventories first saw a LOCAL diff vs the
    // detector baseline (an unadjudicated user mutation - possibly one end of a
    // cross-owner drag). The reconcile DEFERS while it is younger than the defer
    // window, giving the detector time to pair the drag and author the intent
    // (otherwise the reconcile undoes the drag and the rebase erases the evidence).
    std::map<Key, unsigned long>              xferDefer_;
    u32                                       nextXferId_;
    std::set<std::pair<u32, u32> >            appliedXfers_;
    std::map<std::pair<Key, std::string>, unsigned long> wdSuppress_;
    unsigned long                             xferScanMs_; // last detector scan
    // Recapture `k`'s container and overwrite its baseline (clears its pends): call
    // after ANY local mutation we make ourselves so the detector only sees the user.
    void xferRebase(GameWorld* gw, const Key& k);
    // True while the transfer detector is watching an unresolved LOSS of `sid` from
    // container `k` - the W2 census fallback defers its drop verdict for it.
    bool xferPendingLoss(const Key& k, const char* sid);
    bool wdSuppressed(const Key& k, const char* sid, unsigned long now);

    // Smoothness accumulators (measured from the body's actual motion while its
    // source is moving): how often did the rendered body advance per frame?
    unsigned long activeFrames_;
    unsigned long zeroWhileActive_;
    float         maxStep_;
    unsigned long slewSkipFrames_; // active frames excluded while clock-slewing

    // Interp/drive telemetry (protocol 36 jumpiness instrumentation): per-sample
    // regime counts from EntityInterp::lastMode() (EXTRAP/CLAMP = the buffer
    // starved; SEG = a source teleport crossed the ring), hard-snap and
    // walk-reissue counts from the drive layer, split squad (the other player's
    // characters - the user-facing jumpiness) vs world NPCs. Surfaced on a ~5 s
    // "[interp]" stat line + a "SCENARIO INTERP" summary in logSmoothSummary.
    unsigned long interpLerp_;
    unsigned long interpSingle_;
    unsigned long interpClampOld_;
    unsigned long interpExtrap_;
    unsigned long interpSegSnap_;
    unsigned long hardSnapSquad_;   // SNAP_DIST applyRaw on a moving squad body
    unsigned long hardSnapNpc_;     // SNAP_DIST applyRaw on a moving world NPC (near tier)
    unsigned long hardSnapMid_;     // same, MID-tier bodies (Phase 2): counted apart so
                                    //   the snap-rate gate keeps guarding the validated
                                    //   20 Hz pipeline; a newly covered mid NPC's one-time
                                    //   reconciliation snap is correct, not rubber-banding
    unsigned long walkReissueSquad_;
    unsigned long walkReissueNpc_;
    unsigned long restFlipNpc_;     // walk->rest classifier transitions (flap telemetry)
    unsigned long restFlipMid_;     // same, mid-tier bodies (kept out of the near gate)
    // Combat warp diagnosis (Phase 1): cumulative combat-drive corrections, surfaced
    // on the ~5 s "[combat] stats" line and diffed by the combat_snap_rate oracle.
    // A driven combatant is engine-driven locally (its own footwork), so position is
    // corrected in graded bands; these count each band so a session log tells warp
    // (snap) from smooth convergence (softWalk) and names WHY (wrongTgt).
    unsigned long combatSnapTotal_; // combat hard teleports (applyRaw) - the visible warp
    unsigned long combatSoftWalk_;  // combat soft walk-converge corrections (no teleport)
    unsigned long combatSlide_;     // combat FAST-SLIDE corrections: drift past the old snap
                                    //   band (COMBAT_SNAP_DIST) converged with a quick walk
                                    //   instead of a teleport (the smoothness-pass win - these
                                    //   used to all be visible warps)
    unsigned long combatOrder_;     // combat attack (re-)issues (order/target reconcile)
    unsigned long combatWrongTgt_;  // hard snaps fired while fighting the WRONG local target
    unsigned long combatLogTick_;
    unsigned long interpLogTick_;

    // Protocol 36 (wire v35): per-sender clock mapping for the batch send
    // stamp. offsetMs tracks the MINIMUM observed (arrival - sendMs) - the
    // fastest packet carries only the base path delay + clock difference, so
    // min-tracking strips queueing jitter; mapped time = sendMs + offsetMs
    // reconstructs the sender's true capture spacing in the local clock. The
    // offset creeps up slowly (~2 ms/s) so a route change that RAISES the
    // base delay re-converges instead of reading as permanent starvation.
    struct PeerClock {
        long offsetMs; bool have; unsigned long lastCreepMs;
        PeerClock() : offsetMs(0), have(false), lastCreepMs(0) {}
    };
    std::map<u32, PeerClock> peerClock_;

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
    bool                 furnSync_;
    // Chained/pole prisoner sync (protocol 41): master enable
    // (KENSHICOOP_CHAIN_SYNC). Sub-gate within the furniture pipeline.
    bool                 chainSync_;
    // Stealth sync (protocol 20): master enable (KENSHICOOP_STEALTH_SYNC).
    bool                 stealthSync_;
    // Host-side detection-feedback publish state per DRIVEN sneaker: last sent
    // map fingerprint + send time (~4 Hz change-gated), and whether the last
    // snapshot was ACTIVE (so the end of a sneak authors exactly one clearing
    // empty snapshot).
    struct StealthPub {
        u32 hash; unsigned long lastSendMs; bool active;
        StealthPub() : hash(0), lastSendMs(0), active(false) {}
    };
    std::map<Key, StealthPub> stealthPub_;

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
    // Protocol 22: per OWNED tab rank, the last SENT wallet value + send time
    // (change gate + safety resend). A settled economy is silent.
    struct MoneyPub {
        int lastSent; unsigned long lastSendMs;
        MoneyPub() : lastSent(-1), lastSendMs(0) {}
    };
    std::map<unsigned int, MoneyPub> moneyPub_;
    bool moneySync_;
    // Protocol 24 faction-relation sync state, per faction sid.
    // known      = our current baseline (seeded on first sight, updated on every
    //              local change we sent and every received row we applied - the
    //              echo guard: an applied row is never re-detected as local).
    // lastSendVal/lastSendMs = change gate + safety resend (rows never sent
    //              never resend, so a settled diplomacy is silent).
    // seqSeen    = newest per-sender seq applied (stale-row guard).
    struct FacRow {
        float known; float lastSendVal; unsigned long lastSendMs;
        u32 seqSeen; bool seeded;
        FacRow() : known(0), lastSendVal(0), lastSendMs(0), seqSeen(0), seeded(false) {}
    };
    std::map<std::string, FacRow> facRows_;
    u32           facSeqOut_;
    unsigned long facSampleMs_;
    bool          factionSync_;
    // Protocol 26 door-state sync, per door hand (the faction shape: known =
    // baseline, updated on every local change sent AND every received row
    // applied - the echo guard; lastSendMs = change gate + safety resend;
    // seqSeen = stale-row guard).
    struct DoorRow {
        int knownOpen; int knownLocked; unsigned long lastSendMs;
        u32 seqSeen; bool seeded;
        DoorRow() : knownOpen(-1), knownLocked(-1), lastSendMs(0),
                    seqSeen(0), seeded(false) {}
    };
    std::map<Key, DoorRow> doorRows_;
    u32           doorSeqOut_;
    unsigned long doorSampleMs_;
    bool          doorSync_;
    // Protocol 27 placed-building sync. OwnBuild = a building WE placed
    // (registered from the local placement edge; we are its progress
    // authority): lastProg/lastComplete = change gate, lastSendMs = safety
    // resend, doneSent latches the channel silent once the final complete row
    // went out. PeerBuild = a building the PEER placed (learned from
    // PKT_BUILD_PLACE): the key -> local-hand translation entry; minted=0
    // remembers a refused mint so resends don't retry the factory forever;
    // seqSeen = stale STATE-row guard.
    struct OwnBuild {
        unsigned int hand[5];
        float lastProg; int lastComplete; unsigned long lastSendMs;
        bool doneSent;
        bool removed; // dismantled/destroyed + REMOVE announced: silent forever
        // Protocol 30: the PLACE announcement captured at edge-drain time, so
        // a connect-edge resync can re-announce without re-reading the engine
        // (the template sid lives in the edge, not on the Building).
        BuildPlacePacket ann;
        bool haveAnn;
        OwnBuild() : lastProg(-1.0f), lastComplete(-1), lastSendMs(0),
                     doneSent(false), removed(false), haveAnn(false) {
            memset(hand, 0, sizeof(hand));
            memset(&ann, 0, sizeof(ann));
        }
    };
    struct PeerBuild {
        unsigned int localHand[5];
        int minted; u32 seqSeen;
        bool removed; // proxy destroyed on a REMOVE: tombstone (rows skip)
        PeerBuild() : minted(0), seqSeen(0), removed(false) { memset(localHand, 0, sizeof(localHand)); }
    };
    std::map<Key, OwnBuild>  ownBuilds_;
    std::map<Key, PeerBuild> peerBuilds_;
    // Reverse translation (protocol 28): local minted building hand -> the
    // placer's wire key. Lets the door sampler express a MINTED proxy's door
    // in the placer's key space, and the protocol-26 filter recognize placed
    // doors by their parent hand.
    std::map<Key, Key> mintByLocal_;
    u32           buildSeqOut_;
    unsigned long buildSampleMs_;
    bool          buildSync_;
    // Protocol 28 placed-door rows, keyed by (placer building key, door
    // index) - the protocol-26 DoorRow shape on the translated identity.
    struct BdoorRow {
        int knownOpen; int knownLocked; unsigned long lastSendMs;
        u32 seqSeen; bool seeded;
        BdoorRow() : knownOpen(-1), knownLocked(-1), lastSendMs(0),
                     seqSeen(0), seeded(false) {}
    };
    std::map<std::pair<Key, int>, BdoorRow> bdoorRows_;
    u32           bdoorSeqOut_;
    unsigned long bdoorSampleMs_;
    bool          bdoorSync_;
    // Protocol 29: hunger fold-in enable (rides the medical snapshot).
    bool          hungerSync_;
    // Protocol 33 machine rows, keyed by the WIRE identity (keyKind, key) so
    // a baked hand can never collide with a placer key. HOST: lastSend* =
    // the change gate + safety resend (sent = the row went out at least
    // once - first sight sends, see publishProd). JOIN: seqSeen = stale-row
    // guard (the join never publishes, so the send fields stay idle).
    struct ProdRow {
        int knownPower; int knownState;
        int qOut; int qIn0; int qIn1;          // quantized amounts (x100)
        int qGrown; int qDied; int qGrowStart; int qHarv;
        unsigned long lastSendMs;
        u32 seqSeen; bool sent;
        ProdRow() : knownPower(-2), knownState(-2), qOut(-200), qIn0(-200),
                    qIn1(-200), qGrown(-200), qDied(-200), qGrowStart(-200),
                    qHarv(-200), lastSendMs(0), seqSeen(0), sent(false) {}
    };
    std::map<std::pair<int, Key>, ProdRow> prodRows_;
    u32           prodSeqOut_;
    unsigned long prodSampleMs_;
    bool          prodSync_;
    // Protocol 38 known-research rows, keyed by the RESEARCH stringID (the
    // cross-client-stable wire identity, spike 401). HOST: sent/lastSendMs =
    // first-sight send + safety resend. JOIN: seqSeen = stale-row guard,
    // applied = the local startResearch landed (isKnown flipped) so resends
    // stop re-applying.
    struct ResearchRow {
        unsigned long lastSendMs;
        u32  seqSeen;
        bool sent;
        bool applied;
        ResearchRow() : lastSendMs(0), seqSeen(0), sent(false), applied(false) {}
    };
    std::map<std::string, ResearchRow> researchRows_;
    u32           researchSeqOut_;
    unsigned long researchSampleMs_;
    bool          researchSync_;
    // Protocol 23 recruitment sync state.
    bool recruitSync_;
    // Ownership PINS (protocols 23 + 35): per-hand overrides layered on the
    // tab-rank partition. pinOwned_ = hands WE authored (our recruits, our
    // squad moves): publishOwned streams them regardless of which tab rank
    // their container maps to. pinPeer_ = hands the PEER authored (learned
    // from EVT_RECRUIT / EVT_SQUAD_MOVE): never publish them even if a local
    // census would rank them into a tab we own. This is how an appended
    // (mid-session) tab inherits its authoring side's ownership.
    std::set<Key> pinOwned_;
    std::set<Key> pinPeer_;
    // Protocol 35 squad management sync state. tabRank_ is the container ->
    // rank LATCH: seeded from the first census in sorted order (identical to
    // the legacy ranking at session start), then newly-seen containers append
    // - a mid-session tab cannot reshuffle existing ranks. Cleared on session
    // reset (the reloaded save re-seeds it). rekeyedOld_ stamps hands a
    // re-key RETIRED: their stale stream tail must never author a spawn REQ
    // or accept a mint (the duplicate-proxy race, run 192211).
    bool          squadSync_;
    std::map<std::pair<u32, u32>, unsigned int> tabRank_;
    std::map<Key, unsigned long> rekeyedOld_;
    // Fold this tick's distinct sorted containers into the latch (append-only;
    // no-op when squadSync_ is off) and rank one container: latched rank when
    // the gate is on, the legacy per-tick sorted rank otherwise.
    void latchTabs(const std::vector<std::pair<u32, u32> >& ctnrs);
    unsigned int tabRankFor(const std::pair<u32, u32>& key,
                            const std::vector<std::pair<u32, u32> >& ctnrs) const;
    // Squad-tab census for the rank-keyed channels (money, tab-leader
    // inventories): ONE representative member hand per tab rank, using the
    // same latch-aware ranking publishOwned partitions ownership by.
    unsigned int tabRepresentatives(GameWorld* gw, unsigned int rankHand[][5],
                                    unsigned int maxRanks);
    // Shared EVT_RECRUIT / EVT_SQUAD_MOVE receive half: pin the new hand as
    // peer-authored and re-key our local copy of the old hand onto it in
    // proxyByKey_ (restoring it first if host-authority had suppressed it).
    // tag selects the log prefix ("recruit" / "squad").
    void rekeyPeerBody(GameWorld* gw, const Key& oldK, const Key& newK,
                       const char* tag);
    // Phase 1b (cross-game recruit membership): insert the re-keyed body 'c'
    // into THIS client's player squad at the tab named by newK's container, so a
    // recruit/transfer shows in the panel on the PEER too. ownIt selects the
    // ownership pin for the body's ACTUAL local hand: false (default) pins it
    // PEER-owned so control stays with the author (fresh recruit / move into the
    // peer's tab); true pins it OWNED so THIS client controls + streams it (a
    // transfer INTO a tab we own - the control hand-off). Idempotent + tab-aware
    // (a squad-move re-containers an existing member). Shared by the recruit ok=1
    // path, the ok=0 post-mint drain, and the control-flip transfer.
    void insertPeerMember(GameWorld* gw, Character* c, const Key& newK,
                          const char* tag, bool ownIt = false);
    // Hard-snap attribution diagnostics (rubber-banding investigation): one
    // throttled [snap] line per applyRaw teleport with everything needed to
    // classify the cause (gap, source speed+velocity, game speed, slew,
    // whether a walk destination was live). ~4 lines/s max; skipped snaps are
    // counted into the next line.
    void logHardSnap(Character* c, const EntityState& out, const char* kind,
                     float gap, float srcVel, float gate, bool hadDest);

    // ---- Phase 3: unified entity lifecycle (join side) ---------------------
    // One explicit, logged state per hand replacing the implicit union of
    // suppressed_/proxyByKey_/drivenChars_/censusHands_ membership tests as
    // the AUDIT view of an entity's journey. The mechanics stay where they
    // were validated (authority passes, mint pipeline, drive tiers); every
    // decision point REPORTS its outcome here, so the log tells one coherent
    // story per hand ("LIFE hand=.. from=.. to=.. reason=..") and the
    // lifecycle oracle can gate on illegal journeys (e.g. a hand stuck in
    // DISCOVERED while mintable, or a body driven while culled).
    enum LifeState {
        LIFE_UNKNOWN = 0,   // never judged (or pruned after leaving interest)
        LIFE_DISCOVERED,    // known to exist (census) but NO local body yet
        LIFE_RESOLVED,      // local body bound (baked resolve / minted proxy),
                            //   not driven this tick
        LIFE_HI,            // driven at near-tier (20 Hz) cadence; PCs live
                            //   here permanently (Phase 3 unification)
        LIFE_MID,           // driven at mid-tier (round-robin) cadence
        LIFE_PARKED,        // local-AI copy under census existence (park
                            //   fallback owns divergence repair)
        LIFE_CULLED         // suppressed (hidden + frozen; census-absent ghost)
    };
    struct Lifecycle {
        u8            state;
        unsigned long sinceMs;   // when the current state was entered
        unsigned long touchMs;   // last confirmation (stale-entry pruning)
        unsigned long stuckLogMs;// last STUCK self-audit line for this hand
        Lifecycle() : state(LIFE_UNKNOWN), sinceMs(0), touchMs(0),
                      stuckLogMs(0) {}
    };
    std::map<Key, Lifecycle> life_;
    static const char* lifeName(int s);
    // Record a state observation for a hand: logs one LIFE line on CHANGE,
    // refreshes touchMs always. Returns the previous state. The UNKNOWN ->
    // PARKED edge is recorded silently (every wilderness NPC's boring birth
    // would otherwise burst hundreds of lines at session start).
    int lifeSet(const Key& k, int to, const char* reason);
    // ~5 s sweep: drop entries not touched for a while (left interest) and
    // self-audit DISCOVERED entries older than STUCK_MS whose census position
    // sits in a locally LOADED zone (mintable but never minted - the
    // "join never sees the raid the host is fighting" failure the lifecycle
    // oracle gates on; an unloaded far zone is a legitimate indefinite defer).
    void lifeSweep(GameWorld* gw, unsigned long now);
    unsigned long lifeSweepMs_;

    // Debug marker HUD labels (KENSHICOOP_DEBUG_MARKERS=1, spike-47 substrate):
    // pin a colored label to each judged body so authority states are visible
    // live - green DRV = host-driven, red HID = suppressed/culled, yellow
    // LOC = local-sim copy that exists in the host census. No-op (single env
    // check) unless the flag is set. Labels are created once per body and
    // re-captioned only on state change.
    struct DebugMarker { void* label; int color; };
    std::map<Character*, DebugMarker> debugMarkers_;
    void debugMark(Character* c, int colorId, const char* tag);
    // Lifetime guard (2026-07-11 join crash): the map is keyed by raw
    // Character* the engine can free (and REUSE for a new body, silently
    // stealing the old label). enforceHostAuthority prunes entries whose
    // pointer wasn't vouched live this pass (enumerations / driven / proxy /
    // validated suppressed); the label object is ours to destroy safely.
    void pruneDebugMarkers(const std::set<Character*>& live);
    // world_parity roster rows: one "SCENARIO WNPC" line for a body, with the
    // task/pelvis/mv parity fields appended (world_parity oracle) after the
    // legacy hand/pos/cls/name schema (travel_parity's parser ignores the
    // extras). cls=pc rows carry the player characters - the class the legacy
    // dumps EXCLUDED, which is how a diverged host-PC position stayed
    // invisible to every automated gate. Shared by the host (Publish) and
    // join (Authority) dump sites.
    void emitWnpcRow(Character* c, const EntityState& st, const char* cls);
    // world_parity PC rows: capture every playerCharacter (both tabs) and
    // emit each as a cls=pc WNPC row. Called inside the auditRows_ dumps on
    // both sides.
    void emitPcRows(GameWorld* gw);
    // v38 census position parking: snap a census-present unstreamed local
    // copy back onto the host's census position once it diverges past
    // censusParkDist_. Called from both authority passes with this tick's
    // live enumeration pointer. Returns the measured drift (u) from the host's
    // census position, or -1 if unknown (no census row / parking disabled) -
    // the census-band AI-freeze uses it to decide when a body is diverging.
    float parkDivergedCopy(Character* c, const EntityState& st, const Key& k);
    // Census-band AI freeze (KENSHICOOP_CENSUS_FREEZE_AI): suspend the local AI
    // of a census-band body whose drift crossed censusParkDist_, held ~5 s past
    // the last over-threshold tick so the position park can't oscillate it back
    // into fleeing. Divergence-gated: leaves well-tracking census NPCs alone.
    void censusFreezeDivergedAi(Character* c, const Key& k, float drift);
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
    unsigned long speedCombatHoldMs_;  // last time own-squad combat read TRUE (cap hysteresis)

    // Protocol 25 game-clock sync state. timeSlew_ is the join's correction
    // multiplier (1.0 = no correction); the speed layer applies effective *
    // timeSlew_ in its quiet writes so the slew and the consensus compose.
    bool          timeSync_;
    float         timeSlew_;          // join: current slew factor (host: always 1)
    u32           timeSeqOut_;        // host: monotonic seq for PKT_TIME
    u32           timeSeqSeen_;       // join: newest sample seq applied
    unsigned long timeLastSendMs_;    // host: last broadcast
    unsigned long timeLastLogMs_;     // join: offset-log throttle
    // The engine speed the slewed value was derived from; lets the enforcement
    // distinguish "our slewed write" from a real user click.
    float         timeSlewApplied_;   // join: last effective*slew written (-1 = none)
    // The consensus effective with the clock slew folded in (what the quiet
    // writes actually drive; clamped to the engine's sane speed range).
    float slewedEffective(float eff) const {
        if (eff <= 0.01f) return eff; // paused: never slewed
        float s = eff * timeSlew_;
        if (s > 5.0f)  s = 5.0f;
        if (s < 0.05f) s = 0.05f;
        return s;
    }

    // Protocol 21 runtime-spawn proxy replication state.
    bool spawnSync_;
    // JOIN: streamed hand -> locally-minted proxy body. Checked at the
    // applyTargets resolve choke point when engine::resolve fails, so a bound
    // proxy inherits the ENTIRE world-NPC drive path (AI-suspend, damage
    // guard, combat rendering, down/death latches). The proxy's own LOCAL
    // hand never matches the streamed key; enforceHostAuthority recognises it
    // by pointer via drivenChars_ (hide on stale stream / restore on return
    // come free from the existing hysteresis).
    std::map<Key, Character*> proxyByKey_;
    // Lifetime guard (2026-07-11 join crash): the ENGINE owns the proxy body
    // and can despawn it (zone streaming, cleanup) while proxyByKey_ still
    // holds the pointer - every later touch would be a use-after-free.
    // syncSpawns runs a ~1 s liveness sweep: SEH-read each pointer's current
    // hand and resolve it back; anything but the same pointer unbinds the
    // entry untouched.
    // JOIN: per-hand request state - debounce, retry cap, negative-reply
    // backoff (deniedMs = when the host said "can't resolve either" or the
    // local proxy spawn failed; retried only after a long cooldown).
    struct SpawnReqState {
        unsigned long lastSendMs; unsigned int sends; unsigned long deniedMs;
        // Census-mint far-deferral (2026-07-11): the host's reply resolved the
        // hand but its position was outside the mint radius. NOT a denial -
        // the NPC may be walking toward us - so re-ask on a short cadence
        // instead of the long deniedMs cooldown, and reset the send cap so a
        // slow approach can't exhaust it.
        unsigned long farMs;
        // Phase 1 spawn parity: the REQ came from the census-missing scan (the
        // hand is host-census-vouched). Census-sourced replies may FAR-MINT
        // beyond the radius gate when the reply position sits in a locally
        // LOADED zone (an unresolvable hand there is a genuine runtime spawn).
        bool fromCensus;
        // First time the census-missing scan saw this hand unresolvable. The
        // far mint requires a sustained miss (FAR_MINT_ARM_MS): a zone can
        // report loaded a few seconds before its baked bodies resolve, and
        // minting inside that window stands a short-lived double (run
        // 20260712_092921: 8/12 far mints DUPE-HEALed within 3 s). Runtime
        // spawns never resolve locally, so they pass the dwell unharmed.
        unsigned long firstMissMs;
        // Phase 1b: this REQ fulfills a force-REQ recruit/move (rekeyPeerBody
        // ok=0). It rides the fromCensus proximity bypass, but a reliable
        // EVT_RECRUIT PROVES it is a distinct host body, so it must SKIP the
        // census same-template dupe guard (which is for UNcorrelated census
        // hands). Without this the runtime recruit defers forever behind a
        // same-template twin (recruit_sync run 094401).
        bool forceReq;
        SpawnReqState() : lastSendMs(0), sends(0), deniedMs(0), farMs(0),
                          fromCensus(false), firstMissMs(0), forceReq(false) {}
    };
    std::map<Key, SpawnReqState> spawnReq_;
    // JOIN: hands applyTargets failed to resolve this tick, with the streamed
    // position (the request-authoring queue; proximity-gated in syncSpawns so
    // a far unloaded BAKED zone doesn't breed duplicate proxies). fromCensus
    // entries came from the census-missing scan instead of the stream: they
    // carry NO position (the census is a bare hand list), so the send-side
    // proximity gate is skipped and the mint decision moves to the reply
    // (which carries the host's authoritative position).
    struct UnresolvedHand {
        float x, y, z; bool fromCensus;
        // Phase 1b: force-REQ recruit/move hand (reliable-edge correlated) -
        // skips the mint-side same-template dupe guard.
        bool forceReq;
        UnresolvedHand() : x(0), y(0), z(0), fromCensus(false), forceReq(false) {}
    };
    std::map<Key, UnresolvedHand> unresolvedHands_;
    // JOIN: recruited/re-keyed hands whose rekeyPeerBody landed ok=0 (no local
    // body to bind - the recruit fired while this hand was outside our interest,
    // the interest-split "join never saw Ruka" report). A reliable EVT_RECRUIT
    // PROVES the hand is a legit host body that must appear, so these force a
    // spawn REQ regardless of the send-side proximity gate (a far recruit is
    // still streamed as a host-owned member, so it must resolve). Entries clear
    // once the hand binds a proxy or resolves to a real local body.
    std::set<Key> forceReqHands_;
    // JOIN: census-mint reach (see setSpawnMintRadius) + the census-missing
    // scan throttle (the resolve sweep over censusHands_ runs at ~0.5 Hz, not
    // per tick).
    float         spawnMintRadius_;
    unsigned long censusScanMs_;
    // Phase 0 telemetry: hands already reported unresolved (log once per hand).
    std::set<Key> spawnLogged_;
    // HOST: per-hand reply throttle (a re-request within the window is a
    // duplicate in flight, not a new question).
    std::map<Key, unsigned long> spawnReplyMs_;
    // JOIN: last "SCENARIO PROXY ..." telemetry emit (~2 Hz while proxies live).
    unsigned long spawnPosLogMs_;

    // Divergence-gated authority state (step 4).
    bool                 gateAuthority_;
    unsigned long        trustLogTick_;
    unsigned long        trustGrants_;   // trusted-mode entries this run
    unsigned long        trustRevokes_;  // trusted-mode exits (divergence/drift)
};

} // namespace coop

#endif // KENSHICOOP_REPLICATOR_H
