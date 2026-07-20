#ifndef KENSHICOOP_SYNC_TUNING_H
#define KENSHICOOP_SYNC_TUNING_H

// SyncTuning.h - Phase 6d: the one owned home for the change-gated SAMPLED
// channels' send cadence. Before this, every channel in ReplicatorChannels.cpp
// carried its own function-local SAMPLE_MS / MIN_SEND_MS / RESEND_MS literals;
// they now live here as named fields so the whole channel cadence surface reads
// from a single struct the Replicator owns (SyncTuning tuning_). The channel
// bodies still bind their old local names to these fields, so runtime values are
// byte-identical to the literals they replaced - this is a consolidation, not a
// behavior change. Pure POD (a default ctor sets today's tuned values); no
// mutable file-scope state, C++03/v100-safe, header-only.
//
// SCOPE: only the ChangeGate-policy channels (money + the six registry channels:
// faction, doors, placed buildings, placed-building doors, production,
// research). Medical/stats cadence and the drive/combat/interp tunables in
// ReplicatorUtil.h are a different subsystem (Phase 7's drive state machine
// owns those) and intentionally stay where they are.

namespace coop {

struct SyncTuning {
    // Money (protocol 22): wallets move in bursts, ~1 Hz sampling floor is
    // plenty; a lost write self-heals on the safety resend.
    unsigned long moneyMinSendMs;   // sampling floor between money sends
    unsigned long moneyResendMs;    // unchanged-row safety resend

    // Faction relations (protocol 24): relations move in bursts; 1 Hz sample,
    // long safety resend for rows we ever sent.
    unsigned long factionSampleMs;
    unsigned long factionResendMs;

    // Baked doors (protocol 26): doors move in clicks; 1 Hz sample.
    unsigned long doorSampleMs;
    unsigned long doorResendMs;

    // Production machines (protocol 33): machines tick slowly; 1 Hz sample,
    // resend doubles as the join drift corrector.
    unsigned long prodSampleMs;
    unsigned long prodResendMs;

    // Research (protocol 38): unlocks are rare; 1 Hz sample, long lost-row /
    // late-prereq corrector resend.
    unsigned long researchSampleMs;
    unsigned long researchResendMs;

    // Placed buildings - construction progress (protocol 27).
    unsigned long buildSampleMs;
    unsigned long buildResendMs;

    // Placed-building doors (protocol 28): mirrors the protocol-26 door cadence.
    unsigned long bdoorSampleMs;
    unsigned long bdoorResendMs;

    SyncTuning()
        : moneyMinSendMs(1000),   moneyResendMs(5000),
          factionSampleMs(1000),  factionResendMs(10000),
          doorSampleMs(1000),     doorResendMs(10000),
          prodSampleMs(1000),     prodResendMs(10000),
          researchSampleMs(1000), researchResendMs(15000),
          buildSampleMs(1000),    buildResendMs(10000),
          bdoorSampleMs(1000),    bdoorResendMs(10000) {}
};

} // namespace coop

#endif // KENSHICOOP_SYNC_TUNING_H
