// Interp - the foundational client-side interpolation buffer.
//
// Raw apply (Stage 1) snapped each body to the newest received transform, so
// motion stepped at the 20 Hz send rate. This buffer instead renders each body
// slightly in the PAST (a small, jitter-adaptive delay) and interpolates between
// the two snapshots that bracket that render time, so the body glides smoothly
// every frame regardless of packet cadence/jitter. When the buffer is starved it
// dead-reckons briefly off the last velocity; a large source jump (teleport)
// bypasses interpolation so we don't smear across it.
//
// All access is from the single game thread (the Replicator drains the inbound
// queue and samples here on the main tick), so no locking is needed. Plain
// C++03 for the VS2010 (v100) toolchain.

#ifndef KENSHICOOP_INTERP_H
#define KENSHICOOP_INTERP_H

#include "../../netproto/Wire.h"

namespace coop {

struct InterpConfig {
    unsigned long minDelayMs;  // floor on render delay (low-jitter LAN)
    unsigned long maxDelayMs;  // ceiling on render delay (high jitter)
    unsigned long maxExtrapMs; // dead-reckoning cap when the buffer is starved
    float         snapDistSq;  // source step^2 above which we snap (teleport)
    unsigned long staleMs;     // stop driving if newest snapshot is older than this
    InterpConfig();
};

class EntityInterp {
public:
    EntityInterp();

    // What the last sample() actually did - the starvation/teleport telemetry
    // behind the "[interp]" stat line (protocol 36 jumpiness instrumentation).
    // LERP is the healthy regime; EXTRAP/CLAMP_OLD mean the buffer starved
    // (render time ran past the newest snapshot / predates the whole ring);
    // SEG_SNAP is a source teleport bypassing interpolation.
    enum SampleMode {
        SM_NONE = 0, SM_LERP, SM_SINGLE, SM_CLAMP_OLD, SM_EXTRAP, SM_SEG_SNAP
    };
    int           lastMode()    const { return lastMode_; }
    unsigned long lastDelayMs() const { return lastDelay_; }
    float         jitter()      const { return jitterMs_; }
    float         avgInterval() const { return avgIntervalMs_; }

    // Record a received snapshot (identity + transform + locomotion). nowMs is
    // the ring index time: with wire v35 this is the SENDER's capture time
    // mapped into the local clock (clean 20 Hz spacing - velocity and segment
    // math never see path jitter). arrivalMs (0 = unknown) is the local arrival
    // time; the positive gap (arrivalMs - nowMs) is the packet's queueing lag,
    // tracked peak-hold so renderDelay still covers path jitter even though the
    // interval stats no longer contain it.
    void push(const EntityState& e, unsigned long nowMs, unsigned long arrivalMs = 0);

    // Produce the render-time pose at 'nowMs' into 'out' (identity + locomotion
    // copied from the latest snapshot, transform interpolated). Returns false if
    // there is no usable data yet or the stream has gone stale.
    bool sample(unsigned long nowMs, const InterpConfig& cfg, EntityState* out);

    // True if the two newest snapshots differ in position (source is moving).
    // Used by the smoothness oracle to scope its measurement to active motion.
    bool sourceMoving() const;

    // Newest received pose (identity/locomotion from the latest snapshot, position
    // from the newest ring entry) plus a velocity estimate (units/sec) from the two
    // newest snapshots. The walk-drive aims a lead point = newest + vel*lead so the
    // engine keeps a continuous gait instead of stopping at a delayed target.
    // Returns false if there is no usable data. vx/vy/vz may be null.
    bool latest(EntityState* out, float* vx, float* vy, float* vz) const;

    // Sender-stamped duration of the newest ring segment (ms; 0 = fewer than
    // two snapshots). This is the stream's CURRENT per-entity cadence: ~50 ms
    // on the 20 Hz near tier, ~500+ ms on the round-robin mid tier. The
    // walk-drive scales its lead point and the walk/rest debounce by it so a
    // sparsely-sampled NPC keeps a continuous gait instead of reaching the
    // lead point and idling until the next sample (Phase 2 mid-band tier).
    unsigned long lastSegMs() const {
        if (count_ < 2) return 0;
        unsigned long dt = at(count_ - 1).t - at(count_ - 2).t;
        return dt;
    }

    bool empty() const { return count_ == 0; }

    // Ring fill (0..CAP). A young ring (< CAP) means this entity was (re-)
    // acquired within the last ~16 samples - its first reconciliation snap
    // is a coverage event, not steady-state tracking noise (Phase 2).
    int samples() const { return count_; }

private:
    struct Snap {
        unsigned long t;
        float x, y, z, heading;
    };

    static const int CAP = 16;

    const Snap& at(int i) const;     // i in [0, count_): 0 = oldest
    unsigned long renderDelay(const InterpConfig& cfg) const;

    Snap          ring_[CAP];
    int           count_;
    int           head_;             // index of the next write slot
    unsigned long lastArrival_;
    float         avgIntervalMs_;
    float         jitterMs_;
    float         lagMs_;            // peak-decayed queueing lag (arrival - ring time)
    EntityState   last_;             // most recent full state (identity/locomotion)
    bool          haveLast_;
    int           lastMode_;         // SampleMode of the last sample() call
    unsigned long lastDelay_;        // renderDelay used by the last sample() call
};

} // namespace coop

#endif // KENSHICOOP_INTERP_H
