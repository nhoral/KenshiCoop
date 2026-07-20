// ScenarioInventory.cpp - container/inventory scenarios (monolith split from
// Scenario.cpp, 2026-07-12): inv_order, inv_bidir, trade_probe, trade_peer,
// inv_equip/inv_reequip, inv_wpnseq, inv_addequip, wpn_relocate. Classes are
// TU-private (anonymous namespace); only makeInventoryScenario
// (ScenarioSupport.h) is exported.
// Must NOT: change any SCENARIO log string (oracle API, resources/CODE_MAP.md).

#include "ScenarioSupport.h"

namespace coop {
namespace {

// Phase 4a: container-contents (inventory) replication. Both clients anchor on the
// SAME container (v1: the leader's own inventory - a save-stable hand that resolves
// cross-client). Each samples its LOCAL container's contents (count + order-
// independent content hash) every 500 ms; the host performs a LIVE add mid-run. The
// join must (a) observe a content CHANGE (>=2 distinct hashes - proving it wasn't a
// static loaded state) and (b) end with MORE items than its own baseline. The runner
// additionally cross-checks the host's and join's FINAL hashes match (same multiset).
class InventorySyncScenario : public TimedScenario {
public:
    InventorySyncScenario()
        : TimedScenario("inv_order", 0), haveContainer_(false), added_(false), lastLogMs_(0),
          samples_(0), distinct_(0), firstCount_(0), lastCount_(0),
          firstHash_(0), lastHash_(0), prevHash_(0) {
        for (int i = 0; i < 5; ++i) cHand_[i] = 0;
    }

    virtual void onStart(const ScenarioContext& ctx) {
        haveContainer_ = engine::pickInventoryContainer(ctx.gw, cHand_);
        char b[160];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO INV anchor have=%d hand=%u,%u,%u,%u,%u",
            haveContainer_ ? 1 : 0, cHand_[0], cHand_[1], cHand_[2], cHand_[3], cHand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (haveContainer_ && (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0)) {
            lastLogMs_ = ctx.elapsedMs;

            InvItemEntry items[INV_ITEMS_MAX];
            unsigned int hash = 0;
            unsigned int n = engine::captureContainerContents(
                ctx.gw, cHand_, items, INV_ITEMS_MAX, &hash);

            if (samples_ == 0) { firstCount_ = n; firstHash_ = hash; prevHash_ = hash; }
            else if (hash != prevHash_) { ++distinct_; prevHash_ = hash; }
            lastCount_ = n; lastHash_ = hash; ++samples_;

            char b[160];
            _snprintf(b, sizeof(b) - 1,
                "SCENARIO INV %s t=%lu count=%u hash=%u",
                ctx.isHost ? "MEMBER" : "RECV",
                (unsigned long)ctx.elapsedMs, n, hash);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);

            // Host performs the LIVE add once, after a short baseline window. The
            // content-change then rides the reliable snapshot channel to the join.
            if (ctx.isHost && !added_ && ctx.elapsedMs >= ADD_MS) {
                added_ = true;
                char sid[48]; sid[0] = '\0';
                int got = engine::addTestItemsToContainer(ctx.gw, cHand_, 1, sid, sizeof(sid));
                char m[200];
                _snprintf(m, sizeof(m) - 1,
                    "SCENARIO INV ADD added=%d sid='%s'", got, sid[0] ? sid : "(none)");
                m[sizeof(m) - 1] = '\0'; coop::logLine(m);
            }
        }

        // Host outlives the join so its stream/snapshot never goes stale mid-window.
        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            // Host: it performed the live add. Join: it ended holding synced (non-empty)
            // content. The launch stagger means the join may start sampling AFTER the
            // add already propagated (so it never sees the 0->1 edge itself); the
            // runner's INV-SYNC oracle is the authoritative cross-client proof
            // (host live-change + host/join final-hash match). distinct_ is advisory.
            if (ctx.isHost) passed_ = haveContainer_ && added_;
            else            passed_ = haveContainer_ && (lastCount_ > 0) && (lastHash_ != 0);
            return true;
        }
        return false;
    }

private:
    static const unsigned long HOST_DURATION_MS = 40000; // outlive the join's window
    static const unsigned long JOIN_DURATION_MS = 24000;
    static const unsigned long ADD_MS           = 8000;  // baseline, then add live

    bool          haveContainer_;
    bool          added_;
    unsigned long lastLogMs_;
    unsigned int  cHand_[5];
    unsigned int  samples_;
    unsigned int  distinct_;   // count of content-hash changes observed
    unsigned int  firstCount_;
    unsigned int  lastCount_;
    unsigned int  firstHash_;
    unsigned int  lastHash_;
    unsigned int  prevHash_;
};

// inv_bidir (Phase 4a, BIDIRECTIONAL container-contents): each client mutates ONLY the
// inventory of a squad member it OWNS (host = tab-rank 0, join = tab-rank 1 - the same
// partition the Replicator streams on) and samples BOTH squad-tab containers every
// 500 ms, logging OWN (authoritative) and PEER (reconciled) lines keyed by rank. Each
// side runs an ADD-then-REMOVE sequence with a DISTINCT net delta (host -> +2, join ->
// +1) so the runner's per-rank convergence check is unambiguous and removals (not just
// adds) must propagate. The runner cross-checks, per rank, that the NON-authoring side
// converged to the author's FINAL contents - proving inventory flows both ways with no
// loss/dupe on the supported (owned-container) path. Requires a shared save with >=2
// squad tabs (rank 0 and rank 1).
class InventoryBidirScenario : public TimedScenario {
public:
    InventoryBidirScenario()
        : TimedScenario("inv_bidir", 0), haveOwn_(false), added_(false), removed_(false),
          lastLogMs_(0), samples_(0), ownRank_(0),
          firstOwnCount_(0), lastOwnCount_(0), prevOwnHash_(0), distinctOwn_(0) {
        for (int i = 0; i < 5; ++i) ownHand_[i] = 0;
    }

    virtual void onStart(const ScenarioContext& ctx) {
        ownRank_ = ctx.isHost ? 0u : 1u;
        haveOwn_ = resolveRankContainer(ctx.gw, ownRank_, ownHand_);
        char b[160];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO INVB anchor own_rank=%u have=%d hand=%u,%u,%u,%u,%u",
            ownRank_, haveOwn_ ? 1 : 0,
            ownHand_[0], ownHand_[1], ownHand_[2], ownHand_[3], ownHand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            // Sample BOTH squad-tab containers so each client logs its OWN container
            // (authoritative truth it streams) and the PEER's (the one it reconciles).
            for (unsigned int rank = 0; rank < 2; ++rank) {
                unsigned int cHand[5];
                if (!resolveRankContainer(ctx.gw, rank, cHand)) continue;
                InvItemEntry items[INV_ITEMS_MAX];
                unsigned int hash = 0;
                unsigned int n = engine::captureContainerContents(
                    ctx.gw, cHand, items, INV_ITEMS_MAX, &hash);
                const char* role = (rank == ownRank_) ? "OWN" : "PEER";
                if (rank == ownRank_) {
                    if (samples_ == 0) { firstOwnCount_ = n; prevOwnHash_ = hash; }
                    else if (hash != prevOwnHash_) { ++distinctOwn_; prevOwnHash_ = hash; }
                    lastOwnCount_ = n; ++samples_;
                }
                char b[160];
                _snprintf(b, sizeof(b) - 1,
                    "SCENARIO INVB r=%u %s t=%lu count=%u hash=%u",
                    rank, role, (unsigned long)ctx.elapsedMs, n, hash);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }

            // Mutate ONLY our OWN container: ADD a burst, then REMOVE part of it. The
            // remove forces the peer to converge DOWN (not just up), so a removal that
            // failed to propagate would leave the peer stuck above the author's count.
            if (haveOwn_) {
                int addN = ctx.isHost ? 3 : 2; // host ends +2 over baseline, join ends +1
                int remN = 1;
                if (!added_ && ctx.elapsedMs >= ADD_MS) {
                    added_ = true;
                    char sid[48]; sid[0] = '\0';
                    int got = engine::addTestItemsToContainer(ctx.gw, ownHand_, addN, sid, sizeof(sid));
                    char m[200];
                    _snprintf(m, sizeof(m) - 1,
                        "SCENARIO INVB ADD r=%u n=%d sid='%s'", ownRank_, got, sid[0] ? sid : "(none)");
                    m[sizeof(m) - 1] = '\0'; coop::logLine(m);
                }
                if (added_ && !removed_ && ctx.elapsedMs >= REM_MS) {
                    removed_ = true;
                    int got = engine::removeTestItemsFromContainer(ctx.gw, ownHand_, remN);
                    char m[160];
                    _snprintf(m, sizeof(m) - 1, "SCENARIO INVB REM r=%u n=%d", ownRank_, got);
                    m[sizeof(m) - 1] = '\0'; coop::logLine(m);
                }
            }
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            // In-plugin verdict only confirms the scenario EXECUTED (resolved its owned
            // container, sampled, and - on each side - performed its add+remove). The
            // runner's per-rank cross-client convergence check (Test-InventoryBidir) is
            // the authoritative no-loss/no-dupe gate.
            passed_ = haveOwn_ && (samples_ > 0) && added_ && removed_;
            return true;
        }
        return false;
    }

private:
    // Resolve the lowest-hand squad member whose squad-tab CONTAINER has the given rank
    // (distinct hand-containers, sorted - the SAME key the Replicator partitions on) and
    // write its object hand (== its personal-inventory container hand) to out.
    static bool resolveRankContainer(GameWorld* gw, unsigned int rank, unsigned int out[5]) {
        for (int i = 0; i < 5; ++i) out[i] = 0;
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, MAX_SQUAD);
        if (n == 0) return false;
        int best = -1;
        for (unsigned int i = 0; i < n; ++i) {
            int cr = containerRankOf(sq, n, i);
            if (cr < 0 || (unsigned int)cr != rank) continue;
            if (best < 0 || handLess(sq[i], sq[best])) best = (int)i;
        }
        if (best < 0) return false;
        out[0] = sq[best].hType; out[1] = sq[best].hContainer;
        out[2] = sq[best].hContainerSerial; out[3] = sq[best].hIndex; out[4] = sq[best].hSerial;
        return true;
    }
    static bool handLess(const EntityState& a, const EntityState& b) {
        if (a.hType != b.hType) return a.hType < b.hType;
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        if (a.hContainerSerial != b.hContainerSerial) return a.hContainerSerial < b.hContainerSerial;
        if (a.hIndex != b.hIndex) return a.hIndex < b.hIndex;
        return a.hSerial < b.hSerial;
    }
    static bool ctnrLess(const EntityState& a, const EntityState& b) {
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        return a.hContainerSerial < b.hContainerSerial;
    }
    static bool ctnrEq(const EntityState& a, const EntityState& b) {
        return a.hContainer == b.hContainer && a.hContainerSerial == b.hContainerSerial;
    }
    static int containerRankOf(const EntityState* sq, unsigned int n, unsigned int i) {
        EntityState distinct[MAX_SQUAD]; unsigned int dn = 0;
        for (unsigned int a = 0; a < n; ++a) {
            bool seen = false;
            for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[a])) { seen = true; break; }
            if (!seen && dn < MAX_SQUAD) distinct[dn++] = sq[a];
        }
        for (unsigned int a = 1; a < dn; ++a)
            for (unsigned int b = a; b > 0 && ctnrLess(distinct[b], distinct[b-1]); --b) {
                EntityState t = distinct[b]; distinct[b] = distinct[b-1]; distinct[b-1] = t;
            }
        for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[i])) return (int)b;
        return -1;
    }

    static const unsigned long HOST_DURATION_MS = 44000; // outlive the join's window
    static const unsigned long JOIN_DURATION_MS = 28000;
    static const unsigned long ADD_MS           = 8000;  // baseline, then add
    static const unsigned long REM_MS           = 14000; // then remove part of it

    static const unsigned int  MAX_SQUAD        = 32;

    bool          haveOwn_;
    bool          added_;
    bool          removed_;
    unsigned long lastLogMs_;
    unsigned int  samples_;
    unsigned int  ownRank_;
    unsigned int  ownHand_[5];
    unsigned int  firstOwnCount_;
    unsigned int  lastOwnCount_;
    unsigned int  prevOwnHash_;
    unsigned int  distinctOwn_;
};

// trade_probe (protocol-36 BASELINE, evidence not a gate): characterize what happens
// TODAY when a player performs a direct CROSS-OWNER drag - the field-reported dupe /
// wipe / weapon-vanish. The HOST plays the "dragger": it locally relocates real items
// between the join-owned (rank 1) and host-owned (rank 0) squad containers via
// engine::moveItemBetweenContainers (the same engine mutation the UI drag performs),
// which violates the single-writer inventory model on purpose:
//   TAKE  @16s: 1 common item  rank1 -> rank0  (drag OUT of the peer's bag)
//   GIVE  @26s: 1 common item  rank0 -> rank1  (drag INTO the peer's bag)
//   WTAKE @36s: 1 WEAPON       rank1 -> rank0  (the vanish case: no fabrication path)
// Both clients seed their OWN container @6s (join +3 / host +2 commons) so material
// exists, and sample BOTH containers every 500 ms, logging per-container count/hash
// plus the tracked probe-item and weapon quantities. The runner's Test-TradeProbe
// reads the series from both logs and reports the conservation outcome per move
// (dupe / loss / clean) - the log IS the deliverable; nothing here gates sync quality.
class TradeScenario : public TimedScenario {
public:
    explicit TradeScenario(bool peer)
        : TimedScenario(peer ? "trade_peer" : "trade_probe", 0),
          peer_(peer), tag_(peer ? "TRDE" : "TRDP"),
          hostDur_(peer ? 70000UL : 68000UL), joinDur_(peer ? 56000UL : 52000UL),
          lastLogMs_(0), samples_(0),
          seedDone_(false), takeDone_(false), giveDone_(false), wpnDone_(false),
          probeType_(0), wpnType_(0), wpnLatched_(false),
          firstDone_(false), firstWpn0_(0), firstWpn1_(0),
          lastWpn0_(0), lastWpn1_(0) {
        probeSid_[0] = '\0'; wpnSid_[0] = '\0';
        for (int r = 0; r < 2; ++r) { rankHave_[r] = false; for (int k = 0; k < 5; ++k) rankHand_[r][k] = 0; }
    }

    virtual void onStart(const ScenarioContext& ctx) {
        for (unsigned int r = 0; r < 2; ++r)
            rankHave_[r] = resolveRankContainer(ctx.gw, r, rankHand_[r]);
        engine::commonTestItemSid(ctx.gw, probeSid_, sizeof(probeSid_), &probeType_);
        char b[200];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO %s anchor host=%d r0=%d r1=%d probeSid='%s' probeType=%u",
            tag_, ctx.isHost ? 1 : 0, rankHave_[0] ? 1 : 0, rankHave_[1] ? 1 : 0,
            probeSid_[0] ? probeSid_ : "(none)", probeType_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            // Latch the tracked WEAPON deterministically on BOTH clients: the
            // lexicographically smallest weapon sid in the join-owned (rank 1)
            // container at first sample. Same save -> same pick on each side, so
            // the two logs track the same item without exchanging anything.
            if (!wpnLatched_ && rankHave_[1]) latchWeapon(ctx.gw);
            int wpnNow[2] = { 0, 0 };
            bool sampledBoth = true;
            for (unsigned int rank = 0; rank < 2; ++rank) {
                if (!rankHave_[rank]) { sampledBoth = false; continue; }
                InvItemEntry items[INV_ITEMS_MAX];
                unsigned int hash = 0;
                unsigned int n = engine::captureContainerContents(
                    ctx.gw, rankHand_[rank], items, INV_ITEMS_MAX, &hash);
                if (n == 0) sampledBoth = false;
                int probeQty = 0, wpnQty = 0;
                for (unsigned int i = 0; i < n; ++i) {
                    if (probeSid_[0] && items[i].itemType == probeType_ &&
                        strcmp(items[i].stringID, probeSid_) == 0)
                        probeQty += (int)items[i].quantity;
                    if (wpnSid_[0] && items[i].itemType == WEAPON_CAT &&
                        strcmp(items[i].stringID, wpnSid_) == 0)
                        wpnQty += (int)items[i].quantity;
                }
                wpnNow[rank] = wpnQty;
                char b[200];
                _snprintf(b, sizeof(b) - 1,
                    "SCENARIO %s r=%u %s t=%lu count=%u hash=%u probe=%d wpn=%d",
                    tag_, rank, (ctx.isHost == (rank == 0)) ? "OWN" : "PEER",
                    (unsigned long)ctx.elapsedMs, n, hash, probeQty, wpnQty);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                ++samples_;
            }
            // trade_peer conservation tracking; inert in probe mode (emits nothing).
            if (sampledBoth) {
                if (!firstDone_ && wpnLatched_) {
                    firstDone_ = true;
                    firstWpn0_ = wpnNow[0]; firstWpn1_ = wpnNow[1];
                }
                lastWpn0_ = wpnNow[0]; lastWpn1_ = wpnNow[1];
            }

            // Seed material into the container each side OWNS (ordinary, supported
            // single-writer adds - these also prove baseline sync is alive).
            if (!seedDone_ && ctx.elapsedMs >= SEED_MS) {
                seedDone_ = true;
                unsigned int ownRank = ctx.isHost ? 0u : 1u;
                if (rankHave_[ownRank]) {
                    char sid[48]; sid[0] = '\0';
                    int got = engine::addTestItemsToContainer(
                        ctx.gw, rankHand_[ownRank], ctx.isHost ? 2 : 3, sid, sizeof(sid));
                    char m[200];
                    _snprintf(m, sizeof(m) - 1, "SCENARIO %s SEED r=%u n=%d sid='%s'",
                              tag_, ownRank, got, sid[0] ? sid : "(none)");
                    m[sizeof(m) - 1] = '\0'; coop::logLine(m);
                }
            }

            // The cross-owner drags: HOST only (the "player A" of the field report).
            if (ctx.isHost && probeSid_[0] && rankHave_[0] && rankHave_[1]) {
                if (!takeDone_ && ctx.elapsedMs >= TAKE_MS) {
                    takeDone_ = true;
                    int got = engine::moveItemBetweenContainers(
                        ctx.gw, rankHand_[1], rankHand_[0], probeSid_, probeType_, 1);
                    logMove("TAKE", got, probeSid_);
                }
                if (!giveDone_ && ctx.elapsedMs >= GIVE_MS) {
                    giveDone_ = true;
                    int got = engine::moveItemBetweenContainers(
                        ctx.gw, rankHand_[0], rankHand_[1], probeSid_, probeType_, 1);
                    logMove("GIVE", got, probeSid_);
                }
                if (!wpnDone_ && ctx.elapsedMs >= WPN_MS) {
                    wpnDone_ = true;
                    int got = wpnSid_[0]
                        ? engine::moveItemBetweenContainers(
                              ctx.gw, rankHand_[1], rankHand_[0], wpnSid_, WEAPON_CAT, 1)
                        : -1; // no weapon found in the join-owned container
                    logMove("WTAKE", got, wpnSid_[0] ? wpnSid_ : "(none)");
                }
            }
        }

        unsigned long dur = ctx.isHost ? hostDur_ : joinDur_;
        if (ctx.elapsedMs >= dur) {
            bool executed = rankHave_[0] && rankHave_[1] && samples_ > 0 && seedDone_ &&
                            (!ctx.isHost || (takeDone_ && giveDone_ && wpnDone_));
            if (!peer_) {
                // trade_probe: verdict = the probe EXECUTED (containers resolved,
                // sampled, and - on the host - all three cross-owner drags fired). The
                // BEHAVIOR it recorded is judged by the runner's evidence report.
                passed_ = executed;
                return true;
            }
            // trade_peer: additionally gate LOCAL weapon conservation - total
            // unchanged (no vanish, no dupe) and, once a weapon was actually tracked,
            // it ended up in rank 0 (moved, not vanished).
            bool wpnOk = true;
            if (firstDone_ && wpnSid_[0]) {
                wpnOk = (lastWpn0_ + lastWpn1_) == (firstWpn0_ + firstWpn1_) &&
                        lastWpn0_ == firstWpn0_ + 1 && lastWpn1_ == firstWpn1_ - 1;
            }
            char m[220];
            _snprintf(m, sizeof(m) - 1,
                "SCENARIO TRDE verdict executed=%d wpnOk=%d wpn r0 %d->%d r1 %d->%d sid='%s'",
                executed ? 1 : 0, wpnOk ? 1 : 0, firstWpn0_, lastWpn0_,
                firstWpn1_, lastWpn1_, wpnSid_[0] ? wpnSid_ : "(none)");
            m[sizeof(m) - 1] = '\0'; coop::logLine(m);
            passed_ = executed && wpnOk;
            return true;
        }
        return false;
    }

private:
    void latchWeapon(GameWorld* gw) {
        InvItemEntry items[INV_ITEMS_MAX];
        unsigned int hash = 0;
        unsigned int n = engine::captureContainerContents(
            gw, rankHand_[1], items, INV_ITEMS_MAX, &hash);
        if (n == 0) return;          // container not readable yet - retry next sample
        wpnLatched_ = true;          // readable: latch now even if it holds no weapon
        for (unsigned int i = 0; i < n; ++i) {
            if (items[i].itemType != WEAPON_CAT) continue;
            if (!wpnSid_[0] || strcmp(items[i].stringID, wpnSid_) < 0) {
                strncpy(wpnSid_, items[i].stringID, sizeof(wpnSid_) - 1);
                wpnSid_[sizeof(wpnSid_) - 1] = '\0';
                wpnType_ = items[i].itemType;
            }
        }
        char b[160];
        _snprintf(b, sizeof(b) - 1, "SCENARIO %s wpn latched sid='%s'",
                  tag_, wpnSid_[0] ? wpnSid_ : "(none)");
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }
    void logMove(const char* what, int got, const char* sid) {
        char m[200];
        _snprintf(m, sizeof(m) - 1, "SCENARIO %s %s n=%d sid='%s'", tag_, what, got, sid);
        m[sizeof(m) - 1] = '\0'; coop::logLine(m);
    }
    // Same squad-tab -> rank partition the Replicator / inv_bidir use.
    static bool resolveRankContainer(GameWorld* gw, unsigned int rank, unsigned int out[5]) {
        for (int i = 0; i < 5; ++i) out[i] = 0;
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, MAX_SQUAD);
        if (n == 0) return false;
        int best = -1;
        for (unsigned int i = 0; i < n; ++i) {
            int cr = containerRankOf(sq, n, i);
            if (cr < 0 || (unsigned int)cr != rank) continue;
            if (best < 0 || handLess(sq[i], sq[best])) best = (int)i;
        }
        if (best < 0) return false;
        out[0] = sq[best].hType; out[1] = sq[best].hContainer;
        out[2] = sq[best].hContainerSerial; out[3] = sq[best].hIndex; out[4] = sq[best].hSerial;
        return true;
    }
    static bool handLess(const EntityState& a, const EntityState& b) {
        if (a.hType != b.hType) return a.hType < b.hType;
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        if (a.hContainerSerial != b.hContainerSerial) return a.hContainerSerial < b.hContainerSerial;
        if (a.hIndex != b.hIndex) return a.hIndex < b.hIndex;
        return a.hSerial < b.hSerial;
    }
    static bool ctnrLess(const EntityState& a, const EntityState& b) {
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        return a.hContainerSerial < b.hContainerSerial;
    }
    static bool ctnrEq(const EntityState& a, const EntityState& b) {
        return a.hContainer == b.hContainer && a.hContainerSerial == b.hContainerSerial;
    }
    static int containerRankOf(const EntityState* sq, unsigned int n, unsigned int i) {
        EntityState distinct[MAX_SQUAD]; unsigned int dn = 0;
        for (unsigned int a = 0; a < n; ++a) {
            bool seen = false;
            for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[a])) { seen = true; break; }
            if (!seen && dn < MAX_SQUAD) distinct[dn++] = sq[a];
        }
        for (unsigned int a = 1; a < dn; ++a)
            for (unsigned int b = a; b > 0 && ctnrLess(distinct[b], distinct[b-1]); --b) {
                EntityState t = distinct[b]; distinct[b] = distinct[b-1]; distinct[b-1] = t;
            }
        for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[i])) return (int)b;
        return -1;
    }

    // The last drag fires @36s; the slowest downstream machinery (W2 weapon census
    // 30-tick debounce + 1.8 s removal settle + snapshot travel) lands well inside
    // each mode's window. trade_peer runs ~2 s longer - the owner republish that
    // clears the 10 s reconcile-suppression latch must land - and the host always
    // outlives the join's window.
    static const unsigned long SEED_MS          = 6000;
    static const unsigned long TAKE_MS          = 16000;
    static const unsigned long GIVE_MS          = 26000;
    static const unsigned long WPN_MS           = 36000;

    static const unsigned int  MAX_SQUAD  = 32;
    static const unsigned int  WEAPON_CAT = 2;

    bool          peer_;      // false = trade_probe (baseline), true = trade_peer (xfer)
    const char*   tag_;       // "TRDP" (probe) / "TRDE" (peer) - the oracle log tag
    unsigned long hostDur_;
    unsigned long joinDur_;
    unsigned long lastLogMs_;
    unsigned int  samples_;
    bool          seedDone_;
    bool          takeDone_;
    bool          giveDone_;
    bool          wpnDone_;
    char          probeSid_[48];
    unsigned int  probeType_;
    char          wpnSid_[48];
    unsigned int  wpnType_;
    bool          wpnLatched_;
    bool          firstDone_;             // trade_peer: conservation baseline latched
    int           firstWpn0_, firstWpn1_; // trade_peer: tracked-weapon counts at baseline
    int           lastWpn0_,  lastWpn1_;  // trade_peer: latest tracked-weapon counts
    bool          rankHave_[2];
    unsigned int  rankHand_[2][5];
};

// xfer_block (cross-owner trade VETO validation): with KENSHICOOP_BLOCK_XFER on, a
// direct squad-to-squad drag between DIFFERENT-owner tabs must be REFUSED at the
// engine (the item is conserved in the source bag), while a SAME-owner drag still
// succeeds. The HOST drives both via engine::moveItemBetweenContainers(...,
// suspendVeto=false) - the exact remove+add a UI drag performs, subject to the veto:
//   GIVE @16s: 1 common  rank0(host) -> rank1(join)          -> BLOCKED (moved=0)
//   SELF @26s: 1 common  rank0.memberA -> rank0.memberB      -> ALLOWED (moved>=1)
// Both clients seed the host tab @6s and sample both tabs every 500 ms. The in-plugin
// verdict gates on: the cross-owner move returned 0 AND the source/dest probe counts
// are UNCHANGED after it (nothing crossed), and - when a second host-tab member exists
// - the same-owner move succeeded. Protocol 37 is retired under the veto, so no
// PKT_INV_XFER is emitted (the runner cross-checks the logs for the absence).
class XferBlockScenario : public TimedScenario {
public:
    XferBlockScenario()
        : TimedScenario("xfer_block", 0), lastLogMs_(0), samples_(0), seedDone_(false),
          giveDone_(false), selfDone_(false), probeType_(0), haveSelfDst_(false),
          giveMoved_(-2), selfMoved_(-2),
          r0BeforeGive_(-1), r1BeforeGive_(-1), r0AfterGive_(-1), r1AfterGive_(-1) {
        probeSid_[0] = '\0';
        for (int r = 0; r < 2; ++r) { rankHave_[r] = false; for (int k = 0; k < 5; ++k) rankHand_[r][k] = 0; }
        for (int k = 0; k < 5; ++k) selfDst_[k] = 0;
    }

    virtual void onStart(const ScenarioContext& ctx) {
        for (unsigned int r = 0; r < 2; ++r)
            rankHave_[r] = resolveRankMember(ctx.gw, r, 0, rankHand_[r]);
        haveSelfDst_ = resolveRankMember(ctx.gw, 0, 1, selfDst_); // 2nd host-tab member (if any)
        engine::commonTestItemSid(ctx.gw, probeSid_, sizeof(probeSid_), &probeType_);
        char b[220];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO XFB anchor host=%d r0=%d r1=%d selfDst=%d probeSid='%s' probeType=%u",
            ctx.isHost ? 1 : 0, rankHave_[0] ? 1 : 0, rankHave_[1] ? 1 : 0,
            haveSelfDst_ ? 1 : 0, probeSid_[0] ? probeSid_ : "(none)", probeType_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            int probe[2] = { -1, -1 };
            for (unsigned int rank = 0; rank < 2; ++rank) {
                if (!rankHave_[rank]) continue;
                InvItemEntry items[INV_ITEMS_MAX]; unsigned int hash = 0;
                unsigned int n = engine::captureContainerContents(
                    ctx.gw, rankHand_[rank], items, INV_ITEMS_MAX, &hash);
                int pq = 0;
                for (unsigned int i = 0; i < n; ++i)
                    if (probeSid_[0] && items[i].itemType == probeType_ &&
                        strcmp(items[i].stringID, probeSid_) == 0)
                        pq += (int)items[i].quantity;
                probe[rank] = pq;
                char b[200];
                _snprintf(b, sizeof(b) - 1,
                    "SCENARIO XFB r=%u %s t=%lu count=%u hash=%u probe=%d",
                    rank, (ctx.isHost == (rank == 0)) ? "OWN" : "PEER",
                    (unsigned long)ctx.elapsedMs, n, hash, pq);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
                ++samples_;
            }

            // Seed material into the container each side OWNS (baseline sync liveness).
            if (!seedDone_ && ctx.elapsedMs >= SEED_MS) {
                seedDone_ = true;
                unsigned int ownRank = ctx.isHost ? 0u : 1u;
                if (rankHave_[ownRank]) {
                    char sid[48]; sid[0] = '\0';
                    int got = engine::addTestItemsToContainer(ctx.gw, rankHand_[ownRank], 3, sid, sizeof(sid));
                    char m[200]; _snprintf(m, sizeof(m) - 1, "SCENARIO XFB SEED r=%u n=%d sid='%s'",
                                           ownRank, got, sid[0] ? sid : "(none)");
                    m[sizeof(m) - 1] = '\0'; coop::logLine(m);
                }
            }

            // HOST drives the drags AS a UI drag would (subject to the veto).
            if (ctx.isHost && probeSid_[0] && rankHave_[0] && rankHave_[1]) {
                if (!giveDone_ && ctx.elapsedMs >= GIVE_MS) {
                    giveDone_ = true;
                    r0BeforeGive_ = probe[0]; r1BeforeGive_ = probe[1];
                    giveMoved_ = engine::moveItemBetweenContainers(
                        ctx.gw, rankHand_[0], rankHand_[1], probeSid_, probeType_, 1, /*suspendVeto*/false);
                    char m[160]; _snprintf(m, sizeof(m) - 1,
                        "SCENARIO XFB GIVE moved=%d (expect 0=blocked)", giveMoved_);
                    m[sizeof(m) - 1] = '\0'; coop::logLine(m);
                }
                // Capture post-GIVE counts a sample later (nothing should have crossed).
                if (giveDone_ && r0AfterGive_ < 0 && ctx.elapsedMs >= GIVE_MS + 1000) {
                    r0AfterGive_ = probe[0]; r1AfterGive_ = probe[1];
                }
                if (!selfDone_ && ctx.elapsedMs >= SELF_MS) {
                    selfDone_ = true;
                    selfMoved_ = haveSelfDst_
                        ? engine::moveItemBetweenContainers(
                              ctx.gw, rankHand_[0], selfDst_, probeSid_, probeType_, 1, /*suspendVeto*/false)
                        : -1; // no second host-tab member on this save; sub-check skipped
                    char m[160]; _snprintf(m, sizeof(m) - 1,
                        "SCENARIO XFB SELF moved=%d (expect >=1=allowed; -1=skipped)", selfMoved_);
                    m[sizeof(m) - 1] = '\0'; coop::logLine(m);
                }
            }
        }

        unsigned long dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            bool executed = rankHave_[0] && rankHave_[1] && samples_ > 0 && seedDone_ &&
                            (!ctx.isHost || (giveDone_ && selfDone_));
            bool blockOk = true, selfOk = true, conserveOk = true;
            if (ctx.isHost) {
                blockOk = (giveMoved_ == 0);                     // cross-owner drag refused
                if (r0AfterGive_ >= 0 && r1AfterGive_ >= 0 && r0BeforeGive_ >= 0)
                    conserveOk = (r0AfterGive_ == r0BeforeGive_) && (r1AfterGive_ == r1BeforeGive_);
                selfOk = !haveSelfDst_ || (selfMoved_ >= 1);     // same-owner drag still works
            }
            char m[220];
            _snprintf(m, sizeof(m) - 1,
                "SCENARIO XFB verdict executed=%d blockOk=%d conserveOk=%d selfOk=%d give=%d self=%d",
                executed ? 1 : 0, blockOk ? 1 : 0, conserveOk ? 1 : 0, selfOk ? 1 : 0,
                giveMoved_, selfMoved_);
            m[sizeof(m) - 1] = '\0'; coop::logLine(m);
            passed_ = executed && blockOk && conserveOk && selfOk;
            return true;
        }
        return false;
    }

private:
    // Resolve the ordinal-th lowest-hand member of the squad TAB with the given rank
    // (the same tab->rank partition the Replicator and the sibling scenarios use).
    static bool resolveRankMember(GameWorld* gw, unsigned int rank, unsigned int ordinal,
                                  unsigned int out[5]) {
        for (int i = 0; i < 5; ++i) out[i] = 0;
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, MAX_SQUAD);
        if (n == 0) return false;
        unsigned int idx[MAX_SQUAD]; unsigned int m = 0;
        for (unsigned int i = 0; i < n; ++i) {
            int cr = containerRankOf(sq, n, i);
            if (cr >= 0 && (unsigned int)cr == rank && m < MAX_SQUAD) idx[m++] = i;
        }
        for (unsigned int a = 1; a < m; ++a)
            for (unsigned int b = a; b > 0 && handLess(sq[idx[b]], sq[idx[b-1]]); --b) {
                unsigned int t = idx[b]; idx[b] = idx[b-1]; idx[b-1] = t;
            }
        if (ordinal >= m) return false;
        unsigned int i = idx[ordinal];
        out[0] = sq[i].hType; out[1] = sq[i].hContainer;
        out[2] = sq[i].hContainerSerial; out[3] = sq[i].hIndex; out[4] = sq[i].hSerial;
        return true;
    }
    static bool handLess(const EntityState& a, const EntityState& b) {
        if (a.hType != b.hType) return a.hType < b.hType;
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        if (a.hContainerSerial != b.hContainerSerial) return a.hContainerSerial < b.hContainerSerial;
        if (a.hIndex != b.hIndex) return a.hIndex < b.hIndex;
        return a.hSerial < b.hSerial;
    }
    static bool ctnrLess(const EntityState& a, const EntityState& b) {
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        return a.hContainerSerial < b.hContainerSerial;
    }
    static bool ctnrEq(const EntityState& a, const EntityState& b) {
        return a.hContainer == b.hContainer && a.hContainerSerial == b.hContainerSerial;
    }
    static int containerRankOf(const EntityState* sq, unsigned int n, unsigned int i) {
        EntityState distinct[MAX_SQUAD]; unsigned int dn = 0;
        for (unsigned int a = 0; a < n; ++a) {
            bool seen = false;
            for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[a])) { seen = true; break; }
            if (!seen && dn < MAX_SQUAD) distinct[dn++] = sq[a];
        }
        for (unsigned int a = 1; a < dn; ++a)
            for (unsigned int b = a; b > 0 && ctnrLess(distinct[b], distinct[b-1]); --b) {
                EntityState t = distinct[b]; distinct[b] = distinct[b-1]; distinct[b-1] = t;
            }
        for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[i])) return (int)b;
        return -1;
    }

    static const unsigned long HOST_DURATION_MS = 44000; // outlive the join's window
    static const unsigned long JOIN_DURATION_MS = 34000;
    static const unsigned long SEED_MS          = 6000;
    static const unsigned long GIVE_MS          = 16000; // cross-owner drag (blocked)
    static const unsigned long SELF_MS          = 26000; // same-owner drag (allowed)
    static const unsigned int  MAX_SQUAD        = 32;

    unsigned long lastLogMs_;
    unsigned int  samples_;
    bool          seedDone_;
    bool          giveDone_;
    bool          selfDone_;
    char          probeSid_[48];
    unsigned int  probeType_;
    bool          haveSelfDst_;
    int           giveMoved_;
    int           selfMoved_;
    int           r0BeforeGive_, r1BeforeGive_, r0AfterGive_, r1AfterGive_;
    bool          rankHave_[2];
    unsigned int  rankHand_[2][5];
    unsigned int  selfDst_[5];
};

// inv_equip: EQUIPPED-gear (armour/weapon slot) sync. Each client owns one squad tab
// (host rank 0, join rank 1). On the geared member of its OWN tab it UNEQUIPS one REAL
// (save-loaded) worn item and leaves it off - the "drop/unequip armour" action. Because
// the peer loaded the same save, its local copy of that character starts wearing the
// SAME gear, so converging to "one fewer worn item" forces it to actively REMOVE its
// worn copy; a removal that failed to propagate (loose-only sync's blind spot, the bug
// the user hit) would leave the peer still wearing it - a permanent eq/hash mismatch.
// The snapshot now carries each item's equipped flag + slot. The runner cross-checks,
// per rank, that the author's worn count dropped and the NON-authoring side converged
// to the author's FINAL worn state (count + equipped-count + content hash). Fabricated
// re-equips don't persist in the engine, so the equip (up) path is intentionally out of
// scope here. Requires a shared save with >=2 squad tabs whose members wear gear.
class InventoryEquipScenario : public TimedScenario {
public:
    explicit InventoryEquipScenario(bool reequipMode = false)
        : TimedScenario(reequipMode ? "inv_reequip" : "inv_equip", 0),
          haveOwn_(false), haveEq_(false), unequipped_(false),
          reequipped_(false), reequipMode_(reequipMode),
          lastLogMs_(0), samples_(0), ownRank_(0),
          baseEqCount_(0), baseType_(0), lastOwnEq_(0) {
        for (int i = 0; i < 5; ++i) ownHand_[i] = 0;
        for (int r = 0; r < 2; ++r) { rankHave_[r] = false; for (int i = 0; i < 5; ++i) rankHand_[r][i] = 0; }
        baseSid_[0] = '\0';
    }

    virtual void onStart(const ScenarioContext& ctx) {
        ownRank_ = ctx.isHost ? 0u : 1u;
        // Resolve+cache the geared member of BOTH tabs ONCE (the lead isn't always the
        // geared one). Caching keeps sampling locked to the same character even after we
        // unequip - so the OWN/PEER series track one member, not a shifting "first geared"
        // pick. Both clients share the save, so each rank resolves to the SAME member.
        for (unsigned int r = 0; r < 2; ++r)
            rankHave_[r] = resolveGearedRankContainer(ctx.gw, r, rankHand_[r]);
        haveOwn_ = rankHave_[ownRank_];
        if (haveOwn_) {
            for (int i = 0; i < 5; ++i) ownHand_[i] = rankHand_[ownRank_][i];
            haveEq_ = engine::findEquippedItemKey(ctx.gw, ownHand_, baseSid_, sizeof(baseSid_),
                                                  &baseType_, &baseEqCount_) != 0;
        }
        char b[220];
        _snprintf(b, sizeof(b) - 1,
            "SCENARIO INVE anchor own_rank=%u have=%d eq=%d baseEq=%d sid='%s' type=%u",
            ownRank_, haveOwn_ ? 1 : 0, haveEq_ ? 1 : 0, baseEqCount_,
            baseSid_[0] ? baseSid_ : "(none)", baseType_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            // Sample BOTH squad-tab containers: each client logs its OWN tab (the worn
            // state it streams) and the PEER's (the one it reconciles), with the count
            // of EQUIPPED items broken out so the runner can prove the slot - not just
            // a loose copy - converged.
            for (unsigned int rank = 0; rank < 2; ++rank) {
                if (!rankHave_[rank]) continue;
                InvItemEntry items[INV_ITEMS_MAX];
                unsigned int hash = 0;
                unsigned int n = engine::captureContainerContents(
                    ctx.gw, rankHand_[rank], items, INV_ITEMS_MAX, &hash);
                unsigned int eq = 0;
                for (unsigned int i = 0; i < n; ++i) if (items[i].equipped) ++eq;
                const char* role = (rank == ownRank_) ? "OWN" : "PEER";
                if (rank == ownRank_) { ++samples_; lastOwnEq_ = eq; }
                char b[180];
                _snprintf(b, sizeof(b) - 1,
                    "SCENARIO INVE r=%u %s t=%lu count=%u eq=%u hash=%u",
                    rank, role, (unsigned long)ctx.elapsedMs, n, eq, hash);
                b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            }

            // UNEQUIP one REAL worn item from our OWN geared member and leave it off
            // (the "drop/unequip armour" action). Because the peer loaded the SAME save,
            // its local copy of this character starts wearing the SAME gear; to converge
            // to "one fewer worn item" it must actively REMOVE its worn copy. A removal
            // that failed to propagate (the user's bug) would leave the peer still
            // wearing it - a permanent eq/hash mismatch. Fabricated re-equips don't
            // persist in the engine, so we deliberately test only this reliable path.
            if (haveOwn_ && haveEq_) {
                unsigned long unequipAt = reequipMode_ ? RE_UNEQUIP_MS : UNEQUIP_MS;
                if (!unequipped_ && ctx.elapsedMs >= unequipAt) {
                    unequipped_ = true;
                    // inv_equip DESTROYS the worn item (down path, ends reduced). inv_reequip
                    // MOVES it to loose (preserving identity) so it can be re-equipped below -
                    // the faithful "drag worn item into the bag, then back onto the body" cycle.
                    int got = reequipMode_
                        ? engine::unequipItemToLoose(ctx.gw, ownHand_, baseSid_, baseType_, 1)
                        : engine::removeEquippedItem(ctx.gw, ownHand_, baseSid_, baseType_, 1);
                    logStep(ctx, "UNEQUIP", got);
                }
                // RE-EQUIP (up path): equip the REAL loose item we just unequipped. Equipping
                // a real (not fabricated) item persists, so the slot fills back in - and the
                // observer, which down-moved its copy to loose when it saw the unequip, must
                // now UP-move it to converge. A broken up path leaves the observer's copy loose.
                if (reequipMode_ && unequipped_ && !reequipped_ && ctx.elapsedMs >= RE_REEQUIP_MS) {
                    reequipped_ = true;
                    int got = engine::reequipLooseItem(ctx.gw, ownHand_, baseSid_, baseType_, 1);
                    logStep(ctx, "REEQUIP", got);
                }
            }
        }

        unsigned long dur;
        if (reequipMode_) dur = ctx.isHost ? RE_HOST_DURATION_MS : RE_JOIN_DURATION_MS;
        else              dur = ctx.isHost ? HOST_DURATION_MS : JOIN_DURATION_MS;
        if (ctx.elapsedMs >= dur) {
            // In-plugin verdict confirms the scenario EXECUTED. For inv_equip: resolved a
            // geared member, sampled, unequipped. For inv_reequip: additionally re-equipped
            // AND the own worn count returned to its baseline peak (local proof the re-equip
            // of a REAL item PERSISTED - the d25 risk). The runner's per-rank cross-client
            // convergence check is the authoritative gate that the move replicated.
            if (reequipMode_)
                passed_ = haveOwn_ && haveEq_ && (samples_ > 0) && unequipped_ && reequipped_ &&
                          ((int)lastOwnEq_ >= baseEqCount_);
            else
                passed_ = haveOwn_ && haveEq_ && (samples_ > 0) && unequipped_;
            return true;
        }
        return false;
    }

private:
    void logStep(const ScenarioContext& ctx, const char* what, int got) {
        char m[200];
        _snprintf(m, sizeof(m) - 1, "SCENARIO INVE %s r=%u n=%d sid='%s'",
                  what, ownRank_, got, baseSid_[0] ? baseSid_ : "(none)");
        m[sizeof(m) - 1] = '\0'; coop::logLine(m);
        (void)ctx;
    }
    // Same squad-tab -> rank partitioning the Replicator and inv_bidir use, but among the
    // members of `rank`'s tab pick the LOWEST-HAND one that actually WEARS gear (eq >= 1).
    // Both clients share the save, so this resolves to the SAME member on each side - the
    // OWN/PEER comparison stays aligned. Falls back to false if no tab member wears gear.
    static bool resolveGearedRankContainer(GameWorld* gw, unsigned int rank, unsigned int out[5]) {
        for (int i = 0; i < 5; ++i) out[i] = 0;
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, MAX_SQUAD);
        if (n == 0) return false;
        // Index list of this rank's members, sorted ascending by hand (deterministic).
        unsigned int idx[MAX_SQUAD]; unsigned int m = 0;
        for (unsigned int i = 0; i < n; ++i) {
            int cr = containerRankOf(sq, n, i);
            if (cr >= 0 && (unsigned int)cr == rank && m < MAX_SQUAD) idx[m++] = i;
        }
        for (unsigned int a = 1; a < m; ++a)
            for (unsigned int b = a; b > 0 && handLess(sq[idx[b]], sq[idx[b-1]]); --b) {
                unsigned int t = idx[b]; idx[b] = idx[b-1]; idx[b-1] = t;
            }
        for (unsigned int a = 0; a < m; ++a) {
            unsigned int h[5] = { sq[idx[a]].hType, sq[idx[a]].hContainer,
                sq[idx[a]].hContainerSerial, sq[idx[a]].hIndex, sq[idx[a]].hSerial };
            char sid[48]; unsigned int ty = 0; int eq = 0;
            if (engine::findEquippedItemKey(gw, h, sid, sizeof(sid), &ty, &eq) && eq >= 1) {
                for (int k = 0; k < 5; ++k) out[k] = h[k];
                return true;
            }
        }
        return false;
    }
    static bool handLess(const EntityState& a, const EntityState& b) {
        if (a.hType != b.hType) return a.hType < b.hType;
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        if (a.hContainerSerial != b.hContainerSerial) return a.hContainerSerial < b.hContainerSerial;
        if (a.hIndex != b.hIndex) return a.hIndex < b.hIndex;
        return a.hSerial < b.hSerial;
    }
    static bool ctnrLess(const EntityState& a, const EntityState& b) {
        if (a.hContainer != b.hContainer) return a.hContainer < b.hContainer;
        return a.hContainerSerial < b.hContainerSerial;
    }
    static bool ctnrEq(const EntityState& a, const EntityState& b) {
        return a.hContainer == b.hContainer && a.hContainerSerial == b.hContainerSerial;
    }
    static int containerRankOf(const EntityState* sq, unsigned int n, unsigned int i) {
        EntityState distinct[MAX_SQUAD]; unsigned int dn = 0;
        for (unsigned int a = 0; a < n; ++a) {
            bool seen = false;
            for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[a])) { seen = true; break; }
            if (!seen && dn < MAX_SQUAD) distinct[dn++] = sq[a];
        }
        for (unsigned int a = 1; a < dn; ++a)
            for (unsigned int b = a; b > 0 && ctnrLess(distinct[b], distinct[b-1]); --b) {
                EntityState t = distinct[b]; distinct[b] = distinct[b-1]; distinct[b-1] = t;
            }
        for (unsigned int b = 0; b < dn; ++b) if (ctnrEq(distinct[b], sq[i])) return (int)b;
        return -1;
    }

    static const unsigned long HOST_DURATION_MS = 44000; // outlive the join's window
    static const unsigned long JOIN_DURATION_MS = 28000;
    static const unsigned long UNEQUIP_MS       = 8000;  // baseline, then unequip (leave off)

    // inv_reequip cycle (longer): the author UNEQUIPS then RE-EQUIPS, and both actions
    // must land while the OTHER client is alive+synced so the observer can witness the
    // dip+restore (the join only reaches gameplay ~12-20 s into the host's window, so the
    // cycle runs late; the host outlives the join's later own-cycle for the reverse check).
    static const unsigned long RE_HOST_DURATION_MS = 58000;
    static const unsigned long RE_JOIN_DURATION_MS = 42000;
    static const unsigned long RE_UNEQUIP_MS       = 22000; // after the peer is up
    static const unsigned long RE_REEQUIP_MS       = 32000; // hold the dip, then restore

    static const unsigned int  MAX_SQUAD        = 32;

    bool          haveOwn_;
    bool          haveEq_;
    bool          unequipped_;
    bool          reequipped_;
    bool          reequipMode_;
    unsigned long lastLogMs_;
    unsigned int  samples_;
    unsigned int  ownRank_;
    unsigned int  ownHand_[5];
    int           baseEqCount_;
    char          baseSid_[48];
    unsigned int  baseType_;
    unsigned int  lastOwnEq_;
    bool          rankHave_[2];
    unsigned int  rankHand_[2][5];
};

// Local, single-client DIAGNOSTIC: reproduce the manual weapon-drag failure WITHOUT any
// UI by driving the reconcile (engine::applyContainerContents) directly through the exact
// snapshot sequence the join observed - start [weapon EQ + clothes EQ], then a snapshot
// with the weapon LOOSE only, then restore. dumpInventory after each step + the [recon]
// traces inside applyContainerContents show precisely which primitive loses the weapon.
// No network/invSync needed: the scenario's own applyContainerContents calls are the only
// inventory mutation, so the result is deterministic and reproducible from one instance.
class WeaponSeqScenario : public TimedScenario {
public:
    WeaponSeqScenario() : TimedScenario("inv_wpnseq", 0), have_(false), nbase_(0), wIdx_(-1), step_(0) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
    }

    virtual void onStart(const ScenarioContext& ctx) {
        have_ = firstGeared(ctx.gw, hand_);
        if (have_) {
            nbase_ = engine::captureContainerContents(ctx.gw, hand_, base_, INV_ITEMS_MAX, 0);
            for (unsigned int i = 0; i < nbase_; ++i)
                if (base_[i].equipped && wIdx_ < 0) wIdx_ = (int)i; // first worn entry = the weapon
        }
        char b[160];
        _snprintf(b, sizeof(b) - 1, "WSEQ start have=%d nbase=%u wIdx=%d wsid='%s' wtype=%u",
                  have_ ? 1 : 0, nbase_, wIdx_,
                  (wIdx_ >= 0) ? base_[wIdx_].stringID : "(none)",
                  (wIdx_ >= 0) ? base_[wIdx_].itemType : 0u);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (have_) { coop::logLine("WSEQ initial-state:"); engine::dumpInventory(ctx.gw, hand_); }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!have_ || wIdx_ < 0) { if (ctx.elapsedMs >= 8000) { passed_ = false; return true; } return false; }
        // S1 @ 9s: weapon LOOSE + clothes EQ (clean unequip-to-bag) -> move-down weapon.
        if (step_ == 0 && ctx.elapsedMs >= 9000) {
            step_ = 1;
            InvItemEntry s[INV_ITEMS_MAX]; unsigned int ns = 0;
            for (unsigned int i = 0; i < nbase_; ++i) {
                s[ns] = base_[i];
                if ((int)i == wIdx_) { s[ns].equipped = 0; s[ns].slot = 0; } // weapon loose
                ++ns;
            }
            coop::logLine("WSEQ apply S1=[weapon LOOSE + clothes EQ]");
            engine::applyContainerContents(ctx.gw, hand_, s, ns);
            coop::logLine("WSEQ after-S1:"); engine::dumpInventory(ctx.gw, hand_);
        }
        // S2 @ 12s: weapon FULLY GONE (cursor-held transient) -> destroys the weapon.
        if (step_ == 1 && ctx.elapsedMs >= 12000) {
            step_ = 2;
            InvItemEntry s[INV_ITEMS_MAX]; unsigned int ns = 0;
            for (unsigned int i = 0; i < nbase_; ++i)
                if ((int)i != wIdx_) s[ns++] = base_[i]; // everything EXCEPT the weapon
            coop::logLine("WSEQ apply S2=[weapon GONE]");
            engine::applyContainerContents(ctx.gw, hand_, ns ? s : 0, ns);
            coop::logLine("WSEQ after-S2:"); engine::dumpInventory(ctx.gw, hand_);
        }
        // S3 @ 15s: restore baseline (weapon EQ again) -> only CREATE-EQ available (fabricate).
        if (step_ == 2 && ctx.elapsedMs >= 15000) {
            step_ = 3;
            coop::logLine("WSEQ apply S3=[restore baseline EQ]");
            engine::applyContainerContents(ctx.gw, hand_, base_, nbase_);
            coop::logLine("WSEQ after-S3 (immediate):"); engine::dumpInventory(ctx.gw, hand_);
        }
        // @ 18s: re-dump - did the fabricated equipped weapon SURVIVE several ticks? (d25)
        if (step_ == 3 && ctx.elapsedMs >= 18000) {
            step_ = 4;
            coop::logLine("WSEQ after-S3 (3s later, persistence check):");
            engine::dumpInventory(ctx.gw, hand_);
        }
        if (ctx.elapsedMs >= 20000) { passed_ = (step_ == 4); return true; }
        return false;
    }
private:
    static const unsigned int MAX_SQUAD = 32;
    static bool firstGeared(GameWorld* gw, unsigned int out[5]) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, MAX_SQUAD);
        for (unsigned int i = 0; i < n; ++i) {
            unsigned int h[5] = { sq[i].hType, sq[i].hContainer, sq[i].hContainerSerial,
                                  sq[i].hIndex, sq[i].hSerial };
            char sid[48]; unsigned int ty = 0; int eq = 0;
            if (engine::findEquippedItemKey(gw, h, sid, sizeof(sid), &ty, &eq) && eq >= 1) {
                for (int k = 0; k < 5; ++k) out[k] = h[k];
                return true;
            }
        }
        return false;
    }
    bool         have_;
    unsigned int hand_[5];
    InvItemEntry base_[INV_ITEMS_MAX];
    unsigned int nbase_;
    int          wIdx_;
    int          step_;
};

// inv_addequip (LOCAL, single-client, DETERMINISTIC): prove the fix for the "picked-up
// weapon auto-equips into the empty slot, flickers, then VANISHES" bug (d25). When the
// reconcile must ADD an EQUIPPED item the container has NO copy of (curEq=0, curLoose=0),
// the old code fell to fabricate-AND-equip, which the engine's equipment validation
// discards within a tick. applyContainerContents now creates the missing item LOOSE (which
// persists) and equips the now-real loose copy on a LATER reconcile pass - exactly how the
// host re-publishes the same worn snapshot every few seconds. This scenario scripts that
// sequence on one client with NO network: remove a worn item (so there is no copy), then
// re-apply the worn baseline repeatedly, and asserts the slot fills back in AND stays
// filled when we STOP re-applying (the persistence proof the old fabricate path failed).
class InventoryAddEquipScenario : public TimedScenario {
public:
    InventoryAddEquipScenario()
        : TimedScenario("inv_addequip", 0), have_(false), nbase_(0), eIdx_(-1), baseType_(0), baseWorn_(0),
          step_(0), eqAfterCreate_(-1), eqAfterEquip_(-1), eqPersist_(-1) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        baseSid_[0] = '\0';
    }

    virtual void onStart(const ScenarioContext& ctx) {
        have_ = firstGeared(ctx.gw, hand_);
        if (have_) {
            nbase_ = engine::captureContainerContents(ctx.gw, hand_, base_, INV_ITEMS_MAX, 0);
            // Prefer a NON-WEAPON worn item (armour). WEAPONS cannot currently be rebuilt by
            // the engine factory (createItem returns null for them), so the create-then-equip
            // path under test only applies to reconstructable gear; choosing armour isolates
            // the deferred-equip fix from that separate weapon-factory limitation.
            const unsigned int WEAPON_CAT = 2;
            for (unsigned int pass = 0; pass < 2 && eIdx_ < 0; ++pass)
                for (unsigned int i = 0; i < nbase_; ++i)
                    if (base_[i].equipped && (pass == 1 || base_[i].itemType != WEAPON_CAT)) {
                        eIdx_ = (int)i;
                        strncpy(baseSid_, base_[i].stringID, sizeof(baseSid_) - 1);
                        baseType_ = base_[i].itemType;
                        break;
                    }
            if (eIdx_ >= 0) baseWorn_ = wornCount(ctx.gw);
        }
        char b[200];
        _snprintf(b, sizeof(b) - 1,
            "ADDEQ start have=%d nbase=%u eIdx=%d sid='%s' type=%u baseWorn=%d",
            have_ ? 1 : 0, nbase_, eIdx_, baseSid_[0] ? baseSid_ : "(none)", baseType_, baseWorn_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (have_) { coop::logLine("ADDEQ initial:"); engine::dumpInventory(ctx.gw, hand_); }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!have_ || eIdx_ < 0) { if (ctx.elapsedMs >= 6000) { passed_ = false; return true; } return false; }
        // S0 @6s: REMOVE the worn item entirely (apply baseline minus it) -> no copy left.
        if (step_ == 0 && ctx.elapsedMs >= 6000) {
            step_ = 1;
            InvItemEntry s[INV_ITEMS_MAX]; unsigned int ns = 0;
            for (unsigned int i = 0; i < nbase_; ++i) if ((int)i != eIdx_) s[ns++] = base_[i];
            coop::logLine("ADDEQ apply S0=[worn item GONE]");
            engine::applyContainerContents(ctx.gw, hand_, ns ? s : 0, ns);
            coop::logLine("ADDEQ after-S0:"); engine::dumpInventory(ctx.gw, hand_);
            logEq(ctx, "S0-removed", wornCount(ctx.gw));
        }
        // S1 @9s: re-apply baseline (worn DESIRED, no copy present) -> CREATE-LOOSE (deferred);
        //         the worn count may legitimately still be 0 here (that is the whole point).
        if (step_ == 1 && ctx.elapsedMs >= 9000) {
            step_ = 2;
            coop::logLine("ADDEQ apply S1=[restore worn] (expect CREATE-LOOSE, deferred)");
            engine::applyContainerContents(ctx.gw, hand_, base_, nbase_);
            engine::dumpInventory(ctx.gw, hand_);
            eqAfterCreate_ = wornCount(ctx.gw);
            logEq(ctx, "after-create", eqAfterCreate_);
        }
        // S2 @12s: re-apply baseline AGAIN -> MOVE-UP equips the now-real loose copy persistently.
        if (step_ == 2 && ctx.elapsedMs >= 12000) {
            step_ = 3;
            coop::logLine("ADDEQ apply S2=[restore worn] (expect MOVE-UP equip)");
            engine::applyContainerContents(ctx.gw, hand_, base_, nbase_);
            engine::dumpInventory(ctx.gw, hand_);
            eqAfterEquip_ = wornCount(ctx.gw);
            logEq(ctx, "after-equip", eqAfterEquip_);
        }
        // @15s: persistence check WITHOUT re-applying - did the equip SURVIVE several ticks?
        if (step_ == 3 && ctx.elapsedMs >= 15000) {
            step_ = 4;
            coop::logLine("ADDEQ persistence check (no re-apply):");
            engine::dumpInventory(ctx.gw, hand_);
            eqPersist_ = wornCount(ctx.gw);
            logEq(ctx, "persist", eqPersist_);
        }
        if (ctx.elapsedMs >= 18000) {
            // PASS: after the second apply the worn copy returned to its baseline count AND it
            // persisted to the no-reapply check. eqAfterCreate may be 0 (deferred) - allowed.
            passed_ = have_ && (eIdx_ >= 0) && (baseWorn_ >= 1) &&
                      (eqAfterEquip_ >= baseWorn_) && (eqPersist_ >= baseWorn_);
            char b[140];
            _snprintf(b, sizeof(b) - 1,
                "ADDEQ verdict pass=%d baseWorn=%d create=%d equip=%d persist=%d",
                passed_ ? 1 : 0, baseWorn_, eqAfterCreate_, eqAfterEquip_, eqPersist_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return true;
        }
        return false;
    }
private:
    static const unsigned int MAX_SQUAD = 32;
    void logEq(const ScenarioContext& ctx, const char* what, int eq) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "ADDEQ eq-%s=%d (sid='%s')", what, eq, baseSid_[0] ? baseSid_ : "(none)");
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        (void)ctx;
    }
    int wornCount(GameWorld* gw) {
        InvItemEntry it[INV_ITEMS_MAX];
        unsigned int n = engine::captureContainerContents(gw, hand_, it, INV_ITEMS_MAX, 0);
        int c = 0;
        for (unsigned int i = 0; i < n; ++i)
            if (it[i].equipped && it[i].itemType == baseType_ && strcmp(it[i].stringID, baseSid_) == 0) ++c;
        return c;
    }
    static bool firstGeared(GameWorld* gw, unsigned int out[5]) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, MAX_SQUAD);
        for (unsigned int i = 0; i < n; ++i) {
            unsigned int h[5] = { sq[i].hType, sq[i].hContainer, sq[i].hContainerSerial,
                                  sq[i].hIndex, sq[i].hSerial };
            char sid[48]; unsigned int ty = 0; int eq = 0;
            if (engine::findEquippedItemKey(gw, h, sid, sizeof(sid), &ty, &eq) && eq >= 1) {
                for (int k = 0; k < 5; ++k) out[k] = h[k];
                return true;
            }
        }
        return false;
    }
    bool         have_;
    unsigned int hand_[5];
    InvItemEntry base_[INV_ITEMS_MAX];
    unsigned int nbase_;
    int          eIdx_;
    char         baseSid_[48];
    unsigned int baseType_;
    int          baseWorn_;
    int          step_;
    int          eqAfterCreate_;
    int          eqAfterEquip_;
    int          eqPersist_;
};

// wpn_relocate (SPIKE for the conservation model): prove that a WEAPON - which the engine
// factory CANNOT fabricate (createItem returns null) - can still be moved bag -> ground ->
// bag by RELOCATING the REAL object, and that it PERSISTS at each step. This validates the
// "don't create, conserve & move" trade model: both clients already own the weapon (shared
// save), so a drop/pickup is a relocation of each side's real copy, never a create/destroy.
// Single-client + deterministic (no network): unequip the weapon to loose, DROP it (real
// ground item), confirm it survives ticks, then PICK IT UP by re-homing the real object.
class WeaponRelocateScenario : public TimedScenario {
public:
    WeaponRelocateScenario()
        : TimedScenario("wpn_relocate", 0), have_(false), baseType_(0), step_(0),
          invBase_(0), invAfterDrop_(-1), grndAfterDrop_(-1), grndPersist_(-1),
          invAfterPick_(-1), grndAfterPick_(-1), invPersist_(-1) {
        for (int i = 0; i < 5; ++i) hand_[i] = 0;
        baseSid_[0] = '\0';
    }

    virtual void onStart(const ScenarioContext& ctx) {
        have_ = findWeaponHolder(ctx.gw, hand_, baseSid_, sizeof(baseSid_), &baseType_);
        if (have_) invBase_ = invCount(ctx.gw);
        char b[200];
        _snprintf(b, sizeof(b) - 1, "RELOC start have=%d sid='%s' type=%u invBase=%d",
                  have_ ? 1 : 0, baseSid_[0] ? baseSid_ : "(none)", baseType_, invBase_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (have_) { coop::logLine("RELOC initial:"); engine::dumpInventory(ctx.gw, hand_); }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!have_ || !baseSid_[0]) { if (ctx.elapsedMs >= 6000) { passed_ = false; return true; } return false; }
        // S0 @6s: ensure the weapon is LOOSE (unequip the real object to the bag; preserves
        // identity). dropItemFromInventory drops loose items only.
        if (step_ == 0 && ctx.elapsedMs >= 6000) {
            step_ = 1;
            int un = engine::unequipItemToLoose(ctx.gw, hand_, baseSid_, baseType_, 1);
            logN(ctx, "unequip", un);
        }
        // S1 @9s: DROP the real weapon -> a free ground item (native Inventory::dropItem; no
        // createItem). Expect inv -1 and a free ground weapon to appear.
        if (step_ == 1 && ctx.elapsedMs >= 9000) {
            step_ = 2;
            int dr = engine::dropItemFromInventory(ctx.gw, hand_, baseSid_, baseType_, 1);
            invAfterDrop_  = invCount(ctx.gw);
            grndAfterDrop_ = engine::countFreeGroundItemsNear(ctx.gw, hand_, baseSid_, baseType_, radius());
            char b[120]; _snprintf(b, sizeof(b) - 1, "RELOC after-drop dropped=%d inv=%d ground=%d", dr, invAfterDrop_, grndAfterDrop_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        // S1b @12s: the dropped weapon is a REAL persistent object - still on the ground
        // ticks later (a fabricated item would have vanished).
        if (step_ == 2 && ctx.elapsedMs >= 12000) {
            step_ = 3;
            grndPersist_ = engine::countFreeGroundItemsNear(ctx.gw, hand_, baseSid_, baseType_, radius());
            logN(ctx, "ground-persist", grndPersist_);
        }
        // S2 @15s: PICK IT UP by relocating the real ground object into the bag (no create).
        if (step_ == 3 && ctx.elapsedMs >= 15000) {
            step_ = 4;
            int pk = engine::pickupWorldItemIntoInventory(ctx.gw, hand_, baseSid_, baseType_, radius());
            invAfterPick_  = invCount(ctx.gw);
            grndAfterPick_ = engine::countFreeGroundItemsNear(ctx.gw, hand_, baseSid_, baseType_, radius());
            char b[120]; _snprintf(b, sizeof(b) - 1, "RELOC after-pickup picked=%d inv=%d ground=%d", pk, invAfterPick_, grndAfterPick_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            coop::logLine("RELOC after-pickup dump:"); engine::dumpInventory(ctx.gw, hand_);
        }
        // S2b @18s: the re-homed weapon persists in the bag (real object, no fabrication).
        if (step_ == 4 && ctx.elapsedMs >= 18000) {
            step_ = 5;
            invPersist_ = invCount(ctx.gw);
            logN(ctx, "inv-persist", invPersist_);
        }
        if (ctx.elapsedMs >= 21000) {
            bool dropOk    = (invAfterDrop_ >= 0) && (invAfterDrop_ <= invBase_ - 1) && (grndAfterDrop_ >= 1);
            bool dropHeld  = (grndPersist_ >= 1);
            bool pickOk    = (invAfterPick_ >= invBase_) && (grndAfterPick_ < grndAfterDrop_);
            bool pickHeld  = (invPersist_ >= invBase_);
            passed_ = have_ && (invBase_ >= 1) && dropOk && dropHeld && pickOk && pickHeld;
            char b[200];
            _snprintf(b, sizeof(b) - 1,
                "RELOC verdict pass=%d invBase=%d drop(inv=%d grnd=%d held=%d) pick(inv=%d grnd=%d persist=%d)",
                passed_ ? 1 : 0, invBase_, invAfterDrop_, grndAfterDrop_, grndPersist_,
                invAfterPick_, grndAfterPick_, invPersist_);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return true;
        }
        return false;
    }
private:
    static const unsigned int MAX_SQUAD = 32;
    static float radius() { return 18.0f; } // ground search radius (drops land at feet)

    void logN(const ScenarioContext& ctx, const char* what, int n) {
        char b[100]; _snprintf(b, sizeof(b) - 1, "RELOC %s=%d sid='%s'", what, n, baseSid_[0] ? baseSid_ : "(none)");
        b[sizeof(b) - 1] = '\0'; coop::logLine(b); (void)ctx;
    }
    // Count copies of (baseSid_,baseType_) in the holder's inventory (loose + equipped).
    int invCount(GameWorld* gw) {
        InvItemEntry it[INV_ITEMS_MAX];
        unsigned int n = engine::captureContainerContents(gw, hand_, it, INV_ITEMS_MAX, 0);
        int c = 0;
        for (unsigned int i = 0; i < n; ++i)
            if (it[i].itemType == baseType_ && strcmp(it[i].stringID, baseSid_) == 0) ++c;
        return c;
    }
    // Scan squad members for one holding a WEAPON (type==2); prefer an equipped weapon.
    static bool findWeaponHolder(GameWorld* gw, unsigned int out[5], char* outSid,
                                 unsigned int outLen, unsigned int* outType) {
        const unsigned int WEAPON_CAT = 2;
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(gw, /*leaderOnly*/ false, sq, MAX_SQUAD);
        for (unsigned int pass = 0; pass < 2; ++pass) { // pass0: equipped weapon, pass1: any
            for (unsigned int m = 0; m < n; ++m) {
                unsigned int h[5] = { sq[m].hType, sq[m].hContainer, sq[m].hContainerSerial,
                                      sq[m].hIndex, sq[m].hSerial };
                InvItemEntry it[INV_ITEMS_MAX];
                unsigned int cnt = engine::captureContainerContents(gw, h, it, INV_ITEMS_MAX, 0);
                for (unsigned int i = 0; i < cnt; ++i) {
                    if (it[i].itemType != WEAPON_CAT) continue;
                    if (pass == 0 && !it[i].equipped) continue;
                    for (int k = 0; k < 5; ++k) out[k] = h[k];
                    strncpy(outSid, it[i].stringID, outLen - 1); outSid[outLen - 1] = '\0';
                    if (outType) *outType = it[i].itemType;
                    return true;
                }
            }
        }
        return false;
    }

    bool         have_;
    unsigned int hand_[5];
    char         baseSid_[48];
    unsigned int baseType_;
    int          step_;
    int          invBase_, invAfterDrop_, grndAfterDrop_, grndPersist_;
    int          invAfterPick_, grndAfterPick_, invPersist_;
};

} // namespace

Scenario* makeInventoryScenario(const std::string& name) {
    if (name == "inv_order")    return new InventorySyncScenario();
    if (name == "inv_bidir")    return new InventoryBidirScenario();
    if (name == "trade_probe")  return new TradeScenario(/*peer=*/false);
    if (name == "trade_peer")   return new TradeScenario(/*peer=*/true);
    if (name == "xfer_block")   return new XferBlockScenario();
    if (name == "inv_equip")    return new InventoryEquipScenario(/*reequip=*/false);
    if (name == "inv_reequip")  return new InventoryEquipScenario(/*reequip=*/true);
    if (name == "inv_wpnseq")   return new WeaponSeqScenario();
    if (name == "inv_addequip") return new InventoryAddEquipScenario();
    if (name == "wpn_relocate") return new WeaponRelocateScenario();
    return 0;
}

} // namespace coop
