// SaveXfer implementation. See SaveXfer.h. Built for the VS2010 (v100) toolchain.

#define _CRT_SECURE_NO_WARNINGS 1

#include "SaveXfer.h"
#include "../CoopLog.h"
#include "../game/Engine.h" // engine::saveInfo (runtime save-path resolution)
#include "../net/NetLink.h"
#include "../../netproto/ContentHash.h" // fnv1aInit/Update (per-file CRC)

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace coop {
namespace savexfer {
namespace {

const unsigned long WATCH_POLL_MS      = 250;   // inventory poll cadence
const unsigned long WATCH_SETTLE_MS    = 1500;  // stable-for window = complete
const unsigned long WATCH_CHANGE_MS    = 30000; // no change within this = timeout
// Protocol 36 (blank-portraits session bug): the engine writes the squad
// portrait atlas (portraits_texture.png) as a late, separate step of the
// save. A folder can look settled - quick.save present, stable 1.5 s -
// before the atlas lands; declaring quiescence then ships/reloads a
// portrait-less save and the squad tab renders blank avatars. Hold the
// settled verdict for the portrait file up to this bound past arm; a save
// that GENUINELY never writes one (no squad portraits yet) still completes
// via the fallback, just later and with a warning.
const unsigned long WATCH_PORTRAIT_MS  = 10000; // bounded portrait-file wait

// Sender pacing: up to SEND_CHUNKS_PER_BURST x 4 KB chunks queued per
// SEND_BURST_MS window (~2.5 MB/s ceiling). save_probe measured the sync
// fixture at 3.7 MB / 35 files -> ~1.5 s in flight.
const unsigned long SEND_BURST_MS         = 50;
const unsigned int  SEND_CHUNKS_PER_BURST = 32;

// Join a folder and a child with exactly one separator.
std::string pathJoin(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char last = a[a.size() - 1];
    if (last == '\\' || last == '/') return a + b;
    return a + "\\" + b;
}

// Recursive walk helper: accumulate count/bytes/latest-write. Depth-capped so
// a pathological symlink loop cannot hang the main thread.
void walkFolder(const std::string& folder, int depth, unsigned int* files,
                unsigned __int64* bytes, unsigned __int64* latest) {
    if (depth > 4) return;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pathJoin(folder, "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == '\0' ||
             (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            walkFolder(pathJoin(folder, fd.cFileName), depth + 1, files, bytes, latest);
        } else {
            ++*files;
            *bytes += ((unsigned __int64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            unsigned __int64 wt =
                ((unsigned __int64)fd.ftLastWriteTime.dwHighDateTime << 32) |
                fd.ftLastWriteTime.dwLowDateTime;
            if (wt > *latest) *latest = wt;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// Recursive walk collecting RELATIVE file paths + sizes (the transfer's file
// list). 'prefix' is the accumulated relative path ("" at the root).
struct XferFile { std::string rel; unsigned __int64 size; };
void collectFiles(const std::string& folder, const std::string& prefix, int depth,
                  std::vector<XferFile>* out) {
    if (depth > 4) return;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pathJoin(folder, "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == '\0' ||
             (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
            continue;
        std::string rel = prefix.empty() ? std::string(fd.cFileName)
                                         : prefix + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            collectFiles(pathJoin(folder, fd.cFileName), rel, depth + 1, out);
        } else if (rel.size() <= SAVE_PATH_MAX) {
            XferFile xf;
            xf.rel  = rel;
            xf.size = ((unsigned __int64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            out->push_back(xf);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// Create every intermediate directory of 'fullPath' (a FILE path) that is
// missing. The staging root itself is created by onSaveBegin.
void ensureParentDirs(const std::string& fullPath) {
    for (size_t i = 0; i < fullPath.size(); ++i) {
        if (fullPath[i] == '\\' || fullPath[i] == '/') {
            if (i > 2) CreateDirectoryA(fullPath.substr(0, i).c_str(), 0);
        }
    }
}

// Recursively delete a folder tree (staging cleanup / commit swap). Depth-
// capped like the walkers.
void removeTree(const std::string& folder, int depth) {
    if (depth > 6) return;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pathJoin(folder, "*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.' &&
                (fd.cFileName[1] == '\0' ||
                 (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
                continue;
            std::string child = pathJoin(folder, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                removeTree(child, depth + 1);
            } else {
                SetFileAttributesA(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileA(child.c_str());
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(folder.c_str());
}

// A rejected path must never escape the staging folder: reject absolute
// paths, drive letters and any ".." component.
bool relPathSafe(const char* p, unsigned int len) {
    if (len == 0 || len > SAVE_PATH_MAX) return false;
    if (p[0] == '\\' || p[0] == '/') return false;
    for (unsigned int i = 0; i < len; ++i) {
        if (p[i] == ':') return false;
        if (p[i] == '.' && i + 1 < len && p[i + 1] == '.') return false;
    }
    return true;
}

// ---- Sender state (host, main thread only) ------------------------------------

bool                  g_sendActive = false;
u32                   g_sendXferId = 0;      // monotonic per-host
std::string           g_sendName;
std::string           g_sendFolder;
std::vector<XferFile> g_sendFiles;
std::vector<u32>      g_sendCrcs;
unsigned int          g_sendFileIdx = 0;
unsigned __int64      g_sendOffset  = 0;     // within the current file
HANDLE                g_sendHandle  = INVALID_HANDLE_VALUE;
u32                   g_sendCurCrc  = 0;
unsigned __int64      g_sendTotalBytes = 0;
unsigned __int64      g_sendSentBytes  = 0;
unsigned long         g_sendStartTick  = 0;
unsigned long         g_sendLastBurst  = 0;

void sendCloseFile() {
    if (g_sendHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_sendHandle);
        g_sendHandle = INVALID_HANDLE_VALUE;
    }
}

void sendAbort(const char* why) {
    char b[192];
    _snprintf(b, sizeof(b) - 1, "[save] XFER-ABORT id=%u %s", g_sendXferId, why);
    b[sizeof(b) - 1] = '\0'; coop::logErrLine(b);
    sendCloseFile();
    g_sendActive = false;
    g_sendFiles.clear();
    g_sendCrcs.clear();
}

// ---- Receiver state (join, main thread only) -----------------------------------

bool             g_recvActive = false;
u32              g_recvXferId = 0;
std::string      g_recvName;
std::string      g_recvStaging;
u16              g_recvFileCount = 0;
unsigned __int64 g_recvTotalBytes = 0;
unsigned __int64 g_recvBytes = 0;
int              g_recvOpenIdx = -1;   // fileIdx of the open handle
HANDLE           g_recvHandle = INVALID_HANDLE_VALUE;
std::vector<u32> g_recvCrcs;           // incremental FNV per fileIdx
std::vector<u8>  g_recvSeen;           // fileIdx touched at least once
unsigned long    g_recvStartTick = 0;

// Scenario gate accessors' backing state.
u32 g_lastSentXferId  = 0;
int g_lastCommitResult = -1;
u32 g_commitSeq        = 0; // bumps on every DONE handled (protocol 32 latch)

void recvCloseFile() {
    if (g_recvHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_recvHandle);
        g_recvHandle = INVALID_HANDLE_VALUE;
    }
    g_recvOpenIdx = -1;
}

// ---- Quiescence watch state (main thread only) --------------------------------

bool          g_watchArmed   = false;
std::string   g_watchFolder;
unsigned long g_watchArmTick = 0;
unsigned long g_watchLastPoll = 0;
bool          g_watchChangeSeen = false;
unsigned long g_watchLastChange = 0;     // tick of the last observed change
unsigned int  g_watchBaseFiles  = 0;     // arm-time inventory (the change baseline)
unsigned __int64 g_watchBaseBytes  = 0;
unsigned __int64 g_watchBaseWrite  = 0;
unsigned int  g_watchCurFiles   = 0;
unsigned __int64 g_watchCurBytes = 0;

} // namespace

std::string saveFolderFor(const std::string& name) {
    char curGame[96], savePath[512];
    std::string root;
    if (engine::saveInfo(curGame, sizeof(curGame), savePath, sizeof(savePath)) &&
        savePath[0] != '\0') {
        root = savePath;
    } else {
        const char* lad = getenv("LOCALAPPDATA");
        root = pathJoin(lad ? lad : "", "kenshi\\save");
    }
    return pathJoin(root, name);
}

bool folderInventory(const std::string& folder, unsigned int* outFiles,
                     unsigned __int64* outBytes, unsigned __int64* outLatestWrite) {
    unsigned int files = 0;
    unsigned __int64 bytes = 0, latest = 0;
    DWORD attrs = GetFileAttributesA(folder.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        if (outFiles) *outFiles = 0;
        if (outBytes) *outBytes = 0;
        if (outLatestWrite) *outLatestWrite = 0;
        return false;
    }
    walkFolder(folder, 0, &files, &bytes, &latest);
    if (outFiles) *outFiles = files;
    if (outBytes) *outBytes = bytes;
    if (outLatestWrite) *outLatestWrite = latest;
    return true;
}

u32 folderFingerprint(const std::string& name) {
    std::string folder = saveFolderFor(name);
    std::vector<XferFile> files;
    collectFiles(folder, "", 0, &files);
    if (files.empty() || files.size() > 4096) return 0;

    // Per-file content CRC (streamed - same fnv1a the transfer table uses).
    std::vector<unsigned int> crcs(files.size(), 0);
    std::vector<const char*>  paths(files.size(), (const char*)0);
    std::vector<unsigned char> buf(65536);
    for (size_t i = 0; i < files.size(); ++i) {
        paths[i] = files[i].rel.c_str();
        HANDLE h = CreateFileA(pathJoin(folder, files[i].rel).c_str(), GENERIC_READ,
                               FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
        if (h == INVALID_HANDLE_VALUE) return 0; // unreadable = unknown
        unsigned int crc = fnv1aInit();
        DWORD got = 0;
        while (ReadFile(h, &buf[0], (DWORD)buf.size(), &got, 0) && got > 0)
            crc = fnv1aUpdate(crc, &buf[0], (unsigned int)got);
        CloseHandle(h);
        crcs[i] = crc;
    }
    return folderFingerprintOf(&paths[0], &crcs[0], (unsigned int)files.size());
}

void armWatch(const std::string& name) {
    g_watchArmed      = true;
    g_watchFolder     = saveFolderFor(name);
    g_watchArmTick    = GetTickCount();
    g_watchLastPoll   = 0;
    g_watchChangeSeen = false;
    g_watchLastChange = 0;
    g_watchBaseFiles  = 0;
    g_watchBaseBytes  = 0;
    g_watchBaseWrite  = 0;
    g_watchCurFiles   = 0;
    g_watchCurBytes   = 0;
    folderInventory(g_watchFolder, &g_watchBaseFiles, &g_watchBaseBytes,
                    &g_watchBaseWrite);
    char b[640];
    _snprintf(b, sizeof(b) - 1,
              "[save] WATCH armed folder='%s' baseFiles=%u baseBytes=%I64u",
              g_watchFolder.c_str(), g_watchBaseFiles, g_watchBaseBytes);
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
}

bool watching() { return g_watchArmed; }

int tickWatch(unsigned int* outFiles, unsigned __int64* outBytes,
              unsigned long* outWaitedMs) {
    if (!g_watchArmed) return -1;
    unsigned long now = GetTickCount();
    if (outWaitedMs) *outWaitedMs = now - g_watchArmTick;
    if (outFiles) *outFiles = g_watchCurFiles;
    if (outBytes) *outBytes = g_watchCurBytes;
    if (g_watchLastPoll != 0 && now - g_watchLastPoll < WATCH_POLL_MS) return 0;
    g_watchLastPoll = now;

    unsigned int files = 0;
    unsigned __int64 bytes = 0, latest = 0;
    folderInventory(g_watchFolder, &files, &bytes, &latest);
    g_watchCurFiles = files;
    g_watchCurBytes = bytes;
    if (outFiles) *outFiles = files;
    if (outBytes) *outBytes = bytes;

    bool differsFromBase = (files != g_watchBaseFiles) ||
                           (bytes != g_watchBaseBytes) ||
                           (latest > g_watchBaseWrite);
    if (!g_watchChangeSeen) {
        if (differsFromBase) {
            g_watchChangeSeen = true;
            g_watchLastChange = now;
            // Track "still changing" from this new state onward.
            g_watchBaseFiles = files;
            g_watchBaseBytes = bytes;
            g_watchBaseWrite = latest;
        } else if (now - g_watchArmTick >= WATCH_CHANGE_MS) {
            g_watchArmed = false;
            return 2; // never saw the save land - existing content is the save
        }
        return 0;
    }
    if (differsFromBase) {
        g_watchLastChange = now;
        g_watchBaseFiles  = files;
        g_watchBaseBytes  = bytes;
        g_watchBaseWrite  = latest;
        return 0;
    }
    // Stable since the last change: complete once the settle window elapses
    // AND the folder holds a loadable core (quick.save).
    if (now - g_watchLastChange >= WATCH_SETTLE_MS) {
        DWORD qs = GetFileAttributesA(pathJoin(g_watchFolder, "quick.save").c_str());
        if (qs != INVALID_FILE_ATTRIBUTES) {
            // Portrait gate (protocol 36): the atlas is written late; hold a
            // settled-looking folder for it (bounded) so the transferred /
            // reloaded save doesn't blank the squad-tab avatars.
            DWORD pt = GetFileAttributesA(
                pathJoin(g_watchFolder, "portraits_texture.png").c_str());
            if (pt == INVALID_FILE_ATTRIBUTES &&
                now - g_watchArmTick < WATCH_PORTRAIT_MS)
                return 0; // keep watching; any write re-enters the change path
            if (pt == INVALID_FILE_ATTRIBUTES)
                coop::logLine("[save] WARN quiesced WITHOUT portraits_texture.png "
                              "(bounded wait expired; squad avatars may be blank)");
            g_watchArmed = false;
            return 1;
        }
    }
    return 0;
}

// ---- Sender (host) -------------------------------------------------------------

bool beginSend(NetLink& net, u32 localId, const std::string& name) {
    sendCloseFile();
    g_sendFiles.clear();
    g_sendCrcs.clear();
    g_sendActive = false;

    std::string folder = saveFolderFor(name);
    DWORD attrs = GetFileAttributesA(folder.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        char b[640];
        _snprintf(b, sizeof(b) - 1, "[save] XFER-BEGIN refused: no folder '%s'",
                  folder.c_str());
        b[sizeof(b) - 1] = '\0'; coop::logErrLine(b);
        return false;
    }
    collectFiles(folder, "", 0, &g_sendFiles);
    if (g_sendFiles.empty() || g_sendFiles.size() > 0xFFFF) {
        coop::logErrLine("[save] XFER-BEGIN refused: empty/oversized file list");
        g_sendFiles.clear();
        return false;
    }
    g_sendTotalBytes = 0;
    for (size_t i = 0; i < g_sendFiles.size(); ++i)
        g_sendTotalBytes += g_sendFiles[i].size;

    ++g_sendXferId;
    g_sendName      = name;
    g_sendFolder    = folder;
    g_sendFileIdx   = 0;
    g_sendOffset    = 0;
    g_sendCurCrc    = fnv1aInit();
    g_sendSentBytes = 0;
    g_sendStartTick = GetTickCount();
    g_sendLastBurst = 0;
    g_sendCrcs.assign(g_sendFiles.size(), 0);
    g_sendActive    = true;

    SaveBeginPacket bp;
    memset(&bp, 0, sizeof(bp));
    bp.type    = (u8)PKT_SAVE_BEGIN;
    bp.ownerId = localId;
    bp.xferId  = g_sendXferId;
    strncpy(bp.name, name.c_str(), sizeof(bp.name) - 1);
    bp.fileCount  = (u16)g_sendFiles.size();
    bp.totalBytes = g_sendTotalBytes;
    net.queueSaveBegin(bp);

    char b[192];
    _snprintf(b, sizeof(b) - 1,
              "[save] XFER-BEGIN id=%u name='%s' files=%u bytes=%I64u",
              g_sendXferId, name.c_str(), (unsigned)g_sendFiles.size(),
              g_sendTotalBytes);
    b[sizeof(b) - 1] = '\0'; coop::logLine(b);
    return true;
}

bool sending() { return g_sendActive; }

bool tickSend(NetLink& net, u32 localId) {
    if (!g_sendActive) return false;
    unsigned long now = GetTickCount();
    if (g_sendLastBurst != 0 && now - g_sendLastBurst < SEND_BURST_MS) return false;
    g_sendLastBurst = now;

    unsigned char buf[SAVE_CHUNK_MAX];
    for (unsigned int c = 0; c < SEND_CHUNKS_PER_BURST; ++c) {
        if (g_sendFileIdx >= g_sendFiles.size()) break;
        const XferFile& xf = g_sendFiles[g_sendFileIdx];

        if (g_sendHandle == INVALID_HANDLE_VALUE) {
            g_sendHandle = CreateFileA(pathJoin(g_sendFolder, xf.rel).c_str(),
                                       GENERIC_READ, FILE_SHARE_READ, 0,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
            if (g_sendHandle == INVALID_HANDLE_VALUE) {
                sendAbort("open failed (save changed mid-transfer?)");
                return false;
            }
            g_sendOffset = 0;
            g_sendCurCrc = fnv1aInit();
        }

        DWORD want = SAVE_CHUNK_MAX;
        if (xf.size - g_sendOffset < (unsigned __int64)want)
            want = (DWORD)(xf.size - g_sendOffset);
        DWORD got = 0;
        if (want > 0 && (!ReadFile(g_sendHandle, buf, want, &got, 0) || got != want)) {
            sendAbort("read failed (save changed mid-transfer?)");
            return false;
        }
        g_sendCurCrc = fnv1aUpdate(g_sendCurCrc, buf, (unsigned int)got);

        SaveFileHeader fh;
        fh.type    = (u8)PKT_SAVE_FILE;
        fh.ownerId = localId;
        fh.xferId  = g_sendXferId;
        fh.fileIdx = (u16)g_sendFileIdx;
        fh.pathLen = (u16)xf.rel.size();
        fh.offset  = (u32)g_sendOffset;
        fh.dataLen = (u16)got;
        net.queueSaveFile(fh, xf.rel.c_str(), buf, (unsigned int)got);

        g_sendOffset    += got;
        g_sendSentBytes += got;
        if (g_sendOffset >= xf.size) {
            g_sendCrcs[g_sendFileIdx] = g_sendCurCrc;
            sendCloseFile();
            ++g_sendFileIdx;
        }
    }

    if (g_sendFileIdx >= g_sendFiles.size()) {
        SaveDoneHeader dh;
        dh.type      = (u8)PKT_SAVE_DONE;
        dh.ownerId   = localId;
        dh.xferId    = g_sendXferId;
        dh.fileCount = (u16)g_sendFiles.size();
        net.queueSaveDone(dh, g_sendCrcs.empty() ? 0 : &g_sendCrcs[0],
                          (unsigned int)g_sendCrcs.size());
        char b[176];
        _snprintf(b, sizeof(b) - 1,
                  "[save] XFER-SENT id=%u files=%u bytes=%I64u ms=%lu",
                  g_sendXferId, (unsigned)g_sendFiles.size(), g_sendSentBytes,
                  GetTickCount() - g_sendStartTick);
        b[sizeof(b) - 1] = '\0'; coop::logLine(b);
        g_lastSentXferId = g_sendXferId;
        g_sendActive = false;
        g_sendFiles.clear();
        return true;
    }
    return false;
}

u32 lastSentXferId()  { return g_lastSentXferId; }
int lastCommitResult() { return g_lastCommitResult; }
u32 commitSeq()        { return g_commitSeq; }
std::string lastCommitName() { return g_recvName; }

static u32 g_lastAckXferId = 0;
static int g_lastAckOk     = -1;
void noteAck(u32 xferId, int ok) { g_lastAckXferId = xferId; g_lastAckOk = ok; }
u32  lastAckXferId() { return g_lastAckXferId; }
int  lastAckOk()     { return g_lastAckOk; }

void abortAll() {
    if (g_watchArmed) {
        g_watchArmed = false;
        coop::logLine("[save] WATCH disarmed (superseded by coordinated load)");
    }
    if (g_sendActive) sendAbort("superseded by coordinated load");
}

// ---- Receiver (join) -------------------------------------------------------------

void onSaveBegin(const SaveBeginPacket& b) {
    recvCloseFile();
    char name[sizeof(b.name) + 1];
    memcpy(name, b.name, sizeof(b.name));
    name[sizeof(b.name)] = '\0';

    g_recvName       = name;
    g_recvXferId     = b.xferId;
    g_recvFileCount  = b.fileCount;
    g_recvTotalBytes = b.totalBytes;
    g_recvBytes      = 0;
    g_recvStartTick  = GetTickCount();
    g_recvStaging    = saveFolderFor(g_recvName + "__incoming");
    g_recvCrcs.assign(b.fileCount, fnv1aInit());
    g_recvSeen.assign(b.fileCount, 0);

    // A fresh staging folder: stale partials from an aborted transfer would
    // otherwise pollute the CRC verify.
    removeTree(g_recvStaging, 0);
    ensureParentDirs(pathJoin(g_recvStaging, "x")); // save root may not exist yet
    g_recvActive = (CreateDirectoryA(g_recvStaging.c_str(), 0) != 0) ||
                   (GetLastError() == ERROR_ALREADY_EXISTS);

    char lb[704];
    _snprintf(lb, sizeof(lb) - 1,
              "[save] XFER-RECV id=%u name='%s' files=%u bytes=%I64u staging='%s'%s",
              b.xferId, name, (unsigned)b.fileCount, b.totalBytes,
              g_recvStaging.c_str(), g_recvActive ? "" : " STAGING-FAILED");
    lb[sizeof(lb) - 1] = '\0'; coop::logLine(lb);
}

void onSaveFile(const SaveFileHeader& h, const char* path, const unsigned char* data) {
    if (!g_recvActive || h.xferId != g_recvXferId) return; // stale/aborted transfer
    if (h.fileIdx >= g_recvFileCount) return;
    if (!relPathSafe(path, h.pathLen)) {
        coop::logErrLine("[save] XFER chunk rejected: unsafe relative path");
        return;
    }

    if (g_recvOpenIdx != (int)h.fileIdx) {
        recvCloseFile();
        std::string rel(path, path + h.pathLen);
        std::string full = pathJoin(g_recvStaging, rel);
        ensureParentDirs(full);
        // CREATE_ALWAYS on the file's first chunk; OPEN_EXISTING when a later
        // chunk re-opens it (only happens after an interleave, which the
        // ordered channel + sequential sender never produce - belt/braces).
        g_recvHandle = CreateFileA(full.c_str(), GENERIC_WRITE, 0, 0,
                                   g_recvSeen[h.fileIdx] ? OPEN_EXISTING : CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, 0);
        if (g_recvHandle == INVALID_HANDLE_VALUE) {
            coop::logErrLine("[save] XFER chunk write-open FAILED");
            return;
        }
        g_recvOpenIdx = (int)h.fileIdx;
        g_recvSeen[h.fileIdx] = 1;
    }

    LONG hi = 0;
    SetFilePointer(g_recvHandle, (LONG)h.offset, &hi, FILE_BEGIN);
    DWORD wrote = 0;
    if (h.dataLen > 0) {
        if (!WriteFile(g_recvHandle, data, h.dataLen, &wrote, 0) || wrote != h.dataLen) {
            coop::logErrLine("[save] XFER chunk write FAILED");
            return;
        }
        g_recvCrcs[h.fileIdx] = fnv1aUpdate(g_recvCrcs[h.fileIdx], data, h.dataLen);
        g_recvBytes += h.dataLen;
    }
}

int onSaveDone(const SaveDoneHeader& d, const u32* crcs,
               u16* outFiles, unsigned __int64* outBytes) {
    if (outFiles) *outFiles = 0;
    if (outBytes) *outBytes = 0;
    if (!g_recvActive || d.xferId != g_recvXferId) return 0;
    recvCloseFile();
    g_recvActive = false;

    bool ok = (d.fileCount == g_recvFileCount);
    unsigned int bad = 0;
    if (ok) {
        for (u16 i = 0; i < d.fileCount; ++i) {
            if (!g_recvSeen[i] || g_recvCrcs[i] != crcs[i]) { ++bad; ok = false; }
        }
    }

    if (ok) {
        // Commit: swap the staged folder over save/<name>/ - the previous
        // save is only removed AFTER the new one is in place.
        std::string finalDir = saveFolderFor(g_recvName);
        std::string oldDir   = finalDir + "__old";
        removeTree(oldDir, 0);
        bool hadOld = false;
        if (GetFileAttributesA(finalDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
            hadOld = (MoveFileExA(finalDir.c_str(), oldDir.c_str(),
                                  MOVEFILE_WRITE_THROUGH) != 0);
            if (!hadOld) ok = false;
        }
        if (ok && !MoveFileExA(g_recvStaging.c_str(), finalDir.c_str(),
                               MOVEFILE_WRITE_THROUGH)) {
            ok = false;
            if (hadOld) MoveFileExA(oldDir.c_str(), finalDir.c_str(),
                                    MOVEFILE_WRITE_THROUGH); // restore
        }
        if (ok && hadOld) removeTree(oldDir, 0);
    }
    if (!ok) removeTree(g_recvStaging, 0); // never leave a half-written loadable save

    if (outFiles) *outFiles = ok ? g_recvFileCount : 0;
    if (outBytes) *outBytes = ok ? g_recvBytes : 0;

    char b[192];
    _snprintf(b, sizeof(b) - 1,
              "[save] XFER-%s id=%u name='%s' files=%u bytes=%I64u badCrc=%u ms=%lu",
              ok ? "COMMIT" : "FAILED", d.xferId, g_recvName.c_str(),
              (unsigned)g_recvFileCount, g_recvBytes, bad,
              GetTickCount() - g_recvStartTick);
    b[sizeof(b) - 1] = '\0';
    if (ok) coop::logLine(b); else coop::logErrLine(b);
    g_lastCommitResult = ok ? 1 : 0;
    ++g_commitSeq;
    return ok ? 1 : 0;
}

} // namespace savexfer
} // namespace coop
