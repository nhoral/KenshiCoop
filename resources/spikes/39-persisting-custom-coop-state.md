# Spike 39 - Persisting custom co-op state into the save

- Type: STATIC
- Status: DONE
- Save: n/a (static SDK + cross-reference of already-resolved mod calls)
- Branch commit: <filled at commit>

## Goal

Can the mod persist its own data (peer roster, ownership/authority map, per-character
co-op flags, kill counts) so it survives save/load? Enumerate the available channels,
rank them by risk, and identify which are already runtime-reachable vs. need reversing.

## Method

Static read of `SaveManager.h`, `GameDataManager.h`, `GameData.h`, `GameSaveState.h`,
and cross-reference of `src/plugin` for what the mod already does at save/load time
(`g_saveMgrGetFn`/`g_saveMgrLoadFn` in `KenshiCoop.cpp`, the deferred-load comment, the
craft "reconstruct from save, no sidecar" pattern). No code; nothing run.

## The three persistence channels

**Channel A - native in-band "extra data" slot (`Serialisable* moreData`).**
The save/load core is `GameWorld.savedata` (a `GameDataManager`, member 0x320). Its
serialisation entry points each carry an extra-data pointer:
- `GameDataManager::save(const std::string& filename, Serialisable* moreData)` RVA 0x6BCB30
- `GameDataManager::load(filename, modName, modIndex, Serialisable* moreData, keepDeleted)` 0x6C0110
- `GameData::loadGameDataReturn(file, isActive, readOnly, Serialisable* moreData)` 0x6C1F60

So Kenshi *itself* threads a `Serialisable*` companion through the save stream - a
purpose-built hook for riding custom data alongside the save. BUT `Serialisable` is an
obfuscated/forward-declared type in this SDK (it resolves to a stripped token, not a
real definition), so its vtable/ABI is **unreversed**.

**Channel B - a custom `GameData` record / object instance.**
`GameData::saveToFile(path)` 0x6BFEF0 / `loadFromFile(path, itemType)` 0x6C3030 do
record-level file IO; `GameData::addANewInstancedObject(...)` 0x6BE5B0/0x6C2400 and the
`instances` map add object instances; `GameSaveState` carries an `itemType -> GameData*`
`states` map per object (`createState`/`addState`/`getState`/`hasState`). One could mint
a co-op `itemType` record and let the typed container persist it.

**Channel C - a sidecar file keyed off the save identity (mod-controlled).**
`SaveManager` (singleton already resolved by the mod) exposes `getCurrentGame()` 0x47A7D0,
`getSavePath()` 0x47A210, `saveExists(location,name)` 0x36C0D0, `scanGames(...)`. The mod
writes `<savePath>/<currentGame>/_coop.dat` itself. Timing: load is detectable via the
deferred `LOADGAME` signal path the mod already drives (`guardedLoadSave`,
`SaveManager::execute()` runs the real load ~2.7 s later - empirically recorded); save
can be detected by polling `getCurrentGame()` / hooking `SaveManager::save`.

## Findings

1. **Kenshi has a first-class "extra data" channel built into save/load** - a
   `Serialisable* moreData` argument on `GameDataManager::save`/`load` and
   `GameData::loadGameDataReturn`. If `Serialisable` were reversed, custom co-op state
   could be persisted *in-band* with the save (cleanest, atomic with the save). This was
   not previously known in our notes.
2. **The save core is reachable** - `GameWorld.savedata` is a `GameDataManager` at a
   known offset (0x320) and the `SaveManager` singleton is already runtime-resolved and
   driven by the mod, so neither the save manager nor the data manager is a black box at
   the *instance* level.
3. **A sidecar file is the only channel that is feasible today with no reversing.** The
   mod can locate the active save via `getSavePath()`+`getCurrentGame()` and own a
   companion file; load timing reuses the mod's existing `LOADGAME` detection. This needs
   no engine cooperation and cannot corrupt the native save.
4. **The mod currently persists nothing of its own - it reconstructs.** Existing co-op
   state (e.g. craft fixtures, reconstructable gear) is rebuilt on load by searching the
   loaded world, not read back from mod-owned storage. So spike 39 is greenfield: there
   is no prior custom-persistence mechanism to extend.

## Validation

- Channel C reachability (Findings 2-3): the `SaveManager` singleton and `load` are
  **runtime-proven** - `g_saveMgrGetFn`/`g_saveMgrLoadFn` are resolved via
  `GetRealAddress` and called in `KenshiCoop.cpp:guardedLoadSave` (:1990), and spike 5
  saved/loaded through this path. The deferred-load timing (~2.7 s) is an empirically
  recorded result (KenshiCoop.cpp:1984-1989). `getCurrentGame`/`getSavePath` RVAs are
  read from `SaveManager.h` (lines 44/47) - present and on the same proven singleton,
  but those two getters are **not yet called by the mod**, so their exact return strings
  are runtime-UNVALIDATED (see below).
- Channel A/B existence (Finding 1): the `Serialisable* moreData` params and
  `GameData::saveToFile/loadFromFile` RVAs are quoted directly from `GameDataManager.h`
  (lines 38-40) and `GameData.h` (lines 95-96). `GameWorld.savedata` offset from
  `GameWorld.h:99`. These are header facts; the channels are **not exercised**.
- Finding 4: cross-reference of `src/plugin` - the only save/load interactions are the
  SaveManager load and "reconstruct on load" comments (Engine.cpp:1080/1380,
  Replicator.cpp:70); no mod-owned write path exists.

## Open questions / hypotheses (UNVALIDATED)

- **`Serialisable` is unreversed** - Channel A's exact vtable/serialise signature is
  unknown, so in-band persistence is blocked until it is reverse-engineered. High value
  if cracked (atomic with the save, survives copy/move of the save).
- **`getCurrentGame()`/`getSavePath()` return values are unconfirmed at runtime** - need
  a host-only probe to log them and verify the sidecar target path is correct and stable
  across a save/load cycle.
- **No save-completion event is confirmed.** We can detect *load* (LOADGAME signal); the
  symmetric "save just happened" hook (to flush the sidecar atomically) is unproven -
  candidates: hook `SaveManager::save`/`saveGame`, or poll `autoSaveTimer`/`currentGame`.
- **Channel B corruption risk is unassessed** - injecting a custom `itemType` record may
  trip version/type checks (`SaveManager::checkVersion`); untested.

## Implications for co-op

- Ship co-op persistence on **Channel C (sidecar)** first: a `_coop.dat` next to the save
  keyed by `getCurrentGame()`, read on the existing load-detect, written on a save hook.
  Zero risk to the native save and no reversing needed.
- Keep **Channel A** as the long-term ideal (in-band, atomic) pending a `Serialisable`
  reversing spike - it is the "right" engine-blessed mechanism.
- Continue the **reconstruct-on-load** pattern for anything derivable from world state
  (don't persist what can be rebuilt) to keep the sidecar minimal.

## Recommended follow-ups

- Probe `SaveManager::getCurrentGame()`/`getSavePath()` at runtime (host-only DUMP): log
  both across a save+load to validate the sidecar path (closes the main open question).
- Reverse `Serialisable` (its vtable + the save buffer it writes into) to unlock the
  in-band channel.
- Find/confirm a save-completion hook (try `SaveManager::save` / `saveGame` RVA 0x374E40)
  so the sidecar can be flushed in lockstep with the native save.
