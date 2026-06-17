#include "Interp.h"

#include <cmath>

namespace coop {

namespace {
const float PI_F  = 3.14159265358979323846f;
const float TWO_PI = 6.28318530717958647692f;

// Shortest-arc interpolation between two yaw angles (radians).
float lerpAngle(float a, float b, float t) {
    float d = b - a;
    while (d >  PI_F) d -= TWO_PI;
    while (d < -PI_F) d += TWO_PI;
    return a + d * t;
}

float lerpf(float a, float b, float t) { return a + (b - a) * t; }
} // namespace

InterpConfig::InterpConfig()
    : minDelayMs(50), maxDelayMs(200), maxExtrapMs(250),
      snapDistSq(50.0f * 50.0f), staleMs(2000) {}

EntityInterp::EntityInterp()
    : count_(0), head_(0), lastArrival_(0),
      avgIntervalMs_(50.0f), jitterMs_(0.0f), haveLast_(false) {}

const EntityInterp::Snap& EntityInterp::at(int i) const {
    // ring_ holds the last count_ snapshots; head_ points past the newest.
    int idx = head_ - count_ + i;
    idx %= CAP;
    if (idx < 0) idx += CAP;
    return ring_[idx];
}

void EntityInterp::push(const EntityState& e, unsigned long nowMs) {
    // Track inter-arrival interval + jitter (EMA) to size the render delay.
    if (lastArrival_ != 0) {
        float interval = (float)(nowMs - lastArrival_);
        float dev = interval - avgIntervalMs_;
        if (dev < 0) dev = -dev;
        avgIntervalMs_ = avgIntervalMs_ * 0.875f + interval * 0.125f;
        jitterMs_      = jitterMs_      * 0.875f + dev      * 0.125f;
    }
    lastArrival_ = nowMs;

    Snap s;
    s.t = nowMs;
    s.x = e.x; s.y = e.y; s.z = e.z; s.heading = e.heading;
    ring_[head_] = s;
    head_ = (head_ + 1) % CAP;
    if (count_ < CAP) ++count_;

    last_ = e;
    haveLast_ = true;
}

unsigned long EntityInterp::renderDelay(const InterpConfig& cfg) const {
    // One packet interval of cushion plus a couple jitter deviations, clamped.
    float d = avgIntervalMs_ + 2.0f * jitterMs_;
    if (d < (float)cfg.minDelayMs) d = (float)cfg.minDelayMs;
    if (d > (float)cfg.maxDelayMs) d = (float)cfg.maxDelayMs;
    return (unsigned long)d;
}

bool EntityInterp::sourceMoving() const {
    if (count_ < 2) return false;
    const Snap& a = at(count_ - 2);
    const Snap& b = at(count_ - 1);
    float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    return (dx * dx + dy * dy + dz * dz) > 0.0004f; // ~2 cm
}

bool EntityInterp::latest(EntityState* out, float* vx, float* vy, float* vz) const {
    if (!haveLast_ || count_ == 0 || out == 0) return false;
    const Snap& newest = at(count_ - 1);
    *out = last_;
    out->x = newest.x; out->y = newest.y; out->z = newest.z;
    out->heading = newest.heading;
    if (vx || vy || vz) {
        float ex = 0.0f, ey = 0.0f, ez = 0.0f;
        if (count_ >= 2) {
            const Snap& prev = at(count_ - 2);
            float dt = (float)(newest.t - prev.t);
            if (dt > 0.0f) {
                ex = (newest.x - prev.x) * 1000.0f / dt; // units per second
                ey = (newest.y - prev.y) * 1000.0f / dt;
                ez = (newest.z - prev.z) * 1000.0f / dt;
            }
        }
        if (vx) *vx = ex;
        if (vy) *vy = ey;
        if (vz) *vz = ez;
    }
    return true;
}

bool EntityInterp::sample(unsigned long nowMs, const InterpConfig& cfg, EntityState* out) {
    if (!haveLast_ || count_ == 0 || out == 0) return false;

    const Snap& newest = at(count_ - 1);
    if (nowMs - newest.t > cfg.staleMs) return false; // stream dropped; release body

    *out = last_; // identity + locomotion/pose passthrough

    unsigned long delay = renderDelay(cfg);
    unsigned long renderTime = (nowMs > delay) ? (nowMs - delay) : 0;

    // Only one snapshot: nothing to interpolate.
    if (count_ == 1) {
        out->x = newest.x; out->y = newest.y; out->z = newest.z;
        out->heading = newest.heading;
        return true;
    }

    const Snap& oldest = at(0);

    // Render time predates our history: clamp to the oldest known pose.
    if (renderTime <= oldest.t) {
        out->x = oldest.x; out->y = oldest.y; out->z = oldest.z;
        out->heading = oldest.heading;
        return true;
    }

    // Render time is past the newest: dead-reckon briefly off the last segment,
    // then clamp. Prevents a hard stop during a short starvation gap.
    if (renderTime >= newest.t) {
        const Snap& prev = at(count_ - 2);
        unsigned long ahead = renderTime - newest.t;
        if (ahead > cfg.maxExtrapMs) ahead = cfg.maxExtrapMs;
        float seg = (float)(newest.t - prev.t);
        if (seg <= 0.0f) {
            out->x = newest.x; out->y = newest.y; out->z = newest.z;
            out->heading = newest.heading;
            return true;
        }
        float k = (float)ahead / seg; // extrapolation factor along last segment
        out->x = newest.x + (newest.x - prev.x) * k;
        out->y = newest.y + (newest.y - prev.y) * k;
        out->z = newest.z + (newest.z - prev.z) * k;
        out->heading = lerpAngle(prev.heading, newest.heading, 1.0f + k);
        return true;
    }

    // Find the segment [s0, s1] bracketing renderTime and interpolate within it.
    for (int i = 0; i + 1 < count_; ++i) {
        const Snap& s0 = at(i);
        const Snap& s1 = at(i + 1);
        if (renderTime >= s0.t && renderTime <= s1.t) {
            float dx = s1.x - s0.x, dy = s1.y - s0.y, dz = s1.z - s0.z;
            // Source teleport inside this segment: snap to the newer end instead
            // of smearing the body across the gap.
            if (dx * dx + dy * dy + dz * dz > cfg.snapDistSq) {
                out->x = s1.x; out->y = s1.y; out->z = s1.z;
                out->heading = s1.heading;
                return true;
            }
            float span = (float)(s1.t - s0.t);
            float a = (span > 0.0f) ? (float)(renderTime - s0.t) / span : 0.0f;
            out->x = lerpf(s0.x, s1.x, a);
            out->y = lerpf(s0.y, s1.y, a);
            out->z = lerpf(s0.z, s1.z, a);
            out->heading = lerpAngle(s0.heading, s1.heading, a);
            return true;
        }
    }

    // Fallback (shouldn't hit): newest pose.
    out->x = newest.x; out->y = newest.y; out->z = newest.z;
    out->heading = newest.heading;
    return true;
}

} // namespace coop
