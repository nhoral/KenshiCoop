# RE_Kenshi / KenshiLib API Reference

> **Purpose.** A dense, model-facing technical reference for everything this
> project can use to *control Kenshi*. It documents (a) the RE_Kenshi / KenshiLib
> plugin framework (loading, hooking, address resolution), (b) the reverse-engineered
> Kenshi engine control surface (the game classes, functions, arguments and return
> values you can call), and (c) this repo's own facade layers built on top
> (`ScenarioApi`, `Scenario`, `Protocol`, `NetClient`, `MainThreadQueue`, `CoopLog`).
>
> It is a *directory of functions*, not a tutorial. Each entry lists purpose,
> arguments and return value. Signatures are taken verbatim from the KenshiLib
> headers under `third_party/KenshiLib_deps/KenshiLib/Include/` and from this
> repo's `src/`.
>
> **Legacy note (Phase 1).** The original transport/facade stack — `NetClient`,
> `MainThreadQueue`, `ScenarioApi`, and `netproto/Protocol.h` — was **removed**.
> Sections that describe that stack are kept for historical context only. The
> live equivalents are: transport `net/NetLink`, the main-thread bridge
> `core/Inbound`, the wire protocol `netproto/Wire.h` (history in
> `resources/PROTOCOL_HISTORY.md`), and the scenario runner in
> `src/plugin/test/` (Harness build only). Canonical copy:
> [`resources/resources.md`](../resources/resources.md).

---

## Table of contents

1. [Concepts & load model](#1-concepts--load-model)
2. [Calling conventions & safety rules](#2-calling-conventions--safety-rules)
3. [KenshiLib framework API](#3-kenshilib-framework-api)
   - [Plugin entry point](#31-plugin-entry-point)
   - [Address resolution (`core/Functions.h`)](#32-address-resolution-corefunctionsh)
   - [Hooking (`core/Functions.h`)](#33-hooking-corefunctionsh)
   - [Pattern scanning (`core/Scanner.h`)](#34-pattern-scanning-corescannerh)
   - [RVA helpers (`core/RVA.h`)](#35-rva-helpers-corervah)
   - [Logging (`Debug.h`)](#36-logging-debugh)
   - [Version (`kenshi/Kenshi.h`)](#37-version-kenshikenshih)
4. [Global accessors (`kenshi/Globals.h`)](#4-global-accessors-kenshiglobalsh)
5. [Engine control surface](#5-engine-control-surface)
   - [`GameWorld`](#51-gameworld)
   - [`PlayerInterface`](#52-playerinterface)
   - [`RootObjectBase` / `RootObject`](#53-rootobjectbase--rootobject)
   - [`Character`](#54-character)
   - [`AbstractMovementBase` / `CharMovement`](#55-abstractmovementbase--charmovement)
   - [`CharBody`](#56-charbody)
   - [`Tasker`](#57-tasker)
   - [`hand`](#58-hand)
   - [`lektor<T>`](#59-lektort)
   - [`RootObjectFactory`](#510-rootobjectfactory)
   - [`SaveManager`](#511-savemanager)
   - [`GameDataManager` / `GameData`](#512-gamedatamanager--gamedata)
   - [`Faction`](#513-faction)
6. [Key enums](#6-key-enums)
7. [Proven engine-call recipes (this repo)](#7-proven-engine-call-recipes-this-repo)
8. [Project facade: `ScenarioApi`](#8-project-facade-scenarioapi)
9. [Project facade: `Scenario`](#9-project-facade-scenario)
10. [Wire protocol (`netproto/Protocol.h`)](#10-wire-protocol-netprotoprotocolh)
11. [`NetClient`](#11-netclient)
12. [`MainThreadQueue`](#12-mainthreadqueue)
13. [`CoopLog`](#13-cooplog)
14. [Runtime configuration (environment variables)](#14-runtime-configuration-environment-variables)
15. [Ogre / MyGUI / OIS — the underlying engine layer](#15-ogre--mygui--ois--the-underlying-engine-layer)
    - [Accessibility & shipping status](#151-accessibility--shipping-status)
    - [Reaching live Ogre objects from the game](#152-reaching-live-ogre-objects-from-the-game)
    - [Ogre math / value types (safe, ubiquitous)](#153-ogre-math--value-types-safe-ubiquitous)
    - [Ogre scene graph & rendering](#154-ogre-scene-graph--rendering)
    - [MyGUI (3.2.3)](#155-mygui-323)
    - [Kenshi-side GUI windows (`kenshi/gui`)](#156-kenshi-side-gui-windows-kenshigui)
    - [OIS input](#157-ois-input)

---

## 1. Concepts & load model

- **RE_Kenshi** is a Nexus mod (0.3.1+) that, among other features, loads native
  Ogre/Kenshi plugin DLLs. It reads a `RE_Kenshi.json` placed in a normal Kenshi
  mod folder and loads every DLL it lists. The JSON is just:
  `{ "Plugins" : [ "KenshiCoop.dll" ] }`.
- **KenshiLib** is the library a plugin links against. It provides the
  function-hooking API, runtime address resolution against the loaded
  `Kenshi_x64.exe`, and a large set of **reverse-engineered game class layouts**
  (the `kenshi/` headers). You include these headers to get correct struct
  offsets, vtable indices, and the runtime addresses (`RVA = 0x...`) of the
  engine's own functions.
- **Plugin lifecycle.** RE_Kenshi calls the exported `startPlugin()` once after
  the game's launcher window is dismissed and RE_Kenshi has loaded. From there a
  plugin installs hooks; the only safe place to mutate game state is on the main
  thread inside a hooked engine tick.
- **The model used in this repo:** hook `GameWorld::mainLoop_GPUSensitiveStuff`
  (the main-thread tick), reach the live `GameWorld`, and drive the game through
  resolved engine function pointers, guarded by SEH.

---

## 2. Calling conventions & safety rules

These constraints apply to *every* engine call and are the difference between
"controls Kenshi" and "crashes Kenshi":

- **x64 single calling convention.** All engine functions are effectively
  `__fastcall`. For member functions, `this` is the **first argument**.
  References in the C++ signature are passed as **pointers**; by-value PODs with
  trivial destructors (e.g. `Ogre::Vector3`, `Ogre::Quaternion`) are passed by
  value exactly as the game's ABI does.
- **Resolve non-virtual member functions with `GetRealAddress`.** Cast the
  resolved address to a hand-written `typedef ... (__fastcall*)(Self*, ...)`.
  Example: `g_spawnFn = (SpawnFn)KenshiLib::GetRealAddress(&RootObjectFactory::createRandomCharacter);`
- **`GetRealAddress` does NOT work on virtual functions.** For a virtual you
  either (a) call it normally through a live object pointer (a real virtual
  dispatch), or (b) to hook it, resolve the header's `_NV_<name>` twin, which
  shares the same RVA and is directly addressable.
- **Disambiguate overloads** with an explicit `static_cast` to the exact member
  pointer type before passing to `GetRealAddress` (see `GameWorld::destroy`,
  `PlayerInterface::recruit`, `GameDataManager::getData`, `SaveManager::load`,
  `CharBody::_NV_setCurrentAction` in this repo).
- **Main thread only.** Kenshi's engine (ancient Ogre 2.0 "Tindalos") is
  single-threaded and not thread-safe. All game mutation must happen inside the
  main-loop hook. Cross-thread data (e.g. from the net thread) must be marshalled
  through a queue (`MainThreadQueue`) and applied on the main thread.
- **Wrap every engine call in SEH** (`__try/__except`). These functions run
  deep, version-specific engine code; a defensive guard turns a bad call into a
  recoverable failure instead of a process crash. SEH frames must contain **no
  C++ objects needing unwinding** (so keep `std::string`/non-trivial locals in a
  caller frame and pass them in by pointer/reference).
- **`hand` identity is save-stable and cross-machine.** A `hand` resolves to the
  same logical entity on any machine that loaded the same save — the foundation
  for replication.

---

## 3. KenshiLib framework API

### 3.1 Plugin entry point

```cpp
__declspec(dllexport) void startPlugin();
```

- **Purpose.** The single symbol RE_Kenshi calls when the plugin loads. RE_Kenshi
  resolves it by its **C++-mangled** name (`?startPlugin@@YAXXZ`), so it must
  **not** be `extern "C"` (that would export plain `startPlugin` and RE_Kenshi
  would fail with "Could not initialize plugin"). Install hooks and resolve
  function addresses here.
- **Args / Return.** None / `void`.

### 3.2 Address resolution (`core/Functions.h`)

```cpp
namespace KenshiLib {
  KLIB_EXPORT intptr_t GetRealAddress(void* fun);
  template<typename T> intptr_t GetRealAddress(T fun);     // GetRealAddress(&Class::fn)
  template<typename T> T        GetRealFunction(T fun);    // returns a callable of the same type
}
```

- **`GetRealAddress(fun)`** — Map a KenshiLib-reconstructed symbol (member or free
  function pointer) to its real runtime address in the loaded `Kenshi_x64.exe`.
  - *Arg* `fun`: a function/member pointer, e.g. `&Character::clearAllAIGoals`.
  - *Returns*: the absolute runtime address as `intptr_t` (0 if it could not be
    resolved). **Does not work for virtual functions.**
- **`GetRealFunction(fun)`** — Convenience: same as `GetRealAddress` but returns a
  pointer already cast back to `T`, so it is directly callable.
  - *Returns*: `T` (a callable function pointer), or null on failure.

### 3.3 Hooking (`core/Functions.h`)

```cpp
namespace KenshiLib {
  enum HookStatus { SUCCESS, FAIL };
  KLIB_EXPORT HookStatus AddHook(void* target, void* detour, void** original);
  template<typename T>            HookStatus AddHook(intptr_t target, void* detour, T** original);
  template<typename T1,typename T2> HookStatus AddHook(T1* target, void* detour, T2** original);
}
```

- **`AddHook(target, detour, original)`** — Install a detour over an engine
  function (MinHook-style trampoline under the hood).
  - *Arg* `target`: address (or resolved member-pointer address) of the engine
    function to intercept. For a virtual, pass the resolved `_NV_` twin address.
  - *Arg* `detour`: pointer to your replacement function (same ABI/signature).
  - *Arg* `original`: out-param; receives a callable pointer to the original
    function so your detour can call through.
  - *Returns*: `KenshiLib::SUCCESS` or `FAIL`.
- **`HookStatus`** — `SUCCESS` / `FAIL` result enum.

### 3.4 Pattern scanning (`core/Scanner.h`)

```cpp
uintptr_t FindPatternMask(const char* byteMask, const char* checkMask,
                          uintptr_t address, size_t len, int instance);
void      GetModuleCodeRegion(const char* moduleName, uintptr_t* o_base, size_t* o_size);
uintptr_t FindPattern(const std::string& pattern, const char* moduleName, int instance);
uintptr_t FindPattern(const std::string& pattern, uintptr_t address, size_t len, int instance);
uintptr_t FollowRelativeAddress(uintptr_t adr, int trail);
```

- **`FindPatternMask`** — Scan raw memory for `byteMask` filtered by `checkMask`.
  - *Args*: `byteMask` (bytes to match), `checkMask` (which positions are
    significant, typically `x`/`?`), `address`+`len` (region to scan), `instance`
    (which match to return, 0-based). *Returns*: address of the match, or 0.
- **`GetModuleCodeRegion`** — Get the `.text` base/size of a loaded module.
  - *Args*: `moduleName`, out-pointers `o_base`/`o_size`. *Returns*: `void`.
- **`FindPattern(pattern, moduleName, instance)`** — Find an IDA-style byte
  pattern string (e.g. `"48 8B ? ? E8"`) inside a named module.
  - *Returns*: match address, or 0.
- **`FindPattern(pattern, address, len, instance)`** — Same, but over an explicit
  region. *Returns*: match address, or 0.
- **`FollowRelativeAddress(adr, trail)`** — Resolve a relative call/jump operand
  to its absolute target.
  - *Args*: `adr` (start of the instruction), `trail` (bytes of trailing operand).
  - *Returns*: absolute target address.

### 3.5 RVA helpers (`core/RVA.h`)

```cpp
class RVACore { RVACore(offset_t rva); uintptr_t GetUIntPtr() const; };
template<typename T> class RVAPtr : public RVACore {
  operator T*() const; T* operator->() const; T* GetPtr() const; const T* GetConst() const;
};
uintptr_t ProtectedDeref(uintptr_t addr);
uintptr_t ProtectedDerefRel(uintptr_t addr);
```

- **`RVACore(rva)`** — Resolve a static relative virtual address against the
  module base at runtime (SKSE/F4SE-style). `GetUIntPtr()` returns the absolute
  address.
- **`RVAPtr<T>`** — Typed pointer wrapper over an RVA; dereference/`->`/`GetPtr()`
  give a `T*` at that runtime address.
- **`ProtectedDeref(addr)`** — Safely read a pointer at `addr` (guards against a
  fault). *Returns*: the dereferenced value.
- **`ProtectedDerefRel(addr)`** — Safely follow a relative reference; `addr` is the
  start of the instruction (IDA-friendly). *Returns*: absolute target.

### 3.6 Logging (`Debug.h`)

```cpp
KLIB_EXPORT void DebugLog(const std::wstring&); // + std::string + const char* overloads
KLIB_EXPORT void ErrorLog(const std::wstring&); // + std::string + const char* overloads
KLIB_EXPORT std::string GetDebugLog();
KLIB_EXPORT std::string GetLastErrorStdStr();
std::string GetModuleName(void* address);
```

- **`DebugLog(msg)` / `ErrorLog(msg)`** — Write an info/error line to RE_Kenshi's
  debug log (`kenshi.log`). **Not thread-safe / main-thread only.** Args: message
  (`wstring`/`string`/`const char*`). Return: `void`.
- **`GetDebugLog()`** — Return the accumulated debug log text. Return: `std::string`.
- **`GetLastErrorStdStr()`** — Return WinAPI `GetLastError()` rendered as a string.
- **`GetModuleName(address)`** — Return the module that owns `address`.

### 3.7 Version (`kenshi/Kenshi.h`)

```cpp
namespace KenshiLib {
  class BinaryVersion {
    enum KenshiPlatform { GOG, STEAM, UNKNOWN };
    KenshiPlatform GetPlatform(); std::string GetPlatformStr();
    std::string GetBinaryName();  std::string GetVersion(); std::string ToString();
  };
  KLIB_EXPORT BinaryVersion GetKenshiVersion();
}
```

- **`GetKenshiVersion()`** — Identify the running build (platform + version). Use
  to gate version-specific behavior. *Returns*: a `BinaryVersion`.
- **`BinaryVersion::GetBinaryName()`** — `"Kenshi_x64.exe"` (Steam) /
  `"Kenshi_GOG_x64.exe"` (GOG). Other getters return platform/version strings.

---

## 4. Global accessors (`kenshi/Globals.h`)

The engine exposes a handful of process-global pointers (imported from the exe).
The most important is `ou`, the live `GameWorld`.

```cpp
__declspec(dllimport) GameWorld*       ou;       // THE active game world
__declspec(dllimport) GlobalConstants* con;      // tunable global constants
__declspec(dllimport) InputHandler*    key;      // input state
__declspec(dllimport) OptionsHolder*   options;  // game options
__declspec(dllimport) ForgottenGUI*    gui;      // top-level GUI

void  showErrorMessage();
float modMedicalSkill(float skill, Item* equipment, float frameTIME);
void  getFoliageRotation(FoliageSystem::EntData* data, float x, float z, Ogre::Quaternion& rotation);
GameData* getBuildingCollection(GameDataContainer& c);
void  increaseStat(float& stat, float amount, float UPPER_LIMIT);
```

- **`ou`** — Pointer to the active `GameWorld`. In this repo the `GameWorld*` is
  instead taken from the main-loop hook argument (equivalent and avoids relying on
  the import), but `ou` is the canonical global. May be null before a world loads.
- **`con` / `key` / `options` / `gui`** — Global subsystem pointers (constants,
  input, options, GUI root).
- **`increaseStat(stat, amount, UPPER_LIMIT)`** — Raise a skill/stat `stat` by
  `amount`, clamped to `UPPER_LIMIT`. *Returns*: `void`.

---

## 5. Engine control surface

> Below, each class lists the members most useful for *reading and controlling*
> the game. Inline annotations from the headers give each function's RVA and (for
> virtuals) vtable offset. Member-variable offsets are listed where a field is the
> intended access path (e.g. `Character::movement`). `_NV_<name>` twins (same RVA,
> non-virtual address) exist for nearly every virtual and are what you hook.

### 5.1 `GameWorld`

The root of a loaded game. Hold a `GameWorld*` and you can reach the player,
factory, factions, and the spatial query/AI-membership APIs.

**Key members (fields):**
- `PlayerInterface* player` (0x580) — the local player (squad, selection, camera).
- `RootObjectFactory* theFactory` (0x4A0) — spawns world objects/characters.
- `FactionManager* factionMgr` (0x4A8); `NavMesh* navmesh` (0x4B0).
- `GameDataManager gamedata` (0x20) — the active game-data DB (template lookup).
- `bool paused` (0x8B9); `ZoneManager* zoneMgr` (0x8B0).

**Spatial queries & object lifecycle:**
- `void getCharactersWithinSphere(lektor<RootObject*>& results, const Ogre::Vector3& spherePos, float farRadius, float nearRadius, float always, int maxFar, int maxNear, RootObject* skip)` — Interest query: fill `results` with characters near `spherePos`. `farRadius`/`nearRadius`/`always` are tiered distances; `maxFar`/`maxNear` cap counts; `skip` is excluded. Return: `void`. *(This repo's NPC interest engine.)*
- `void getObjectsWithinSphere(lektor<RootObject*>& results, const Ogre::Vector3& spherePos, float radius, itemType type, int maxNumber, RootObject* skip)` — Like above but for any `itemType` within `radius`.
- `void getObjectsWithinBox(lektor<RootObject*>& results, const Ogre::Vector3& pos, const Ogre::Vector3& size, const Ogre::Quaternion& rot, itemType type, int maxNumber, RootObject* skip)` — Box-shaped query.
- `bool destroy(RootObject* obj, bool justUnloaded, const char* debugInfo)` — Engine removal of a dynamic object. `justUnloaded=false` for true destruction; `debugInfo` is a log tag. Return: success. **After this the pointer is dead.** *(Overloaded; cast to this exact signature to resolve.)*
- `void destroy(...)` overloads exist for `MovableObject*`, `AttachedEntity*`, `TownBuildingsManager*`, `NestBatcher*`, `GameData*`.
- `bool findValidSpawnPos(Ogre::Vector3& pos, const Ogre::Vector3& centerArea)` — Find a navmesh-valid spawn point near `centerArea`; writes `pos`. Return: success.
- `bool fixNaNPosition(Ogre::Vector3& pos) const` — Repair a NaN position in place.

**AI update-list membership (suppress/restore local simulation):**
- `void removeFromUpdateListMain(Character* character)` — Pull a character out of the engine's main AI update so it stops simulating locally (you then drive it). Return: `void`.
- `void addToUpdateListMain(Character* character)` — Re-enroll a character into local AI. Return: `void`.
- `const ogre_unordered_set<Character*>::type& getCharacterUpdateList() const` — The current AI update set.

**Pause / speed / camera:**
- `void togglePause(bool on)`; `void userPause(bool p)`; `bool isPaused() const`.
- `void setGameSpeed(float speed, bool click)`; `void setFrameSpeedMultiplier(float m)`; `float getFrameSpeedMultiplier() const`.
- `const Ogre::Vector3 getCameraCenter() const`; `const Ogre::Vector3 getCameraPos() const`.

**Messaging / time:**
- `void showPlayerAMessage(const std::string& message, bool queued)` — On-screen message to the player. (`_withLog` / `D` variants also exist.)
- `void playNotification(const char* sound) const` — Play a notification sound.
- `double getTimeStamp()`; `TimeOfDay getTimeStamp_inGameHours()`; `float getLengthOfHourInRealSeconds()`.

**Main tick (hook target):**
- `virtual void mainLoop_GPUSensitiveStuff(float time)` / `void _NV_mainLoop_GPUSensitiveStuff(float time)` (RVA 0x7877A0) — The per-frame main-thread tick. **Hook the `_NV_` address.** `time` is the frame delta.

### 5.2 `PlayerInterface`

The local player's controller: squad roster, selection, camera, orders. Reach via
`gw->player`.

**Squad / characters:**
- `lektor<Character*> playerCharacters` (0x2B0) — the player's controllable squad. `playerCharacters[0]` is the primary/leader character used as the local-player position source.
- `const lektor<Character*>& getAllPlayerCharacters() const` — the squad roster.
- `void getAllPlayerCharacters(lektor<RootObject*>& list)` — fill a list with the squad.
- `Character* getAnyPlayerCharacter() const` — any squad member, or null.
- `bool recruit(Character* character, bool editor)` — enlist `character` into the player squad. `editor=false` for normal play. Return: success. *(Overloaded; a `lektor<Character*>` variant also exists — cast to disambiguate.)*
- `Faction* getFaction() const`; `void setFaction(Faction* f)` — the player's faction.
- `ActivePlatoon* createSquad()` — make a new empty squad.

**Selection & camera:**
- `void selectObject(RootObject* obj, bool modifier)`; `void unselectAll()`; `void selectAll()`.
- `void selectPlayerCharacter(int index, bool modifier, bool track)`.
- `void getAllSelectedObjects(lektor<RootObject*>& out, itemType type)`.
- `Character* getNearestCharacterTo(const Ogre::Vector3& pos)` / `getNearestSelectedCharacterTo(...)`.
- `void focusCamera(const Ogre::Vector3& pos)`; `void startTrackCharacter(RootObject* target)`; `void stopTrackCharacter()`.

**Orders (issue tasks to selected characters):**
- `void newPlayerTaskSelectedCharacters(TaskType t, const hand& targetH, Building* destinationIndoors, const Ogre::Vector3& clickpos, bool addDontClear)` — issue a player task to the current selection.
- `void addOrderSelectedCharacters(Building* destinationIndoors, TaskType task, RootObject* subject, bool shift, bool addDontClear, const Ogre::Vector3& location)`.
- `void addJobSelectedCharacters(TaskType task, RootObject* subject, bool shift, bool add, const Ogre::Vector3& location)`; `void removeJobSelectedCharacters(TaskType t)`.
- `void setOrderSelectedCharacters(MessageForB::StandingOrder order)`.
- `void stopCharactersMovement()`.

### 5.3 `RootObjectBase` / `RootObject`

The common base of all world objects (characters, buildings, items). Provides the
universal read accessors.

**`RootObjectBase` (key virtuals + fields):**
- `virtual Ogre::Vector3 getPosition()` (vt 0x40) — world position.
- `virtual std::string getName() const` (vt 0x8) / `virtual void setName(const std::string& name)` (vt 0x10).
- `virtual GameData* getGameData() const` (vt 0x18) — the object's template/definition.
- `virtual itemType getDataType() const` (vt 0x20) — the object's type tag (e.g. `CHARACTER`). Use to safely downcast query results.
- `virtual Faction* getFaction() const` (vt 0x58); `bool hasFaction() const`.
- `const hand& getHandle() const` — the object's save-stable `hand`.
- Fields: `Faction* owner` (0x10), `GameData* data` (0x40), `Ogre::Vector3 pos` (0x48), `hand handle` (0x58).

**`RootObject` (adds):**
- `virtual Ogre::Quaternion getOrientation() const` (vt 0xC0) — world orientation; `getOrientation().getYaw().valueRadians()` is the heading used on the wire.
- `RootObjectContainer* container` — the object's container; for a player character this *is* its active-squad container (what spawn borrows to enlist into the squad).
- `virtual void select()` / `unselect()`; `virtual void setVisible(bool)` / `getVisible()`.
- `virtual bool giveItem(Item*, bool dropOnFail, bool destroyOnFail)`; `virtual Inventory* getInventory() const`.

### 5.4 `Character`

A living entity (`extends RootObject`). The richest control surface. Many methods
are virtual — call through the object; hook via the `_NV_` twin.

**Transform / read:**
- `virtual Ogre::Vector3 getPosition()` (vt, RVA 0x5CDF00) — live world position.
- `Ogre::Vector3 _getRawPosition() const`; `Ogre::Vector3 getRawEntityPosition()`.
- `virtual float getMovementSpeed() const`; `virtual Ogre::Vector3 getMovementDirection() const`.
- `Ogre::Vector3 getPredictedPosition(float secondsInFuture)`.
- `Ogre::Vector3 getBoneWorldPosition(const std::string& name)`.

**Movement / teleport (high-level; prefer `CharMovement` for precise control):**
- `void teleport(const Ogre::Vector3& moveBy, const Ogre::Quaternion& rot)` / `void teleport(const Ogre::Vector3& moveBy)` — relative teleport (by offset).
- `void relocationTeleport(const Ogre::Vector3& moveBy)`; `void teleportVisuallyOnly(const Ogre::Vector3& to, const Ogre::Quaternion& rot)`.
- `void setDestination(const Ogre::Vector3& pos, bool shift)` — order a walk to `pos`.
- `void lookatPosition(const Ogre::Vector3& v, bool fullbodyFacing)`.
- `CharMovement* getMovement()` / field `CharMovement* movement` (0x640) — the authoritative locomotion/Havok controller (see §5.5). This repo positions ghosts through `movement` because it owns the real position (writing elsewhere gets snapped back).

**AI / tasks / orders:**
- `void clearAllAIGoals()` — drop autonomous goals (quiet the character). Return: `void`.
- `void addGoal(TaskType t, RootObjectBase* subject)`; `void addJob(TaskType t, RootObject* subject, bool shift, bool addDontClear, const Ogre::Vector3& location)`; `void removeJob(TaskType t)`.
- `void addOrder(Building* dest, TaskType t, RootObject* subject, bool shift, bool clear, const Ogre::Vector3& location)`.
- `void reThinkCurrentAIAction()`; `void endCombatMode()`.
- `void setStandingOrder(MessageForB::StandingOrder orderID, bool on)`; `bool getStandingOrder(...) const`.
- `CharBody* getBody()` / field `CharBody* body` (0x648) — the task/animation body (see §5.6).
- `AI* getAI()` / field `AI* ai` (0x650).

**Squad / faction / platoon:**
- `Platoon* separateIntoMyOwnSquad(bool permanent)` — eject from current squad into its own (used to remove a spawned body from the player's controllable party). Return: the new `Platoon*` (often ignored).
- `void setSquadMemberType(SquadMemberType memType)`; `int squadMemberID` (0x418).
- `virtual void setFaction(Faction* p, ActivePlatoon* a)`; `RaceData* getRace() const`; `virtual void setRace(GameData* r)`.
- `ActivePlatoon* getPlatoon() const`; `Character* getSquadLeader()`; `bool isPlayerCharacter() const`.

**Combat / state / health:**
- `void attackTarget(Character* who)`; `hand getAttackTarget() const`; `bool isInCombatMode(bool melee, bool ranged) const`.
- `void healCompletely()`; `bool isDead() const`; `virtual bool isUnconcious() const`; `bool isDown()`.
- `void ragdollMode(bool on, RagdollPart::Enum part)`; `bool isRagdoll() const`; `virtual void setProneState(ProneState p)`.
- `void declareDead()`; `MedicalSystem* getMedical()`; `CharStats* getStats()`; `CombatClass* getCombatClass() const`.

**Inventory / items / money:**
- `virtual bool giveItem(Item* item, bool dropOnFail, bool destroyOnFail)`; `Inventory* inventory` (0x2E8).
- `virtual void equipItem(const std::string& sectionName, Item* item)` / `unequipItem(...)`.
- `virtual int getMoney() const`; `virtual bool takeMoney(int n)`.

**Misc control:**
- `virtual void setName(const std::string& name)`; `void setNameTagVisible(bool value)`.
- `virtual void say(const std::string& s)`; `void sayALine(const std::string& line, bool force)`.
- `void runSlaveAnim(const std::string& anim, float speed, float sync)` — play a named animation clip (this repo hooks it to learn clip↔task mapping). `endSlaveAnim(name)` ends it.
- `void setStealthMode(bool on)`; `bool isStealthMode() const`.
- `const hand& handle` / `virtual void setHandle(const hand& h)` — the save-stable identity.

### 5.5 `AbstractMovementBase` / `CharMovement`

The locomotion controller (`Character::movement`). `CharMovement` extends
`AbstractMovementBase`; most control methods are virtual and called directly. This
is the **authoritative position owner** — write here for teleports that stick.

**Positioning / teleport:**
- `virtual void _setPositionDirectionAndTeleport(const Ogre::Vector3& position, const Ogre::Quaternion& orientation)` (RVA 0x65E260) — snap to an exact pose. The repo's precise teleport primitive.
- `virtual void _setPositionAndTeleport(const Ogre::Vector3& p, int floor)`; `virtual void _setPositionSimple(const Ogre::Vector3& p)`.
- `void teleportCollisionHull(const Ogre::Vector3& _pos)`.

**Destinations / speed:**
- `virtual void setDestination(const Ogre::Vector3& dest, UpdatePriority priority, bool notVertical)` (RVA 0x660AF0) — order locomotion to `dest`. Overloads target a `Character*`, `Building*`, or `RootObjectBase*`.
- `virtual void setDesiredSpeed(MoveSpeed speed)` (+ `float` overload); `void setDesiredSpeedOrders(MoveSpeed)`; `void restoreDesiredSpeed()`.
- `void setMaxSpeed(float ms)`; `float getMaxSpeed() const`; `void setStandardWalkSpeed(float s)`.
- `virtual void halt()` (RVA 0x65F4F0) — stop locomotion **and reset the animation phase** (calling every frame freezes the body on frame 0 — use once to settle, then teleport without halting).
- `void invalidatePath()`; `virtual void manualMovement(const Ogre::Vector3& v)`; `void setDirectMovement(const Ogre::Vector3& d, float limit)`; `void setMovementMode(MovementMode mode)`.
- `virtual void faceDirection(const Ogre::Vector3& dir)`; `virtual void lookatPosition(const Ogre::Vector3& pos)`.

**State read (these drive animation selection):**
- `bool currentlyMoving` (0x24); `float currentSpeed` (0xB8); `float desiredSpeed` (0xBC); `Ogre::Vector3 currentMotion` (0xA8). The engine's `AnimationClass` picks walk/idle/run from these — mirroring them onto a remote copy makes its locomotion clip match the source.
- `bool isCurrentlyMoving() const`; `float getCurrentSpeed() const`; `Ogre::Vector3 getCurrentMotion() const`; `virtual bool isDestinationReached() const`; `virtual bool isIdle() const`.
- `Ogre::Vector3 predictNextPosition(bool accurate)`; `bool isRunning()`.
- `Character* getCharacter()`; `int getCurrentFloor() const`; `bool isIndoors() const`.

### 5.6 `CharBody`

The task/animation body (`Character::body`). Owns the **current action** — this is
how you reproduce another machine's pose (sit/operate at the same fixture).

- `virtual bool setCurrentAction(TaskType t, RootObject* target)` / `_NV_setCurrentAction(TaskType, RootObject*)` (RVA 0x5C5F00) — commit the body to task `t` targeting `target`. The repo's pose-reproduction primitive. **Passing a null `target` for a target-needing task makes the engine auto-pick a nearby fixture and walk there** — only commit when the subject resolves locally. *(Overloaded with a `Tasker*` form — cast to disambiguate.)*
- `Tasker* getCurrentAction()` / field `Tasker* currentAction` (0x68) — the active task object (read its `key()` to learn the `TaskType`).
- `void endAction()`; `bool isIdle() const`; `hand getCurrentSubject() const`; `hand target` (0x28).
- `Character* getCharacter()`; `Faction* getFaction() const`; `Platoon* getPlatoon() const`.

### 5.7 `Tasker`

A single AI task instance (what a `CharBody::currentAction` points at).

- `TaskType key() const` (RVA 0x2680D0) — the task's type (e.g. `SIT_AROUND`, `OPERATE_MACHINERY`). The repo streams this to reproduce poses. Return: `TaskType`.
- `hand subject` (0x10) — the object the task targets (the fixture). Streamed alongside `key()`.
- `Ogre::Vector3 getLocation() const` / `void setLocation(const Ogre::Vector3& loc)`.
- `hand getNextSubTarget(AI* ai)`; `const std::string& getDescription() const`; `bool needsSubjectOrLocation() const`.
- `taskPriority priority` (0x8); `TaskData* taskData` (0x70); `const TaskData* const getTaskData() const`.

Supporting types in the same header: `TaskData` (static per-task definition:
requirements/results/permajob info), the `StateType` enum (AI world-state
predicates), and `taskPriority` / `PermajobType` enums.

### 5.8 `hand`

A 20-byte, **save-stable, cross-machine composite key** identifying any world
object. The cornerstone of replication: the same `hand` resolves to the same
logical entity on every machine that loaded the same save.

**Fields:** `itemType type` (0x8), `unsigned container` (0xC),
`unsigned containerSerial` (0x10), `unsigned index` (0x14), `unsigned serial` (0x18).

**Construction (resolve via `GetRealAddress`, build in a zeroed scratch buffer so
the layout is valid):**
- `hand(unsigned index, unsigned serial, itemType type, unsigned container, unsigned containerSerial)` (RVA 0xCCFF0) — build from raw fields (the repo's wire→hand path). Note the **argument order**: index, serial, type, container, containerSerial.
- `hand(RootObjectBase* from)`; `hand(GameData* fromLoadedState, itemType typ)`; `hand()` (null).

**Resolution (handle → live pointer; returns null if not loaded here):**
- `Character* getCharacter() const` (RVA 0x796E30).
- `RootObject* getRootObject() const`; `RootObjectBase* getRootObjectBase() const`.
- `Building* getBuilding() const`; `Item* getItem() const`; `Platoon* getPlatoon() const`; `ActivePlatoon* getActivePlatoon() const`; `TownBase* getTown() const`.

**Utility:**
- `std::string toString() const` / `void fromString(const std::string& str)` — serialize/parse.
- `bool isNull() const`; `bool isValid() const`; `void setNull()`; `operator bool() const`.
- `bool operator<(const hand&) const`; `operator==` / `operator!=`; `bool squadMatch(const hand&) const`.
- `static const hand NULL_HAND`.

### 5.9 `lektor<T>`

The engine's lightweight vector (used for all query out-params and the squad
roster). Header-only reconstruction.

- `T& operator[](uint32_t i)` / `T& at(uint32_t i)` — element access (asserts in-range).
- `uint32_t size() const` (field `count`) — element count.
- `uint32_t capacity() const` (field `maxSize`).
- `void clear()` — reset count to 0 (reuse the buffer across queries to avoid reallocations).
- `iterator begin()/end()`; `bool valid() const`.
- Raw fields: `uint32_t count`, `uint32_t maxSize`, `T* stuff`.

### 5.10 `RootObjectFactory`

Creates world objects/characters (`gw->theFactory`). The spawn entry points are
the repo's "make a body" primitives.

- `RootObject* createRandomCharacter(Faction* faction, Ogre::Vector3 position, RootObjectContainer* owner, GameData* characterTemplate, Building* home, float age)` (RVA 0x582F60) — spawn a character. `owner` may be **null** (the factory makes a free body); passing the player's squad container enlists it into the player squad. `characterTemplate` may be the player's own `GameData`. Return: the new `RootObject*` (cast to `Character*`), or null.
- `RootObjectBase* create(GameData* data, Ogre::Vector3 position, bool isFromActiveLevelMod, Faction* owner, Ogre::Quaternion rotation, FactoryCallbackInterface* callbackObject, RootObjectContainer* certainContainer, GameSaveState* state, bool invisible, Building* homeBuilding, float age)` — general object create.
- `Building* createBuilding(...)`; `LocationNode* createLocationNode(...)`.
- `Item* createItem(GameData* gd, const hand& handle, GameData* weaponMesh, GameData* matData, int levelOverride, Faction* flagUniform)` / `Item* createItem(GameData* itemState)`; `Item* copyItem(Item* from)`.
- `Platoon* createRandomSquad(...)` — spawn a whole AI squad.
- `void mainThreadUpdate()` — flush the factory's deferred create list (runs on the main thread).

### 5.11 `SaveManager`

Save/load orchestration (singleton). Used here to auto-load a save from the title
screen.

- `static SaveManager* getSingleton()` (RVA 0x37D7E0) — the singleton (may be null before the save subsystem is up).
- `void load(const std::string& name)` (RVA 0x47AD00) — **deferred** load by save folder name. Sets the `LOADGAME` signal and returns immediately; the engine's `execute()` performs the load a few frames later. **Must only be issued after the menu/save subsystem has settled**, else the deferred load crashes. *(Overloaded with `load(const SaveInfo&, bool)` — cast to disambiguate.)*
- `void newGame(const std::string& startId)`; `void save(const std::string& s, bool autosave)`; `void import(const SaveInfo& s, int flags)`.
- `bool savesExist()` (RVA 0x36B160) — readiness/existence probe (use to confirm the subsystem is up before auto-loading).
- `bool saveExists(const std::string& location, const std::string& name)`.
- `int scanGames(lektor<SaveInfo>& list, bool loadDetails)`; `bool loadInfo(SaveInfo& info)`; `bool checkVersion(const SaveInfo& info)`.
- `const std::string& getCurrentGame()`; `const std::string& getSavePath() const`.
- `enum Flags { RESET_POSITION, IMPORT_SQUAD, IMPORT_BUILDINGS, IMPORT_RESEARCH, IMPORT_NPC_STATES, IMPORT_RELATIONS }`; `enum Signal { SAVEGAME, LOADGAME, IMPORTGAME, NEWGAME }`.

### 5.12 `GameDataManager` / `GameData`

`GameDataManager` is the game-data database (`gw->gamedata`). `GameData` is a
single definition/template (a race, item, character, building type, etc.).

- `GameData* getData(const std::string& sid, itemType category)` (RVA 0x6BF420) — look up a definition by its **string id** filtered by `category` (e.g. `CHARACTER`). The repo's template lookup. *(Overloaded with `getData(sid)` and `getData(int id)` — cast to disambiguate.)* Return: the `GameData*` or null.
- `GameData* getData(const std::string& sid)` — lookup by string id, any type.
- `GameData* getData(int id)` — lookup by numeric id.
- `GameData* getDataByName(const std::string& dataName, itemType category)` — by display name.
- `void getDataOfType(lektor<GameData*>& list, itemType type)` — enumerate all of a type.

A `GameData*` obtained from `Character::getGameData()` is a guaranteed-valid
template for that character (the repo uses the player's own template as a spawn
fallback).

### 5.13 `Faction`

A faction (`Character::getFaction()` / `gw->player->getFaction()`). Constructed
from a name; large surface for relations, towns, and squads. Most control flows
take a `Faction*` you already have (the player's). Key entry points include
faction relations, town/squad management, and AI player config; for spawning you
generally pass the player's faction straight through to
`createRandomCharacter`.

---

## 6. Key enums

From `kenshi/Enums.h` and related headers. Use the symbolic names (ordinals vary
across versions).

- **`MoveSpeed`** `{ WALK, JOG, RUN, GROUPED, NO_SPEED_CHANGE }` — locomotion speed band for `setDesiredSpeed`.
- **`UpdatePriority`** `{ LOW_PRIORITY, MED_PRIORITY, HIGH_PRIORITY }` — `setDestination` priority.
- **`TaskType`** — large enum of AI tasks. Frequently relevant values: `IDLE`, `HOLD_POSITION`, `STAND_STILL`, `STAND_AT_NODE`, `STAND_AT_SHOPKEEPER_NODE`, `WANDER_TOWN`, `OPERATE_MACHINERY`, `REST`, `SIT_AROUND`, `RELAX_IN_TOWN_PACKAGE`, `SIT_ON_THRONE`, `GO_TO_THE_BAR_AND_DRINK`, `USE_BED`, `USE_BED_ORDER`, `SLEEP_ON_FLOOR`, `MELEE_ATTACK`, `CHASE`, `RUN_AWAY`, `FOLLOW_PLAYER_ORDER`, `MOVE_CUS_ORDERED`, `NULL_TASK`. *(This repo uses `STAND_STILL` as a self-contained idle for at-rest replicated NPCs.)*
- **`itemType`** — the data-type tag returned by `getDataType()`; `CHARACTER` is the value used to filter characters out of spatial query results, and is the `category` passed to `getData` for character templates.
- **`SquadMemberType`** `{ SQUAD_1, SQUAD_2, SQUAD_LEADER, SQUAD_SIGNALS_PLAN, SQUAD_SLAVE }`.
- **`ProneState`** `{ PS_NORMAL, PS_STAYING_LOW, PS_CRIPPLED, PS_PLAYING_DEAD, PS_KO }`.
- **`RagdollPart::Enum`** `{ NONE, WHOLE, RIGHT_ARM, LEFT_ARM, HEAD, RIGHT_LEG, LEFT_LEG, CARRY_MODE, ARMS, LEGS, ALL }`.
- **`MovementMode`** `{ MOVE_NORMAL, MOVE_COMBAT, MOVE_DIRECTION }`.
- **`SaveManager::Flags`** / **`SaveManager::Signal`** — see §5.11.
- **`taskPriority`** `{ TP_JUST_ACTION, TP_FLUFF, TP_NON_URGENT, TP_URGENT, TP_OBEDIENCE, TP_MAX_SIZE }`.

---

## 7. Proven engine-call recipes (this repo)

These are the exact resolved function pointers and SEH-guarded call shapes the
plugin uses today — the *practical, verified* "what is allowed" for controlling
Kenshi. All are resolved once in `resolveGhostFns()` / `resolveSaveFns()` and
called only on the main thread inside `__try/__except`.

| Capability | Resolved member | Typedef'd call shape |
|---|---|---|
| Spawn a character | `&RootObjectFactory::createRandomCharacter` | `RootObject* (__fastcall*)(RootObjectFactory*, Faction*, Ogre::Vector3, RootObjectContainer*, GameData*, Building*, float)` |
| Quiet AI goals | `&Character::clearAllAIGoals` | `void (__fastcall*)(Character*)` |
| Eject to own squad | `&Character::separateIntoMyOwnSquad` | `void* (__fastcall*)(Character*, bool)` |
| Destroy an object | `&GameWorld::destroy` *(cast to `(RootObject*,bool,const char*)`)* | `bool (__fastcall*)(GameWorld*, RootObject*, bool, const char*)` |
| Interest query | `&GameWorld::getCharactersWithinSphere` | `void (__fastcall*)(GameWorld*, lektor<RootObject*>*, const Ogre::Vector3*, float, float, float, int, int, RootObject*)` |
| Suppress local AI | `&GameWorld::removeFromUpdateListMain` | `void (__fastcall*)(GameWorld*, Character*)` |
| Restore local AI | `&GameWorld::addToUpdateListMain` | `void (__fastcall*)(GameWorld*, Character*)` |
| Resolve hand→char | `&hand::getCharacter` | `Character* (__fastcall*)(const hand*)` |
| Build a hand | `&hand::_CONSTRUCTOR` *(5-arg overload)* | `hand* (__fastcall*)(hand*, unsigned, unsigned, itemType, unsigned, unsigned)` |
| Read current task | `&Tasker::key` | `int (__fastcall*)(const void*)` |
| Resolve hand→object | `&hand::getRootObject` | `RootObject* (__fastcall*)(const hand*)` |
| Commit a task/pose | `&CharBody::_NV_setCurrentAction` *(cast to `(TaskType,RootObject*)`)* | `bool (__fastcall*)(void* charBody, int taskType, RootObject*)` |
| Recruit to squad | `&PlayerInterface::recruit` *(cast to `(Character*,bool)`)* | `bool (__fastcall*)(PlayerInterface*, Character*, bool)` |
| Lookup template | `&GameDataManager::getData` *(cast to `(const std::string&, itemType)`)* | `GameData* (__fastcall*)(GameDataManager*, const std::string*, itemType)` |
| Save singleton | `&SaveManager::getSingleton` | `SaveManager* (__fastcall*)()` |
| Load a save | `&SaveManager::load` *(cast to `(const std::string&)`)* | `void (__fastcall*)(SaveManager*, const std::string*)` |
| Saves exist? | `&SaveManager::savesExist` | `bool (__fastcall*)(SaveManager*)` |

**Called directly through live objects (virtual dispatch, no resolve needed):**
- `Character::getPosition()`, `Character::getOrientation().getYaw().valueRadians()`.
- `Character::movement` → `CharMovement::_setPositionDirectionAndTeleport(pos, rot)`, `::halt()`, `::setDestination(dest, HIGH_PRIORITY, false)`, `::setDesiredSpeed(RUN)`.
- `CharMovement` fields `currentlyMoving` / `currentSpeed` / `desiredSpeed` / `currentMotion` (read for capture, write to mirror locomotion/animation).
- `Character::body` → `CharBody::currentAction` → `Tasker::subject` (capture the task subject).
- `Character::handle` (the 5-field `hand` read for identity).

**Hooks installed (via `AddHook` on resolved `_NV_` addresses):**
- `GameWorld::_NV_mainLoop_GPUSensitiveStuff` — the per-frame main-thread tick.
- `TitleScreen::_NV_update` — safe point to trigger an auto-load (only when `KENSHICOOP_SAVE` is set).
- `Character::runSlaveAnim` — discovery hook to harvest clip↔task names.

---

## 8. Project facade: `ScenarioApi`

`src/plugin/ScenarioApi.h` — namespace `coop`. The thin, crash-safe action facade
scenarios use to touch the game. Every function null-checks and returns a failure
value rather than throwing; all positions are **Ogre world coordinates**;
everything runs main-thread + SEH-guarded.

- `void scenarioLog(const char* msg)` — Emit one preformatted log line through `coopLog` (lands in both the coop log and `kenshi.log`).
- `int remotePlayerCount()` — Number of remote players currently tracked (>0 means a peer is in-game and sending; use as a connection handshake).
- `Character* localPlayer(GameWorld* gw)` — The first player character, or 0.
- `Faction* playerFaction(GameWorld* gw)` — The player's faction, or 0.
- `RootObjectContainer* playerSquadContainer(GameWorld* gw)` — The active squad container, or 0.
- `GameData* playerTemplate(GameWorld* gw)` — The player's own `GameData` template, or 0.
- `GameData* lookupTemplate(GameWorld* gw, const char* stringId)` — Look up a template by string id; falls back to the player's template if not found.
- `int playerSquadSize(GameWorld* gw)` — Squad member count, or -1 on fault.
- `Character* spawnIntoPlayerSquad(GameWorld* gw, GameData* tmpl, const Ogre::Vector3& pos)` — Spawn a character from `tmpl` (or the player's template if `tmpl==0`) at `pos` and enlist it into the player squad. Return: the new `Character*` or 0.
- `bool getCharPos(Character* c, Ogre::Vector3* out)` — Read a character's live position. Return: false on fault.
- `bool getCharHand(Character* c, u32 out[5])` — Read a character's save-stable hand as `{index, serial, type, container, containerSerial}`. Return: false on fault.
- `bool teleportChar(Character* c, const Ogre::Vector3& pos, float headingRad)` — Snap to an exact transform and stop (halt + teleport). Return: false on fault.
- `bool moveCharTo(Character* c, const Ogre::Vector3& dest)` — Order a walk to an absolute destination (engine locomotion). Return: false on fault.
- `void clearCharGoals(Character* c)` — Clear autonomous AI goals (best-effort).
- `void despawnChar(GameWorld* gw, Character* c)` — Remove a spawned character (best-effort). The pointer is dead afterward.

## 9. Project facade: `Scenario`

`src/plugin/Scenario.h` — namespace `coop`. A scenario is a deterministic,
time-gated state machine run after the auto-loaded save reaches gameplay. Both
clients run the same object (host drives, join observes); the test runner compares
logs.

- `struct ScenarioContext { GameWorld* gw; bool isHost; u32 localId; DWORD elapsedMs; unsigned tick; }` — per-frame context passed to a scenario (`gw` is never null while running; `elapsedMs` is wall-clock since `onStart`; `isHost` selects authoritative vs observer behavior).
- `class Scenario` (abstract):
  - `virtual const char* name() const = 0` — stable id (must match `KENSHICOOP_SCENARIO`).
  - `virtual void onStart(const ScenarioContext& ctx) = 0` — called once on the first in-game frame.
  - `virtual bool onTick(const ScenarioContext& ctx) = 0` — called each frame; return `true` when complete (harness logs `SCENARIO RESULT` and exits).
  - `virtual bool passed() const = 0` — final in-plugin verdict (queried once after `onTick` returns true).
- `Scenario* makeScenario(const std::string& name)` — Registry factory; construct a scenario by name or return 0 if unknown. Caller owns the result (freed at process exit). Registered scenarios: `squad_spawn_sync`, `squad_move_sync`.

Scenarios touch the game **only** through the `ScenarioApi` facade (never the
engine directly) so all mutation stays SEH-guarded on the main thread.

## 10. Wire protocol (`netproto/Protocol.h`)

> **Note:** this section documents the original milestone protocol. The **live
> protocol is `src/netproto/Wire.h`** (`PROTOCOL_VERSION = 36`, checked during
> handshake), which extends the same conventions (plain C++03, little-endian,
> packed structs, `packetType`/`readPacket` helpers) with the full packet set:
> inventory/equipment sync, cross-owner transfer intents (`PKT_INV_XFER`),
> world items (bidirectional), medical/stats/stealth, buildings and production,
> money/faction/time/speed, coordinated save/load streaming, and NPC census.
> `Wire.h` is heavily commented per-packet and is the source of truth;
> `src/prototest/main.cpp` asserts every struct size and round-trip.

namespace `coop`. Plain C++03, little-endian, packed structs sent as raw bytes.
`PROTOCOL_VERSION = 5` (checked during handshake).

**Types:** `u8`, `u16`, `u32`, `f32`.

**`enum PacketType`** (first byte of every packet): `PKT_HELLO=1`, `PKT_WELCOME=2`,
`PKT_PLAYER_STATE=3`, `PKT_PING=4`, `PKT_PONG=5`, `PKT_PLAYER_LEFT=6`,
`PKT_NPC_STATE=7`, `PKT_SQUAD_STATE=8`. Sentinel `PLAYER_ID_ALL = 0xFFFFFFFF`.

**Packets:**
- `HelloPacket { u8 type; u16 version; u8 nameLen; /* name[nameLen] */ }` — client→host on connect.
- `WelcomePacket { u8 type; u32 playerId }` — host→client; assigns a player id.
- `PlayerStatePacket { u8 type; u32 playerId; u32 tick; f32 x,y,z; f32 heading }` — a player's transform for a tick (presence heartbeat).
- `PingPacket { u8 type; u32 nonce }` — liveness / RTT (`PKT_PING`/`PKT_PONG`).
- `NpcStateEntry` (75 bytes) — one replicated entity: 5-field `hand` (`htype, hcontainer, hcontainerSerial, hindex, hserial`), transform (`x,y,z,heading`), current `task` (`u16`, `NPC_TASK_NONE=0xFFFF` if none) + 5-field subject hand (`stype, scontainer, scontainerSerial, sindex, sserial`), and locomotion state (`cspeed, cmotionX/Y/Z, cmoving`).
- `NpcBatchHeader { u8 type; u8 count }` — NPC batch: `[header][NpcStateEntry*count]`. `NPC_BATCH_MAX = 18` per datagram.
- `SquadBatchHeader { u8 type; u32 ownerId; u8 count }` — owner-tagged squad batch (bidirectional; each peer streams its own squad).

**Helpers:**
- `u8 packetType(const void* data, unsigned len)` — first byte (type tag), or 0 if empty.
- `template<typename T> bool readPacket(const void* data, unsigned len, T* out)` — typed read; returns true and fills `out` if the buffer is large enough.

## 11. `NetClient`

`src/plugin/NetClient.h` — namespace `coop`. Owns ENet on a dedicated background
thread. The net thread exclusively owns the `ENetHost`; inbound data is handed to
the main thread via `MainThreadQueue`; the main thread publishes outbound state.

- `bool startHost(int port, MainThreadQueue* inbound)` — Listen as host on `port`; inbound states go to `inbound`. Return: success.
- `bool startClient(const std::string& ip, int port, MainThreadQueue* inbound)` — Connect to `ip:port`. Return: success.
- `void stop()` — Signal the net thread to stop and join it.
- `void setLocalState(const PlayerStatePacket& p)` — **Main thread**: publish this client's transform for the net thread to transmit. Thread-safe.
- `void setNpcStates(const NpcStateEntry* arr, unsigned count)` — **Main thread (host)**: publish the nearby-NPC batch to broadcast. Thread-safe (copies under lock).
- `void setSquadStates(u32 ownerId, const NpcStateEntry* arr, unsigned count)` — **Main thread (both peers)**: publish this client's OWN squad members, tagged `ownerId`. Bidirectional. Thread-safe.
- `bool isRunning() const` — Whether the net thread is live.
- `u32 localId() const` — This client's assigned network player id.

## 12. `MainThreadQueue`

`src/plugin/MainThreadQueue.h` — namespace `coop`. The thread bridge: the net
thread pushes copied PODs; the main-thread tick drains them at a safe point
(Win32 `CRITICAL_SECTION`, no `std::mutex` for VS2010).

- `struct OwnedNpcState { u32 ownerId; NpcStateEntry e; }` — a received squad member plus its owner's network id.
- `void push(const PlayerStatePacket& p)` — *Net thread*: enqueue a player state.
- `void drain(std::deque<PlayerStatePacket>& out)` — *Main thread*: move all pending player states into `out` (swapped; leaves the queue empty).
- `void pushNpc(const NpcStateEntry& e)` — *Net thread*: enqueue an NPC transform.
- `void drainNpc(std::deque<NpcStateEntry>& out)` — *Main thread*: drain NPC transforms.
- `void pushSquad(u32 ownerId, const NpcStateEntry& e)` — *Net thread*: enqueue an owner-tagged squad member.
- `void drainSquad(std::deque<OwnedNpcState>& out)` — *Main thread*: drain squad members.

## 13. `CoopLog`

`src/plugin/CoopLog.h` — namespace `coop`. A tiny thread-safe file logger
(timestamped, per-line-flushed, host/join tagged) separate from `kenshi.log` so an
automated runner can parse it. Guards a `FILE*` with a `CRITICAL_SECTION` so the
net thread can write too.

- `void logInit(const char* path, const char* modeTag)` — Open the log at `path` (truncating any prior run) and remember a short tag (`"HOST"`/`"JOIN"`). Call once at load.
- `void logLine(const char* msg)` — Append one timestamped/tagged INFO line and flush. Thread-safe.
- `void logErrLine(const char* msg)` — Same, as an ERROR line.
- `void logClose()` — Flush and close (called right before `TerminateProcess` on test self-exit).

> In the plugin, `coopLog()` / `coopErr()` wrappers call **both** `CoopLog` and
> KenshiLib's `DebugLog`/`ErrorLog`, so events land in both logs.

## 14. Runtime configuration (environment variables)

Read once in `startPlugin()`; no recompile needed to change role/behavior.

- `KENSHICOOP_MODE` — `host` (default) or `join`.
- `KENSHICOOP_IP` — host IP when joining (default `127.0.0.1`).
- `KENSHICOOP_PORT` — UDP port (default `27800`).
- `KENSHICOOP_SAVE` — existing save folder name to auto-load from the title screen (default empty = manual). Auto-loading a non-existent save crashes the game.
- `KENSHICOOP_AUTOLOAD_DELAY_MS` — settle time before issuing the deferred load (default `5000`).
- `KENSHICOOP_TEST_SECONDS` — if >0, self-exit (via `TerminateProcess`) this many seconds after gameplay starts (default `0` = never). Used by the test runner.
- `KENSHICOOP_LOG` — path to the dedicated coop log (default `KenshiCoop_host.log` / `KenshiCoop_join.log`).
- `KENSHICOOP_SCENARIO` — name of a compiled scenario to run after load (default empty = normal co-op tick).
- `KENSHICOOP_AUTOSPAWN` — host-only manual-validation helper: spawn N distinct-hand units into the squad once after gameplay settles.
- `KENSHICOOP_OWN_INDICES` — squad ownership partition for the inhabit model: `""` = own all (default); `"0"` = own only index 0; `"~0"` = own all except 0; `"1,3"` = own indices 1 and 3.

---

## 15. Ogre / MyGUI / OIS — the underlying engine layer

**Short answer: yes, all three are accessible and worth documenting.** Kenshi is
built on **Ogre 2.0 "Tindalos"** (a custom Ogre 2.x fork), with **MyGUI 3.2.3**
for its UI and **OIS** for raw input. KenshiLib ships the *complete upstream
headers* for all three under `third_party/KenshiLib_deps/KenshiLib/Include/`:
`ogre/` (270 headers), `mygui/` (182 headers), and `ois/`. Unlike the `kenshi/`
headers (which are reverse-engineered struct reconstructions), these are the
**genuine library headers** — full class declarations, doc comments, and inline
implementations.

### 15.1 Accessibility & shipping status

- **Headers:** included on the compile path already (`KenshiCoop.vcxproj` adds
  `$(KENSHILIB_DIR)/Include`). You can `#include <ogre/OgreSceneNode.h>`,
  `<mygui/MyGUI.h>`, `<ois/OIS.h>` today.
- **Linking:** `OgreMain_x64.lib` is linked, so Ogre exported symbols (singletons,
  scene/resource managers, math out-of-line helpers) are **directly callable**.
  `MyGUIEngine_x64.lib` is shipped in the deps but **not currently linked** — add
  it to `<AdditionalDependencies>` to call MyGUI functions directly (otherwise you
  can still read MyGUI types reached through the game's GUI objects). OIS is
  exercised through Kenshi's own input layer rather than linked directly.
- **ABI caveat (important).** The shipped headers are **stock upstream** Ogre 2.0 /
  MyGUI 3.2.3. Kenshi ships a *modified* build of these libraries, so while the
  public **API surface** matches, exact **struct layouts / vtable orders / private
  behavior may diverge** from stock. Practical guidance:
  - **Math / value types are safe** — they are header-inline PODs with stable
    layouts (`Vector3`, `Quaternion`, etc.). Use freely.
  - For **heavyweight managers/singletons and scene-graph mutation**, prefer
    reaching the **live objects the game hands you** (see §15.2) and treat
    constructing/owning Ogre resources yourself as higher-risk. When in doubt,
    trust the `kenshi/` reconstructions (authoritative for the *modified* engine)
    over assumptions about stock Ogre internals.
- **Thread-safety:** the same main-thread-only rule applies. Scene-graph and GUI
  mutation must happen on the render/main thread (inside the main-loop hook), never
  the net thread.

### 15.2 Reaching live Ogre objects from the game

The Kenshi structures expose live Ogre pointers — the safest way in:

- **`CameraClass`** (`gw->player->getCamera()`): `Ogre::Camera* camera` (0x68),
  `Ogre::SceneNode* getCameraNode()`, `Ogre::SceneNode* getCenterNode()`,
  `Ogre::SceneNode* node`/`center`. The whole player camera rig as Ogre objects.
- **`ResourceLoader`**: `Ogre::SceneManager* sceneManager` (0x98) — the active
  scene manager (create nodes/lights, run scene queries through it).
- **`GameWorld::render`** — `RendererT*`, the engine's renderer wrapper (owns the
  render window / scene setup).
- **Ogre singletons** (via `OgreMain_x64.lib`): `Ogre::Root::getSingletonPtr()`,
  then `Root::getSceneManager(name)`, `Root::getAutoCreatedWindow()`, and the
  resource singletons (`MeshManager`, `MaterialManager`, `TextureManager`,
  `ResourceGroupManager`) — each an `Ogre::Singleton<T>` with
  `getSingleton()`/`getSingletonPtr()`.

### 15.3 Ogre math / value types (safe, ubiquitous)

These are the types that appear all over the `kenshi/` API (positions,
orientations, bounds). Header-inline, no linking needed, layout-stable.

- **`Ogre::Vector3` / `Vector2` / `Vector4`** — `Real x,y,z[,w]`. Full operator set,
  `length()`, `squaredLength()`, `normalise()`, `dotProduct()`, `crossProduct()`,
  `distance()`, `Vector3::ZERO` / `UNIT_X/Y/Z`. The world-coordinate workhorse.
- **`Ogre::Quaternion`** — orientation. `Quaternion(const Radian& angle, const Vector3& axis)`,
  `getYaw()/getPitch()/getRoll()` → `Radian`, `*` (compose/rotate), `Slerp()`,
  `Quaternion::IDENTITY`. Heading on the wire is `getOrientation().getYaw().valueRadians()`.
- **`Ogre::Radian` / `Ogre::Degree`** — strongly-typed angles; `valueRadians()`,
  `valueDegrees()`. **`Ogre::Math`** — `Math::Sqrt`, `Sin/Cos/ATan2`, `Math::PI`,
  `RangeRandom`, `Clamp`.
- **`Ogre::Matrix3` / `Matrix4`** — rotation/transform matrices; `*`,
  `inverse()`, `transpose()`, `makeTransform()`.
- **`Ogre::ColourValue`** — `r,g,b,a`; `ColourValue::White/Black/Red/...`.
- **`Ogre::AxisAlignedBox` / `Ogre::Aabb`** — bounds; `intersects()`, `contains()`,
  `getCenter()`, `getMinimum()/getMaximum()`. (`Character::getAABB()` returns an `Aabb`.)
- **`Ogre::Ray`** — `Ray(origin, direction)`, `getPoint(t)`, `intersects(...)` — for
  picking/line-of-sight math.
- **`Ogre::Plane`**, **`Ogre::Sphere`**, **`Ogre::PlaneBoundedVolume`** — geometric
  primitives used by selection/queries (e.g. `PlayerInterface::SelectionBox`).

### 15.4 Ogre scene graph & rendering

Real classes; call through **live pointers** obtained per §15.2. (Most node
methods are tagged `virtual_l1`/`virtual_l2` in Tindalos — call them normally on
the object.) Higher-risk than math types — prefer reading/transforming objects the
game created over creating your own.

- **`Ogre::Root`** *(singleton)* — `getSingletonPtr()`, `getSceneManager(const String&)`,
  `createSceneManager(...)`, `getAutoCreatedWindow()`, `renderOneFrame()`.
- **`Ogre::SceneManager`** — `getRootSceneNode(SceneMemoryMgrTypes)`,
  `createSceneNode(...)`, `createEntity(meshName)`, `createLight()`,
  `createCamera(name)`, `createManualObject()`, `destroySceneNode/destroyEntity`,
  scene queries (`createRayQuery`, `createSphereQuery`).
- **`Ogre::Node` / `Ogre::SceneNode`** — transform & hierarchy:
  `setPosition(const Vector3&)`, `setOrientation(const Quaternion&)`,
  `setScale(...)`, `translate(...)`, `yaw/pitch/roll(const Radian&)`,
  `_getDerivedPosition()`, `_getDerivedPositionUpdated()`,
  `attachObject(MovableObject*)`, `detachObject(...)`, `detachAllObjects()`,
  `createChildSceneNode(...)`, `numAttachedObjects()`, `getAttachedObject(index)`.
- **`Ogre::MovableObject`** (base of `Entity`, `Light`, `Camera`, `ParticleSystem`,
  `BillboardSet`) — `setVisible(bool)`, `getVisible()`, `setQueryFlags(...)`,
  `getParentSceneNode()`, `getName()`, `setRenderQueueGroup(...)`.
- **`Ogre::Entity`** (v1 mesh instance; Tindalos has no `Item`) — `getSubEntity(i)`,
  `getNumSubEntities()`, `setMaterialName(...)`, `getSkeleton()`, animation state.
  `Ogre::SubEntity` — `setMaterialName(...)`, `setMaterial(...)`.
- **`Ogre::Camera`** — `setPosition/lookAt`, `getDerivedPosition/Orientation`,
  `getCameraToViewportRay(x,y)` (screen→world picking).
- **`Ogre::Light`** — `setType(LT_DIRECTIONAL/POINT/SPOTLIGHT)`,
  `setDiffuseColour/SpecularColour`, `setPowerScale(...)`.
- **`Ogre::ManualObject`** — draw custom lines/geometry (debug overlays):
  `begin(material, OT_LINE_LIST)`, `position(...)`, `colour(...)`, `end()`.
- **`Ogre::BillboardSet`**, **`Ogre::ParticleSystem`** — sprites/effects.
- **Resources:** `Ogre::MeshManager`, `MaterialManager`, `TextureManager`,
  `ResourceGroupManager` (each a singleton) — load/lookup meshes, materials, textures.
- **`Ogre::SharedPtr<T>`** — Ogre's intrusive shared pointer (e.g.
  `CharMovement::speedGroup` is `Ogre::SharedPtr<SpeedGroup>`).

### 15.5 MyGUI (3.2.3)

Kenshi's UI toolkit. Reach the singleton with `MyGUI::Gui::getInstancePtr()` (add
`MyGUIEngine_x64.lib` to link MyGUI calls directly). Use raw MyGUI to add your own
windows/overlays; use the `kenshi/gui` wrappers (§15.6) to drive Kenshi's existing UI.

- **`MyGUI::Gui`** *(singleton)* — `getInstancePtr()`;
  `Widget* createWidgetT(const std::string& type, const std::string& skin, const IntCoord& coord, Align align, const std::string& layer, const std::string& name = "")`
  (and `int left,top,width,height` / `createWidgetReal*` variants);
  `destroyWidget(Widget*)`, `destroyWidgets(...)`.
- **`MyGUI::Widget`** — base UI element: `setPosition/setSize/setCoord`,
  `setVisible(bool)`, `setEnabled(bool)`, `setUserString(...)`, `eventMouseButtonClick`
  and other delegate events, `castType<T>()`.
- **Common widget types** (each its own header): `Button`, `EditBox`, `TextBox`,
  `ListBox`, `ComboBox`, `ImageBox`, `Window`, `ScrollBar`, `ProgressBar`,
  `ScrollView`, `TabControl`, `MenuBar`, `Canvas`.
- **Managers:** `MyGUI::LayoutManager` (load `.layout` files),
  `PointerManager`, `InputManager`, `LayerManager`, `WidgetManager`,
  `ResourceManager`. **Value types:** `IntPoint`, `IntSize`, `IntCoord`,
  `Colour`, `Align`, `UString`.

### 15.6 Kenshi-side GUI windows (`kenshi/gui`)

Reverse-engineered wrappers (with RVAs) around Kenshi's own MyGUI screens — the
**safer** way to interact with existing UI than raw MyGUI. Notables:

- `TitleScreen` — main menu; `_NV_update` is the auto-load hook point used by this repo.
- `MainBarGUI` — the bottom action bar. `DatapanelGUI` / `DataPanelLine` — the
  selected-character info panel. `OrdersPanel` — squad orders.
- `InventoryGUI` / `InventoryTraderGUI` / `CharacterTradingWindow` — inventory & trade.
- `MessageBoxManager` — modal message boxes. `ToolTip`, `ScreenLabel` — overlays/labels.
- `MapScreen`, `SquadManagementScreen`, `ManagementScreen`, `FactionsScreen`,
  `CharacterStatsWindow`, `CharacterEditWindow`, `DialogueWindow`, `LoadSaveWindow`,
  `NewGameWindow`/`NewGameOptionsWindow`, `OptionsWindow`, `BuildModeWindow`,
  `LevelEditor`, `GameDataEditorWindow`, `LoadingWindow`/`SplashScreen`,
  `PortraitManager`, `ProspectingWindow`, `TutorialGUI`, `TransformWindow`.
- `ForgottenGUI` — the top-level GUI root (global `gui` in `Globals.h`).

### 15.7 OIS input

Raw input library (`ois/OIS.h`). Kenshi already wraps input in its own
`InputHandler` (global `key` in `Globals.h`), which is the preferred path for
in-game input; OIS is the lower layer beneath it.

- **`OIS::InputManager`** — `createInputObject(...)`, device enumeration.
- **`OIS::Keyboard`** — `isKeyDown(KeyCode)`, `KeyCode` enum (`KC_W`, `KC_ESCAPE`, ...),
  buffered `KeyListener`.
- **`OIS::Mouse`** — `MouseState` (abs/rel axes, buttons), `MouseButtonID`
  (`MB_Left`, `MB_Right`, ...), buffered `MouseListener`.
- **`OIS::JoyStick`** — gamepad/joystick state and listeners.
