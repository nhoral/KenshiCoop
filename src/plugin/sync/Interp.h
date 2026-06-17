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

    // Record a received snapshot (identity + transform + locomotion) with its
    // local arrival time. Updates the jitter estimate used to size the delay.
    void push(const EntityState& e, unsigned long nowMs);

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

    bool empty() const { return count_ == 0; }

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
    EntityState   last_;             // most recent full state (identity/locomotion)
    bool          haveLast_;
};

} // namespace coop

#endif // KENSHICOOP_INTERP_H
