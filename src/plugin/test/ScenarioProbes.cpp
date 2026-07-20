// ScenarioProbes.cpp - diagnostic probes + economy/faction/time scenarios
// (monolith split from Scenario.cpp, 2026-07-12): spike (the numbered-probe
// dispatcher), shop_probe/money_sync, vendor_trade, recruit_probe/
// recruit_sync, squad_probe/squad_sync, faction_probe/faction_sync,
// time_probe/time_sync, hunger_probe/hunger_sync. Classes are TU-private
// (anonymous namespace); only makeProbeScenario (ScenarioSupport.h) is
// exported.
// Must NOT: change any SCENARIO/SPIKE log string (oracle API, resources/CODE_MAP.md).

#include "ScenarioSupport.h"

namespace coop {
namespace {


// ===========================================================================
// SpikeScenario - generic investigative harness for the autonomous spike loop.
// The concrete probe is selected by the KENSHICOOP_SPIKE env var ("1".."50").
// Each probe emits "SPIKE <id> ..." evidence lines that run_spike.ps1 collects
// and the per-spike findings doc summarizes. It is DIAGNOSTIC: passed() means
// "the probe executed and produced evidence", not a cross-client sync gate.
//
// All spike-specific code lives HERE so it can be reverted in one place between
// batches (the harness baseline keeps only the dispatcher + the smoke probe).
// Add a probe by extending dispatchStart()/dispatchTick() with a new id branch.
// ===========================================================================
class SpikeScenario : public TimedScenario {
public:
    SpikeScenario()
        : TimedScenario("spike", /*evidenceMs=*/0),
          passedSet_(false), started_(false), smokeDone_(false),
          nativeDone_(false), lastLogMs_(0), durMs_(30000), wmStep_(0),
          r4Step_(0), r4Ops_(0), r4NextMs_(0), r4Have_(false), r4Placed_(false),
          r4Started_(false) {
        const char* id = std::getenv("KENSHICOOP_SPIKE");
        id_ = id ? id : "0";
        wmSid_[0] = '\0';
        r4Sid_[0] = '\0';
        r4ResSid_[0] = '\0';
        for (int i = 0; i < 5; ++i) r4Hand_[i] = 0;
    }

    virtual void onStart(const ScenarioContext& ctx) {
        started_ = true;
        logSpike("start id=%s host=%d", id_.c_str(), ctx.isHost ? 1 : 0);
        dispatchStart(ctx);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Keep a MEMBER/RECV anchor flowing so the runner can time a screenshot
        // and never stalls on the missing-anchor wait.
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            Character* ld = engine::leader(ctx.gw);
            if (ld) logScenarioLine(ctx.isHost ? "MEMBER" : "RECV", ld);
            dispatchTick(ctx);
        }
        if (ctx.elapsedMs >= durMs_) {
            // Diagnostic: a probe "passes" if it executed without faulting and
            // emitted at least its start line. Concrete probes set passed_ when
            // their evidence is captured; default to true so a pure-enumeration
            // probe still reports a clean RESULT.
            if (!passedSet_) passed_ = true;
            return true;
        }
        return false;
    }

private:
    void logSpike(const char* fmt, ...) {
        char b[480];
        char f[512];
        _snprintf(f, sizeof(f) - 1, "SPIKE %s %s", id_.c_str(), fmt);
        f[sizeof(f) - 1] = '\0';
        va_list ap; va_start(ap, fmt);
        _vsnprintf(b, sizeof(b) - 1, f, ap);
        va_end(ap);
        b[sizeof(b) - 1] = '\0';
        coop::logLine(b);
    }
    void setPass(bool ok) { passed_ = ok; passedSet_ = true; }

    // ---- per-spike dispatch ------------------------------------------------
    void dispatchStart(const ScenarioContext& ctx) {
        // id "0" = smoke probe (baseline): prove the harness runs end to end.
        // Concrete spikes are added as additional id branches per batch.
        if (id_ == "451" && ctx.isHost) {
            // Weapon-mint recipe trace: watch the ENGINE create weapons (armed
            // runtime NPC spawn), compare with our failing diag calls, then
            // replay the captured recipe from plugin context - all one run.
            bool ok = engine::installCreateItemTraceHook();
            logSpike("mkspy install ok=%d", ok ? 1 : 0);
            durMs_ = 38000;
        }
        if (id_ == "401") {
            // Research tech-tree store map: both sides run the script (the host
            // drives operate(); both census + diff their own store).
            durMs_ = 40000;
        }
    }

    void dispatchTick(const ScenarioContext& ctx) {
        // Smoke probe: log the leader hand/pos + a rough nearby-NPC count once,
        // so the baseline build is verifiably exercisable before real probes land.
        if (!smokeDone_) {
            smokeDone_ = true;
            Character* ld = engine::leader(ctx.gw);
            unsigned int h[5]; float x = 0, y = 0, z = 0;
            if (ld && engine::readHand(ld, h) && engine::readPos(ld, &x, &y, &z)) {
                logSpike("smoke leader hand=%u,%u,%u,%u,%u pos=%.1f,%.1f,%.1f",
                         h[0], h[1], h[2], h[3], h[4], x, y, z);
            } else {
                logSpike("smoke leader UNRESOLVED");
            }
            setPass(true);
        }
        if (id_ == "402" && ctx.isHost && !nativeDone_ &&
            ctx.elapsedMs >= 2000) {
            nativeDone_ = true;
            int rc = engine::probeNativeSnapshot(ctx.gw);
            logSpike("native snapshot rc=%d", rc);
            setPass(rc == 1);
        }
        if (id_ == "451" && ctx.isHost) tick451(ctx);
        if (id_ == "401") tick401(ctx);
    }

    // Spike 401 (research tech-tree store): locate/place a research bench,
    // baseline-snapshot PlayerInterface::technology (hex + GameData-slot
    // classification), then drive the bench's own operate() at 1 Hz while
    // dword-diffing the store each second - the moving dwords are the progress
    // scalar, the mutating region the completed-set container. The join runs
    // the same census + store-diff WITHOUT driving (does its store move at all
    // while only the host researches? that silence/motion IS the gap evidence).
    void r4Census(const ScenarioContext& ctx) {
        engine::ProdRead rows[32];
        unsigned int n = engine::enumMachinesNear(ctx.gw, 100.0f, rows, 32);
        for (unsigned int i = 0; i < n && !r4Have_; ++i)
            if (rows[i].classType == 5 /*BCTYPE_RESEARCH*/) {
                memcpy(r4Hand_, rows[i].hand, sizeof(r4Hand_));
                strncpy(r4Sid_, rows[i].sid, sizeof(r4Sid_) - 1);
                r4Sid_[sizeof(r4Sid_) - 1] = '\0';
                r4Have_ = true;
                logSpike("bench found sid='%s' hand=%u,%u,%u,%u,%u (of %u)",
                         r4Sid_, r4Hand_[0], r4Hand_[1], r4Hand_[2],
                         r4Hand_[3], r4Hand_[4], n);
            }
    }

    void tick401(const ScenarioContext& ctx) {
        // @3s: latch the first baked research bench in census range; when the
        // save has none (no captured sync run ever logged class=5), the HOST
        // places one (kind 3) and ramps it complete on the next step.
        if (r4Step_ == 0 && ctx.elapsedMs >= 3000) {
            r4Step_ = 1;
            r4Census(ctx);
            if (!r4Have_) {
                logSpike("bench baked=0 (none in 100m)");
                if (ctx.isHost) {
                    int rc = engine::probePlaceMachine(ctx.gw, 8.0f, 2.0f,
                                                       /*kind*/3, r4Hand_,
                                                       r4Sid_, sizeof(r4Sid_));
                    r4Placed_ = r4Have_ = (rc == 1);
                    logSpike("bench place rc=%d sid='%s'", rc,
                             r4Sid_[0] ? r4Sid_ : "(none)");
                }
            }
        }
        // @5s: ramp the placed site complete (>= 1.0 self-completes natively).
        if (r4Step_ == 1 && ctx.elapsedMs >= 5000) {
            r4Step_ = 2;
            if (r4Placed_) {
                engine::BuildRead post;
                bool ok = engine::writeBuildProgressByHand(r4Hand_, 1.0f, &post);
                logSpike("bench ramp ok=%d complete=%d", ok ? 1 : 0,
                         ok ? post.complete : -1);
            }
        }
        // @7s: store baseline (hex + slot classification), enumerate the first
        // RESEARCH records with the engine's own known/can predicates, and pick
        // the SUBJECT: the first not-known researchABLE record (deterministic -
        // gamedata order is shared, so host and join pick the same sid).
        if (r4Step_ == 2 && ctx.elapsedMs >= 7000) {
            r4Step_ = 3;
            int rs = engine::probeResearchStore(ctx.gw, 0);
            unsigned int total = engine::probeResearchEnum(ctx.gw, 24);
            int picked = engine::researchPickSubject(ctx.gw, r4ResSid_,
                                                     sizeof(r4ResSid_));
            char sel[48];
            int hasSel = engine::probeCurrentResearchSid(sel, sizeof(sel));
            logSpike("store baseline rs=%d research-total=%u picked=%d "
                     "subject='%s' current='%s'",
                     rs, total, picked, r4ResSid_[0] ? r4ResSid_ : "(none)",
                     hasSel ? sel : "(none)");
            r4NextMs_ = ctx.elapsedMs + 1000;
        }
        // Host @10s: SELECT via the engine's own lever (startResearch - the
        // UI click's commit) so the operate() bursts have something to
        // progress. Bracketing store diffs isolate what SELECT itself writes.
        if (r4Step_ == 3 && ctx.isHost && !r4Started_ &&
            ctx.elapsedMs >= 10000 && r4ResSid_[0]) {
            r4Started_ = true;
            engine::probeResearchStore(ctx.gw, 1); // pre-select diff marker
            int rc = engine::researchStartBySid(ctx.gw, r4ResSid_);
            logSpike("select rc=%d sid='%s'", rc, r4ResSid_);
            engine::probeResearchStore(ctx.gw, 1); // what did SELECT change?
        }
        // 8..34s @1 Hz: host drives operate(), both read the bench + diff the
        // store + track the subject's known flag. The join keeps re-censusing
        // until the (possibly minted) bench copy appears locally.
        if (r4Step_ == 3 && ctx.elapsedMs < 34000 &&
            ctx.elapsedMs >= r4NextMs_) {
            r4NextMs_ = ctx.elapsedMs + 1000;
            if (!r4Have_) r4Census(ctx);
            int op = -1;
            if (ctx.isHost && r4Have_)
                op = engine::operateMachineByHand(ctx.gw, r4Hand_, 1.0f) ? 1 : 0;
            int tech = -1, power = -1;
            float prog = -1.0f;
            if (r4Have_)
                engine::probeResearchBenchRead(r4Hand_, &tech, &prog, &power);
            int known = -1, can = -1;
            if (r4ResSid_[0])
                engine::researchQueryBySid(ctx.gw, r4ResSid_, &known, &can);
            ++r4Ops_;
            char sel[48];
            int hasSel = engine::probeCurrentResearchSid(sel, sizeof(sel));
            logSpike("bench n=%u op=%d tech=%d prog=%.4f power=%d known=%d "
                     "cur='%s' t=%lu",
                     r4Ops_, op, tech, prog, power, known,
                     hasSel ? sel : "", ctx.elapsedMs);
            engine::probeResearchStore(ctx.gw, 1);
        }
        // @35s: final state + summary.
        if (r4Step_ == 3 && ctx.elapsedMs >= 35000) {
            r4Step_ = 4;
            int known = -1, can = -1;
            if (r4ResSid_[0])
                engine::researchQueryBySid(ctx.gw, r4ResSid_, &known, &can);
            char sel[48];
            int hasSel = engine::probeCurrentResearchSid(sel, sizeof(sel));
            logSpike("summary have=%d placed=%d samples=%u subject='%s' "
                     "known=%d current='%s'",
                     r4Have_ ? 1 : 0, r4Placed_ ? 1 : 0, r4Ops_,
                     r4ResSid_[0] ? r4ResSid_ : "(none)", known,
                     hasSel ? sel : "(none)");
            setPass(true);
        }
    }

    // Spike 451 script (host only): @3s spawn 2 armed runtime NPCs (the engine
    // mints their weapons - the trace captures its recipe); @14s run the failing
    // diag matrix (its calls now trace too, for arg-by-arg comparison); @20s
    // replay the captured engine recipe from plugin context onto the leader.
    void tick451(const ScenarioContext& ctx) {
        if (wmStep_ == 0 && ctx.elapsedMs >= 3000) {
            wmStep_ = 1;
            unsigned int hands[4][5];
            unsigned int n = engine::spawnRuntimeSquad(ctx.gw, 2, hands);
            logSpike("spawned n=%u", n);
        }
        // Leader hand via pickInventoryContainer: readObjectHand layout
        // [type,container,containerSerial,index,serial] - what resolveObjectByHand
        // expects (run 1 passed readHand's [index,serial,...] layout -> "no inv").
        if (wmStep_ == 1 && ctx.elapsedMs >= 14000) {
            wmStep_ = 2;
            unsigned int h[5];
            if (engine::pickInventoryContainer(ctx.gw, h)) {
                logSpike("diag begin");
                engine::diagWeaponCreate(ctx.gw, h, 24);
            } else {
                logSpike("diag SKIP leader unresolved");
            }
        }
        if (wmStep_ == 2 && ctx.elapsedMs >= 20000) {
            wmStep_ = 3;
            unsigned int h[5];
            int res = -2;
            if (engine::pickInventoryContainer(ctx.gw, h))
                res = engine::probeReplayWeaponMint(ctx.gw, h);
            logSpike("replay res=%d", res);
        }
        // Phase-2 persistence legs: fabricate LOOSE via the wire path, equip the
        // REAL loose copy a tick later (the reconcile MOVE-UP path), then census -
        // does the fabricated weapon persist worn (the d25 revisit)?
        if (wmStep_ == 3 && ctx.elapsedMs >= 24000) {
            wmStep_ = 4;
            unsigned int h[5];
            int res = 0;
            if (engine::pickInventoryContainer(ctx.gw, h))
                res = engine::probeFabricateWeaponLoose(ctx.gw, h, wmSid_, sizeof(wmSid_));
            logSpike("fab loose res=%d sid='%s'", res, wmSid_[0] ? wmSid_ : "(none)");
        }
        if (wmStep_ == 4 && ctx.elapsedMs >= 27000) {
            wmStep_ = 5;
            unsigned int h[5];
            int eq = -1;
            if (wmSid_[0] && engine::pickInventoryContainer(ctx.gw, h))
                eq = engine::reequipLooseItem(ctx.gw, h, wmSid_, 2 /*WEAPON*/, 1);
            logSpike("fab equip eq=%d", eq);
        }
        if (wmStep_ == 5 && ctx.elapsedMs >= 32000) {
            wmStep_ = 6;
            unsigned int h[5];
            int loose = 0, worn = 0;
            if (wmSid_[0] && engine::pickInventoryContainer(ctx.gw, h)) {
                InvItemEntry items[INV_ITEMS_MAX];
                unsigned int n = engine::captureContainerContents(ctx.gw, h, items,
                                                                  INV_ITEMS_MAX, 0);
                for (unsigned int i = 0; i < n; ++i) {
                    if (strcmp(items[i].stringID, wmSid_) != 0) continue;
                    if (items[i].equipped) worn += items[i].quantity;
                    else                   loose += items[i].quantity;
                }
            }
            logSpike("fab persist loose=%d worn=%d", loose, worn);
            setPass(true);
        }
    }

    std::string   id_;
    bool          passedSet_;
    bool          started_;
    bool          smokeDone_;
    bool          nativeDone_;
    unsigned long lastLogMs_;
    unsigned long durMs_;
    int           wmStep_;     // spike 451 script step
    char          wmSid_[48];  // spike 451 fabricated-weapon template sid
    int           r4Step_;     // spike 401 script step
    unsigned int  r4Ops_;      // spike 401 drive/diff samples taken
    unsigned long r4NextMs_;   // spike 401 next 1 Hz sample time
    bool          r4Have_;     // spike 401 research bench latched
    bool          r4Placed_;   // spike 401 bench was probe-placed (needs ramp)
    bool          r4Started_;  // spike 401 startResearch lever fired (host)
    unsigned int  r4Hand_[5];  // spike 401 research bench local hand
    char          r4Sid_[48];  // spike 401 research bench template sid
    char          r4ResSid_[48]; // spike 401 subject RESEARCH record sid
};
// shop_probe (protocol 22 phase 0, probe tier): money + vendor-trading evidence.
//
// Kenshi facts under test (spikes 28-30): the wallet is per-Platoon (Ownerships::
// money - no global player wallet), vendors are ShopTrader RootObjects with
// save-stable hands, and Inventory::buyItem mutates vendor stock + wallet LOCALLY
// on one client only. Nothing about money is on the wire today.
//
// Script:
//   * both sides, 1 Hz: enumerate nearby vendors ("SCENARIO VENDOR hand=..
//     money=.. stock=.. thand=..") and read every squad tab's wallet
//     ("SCENARIO WALLET rank=.. money=..") - the divergence series.
//   * host t=10s / join t=22s: each side (1) SETS its OWNED tab's wallet to a
//     side-distinct sentinel via Ownerships::setMoney (host rank0=5000, join
//     rank1=7000) - validates the 1b apply primitive AND, on the peer's WALLET
//     series, decisively answers "does any wallet state cross today"; then
//     (2) attempts ONE programmatic Inventory::buyItem against the nearest
//     stocked vendor ([shop] BUY-BEFORE/AFTER evidence). Vendor inventories
//     are lazy (built on shop-open), so the stock is forced first.
// The verdict only asserts the script ran (wallet series + the scripted action
// logged on each side); what crossed is judged/recorded by Test-ShopProbe.
// The SAME script also runs as "money_sync" (probe=false): with the protocol
// 22 wallet channel LEFT ON, the sentinel writes must CROSS - each side's
// WALLET series must converge to the peer's sentinel. Test-MoneySync gates on
// that convergence (the shop_probe run gates only on the evidence existing).
class ShopProbeScenario : public TimedScenario {
public:
    explicit ShopProbeScenario(bool probe)
        : TimedScenario(probe ? "shop_probe" : "money_sync", /*evidenceMs=*/1000),
          probe_(probe), actDone_(false),
          buyRes_(-9), walletReads_(0), sawVendor_(false) {}

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SHOPPROBE start host=%d probe=%d",
                  ctx.isHost ? 1 : 0, probe_ ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // 1 Hz evidence: vendor census + per-tab wallet series (both sides).
        // The money_sync leg skips the vendor census (wallet-only gate).
        if (evidenceDue(ctx.elapsedMs)) {
            if (probe_) logVendors(ctx);
            logWallets(ctx);
        }
        // One scripted action window per side, staggered so the logs separate
        // the host crossing window from the join one. Each side (1) RAISES the
        // wallet of the tab it OWNS via Ownerships::setMoney - the decisive
        // "does money cross today" divergence lever plus the 1b apply-primitive
        // validation - then (2) attempts the programmatic vendor purchase
        // (records the vendor-stock findings).
        unsigned long actAt = ctx.isHost ? HOST_ACT_AT_MS : JOIN_ACT_AT_MS;
        if (!actDone_ && ctx.elapsedMs >= actAt) {
            actDone_ = true;
            doWalletSet(ctx);
            if (probe_) doBuy(ctx);
        }
        if (ctx.elapsedMs >= DURATION_MS) {
            // Script-ran gate only: wallets were readable and both scripted
            // actions were logged (any result - the RESULTS are findings).
            passed_ = (walletReads_ > 0) && actDone_;
            return true;
        }
        return false;
    }

private:
    void logVendors(const ScenarioContext& ctx) {
        engine::VendorRead v[MAX_VENDORS];
        unsigned int n = engine::listVendorsNear(ctx.gw, v, MAX_VENDORS, VENDOR_RADIUS);
        for (unsigned int i = 0; i < n; ++i) {
            // Keyed by the trader CHARACTER's save-stable hand (thand) - the
            // ShopTrader wrapper's own serial is runtime-minted and differs
            // per client/run (run 103018 finding), so thand is what the oracle
            // uses to match vendors across the two logs.
            char b[288];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO VENDOR hand=%u,%u,%u,%u,%u money=%d stock=%d qty=%d "
                      "src=%d thand=%u,%u,%u,%u,%u sid='%s' t=%lu",
                      v[i].hand[0], v[i].hand[1], v[i].hand[2], v[i].hand[3],
                      v[i].hand[4], v[i].money, v[i].stock, v[i].qty, v[i].src,
                      v[i].traderHand[0], v[i].traderHand[1], v[i].traderHand[2],
                      v[i].traderHand[3], v[i].traderHand[4], v[i].sid, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (n > 0) sawVendor_ = true;
        char c[64];
        _snprintf(c, sizeof(c) - 1, "SCENARIO VENDORS n=%u t=%lu", n, ctx.elapsedMs);
        c[sizeof(c) - 1] = '\0'; coop::logLine(c);
    }

    // One WALLET line per distinct squad tab (keyed by RANK, the cross-client
    // stable tab identity): the money series the oracle diffs host-vs-join.
    void logWallets(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        for (unsigned int rank = 0; rank < 4; ++rank) {
            int li = tabLeaderIdx(sq, n, rank);
            if (li < 0) continue;
            unsigned int h[5];
            handFromEntity(sq[li], h);
            int money = -1;
            if (engine::readWalletByHand(h, &money)) ++walletReads_;
            char b[96];
            _snprintf(b, sizeof(b) - 1, "SCENARIO WALLET rank=%u money=%d t=%lu",
                      rank, money, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    // Wallet-write leg: set the OWNED tab's wallet to a side-distinct sentinel
    // via the engine accessor (host 5000, join 7000). Proves writeWallet works
    // (the 1b apply primitive) and, on the peer's WALLET series, whether ANY
    // wallet state crosses today (expected: it does not - the 1b gap evidence).
    void doWalletSet(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        unsigned int rank = ctx.isHost ? 0u : 1u;
        int li = tabLeaderIdx(sq, n, rank);
        if (li < 0) { li = tabLeaderIdx(sq, n, 0u); rank = 0u; }
        int before = -1, after = -1, ok = 0;
        int target = ctx.isHost ? 5000 : 7000;
        if (li >= 0) {
            unsigned int h[5];
            handFromEntity(sq[li], h);
            engine::readWalletByHand(h, &before);
            ok = engine::writeWalletByHand(h, target) ? 1 : 0;
            engine::readWalletByHand(h, &after);
        }
        char b[144];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO WALLETSET who=%s rank=%u target=%d ok=%d before=%d after=%d t=%lu",
                  ctx.isHost ? "host" : "join", rank, target, ok, before, after,
                  ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void doBuy(const ScenarioContext& ctx) {
        // Vendor inventories are LAZY (built when the trade UI first opens - run
        // 101952: every enumerated vendor read stock=-1), so force the engine's
        // own stock build on each candidate until one holds items, then re-read.
        engine::VendorRead v[MAX_VENDORS];
        unsigned int nv = engine::listVendorsNear(ctx.gw, v, MAX_VENDORS, VENDOR_RADIUS);
        int pick = -1;
        for (unsigned int i = 0; i < nv; ++i) {
            if (v[i].stock <= 0) {
                int r = engine::ensureVendorStock(ctx.gw, v[i].hand);
                char eb[112];
                _snprintf(eb, sizeof(eb) - 1, "SCENARIO SHOPSTOCK vendor=%u,%u ensure=%d",
                          v[i].hand[3], v[i].hand[4], r);
                eb[sizeof(eb) - 1] = '\0'; coop::logLine(eb);
            }
        }
        nv = engine::listVendorsNear(ctx.gw, v, MAX_VENDORS, VENDOR_RADIUS);
        for (unsigned int i = 0; i < nv; ++i)
            if (v[i].stock > 0) { pick = (int)i; break; }
        // Buyer = the tab THIS side owns (host rank 0, join rank 1; fall back to
        // rank 0 when the save has a single tab).
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int li = tabLeaderIdx(sq, n, ctx.isHost ? 0u : 1u);
        if (li < 0) li = tabLeaderIdx(sq, n, 0u);
        char sid[48]; sid[0] = '\0';
        if (pick < 0 || li < 0) {
            buyRes_ = -2; // no vendor / no buyer - recorded, judged by the oracle
        } else {
            unsigned int bh[5];
            handFromEntity(sq[li], bh);
            buyRes_ = engine::probeVendorBuy(ctx.gw, v[pick].hand, bh, sid, sizeof(sid));
        }
        char b[176];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO SHOPBUY who=%s res=%d sid='%s' vendor=%u,%u t=%lu",
                  ctx.isHost ? "host" : "join", buyRes_, sid,
                  pick >= 0 ? v[pick].hand[3] : 0u, pick >= 0 ? v[pick].hand[4] : 0u,
                  ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    static const unsigned long HOST_ACT_AT_MS = 10000;
    static const unsigned long JOIN_ACT_AT_MS = 22000;
    static const unsigned long DURATION_MS    = 40000;
    static const unsigned int  MAX_VENDORS    = 8;
    static const unsigned int  MAX_SQUAD      = 32;
    static const float         VENDOR_RADIUS;

    bool          probe_;
    bool          actDone_;
    int           buyRes_;
    unsigned int  walletReads_;
    bool          sawVendor_;
};
const float ShopProbeScenario::VENDOR_RADIUS = 100.0f;

// vendor_trade (protocol 22 phase 1c): the buyer-side purchase COMPOSITE gate.
//
// A real Inventory::buyItem is unreachable in automation (vendor inventories
// are lazy and the test save's SHOP_TRADER_CLASS objects carry no bound trader
// - shop_probe runs 103018-104036), so the scenario performs the exact buyer-
// side mutations ONE purchase makes - a wallet debit and the bought item
// landing in the buyer's personal inventory, same tick - on the tab each side
// OWNS, and gates that BOTH effects converge on the peer through the two
// existing channels (PKT_MONEY + the bidirectional inventory snapshots). The
// VENDOR-side mutation (stock shrink, register cash) intentionally stays local
// for now: the engine regenerates vendor stock per client anyway, and the
// [shop] BUY-LOCAL detour is gathering the field evidence for that mirror.
//
// Script (mirrors money_sync's stagger):
//   * both sides, 1 Hz: every tab's WALLET line + a TINV line (count + content
//     hash) for every tab leader's personal container.
//   * host t=6s: seed its rank-0 wallet to 5000; t=10s: TRADE - one test item
//     into the rank-0 leader's inventory + wallet -= 250 (-> 4750).
//   * join t=18s/t=22s: same on its rank-1 tab with 7000 -> 6750.
class VendorTradeScenario : public TimedScenario {
public:
    VendorTradeScenario()
        : TimedScenario("vendor_trade", /*evidenceMs=*/1000),
          seeded_(false), traded_(false),
          tradeOk_(false), walletReads_(0) {}

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO VENDORTRADE start host=%d",
                  ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (evidenceDue(ctx.elapsedMs)) {
            logSeries(ctx);
        }
        unsigned long seedAt  = ctx.isHost ? HOST_SEED_AT_MS  : JOIN_SEED_AT_MS;
        unsigned long tradeAt = ctx.isHost ? HOST_TRADE_AT_MS : JOIN_TRADE_AT_MS;
        if (!seeded_ && ctx.elapsedMs >= seedAt)  { seeded_ = true; doSeed(ctx); }
        if (!traded_ && ctx.elapsedMs >= tradeAt) { traded_ = true; doTrade(ctx); }
        if (ctx.elapsedMs >= DURATION_MS) {
            passed_ = (walletReads_ > 0) && traded_ && tradeOk_;
            return true;
        }
        return false;
    }

private:
    // One WALLET + one TINV line per distinct squad tab (keyed by rank).
    void logSeries(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        for (unsigned int rank = 0; rank < 4; ++rank) {
            int li = tabLeaderIdx(sq, n, rank);
            if (li < 0) continue;
            unsigned int h[5];
            handFromEntity(sq[li], h);
            int money = -1;
            if (engine::readWalletByHand(h, &money)) ++walletReads_;
            char b[96];
            _snprintf(b, sizeof(b) - 1, "SCENARIO WALLET rank=%u money=%d t=%lu",
                      rank, money, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            InvItemEntry items[INV_ITEMS_MAX];
            unsigned int hash = 0;
            unsigned int cnt = engine::captureContainerContents(
                ctx.gw, h, items, INV_ITEMS_MAX, &hash);
            char c[112];
            _snprintf(c, sizeof(c) - 1, "SCENARIO TINV rank=%u count=%u hash=%u t=%lu",
                      rank, cnt, hash, ctx.elapsedMs);
            c[sizeof(c) - 1] = '\0'; coop::logLine(c);
        }
    }

    // The tab this side owns: host rank 0, join rank 1 (leader fallback).
    bool ownLeaderHand(const ScenarioContext& ctx, unsigned int h[5],
                       unsigned int* outRank) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        unsigned int rank = ctx.isHost ? 0u : 1u;
        int li = tabLeaderIdx(sq, n, rank);
        if (li < 0) { li = tabLeaderIdx(sq, n, 0u); rank = 0u; }
        if (li < 0) return false;
        handFromEntity(sq[li], h);
        if (outRank) *outRank = rank;
        return true;
    }

    void doSeed(const ScenarioContext& ctx) {
        unsigned int h[5]; unsigned int rank = 0;
        int ok = 0, target = ctx.isHost ? SEED_HOST : SEED_JOIN;
        if (ownLeaderHand(ctx, h, &rank))
            ok = engine::writeWalletByHand(h, target) ? 1 : 0;
        char b[112];
        _snprintf(b, sizeof(b) - 1, "SCENARIO TRADESEED who=%s rank=%u target=%d ok=%d t=%lu",
                  ctx.isHost ? "host" : "join", rank, target, ok, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void doTrade(const ScenarioContext& ctx) {
        unsigned int h[5]; unsigned int rank = 0;
        int w0 = -1, w1 = -1, added = 0;
        unsigned int cnt0 = 0, cnt1 = 0, hash = 0;
        char sid[48]; sid[0] = '\0';
        if (ownLeaderHand(ctx, h, &rank)) {
            InvItemEntry items[INV_ITEMS_MAX];
            engine::readWalletByHand(h, &w0);
            cnt0 = engine::captureContainerContents(ctx.gw, h, items, INV_ITEMS_MAX, &hash);
            // The two buyer-side mutations of one purchase, same tick.
            added = engine::addTestItemsToContainer(ctx.gw, h, 1, sid, sizeof(sid));
            if (w0 >= PRICE) engine::writeWalletByHand(h, w0 - PRICE);
            engine::readWalletByHand(h, &w1);
            cnt1 = engine::captureContainerContents(ctx.gw, h, items, INV_ITEMS_MAX, &hash);
        }
        tradeOk_ = (added > 0) && (w1 >= 0) && (w0 - w1 == PRICE);
        char b[208];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO TRADE who=%s rank=%u ok=%d sid='%s' price=%d "
                  "wBefore=%d wAfter=%d cntBefore=%u cntAfter=%u t=%lu",
                  ctx.isHost ? "host" : "join", rank, tradeOk_ ? 1 : 0, sid, PRICE,
                  w0, w1, cnt0, cnt1, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    static const unsigned long HOST_SEED_AT_MS  = 6000;
    static const unsigned long HOST_TRADE_AT_MS = 10000;
    static const unsigned long JOIN_SEED_AT_MS  = 18000;
    static const unsigned long JOIN_TRADE_AT_MS = 22000;
    static const unsigned long DURATION_MS      = 40000;
    static const unsigned int  MAX_SQUAD        = 32;
    static const int           SEED_HOST        = 5000;
    static const int           SEED_JOIN        = 7000;
    static const int           PRICE            = 250;

    bool          seeded_;
    bool          traded_;
    bool          tradeOk_;
    unsigned int  walletReads_;
};

// recruit_probe (protocol 23 phase 0, probe tier): mid-session recruitment
// evidence. No recruit sync exists - a recruit exists on the recruiting client
// only - and the DESIGN questions are identity-shaped:
//   * does PlayerInterface::recruit work programmatically on both sides?
//   * what happens to the subject's HAND - the container MUST change (it moves
//     into a player platoon); do index/serial survive (peer could re-key) or
//     is the identity fully broken?
//   * does the recruit land in an EXISTING tab (rank stable) or mint a NEW
//     platoon (the sorted-container rank partition could RESHUFFLE mid-session
//     - the ownership hazard)?
//   * what does the PEER see? For the BAKED leg its copy of the subject still
//     stands (now unstreamed by the recruiter -> authority suppression?); the
//     recruiter streams an unresolvable new hand (spawn-sync REQ/proxy?). The
//     oracle cross-references the [spawn]/[authority] logs for both.
// Script: 1 Hz TABS census (distinct sorted containers + squad size) on both
// sides; host t=10s recruits the nearest BAKED world NPC, t=14s a RUNTIME
// spawn; join the same at t=22s/26s. Verdict gates only that the script ran
// (both legs logged + census series present); everything else is FINDINGs.
//
// The same script doubles as recruit_sync (probe=false, full tier): recruit
// sync stays ON (protocol 23) and every leg must actually SUCCEED locally
// (res=1); the Test-RecruitSync oracle then gates the cross-machine half from
// the logs - the peer re-keyed its local body to each recruited hand (baked
// legs, no duplicate proxy) or minted one via the bidirectional describe
// channel (runtime legs), and tracked it (SCENARIO PROXY series).
class RecruitProbeScenario : public TimedScenario {
public:
    explicit RecruitProbeScenario(bool probe)
        : TimedScenario(probe ? "recruit_probe" : "recruit_sync", /*evidenceMs=*/1000),
          probe_(probe),
          bakedDone_(false), runtimeDone_(false),
          bakedRes_(-9), runtimeRes_(-9), tabsLogged_(0) {}

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO RECRUITPROBE start host=%d",
                  ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (evidenceDue(ctx.elapsedMs)) {
            logTabs(ctx);
        }
        unsigned long bakedAt   = ctx.isHost ? HOST_ACT_AT_MS : JOIN_ACT_AT_MS;
        unsigned long runtimeAt = bakedAt + 4000;
        if (!bakedDone_ && ctx.elapsedMs >= bakedAt) {
            bakedDone_ = true;
            bakedRes_ = doRecruit(ctx, /*runtime=*/false);
        }
        if (!runtimeDone_ && ctx.elapsedMs >= runtimeAt) {
            runtimeDone_ = true;
            runtimeRes_ = doRecruit(ctx, /*runtime=*/true);
        }
        if (ctx.elapsedMs >= DURATION_MS) {
            passed_ = bakedDone_ && runtimeDone_ && (tabsLogged_ > 0);
            // The gated variant requires the recruits to have actually happened
            // (the probe only requires the script to have run - failure IS data).
            if (!probe_) passed_ = passed_ && (bakedRes_ == 1) && (runtimeRes_ == 1);
            return true;
        }
        return false;
    }

private:
    // Distinct sorted squad-tab containers + squad size: the rank-partition
    // census whose REORDERING mid-series is the ownership-reshuffle finding.
    void logTabs(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        std::vector<std::pair<unsigned int, unsigned int> > ctnrs;
        for (unsigned int i = 0; i < n; ++i)
            ctnrs.push_back(std::make_pair(sq[i].hContainer, sq[i].hContainerSerial));
        std::sort(ctnrs.begin(), ctnrs.end());
        ctnrs.erase(std::unique(ctnrs.begin(), ctnrs.end()), ctnrs.end());
        char list[128]; list[0] = '\0';
        unsigned int used = 0;
        for (unsigned int i = 0; i < ctnrs.size() && used + 24 < sizeof(list); ++i) {
            used += (unsigned int)_snprintf(list + used, sizeof(list) - used - 1,
                                            "%s%u:%u", i ? "|" : "",
                                            ctnrs[i].first, ctnrs[i].second);
        }
        list[sizeof(list) - 1] = '\0';
        char b[208];
        _snprintf(b, sizeof(b) - 1, "SCENARIO TABS n=%u squad=%u list=%s t=%lu",
                  (unsigned int)ctnrs.size(), n, list, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        ++tabsLogged_;
    }

    int doRecruit(const ScenarioContext& ctx, bool runtime) {
        unsigned int hb[5], ha[5];
        int res = engine::probeRecruit(ctx.gw, runtime, hb, ha);
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO RECRUIT who=%s leg=%s res=%d "
                  "before=%u,%u,%u,%u,%u after=%u,%u,%u,%u,%u t=%lu",
                  ctx.isHost ? "host" : "join", runtime ? "runtime" : "baked", res,
                  hb[0], hb[1], hb[2], hb[3], hb[4],
                  ha[0], ha[1], ha[2], ha[3], ha[4], ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return res;
    }

    static const unsigned long HOST_ACT_AT_MS = 10000;
    static const unsigned long JOIN_ACT_AT_MS = 22000;
    static const unsigned long DURATION_MS    = 40000;
    static const unsigned int  MAX_SQUAD      = 48;

    bool          probe_;
    bool          bakedDone_;
    bool          runtimeDone_;
    int           bakedRes_;
    int           runtimeRes_;
    unsigned int  tabsLogged_;
};

// recruit_ctl (Phase 1b gait + phantom VALIDATION, full tier): the recruit
// family's CONTROL scenario. recruit_sync proves a recruit CONVERGES + joins
// the squad on the peer; recruit_ctl proves the two Phase-1b behaviors layered
// on top - (1) a DRIVEN squad member reproduces the owner's RUN gait (not a
// walk), and (2) a control-flip TRANSFER does not mint a phantom duplicate.
// Timeline (save 'sync', both squads present; clock from ARM = peer-ready):
//   t=8s   HOST recruits the nearest baked NPC. The join re-keys its copy onto
//          the host hand (Case A) + inserts it as a peer-owned member, so the
//          join finds it as the NEW squad member (set-diff vs the onStart base).
//   t=12-20s  HOST walks the recruit (owner). Both sides log SCENARIO GAIT
//          phase=A: host=owner (streams a run), join=driver (its reproduced
//          readMotion speed is the gait-parity sample - a WALK here is the bug).
//   t=24s  JOIN moves the recruit INTO its own tab (lever 1) -> control-flip:
//          the join CLAIMS ownership, the host re-keys + becomes the driver.
//          The host's last in-flight batches for the now-owned hand must NOT
//          mint a proxy (the phantom "Squint").
//   t=28-36s  JOIN walks the recruit (owner now). Both sides log GAIT phase=B:
//          join=owner, host=driver (validates the host-side drive mirror too).
//   t=40s  end.
// Each side reads its OWN stored Character* for the gait sample (stable across
// recruit/move - setFaction re-containers but never recreates the object), so
// no cross-hand correlation is needed. Test-RecruitCtl gates gait-parity
// (driver median speed vs owner median per phase) + anti-phantom (no proxy
// BOUND for a CONTROL-FLIP-claimed hand; end squad-size parity).
class RecruitCtlScenario : public TimedScenario {
public:
    RecruitCtlScenario()
        : TimedScenario("recruit_ctl", /*evidenceMs=*/0),
          lastGaitMs_(0), lastWalkMs_(0), altDir_(0),
          recruitDone_(false), recruitRes_(-9), subject_(0),
          moveDone_(false), moveRes_(-9), baseN_(0), haveHome_(false) {
        memset(recruitHand_, 0, sizeof(recruitHand_));
        memset(homeHand_, 0, sizeof(homeHand_));
        for (int i = 0; i < MAX_BASE; ++i)
            for (int j = 0; j < 5; ++j) baseHands_[i][j] = 0;
    }

    virtual void onStart(const ScenarioContext& ctx) {
        // Baseline squad hands: the join identifies the NEW member (the recruit)
        // by set difference, and the HOST uses them to find the JOIN's tab (the
        // transfer target - the baseline member whose container differs from the
        // recruit's, i.e. the OTHER squad's tab) at transfer time.
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        baseN_ = 0;
        for (unsigned int i = 0; i < n && baseN_ < MAX_BASE; ++i)
            handFromEntity(sq[i], baseHands_[baseN_++]);
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO CTL start host=%d base=%u",
                  ctx.isHost ? 1 : 0, baseN_);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // 1) Recruit (host t=8s).
        if (ctx.isHost && !recruitDone_ && ctx.elapsedMs >= RECRUIT_AT_MS) {
            recruitDone_ = true;
            unsigned int hb[5], ha[5];
            recruitRes_ = engine::probeRecruit(ctx.gw, /*runtime=*/false, hb, ha);
            if (recruitRes_ == 1) {
                memcpy(recruitHand_, ha, sizeof(recruitHand_));
                subject_ = engine::resolveCharByHand(ha[3], ha[4], ha[0],
                                                     ha[1], ha[2]);
            }
            char b[176];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO CTL recruit res=%d hand=%u,%u,%u,%u,%u t=%lu",
                      recruitRes_, ha[0], ha[1], ha[2], ha[3], ha[4],
                      ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }

        // Join: discover the recruit as the NEW squad member (poll until found).
        if (!ctx.isHost && !subject_) findNewMember(ctx);

        // 2) Gait sampling (both sides, ~4 Hz inside a walk window).
        if (subject_ && (ctx.elapsedMs - lastGaitMs_ >= 250)) {
            int phase = phaseAt(ctx.elapsedMs);
            if (phase >= 0) { lastGaitMs_ = ctx.elapsedMs; logGait(ctx, phase); }
        }

        // 3) Owner walk-drive: the phase's OWNER re-issues a far run every 1s.
        //    Phase A owner = host; phase B owner = join.
        if (subject_) {
            int phase = phaseAt(ctx.elapsedMs);
            bool owner = (phase == 0 && ctx.isHost) || (phase == 1 && !ctx.isHost);
            if (owner && (ctx.elapsedMs - lastWalkMs_ >= 1000)) {
                lastWalkMs_ = ctx.elapsedMs;
                driveWalk(ctx);
            }
        }

        // 4) Transfer (HOST t=24s): the host moves the recruit INTO the JOIN's
        //    tab. This is the direction that flips control TO the join - the
        //    join RECEIVES the move into a tab it owns, so its rekeyPeerBody
        //    claims ownership (CONTROL-FLIP) and the phantom-mint race fires on
        //    the join (the reproduction of manual 2026-07-17: Squint). A
        //    join-authored move into its own tab does NOT flip (the author does
        //    not run rekeyPeerBody; the host receiver does not own the dest).
        if (ctx.isHost && subject_ && !moveDone_ && ctx.elapsedMs >= MOVE_AT_MS) {
            moveDone_ = true;
            doTransfer(ctx);
        }

        if (ctx.elapsedMs >= DURATION_MS) {
            // Host: the recruit landed AND the transfer was authored (rc=1).
            // Join: it found the recruit (and, post-flip, owns + walks it in
            // phase B). The cross-machine gait + phantom verdict is
            // Test-RecruitCtl's job (reads the GAIT/spawn logs on both files).
            if (ctx.isHost) passed_ = (recruitRes_ == 1) && moveDone_ &&
                                      (moveRes_ == 1);
            else            passed_ = (subject_ != 0);
            return true;
        }
        return false;
    }

private:
    static bool sameHand(const unsigned int a[5], const unsigned int b[5]) {
        return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] &&
               a[3] == b[3] && a[4] == b[4];
    }

    // -1 outside a walk window, 0 = phase A, 1 = phase B.
    int phaseAt(unsigned long t) const {
        if (t >= PHASE_A_FROM && t < PHASE_A_TO) return 0;
        if (t >= PHASE_B_FROM && t < PHASE_B_TO) return 1;
        return -1;
    }

    void findNewMember(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        for (unsigned int i = 0; i < n; ++i) {
            unsigned int h[5]; handFromEntity(sq[i], h);
            bool baseline = false;
            for (unsigned int b = 0; b < baseN_; ++b)
                if (sameHand(h, baseHands_[b])) { baseline = true; break; }
            if (baseline) continue;
            Character* c = engine::resolveCharByHand(h[3], h[4], h[0], h[1], h[2]);
            if (!c) continue;
            memcpy(recruitHand_, h, sizeof(recruitHand_));
            subject_ = c;
            char b[176];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO CTL found hand=%u,%u,%u,%u,%u t=%lu",
                      h[0], h[1], h[2], h[3], h[4], ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            return;
        }
    }

    // Live hand of the subject (re-read every call so it tracks a re-key /
    // control-flip re-container). readObjectHand + handFromEntity share the same
    // [t,c,cs,i,s] layout, so the result compares directly against a squad
    // capture.
    bool subjectHand(unsigned int out[5]) {
        if (!subject_) return false;
        return engine::readObjectHand(reinterpret_cast<RootObject*>(subject_), out);
    }

    // Current position of the subject via a squad capture matched on its LIVE
    // hand (tracks the post-transfer re-container).
    bool subjectPos(const ScenarioContext& ctx, float* x, float* y, float* z) {
        unsigned int cur[5];
        if (!subjectHand(cur)) return false;
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        for (unsigned int i = 0; i < n; ++i) {
            unsigned int h[5]; handFromEntity(sq[i], h);
            if (sameHand(h, cur)) {
                *x = sq[i].x; *y = sq[i].y; *z = sq[i].z; return true;
            }
        }
        return false;
    }

    void logGait(const ScenarioContext& ctx, int phase) {
        bool moving = false; float speed = 0.0f;
        int ok = engine::readMotion(subject_, &moving, &speed) ? 1 : 0;
        bool owner = (phase == 0) ? ctx.isHost : (!ctx.isHost);
        char b[176];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO GAIT who=%s phase=%c role=%s moving=%d speed=%.2f "
                  "ok=%d t=%lu",
                  ctx.isHost ? "host" : "join", (phase == 0) ? 'A' : 'B',
                  owner ? "own" : "drive", moving ? 1 : 0, speed, ok,
                  ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void driveWalk(const ScenarioContext& ctx) {
        float x, y, z;
        if (!subjectPos(ctx, &x, &y, &z)) return;
        // Order a FAR run so the engine picks the run gait; alternate the
        // direction so the body bounces in-region for the whole window.
        float d = (altDir_ & 1) ? -180.0f : 180.0f;
        ++altDir_;
        engine::walkTo(subject_, x + d, y, z, 30.0f);
    }

    // The host moves the recruit INTO the join's tab. Target = the baseline
    // member whose container differs from the recruit's current container (on
    // the 2-tab 'sync' save that is the join's own squad tab - robust to which
    // rank number each side owns). memberHand is the subject's LIVE hand.
    void doTransfer(const ScenarioContext& ctx) {
        unsigned int mh[5];
        if (!subjectHand(mh)) { moveRes_ = -8; homeLog(ctx, 0); return; }
        // Pick the join's tab leader: a baseline hand with a different (c,cs).
        int home = -1;
        for (unsigned int i = 0; i < baseN_; ++i) {
            if (baseHands_[i][1] != mh[1] || baseHands_[i][2] != mh[2]) {
                home = (int)i; break;
            }
        }
        if (home < 0) { moveRes_ = -7; homeLog(ctx, 0); return; }
        memcpy(homeHand_, baseHands_[home], sizeof(homeHand_)); haveHome_ = true;
        unsigned int hb[5], ha[5];
        moveRes_ = engine::probeMoveSquadMember(ctx.gw, mh, homeHand_,
                                                /*lever=*/1, hb, ha);
        if (moveRes_ == 1) memcpy(recruitHand_, ha, sizeof(recruitHand_));
        char b[240];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO CTL move rc=%d target=%u,%u before=%u,%u,%u,%u,%u "
                  "after=%u,%u,%u,%u,%u t=%lu",
                  moveRes_, homeHand_[1], homeHand_[2],
                  hb[0], hb[1], hb[2], hb[3], hb[4],
                  ha[0], ha[1], ha[2], ha[3], ha[4], ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void homeLog(const ScenarioContext& ctx, int) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO CTL move rc=%d (no target) t=%lu",
                  moveRes_, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    static const unsigned long RECRUIT_AT_MS = 8000;
    static const unsigned long PHASE_A_FROM  = 12000;
    static const unsigned long PHASE_A_TO    = 20000;
    static const unsigned long MOVE_AT_MS    = 24000;
    static const unsigned long PHASE_B_FROM  = 28000;
    static const unsigned long PHASE_B_TO    = 36000;
    static const unsigned long DURATION_MS   = 40000;
    static const unsigned int  MAX_SQUAD     = 48;
    static const unsigned int  MAX_BASE      = 48;

    unsigned long lastGaitMs_;
    unsigned long lastWalkMs_;
    unsigned int  altDir_;
    bool          recruitDone_;
    int           recruitRes_;
    Character*    subject_;
    bool          moveDone_;
    int           moveRes_;
    unsigned int  baseN_;
    unsigned int  baseHands_[MAX_BASE][5];
    unsigned int  recruitHand_[5];
    unsigned int  homeHand_[5];
    bool          haveHome_;
};

// squad_probe (protocol 35 phase 0, probe tier; squadSync forced OFF) /
// squad_sync (probe=false, full tier; squadSync ON). Moving a unit between
// squad tabs RE-CONTAINERS it - the hand changes like a recruit's - but no
// engine function owns the UI drag, and the rank partition re-sorts the
// distinct containers EVERY tick, so a move breaks stream identity AND can
// reshuffle whole-tab ownership. The probe's DESIGN questions:
//   * pointer-diff detection: does the ~1 Hz roster poll (Character* -> hand
//     baseline) catch the separate-into-new-squad re-container (the SQEDGE
//     lines must mirror the SQMOVE before/after pair)?
//   * identity: which hand fields survive a move (container must change -
//     do index/serial hold, the re-key precondition)?
//   * rank reshuffle: when the new tab appears (and disappears on the move
//     back), do the PRE-EXISTING tabs keep their ranks (the SQTABS series
//     ordering) or does ownership silently flip (the hazard)?
//   * move-back lever: does Character::setFaction(playerFaction, platoon)
//     (lever 1) or ActivePlatoon::addCharacterAt (lever 2) land a member in
//     an EXISTING tab programmatically - and does the returning member get
//     its ORIGINAL hand back or a fresh index (a second re-key)?
//   * peer behavior: what does the other side see while sync is OFF (the
//     unresolved-hand telemetry / authority suppression on the join, the
//     stale copy in the old tab - the gap protocol 35 closes)?
// Script: 1 Hz SQTABS census (distinct sorted containers + per-tab member
// counts) on both sides; the probe tier also polls the roster + drains
// SQEDGE lines (the sync tier leaves both to the Replicator). HOST t=10s
// separates its own tab's HIGHEST-hand member into a new squad (lever 0,
// the setup-scene-proven path), t=20s moves it back into its original tab
// (lever 1; lever 2 fallback t=26s if the hand did not change). The JOIN's
// tab is single-member on the squad1 save and a solo separate is an engine
// no-op (probe run 185825), so the join goes the other way around: t=30s
// lever 1 moves its member INTO the host's rank-0 tab (lever 2 fallback
// t=36s), t=40s lever 0 separates it back OUT into its own new tab. 55 s.
// Probe gates only that the script ran (census + both sides' attempts
// logged); the sync tier additionally requires every scripted move to have
// LANDED locally (rc=1) - crossing is Test-SquadSync's job.
class SquadProbeScenario : public TimedScenario {
public:
    explicit SquadProbeScenario(bool probe)
        : TimedScenario(probe ? "squad_probe" : "squad_sync", /*evidenceMs=*/1000),
          probe_(probe), tabsLogged_(0),
          sepDone_(false), sepRc_(-9), backDone_(false), backRc_(-9),
          back2Done_(false), back2Rc_(-9), havePick_(false) {
        memset(memberHand_, 0, sizeof(memberHand_));
        memset(homeHand_, 0, sizeof(homeHand_));
    }

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SQUADPROBE start host=%d",
                  ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (evidenceDue(ctx.elapsedMs)) {
            logTabs(ctx);
            // Probe tier: the scenario owns the roster poll + edge drain (sync
            // is forced OFF, so the Replicator is not competing for the queue).
            if (probe_) logEdges(ctx);
        }
        if (ctx.isHost) {
            // Separate OUT (L0), then move BACK into the original tab (L1,
            // L2 fallback).
            if (!sepDone_ && ctx.elapsedMs >= HOST_SEP_AT_MS) {
                sepDone_ = true;
                doSeparate(ctx);
            }
            if (sepRc_ == 1 && !backDone_ &&
                ctx.elapsedMs >= HOST_SEP_AT_MS + BACK_DELAY_MS) {
                backDone_ = true;
                backRc_ = doLeverMove(ctx, 1);
            }
            if (backDone_ && backRc_ != 1 && !back2Done_ &&
                ctx.elapsedMs >= HOST_SEP_AT_MS + BACK_DELAY_MS + FALLBACK_DELAY_MS) {
                back2Done_ = true;
                back2Rc_ = doLeverMove(ctx, 2);
            }
        } else {
            // The join's tab is solo (a separate would no-op), so: move INTO
            // the host's tab first (L1, L2 fallback), then separate back OUT
            // (L0 - now a multi-member tab, the proven path).
            if (!backDone_ && ctx.elapsedMs >= JOIN_IN_AT_MS) {
                backDone_ = true;
                backRc_ = doLeverMove(ctx, 1);
            }
            if (backDone_ && backRc_ != 1 && !back2Done_ &&
                ctx.elapsedMs >= JOIN_IN_AT_MS + FALLBACK_DELAY_MS) {
                back2Done_ = true;
                back2Rc_ = doLeverMove(ctx, 2);
            }
            if ((backRc_ == 1 || back2Rc_ == 1) && !sepDone_ &&
                ctx.elapsedMs >= JOIN_IN_AT_MS + BACK_DELAY_MS) {
                sepDone_ = true;
                doSeparate(ctx);
            }
        }
        if (ctx.elapsedMs >= DURATION_MS) {
            // Probe: only that the script RAN (a refusal IS data). Sync tier:
            // every scripted move must have LANDED locally (rc=1) - the
            // cross-machine half is Test-SquadSync's job.
            passed_ = (tabsLogged_ > 0) && (ctx.isHost ? sepDone_ : backDone_);
            if (!probe_) {
                bool moveIn = (backRc_ == 1) || (back2Rc_ == 1);
                passed_ = passed_ && moveIn && (sepRc_ == 1);
            }
            return true;
        }
        return false;
    }

private:
    // Distinct sorted squad-tab containers with per-tab member counts: the
    // rank-partition census whose ORDERING drift mid-series is the ownership-
    // reshuffle finding (and whose new/vanishing rows time the move legs).
    void logTabs(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        std::vector<std::pair<unsigned int, unsigned int> > ctnrs;
        for (unsigned int i = 0; i < n; ++i)
            ctnrs.push_back(std::make_pair(sq[i].hContainer, sq[i].hContainerSerial));
        std::sort(ctnrs.begin(), ctnrs.end());
        ctnrs.erase(std::unique(ctnrs.begin(), ctnrs.end()), ctnrs.end());
        char list[160]; list[0] = '\0';
        unsigned int used = 0;
        for (unsigned int i = 0; i < ctnrs.size() && used + 32 < sizeof(list); ++i) {
            unsigned int cnt = 0;
            for (unsigned int k = 0; k < n; ++k)
                if (sq[k].hContainer == ctnrs[i].first &&
                    sq[k].hContainerSerial == ctnrs[i].second) ++cnt;
            used += (unsigned int)_snprintf(list + used, sizeof(list) - used - 1,
                                            "%s%u:%u:%u", i ? "|" : "",
                                            ctnrs[i].first, ctnrs[i].second, cnt);
        }
        list[sizeof(list) - 1] = '\0';
        char b[240];
        _snprintf(b, sizeof(b) - 1, "SCENARIO SQTABS n=%u squad=%u list=%s t=%lu",
                  (unsigned int)ctnrs.size(), n, list, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        ++tabsLogged_;
    }

    // Probe tier: poll + drain the pointer-diff queue, logging every edge -
    // the detection-mechanism evidence the SQMOVE pairs are checked against.
    void logEdges(const ScenarioContext& ctx) {
        engine::pollSquadRoster(ctx.gw);
        engine::SquadMoveEdge edges[8];
        unsigned int n = engine::drainSquadMoveEdges(edges, 8);
        for (unsigned int i = 0; i < n; ++i) {
            char b[208];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO SQEDGE who=%s before=%u,%u,%u,%u,%u "
                      "after=%u,%u,%u,%u,%u t=%lu",
                      ctx.isHost ? "host" : "join",
                      edges[i].before[0], edges[i].before[1], edges[i].before[2],
                      edges[i].before[3], edges[i].before[4],
                      edges[i].after[0], edges[i].after[1], edges[i].after[2],
                      edges[i].after[3], edges[i].after[4], ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
    }

    // Pick OUR tab's HIGHEST-hand member (never the tab leader - interest
    // centers and the coop_presence mover anchor on the lowest hand) and
    // remember the RANK-0 tab leader's hand as the lever-1/2 target (the
    // host's move-back home; the tab the join moves INTO).
    bool pickMember(const ScenarioContext& ctx) {
        if (havePick_) return true;
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        unsigned int ownRank = ctx.isHost ? 0u : 1u;
        int lead = tabLeaderIdx(sq, n, ownRank);
        int home = tabLeaderIdx(sq, n, 0u);
        if (lead < 0 || home < 0) return false;
        int best = -1;
        for (unsigned int i = 0; i < n; ++i) {
            if (tabRankOf(sq, n, i) != (int)ownRank) continue;
            if ((int)i == lead) continue;
            if (best < 0 || tabHandLess(sq[best], sq[i])) best = (int)i;
        }
        // A single-member tab falls back to its leader (a FINDING, not a
        // skip: the move mechanics are identical, the anchor just shifts).
        if (best < 0) best = lead;
        handFromEntity(sq[best], memberHand_);
        handFromEntity(sq[home], homeHand_);
        havePick_ = true;
        char b[176];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO SQPICK who=%s member=%u,%u,%u,%u,%u home=%u,%u,%u,%u,%u "
                  "leaderFallback=%d t=%lu",
                  ctx.isHost ? "host" : "join",
                  memberHand_[0], memberHand_[1], memberHand_[2], memberHand_[3],
                  memberHand_[4], homeHand_[0], homeHand_[1], homeHand_[2],
                  homeHand_[3], homeHand_[4], (best == lead) ? 1 : 0, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        return true;
    }

    void doSeparate(const ScenarioContext& ctx) {
        if (!pickMember(ctx)) { sepRc_ = -8; logMove(ctx, 0, -8, 0, 0); return; }
        unsigned int hb[5], ha[5];
        sepRc_ = engine::probeMoveSquadMember(ctx.gw, memberHand_, 0, /*lever*/0,
                                              hb, ha);
        if (sepRc_ == 1) memcpy(memberHand_, ha, sizeof(memberHand_));
        logMove(ctx, 0, sepRc_, hb, ha);
    }

    int doLeverMove(const ScenarioContext& ctx, int lever) {
        if (!pickMember(ctx)) { logMove(ctx, lever, -8, 0, 0); return -8; }
        unsigned int hb[5], ha[5];
        int rc = engine::probeMoveSquadMember(ctx.gw, memberHand_, homeHand_,
                                              lever, hb, ha);
        if (rc == 1) memcpy(memberHand_, ha, sizeof(memberHand_));
        logMove(ctx, lever, rc, hb, ha);
        return rc;
    }

    void logMove(const ScenarioContext& ctx, int lever, int rc,
                 const unsigned int* hb, const unsigned int* ha) {
        static const unsigned int Z[5] = { 0, 0, 0, 0, 0 };
        if (!hb) hb = Z;
        if (!ha) ha = Z;
        char b[224];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO SQMOVE who=%s lever=%d rc=%d "
                  "before=%u,%u,%u,%u,%u after=%u,%u,%u,%u,%u t=%lu",
                  ctx.isHost ? "host" : "join", lever, rc,
                  hb[0], hb[1], hb[2], hb[3], hb[4],
                  ha[0], ha[1], ha[2], ha[3], ha[4], ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    static const unsigned long HOST_SEP_AT_MS    = 10000;
    static const unsigned long JOIN_IN_AT_MS     = 30000;
    static const unsigned long BACK_DELAY_MS     = 10000;
    static const unsigned long FALLBACK_DELAY_MS = 6000;
    static const unsigned long DURATION_MS       = 55000;
    static const unsigned int  MAX_SQUAD         = 48;

    bool          probe_;
    unsigned int  tabsLogged_;
    bool          sepDone_;
    int           sepRc_;
    bool          backDone_;
    int           backRc_;
    bool          back2Done_;
    int           back2Rc_;
    bool          havePick_;
    unsigned int  memberHand_[5];
    unsigned int  homeHand_[5];
};

// faction_probe (protocol 24 phase 0, probe tier; factionSync forced OFF) /
// faction_sync (probe=false, full tier; sync ON). Relation state between the
// player faction and world factions is per-client `FactionRelations` with no
// channel - attacking a faction flips hostility on ONE machine only. The
// probe's DESIGN questions:
//   * are faction GameData stringIDs cross-client stable (the wire identity)?
//   * a sentinel FactionRelations::setRelation on one side - does anything
//     cross (expected: no) and does the write itself stick (both rows)?
//   * which row does the engine keep operative - the player faction's table
//     toward them, THEIR table toward the player, or mirrored?
//   * what do REAL mutations look like (the [fac] AFFECT detour lines)?
// Script: 1 Hz FACREL series (every faction row with a nonzero relation or a
// derived flag, capped, PLUS the sentinel rows) on both sides; host t=10s
// writes sentinel -75 on the first sorted faction, join t=22s writes +65 on
// the second (both rows). The probe gates only that the script ran; the sync
// variant requires both writes ok - Test-FactionSync gates the convergence.
class FactionProbeScenario : public TimedScenario {
public:
    explicit FactionProbeScenario(bool probe)
        : TimedScenario(probe ? "faction_probe" : "faction_sync", /*evidenceMs=*/1000),
          probe_(probe),
          wrote_(false), writeOk_(false), relLogged_(0) {
        sentinelSid_[0] = '\0';
    }

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO FACPROBE start host=%d",
                  ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (evidenceDue(ctx.elapsedMs)) {
            logRelations(ctx);
        }
        unsigned long writeAt = ctx.isHost ? HOST_WRITE_AT_MS : JOIN_WRITE_AT_MS;
        if (!wrote_ && ctx.elapsedMs >= writeAt) {
            wrote_ = true;
            doSentinel(ctx);
        }
        if (ctx.elapsedMs >= DURATION_MS) {
            passed_ = wrote_ && (relLogged_ > 0);
            // The gated variant requires the sentinel write to have stuck
            // (the probe only requires the script to have run).
            if (!probe_) passed_ = passed_ && writeOk_;
            return true;
        }
        return false;
    }

private:
    // One FACREL line per INTERESTING faction row (nonzero relation on either
    // side, or a derived flag set, or the sentinel target) + a census line.
    void logRelations(const ScenarioContext& ctx) {
        engine::FactionRead rows[MAX_FACTIONS];
        unsigned int n = engine::listPlayerRelations(ctx.gw, rows, MAX_FACTIONS);
        char c[96];
        _snprintf(c, sizeof(c) - 1, "SCENARIO FACCOUNT n=%u t=%lu", n, ctx.elapsedMs);
        c[sizeof(c) - 1] = '\0'; coop::logLine(c);
        unsigned int logged = 0;
        for (unsigned int i = 0; i < n; ++i) {
            const engine::FactionRead& r = rows[i];
            bool sentinel = sentinelSid_[0] != '\0' && strcmp(r.sid, sentinelSid_) == 0;
            bool interesting = sentinel ||
                r.usToThem <= -0.5f || r.usToThem >= 0.5f ||
                r.themToUs <= -0.5f || r.themToUs >= 0.5f ||
                r.enemy == 1 || r.enemyRecip == 1 || r.ally == 1;
            if (!interesting) continue;
            if (!sentinel && logged >= MAX_LOG_ROWS) continue;
            char b[208];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO FACREL sid='%s' us=%.1f them=%.1f enemy=%d erecip=%d ally=%d t=%lu",
                      r.sid, r.usToThem, r.themToUs, r.enemy, r.enemyRecip, r.ally,
                      ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
            ++logged;
        }
        if (logged > 0) ++relLogged_;
    }

    // Deterministic cross-client pick: sort every readable sid ascending; the
    // host writes the FIRST, the join the SECOND (distinct factions, distinct
    // values - two independent crossing legs for the oracle to pair).
    bool pickSentinel(const ScenarioContext& ctx, char* outSid, unsigned int outLen) {
        engine::FactionRead rows[MAX_FACTIONS];
        unsigned int n = engine::listPlayerRelations(ctx.gw, rows, MAX_FACTIONS);
        if (n == 0) return false;
        std::vector<std::string> sids;
        for (unsigned int i = 0; i < n; ++i) sids.push_back(std::string(rows[i].sid));
        std::sort(sids.begin(), sids.end());
        unsigned int idx = ctx.isHost ? 0u : (n > 1 ? 1u : 0u);
        strncpy(outSid, sids[idx].c_str(), outLen - 1);
        outSid[outLen - 1] = '\0';
        return true;
    }

    void doSentinel(const ScenarioContext& ctx) {
        float target = ctx.isHost ? (float)SENTINEL_HOST : (float)SENTINEL_JOIN;
        float before = -999.0f, after = -999.0f;
        int ok = 0;
        if (pickSentinel(ctx, sentinelSid_, sizeof(sentinelSid_))) {
            ok = engine::writeRelationBySid(ctx.gw, sentinelSid_, target,
                                            /*reciprocal*/ true, &before, &after) ? 1 : 0;
        }
        writeOk_ = (ok == 1) && (after > target - 0.5f) && (after < target + 0.5f);
        char b[208];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO FACWRITE who=%s sid='%s' target=%.1f ok=%d before=%.1f after=%.1f t=%lu",
                  ctx.isHost ? "host" : "join", sentinelSid_, target, ok, before, after,
                  ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    static const unsigned long HOST_WRITE_AT_MS = 10000;
    static const unsigned long JOIN_WRITE_AT_MS = 22000;
    static const unsigned long DURATION_MS      = 40000;
    static const unsigned int  MAX_FACTIONS     = 96;
    static const unsigned int  MAX_LOG_ROWS     = 10;
    static const int           SENTINEL_HOST    = -75;
    static const int           SENTINEL_JOIN    = 65;

    bool          probe_;
    bool          wrote_;
    bool          writeOk_;
    unsigned int  relLogged_;
    char          sentinelSid_[48];
};

// time_probe (protocol 25 phase 0, probe tier; timeSync AND speedSync forced
// OFF) / time_sync (probe=false, full tier; both syncs ON). Each client
// integrates its own game clock from its own load/pause moments - day/night
// (NPC schedules, shop hours, stealth vision) diverges with no channel. The
// probe's DESIGN questions:
//   * what does getTimeStamp_inGameHours return - absolute campaign hours
//     (save-derived, both clients near-equal on the shared save) or hours
//     since load (arbitrary offset)?
//   * how big is the initial host/join offset and how fast does it drift?
//   * does the clock rate track frameSpeedMult (a 2x burst -> 2x clock)?
//     That relation is what makes SLEW a viable correction lever.
// Script: 1 Hz GTIME series (clock + hourLen + fsm + paused) on both sides;
// a 2x speed burst t=15..25s - HOST-only loud click in the probe (speedSync
// off, applies directly), BOTH sides click in the sync variant (the consensus
// arbitrates min(2,2)=2x) so convergence is tested across a speed change.
// The probe gates only that the script ran; Test-TimeSync gates convergence.
class TimeProbeScenario : public TimedScenario {
public:
    explicit TimeProbeScenario(bool probe)
        : TimedScenario(probe ? "time_probe" : "time_sync", /*evidenceMs=*/1000),
          probe_(probe),
          burstOn_(false), burstOff_(false), samples_(0) {}

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO TPROBE start host=%d", ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (evidenceDue(ctx.elapsedMs)) {
            logClock(ctx);
        }
        // The burst clicker: probe = host only (speedSync off, direct apply);
        // sync = both sides (each vote 2x, the consensus arbitrates 2x).
        bool iClick = probe_ ? ctx.isHost : true;
        if (iClick && !burstOn_ && ctx.elapsedMs >= BURST_ON_MS) {
            burstOn_ = true;
            doClick(ctx, 2.0f);
        }
        if (iClick && !burstOff_ && ctx.elapsedMs >= BURST_OFF_MS) {
            burstOff_ = true;
            doClick(ctx, 1.0f);
        }
        // The sync variant runs longer: the join's catch-up slew is capped at
        // 2x (gentle on the world), so closing the ~0.3 gh load skew takes
        // ~35 s - the convergence gate needs headroom past that.
        unsigned long duration = probe_ ? DURATION_MS : SYNC_DURATION_MS;
        if (ctx.elapsedMs >= duration) {
            passed_ = (samples_ > 0) && (!iClick || (burstOn_ && burstOff_));
            return true;
        }
        return false;
    }

private:
    void logClock(const ScenarioContext& ctx) {
        double hours = -1.0; float hourLen = -1.0f;
        bool ok = engine::readGameClock(ctx.gw, &hours, &hourLen);
        float mult = -1.0f; bool paused = false;
        engine::readGameSpeed(ctx.gw, &mult, &paused);
        char b[176];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO GTIME hours=%.5f hourLen=%.1f fsm=%.2f paused=%d ok=%d t=%lu",
                  hours, hourLen, mult, paused ? 1 : 0, ok ? 1 : 0, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        if (ok) ++samples_;
    }

    void doClick(const ScenarioContext& ctx, float mult) {
        // The LOUD simulated click: registers as this player's vote under
        // speed consensus (sync variant) and applies directly without it
        // (probe variant, speedSync forced off).
        bool ok = engine::writeGameSpeed(ctx.gw, mult, false);
        char b[112];
        _snprintf(b, sizeof(b) - 1, "SCENARIO TCLICK who=%s mult=%.1f ok=%d t=%lu",
                  ctx.isHost ? "host" : "join", mult, ok ? 1 : 0, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    static const unsigned long BURST_ON_MS      = 15000;
    static const unsigned long BURST_OFF_MS     = 25000;
    static const unsigned long DURATION_MS      = 40000;
    static const unsigned long SYNC_DURATION_MS = 65000;

    bool          probe_;
    bool          burstOn_;
    bool          burstOff_;
    unsigned int  samples_;
};

// hunger_probe (protocol 29 phase 0, probe tier; hungerSync forced OFF, the
// rest of the medical snapshot streaming as usual) / hunger_sync
// (probe=false, full tier; hungerSync ON). Hunger is a per-client local
// simulation: each engine decays EVERY character's hunger locally and eating
// happens only on the owner's client, so a driven copy starves in the peer's
// view (stat penalties, eventual hunger KO). The probe's DESIGN questions:
//   * what scale does MedicalSystem::hunger use here, and do two clients
//     decay the same body's copies at the same rate (the census answers by
//     comparing series for the same hand)?
//   * does a direct hunger write STICK (or does medicalUpdate clamp/reset)?
//   * with hungerSync off, does the sentinel stay local (the gap)?
//   * what does dazedOrAlert hold at rest (drunk/drug-evidence for the
//     deferred status-effect half)?
// Script per side: 1 Hz census of the WHOLE squad (own + driven tabs) logging
// hunger/fed/dazed per hand; host t=15s / join t=22s writes a SENTINEL
// hunger (own-tab leader, current * 0.6 - proportional, so no scale
// assumption); 50s duration. Both tiers gate the local legs (census ran +
// sentinel wrote and stuck); crossing is the sync oracle's job.
class HungerProbeScenario : public TimedScenario {
public:
    explicit HungerProbeScenario(bool probe)
        : TimedScenario(probe ? "hunger_probe" : "hunger_sync", /*evidenceMs=*/1000),
          probe_(probe), censusLogged_(0),
          haveOwn_(false), wrote_(false), writeOk_(false) {
        memset(ownHand_, 0, sizeof(ownHand_));
    }

    virtual void onStart(const ScenarioContext& ctx) {
        char b[96];
        _snprintf(b, sizeof(b) - 1, "SCENARIO HUNGERPROBE start host=%d",
                  ctx.isHost ? 1 : 0);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        if (!haveOwn_) latchOwn(ctx);
        if (evidenceDue(ctx.elapsedMs)) {
            logCensus(ctx);
        }
        unsigned long writeAt = ctx.isHost ? HOST_WRITE_AT_MS : JOIN_WRITE_AT_MS;
        if (!wrote_ && haveOwn_ && ctx.elapsedMs >= writeAt) {
            wrote_ = true;
            doSentinel(ctx);
        }
        if (ctx.elapsedMs >= DURATION_MS) {
            passed_ = (censusLogged_ > 0) && wrote_ && writeOk_;
            return true;
        }
        return false;
    }

private:
    void latchOwn(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        int idx = tabLeaderIdx(sq, n, ctx.isHost ? 0u : 1u);
        if (idx < 0) return;
        handFromEntity(sq[idx], ownHand_);
        haveOwn_ = true;
        char b[128];
        _snprintf(b, sizeof(b) - 1, "SCENARIO HUNGEROWN who=%s hand=%u.%u.%u.%u.%u",
                  ctx.isHost ? "host" : "join",
                  ownHand_[0], ownHand_[1], ownHand_[2], ownHand_[3], ownHand_[4]);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    void logCensus(const ScenarioContext& ctx) {
        EntityState sq[MAX_SQUAD];
        unsigned int n = engine::captureSquad(ctx.gw, false, sq, MAX_SQUAD);
        for (unsigned int i = 0; i < n && i < MAX_LOG_ROWS; ++i) {
            unsigned int h[5];
            handFromEntity(sq[i], h);
            engine::MedicalRead mr;
            if (!engine::readMedicalByHand(h, &mr)) continue;
            char b[224];
            _snprintf(b, sizeof(b) - 1,
                      "SCENARIO HUNGER hand=%u.%u.%u.%u.%u hunger=%.3f fed=%.3f "
                      "dazed=%.3f t=%lu",
                      h[0], h[1], h[2], h[3], h[4],
                      mr.hunger, mr.fed, mr.dazed, ctx.elapsedMs);
            b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        }
        if (n > 0) ++censusLogged_;
    }

    void doSentinel(const ScenarioContext& ctx) {
        engine::MedicalRead before;
        float bh = -1.0f, want = -1.0f, after = -1.0f;
        int ok = 0;
        if (engine::readMedicalByHand(ownHand_, &before) && before.hunger >= 0.0f) {
            bh = before.hunger;
            // Proportional sentinel: a distinctive ~40% drop without assuming
            // the engine's hunger scale (a probe question).
            want = bh * 0.6f;
            if (engine::writeHungerByHand(ownHand_, want, /*fed*/-1.0f)) {
                engine::MedicalRead post;
                if (engine::readMedicalByHand(ownHand_, &post)) {
                    after = post.hunger;
                    float d = after - want;
                    ok = (d < 1.0f && d > -1.0f) ? 1 : 0; // stuck within noise
                }
            }
        }
        writeOk_ = (ok == 1);
        char b[192];
        _snprintf(b, sizeof(b) - 1,
                  "SCENARIO HUNGERWRITE who=%s hand=%u.%u.%u.%u.%u before=%.3f "
                  "write=%.3f after=%.3f ok=%d t=%lu",
                  ctx.isHost ? "host" : "join",
                  ownHand_[0], ownHand_[1], ownHand_[2], ownHand_[3], ownHand_[4],
                  bh, want, after, ok, ctx.elapsedMs);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    }

    static const unsigned long HOST_WRITE_AT_MS = 15000;
    static const unsigned long JOIN_WRITE_AT_MS = 22000;
    static const unsigned long DURATION_MS      = 50000;
    static const unsigned int  MAX_SQUAD        = 16;
    static const unsigned int  MAX_LOG_ROWS     = 8;

    bool          probe_;
    unsigned int  censusLogged_;
    bool          haveOwn_;
    bool          wrote_;
    bool          writeOk_;
    unsigned int  ownHand_[5];
};

} // namespace

Scenario* makeProbeScenario(const std::string& name) {
    if (name == "spike")        return new SpikeScenario();
    if (name == "shop_probe")   return new ShopProbeScenario(/*probe=*/true);
    if (name == "money_sync")   return new ShopProbeScenario(/*probe=*/false);
    if (name == "vendor_trade") return new VendorTradeScenario();
    if (name == "recruit_probe") return new RecruitProbeScenario(true);
    if (name == "recruit_sync")  return new RecruitProbeScenario(false);
    if (name == "recruit_ctl")   return new RecruitCtlScenario();
    if (name == "squad_probe")   return new SquadProbeScenario(true);
    if (name == "squad_sync")    return new SquadProbeScenario(false);
    if (name == "faction_probe") return new FactionProbeScenario(true);
    if (name == "faction_sync")  return new FactionProbeScenario(false);
    if (name == "time_probe")    return new TimeProbeScenario(true);
    if (name == "time_sync")     return new TimeProbeScenario(false);
    if (name == "hunger_probe")  return new HungerProbeScenario(true);
    if (name == "hunger_sync")   return new HungerProbeScenario(false);
    return 0;
}

} // namespace coop
