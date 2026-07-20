// EngineProbe.h - narrow PUBLIC engine surface: raw-RVA diagnostic probes
// (spike 401 research tech-tree store, spike 451 weapon-mint recipe trace, spike
// 402 native-snapshot round-trip). Carved out of Engine.h (Phase 5a domain split,
// 2026-07-19) so the probe scenarios (ScenarioProbes.cpp via ScenarioSupport.h)
// include the spike levers and the shipping replication surface stops seeing them.
//
// These are DIAGNOSTIC levers: several are unmapped-store spikes gated behind
// probe scenarios / env vars, not part of the steady-state sync path.
//
// PUBLIC header: SEH-guarded facade only, no <kenshi/...> internal headers
// (those live in the adapter, EngineInternal.h). Forward declarations only.

#ifndef KENSHICOOP_ENGINE_PROBE_H
#define KENSHICOOP_ENGINE_PROBE_H

class GameWorld;

namespace coop {
namespace engine {

// ---- Weapon-mint fabrication probes (spike 451) -----------------------------

// DIAGNOSTIC: probe whether the engine factory can instantiate WEAPON base templates at
// all (createItem returns null for the save weapon even with manufacturer+material). Tries
// the first `maxTry` weapon templates with no/man/man+mat and logs [wpndiag] success
// counts. Trials are added to cHand's inventory and immediately destroyed.
void diagWeaponCreate(GameWorld* gw, const unsigned int cHand[5], int maxTry);

// Spike 451 (weapon fabrication recipe): detour BOTH RootObjectFactory::createItem
// overloads + copyItem and log every WEAPON-template call the ENGINE itself makes -
// full args, caller RVA and result ("[mkspy] ..." lines) - so the plugin can mimic
// the engine's own weapon-mint recipe (our 6-arg call returns null for every weapon
// template; diagWeaponCreate measured 0/24). The last SUCCESSFUL engine weapon mint's
// args are captured for probeReplayWeaponMint. Install once (host side).
bool installCreateItemTraceHook();
// Replay the last captured engine weapon mint from PLUGIN context: same GameData
// pointers/level/faction, a fresh blank hand, tryAddItem into cHand's inventory.
// Returns 1 created+added / 0 nothing captured or create returned null / -1 fault.
int probeReplayWeaponMint(GameWorld* gw, const unsigned int cHand[5]);
// Phase-2 persistence check: fabricate ONE weapon LOOSE through the normal wire
// path (createItemAndAdd, spike-451 recipe, first WEAPON template + fallback
// manufacturer) into cHand's inventory, writing the template sid to outSid so the
// caller can equip it later (reequipLooseItem) and re-census for persistence.
// Returns 1 on success.
int probeFabricateWeaponLoose(GameWorld* gw, const unsigned int cHand[5],
                              char* outSid, unsigned int outLen);
// SEH-guarded (weapon_loot): deterministically pick a NOVEL weapon template - the
// first WEAPON GameData (enumeration order is gamedata-stable, so both clients pick
// the same one) that is not FISTS and whose sid is not currently in cHand's
// container. Fabricating it simulates a mid-session weapon acquisition (loot /
// vendor buy) of a weapon that exists in NO shared-save inventory. Returns 1 and
// writes the sid on success.
int commonNovelWeaponSid(GameWorld* gw, const unsigned int cHand[5],
                         char* outSid, unsigned int outLen);

// NOTE: the spike-401 research tech-tree store surface stays in Engine.h for now
// - three of those entry points (researchEnumKnown / researchQueryBySid /
// researchStartBySid) are load-bearing protocol-38 SYNC calls, so the block is
// interleaved sync+probe and migrates in a later Phase 5 increment.

// ---- Native-snapshot round-trip probe (spike 402) ---------------------------

// Spike 402: serialize the local leader through Kenshi's native
// RootObjectContainer -> GameSaveState -> GameDataContainer pipeline into an
// isolated temporary container, save it as a micro-save, then load it into a
// second isolated container. Does NOT apply the snapshot to the live object.
// Returns 1 round-trip ok / 0 unavailable or empty / -1 native save/load failed.
int probeNativeSnapshot(GameWorld* gw);

} // namespace engine
} // namespace coop

#endif // KENSHICOOP_ENGINE_PROBE_H
