// StatusAutohide.h - auto-hide policy for the persistent co-op status banner
// (pure, zero game/Win32 deps; unit-tested in prototest).
//
// The green "Connected - peer joined" banner (coopOverlayTick) floats over the
// local leader for the WHOLE session. Once the session has held that settled,
// fully-connected ("green") state continuously for a while, the banner has served
// its purpose and should fade out so it stops cluttering the screen. If the state
// later leaves green (the peer disconnects, the session drops, or it cycles back
// through the "waiting/connecting" state on a reconnect) the banner reappears
// immediately, and the timer re-arms once the green state settles again.
//
// This header holds ONLY the time/state decision so it can be unit-tested without a
// game or GUI. The caller (Plugin.cpp coopPanelDrive) owns the millisecond clock
// (GetTickCount) and the show/hide plumbing (coopOverlayTick's show flag). Same
// unsigned-tick, wrap-safe shape as WorkPose.h's poseClearElapsed.
//
// Used by:
//   * Plugin.cpp coopPanelDrive - gates the overlay 'show' flag
//   * prototest                 - the no-game unit layer

#ifndef COOP_STATUS_AUTOHIDE_H
#define COOP_STATUS_AUTOHIDE_H

namespace coop {

// Default auto-hide delay (ms) for the connected/green status banner. Mirrors the
// KENSHICOOP_STATUS_AUTOHIDE_MS default; 0 disables auto-hide entirely (banner stays
// up for the whole session = the pre-autohide behavior).
static const unsigned long STATUS_AUTOHIDE_MS = 10000;

// True once the banner has sat in the settled "green" (fully connected) state long
// enough to auto-hide. 'stableSinceMs' is the tick the green state began (0 = NOT
// green right now, so never auto-hidden). 'autohideMs' == 0 disables the feature
// (always returns false = keep showing). Unsigned tick math matches the engine's
// millisecond clock, so a GetTickCount wrap across the window still yields the true
// elapsed delta (see WorkPose.h poseClearElapsed for the same idiom).
inline bool statusAutohideElapsed(unsigned long stableSinceMs, unsigned long now,
                                  unsigned long autohideMs) {
    if (autohideMs == 0) return false;    // feature off: banner never auto-hides
    if (stableSinceMs == 0) return false; // not in the green state: keep showing
    return (now - stableSinceMs) >= autohideMs;
}

// Whether the persistent status banner should be VISIBLE this tick, given the live
// session flags and the auto-hide timer. 'running' == false hides it (no session).
// While running, the banner shows UNLESS it has auto-hidden after a sustained green
// streak. Only the green state arms 'stableSinceMs', so a running-but-not-green
// state (waiting/connecting, stableSinceMs == 0) always shows.
inline bool statusOverlayShown(bool running, unsigned long stableSinceMs,
                               unsigned long now, unsigned long autohideMs) {
    if (!running) return false;
    return !statusAutohideElapsed(stableSinceMs, now, autohideMs);
}

} // namespace coop

#endif // COOP_STATUS_AUTOHIDE_H
