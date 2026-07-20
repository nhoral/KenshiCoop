// EngineSync.h - the Replicator's canonical narrow engine include (Phase 5a
// domain split, 2026-07-19). This is the STABLE name the replication layer
// depends on; it currently re-exports the Engine.h sync core.
//
// Phase 5a carved the cleanly-separable public surfaces (UI panel -> EngineUi.h,
// deterministic test-scene setup -> EngineScenario.h, raw-RVA diagnostic probes
// -> EngineProbe.h) OUT of Engine.h, so a consumer that includes EngineSync.h /
// Engine.h no longer transitively sees the panel, scene-builder, or spike-probe
// APIs. The remaining sync-vs-scenario reclassification (medical / stats /
// furniture / carry / inventory / world-item / combat scaffolds that are still
// interleaved with their sync siblings) rides along in later Phase 5 increments;
// as those decls move into EngineScenario.h, Engine.h shrinks to the pure sync
// surface and this header stays the Replicator's unchanged include point.

#ifndef KENSHICOOP_ENGINE_SYNC_H
#define KENSHICOOP_ENGINE_SYNC_H

#include "Engine.h"

#endif // KENSHICOOP_ENGINE_SYNC_H
