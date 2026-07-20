// ChangeGate - the ONE change-gated send/accept policy the periodic state
// channels share (Phase 6).
//
// Almost every sampled channel in ReplicatorChannels.cpp (money, factions,
// doors, build state, build doors, prod, research, ...) reimplements the SAME
// three-part policy by hand, with only its constants and its typed baseline
// differing:
//
//   1. SAMPLE THROTTLE  - only walk the world's rows once per SAMPLE_MS.
//   2. PER-ROW SEND GATE - resend if the value CHANGED vs the last-sent
//      baseline, or if a safety RESEND_MS has elapsed since the last send;
//      an optional MIN_SEND_MS hard-throttles bursts after a send.
//   3. SEQ ACCEPT (apply) - a per-sender monotonic seq drops stale/duplicate
//      rows (resends + echoes) before the receiver touches the engine.
//
// Cloning that policy per channel is where the drift lives: money treats a
// never-sent row as resend-due (it has no silent seed step) while doors/faction
// seed the baseline silently first and only resend rows they have ACTUALLY
// sent. Both fall out of the SAME predicate once `resendUnsent` is a parameter,
// so this header captures the decision in one place and the channels keep only
// their typed baseline + constants.
//
// Pure inline C++03, zero game/logger/wire dependency (matches EngineFaults.h /
// EngineCaps.h) so the unit layer (prototest) locks the policy directly. The
// TYPED baseline compare (money value == , door open/locked, faction |delta| >
// EPS) stays at the call site - only the timing/seq decision lives here.

#ifndef KENSHICOOP_CHANGE_GATE_H
#define KENSHICOOP_CHANGE_GATE_H

namespace coop {
namespace sync {

// SAMPLE THROTTLE (pass level). True when a fresh world sample is due: the very
// first pass (lastSampleMs == 0) always samples, then at most once per sampleMs.
// The caller stamps lastSampleMs = nowMs when this returns true. Unsigned
// subtraction tolerates the clock's midnight wrap within one sample period.
inline bool gateSampleDue(unsigned long nowMs, unsigned long lastSampleMs,
                          unsigned long sampleMs) {
    return lastSampleMs == 0 || (nowMs - lastSampleMs) >= sampleMs;
}

// PER-ROW SEND GATE. Decide whether a change-gated row should send THIS pass.
//   changed      : sampled value differs from the last-sent baseline (typed
//                  compare owned by the caller).
//   nowMs        : current clock.
//   lastSendMs   : ms of this row's last send; 0 = never sent.
//   minSendMs    : hard throttle after a send (0 = none). A row sent < minSendMs
//                  ago never resends, even on change - burst suppression.
//   resendMs     : periodic safety resend for a row that HAS been sent.
//   resendUnsent : how to treat a never-sent (lastSendMs == 0), unchanged row.
//                  true  = resend-due (money: no silent seed, so stream it once);
//                  false = hold (doors/faction: the baseline was seeded silently,
//                  send only on a real change or a real post-send resend).
// A changed row always sends (subject to the min-send throttle).
inline bool gateShouldSend(bool changed, unsigned long nowMs,
                           unsigned long lastSendMs, unsigned long minSendMs,
                           unsigned long resendMs, bool resendUnsent) {
    if (lastSendMs != 0 && (nowMs - lastSendMs) < minSendMs)
        return false;                       // throttled: sent too recently
    if (changed)
        return true;                        // a real change always crosses
    if (lastSendMs == 0)
        return resendUnsent;                // never-sent, unchanged row
    return (nowMs - lastSendMs) >= resendMs; // periodic safety resend
}

// SEQ ACCEPT (apply level). A per-sender seq is monotonic; accept a row iff it
// is the first ever seen for this key (seqSeen == 0) or strictly newer than the
// last accepted. Drops resends + echoes of already-applied state. The caller
// stamps seqSeen = incomingSeq when this returns true.
inline bool gateSeqAccept(unsigned int seqSeen, unsigned int incomingSeq) {
    return seqSeen == 0 || incomingSeq > seqSeen;
}

} // namespace sync
} // namespace coop

#endif // KENSHICOOP_CHANGE_GATE_H
