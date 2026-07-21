// prototest - the asserting unit layer for the KenshiCoop wire protocol.
//
// Runs in milliseconds, before any game launch, as step 0 of every regression
// tier (scripts/regress.ps1). Locks three things:
//   1. The WIRE CONTRACT: exact packed sizes + field offsets of every packet in
//      src/netproto/Wire.h. A padding/reorder slip silently desyncs both
//      clients (they memcpy struct bytes); this catches it at compile-run time.
//   2. The CONTENT HASH (src/netproto/ContentHash.h): the inventory-sync
//      convergence key. Must be deterministic, field-sensitive, and
//      order-independent across entries - cross-client equality of these sums
//      IS the inv oracle's proof.
//   3. The INTERPOLATION BUFFER (src/plugin/sync/Interp.cpp): bracketing,
//      clamping, dead-reckoning cap, staleness, teleport snap.
//
// Zero game dependencies. Exit code = number of failed checks (0 = PASS).
//
// Build: cmd /c scripts\build_prototest.cmd  ->  dist\prototest.exe

#define _CRT_SECURE_NO_WARNINGS 1
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <cstdio>
#include <cstring>

#include "../netproto/Wire.h"
#include "../netproto/ContentHash.h"
#include "../plugin/sync/Interp.h"
#include "../plugin/core/OwnRanks.h"
#include "../plugin/core/SteamId.h"
#include "../plugin/core/WorkPose.h"
#include "../plugin/core/DeathLatch.h"
#include "../plugin/core/Inbound.h" // Phase 0 queue-lifecycle fixes (header-only)
#include "../plugin/game/EngineFaults.h" // Phase 5c: fault throttle (pure inline)
#include "../plugin/game/EngineCaps.h"   // Phase 5d: capability registry (pure inline)
#include "../plugin/game/ToastTimer.h"   // ephemeral connect/disconnect toast clock
#include "../plugin/sync/ChangeGate.h"   // Phase 6: change-gated send/accept policy

#include <set>

using namespace coop;

static int g_failed = 0;
static int g_total  = 0;

#define CHECK(name, cond) do { \
    ++g_total; \
    if (cond) { std::printf("  ok   %s\n", name); } \
    else      { std::printf("  FAIL %s\n", name); ++g_failed; } \
} while (0)

#define CHECK_EQ(name, actual, expected) do { \
    ++g_total; \
    unsigned long long a_ = (unsigned long long)(actual); \
    unsigned long long e_ = (unsigned long long)(expected); \
    if (a_ == e_) { std::printf("  ok   %s (= %llu)\n", name, a_); } \
    else { std::printf("  FAIL %s (actual %llu != expected %llu)\n", name, a_, e_); ++g_failed; } \
} while (0)

// ---- 1. Wire contract: packed sizes ------------------------------------------

static void testSizes() {
    std::printf("== wire struct sizes (the packed contract both clients memcpy) ==\n");
    CHECK_EQ("sizeof(HelloPacket)",             sizeof(HelloPacket),             4);
    CHECK_EQ("sizeof(WelcomePacket)",           sizeof(WelcomePacket),           7);
    CHECK_EQ("sizeof(EventPacket)",             sizeof(EventPacket),             54);
    CHECK_EQ("sizeof(EntityState)",             sizeof(EntityState),             79);
    CHECK_EQ("sizeof(EntityBatchHeader)",       sizeof(EntityBatchHeader),       14); // v35: +sendMs; v44: +epoch
    CHECK_EQ("sizeof(InvItemEntry)",            sizeof(InvItemEntry),            158); // v42: +locked+lockReserved
    CHECK_EQ("sizeof(InvSnapshotHeader)",       sizeof(InvSnapshotHeader),       27); // v33: +keyKind
    CHECK_EQ("sizeof(WorldItemEntry)",          sizeof(WorldItemEntry),          73);
    CHECK_EQ("sizeof(WorldItemSnapshotHeader)", sizeof(WorldItemSnapshotHeader), 6);
    CHECK_EQ("sizeof(WorldItemRemoveHeader)",   sizeof(WorldItemRemoveHeader),   6);
    CHECK_EQ("sizeof(WorldDropPacket)",         sizeof(WorldDropPacket),         191);
    CHECK_EQ("sizeof(WorldPickupPacket)",       sizeof(WorldPickupPacket),       91); // v40: +item identity
    CHECK_EQ("sizeof(InvXferPacket)",           sizeof(InvXferPacket),           201); // v36

    CHECK_EQ("sizeof(MedPartEntry)",            sizeof(MedPartEntry),            19);
    CHECK_EQ("sizeof(MedicalPacket)",           sizeof(MedicalPacket),           467);
    CHECK_EQ("sizeof(TreatmentPacket)",         sizeof(TreatmentPacket),         77);
    CHECK_EQ("sizeof(SpeedPacket)",             sizeof(SpeedPacket),             14);
    CHECK_EQ("sizeof(StatsPacket)",             sizeof(StatsPacket),             194);
    CHECK_EQ("sizeof(StealthPacket)",           sizeof(StealthPacket),           427);
    CHECK_EQ("sizeof(SpawnReqPacket)",          sizeof(SpawnReqPacket),          25);
    CHECK_EQ("sizeof(SpawnInfoPacket)",         sizeof(SpawnInfoPacket),         143);
    CHECK_EQ("sizeof(MoneyPacket)",             sizeof(MoneyPacket),             13);
    CHECK_EQ("sizeof(FactionPacket)",           sizeof(FactionPacket),           61);
    CHECK_EQ("sizeof(TimePacket)",              sizeof(TimePacket),              17);
    CHECK_EQ("sizeof(DoorPacket)",              sizeof(DoorPacket),              31);
    CHECK_EQ("sizeof(BuildPlacePacket)",        sizeof(BuildPlacePacket),        94);
    CHECK_EQ("sizeof(BuildStatePacket)",        sizeof(BuildStatePacket),        34);
    CHECK_EQ("sizeof(BuildDoorPacket)",         sizeof(BuildDoorPacket),         32);
    CHECK_EQ("sizeof(BuildRemovePacket)",       sizeof(BuildRemovePacket),       29);
    CHECK_EQ("sizeof(SaveReqPacket)",           sizeof(SaveReqPacket),           57);
    CHECK_EQ("sizeof(SaveBeginPacket)",         sizeof(SaveBeginPacket),         67);
    CHECK_EQ("sizeof(SaveFileHeader)",          sizeof(SaveFileHeader),          19);
    CHECK_EQ("sizeof(SaveDoneHeader)",          sizeof(SaveDoneHeader),          11);
    CHECK_EQ("sizeof(SaveAckPacket)",           sizeof(SaveAckPacket),           20);
    CHECK_EQ("sizeof(LoadGoPacket)",            sizeof(LoadGoPacket),            61);
    CHECK_EQ("sizeof(LoadReqPacket)",           sizeof(LoadReqPacket),           57);
    CHECK_EQ("sizeof(LoadNackPacket)",          sizeof(LoadNackPacket),          61);
    CHECK_EQ("sizeof(ProdPacket)",              sizeof(ProdPacket),              109);
    CHECK_EQ("sizeof(NpcCensusHeader)",         sizeof(NpcCensusHeader),         7); // v35: census
    CHECK_EQ("sizeof(ResearchPacket)",          sizeof(ResearchPacket),          57); // v37: research
    CHECK_EQ("sizeof(CamHintPacket)",           sizeof(CamHintPacket),           17); // v43: camera hint
    // A full entity batch must fit one ~1400 B datagram (NetLink chunking cap).
    CHECK("entity batch fits datagram",
          sizeof(EntityBatchHeader) + ENTITY_BATCH_MAX * sizeof(EntityState) <= 1428);
    // The Steam sender chunk must fit the 1200 B clamped Steam MTU with room
    // for ENet's per-packet overhead (an oversized UNRELIABLE packet would be
    // sent as RELIABLE fragments - motion-stream stalls; review 2026-07-10).
    CHECK("steam entity batch fits clamped MTU",
          sizeof(EntityBatchHeader) + ENTITY_BATCH_MAX_STEAM * sizeof(EntityState) <= 1150);
    CHECK("steam cap under hard receive bound",
          ENTITY_BATCH_MAX_STEAM <= ENTITY_BATCH_MAX);
    CHECK("world-item batch fits datagram",
          sizeof(WorldItemSnapshotHeader) + WORLD_ITEMS_MAX * sizeof(WorldItemEntry) <= 1400);

    // Carried-body sync (protocol 18): the synthetic carry task must never
    // collide with TASK_NONE, the combat stances, or a real engine task key
    // (small ints), and it must classify as carry but NOT as combat.
    CHECK("TASK_CARRY_BODY != TASK_NONE",         TASK_CARRY_BODY != TASK_NONE);
    CHECK("TASK_CARRY_BODY != TASK_COMBAT_MELEE", TASK_CARRY_BODY != TASK_COMBAT_MELEE);
    CHECK("TASK_CARRY_BODY != TASK_COMBAT_WAIT",  TASK_CARRY_BODY != TASK_COMBAT_WAIT);
    CHECK("TASK_CARRY_BODY above engine keys",    TASK_CARRY_BODY >= 0xFE00u);
    CHECK("taskIsCarry(TASK_CARRY_BODY)",         taskIsCarry(TASK_CARRY_BODY));
    CHECK("!taskIsCombat(TASK_CARRY_BODY)",       !taskIsCombat(TASK_CARRY_BODY));
    CHECK("!taskIsCarry(TASK_COMBAT_MELEE)",      !taskIsCarry(TASK_COMBAT_MELEE));
    // BODY_CARRIED is a distinct bit, EXCLUDED from bodyIsDown (the receiver
    // checks bodyIsCarried FIRST and skips the down path for a carried body).
    CHECK("BODY_CARRIED distinct bit",
          BODY_CARRIED != BODY_DOWN && BODY_CARRIED != BODY_RAGDOLL &&
          BODY_CARRIED != BODY_DEAD && BODY_CARRIED != BODY_CRAWL);
    CHECK("bodyIsDown excludes BODY_CARRIED",     !bodyIsDown(BODY_CARRIED));
    CHECK("bodyIsCarried(BODY_CARRIED)",          bodyIsCarried(BODY_CARRIED));
    CHECK("carried+down still reads down",        bodyIsDown(BODY_CARRIED | BODY_DOWN));
    CHECK("carried+down still reads carried",     bodyIsCarried(BODY_CARRIED | BODY_RAGDOLL));
    CHECK("!bodyIsCarried(BODY_DOWN)",            !bodyIsCarried(BODY_DOWN));
    // The new reliable events must be distinct from the existing set.
    CHECK("EVT_PICKUP_BODY distinct",
          EVT_PICKUP_BODY != EVT_NONE && EVT_PICKUP_BODY != EVT_KNOCKOUT &&
          EVT_PICKUP_BODY != EVT_DEATH && EVT_PICKUP_BODY != EVT_REVIVE &&
          EVT_PICKUP_BODY != EVT_AMPUTATE && EVT_PICKUP_BODY != EVT_CRUSH);
    CHECK("EVT_DROP_BODY distinct",
          EVT_DROP_BODY != EVT_PICKUP_BODY && EVT_DROP_BODY != EVT_NONE &&
          EVT_DROP_BODY != EVT_CRUSH);

    // Furniture occupancy (protocol 19): the new bodyState bits are distinct
    // and EXCLUDED from bodyIsDown (the receiver checks bodyInFurniture FIRST,
    // like the carried carve-out).
    CHECK("BODY_IN_BED distinct bit",
          BODY_IN_BED != BODY_DOWN && BODY_IN_BED != BODY_RAGDOLL &&
          BODY_IN_BED != BODY_DEAD && BODY_IN_BED != BODY_CRAWL &&
          BODY_IN_BED != BODY_CARRIED);
    CHECK("BODY_IN_CAGE distinct bit",
          BODY_IN_CAGE != BODY_IN_BED && BODY_IN_CAGE != BODY_DOWN &&
          BODY_IN_CAGE != BODY_RAGDOLL && BODY_IN_CAGE != BODY_DEAD &&
          BODY_IN_CAGE != BODY_CRAWL && BODY_IN_CAGE != BODY_CARRIED);
    CHECK("bodyIsDown excludes occupancy",   !bodyIsDown(BODY_IN_BED | BODY_IN_CAGE));
    CHECK("bodyInFurniture(BODY_IN_BED)",    bodyInFurniture(BODY_IN_BED));
    CHECK("bodyInFurniture(BODY_IN_CAGE)",   bodyInFurniture(BODY_IN_CAGE));
    CHECK("!bodyInFurniture(down|carried)",  !bodyInFurniture(BODY_DOWN | BODY_CARRIED));
    CHECK("occupant+down still reads down",  bodyIsDown(BODY_IN_CAGE | BODY_DOWN));
    // Chained/pole prisoner (protocol 41): distinct bit, rides the furniture
    // carve-out (bodyInFurniture true) but still reads down when KO'd.
    CHECK("BODY_CHAINED distinct bit",
          BODY_CHAINED != BODY_IN_BED && BODY_CHAINED != BODY_IN_CAGE &&
          BODY_CHAINED != BODY_DOWN && BODY_CHAINED != BODY_RAGDOLL &&
          BODY_CHAINED != BODY_DEAD && BODY_CHAINED != BODY_CRAWL &&
          BODY_CHAINED != BODY_CARRIED && BODY_CHAINED != BODY_SNEAK);
    CHECK("bodyChained(BODY_CHAINED)",       bodyChained(BODY_CHAINED));
    CHECK("bodyInFurniture(BODY_CHAINED)",   bodyInFurniture(BODY_CHAINED));
    CHECK("!bodyChained(down|carried)",      !bodyChained(BODY_DOWN | BODY_CARRIED));
    CHECK("chained+down still reads down",   bodyIsDown(BODY_CHAINED | BODY_DOWN));
    // The new reliable events are distinct from the whole existing set.
    CHECK("EVT_ENTER_FURNITURE distinct",
          EVT_ENTER_FURNITURE != EVT_NONE && EVT_ENTER_FURNITURE != EVT_KNOCKOUT &&
          EVT_ENTER_FURNITURE != EVT_DEATH && EVT_ENTER_FURNITURE != EVT_REVIVE &&
          EVT_ENTER_FURNITURE != EVT_AMPUTATE && EVT_ENTER_FURNITURE != EVT_CRUSH &&
          EVT_ENTER_FURNITURE != EVT_PICKUP_BODY && EVT_ENTER_FURNITURE != EVT_DROP_BODY);
    CHECK("EVT_EXIT_FURNITURE distinct",
          EVT_EXIT_FURNITURE != EVT_ENTER_FURNITURE && EVT_EXIT_FURNITURE != EVT_NONE &&
          EVT_EXIT_FURNITURE != EVT_PICKUP_BODY && EVT_EXIT_FURNITURE != EVT_DROP_BODY);

    // Stealth sync (protocol 20).
    CHECK("BODY_SNEAK distinct bit",
          BODY_SNEAK != BODY_DOWN && BODY_SNEAK != BODY_RAGDOLL &&
          BODY_SNEAK != BODY_DEAD && BODY_SNEAK != BODY_CRAWL &&
          BODY_SNEAK != BODY_CARRIED && BODY_SNEAK != BODY_IN_BED &&
          BODY_SNEAK != BODY_IN_CAGE);
    CHECK("bodyIsDown excludes BODY_SNEAK", !bodyIsDown(BODY_SNEAK));
    CHECK("bodySneaking(BODY_SNEAK)",       bodySneaking(BODY_SNEAK));
    CHECK("!bodySneaking(BODY_CRAWL)",      !bodySneaking(BODY_CRAWL));
    CHECK("sneak+crawl still reads sneak",  bodySneaking((u16)(BODY_SNEAK | BODY_CRAWL)));

    // Recruitment sync (protocol 23): the new reliable event is distinct from
    // the whole existing set (it rides the EventPacket shape unchanged).
    CHECK("EVT_RECRUIT distinct",
          EVT_RECRUIT != EVT_NONE && EVT_RECRUIT != EVT_KNOCKOUT &&
          EVT_RECRUIT != EVT_DEATH && EVT_RECRUIT != EVT_REVIVE &&
          EVT_RECRUIT != EVT_AMPUTATE && EVT_RECRUIT != EVT_CRUSH &&
          EVT_RECRUIT != EVT_PICKUP_BODY && EVT_RECRUIT != EVT_DROP_BODY &&
          EVT_RECRUIT != EVT_ENTER_FURNITURE && EVT_RECRUIT != EVT_EXIT_FURNITURE);

    // Squad management sync (protocol 35, v34): the move re-key event rides
    // the EventPacket shape unchanged; both ends must agree on its id, and
    // the HELLO version gates the mismatch.
    CHECK_EQ("EVT_SQUAD_MOVE id", (int)EVT_SQUAD_MOVE, 11);
    CHECK("EVT_SQUAD_MOVE distinct", EVT_SQUAD_MOVE != EVT_RECRUIT &&
          EVT_SQUAD_MOVE != EVT_NONE && EVT_SQUAD_MOVE != EVT_EXIT_FURNITURE);
    CHECK_EQ("PROTOCOL_VERSION (v44: bulk channel + session epoch)", (int)PROTOCOL_VERSION, 44);
}

// ---- 2. readPacket / packetType round-trips -----------------------------------

// Fill a struct with a deterministic byte pattern (distinct per offset).
template <typename T>
static void fillPattern(T* p, unsigned char seed) {
    unsigned char* b = reinterpret_cast<unsigned char*>(p);
    for (unsigned i = 0; i < sizeof(T); ++i) b[i] = (unsigned char)(seed + i * 7);
}

template <typename T>
static void roundTrip(const char* name, u8 typeTag) {
    T in;
    fillPattern(&in, (unsigned char)(typeTag * 31));
    in.type = typeTag;
    unsigned char buf[512];
    std::memcpy(buf, &in, sizeof(T));

    char label[128];

    T out;
    std::memset(&out, 0, sizeof(T));
    bool okRead = readPacket(buf, (unsigned)sizeof(T), &out);
    std::sprintf(label, "%s round-trip read", name);
    CHECK(label, okRead && std::memcmp(&in, &out, sizeof(T)) == 0);

    std::sprintf(label, "%s packetType tag", name);
    CHECK(label, packetType(buf, (unsigned)sizeof(T)) == typeTag);

    // Truncated by one byte: the reader MUST reject (never a partial fill).
    std::sprintf(label, "%s rejects truncated buffer", name);
    CHECK(label, !readPacket(buf, (unsigned)sizeof(T) - 1, &out));
}

static void testRoundTrips() {
    std::printf("== readPacket round-trips + truncation rejection ==\n");
    roundTrip<HelloPacket>("HelloPacket", (u8)PKT_HELLO);
    roundTrip<WelcomePacket>("WelcomePacket", (u8)PKT_WELCOME);
    roundTrip<EventPacket>("EventPacket", (u8)PKT_EVENT);
    roundTrip<WorldDropPacket>("WorldDropPacket", (u8)PKT_WORLD_DROP);
    roundTrip<WorldPickupPacket>("WorldPickupPacket", (u8)PKT_WORLD_PICKUP);
    roundTrip<InvXferPacket>("InvXferPacket", (u8)PKT_INV_XFER);
    roundTrip<MedicalPacket>("MedicalPacket", (u8)PKT_MEDICAL);
    roundTrip<TreatmentPacket>("TreatmentPacket", (u8)PKT_TREATMENT);
    roundTrip<SpeedPacket>("SpeedPacket(REQ)", (u8)PKT_SPEED_REQ);
    roundTrip<SpeedPacket>("SpeedPacket(SET)", (u8)PKT_SPEED_SET);
    roundTrip<StatsPacket>("StatsPacket", (u8)PKT_STATS);
    roundTrip<MoneyPacket>("MoneyPacket", (u8)PKT_MONEY);
    roundTrip<FactionPacket>("FactionPacket", (u8)PKT_FACTION);
    roundTrip<TimePacket>("TimePacket", (u8)PKT_TIME);
    roundTrip<DoorPacket>("DoorPacket", (u8)PKT_DOOR);
    roundTrip<BuildPlacePacket>("BuildPlacePacket", (u8)PKT_BUILD_PLACE);
    roundTrip<BuildStatePacket>("BuildStatePacket", (u8)PKT_BUILD_STATE);
    roundTrip<BuildDoorPacket>("BuildDoorPacket", (u8)PKT_BUILD_DOOR);
    roundTrip<BuildRemovePacket>("BuildRemovePacket", (u8)PKT_BUILD_REMOVE);
    roundTrip<StealthPacket>("StealthPacket", (u8)PKT_STEALTH);
    roundTrip<SpawnReqPacket>("SpawnReqPacket", (u8)PKT_SPAWN_REQ);
    roundTrip<SpawnInfoPacket>("SpawnInfoPacket", (u8)PKT_SPAWN_INFO);
    roundTrip<SaveReqPacket>("SaveReqPacket", (u8)PKT_SAVE_REQ);
    roundTrip<SaveBeginPacket>("SaveBeginPacket", (u8)PKT_SAVE_BEGIN);
    roundTrip<SaveAckPacket>("SaveAckPacket", (u8)PKT_SAVE_ACK);
    roundTrip<LoadGoPacket>("LoadGoPacket", (u8)PKT_LOAD_GO);
    roundTrip<LoadReqPacket>("LoadReqPacket", (u8)PKT_LOAD_REQ);
    roundTrip<LoadNackPacket>("LoadNackPacket", (u8)PKT_LOAD_NACK);
    roundTrip<ProdPacket>("ProdPacket", (u8)PKT_PROD);
    roundTrip<ResearchPacket>("ResearchPacket", (u8)PKT_RESEARCH);

    CHECK("packetType(null) == 0", packetType(0, 10) == 0);
    unsigned char b0[1] = { 0 };
    CHECK("packetType(len 0) == 0", packetType(b0, 0) == 0);
    CHECK("readPacket(null) rejected", !readPacket<HelloPacket>(0, 4, (HelloPacket*)b0) || true);
}

// ---- 3. Field-offset lock (HELLO version + batch framing) -----------------------

static void testFraming() {
    std::printf("== field offsets + batch framing ==\n");

    // HELLO: [u8 type][u16 version][u8 nameLen] - the version check that rejects
    // mismatched builds depends on this exact layout.
    unsigned char hello[4];
    hello[0] = (unsigned char)PKT_HELLO;
    hello[1] = (unsigned char)(PROTOCOL_VERSION & 0xFF);
    hello[2] = (unsigned char)((PROTOCOL_VERSION >> 8) & 0xFF);
    hello[3] = 0;
    HelloPacket h;
    CHECK("HELLO parses from raw bytes", readPacket(hello, 4, &h));
    CHECK_EQ("HELLO version field offset", h.version, PROTOCOL_VERSION);
    CHECK("HELLO version mismatch detectable", ((u16)(PROTOCOL_VERSION + 1)) != h.version);

    // Entity batch framing: [EntityBatchHeader][EntityState*count], the exact
    // bounds check NetLink applies ("len >= need") must hold for a full batch
    // and reject a batch whose count field overruns the actual payload.
    const unsigned N = 3;
    unsigned char buf[sizeof(EntityBatchHeader) + 3 * sizeof(EntityState)];
    EntityBatchHeader hdr;
    hdr.type = (u8)PKT_ENTITY_BATCH; hdr.ownerId = 42; hdr.sendMs = 123456u;
    hdr.epoch = 7u; hdr.count = (u8)N;
    std::memcpy(buf, &hdr, sizeof(hdr));
    EntityState src[N];
    for (unsigned i = 0; i < N; ++i) {
        fillPattern(&src[i], (unsigned char)(i * 13 + 1));
        std::memcpy(buf + sizeof(hdr) + i * sizeof(EntityState), &src[i], sizeof(EntityState));
    }
    unsigned len = (unsigned)sizeof(buf);
    EntityBatchHeader rh;
    std::memcpy(&rh, buf, sizeof(rh));
    unsigned need = (unsigned)sizeof(EntityBatchHeader) + (unsigned)rh.count * (unsigned)sizeof(EntityState);
    CHECK("entity batch: full payload accepted",
          len >= need && rh.count == N && rh.ownerId == 42 && rh.sendMs == 123456u
          && rh.epoch == 7u);
    bool all = true;
    for (unsigned i = 0; i < N; ++i) {
        EntityState e;
        std::memcpy(&e, buf + sizeof(rh) + i * sizeof(EntityState), sizeof(e));
        if (std::memcmp(&e, &src[i], sizeof(e)) != 0) all = false;
    }
    CHECK("entity batch: entries round-trip", all);
    // Lying count: header claims one more entity than the datagram carries.
    rh.count = (u8)(N + 1);
    need = (unsigned)sizeof(EntityBatchHeader) + (unsigned)rh.count * (unsigned)sizeof(EntityState);
    CHECK("entity batch: overrun count rejected by len>=need", !(len >= need));

    // NPC census framing (protocol 36): [NpcCensusHeader][u32 hand[5] * count],
    // the exact "len >= need" bound NetLink applies plus the NPC_CENSUS_MAX cap.
    {
        const unsigned CN = 4;
        unsigned char cbuf[sizeof(NpcCensusHeader) + CN * 5 * sizeof(u32)];
        NpcCensusHeader ch;
        ch.type = (u8)PKT_NPC_CENSUS; ch.ownerId = 1; ch.count = (u16)CN;
        std::memcpy(cbuf, &ch, sizeof(ch));
        u32 hands[CN * 5];
        for (unsigned i = 0; i < CN * 5; ++i) hands[i] = 1000u + i;
        std::memcpy(cbuf + sizeof(ch), hands, sizeof(hands));
        NpcCensusHeader cr;
        std::memcpy(&cr, cbuf, sizeof(cr));
        unsigned clen  = (unsigned)sizeof(cbuf);
        unsigned cneed = (unsigned)sizeof(NpcCensusHeader) + (unsigned)cr.count * 5 * (unsigned)sizeof(u32);
        CHECK("npc census: full payload accepted",
              clen >= cneed && cr.count == CN && cr.count <= NPC_CENSUS_MAX);
        u32 back[CN * 5];
        std::memcpy(back, cbuf + sizeof(cr), sizeof(back));
        CHECK("npc census: hands round-trip", std::memcmp(back, hands, sizeof(hands)) == 0);
        cr.count = (u16)(CN + 1);
        cneed = (unsigned)sizeof(NpcCensusHeader) + (unsigned)cr.count * 5 * (unsigned)sizeof(u32);
        CHECK("npc census: overrun count rejected by len>=need", !(clen >= cneed));
        CHECK("npc census: cap sane", NPC_CENSUS_MAX >= 256 && NPC_CENSUS_MAX <= 2048);
    }

    // Save-file chunk framing (protocol 31): [SaveFileHeader][path][payload],
    // the exact "len >= need" bound NetLink applies, plus the pathLen/dataLen
    // sanity caps that reject a malformed chunk.
    {
        const char* relPath = "platoon\\Drifters_0.platoon";
        const unsigned pl = (unsigned)std::strlen(relPath);
        const unsigned dl = 100;
        unsigned char sbuf[sizeof(SaveFileHeader) + 64 + 100];
        SaveFileHeader fh;
        fh.type = (u8)PKT_SAVE_FILE; fh.ownerId = 0; fh.xferId = 7;
        fh.fileIdx = 3; fh.pathLen = (u16)pl; fh.offset = 4096; fh.dataLen = (u16)dl;
        std::memcpy(sbuf, &fh, sizeof(fh));
        std::memcpy(sbuf + sizeof(fh), relPath, pl);
        for (unsigned i = 0; i < dl; ++i) sbuf[sizeof(fh) + pl + i] = (unsigned char)i;
        unsigned slen = (unsigned)(sizeof(fh) + pl + dl);

        SaveFileHeader rfh;
        std::memcpy(&rfh, sbuf, sizeof(rfh));
        unsigned sneed = (unsigned)sizeof(SaveFileHeader) + rfh.pathLen + rfh.dataLen;
        CHECK("save chunk: full payload accepted",
              slen >= sneed && rfh.pathLen > 0 && rfh.pathLen <= SAVE_PATH_MAX &&
              rfh.dataLen <= SAVE_CHUNK_MAX);
        CHECK("save chunk: path bytes at header end",
              std::memcmp(sbuf + sizeof(SaveFileHeader), relPath, pl) == 0);
        CHECK("save chunk: payload follows path",
              sbuf[sizeof(SaveFileHeader) + pl + 42] == 42);
        // Lying dataLen: claims more payload than the packet carries.
        rfh.dataLen = (u16)(dl + 1);
        sneed = (unsigned)sizeof(SaveFileHeader) + rfh.pathLen + rfh.dataLen;
        CHECK("save chunk: overrun dataLen rejected by len>=need", !(slen >= sneed));
        // Oversized dataLen: above the chunk cap even if the bytes were there.
        rfh.dataLen = (u16)(SAVE_CHUNK_MAX + 1);
        CHECK("save chunk: dataLen above SAVE_CHUNK_MAX rejected",
              !(rfh.dataLen <= SAVE_CHUNK_MAX));
        // Zero pathLen: a chunk with no relative path is malformed.
        rfh.pathLen = 0;
        CHECK("save chunk: zero pathLen rejected", !(rfh.pathLen > 0));
    }

    // Save-done framing: [SaveDoneHeader][u32 crc * fileCount].
    {
        const unsigned FC = 5;
        unsigned char dbuf[sizeof(SaveDoneHeader) + FC * sizeof(u32)];
        SaveDoneHeader dh;
        dh.type = (u8)PKT_SAVE_DONE; dh.ownerId = 0; dh.xferId = 7; dh.fileCount = FC;
        std::memcpy(dbuf, &dh, sizeof(dh));
        u32 crcs[FC] = { 1, 2, 3, 4, 5 };
        std::memcpy(dbuf + sizeof(dh), crcs, sizeof(crcs));
        unsigned dlen = (unsigned)sizeof(dbuf);
        SaveDoneHeader rdh;
        std::memcpy(&rdh, dbuf, sizeof(rdh));
        unsigned dneed = (unsigned)sizeof(SaveDoneHeader) + rdh.fileCount * (unsigned)sizeof(u32);
        CHECK("save done: full CRC table accepted", dlen >= dneed && rdh.fileCount == FC);
        rdh.fileCount = (u16)(FC + 1);
        dneed = (unsigned)sizeof(SaveDoneHeader) + rdh.fileCount * (unsigned)sizeof(u32);
        CHECK("save done: overrun fileCount rejected by len>=need", !(dlen >= dneed));
    }
}

// ---- 3b. Save-transfer CRC (protocol 31): incremental FNV-1a-32 ------------------
// The receiver folds each arriving chunk into the file's running CRC; the
// sender does the same while reading. Chunk-split invariance IS the
// reassembly correctness proof: however the file is cut into chunks, the
// final CRC equals the whole-file hash the sender put in the DONE table.

static void testSaveCrc() {
    std::printf("== save-transfer CRC (fnv1a incremental) ==\n");
    unsigned char data[10000];
    for (unsigned i = 0; i < sizeof(data); ++i)
        data[i] = (unsigned char)(i * 31 + (i >> 8));

    // One-shot reference.
    unsigned ref = fnv1aUpdate(fnv1aInit(), data, sizeof(data));
    CHECK("crc deterministic", fnv1aUpdate(fnv1aInit(), data, sizeof(data)) == ref);

    // 4 KB chunking (the wire chunk size) folds to the same value.
    unsigned h = fnv1aInit();
    for (unsigned off = 0; off < sizeof(data); off += SAVE_CHUNK_MAX) {
        unsigned n = sizeof(data) - off;
        if (n > SAVE_CHUNK_MAX) n = SAVE_CHUNK_MAX;
        h = fnv1aUpdate(h, data + off, n);
    }
    CHECK("crc chunk-split invariant (4 KB chunks)", h == ref);

    // Pathological 1-byte chunks fold to the same value too.
    h = fnv1aInit();
    for (unsigned i = 0; i < sizeof(data); ++i) h = fnv1aUpdate(h, data + i, 1);
    CHECK("crc chunk-split invariant (1 B chunks)", h == ref);

    // A single flipped byte perturbs the CRC (corruption is caught).
    data[5000] ^= 1;
    CHECK("crc detects a flipped byte", fnv1aUpdate(fnv1aInit(), data, sizeof(data)) != ref);
    data[5000] ^= 1;

    // Empty file: CRC = the FNV offset basis, same on both ends.
    CHECK("crc of empty file = fnv basis", fnv1aInit() == 2166136261u);
}

// ---- 3b. Folder fingerprint (protocol 32 coordinated load) --------------------
// The join compares the host's LOAD_GO fingerprint against its own on-disk
// copy - equality must mean "byte-identical folder" regardless of directory
// enumeration order or path case, and any divergence must perturb it.

static void testFolderFingerprint() {
    std::printf("== folder fingerprint (coordinated load) ==\n");
    const char* paths[4] = { "quick.save", "platoon\\a.platoon",
                             "platoon\\b.platoon", "zone\\zone.1.2.zone" };
    unsigned int crcs[4] = { 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u };
    unsigned ref = folderFingerprintOf(paths, crcs, 4);

    CHECK("fp deterministic", folderFingerprintOf(paths, crcs, 4) == ref);
    CHECK("fp nonzero (0 reserved for missing)", ref != 0);

    // Enumeration-order invariance: FindFirstFile order differs by filesystem;
    // the same (path, crc) SET must fingerprint identically.
    const char* paths2[4] = { paths[2], paths[0], paths[3], paths[1] };
    unsigned int crcs2[4] = { crcs[2], crcs[0], crcs[3], crcs[1] };
    CHECK("fp enumeration-order invariant",
          folderFingerprintOf(paths2, crcs2, 4) == ref);

    // Windows path case-insensitivity: the same folder listed with different
    // case must agree cross-machine.
    const char* paths3[4] = { "QUICK.SAVE", "Platoon\\A.platoon",
                              "platoon\\b.PLATOON", "zone\\ZONE.1.2.zone" };
    CHECK("fp path-case invariant", folderFingerprintOf(paths3, crcs, 4) == ref);

    // Sensitivity: one changed file content, a renamed path, a missing file
    // and an added file must all perturb the value.
    unsigned int crcs4[4] = { crcs[0], crcs[1] ^ 1u, crcs[2], crcs[3] };
    CHECK("fp detects changed file content",
          folderFingerprintOf(paths, crcs4, 4) != ref);
    const char* paths5[4] = { "quick.save", "platoon\\a.platoon",
                              "platoon\\c.platoon", "zone\\zone.1.2.zone" };
    CHECK("fp detects renamed path", folderFingerprintOf(paths5, crcs, 4) != ref);
    CHECK("fp detects missing file", folderFingerprintOf(paths, crcs, 3) != ref);
    const char* paths6[5] = { paths[0], paths[1], paths[2], paths[3], "extra.bin" };
    unsigned int crcs6[5] = { crcs[0], crcs[1], crcs[2], crcs[3], 0x55555555u };
    CHECK("fp detects added file", folderFingerprintOf(paths6, crcs6, 5) != ref);

    // Empty folder = 0 (the "missing/unreadable" sentinel).
    CHECK("fp of empty set = 0", folderFingerprintOf(paths, crcs, 0) == 0);
}

// ---- 4. Content hash (the inventory convergence key) -----------------------------

static InvItemEntry makeEntry() {
    InvItemEntry e;
    std::memset(&e, 0, sizeof(e));
    std::strcpy(e.stringID, "wooden_sandals");
    e.itemType = 7; e.quantity = 2; e.quality = 150;
    e.equipped = 0; e.slot = 0; e.section = 0;
    std::strcpy(e.manufacturer, "");
    std::strcpy(e.material, "");
    return e;
}

static void testContentHash() {
    std::printf("== content hash (ContentHash.h) ==\n");
    InvItemEntry a = makeEntry();
    InvItemEntry b = makeEntry();
    CHECK("hash deterministic (equal entries equal)", invEntryHash(a) == invEntryHash(b));

    // Every field that defines content identity must perturb the hash.
    unsigned base = invEntryHash(a);
    b = makeEntry(); std::strcpy(b.stringID, "wooden_sandalz");
    CHECK("stringID perturbs hash",     invEntryHash(b) != base);
    b = makeEntry(); b.itemType = 8;
    CHECK("itemType perturbs hash",     invEntryHash(b) != base);
    b = makeEntry(); b.quantity = 3;
    CHECK("quantity perturbs hash",     invEntryHash(b) != base);
    b = makeEntry(); b.quality = 151;
    CHECK("quality perturbs hash",      invEntryHash(b) != base);
    b = makeEntry(); b.equipped = 1;
    CHECK("equipped perturbs hash",     invEntryHash(b) != base);
    b = makeEntry(); b.slot = 5;
    CHECK("slot perturbs hash",         invEntryHash(b) != base);
    b = makeEntry(); b.locked = 1;
    CHECK("locked perturbs hash",       invEntryHash(b) != base);
    b = makeEntry(); b.section = 1234;
    CHECK("section perturbs hash",      invEntryHash(b) != base);
    b = makeEntry(); std::strcpy(b.manufacturer, "cross");
    CHECK("manufacturer perturbs hash", invEntryHash(b) != base);
    b = makeEntry(); std::strcpy(b.material, "iron");
    CHECK("material perturbs hash",     invEntryHash(b) != base);

    // Order independence: the container fingerprint is the SUM of entry hashes,
    // so any permutation of the same multiset must produce the same sum.
    InvItemEntry e1 = makeEntry();
    InvItemEntry e2 = makeEntry(); std::strcpy(e2.stringID, "iron_katana"); e2.equipped = 1;
    InvItemEntry e3 = makeEntry(); e3.quantity = 9;
    unsigned s123 = invEntryHash(e1) + invEntryHash(e2) + invEntryHash(e3);
    unsigned s312 = invEntryHash(e3) + invEntryHash(e1) + invEntryHash(e2);
    CHECK("container sum order-independent", s123 == s312);

    // Section-name hash: '' reserved as 0 (loose); non-empty never 0; stable.
    CHECK("sectionNameHash('') == 0",      sectionNameHash("") == 0);
    CHECK("sectionNameHash(null) == 0",    sectionNameHash(0) == 0);
    CHECK("sectionNameHash nonzero",       sectionNameHash("hip") != 0);
    CHECK("sectionNameHash deterministic", sectionNameHash("hip") == sectionNameHash("hip"));
    CHECK("sectionNameHash distinguishes weapon slots", sectionNameHash("hip") != sectionNameHash("back"));

    // Canonical vector: print (not assert) so the baseline doc can record it and
    // a future intentional change is visible in the diff.
    std::printf("  note canonical invEntryHash(wooden_sandals x2 q150) = %u\n", base);
}

// ---- 5. Interpolation buffer invariants -------------------------------------------

static EntityState entAt(float x) {
    EntityState e;
    std::memset(&e, 0, sizeof(e));
    e.hIndex = 1; e.hSerial = 2; e.task = TASK_NONE;
    e.x = x; e.y = 0.0f; e.z = 0.0f; e.heading = 0.0f;
    return e;
}

static void testInterp() {
    std::printf("== interpolation buffer (Interp.cpp) ==\n");
    InterpConfig cfg; // min 50 / max 200 delay, extrap 250, snap 50u, stale 2000

    // Bracketed interpolation: 20 Hz feed moving +1u per 50ms tick.
    {
        EntityInterp it;
        for (int i = 0; i <= 10; ++i) it.push(entAt((float)i), 1000 + i * 50);
        // nowMs=1550 -> renderTime = 1550 - delay(>=50,<=200) = [1350,1500]
        // -> x must interpolate inside [7,10] and never exceed the newest.
        EntityState out;
        bool ok = it.sample(1550, cfg, &out);
        CHECK("bracketed sample returns data", ok);
        CHECK("bracketed sample within segment bounds", ok && out.x >= 6.9f && out.x <= 10.01f);

        // Monotonic advance: successive sample times never move the body backwards.
        float prev = -1.0f; bool mono = true;
        for (unsigned long t = 1400; t <= 1550; t += 10) {
            EntityState o;
            if (it.sample(t, cfg, &o)) { if (o.x < prev - 0.001f) mono = false; prev = o.x; }
        }
        CHECK("sampled position monotonic for monotone source", mono);
    }

    // Dead-reckoning cap: starved buffer extrapolates at most maxExtrapMs beyond
    // the newest snapshot (here: 1u/50ms -> cap = +5u over newest).
    {
        EntityInterp it;
        it.push(entAt(0.0f), 1000);
        it.push(entAt(1.0f), 1050);
        EntityState out;
        bool ok = it.sample(2900, cfg, &out); // renderTime far past newest, still < stale
        CHECK("starved sample still returns (dead-reckon)", ok);
        CHECK("dead-reckoning capped at maxExtrapMs", ok && out.x <= 1.0f + 5.0f + 0.01f);
    }

    // Staleness: a stream older than staleMs releases the body (sample -> false).
    {
        EntityInterp it;
        it.push(entAt(0.0f), 1000);
        EntityState out;
        CHECK("stale stream releases body", !it.sample(1000 + cfg.staleMs + 500, cfg, &out));
    }

    // Teleport snap: a segment step beyond snapDist snaps to the newer end
    // instead of smearing the body across the gap.
    {
        EntityInterp it;
        it.push(entAt(0.0f), 1000);
        it.push(entAt(1000.0f), 1050); // 1000u jump >> 50u snap distance
        it.push(entAt(1001.0f), 1100);
        EntityState out;
        bool ok = it.sample(1120, cfg, &out); // renderTime ~1070 -> inside the jump segment... 
        // renderTime lands in [1000,1050] or [1050,1100] depending on adaptive delay;
        // in the jump segment we must NOT see a smeared mid-point (x in ~[100,900]).
        bool smeared = ok && out.x > 100.0f && out.x < 900.0f;
        CHECK("teleport does not smear", ok && !smeared);
    }

    // Dead-reckon past a teleport: when the LAST segment is a jump (a park /
    // fast-travel) and the render time runs past the newest sample, the buffer
    // must HOLD the newest pose - never dead-reckon ALONG the jump vector, which
    // multiplies the delta by ahead/seg and flings the body thousands of units
    // past its real position (the roaming/fast-travel warp fixed 2026-07-20).
    {
        EntityInterp it;
        it.push(entAt(0.0f), 1000);
        it.push(entAt(1000.0f), 1050); // last segment = 1000u jump >> 50u snap
        EntityState out;
        // nowMs=1350 -> renderTime = 1350 - delay(<=200) >= 1150, past newest
        // (1050) but < staleMs, so we hit the extrapolation branch.
        bool ok = it.sample(1350, cfg, &out);
        CHECK("dead-reckon past teleport returns", ok);
        // Without the guard this overshoots to ~3000u; the guard holds newest.
        CHECK("dead-reckon past teleport holds newest (no overshoot)",
              ok && out.x <= 1000.5f && out.x >= 999.5f);
    }

    // Single snapshot: returns that pose verbatim.
    {
        EntityInterp it;
        it.push(entAt(7.0f), 1000);
        EntityState out;
        bool ok = it.sample(1040, cfg, &out);
        CHECK("single snapshot returns pose", ok && out.x == 7.0f);
    }

    // Identity/locomotion passthrough: sample carries the latest full state.
    {
        EntityInterp it;
        EntityState e = entAt(3.0f);
        e.bodyState = BODY_DOWN; e.cMoving = 1; e.task = 42;
        it.push(e, 1000);
        EntityState out;
        bool ok = it.sample(1030, cfg, &out);
        CHECK("identity+state passthrough", ok && out.bodyState == BODY_DOWN && out.cMoving == 1 && out.task == 42 && out.hIndex == 1);
    }
}

// ---- 6. Ownership rank resolution (OwnRanks.h) ----------------------------------
// Guards the squad-tab ownership partition, especially the F2-panel role switch
// regression (2026-07-14): a session launched as HOST resolves ranks to {0};
// switching to JOIN must re-resolve to {1}, or the client claims the host's
// rank-0 player squad and that unit never moves. An explicit env override is
// preserved across the switch.

static bool ranksAre(const std::set<unsigned int>& r, int a, int b) {
    if (b < 0) return r.size() == 1 && r.count((unsigned)a) == 1;
    return r.size() == 2 && r.count((unsigned)a) == 1 && r.count((unsigned)b) == 1;
}

static void testOwnRanks() {
    std::printf("== ownership rank resolution (OwnRanks.h) ==\n");

    // Role defaults from a clean slate.
    {
        std::set<unsigned int> r;
        resolveOwnRanks(r, true, false);
        CHECK("host default owns {0}", ranksAre(r, 0, -1));
        r.clear();
        resolveOwnRanks(r, false, false);
        CHECK("join default owns {1}", ranksAre(r, 1, -1));
    }

    // THE FIX: a session that started HOST (ranks {0}) switches to JOIN via the
    // panel and MUST end up owning {1}, not the host's {0}.
    {
        std::set<unsigned int> r;
        resolveOwnRanks(r, true, false);          // launched HOST -> {0}
        CHECK("pre-switch ranks are {0}", ranksAre(r, 0, -1));
        resolveOwnRanks(r, false, false);         // panel switch to JOIN
        CHECK("HOST->JOIN switch re-resolves to {1}", ranksAre(r, 1, -1));
        resolveOwnRanks(r, true, false);          // and back to HOST
        CHECK("JOIN->HOST switch re-resolves to {0}", ranksAre(r, 0, -1));
    }

    // An explicit env override is preserved across a role switch (the user asked
    // for a specific partition; the panel must not clobber it).
    {
        std::set<unsigned int> r;
        r.insert(2u); r.insert(3u);
        resolveOwnRanks(r, false, true);          // fromEnv -> untouched
        CHECK("env override preserved as JOIN", ranksAre(r, 2, 3));
        resolveOwnRanks(r, true, true);           // still untouched as HOST
        CHECK("env override preserved as HOST", ranksAre(r, 2, 3));
    }

    // CSV parse (KENSHICOOP_OWN_SQUAD/OWN_RANK surface).
    {
        std::set<unsigned int> r;
        CHECK("parse '' -> no ranks",        !parseRankList("", r) && r.empty());
        r.clear();
        CHECK("parse '0' -> {0}",            parseRankList("0", r) && ranksAre(r, 0, -1));
        r.clear();
        CHECK("parse '1,2' -> {1,2}",        parseRankList("1,2", r) && ranksAre(r, 1, 2));
        r.clear();
        CHECK("parse ' 3 ; 5 ' tolerant",    parseRankList(" 3 ; 5 ", r) && ranksAre(r, 3, 5));
        r.clear();
        CHECK("parse '2,2' dedups to {2}",   parseRankList("2,2", r) && ranksAre(r, 2, -1));
    }

    // Config-style resolution: env-provided ranks set fromEnv true and survive;
    // empty env falls back to the role default.
    {
        std::set<unsigned int> r;
        bool fromEnv = parseRankList("1", r);
        resolveOwnRanks(r, true, fromEnv);        // env said {1} even though HOST
        CHECK("env {1} wins over HOST default", fromEnv && ranksAre(r, 1, -1));
        r.clear();
        fromEnv = parseRankList("", r);
        resolveOwnRanks(r, false, fromEnv);       // no env -> JOIN default {1}
        CHECK("empty env -> JOIN default {1}", !fromEnv && ranksAre(r, 1, -1));
    }
}

// ---- 7. SteamID64 parse (SteamId.h) ---------------------------------------------
// Guards the F2 panel "Paste friend's Steam ID" button: clipboard text is noisy
// (surrounding whitespace, a trailing newline, or a "Steam ID: 7656..." wrapper),
// so parseSteamId64 keeps only digits and requires a 17-digit community ID
// (76561... prefix). Arbitrary clipboard junk must be rejected.

static void testSteamIdParse() {
    std::printf("== SteamID64 parse (SteamId.h) ==\n");
    unsigned long long id = 0;

    id = 0;
    CHECK("clean 17-digit id accepted",
          coop::parseSteamId64("76561198000000000", id) && id == 76561198000000000ull);
    id = 0;
    CHECK("surrounding whitespace/newline stripped",
          coop::parseSteamId64("  76561198012345678 \r\n", id) && id == 76561198012345678ull);
    id = 0;
    CHECK("wrapper text 'Steam ID: <n>' stripped",
          coop::parseSteamId64("Steam ID: 76561198012345678", id) && id == 76561198012345678ull);

    // Rejections leave the caller's value untouched.
    id = 123ull;
    CHECK("empty string rejected",        !coop::parseSteamId64("", id) && id == 123ull);
    CHECK("non-numeric rejected",         !coop::parseSteamId64("not-an-id", id) && id == 123ull);
    CHECK("too short (16 digits) rejected",
          !coop::parseSteamId64("7656119800000000", id) && id == 123ull);
    CHECK("too long (18 digits) rejected",
          !coop::parseSteamId64("765611980000000000", id) && id == 123ull);
    CHECK("17 digits, wrong prefix rejected",
          !coop::parseSteamId64("12345678901234567", id) && id == 123ull);
}

// ---- 8. Pose-fixture acceptance (WorkPose.h) ------------------------------------
// Guards the mining-sync fix (2026-07-14): a player mining an ore node operates a
// mine building. A single 6 m seat gate rejected the CORRECT mine as "far"
// (applyTaskOrder -> park, no mining animation on the peer). Field distances varied
// wildly (one mine ~8.9 m from origin, a larger one 57 m host / 104 m join), so no
// fixed radius covers both. Work fixtures are unique buildings with reliable
// cross-client hands, so they are TRUSTED (ungated); only seats are distance-gated
// (they mis-resolve to a wrong nearby prop).

static void testWorkPoseMatch() {
    std::printf("== pose-fixture acceptance (WorkPose.h) ==\n");

    // Gate applies to seats, never to work fixtures.
    CHECK("seat radius 6 m",            SEAT_MATCH_DIST == 6.0f);
    CHECK("seat is distance-gated",     poseIsDistanceGated(false));
    CHECK("work is NOT distance-gated", !poseIsDistanceGated(true));

    // THE FIX: work fixtures are accepted at ANY resolved distance (the mine origin
    // can sit 8.9 m, 57 m or 104 m from the operate spot), while a seat at those
    // distances is rejected as a mis-resolved wrong prop.
    CHECK("mining 8.9 m accepted as work",  poseFixtureAccepted(true,  8.9f));
    CHECK("mining 57 m accepted as work",   poseFixtureAccepted(true,  57.0f));
    CHECK("mining 104 m accepted as work",  poseFixtureAccepted(true,  104.0f));
    CHECK("mining 8.9 m rejected as seat", !poseFixtureAccepted(false, 8.9f));

    // Medic sync (2026-07-15): a first-aid subject is the PATIENT (a character), also
    // identity-trusted (isWorkFixtureTask || isMedicTask -> the boolean below), so a
    // patient whose driven copy is mid-motion (metres from the streamed transform) is
    // still accepted, exactly like a work fixture; a seat at the same range is not.
    CHECK("medic 12 m accepted (identity-trusted)",  poseFixtureAccepted(true,  12.0f));
    CHECK("medic 12 m rejected as seat",            !poseFixtureAccepted(false, 12.0f));

    // Seat still tight: a fixture right under the body is accepted, a far stool not.
    CHECK("seat 3 m accepted",   poseFixtureAccepted(false, 3.0f));
    CHECK("seat 6 m boundary",   poseFixtureAccepted(false, 6.0f));
    CHECK("seat 6.1 m rejected", !poseFixtureAccepted(false, 6.1f));

    // Squared-distance form (the engine gate) agrees with the metres form.
    CHECK("sq: work 104 m accepted",   poseFixtureAcceptedSq(true,  104.0f * 104.0f));
    CHECK("sq: seat 3 m accepted",     poseFixtureAcceptedSq(false, 3.0f * 3.0f));
    CHECK("sq: seat 6 m boundary",     poseFixtureAcceptedSq(false, 6.0f * 6.0f));
    CHECK("sq: seat 6.1 m rejected",  !poseFixtureAcceptedSq(false, 6.1f * 6.1f));
}

// ---- 9. Debounced task-clear (WorkPose.h poseClearElapsed) ----------------------
// Guards the job-removal fix (2026-07-14): removing a job on the host while the
// character stays STATIONARY streams task=NONE continuously (the movement re-arm
// never fires), so the join must release the held mine/operate pose after a
// sustained-NONE window instead of holding it forever. Transient NONE blips (1-2
// capture frames) must NOT clear a committed pose, so the release is DEBOUNCED.
// clearMs mirrors TASK_CLEAR_MS in ReplicatorUtil.h (game-coupled, so not included
// here); keep the literal in sync with that constant.
static void testTaskClear() {
    std::printf("== debounced task-clear (WorkPose.h) ==\n");
    const unsigned long clearMs = 1200; // mirror of TASK_CLEAR_MS

    // No streak in progress (noneTick == 0) never clears, regardless of 'now'.
    CHECK("no streak never clears",       !poseClearElapsed(0,     999999, clearMs));

    // A transient blip below the window holds (anti-oscillation guarantee).
    CHECK("blip 0 ms holds",              !poseClearElapsed(10000, 10000,  clearMs));
    CHECK("blip 1199 ms holds",           !poseClearElapsed(10000, 11199,  clearMs));

    // Sustained NONE at/after the window releases (genuine stationary un-assign).
    CHECK("streak 1200 ms clears",         poseClearElapsed(10000, 11200,  clearMs));
    CHECK("streak 5 s clears",             poseClearElapsed(10000, 15000,  clearMs));

    // Unsigned tick wrap (GetTickCount rollover): now - noneTick still yields the
    // elapsed delta, so a streak spanning the wrap boundary still clears on time.
    // 'now' values are written pre-wrapped (as GetTickCount would report post-rollover)
    // so the arithmetic under test is the real subtraction, not a constant overflow.
    const unsigned long nearMax   = 0xFFFFFFFFul - 100; // streak started 100 ms before wrap
    const unsigned long stillPre  = nearMax + 50;       // 50 ms later, before wrap (no overflow)
    const unsigned long postWrap  = 1199UL;             // (nearMax + 1300) mod 2^32: 1300 ms later
    CHECK("wrap: 50 ms elapsed holds",    !poseClearElapsed(nearMax, stillPre, clearMs));
    CHECK("wrap: 1300 ms elapsed clears",  poseClearElapsed(nearMax, postWrap, clearMs));
}

// ---- 10. Death/KO latch carry across re-key (DeathLatch.h rekeyCarryLatch) ------
// Guards the death-consistency fix (2026-07-15): a dead/KO'd body that RE-KEYS
// (owner re-containers it - squad move / recruit) must keep its down/death pin,
// or the peer stands the corpse back up under the new hand ("dead on one game,
// alive on the other"). rekeyPeerBody snapshots the OLD key's latch and OR-merges
// it onto the new key; this locks that merge (monotone: never loses a pin, never
// clears a latch already present on the new key).
static void testDeathRekey() {
    std::printf("== death/KO latch carry on re-key (DeathLatch.h) ==\n");

    // Dead old key, fresh new key -> death carries.
    LatchState r1 = rekeyCarryLatch(LatchState(true, true, true), LatchState());
    CHECK("dead old -> new death latched",  r1.death);
    CHECK("dead old -> new ko latched",      r1.ko);
    CHECK("dead old -> new down carried",    r1.down);

    // KO-only old key -> ko carries, death stays clear.
    LatchState r2 = rekeyCarryLatch(LatchState(false, true, true), LatchState());
    CHECK("ko old -> new ko latched",        r2.ko);
    CHECK("ko old -> new death still clear", !r2.death);

    // Alive old key, alive new key -> nothing invented.
    LatchState r3 = rekeyCarryLatch(LatchState(), LatchState());
    CHECK("alive+alive -> no death",         !r3.death);
    CHECK("alive+alive -> no ko",            !r3.ko);

    // New key already has a fresh EVT_DEATH (beat the re-key edge): OR-merge must
    // PRESERVE it even though the old key was alive.
    LatchState r4 = rekeyCarryLatch(LatchState(), LatchState(true, true, false));
    CHECK("alive old + dead new -> death kept", r4.death);
    CHECK("alive old + dead new -> ko kept",    r4.ko);

    // Monotone: merging can only ADD pins, never remove one present on either key.
    LatchState r5 = rekeyCarryLatch(LatchState(true, false, false),
                                    LatchState(false, true, false));
    CHECK("merge keeps old death", r5.death);
    CHECK("merge keeps new ko",    r5.ko);
}

// ---- 11. Inbound queue lifecycle (Inbound.h) ------------------------------------
// Locks the Phase 0 correctness fixes against regression:
//  (a) flushWorldState() drops a queued cross-owner invXfer intent - the bug was
//      that a transfer enqueued before a world reload SURVIVED it (invXfer_ was
//      missing from the clear list), applying against the fresh world.
//  (b) sawRemote_ (peer-readiness) clears on the session-reset edge, so a new
//      scenario cannot arm on a departed peer's stale readiness.
//  (c) the internal session generation advances on every flush (the Phase 0 seed
//      for the Phase 4 wire epoch).
// It also proves the world-state vs session-preserving split: a world-state queue
// is dropped while a coordinated-load queue survives the same flush.
static void testInboundLifecycle() {
    std::printf("== inbound queue lifecycle (Inbound.h) ==\n");
    Inbound in;

    CHECK_EQ("initial session generation", in.sessionGeneration(), 0);
    CHECK("sawRemote false before any entity", !in.sawRemoteEntity());

    EntityState e; std::memset(&e, 0, sizeof(e));
    in.pushEntity(1, 1000, e);
    CHECK("sawRemote true after owned-entity batch", in.sawRemoteEntity());

    // (a) THE FIX: a cross-owner transfer intent must not survive a reload.
    InvXferPacket xf; std::memset(&xf, 0, sizeof(xf));
    xf.type = (u8)PKT_INV_XFER; xf.ownerId = 1;
    in.pushInvXfer(1, xf);
    {
        std::deque<InboundInvXfer> peek;
        in.drainInvXfers(peek);
        CHECK("invXfer enqueues normally", peek.size() == 1);
    }
    in.pushInvXfer(1, xf);          // re-enqueue, then hit the reload edge
    in.flushWorldState();
    {
        std::deque<InboundInvXfer> after;
        in.drainInvXfers(after);
        CHECK("invXfer dropped by flushWorldState (was the bug)", after.empty());
    }

    // (b) + (c): the same flush cleared readiness and advanced the generation.
    CHECK("sawRemote cleared by flushWorldState", !in.sawRemoteEntity());
    CHECK_EQ("generation advanced by flush", in.sessionGeneration(), 1);

    // world-state vs session-preserving: a world event drops, a LOAD_GO survives.
    EventPacket ev; std::memset(&ev, 0, sizeof(ev)); ev.type = (u8)PKT_EVENT; ev.ownerId = 1;
    in.pushEvent(1, ev);
    LoadGoPacket lg; std::memset(&lg, 0, sizeof(lg)); lg.type = (u8)PKT_LOAD_GO; lg.ownerId = 0;
    in.pushLoadGo(0, lg);
    in.flushWorldState();
    {
        std::deque<InboundEvent> evOut; in.drainEvents(evOut);
        std::deque<InboundLoadGo> lgOut; in.drainLoadGos(lgOut);
        CHECK("world-state event dropped by flush", evOut.empty());
        CHECK("coordinated-load GO survives flush (session-preserving)", lgOut.size() == 1);
    }
    CHECK_EQ("generation advanced again", in.sessionGeneration(), 2);
}

// ---- 11b. flushWorldState() full-coverage contract (Inbound.h) ------------------
// The invXfer_ bug (a world-state queue silently missing from the clear list) is a
// CLASS of bug: every queue Inbound owns must be classified as either WORLD-STATE
// (dropped on the reload/reconnect/disconnect edge) or SESSION-PRESERVING (kept
// because the connection outlives the world swap). This test pushes a sentinel
// into EVERY queue, hits one flush, and asserts the split for all of them, so a
// queue added later without being classified in flushWorldState() fails here.
//
// WHEN YOU ADD A NEW INBOUND QUEUE: add its push here and assert it in the correct
// group. A queue absent from both groups is unverified - that is the bug.
static void testFlushWorldStateContract() {
    std::printf("== flushWorldState full-coverage contract (Inbound.h) ==\n");
    Inbound in;

    // Zeroed payloads - the flush contract is about queue membership, not content.
    EntityState     e;   std::memset(&e,   0, sizeof(e));
    EventPacket     ev;  std::memset(&ev,  0, sizeof(ev));
    u32             cKey[5]; std::memset(cKey, 0, sizeof(cKey));
    WorldDropPacket wdp; std::memset(&wdp, 0, sizeof(wdp));
    WorldPickupPacket wpp; std::memset(&wpp, 0, sizeof(wpp));
    InvXferPacket   xf;  std::memset(&xf,  0, sizeof(xf));
    MedicalPacket   mp;  std::memset(&mp,  0, sizeof(mp));
    TreatmentPacket tp;  std::memset(&tp,  0, sizeof(tp));
    SpeedPacket     sp;  std::memset(&sp,  0, sizeof(sp));
    StatsPacket     stp; std::memset(&stp, 0, sizeof(stp));
    MoneyPacket     mo;  std::memset(&mo,  0, sizeof(mo));
    FactionPacket   fa;  std::memset(&fa,  0, sizeof(fa));
    TimePacket      ti;  std::memset(&ti,  0, sizeof(ti));
    DoorPacket      dp;  std::memset(&dp,  0, sizeof(dp));
    ProdPacket      pr;  std::memset(&pr,  0, sizeof(pr));
    ResearchPacket  rp;  std::memset(&rp,  0, sizeof(rp));
    BuildPlacePacket  bp; std::memset(&bp,  0, sizeof(bp));
    BuildStatePacket  bs; std::memset(&bs,  0, sizeof(bs));
    BuildDoorPacket   bd; std::memset(&bd,  0, sizeof(bd));
    BuildRemovePacket br; std::memset(&br,  0, sizeof(br));
    StealthPacket   sl;  std::memset(&sl,  0, sizeof(sl));
    SpawnReqPacket  sq;  std::memset(&sq,  0, sizeof(sq));
    SpawnInfoPacket si;  std::memset(&si,  0, sizeof(si));
    CamHintPacket   ch;  std::memset(&ch,  0, sizeof(ch));
    // Session-preserving payloads.
    SaveReqPacket   srq; std::memset(&srq, 0, sizeof(srq));
    SaveBeginPacket sbg; std::memset(&sbg, 0, sizeof(sbg));
    SaveFileHeader  sfh; std::memset(&sfh, 0, sizeof(sfh)); // pathLen/dataLen = 0
    SaveDoneHeader  sdh; std::memset(&sdh, 0, sizeof(sdh)); // fileCount = 0
    SaveAckPacket   sak; std::memset(&sak, 0, sizeof(sak));
    LoadGoPacket    lg;  std::memset(&lg,  0, sizeof(lg));
    LoadReqPacket   lrq; std::memset(&lrq, 0, sizeof(lrq));
    LoadNackPacket  lnk; std::memset(&lnk, 0, sizeof(lnk));

    // --- Push one sentinel into every WORLD-STATE queue (27).
    in.pushEntity(1, 0, e);
    in.pushEvent(1, ev);
    in.pushInv(1, 0, cKey, 0, 0);
    in.pushWorldItems(1, 0, 0);
    in.pushWorldRemove(1, 0, 0);
    in.pushNpcCensus(1, 0, 0, 0);
    in.pushWorldDrop(1, wdp);
    in.pushWorldPickup(1, wpp);
    in.pushInvXfer(1, xf);
    in.pushMedical(1, mp);
    in.pushTreatment(1, tp);
    in.pushSpeed(1, sp);
    in.pushStats(1, stp);
    in.pushMoney(1, mo);
    in.pushFaction(1, fa);
    in.pushTime(1, ti);
    in.pushDoor(1, dp);
    in.pushProd(1, pr);
    in.pushResearch(1, rp);
    in.pushBuildPlace(1, bp);
    in.pushBuildState(1, bs);
    in.pushBuildDoor(1, bd);
    in.pushBuildRemove(1, br);
    in.pushStealth(1, sl);
    in.pushSpawnReq(1, sq);
    in.pushSpawnInfo(1, si);
    in.pushCamHint(1, ch);

    // --- Push one sentinel into every SESSION-PRESERVING queue (10).
    in.pushConnect(0);
    in.pushLeave(0);
    in.pushSaveReq(1, srq);
    in.pushSaveBegin(1, sbg);
    in.pushSaveFile(1, sfh, "", 0);
    in.pushSaveDone(1, sdh, 0);
    in.pushSaveAck(1, sak);
    in.pushLoadGo(0, lg);
    in.pushLoadReq(1, lrq);
    in.pushLoadNack(1, lnk);

    in.flushWorldState();

    // --- Every WORLD-STATE queue must now be empty.
    #define WS_EMPTY(name, type, drain) do { \
        std::deque<type> out; in.drain(out); \
        CHECK("world-state dropped: " name, out.empty()); } while (0)
    WS_EMPTY("entity",      InboundEntity,      drainEntities);
    WS_EMPTY("event",       InboundEvent,       drainEvents);
    WS_EMPTY("inv",         InboundInv,         drainInv);
    WS_EMPTY("worldItems",  InboundWorldItems,  drainWorldItems);
    WS_EMPTY("worldRemove", InboundWorldRemove, drainWorldRemove);
    WS_EMPTY("npcCensus",   InboundNpcCensus,   drainNpcCensus);
    WS_EMPTY("worldDrop",   InboundWorldDrop,   drainWorldDrops);
    WS_EMPTY("worldPickup", InboundWorldPickup, drainWorldPickups);
    WS_EMPTY("invXfer",     InboundInvXfer,     drainInvXfers);
    WS_EMPTY("medical",     InboundMedical,     drainMedical);
    WS_EMPTY("treatment",   InboundTreatment,   drainTreatments);
    WS_EMPTY("speed",       InboundSpeed,       drainSpeed);
    WS_EMPTY("stats",       InboundStats,       drainStats);
    WS_EMPTY("money",       InboundMoney,       drainMoney);
    WS_EMPTY("faction",     InboundFaction,     drainFaction);
    WS_EMPTY("time",        InboundTime,        drainTime);
    WS_EMPTY("door",        InboundDoor,        drainDoor);
    WS_EMPTY("prod",        InboundProd,        drainProd);
    WS_EMPTY("research",    InboundResearch,    drainResearch);
    WS_EMPTY("buildPlace",  InboundBuildPlace,  drainBuildPlace);
    WS_EMPTY("buildState",  InboundBuildState,  drainBuildState);
    WS_EMPTY("buildDoor",   InboundBuildDoor,   drainBuildDoor);
    WS_EMPTY("buildRemove", InboundBuildRemove, drainBuildRemove);
    WS_EMPTY("stealth",     InboundStealth,     drainStealth);
    WS_EMPTY("spawnReq",    InboundSpawnReq,    drainSpawnReqs);
    WS_EMPTY("spawnInfo",   InboundSpawnInfo,   drainSpawnInfos);
    WS_EMPTY("camHint",     InboundCamHint,     drainCamHints);
    #undef WS_EMPTY

    // --- Every SESSION-PRESERVING queue must still hold its sentinel.
    #define SP_KEPT(name, type, drain) do { \
        std::deque<type> out; in.drain(out); \
        CHECK("session-preserving kept: " name, out.size() == 1); } while (0)
    { std::deque<u32> out; in.drainConnects(out);
      CHECK("session-preserving kept: connect", out.size() == 1); }
    { std::deque<u32> out; in.drainLeaves(out);
      CHECK("session-preserving kept: leave", out.size() == 1); }
    SP_KEPT("saveReq",   InboundSaveReq,   drainSaveReqs);
    SP_KEPT("saveBegin", InboundSaveBegin, drainSaveBegins);
    SP_KEPT("saveFile",  InboundSaveFile,  drainSaveFiles);
    SP_KEPT("saveDone",  InboundSaveDone,  drainSaveDones);
    SP_KEPT("saveAck",   InboundSaveAck,   drainSaveAcks);
    SP_KEPT("loadGo",    InboundLoadGo,    drainLoadGos);
    SP_KEPT("loadReq",   InboundLoadReq,   drainLoadReqs);
    SP_KEPT("loadNack",  InboundLoadNack,  drainLoadNacks);
    #undef SP_KEPT
}

// ---- 12. Worker-teardown ordering (models NetLink::stop()) -----------------------
// The NetLink::stop() fix: ENet teardown (enet_deinitialize + CloseHandle) must
// happen ONLY after the net worker has fully exited - the worker owns transport
// cleanup, so deinitializing while it still runs is a use-after-free / double
// free. The old code tore down unconditionally on a 2 s wait TIMEOUT. This locks
// the ordering invariant with a bare Win32 worker (no ENet dependency in the unit
// layer): stop() waits for the thread to exit FIRST, and the worker's cleanup
// strictly precedes the post-wait teardown.
static volatile LONG g_teardownSeq   = 0;
static LONG          g_workerCleanup = 0;
static LONG          g_teardown      = 0;
static DWORD WINAPI teardownWorker(LPVOID) {
    Sleep(40); // simulate the service loop draining + transport cleanup
    g_workerCleanup = InterlockedIncrement(&g_teardownSeq);
    return 0;
}
static void testTeardownOrdering() {
    std::printf("== worker-teardown ordering (NetLink::stop contract) ==\n");
    g_teardownSeq = 0; g_workerCleanup = 0; g_teardown = 0;
    HANDLE th = CreateThread(0, 0, &teardownWorker, 0, 0, 0);
    CHECK("worker thread created", th != 0);
    if (th) {
        DWORD wr = WaitForSingleObject(th, INFINITE); // stop(): wait for full exit
        CHECK("wait returns signalled (worker exited)", wr == WAIT_OBJECT_0);
        CloseHandle(th);
        g_teardown = InterlockedIncrement(&g_teardownSeq); // "enet_deinitialize" AFTER
        CHECK("worker cleanup precedes teardown", g_workerCleanup < g_teardown);
    }
}

// ---- ObjectHand: dual-layout unification contract (Phase 5b) ----------------
// Locks the ONE typed identity's two legacy array orders so a future edit can't
// silently reorder a field (the exact "dual hand[5] layout" desync footgun).
static void testObjectHandLayout() {
    std::printf("\n== ObjectHand layout (Phase 5b) ==\n");
    ObjectHand h;
    h.type = 11; h.container = 22; h.containerSerial = 33; h.index = 44; h.serial = 55;

    // OBJECT order  = {type, container, containerSerial, index, serial}
    u32 obj[5];
    h.toObjOrder(obj);
    CHECK("objOrder[0]=type",            obj[0] == 11);
    CHECK("objOrder[1]=container",       obj[1] == 22);
    CHECK("objOrder[2]=containerSerial", obj[2] == 33);
    CHECK("objOrder[3]=index",           obj[3] == 44);
    CHECK("objOrder[4]=serial",          obj[4] == 55);

    // CHAR-KEY order = {index, serial, type, container, containerSerial}
    u32 ck[5];
    h.toCharKey(ck);
    CHECK("charKey[0]=index",           ck[0] == 44);
    CHECK("charKey[1]=serial",          ck[1] == 55);
    CHECK("charKey[2]=type",            ck[2] == 11);
    CHECK("charKey[3]=container",       ck[3] == 22);
    CHECK("charKey[4]=containerSerial", ck[4] == 33);

    // The two legacy orders are genuinely different layouts (the footgun itself).
    CHECK("obj order != char-key order", std::memcmp(obj, ck, sizeof(obj)) != 0);

    // Round-trips: from*(to*(h)) == h for both orders.
    CHECK("fromObjOrder round-trips",  ObjectHand::fromObjOrder(obj).equals(h));
    CHECK("fromCharKey round-trips",   ObjectHand::fromCharKey(ck).equals(h));

    // Cross-order remap through the POD reproduces the manual [3][4][0][1][2]
    // char-key remap of an object-order array (the exact call-site footgun).
    u32 remap[5];
    ObjectHand::fromObjOrder(obj).toCharKey(remap);
    CHECK("obj->charkey remap [0]=obj[3]", remap[0] == obj[3]);
    CHECK("obj->charkey remap [1]=obj[4]", remap[1] == obj[4]);
    CHECK("obj->charkey remap [2]=obj[0]", remap[2] == obj[0]);
    CHECK("obj->charkey remap [3]=obj[1]", remap[3] == obj[1]);
    CHECK("obj->charkey remap [4]=obj[2]", remap[4] == obj[2]);

    // EntityState's named hand fields ARE object order: an ObjectHand built from
    // them must serialize to the same object-order array.
    EntityState e;
    std::memset(&e, 0, sizeof(e));
    e.hType = 11; e.hContainer = 22; e.hContainerSerial = 33; e.hIndex = 44; e.hSerial = 55;
    ObjectHand eh;
    eh.type = e.hType; eh.container = e.hContainer; eh.containerSerial = e.hContainerSerial;
    eh.index = e.hIndex; eh.serial = e.hSerial;
    CHECK("EntityState hand == object order", eh.equals(h));

    // resolvable(): the engine's null handle is all-zero; a non-zero index or
    // serial names a live object, type/container alone never do.
    ObjectHand z; z.type = z.container = z.containerSerial = z.index = z.serial = 0;
    CHECK("all-zero hand not resolvable",       !z.resolvable());
    ObjectHand idxOnly = z; idxOnly.index = 1;
    CHECK("index-only hand resolvable",          idxOnly.resolvable());
    ObjectHand serOnly = z; serOnly.serial = 1;
    CHECK("serial-only hand resolvable",         serOnly.resolvable());
    ObjectHand tcOnly = z; tcOnly.type = 7; tcOnly.container = 9;
    CHECK("type/container-only NOT resolvable", !tcOnly.resolvable());

    // equals() is field-sensitive on every one of the five fields.
    ObjectHand d;
    d = h; d.type++;            CHECK("equals detects type diff",      !d.equals(h));
    d = h; d.container++;       CHECK("equals detects container diff", !d.equals(h));
    d = h; d.containerSerial++; CHECK("equals detects cser diff",      !d.equals(h));
    d = h; d.index++;           CHECK("equals detects index diff",     !d.equals(h));
    d = h; d.serial++;          CHECK("equals detects serial diff",    !d.equals(h));
}

// ---- Engine fault throttle contract (Phase 5c) ------------------------------
// Locks the pure throttle decision that gates the "[engine] FAULT" oracle line:
// always emit the first hit, then at most once per interval, tolerating the
// wall-clock midnight wrap by erring toward an extra emit (never silent forever).
static void testEngineFaults() {
    std::printf("\n== engine fault throttle (Phase 5c) ==\n");
    using coop::engine::faultShouldLog;
    using coop::engine::FAULT_OP_COUNT;
    using coop::engine::FAULT_RESOLVE_CHAR;
    using coop::engine::FAULT_RESOLVE_OBJECT;

    CHECK("FAULT_OP_COUNT > 0",   (int)FAULT_OP_COUNT > 0);
    CHECK("resolve ops ordered",  FAULT_RESOLVE_CHAR == 0 && FAULT_RESOLVE_OBJECT == 1);

    unsigned long last = 0;
    CHECK("first hit logs",             faultShouldLog(1, 5000, &last, 1000));
    CHECK("first hit stamps lastMs",    last == 5000);
    CHECK("hit within interval quiet",  !faultShouldLog(2, 5500, &last, 1000));
    CHECK("lastMs unchanged in quiet",  last == 5000);
    CHECK("hit at interval logs",       faultShouldLog(3, 6000, &last, 1000));
    CHECK("lastMs advanced",            last == 6000);
    CHECK("just-before-boundary quiet", !faultShouldLog(4, 6999, &last, 1000));

    // Midnight wrap of wallClockMs: unsigned delta stays huge -> emit (never
    // permanently suppress across the wrap).
    unsigned long wrapLast = 86399000UL;
    CHECK("wrap boundary logs", faultShouldLog(5, 1000, &wrapLast, 1000));

    // Null lastMs (defensive): only the very first hit logs.
    CHECK("null lastMs first logs",  faultShouldLog(1, 0, 0, 1000));
    CHECK("null lastMs later quiet", !faultShouldLog(2, 0, 0, 1000));
}

static void testEngineCaps() {
    std::printf("\n== engine capability registry (Phase 5d) ==\n");
    using namespace coop::engine;

    // capName tokens are the oracle contract: stable, in enum order, guarded.
    CHECK("CAP_COUNT > 0",          (int)CAP_COUNT > 0);
    CHECK("cap core is hand",       std::strcmp(capName(CAP_HAND_RESOLVE), "hand_resolve") == 0);
    CHECK("cap saveload token",     std::strcmp(capName(CAP_SAVELOAD), "saveload") == 0);
    CHECK("cap faction token",      std::strcmp(capName(CAP_FACTION), "faction") == 0);
    CHECK("cap out-of-range low",   std::strcmp(capName((Capability)-1), "unknown") == 0);
    CHECK("cap out-of-range high",  std::strcmp(capName(CAP_COUNT), "unknown") == 0);

    // Synthetic resolved slots: two caps, one with a redundant required row.
    void* pA = (void*)1;  // saveload row 1
    void* pB = (void*)1;  // saveload row 2
    void* pH = (void*)1;  // hand_resolve (core)
    void* pF = (void*)1;  // faction (unrelated)
    const CapRow rows[] = {
        { &pA, "SaveManager::get",  CAP_SAVELOAD,     true },
        { &pB, "SaveManager::load", CAP_SAVELOAD,     true },
        { &pH, "hand::getCharacter",CAP_HAND_RESOLVE, true },
        { &pF, "FactionRelations",  CAP_FACTION,      true }
    };
    const int n = (int)(sizeof(rows) / sizeof(rows[0]));
    bool avail[CAP_COUNT];

    // (1) Everything resolved -> the three exercised caps are available; every
    // untouched cap (no rows) stays fail-closed false; core is OK.
    capEvaluate(rows, n, avail);
    CHECK("all-resolved saveload on",   avail[CAP_SAVELOAD]);
    CHECK("all-resolved hand on",       avail[CAP_HAND_RESOLVE]);
    CHECK("all-resolved faction on",    avail[CAP_FACTION]);
    CHECK("untouched cap fail-closed",  !avail[CAP_DOOR]);
    CHECK("core ok when hand resolved", capCoreOk(avail));

    // (2) One of saveload's two required rows drops -> the WHOLE cap fails, but
    // the other caps are untouched (no cross-contamination).
    pB = 0;
    capEvaluate(rows, n, avail);
    CHECK("partial-miss fails cap",     !avail[CAP_SAVELOAD]);
    CHECK("sibling cap unaffected",     avail[CAP_HAND_RESOLVE]);
    CHECK("unrelated cap unaffected",   avail[CAP_FACTION]);
    pB = (void*)1;

    // (3) Core hand-resolve missing -> capCoreOk trips (unsupported image),
    // while an unrelated cap can still be up.
    pH = 0;
    capEvaluate(rows, n, avail);
    CHECK("core down when hand missing", !capCoreOk(avail));
    CHECK("hand cap off",                !avail[CAP_HAND_RESOLVE]);
    CHECK("faction still up",            avail[CAP_FACTION]);
    pH = (void*)1;

    // (4) capRowResolved: null slot and null pointer both read as unresolved.
    void* live = (void*)1;
    void* dead = 0;
    CapRow rLive = { &live, "x", CAP_SAVELOAD, true };
    CapRow rDead = { &dead, "y", CAP_SAVELOAD, true };
    CapRow rNoSlot = { 0, "z", CAP_SAVELOAD, true };
    CHECK("row resolved (live ptr)",  capRowResolved(rLive));
    CHECK("row unresolved (null ptr)", !capRowResolved(rDead));
    CHECK("row unresolved (no slot)",  !capRowResolved(rNoSlot));
}

// Phase 6: the shared change-gated send/accept policy (ChangeGate.h). This
// locks the exact decisions the money + door channels used to inline by hand,
// so a future consolidation can't silently drift the wire cadence.
static void testChangeGate() {
    std::printf("\n== change-gate policy (Phase 6) ==\n");
    using namespace coop::sync;

    // --- gateSampleDue: first pass always samples, then once per sampleMs. ----
    CHECK("sample due first pass",      gateSampleDue(50000, 0, 1000));
    CHECK("sample not due within win", !gateSampleDue(50500, 50000, 1000));
    CHECK("sample due at interval",     gateSampleDue(51000, 50000, 1000));
    CHECK("sample due past interval",   gateSampleDue(52000, 50000, 1000));

    // --- gateSeqAccept: monotonic per-sender, first sight always accepted. ---
    CHECK("seq accept first sight",   gateSeqAccept(0, 1));
    CHECK("seq accept first sight hi",gateSeqAccept(0, 999));
    CHECK("seq accept newer",         gateSeqAccept(5, 6));
    CHECK("seq drop equal",          !gateSeqAccept(5, 5));
    CHECK("seq drop older",          !gateSeqAccept(5, 4));

    // --- gateShouldSend, MONEY flavor (minSend=1000, resend=5000, unsent=1) ---
    // A never-sent row streams once even unchanged (no silent seed).
    CHECK("money unsent unchanged sends",
          gateShouldSend(false, 90000, 0, 1000, 5000, true));
    // A change always crosses (row sent long ago, past the throttle).
    CHECK("money change sends",
          gateShouldSend(true, 90000, 80000, 1000, 5000, true));
    // ...but not within the min-send throttle window after a send.
    CHECK("money change throttled",
          !gateShouldSend(true, 80500, 80000, 1000, 5000, true));
    // Unchanged + sent recently (past throttle, before resend) holds.
    CHECK("money unchanged holds pre-resend",
          !gateShouldSend(false, 82000, 80000, 1000, 5000, true));
    // Unchanged + resend window elapsed -> safety resend.
    CHECK("money unchanged resends",
          gateShouldSend(false, 86000, 80000, 1000, 5000, true));

    // --- gateShouldSend, DOOR flavor (minSend=0, resend=10000, unsent=0) -----
    // A silently-seeded, never-sent, unchanged row HOLDS (no first-sight send).
    CHECK("door unsent unchanged holds",
          !gateShouldSend(false, 90000, 0, 0, 10000, false));
    // A real change crosses immediately (no throttle).
    CHECK("door change sends",
          gateShouldSend(true, 90000, 0, 0, 10000, false));
    // Unchanged, sent within resend window -> hold.
    CHECK("door unchanged holds pre-resend",
          !gateShouldSend(false, 85000, 80000, 0, 10000, false));
    // Unchanged, resend window elapsed -> safety resend.
    CHECK("door unchanged resends",
          gateShouldSend(false, 90000, 80000, 0, 10000, false));
    // A change sent 1ms ago still crosses under a zero throttle.
    CHECK("door change no throttle",
          gateShouldSend(true, 80001, 80000, 0, 10000, false));
}

// ---- 12. Ephemeral toast visibility clock (ToastTimer.h toastVisible) -----------
// Guards the peer connect/disconnect on-screen TOAST (2026-07-21): armPeerToast
// records GetTickCount at the connect/leave edge and coopPanelDrive keeps the
// EngineUi toast label shown only while toastVisible() is true, then disarms so
// the label self-hides - the "momentary transition notice" distinct from the
// persistent status banner. This locks the pure timing: un-armed is never shown,
// an armed toast shows through the window and hides exactly at TOAST_SHOW_MS, and
// the unsigned subtraction tolerates a GetTickCount wrap mid-window.
static void testToastTimer() {
    std::printf("== ephemeral toast visibility clock (ToastTimer.h) ==\n");
    using coop::engine::toastVisible;
    const unsigned long dur = coop::engine::TOAST_SHOW_MS; // 4000

    // Un-armed: never visible, whatever the clock says.
    CHECK("unarmed never visible",        !toastVisible(false, 0,     999999, dur));
    CHECK("unarmed never visible (armMs)",!toastVisible(false, 10000, 10000,  dur));

    // Armed: visible from the arming instant, through the window, hidden AT the
    // boundary (elapsed >= duration) and after.
    CHECK("armed visible at t0",           toastVisible(true, 10000, 10000, dur));
    CHECK("armed visible mid-window",      toastVisible(true, 10000, 12000, dur));
    CHECK("armed visible at 3999 ms",      toastVisible(true, 10000, 13999, dur));
    CHECK("armed hidden AT window (4000)", !toastVisible(true, 10000, 14000, dur));
    CHECK("armed hidden past window",      !toastVisible(true, 10000, 20000, dur));

    // GetTickCount wrap: armed 100 ms before the 2^32 rollover, the unsigned delta
    // still measures true elapsed time across the boundary.
    const unsigned long nearMax  = 0xFFFFFFFFul - 100; // armed 100 ms before wrap
    const unsigned long preWrap  = nearMax + 50;       // 50 ms later, no overflow
    const unsigned long postWrap = 1899UL;             // (nearMax + 2000) mod 2^32
    const unsigned long postHide = 3999UL;             // (nearMax + 4100) mod 2^32
    CHECK("wrap: 50 ms still visible",     toastVisible(true, nearMax, preWrap,  dur));
    CHECK("wrap: 2000 ms still visible",   toastVisible(true, nearMax, postWrap, dur));
    CHECK("wrap: 4100 ms hidden",         !toastVisible(true, nearMax, postHide, dur));

    // Duration sanity: a few seconds - long enough to read, short enough to feel
    // momentary and not be mistaken for the persistent banner.
    CHECK("toast window sane (2-8 s)", dur >= 2000 && dur <= 8000);
}

int main() {
    std::printf("prototest: KenshiCoop wire/hash/interp unit layer (protocol v%u)\n",
                (unsigned)PROTOCOL_VERSION);
    testSizes();
    testObjectHandLayout();
    testEngineFaults();
    testEngineCaps();
    testChangeGate();
    testRoundTrips();
    testFraming();
    testSaveCrc();
    testFolderFingerprint();
    testContentHash();
    testInterp();
    testOwnRanks();
    testSteamIdParse();
    testWorkPoseMatch();
    testTaskClear();
    testDeathRekey();
    testInboundLifecycle();
    testFlushWorldStateContract();
    testTeardownOrdering();
    testToastTimer();
    std::printf("\nprototest: %d/%d checks passed%s\n",
                g_total - g_failed, g_total, g_failed ? " - FAIL" : " - PASS");
    return g_failed;
}
