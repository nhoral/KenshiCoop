#define _CRT_SECURE_NO_WARNINGS 1 // getenv/atoi are fine; silence VC10 C4996

#include "Config.h"
#include "OwnRanks.h"
#include <cstdlib>
#include <cstdio>
#include <map>
#include <fstream>
#include <iterator>
#include <windows.h>

namespace coop {
namespace {

std::string envOr(const char* key, const char* def) {
    const char* v = std::getenv(key);
    return std::string(v ? v : def);
}

// ---- coop_config.json (flat, tolerant) --------------------------------------
// The interactive install configures the friend code (steamPeer) + connection
// defaults in coop_config.json next to the DLL. This is a deliberately small
// parser: flat "key": value pairs only (string / number / bool), // line
// comments stripped. Values feed loadConfig as the DEFAULTS for envOr(), so the
// env-var test harness still wins (env > file > hard-coded).

std::map<std::string, std::string> parseFlatJson(const std::string& text) {
    std::map<std::string, std::string> m;
    // Strip // line comments first (our values never contain "//").
    std::string s;
    size_t i = 0;
    while (i <= text.size()) {
        size_t nl = text.find('\n', i);
        std::string line = text.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        size_t c = line.find("//");
        if (c != std::string::npos) line = line.substr(0, c);
        s += line;
        s += '\n';
        if (nl == std::string::npos) break;
        i = nl + 1;
    }
    // Scan "key" : <value>. Quoted values read to the closing quote; bare values
    // (numbers/bools) read to the next , } or newline.
    size_t p = 0;
    while (true) {
        size_t q1 = s.find('"', p);
        if (q1 == std::string::npos) break;
        size_t q2 = s.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string key = s.substr(q1 + 1, q2 - q1 - 1);
        size_t colon = s.find(':', q2 + 1);
        if (colon == std::string::npos) break;
        size_t v = colon + 1;
        while (v < s.size() && (s[v] == ' ' || s[v] == '\t' || s[v] == '\r' || s[v] == '\n')) ++v;
        std::string val;
        if (v < s.size() && s[v] == '"') {
            size_t ve = s.find('"', v + 1);
            if (ve == std::string::npos) break;
            val = s.substr(v + 1, ve - v - 1);
            p = ve + 1;
        } else {
            size_t ve = v;
            while (ve < s.size() && s[ve] != ',' && s[ve] != '}' && s[ve] != '\n' && s[ve] != '\r') ++ve;
            val = s.substr(v, ve - v);
            while (!val.empty() && (val[val.size() - 1] == ' ' || val[val.size() - 1] == '\t')) val.erase(val.size() - 1);
            p = ve;
        }
        m[key] = val;
    }
    return m;
}

// Absolute path to coop_config.json next to KenshiCoop.dll (fallback: cwd).
std::string configFilePath() {
    char buf[MAX_PATH];
    HMODULE h = GetModuleHandleA("KenshiCoop.dll");
    DWORD n = GetModuleFileNameA(h, buf, MAX_PATH); // h == 0 would give the exe path
    if (h == 0 || n == 0 || n >= MAX_PATH) return "coop_config.json";
    std::string p(buf, n);
    size_t slash = p.find_last_of("\\/");
    p = (slash != std::string::npos) ? p.substr(0, slash + 1) : std::string();
    return p + "coop_config.json";
}

std::map<std::string, std::string> readConfigFile() {
    std::map<std::string, std::string> m;
    std::ifstream f(configFilePath().c_str(), std::ios::binary);
    if (!f) return m;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parseFlatJson(text);
}

// File value for 'key' if present + non-empty, else 'def'. Used as the default
// argument to envOr() so precedence is env > file > hard-coded.
std::string fileOr(const std::map<std::string, std::string>& f, const char* key, const char* def) {
    std::map<std::string, std::string>::const_iterator it = f.find(key);
    if (it != f.end() && !it->second.empty()) return it->second;
    return std::string(def);
}

} // namespace

void loadConfig(Config& c) {
    // coop_config.json (next to the DLL) supplies the interactive-install
    // defaults; env vars still override every one of them (test harness).
    std::map<std::string, std::string> f = readConfigFile();

    std::string mode = envOr("KENSHICOOP_MODE", fileOr(f, "role", "host").c_str());
    c.isHost      = (mode != "join");
    c.ip          = envOr("KENSHICOOP_IP", fileOr(f, "ip", "127.0.0.1").c_str());
    c.port        = std::atoi(envOr("KENSHICOOP_PORT", fileOr(f, "port", "27800").c_str()).c_str());
    c.save        = envOr("KENSHICOOP_SAVE", "");
    c.testSeconds = std::atoi(envOr("KENSHICOOP_TEST_SECONDS", "0").c_str());

    std::string defLog = c.isHost ? "KenshiCoop_host.log" : "KenshiCoop_join.log";
    c.logPath  = envOr("KENSHICOOP_LOG", defLog.c_str());
    c.scenario = envOr("KENSHICOOP_SCENARIO", "");

    int d = std::atoi(envOr("KENSHICOOP_AUTOLOAD_DELAY_MS", "5000").c_str());
    c.autoLoadDelayMs = (d > 0) ? (unsigned long)d : 5000ul;

    c.setupScene = envOr("KENSHICOOP_SETUP", "");
    c.bakeSave   = envOr("KENSHICOOP_BAKESAVE", "");
    c.probeRecruit = envOr("KENSHICOOP_PROBE_RECRUIT", "") == "1";
    // AI-suspend is the DEFAULT quieting layer for the join's driven world NPCs
    // (review 2026-07-05). KENSHICOOP_AI_SUSPEND=0 is the escape hatch; the legacy
    // probe env still forces it on so old harness call sites keep working.
    c.aiSuspend = (envOr("KENSHICOOP_AI_SUSPEND", "1") != "0") ||
                  (envOr("KENSHICOOP_PROBE_AISUSPEND", "") == "1");
    c.noDetach  = envOr("KENSHICOOP_NO_DETACH", "") == "1";
    c.damageGuard = envOr("KENSHICOOP_DAMAGE_GUARD", "1") != "0";
    // Divergence-gated authority promoted to DEFAULT ON (step-4 A/B, 2026-07-05:
    // trusted-set engaged in 4/4 runs - grants 5-8, trusted ~5 of 12 driven - with
    // npc_track/pose gates at parity; the single red run was a host crash that
    // retry-passed). "0" is the escape hatch.
    c.gateAuthority = envOr("KENSHICOOP_GATE_AUTHORITY", "1") != "0";

    // Inventory sync (Phase 4a). Env semantics: "1" = force on, "0" = force off
    // (escape hatch), unset = ON for REAL sessions (scenario == "" - the 2026-07-07
    // remote session played with it off and equipment changes never crossed) and the
    // manual inventory setup scene. Scripted test scenarios that need it (the inv_*,
    // trade_*, world_*_drop, vendor/store/weapon_loot family) now force it ON via
    // their manifest DiagEnv (KENSHICOOP_INV_SYNC=1) instead of being named here.
    {
        std::string env = envOr("KENSHICOOP_INV_SYNC", "");
        bool auto_ = (c.scenario == "") || (c.setupScene == "inventory");
        c.invSync = (env == "1") || (env != "0" && auto_);
    }

    // Cross-owner transfer intents (protocol 37). Env semantics: "1" = force on,
    // "0" = force off (escape hatch), unset = ON whenever invSync is on - the drag
    // dupe/wipe/weapon-vanish is a real-session bug, so the fix defaults on with the
    // channel it repairs. The trade_probe diagnostic (which baselines the UNFIXED
    // signatures) forces it OFF via its manifest DiagEnv (KENSHICOOP_XFER_SYNC=0).
    {
        std::string env = envOr("KENSHICOOP_XFER_SYNC", "");
        c.xferSync = (env == "1") || (env != "0" && c.invSync);
    }

    // Cross-owner trade veto (OPT-IN). Blocks direct squad-to-squad drags and
    // forces ground drops. Superseded as the DEFAULT by allowing direct trade via
    // Protocol 37 (xferSync): the tryAddItem veto could not reliably classify every
    // engine drag path (the portrait-drag routes the item through the cursor, whose
    // owner is not a squad character - see the 2026-07-13 session), so a blocked
    // drag leaked as a phantom ground drop. Protocol 37 is post-hoc (diffs container
    // totals) and therefore path-agnostic, so REAL sessions now ALLOW + replicate.
    // Env semantics: "1" = force the veto on; unset/"0" = off. The dedicated
    // xfer_block scenario arms it via its manifest DiagEnv (KENSHICOOP_BLOCK_XFER=1).
    {
        std::string env = envOr("KENSHICOOP_BLOCK_XFER", "");
        c.blockXfer = (env == "1");
        if (c.blockXfer) c.xferSync = false; // veto supersedes replicate
    }

    // World-item sync (Phase W1/W2). Env semantics: "1" = force on, "0" = force off
    // (escape hatch), unset = ON for REAL sessions (scenario == "" - dropped gear was
    // invisible cross-client in the 2026-07-07 remote session with it off). The
    // scripted world scenarios that need it (world_item_*, world_*_drop, limb_loss)
    // force it ON via their manifest DiagEnv (KENSHICOOP_WORLD_SYNC=1).
    {
        std::string env = envOr("KENSHICOOP_WORLD_SYNC", "");
        bool auto_ = (c.scenario == ""); // free play / real co-op session
        c.worldSync = (env == "1") || (env != "0" && auto_);
    }

    // Medical sync (phase 2 of the player-combat/medical plan): DEFAULT ON -
    // owner-authoritative vitals for player-squad members + treatment forwarding.
    // Without it, spikes 21-23's truth holds: driven copies' vitals diverge
    // forever and cross-player first aid is lost. "0" is the A/B escape hatch.
    c.medSync = envOr("KENSHICOOP_MED_SYNC", "1") != "0";

    // Consensus game-speed sync: DEFAULT ON - requests min-arbitrated by the
    // host, combat caps fast-forward at 1x. "0" is the A/B escape hatch. The
    // speed_probe spike (which drives the quiet/loud writers directly) and
    // time_probe force it OFF via their manifest DiagEnv (KENSHICOOP_SPEED_SYNC=0).
    c.speedSync = envOr("KENSHICOOP_SPEED_SYNC", "1") != "0";

    // Character stats sync (protocol 17): DEFAULT ON - owner-authoritative
    // CharStats stream for player-squad members. Without it a driven copy
    // keeps save-load stats all session, and the peer's engine resolves REAL
    // fights with those stale numbers. "0" is the A/B escape hatch.
    c.statsSync = envOr("KENSHICOOP_STATS_SYNC", "1") != "0";

    // Carried-body sync (protocol 18): DEFAULT ON - reliable pickup/drop
    // edges + self-healing carried state for player-squad members. Without
    // it the peer's down-enforcement drags/teleports a carried KO'd body
    // along the ground behind its carrier. "0" is the A/B escape hatch.
    c.carrySync = envOr("KENSHICOOP_CARRY_SYNC", "1") != "0";

    // Furniture occupancy sync (protocol 19, DEFAULT ON): reliable enter/exit
    // edges + self-healing BODY_IN_BED/BODY_IN_CAGE state, executed engine-
    // native (setBedMode/setPrisonMode) between each machine's local pair.
    // "0" is the A/B escape hatch.
    c.furnSync = envOr("KENSHICOOP_FURN_SYNC", "1") != "0";
    // Chained/pole prisoner sync (protocol 41, DEFAULT ON): rides the furniture
    // pipeline as kind=3 (Character::isChained -> setChainedMode). "0" disables
    // just the chain kind (beds/cages keep working).
    c.chainSync = envOr("KENSHICOOP_CHAIN_SYNC", "1") != "0";
    c.stealthSync = envOr("KENSHICOOP_STEALTH_SYNC", "1") != "0";
    c.moneySync   = envOr("KENSHICOOP_MONEY_SYNC", "1") != "0";
    c.spawnSync   = envOr("KENSHICOOP_SPAWN_SYNC", "1") != "0";
    c.recruitSync = envOr("KENSHICOOP_RECRUIT_SYNC", "1") != "0";
    c.factionSync = envOr("KENSHICOOP_FACTION_SYNC", "1") != "0";
    c.timeSync    = envOr("KENSHICOOP_TIME_SYNC", "1") != "0";
    c.doorSync    = envOr("KENSHICOOP_DOOR_SYNC", "1") != "0";
    c.buildSync   = envOr("KENSHICOOP_BUILD_SYNC", "1") != "0";
    c.bdoorSync   = envOr("KENSHICOOP_BDOOR_SYNC", "1") != "0";
    c.hungerSync  = envOr("KENSHICOOP_HUNGER_SYNC", "1") != "0";
    c.saveSync    = envOr("KENSHICOOP_SAVE_SYNC", "1") != "0";
    c.loadSync    = envOr("KENSHICOOP_LOAD_SYNC", "1") != "0";
    c.prodSync    = envOr("KENSHICOOP_PROD_SYNC", "1") != "0";
    c.researchSync = envOr("KENSHICOOP_RESEARCH_SYNC", "1") != "0";
    c.storeSync   = envOr("KENSHICOOP_STORE_SYNC", "1") != "0";
    c.squadSync   = envOr("KENSHICOOP_SQUAD_SYNC", "1") != "0";
    c.latejoinSync = envOr("KENSHICOOP_LATEJOIN_SYNC", "1") != "0";
    // NOTE: every channel above DEFAULTS ON for real sessions; the diagnostic
    // "*_probe" scenarios (and time_probe/speed_sync) that need a channel OFF to
    // measure the unsynced baseline force it via their manifest DiagEnv (e.g.
    // shop_probe -> KENSHICOOP_MONEY_SYNC=0). This keeps scenario NAMES out of the
    // shipped plugin - the manifest (scripts/scenarios.psd1) is the single source
    // of truth for per-scenario channel A/B; see Contract.Tests's DiagEnv guard.

    // Transport: "udp" (default) keeps the stock ENet/UDP path (the whole local
    // harness runs on it); "steam" tunnels ENet over Steam P2P by SteamID (no
    // port forwarding / CGNAT-immune). steamPeer is the OTHER player's steamid64
    // (two-code exchange); steamPing arms the channel-1 reachability spike.
    c.transport = envOr("KENSHICOOP_TRANSPORT", fileOr(f, "transport", "udp").c_str());
    c.steamPeer = (unsigned long long)_strtoui64(
        envOr("KENSHICOOP_STEAM_PEER", fileOr(f, "steamPeer", "0").c_str()).c_str(), 0, 10);
    c.steamPing = (unsigned long long)_strtoui64(envOr("KENSHICOOP_STEAM_PING", "0").c_str(), 0, 10);

    // In-game panel session control: opt-in legacy auto-start. Default OFF so a
    // panel-driven (env-free) install defers the session to the Connect button;
    // the test harness overrides this in Plugin.cpp (scenario / test-seconds).
    // Accepts "1"/"true" from the file's JSON bool.
    {
        std::string ac = envOr("KENSHICOOP_AUTOCONNECT", fileOr(f, "autoConnect", "0").c_str());
        c.autoConnect = (ac == "1" || ac == "true");
    }

    // Protocol 36 movement-smoothness knobs. Defaults are the historical
    // constants; any positive env value overrides for live A/B tuning.
    {
        c.sendStamp = envOr("KENSHICOOP_SEND_STAMP", "1") != "0";
        int v;
        v = std::atoi(envOr("KENSHICOOP_INTERP_MIN_DELAY_MS", "0").c_str());
        c.interpMinDelayMs = (v > 0) ? (unsigned int)v : 50u;
        v = std::atoi(envOr("KENSHICOOP_INTERP_MAX_DELAY_MS", "0").c_str());
        c.interpMaxDelayMs = (v > 0) ? (unsigned int)v : 200u;
        v = std::atoi(envOr("KENSHICOOP_INTERP_MAX_EXTRAP_MS", "0").c_str());
        c.interpMaxExtrapMs = (v > 0) ? (unsigned int)v : 250u;
        v = std::atoi(envOr("KENSHICOOP_INTERP_STALE_MS", "0").c_str());
        c.interpStaleMs = (v > 0) ? (unsigned int)v : 2000u;
        double f;
        f = std::atof(envOr("KENSHICOOP_INTERP_SNAP_DIST", "0").c_str());
        c.interpSnapDist = (f > 0.0) ? (float)f : 50.0f;
        f = std::atof(envOr("KENSHICOOP_CATCHUP_K", "0").c_str());
        c.catchupK = (f > 0.0) ? (float)f : 2.0f;
        f = std::atof(envOr("KENSHICOOP_SNAP_DIST", "0").c_str());
        c.snapDist = (f > 0.0) ? (float)f : 8.0f;
        f = std::atof(envOr("KENSHICOOP_SNAP_SECONDS", "0").c_str());
        c.snapSeconds = (f > 0.0) ? (float)f : 0.75f;
        // Combat convergence bands (0 = keep the drive's ReplicatorUtil default).
        c.combatSoftDist    = (float)std::atof(envOr("KENSHICOOP_COMBAT_SOFT_DIST", "0").c_str());
        c.combatSnapDist    = (float)std::atof(envOr("KENSHICOOP_COMBAT_SNAP_DIST", "0").c_str());
        c.combatBigSnapDist = (float)std::atof(envOr("KENSHICOOP_COMBAT_BIG_SNAP_DIST", "0").c_str());
        c.combatSlideMax    = (float)std::atof(envOr("KENSHICOOP_COMBAT_SLIDE_MAX", "0").c_str());
        c.combatConvergeMs  = (unsigned int)std::atoi(envOr("KENSHICOOP_COMBAT_CONVERGE_MS", "0").c_str());
        // Census radius: "0" (explicit) disables; absent = 2000 u default.
        std::string cr = envOr("KENSHICOOP_CENSUS_RADIUS", "");
        c.censusRadius = cr.empty() ? 2000.0f : (float)std::atof(cr.c_str());
        if (c.censusRadius < 0.0f) c.censusRadius = 0.0f;
        // Census-mint radius: "0" (explicit) disables; absent = 600 u default.
        std::string mr = envOr("KENSHICOOP_SPAWN_MINT_RADIUS", "");
        c.spawnMintRadius = mr.empty() ? 600.0f : (float)std::atof(mr.c_str());
        if (c.spawnMintRadius < 0.0f) c.spawnMintRadius = 0.0f;
        // Census park distance: "0" (explicit) disables; absent = 120 u
        // default. Deliberately ABOVE town-schedule divergence (~50 u for a
        // bar NPC seated at a different stool per sim - run 185524 showed
        // parking those fights the seat AI every frame); a genuinely
        // divergent wanderer (the pack-hidden class) measures 500-900 u.
        std::string cp = envOr("KENSHICOOP_CENSUS_PARK", "");
        c.censusParkDist = cp.empty() ? 120.0f : (float)std::atof(cp.c_str());
        if (c.censusParkDist < 0.0f) c.censusParkDist = 0.0f;
        // Census-band AI freeze: quiesce a diverging census-band body's local
        // AI so it can't flee/aggro the join's guards. DEFAULT ON; the A/B
        // escape hatch restores the position-park-only behavior.
        c.censusFreezeAi = envOr("KENSHICOOP_CENSUS_FREEZE_AI", "1") != "0";
        // Camera-anchored interest (protocol 43): fold the local camera +
        // peer camera hint into the interest anchors. DEFAULT ON; the A/B
        // escape hatch restores tab-leader-only anchors.
        c.camInterest = envOr("KENSHICOOP_CAM_INTEREST", "1") != "0";
        // Task-selection observation spike: OFF by default (diagnostic only).
        c.taskSelectSpike = envOr("KENSHICOOP_TASK_SPIKE", "0") != "0";
        // Jail put-to-work desync spike: OFF by default (diagnostic only).
        c.jailProbe = envOr("KENSHICOOP_JAIL_PROBE", "0") != "0";
        // Jail put-to-work observation spike (Phase A): OFF by default.
        c.jailObserve = envOr("KENSHICOOP_JAIL_OBSERVE", "0") != "0";
        // Starve hold: "0" (explicit) restores legacy release-on-stale;
        // absent = 10 s guard-hold default.
        std::string sh = envOr("KENSHICOOP_STARVE_HOLD_MS", "");
        int shv = sh.empty() ? 10000 : std::atoi(sh.c_str());
        c.starveHoldMs = (shv > 0) ? (unsigned int)shv : 0u;
    }

    int delay  = std::atoi(envOr("KENSHICOOP_NETSIM_DELAY_MS", "0").c_str());
    int jitter = std::atoi(envOr("KENSHICOOP_NETSIM_JITTER_MS", "0").c_str());
    int loss   = std::atoi(envOr("KENSHICOOP_NETSIM_LOSS_PCT", "0").c_str());
    c.netSimDelayMs  = (delay  > 0) ? (unsigned int)delay  : 0u;
    c.netSimJitterMs = (jitter > 0) ? (unsigned int)jitter : 0u;
    c.netSimLossPct  = (loss   > 0) ? (unsigned int)(loss > 100 ? 100 : loss) : 0u;

    c.fakeClockSkewMs = (long)std::atoi(envOr("KENSHICOOP_FAKE_CLOCK_SKEW_MS", "0").c_str());

    int armTimeout = std::atoi(envOr("KENSHICOOP_ARM_TIMEOUT_MS", "45000").c_str());
    c.scenarioArmTimeoutMs = (armTimeout > 0) ? (unsigned long)armTimeout : 0ul;

    // Ownership squad-tab ranks: parse a CSV of unsigned ints (e.g. "0", "1", "1,2").
    // KENSHICOOP_OWN_SQUAD is primary; KENSHICOOP_OWN_RANK is an accepted alias. Empty
    // env -> default (host owns tab {0} / join owns tab {1}).
    c.ownRanks.clear();
    {
        std::string ranks = envOr("KENSHICOOP_OWN_SQUAD", envOr("KENSHICOOP_OWN_RANK", "").c_str());
        c.ownRanksFromEnv = parseRankList(ranks, c.ownRanks);
        resolveOwnRanks(c.ownRanks, c.isHost, c.ownRanksFromEnv);
    }
}

std::string describeConfig(const Config& c) {
    // Compact "on/off" channel roster + the knobs most likely to explain a
    // divergence. Kept on one line so it is greppable in the log.
    std::string s = "KenshiCoop: effective cfg";
    s += " scenario='" + c.scenario + "'";
    if (!c.setupScene.empty()) s += " setup='" + c.setupScene + "'";
    s += " transport=" + c.transport;
    struct Flag { const char* name; bool on; };
    const Flag flags[] = {
        { "inv",     c.invSync },      { "xfer",    c.xferSync },
        { "blockXfer", c.blockXfer },  { "world",   c.worldSync },
        { "med",     c.medSync },      { "speed",   c.speedSync },
        { "stats",   c.statsSync },    { "carry",   c.carrySync },
        { "furn",    c.furnSync },     { "chain",   c.chainSync },
        { "stealth", c.stealthSync },  { "money",   c.moneySync },
        { "spawn",   c.spawnSync },    { "recruit", c.recruitSync },
        { "faction", c.factionSync },  { "time",    c.timeSync },
        { "door",    c.doorSync },     { "build",   c.buildSync },
        { "bdoor",   c.bdoorSync },    { "hunger",  c.hungerSync },
        { "save",    c.saveSync },     { "load",    c.loadSync },
        { "prod",    c.prodSync },     { "research",c.researchSync },
        { "store",   c.storeSync },    { "squad",   c.squadSync },
        { "latejoin",c.latejoinSync }, { "aiSuspend", c.aiSuspend },
        { "gateAuth",c.gateAuthority },{ "camInterest", c.camInterest },
        { "censusFreezeAi", c.censusFreezeAi },
    };
    s += " on=[";
    bool first = true;
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        if (!flags[i].on) continue;
        if (!first) s += ",";
        s += flags[i].name;
        first = false;
    }
    s += "] off=[";
    first = true;
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); ++i) {
        if (flags[i].on) continue;
        if (!first) s += ",";
        s += flags[i].name;
        first = false;
    }
    s += "]";
    char b[160];
    _snprintf(b, sizeof(b) - 1,
              " censusR=%.0f mintR=%.0f park=%.0f snap=%.1f/%.2fs armMs=%lu",
              c.censusRadius, c.spawnMintRadius, c.censusParkDist,
              c.snapDist, c.snapSeconds, c.scenarioArmTimeoutMs);
    b[sizeof(b) - 1] = '\0';
    s += b;
    return s;
}

void reloadPeerFromFile(Config& c) {
    // Re-read only the connection TARGET (friend code + UDP endpoint) from
    // coop_config.json, so editing the file then hitting Connect in the panel
    // takes effect without a game restart. Role/transport come from the panel
    // toggles at Connect time and are left untouched here.
    std::map<std::string, std::string> f = readConfigFile();
    std::map<std::string, std::string>::const_iterator it;
    it = f.find("steamPeer");
    if (it != f.end() && !it->second.empty())
        c.steamPeer = (unsigned long long)_strtoui64(it->second.c_str(), 0, 10);
    it = f.find("ip");
    if (it != f.end() && !it->second.empty()) c.ip = it->second;
    it = f.find("port");
    if (it != f.end() && !it->second.empty()) c.port = std::atoi(it->second.c_str());
}

} // namespace coop
