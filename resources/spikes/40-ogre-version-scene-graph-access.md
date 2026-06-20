# Spike 40 - Ogre version + scene graph access

- Type: STATIC
- Status: DONE
- Save: n/a (static SDK analysis)
- Branch commit: <filled at commit>

## Goal

Pin the exact Ogre version Kenshi runs and determine how (and whether) the mod can
reach the live scene graph - `SceneManager`, `SceneNode`s, `Camera`. This gates every
render-side co-op feature (overlay HUD, nameplates, markers, attaching our own visuals)
and tells us which Ogre API era we must target.

## Method

Static read of the bundled Ogre headers (`ogre/OgrePrerequisites.h`,
`OgreRoot.h`, `OgreSceneManager.h`, `OgreCamera.h`), Kenshi's reconstructed
`CameraClass.h` and `Globals.h`, plus a repo-wide check for (a) any Ogre method RVA
annotations and (b) existing Ogre use in `src/plugin`. No code; nothing run.

## Findings

1. **Ogre version is 2.0.0 "Tindalos" (suffix "unstable").** Direct from
   `OgrePrerequisites.h`:

```
#define OGRE_VERSION_MAJOR 2
#define OGRE_VERSION_MINOR 0
#define OGRE_VERSION_PATCH 0
#define OGRE_VERSION_SUFFIX "unstable"
#define OGRE_VERSION_NAME "Tindalos"
```

   This is an **early experimental Ogre 2.0 branch**, not Ogre 1.x and not the later
   stable 2.1/2.2/2.3. Consequences: SoA/`ArrayMemoryManager` scene nodes,
   `SceneMemoryMgrTypes` (`SCENE_DYNAMIC`/`SCENE_STATIC`), HLMS-era materials, and a
   compositor-driven render path. Any render code must target the 2.0 "Tindalos" API
   shape (e.g. `getRootSceneNode(SCENE_DYNAMIC)`), and overlay/HUD APIs differ from both
   Ogre 1.x Overlays and later 2.x.

2. **The live scene graph IS reachable - but only through Kenshi's reconstructed
   accessors, not Ogre's own.** `Globals.h` exposes no scene manager (`ou`/`con`/`key`/
   `options`/`gui` only). The proven route is the spike-35 camera:
   `PlayerInterface::getCamera()` -> `CameraClass`, which holds
   `Ogre::Camera* camera` (0x68), and returns live nodes via
   `getCameraNode()` (RVA 0x100930), `getCenterNode()` (0x166E30), plus members
   `node` (0x70) / `center` (0x58). From an `Ogre::Camera*` one can in principle reach
   `getSceneManager()` -> `getRootSceneNode()` to walk/attach to the graph.

3. **CRITICAL CONSTRAINT: no Ogre method is RVA-annotated, so Ogre's own functions are
   not `GetRealAddress`-resolvable.** A repo-wide scan of `ogre/` found **zero**
   `RVA = 0x...` annotations (vs. ~thousands on `kenshi/` reconstructed methods). The
   mod's entire calling mechanism (`KenshiLib::GetRealAddress(&Class::method)`) depends
   on those RVA comments, which exist only for `kenshi::` symbols. Therefore
   `Ogre::Root::getSingletonPtr()`, `SceneManager::getRootSceneNode()`,
   `SceneManager::createSceneNode()`, etc. **cannot be called via the established path** -
   their addresses are unknown.

4. **Operating on Ogre objects must therefore go through (a) header-inline methods or
   (b) raw member offsets - the same struct-poking the mod already does for Kenshi.**
   Many Ogre 2.0 node/camera accessors are inline in the headers (compiled into the mod
   directly, no exe symbol needed); anything non-inline needs a manually resolved RVA or
   a direct member write. Reading/animating a node we *obtain* from `CameraClass` is the
   low-risk subset; *creating* new Ogre objects (nodes/entities/overlays) is the hard
   part because the factory methods are non-inline and unannotated.

5. **The mod currently uses no Ogre directly.** No `Ogre::Root`/`SceneManager`/
   `SceneNode` references exist in `src/plugin`, so render-side work is greenfield and
   the constraints in (3)/(4) are unmitigated by any prior pattern.

## Validation

- Finding 1: direct quote of `OgrePrerequisites.h` lines 51-55 (version macros). This is
  a definitive header fact, not inference.
- Finding 2: `Globals.h` read in full (no scene pointer); `CameraClass.h` member/RVA
  lines quoted (0x68/0x70/0x58, getCameraNode 0x100930, getCenterNode 0x166E30). The
  `CameraClass` instance itself is **runtime-proven** - spike 35 obtained it live via
  `player->getCamera()` and drove it. So the `Ogre::Camera*`/`SceneNode*` it holds are
  pointers off a confirmed-live object (one deref away), though see Open questions.
- Finding 3: `rg "RVA = 0x"` over the entire `ogre/` header tree returned **0** matches
  (OgreRoot/SceneManager/SceneNode/Node specifically: 0). This is the hard evidence that
  Ogre symbols are outside the GetRealAddress mechanism.
- Finding 5: `rg` for Ogre scene types over `src/plugin` returned no matches.

## Open questions / hypotheses (UNVALIDATED)

- **The Ogre objects behind `CameraClass::camera`/`getCameraNode()` were never
  dereferenced.** Spike 35 used Kenshi's `getCameraPos()` (a reconstructed RVA), not raw
  Ogre. Whether reading `Ogre::Camera`/`SceneNode` members at our assumed offsets is
  correct/safe is UNVALIDATED - needs a host-only probe that reads e.g. node position via
  the Ogre header and compares to the Kenshi `getCameraPos()`.
- **Which Ogre accessors are header-inline vs. need an RVA is uncatalogued.** This
  decides how much of the 2.0 API is usable for free. Needs a compile-test per call.
- **Reaching `SceneManager` from `Ogre::Camera`** (via `getSceneManager()`) is assumed
  but unproven - if that getter is non-inline it is unreachable without its RVA.
- **Overlay/HUD path (spike 41) feasibility hinges on all of the above** and is the next
  thing to actually test at runtime.

## Implications for co-op

- Target the **Ogre 2.0 "Tindalos"** API specifically for any render code; do not assume
  Ogre 1.x Overlay or later 2.x signatures.
- The cheapest render features attach to / read **nodes we obtain from Kenshi** (camera
  node, character scene nodes) rather than creating Ogre objects from scratch.
- Creating new visuals (HUD/markers) will likely require either an RVA-resolution pass on
  a handful of Ogre factory methods or reuse of Kenshi's own GUI (`ForgottenGUI`/MyGUI,
  spikes 36/50) instead of raw Ogre - MyGUI is the safer HUD route given constraint (3).

## Recommended follow-ups

- Host-only probe: from the proven `CameraClass`, read `camera`/`getCameraNode()` and
  dereference one Ogre `SceneNode` member (position), cross-checking against
  `getCameraPos()` to validate offsets (closes the main open question).
- Try reaching `SceneManager` via the camera and enumerate one node child to confirm the
  graph is walkable.
- Pivot HUD work to MyGUI/`ForgottenGUI` (spikes 41/50) since raw Ogre object *creation*
  is blocked by the missing RVAs; reserve raw Ogre for reads/attaching to existing nodes.
