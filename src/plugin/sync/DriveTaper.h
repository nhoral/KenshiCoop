// DriveTaper - the pure deceleration-taper math for the walk-drive.
//
// Ported concept from CTRL-ALT-E/KENSHI-CO-OP commit e36b960 ("Walk-drive:
// eliminate on-stop overshoot/snap-back of remote characters"): as the driven
// source decelerates toward a stop, the walk-drive's lead projection, its
// gap-proportional catch-up boost, and its speed cap must all shrink in lockstep
// with the source's TRUE translation velocity, so the body converges to the
// source's velocity at the mark instead of overrunning it and snapping back.
//
// The taper is a single clamped ratio: speedFrac = srcSpeed / refSpeed in
// [0, 1] (1 at/above cruise, 0 at a full stop). Kept as a pure, dependency-free
// inline so the unit layer (prototest) can exercise it directly - no game,
// engine, or logger dependency.

#ifndef KENSHICOOP_DRIVE_TAPER_H
#define KENSHICOOP_DRIVE_TAPER_H

namespace coop {

// Deceleration taper fraction in [0, 1]. srcSpeed is the source's TRUE
// translation velocity (u/s, estimated from the snapshot stream - NOT the host's
// slowly-decaying cSpeed field, which lies during a stop). refSpeed is the speed
// at/above which the drive runs at full strength; below it the returned fraction
// tapers linearly to 0 as the source halts. A non-positive refSpeed disables the
// taper (returns full strength) so a mis-tuned reference never freezes the drive.
inline float driveSpeedTaper(float srcSpeed, float refSpeed) {
    if (refSpeed <= 0.0f) return 1.0f; // no usable reference -> no taper
    float f = srcSpeed / refSpeed;
    if (f < 0.0f) f = 0.0f; // clamp low (negative speeds are noise)
    if (f > 1.0f) f = 1.0f; // clamp high (at/above cruise = full strength)
    return f;
}

} // namespace coop

#endif // KENSHICOOP_DRIVE_TAPER_H
