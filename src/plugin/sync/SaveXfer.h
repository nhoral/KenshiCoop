// SaveXfer - coordinated save + session resume (protocol 31).
//
// The HOST's save is authoritative: on a coordinated save the host writes its
// native save (which already contains the join's squad state via the sync
// channels), waits for the folder to QUIESCE (Kenshi's save is deferred +
// multi-file: quick.save + platoon/*.platoon + zone/*.zone + portraits), then
// streams the whole save folder to the join over CH_RELIABLE in ~4 KB chunks.
// The join stages into save/<name>__incoming/, verifies per-file hashes on
// DONE, commits (replace save/<name>/), and ACKs. Resume = both clients load
// the identical file, which re-runs the shared-save-lineage guarantee all the
// hand-keyed replication rests on (no sidecar; session-placed buildings and
// recruits bake into ONE save with ONE hand, identical on both sides).
//
// This header is engine-free (file IO + wire only); Plugin.cpp drives it from
// the main thread. VS2010 (v100) compatible.

#ifndef KENSHICOOP_SAVEXFER_H
#define KENSHICOOP_SAVEXFER_H

#include <string>
#include "../../netproto/Wire.h"

namespace coop {

class NetLink;
class Inbound;

namespace savexfer {

// ---- Save-folder location ----------------------------------------------------

// On-disk folder of save 'name': <SaveManager::getSavePath()>/<name> when the
// runtime getters resolve (spike 39, validated by save_probe), else the
// %LOCALAPPDATA%\kenshi\save\<name> convention every harness script assumes.
std::string saveFolderFor(const std::string& name);

// Recursive inventory of 'folder': file count, total bytes, and the newest
// last-write FILETIME (as u64). Returns false when the folder doesn't exist.
bool folderInventory(const std::string& folder, unsigned int* outFiles,
                     unsigned __int64* outBytes, unsigned __int64* outLatestWrite);

// Protocol 32 (coordinated load): fingerprint of save 'name' - FNV-1a over
// the sorted lower-cased relative paths + per-file content CRCs
// (coop::folderFingerprintOf). Byte-identical folders agree cross-machine;
// 0 = missing/unreadable folder (the LOAD_GO/NACK "unknown" sentinel).
// Reads every file once (a few MB save = a few ms; called on load edges
// only, never per tick).
u32 folderFingerprint(const std::string& name);

// ---- Save-completion detection (folder quiescence) ---------------------------
// SaveManager::save is DEFERRED (it sets a signal; execute() writes the files
// over the following seconds), so the only robust completion edge is the save
// folder going quiet: a CHANGE vs the arm-time inventory must be seen first
// (an overwritten save starts from a fully-populated stable folder), then the
// inventory must hold still for the settle window with quick.save present.

// Begin watching save 'name' (one watch at a time; re-arming resets).
void armWatch(const std::string& name);
bool watching();

// Poll the watch (throttled internally; call every main-loop tick). Returns:
//   0 = pending; 1 = COMPLETE (change seen, then stable + quick.save);
//   2 = TIMEOUT (no change within the change window - treat the folder's
//       existing content as the save); -1 = not armed.
// outFiles/outBytes report the latest inventory; outWaitedMs the time since arm.
int tickWatch(unsigned int* outFiles, unsigned __int64* outBytes,
              unsigned long* outWaitedMs);

// ---- Sender (host): paced chunked send of a whole save folder ---------------
// After the quiescence edge, beginSend snapshots the folder's file list (paths
// + sizes) and queues PKT_SAVE_BEGIN; tickSend then streams PKT_SAVE_FILE
// chunks paced at ~32 x 4 KB per 50 ms window (~2.5 MB/s ceiling - a 4 MB save
// lands in under 2 s without starving the live channels, which ride the same
// CH_RELIABLE ordered stream) and finishes with the PKT_SAVE_DONE CRC table.

// Snapshot save 'name' and queue the BEGIN. Returns false when the folder is
// missing/empty (nothing is queued). One transfer at a time; a re-begin
// abandons the previous one (the join drops stale xferIds).
bool beginSend(NetLink& net, u32 localId, const std::string& name);
bool sending();
// Pump the active transfer (call every main-loop tick; internally throttled).
// Logs "[save] XFER-SENT ..." and returns true on the tick the DONE goes out.
bool tickSend(NetLink& net, u32 localId);

// ---- Receiver (join): staged, CRC-verified, atomically-committed ------------
// Chunks arrive in order (CH_RELIABLE): BEGIN wipes + creates
// save/<name>__incoming/, FILE chunks append into it (incremental FNV-1a-32
// per file), DONE verifies the CRC table and commits: the existing
// save/<name>/ is swapped out and the staging folder renamed over it. A
// failed verify discards staging and leaves the previous save untouched.

void onSaveBegin(const SaveBeginPacket& b);
// path (not '\0'-terminated, pathLen bytes) + data are the wire payload.
void onSaveFile(const SaveFileHeader& h, const char* path, const unsigned char* data);
// Returns 1 = verified + committed, 0 = failed (staging discarded). Fills the
// ACK counters either way.
int onSaveDone(const SaveDoneHeader& d, const u32* crcs,
               u16* outFiles, unsigned __int64* outBytes);

// ---- Scenario gate accessors -------------------------------------------------
// HOST: xferId of the last transfer whose DONE went out (0 = none yet).
u32 lastSentXferId();
// JOIN: outcome of the last DONE verify+commit (-1 = none yet, 1 = committed,
// 0 = failed).
int lastCommitResult();
// JOIN: monotonic count of DONE verifications handled (success or failure) -
// the coordinated-load pending latch keys off "a NEW commit happened" plus
// lastCommitName/lastCommitResult, instead of re-firing on an old latch.
u32 commitSeq();
// JOIN: save name of the last handled transfer.
std::string lastCommitName();
// HOST: the join's commit acknowledgement (Plugin.cpp feeds noteAck on the
// PKT_SAVE_ACK drain). lastAckXferId 0 = none yet; lastAckOk 1 = the join
// verified + committed - the load_sync scenario's "join holds my copy" gate.
void noteAck(u32 xferId, int ok);
u32  lastAckXferId();
int  lastAckOk();

// Protocol 32: a coordinated LOAD supersedes any in-flight save coordination -
// disarm the quiescence watch and abort an active send (the join drops the
// orphaned chunks by xferId; the reloaded world's saves restart cleanly).
void abortAll();

} // namespace savexfer
} // namespace coop

#endif // KENSHICOOP_SAVEXFER_H
