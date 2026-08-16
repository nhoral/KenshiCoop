// HostIntent.h - reusable policy for host-canonical interaction channels.
// Pure C++03: no game, Win32 or network dependencies, so prototest can lock the
// idempotency/ack contract before the pattern is reused by another subsystem.

#ifndef COOP_HOST_INTENT_H
#define COOP_HOST_INTENT_H

namespace coop {

// Sequence 0 is reserved for "none". Reliable ordering handles transport order;
// this guard makes duplicate retries harmless at the authority.
inline bool hostIntentIsNew(unsigned int lastSeq, unsigned int incomingSeq) {
    return incomingSeq != 0u && incomingSeq > lastSeq;
}

// A canonical state row settles only the pending intent it explicitly answers.
inline bool hostIntentAckCovers(unsigned int localOwnerId,
                                unsigned int pendingSeq,
                                unsigned int ackOwnerId,
                                unsigned int ackSeq) {
    return pendingSeq != 0u && ackOwnerId == localOwnerId && ackSeq >= pendingSeq;
}

// Wall-clock retry backstop. At rest there is no traffic; while an ack is missing
// the same idempotent intent may be resent after the interval.
inline bool hostIntentRetryDue(unsigned long nowMs, unsigned long lastSendMs,
                               unsigned long retryMs) {
    return lastSendMs == 0ul || (nowMs - lastSendMs) >= retryMs;
}

} // namespace coop

#endif // COOP_HOST_INTENT_H
