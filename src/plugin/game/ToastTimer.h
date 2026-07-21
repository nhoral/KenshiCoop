// ToastTimer - pure visibility clock for the ephemeral on-screen co-op toast.
//
// The persistent status overlay (coopOverlayTick) shows the CURRENT session
// state (offline/waiting/connected) and stays up as long as the session runs.
// It cannot, on its own, announce a TRANSITION: the exact moment a peer connects
// or drops. This module is the timer behind a second, EPHEMERAL banner
// (coopToastTick) that pops "Peer connected" / "Peer disconnected" for a few
// seconds and then removes itself - distinct from the persistent overlay.
//
// Only the timing decision lives here, as pure inline logic with no I/O and no
// game/GUI dependency, so the unit layer (prototest) can lock it without linking
// the engine. The Plugin arms the toast (records nowMs) on a connect/leave edge
// and each frame asks toastVisible() whether to still draw it; the drawing is
// EngineUi's job.

#ifndef KENSHICOOP_TOAST_TIMER_H
#define KENSHICOOP_TOAST_TIMER_H

namespace coop {
namespace engine {

// How long an armed toast stays on screen before it self-hides (ms). A few
// seconds: long enough to read the transition, short enough to feel momentary
// and never be mistaken for the persistent status banner.
const unsigned long TOAST_SHOW_MS = 4000;

// Pure visibility decision (unit-testable): a toast armed at armMs is visible at
// nowMs while fewer than durationMs have elapsed. An un-armed toast is never
// visible. The subtraction is unsigned so a GetTickCount() wrap within one
// window still yields the true elapsed delta (mirrors poseClearElapsed).
inline bool toastVisible(bool armed, unsigned long armMs, unsigned long nowMs,
                         unsigned long durationMs) {
    if (!armed) return false;
    return (unsigned long)(nowMs - armMs) < durationMs;
}

} // namespace engine
} // namespace coop

#endif // KENSHICOOP_TOAST_TIMER_H
