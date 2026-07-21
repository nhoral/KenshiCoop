// savexfer_test - data-safety unit coverage for the protocol-31 coordinated
// save RECEIVER (src/plugin/sync/SaveXfer.cpp), linked into prototest.
//
// The receiver stages a host->join save transfer into save/<name>__incoming/,
// verifies per-file CRCs on DONE, and atomically swaps it over save/<name>/.
// These tests drive the receiver against a REAL temp save tree (LOCALAPPDATA is
// pointed at a throwaway dir so saveFolderFor() resolves there) and assert the
// core invariant that MUST hold for real users: a transfer that FAILS, is
// EMPTY/degenerate, or carries an UNSAFE name must NEVER destroy, empty, or
// strand the join's existing local saves. Only the private __incoming staging
// may ever be discarded.
//
// Build: compiled + linked by scripts/build_prototest.cmd. Zero game calls -
// the engine/log/NetLink symbols SaveXfer.cpp references are stubbed below.
//
// v100 (VC++ 2010) compatible.

#define _CRT_SECURE_NO_WARNINGS 1
// enet (pulled in via net/NetLink.h below) includes <winsock2.h>; the build
// defines WIN32_LEAN_AND_MEAN (as the plugin does) so <windows.h> never drags
// in the old <winsock.h> that would collide with it.

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

#include "../netproto/Wire.h"
#include "../netproto/ContentHash.h"
#include "../plugin/sync/SaveXfer.h"
#include "../plugin/game/Engine.h"   // coop::engine::saveInfo (stubbed below)
#include "../plugin/CoopLog.h"       // coop::logLine/logErrLine (stubbed below)
#include "../plugin/net/NetLink.h"   // coop::NetLink queue* (stubbed below)

using namespace coop;

// prototest's shared counters (main.cpp). Every CHECK folds into the run total.
extern int g_total;
extern int g_failed;

#define SXCHECK(name, cond) do { \
    ++g_total; \
    if (cond) { std::printf("  ok   %s\n", name); } \
    else      { std::printf("  FAIL %s\n", name); ++g_failed; } \
} while (0)

// ---- Stubs for the symbols SaveXfer.cpp references (no game, no net) ---------
namespace coop {
    // The logger just goes to stdout so a failing run still shows the receiver's
    // own REFUSED/RECOVER/COMMIT/FAILED lines inline with the checks.
    void logInit(const char*, const char*) {}
    unsigned long wallClockMs() { return 0; }
    void logSetFakeSkewMs(long) {}
    void logLine(const char* m)    { std::printf("    [log] %s\n", m ? m : ""); }
    void logErrLine(const char* m) { std::printf("    [err] %s\n", m ? m : ""); }
    void logClose() {}

    namespace engine {
        // Force the %LOCALAPPDATA%\kenshi\save fallback so the test controls the
        // save root purely by setting LOCALAPPDATA (no engine pointers needed).
        bool saveInfo(char* cg, unsigned cl, char* sp, unsigned pl) {
            if (cg && cl) cg[0] = '\0';
            if (sp && pl) sp[0] = '\0';
            return false;
        }
    }

    // Sender-only sinks (the receiver tests never call the sender, but
    // SaveXfer.cpp's sender functions reference these members).
    void NetLink::queueSaveBegin(const SaveBeginPacket&) {}
    void NetLink::queueSaveFile(const SaveFileHeader&, const char*,
                                const unsigned char*, unsigned int) {}
    void NetLink::queueSaveDone(const SaveDoneHeader&, const u32*, unsigned int) {}
}

// ---- Tiny filesystem helpers (test-local; SaveXfer's own are file-private) ---
namespace {

void mkTree(const std::string& path) {
    for (size_t i = 3; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '\\' || path[i] == '/')
            CreateDirectoryA(path.substr(0, i).c_str(), 0);
    }
}

void rmTree(const std::string& folder) {
    WIN32_FIND_DATAA fd;
    std::string spec = folder + "\\*";
    HANDLE h = FindFirstFileA(spec.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.' &&
                (fd.cFileName[1] == '\0' ||
                 (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
                continue;
            std::string child = folder + "\\" + fd.cFileName;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rmTree(child);
            else { SetFileAttributesA(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                   DeleteFileA(child.c_str()); }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(folder.c_str());
}

bool dirExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void writeFile(const std::string& full, const std::string& content) {
    // Ensure parent exists.
    size_t s = full.find_last_of("\\/");
    if (s != std::string::npos) mkTree(full.substr(0, s));
    HANDLE h = CreateFileA(full.c_str(), GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    if (!content.empty()) WriteFile(h, content.data(), (DWORD)content.size(), &wrote, 0);
    CloseHandle(h);
}

std::string readFile(const std::string& full) {
    HANDLE h = CreateFileA(full.c_str(), GENERIC_READ, FILE_SHARE_READ, 0,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    std::string out;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof(buf), &got, 0) && got > 0) out.append(buf, got);
    CloseHandle(h);
    return out;
}

typedef std::pair<std::string, std::string> File; // (relpath, content)

// Drive a full BEGIN/FILE*/DONE transfer through the receiver. When zeroFiles is
// true a degenerate 0-file transfer is sent (no FILE chunks, empty CRC table).
// When badCrc is true the first file's CRC is corrupted so the DONE verify
// mismatches. Returns onSaveDone's result (1 committed, 0 failed/refused).
int deliver(const std::string& name, const std::vector<File>& files,
            bool badCrc, bool zeroFiles) {
    static u32 s_xfer = 100;
    u32 xfer = ++s_xfer;
    u16 count = zeroFiles ? 0 : (u16)files.size();

    SaveBeginPacket bp;
    memset(&bp, 0, sizeof(bp));
    bp.type = (u8)PKT_SAVE_BEGIN;
    bp.xferId = xfer;
    if (!name.empty()) strncpy(bp.name, name.c_str(), sizeof(bp.name) - 1);
    bp.fileCount = count;
    unsigned __int64 total = 0;
    if (!zeroFiles) for (size_t i = 0; i < files.size(); ++i) total += files[i].second.size();
    bp.totalBytes = total;
    savexfer::onSaveBegin(bp);

    std::vector<u32> crcs;
    if (!zeroFiles) {
        crcs.resize(files.size(), 0);
        for (size_t i = 0; i < files.size(); ++i) {
            const std::string& rel = files[i].first;
            const std::string& dat = files[i].second;
            SaveFileHeader h;
            memset(&h, 0, sizeof(h));
            h.type = (u8)PKT_SAVE_FILE;
            h.xferId = xfer;
            h.fileIdx = (u16)i;
            h.pathLen = (u16)rel.size();
            h.offset = 0;
            h.dataLen = (u16)dat.size();
            savexfer::onSaveFile(h, rel.c_str(),
                                 dat.empty() ? 0 : (const unsigned char*)dat.data());
            crcs[i] = fnv1aUpdate(fnv1aInit(),
                                  dat.empty() ? "" : dat.data(), (unsigned)dat.size());
        }
        if (badCrc && !crcs.empty()) crcs[0] ^= 0x1u; // corrupt -> DONE mismatch
    }

    SaveDoneHeader d;
    memset(&d, 0, sizeof(d));
    d.type = (u8)PKT_SAVE_DONE;
    d.xferId = xfer;
    d.fileCount = count;
    u16 of = 0; unsigned __int64 ob = 0;
    return savexfer::onSaveDone(d, crcs.empty() ? 0 : &crcs[0], &of, &ob);
}

} // namespace

// ---- The tests --------------------------------------------------------------

void testSaveXfer() {
    std::printf("== SaveXfer receiver data-safety (protocol 31) ==\n");

    // Point saveFolderFor() at a throwaway tree via LOCALAPPDATA.
    char tmp[MAX_PATH]; tmp[0] = '\0';
    GetTempPathA(sizeof(tmp), tmp);
    char root[MAX_PATH];
    _snprintf(root, sizeof(root) - 1, "%ssxtest_%lu", tmp, (unsigned long)GetCurrentProcessId());
    root[sizeof(root) - 1] = '\0';
    rmTree(root);
    std::string la = std::string(root);
    _putenv((std::string("LOCALAPPDATA=") + la).c_str());
    // saveFolderFor(name) now = <root>\kenshi\save\<name>
    std::string saveRoot = savexfer::saveFolderFor(std::string()); // <root>\kenshi\save\ (or trailing sep)

    // --- 0. name-safety predicate --------------------------------------------
    SXCHECK("saveNameSafe accepts a normal slot", savexfer::saveNameSafe("coopresume"));
    SXCHECK("saveNameSafe rejects empty (root)",  !savexfer::saveNameSafe(""));
    SXCHECK("saveNameSafe rejects separator",     !savexfer::saveNameSafe("a\\b"));
    SXCHECK("saveNameSafe rejects forward sep",   !savexfer::saveNameSafe("a/b"));
    SXCHECK("saveNameSafe rejects ..",            !savexfer::saveNameSafe(".."));
    SXCHECK("saveNameSafe rejects drive",         !savexfer::saveNameSafe("C:x"));

    // --- A. CRC-invalid transfer must leave an EXISTING save byte-intact ------
    // (documents the core CRC-failure invariant; the join keeps its own save.)
    {
        std::string cr = savexfer::saveFolderFor("coopresume");
        rmTree(cr);
        writeFile(cr + "\\quick.save", "JOIN-REAL-SAVE");
        std::vector<File> f;
        f.push_back(File("quick.save", "HOST-INCOMING-DIFFERENT"));
        int rc = deliver("coopresume", f, /*badCrc*/true, /*zeroFiles*/false);
        SXCHECK("badCrc: onSaveDone reports failure", rc == 0);
        SXCHECK("badCrc: join's existing save is byte-intact",
                readFile(cr + "\\quick.save") == "JOIN-REAL-SAVE");
        SXCHECK("badCrc: no __incoming staging left behind",
                !dirExists(savexfer::saveFolderFor("coopresume__incoming")));
        SXCHECK("badCrc: no __old orphan left behind",
                !dirExists(savexfer::saveFolderFor("coopresume__old")));
        rmTree(cr);
    }

    // --- B. degenerate 0-file transfer must NOT replace/empty a real save -----
    // (RED on the pre-fix receiver: fileCount==g_recvFileCount==0 counted as
    //  success -> commit moved the real save to __old, moved empty staging in,
    //  then deleted __old -> the join's save was destroyed.)
    {
        std::string cr = savexfer::saveFolderFor("coopresume");
        rmTree(cr);
        writeFile(cr + "\\quick.save", "JOIN-REAL-SAVE");
        int rc = deliver("coopresume", std::vector<File>(), /*badCrc*/false, /*zeroFiles*/true);
        SXCHECK("empty-xfer: onSaveDone reports failure", rc == 0);
        SXCHECK("empty-xfer: join's real save survives (not emptied)",
                readFile(cr + "\\quick.save") == "JOIN-REAL-SAVE");
        SXCHECK("empty-xfer: no __old orphan (backup not deleted)",
                !dirExists(savexfer::saveFolderFor("coopresume__old")));
        rmTree(cr);
    }

    // --- C. unsafe/empty NAME must NOT resolve to the save ROOT and wipe all ---
    // (RED on the pre-fix receiver: an empty name resolved finalDir to the save
    //  ROOT; a 0-file "success" moved the WHOLE save folder to __old -> every
    //  sibling save (squad1, current, ...) disappeared.)
    {
        std::string s1 = savexfer::saveFolderFor("squad1");
        std::string cu = savexfer::saveFolderFor("current");
        rmTree(s1); rmTree(cu);
        writeFile(s1 + "\\quick.save", "SQUAD1-DATA");
        writeFile(cu + "\\quick.save", "CURRENT-DATA");
        int rc = deliver(/*name*/"", std::vector<File>(), /*badCrc*/false, /*zeroFiles*/true);
        SXCHECK("unsafe-name: onSaveDone reports failure", rc == 0);
        SXCHECK("unsafe-name: sibling save 'squad1' survives",
                readFile(s1 + "\\quick.save") == "SQUAD1-DATA");
        SXCHECK("unsafe-name: sibling save 'current' survives",
                readFile(cu + "\\quick.save") == "CURRENT-DATA");
        rmTree(s1); rmTree(cu);
    }

    // --- D. a VALID transfer still commits (the fix must not break the happy path)
    {
        std::string cr = savexfer::saveFolderFor("coopresume");
        rmTree(cr);
        writeFile(cr + "\\quick.save", "OLD-JOIN-SAVE");
        std::vector<File> f;
        f.push_back(File("quick.save", "NEW-HOST-SAVE"));
        f.push_back(File("platoon\\a.platoon", "PLATOON-A"));
        int rc = deliver("coopresume", f, /*badCrc*/false, /*zeroFiles*/false);
        SXCHECK("valid-xfer: onSaveDone commits", rc == 1);
        SXCHECK("valid-xfer: host content is now on disk",
                readFile(cr + "\\quick.save") == "NEW-HOST-SAVE");
        SXCHECK("valid-xfer: nested host file committed",
                readFile(cr + "\\platoon\\a.platoon") == "PLATOON-A");
        SXCHECK("valid-xfer: staging cleaned up",
                !dirExists(savexfer::saveFolderFor("coopresume__incoming")));
        SXCHECK("valid-xfer: no __old orphan after commit",
                !dirExists(savexfer::saveFolderFor("coopresume__old")));
        rmTree(cr);
    }

    // --- E. crash recovery: a save stranded in __old is restored --------------
    // (simulates a commit that died AFTER moving the real save out but BEFORE
    //  moving the verified staging in - the "join lost its save" crash window.)
    {
        std::string cr  = savexfer::saveFolderFor("coopresume");
        std::string old = savexfer::saveFolderFor("coopresume__old");
        rmTree(cr); rmTree(old);
        writeFile(old + "\\quick.save", "STRANDED-REAL-SAVE"); // only __old exists
        savexfer::recoverStrandedSave("coopresume");
        SXCHECK("recover: stranded save restored to save/<name>",
                readFile(cr + "\\quick.save") == "STRANDED-REAL-SAVE");
        SXCHECK("recover: __old cleaned after restore",
                !dirExists(old));
        // Redundant-__old case: both exist -> __old reclaimed, real save kept.
        writeFile(cr + "\\quick.save", "COMMITTED-SAVE");
        writeFile(old + "\\quick.save", "REDUNDANT-BACKUP");
        savexfer::recoverStrandedSave("coopresume");
        SXCHECK("recover: committed save kept when both exist",
                readFile(cr + "\\quick.save") == "COMMITTED-SAVE");
        SXCHECK("recover: redundant __old reclaimed",
                !dirExists(old));
        rmTree(cr); rmTree(old);
    }

    // --- F. crash recovery: a stranded PARTIAL staging (__incoming) is discarded
    // Reproduces a transfer killed mid-receive (kill -9 / crash / forced game
    // close): the join has staged SOME but not ALL files into __incoming and the
    // commit was never attempted. On the next recovery the partial staging must be
    // discarded cleanly, and the join's real save must be left byte-intact - the
    // partial scratch is NEVER the real save and must never touch it.
    {
        std::string cr  = savexfer::saveFolderFor("coopresume");
        std::string inc = savexfer::saveFolderFor("coopresume__incoming");
        std::string old = savexfer::saveFolderFor("coopresume__old");
        rmTree(cr); rmTree(inc); rmTree(old);
        // Real save present; partial staging with only 1 of an expected 2 files.
        writeFile(cr  + "\\quick.save", "JOIN-REAL-SAVE");
        writeFile(inc + "\\quick.save", "HALF-RECEIVED"); // platoon\a.platoon never arrived
        savexfer::recoverStrandedSave("coopresume");
        SXCHECK("recover-partial: stranded __incoming discarded",
                !dirExists(inc));
        SXCHECK("recover-partial: join's real save byte-intact",
                readFile(cr + "\\quick.save") == "JOIN-REAL-SAVE");
        SXCHECK("recover-partial: no __old orphan invented",
                !dirExists(old));
        rmTree(cr); rmTree(inc); rmTree(old);
    }

    // --- F2. mid-commit-swap kill: __old (real save moved out) AND a leftover
    // __incoming both present, save/<name>/ absent. Recovery must restore the real
    // save from __old AND discard the orphaned staging in one pass.
    {
        std::string cr  = savexfer::saveFolderFor("coopresume");
        std::string inc = savexfer::saveFolderFor("coopresume__incoming");
        std::string old = savexfer::saveFolderFor("coopresume__old");
        rmTree(cr); rmTree(inc); rmTree(old);
        writeFile(old + "\\quick.save", "STRANDED-REAL-SAVE");     // real save, moved out
        writeFile(inc + "\\quick.save", "UNCOMMITTED-STAGING");    // staging, never swapped in
        savexfer::recoverStrandedSave("coopresume");
        SXCHECK("recover-swap: real save restored from __old",
                readFile(cr + "\\quick.save") == "STRANDED-REAL-SAVE");
        SXCHECK("recover-swap: __old cleaned after restore",
                !dirExists(old));
        SXCHECK("recover-swap: leftover __incoming discarded",
                !dirExists(inc));
        rmTree(cr); rmTree(inc); rmTree(old);
    }

    // --- F3. integration: a leftover partial staging must never leak into the
    // next committed save. Pre-seed __incoming with a stale file that is NOT part
    // of the new transfer; a following valid transfer of the same name must commit
    // ONLY the new file set (stale scratch discarded, real save replaced atomically).
    {
        std::string cr  = savexfer::saveFolderFor("coopresume");
        std::string inc = savexfer::saveFolderFor("coopresume__incoming");
        std::string old = savexfer::saveFolderFor("coopresume__old");
        rmTree(cr); rmTree(inc); rmTree(old);
        writeFile(cr  + "\\quick.save", "JOIN-REAL-SAVE");
        writeFile(inc + "\\stale.dat",  "GARBAGE-FROM-PRIOR-KILL"); // not in the new set
        writeFile(inc + "\\quick.save", "HALF-RECEIVED");
        std::vector<File> f;
        f.push_back(File("quick.save", "NEW-HOST-SAVE"));
        f.push_back(File("platoon\\a.platoon", "PLATOON-A"));
        int rc = deliver("coopresume", f, /*badCrc*/false, /*zeroFiles*/false);
        SXCHECK("leftover-partial: valid transfer still commits", rc == 1);
        SXCHECK("leftover-partial: committed save has the new content",
                readFile(cr + "\\quick.save") == "NEW-HOST-SAVE");
        SXCHECK("leftover-partial: nested new file committed",
                readFile(cr + "\\platoon\\a.platoon") == "PLATOON-A");
        SXCHECK("leftover-partial: stale scratch file did NOT leak into the commit",
                !dirExists(cr + "\\stale.dat") &&
                readFile(cr + "\\stale.dat").empty());
        SXCHECK("leftover-partial: staging cleaned up",
                !dirExists(inc));
        SXCHECK("leftover-partial: no __old orphan after commit",
                !dirExists(old));
        rmTree(cr); rmTree(inc); rmTree(old);
    }

    // Clean the throwaway tree.
    rmTree(root);
    (void)saveRoot;
}
