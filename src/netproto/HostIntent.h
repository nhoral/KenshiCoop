// HostIntent.h - reusable sequencing policy for host-canonical interactions.
// Pure C++03 and dependency-free so prototest can lock the contract.

#ifndef COOP_HOST_INTENT_H
#define COOP_HOST_INTENT_H

namespace coop {

inline bool hostIntentIsNew(unsigned int lastSeq, unsigned int incomingSeq) {
    return incomingSeq != 0u && incomingSeq > lastSeq;
}

inline bool hostIntentAckCovers(unsigned int localOwnerId,
                                unsigned int pendingSeq,
                                unsigned int ackOwnerId,
                                unsigned int ackSeq) {
    return pendingSeq != 0u && ackOwnerId == localOwnerId && ackSeq >= pendingSeq;
}

inline bool hostIntentRetryDue(unsigned long nowMs, unsigned long lastSendMs,
                               unsigned long retryMs) {
    return lastSendMs == 0ul || (nowMs - lastSendMs) >= retryMs;
}

} // namespace coop
#endif
