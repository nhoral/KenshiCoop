// CoopLog - tiny thread-safe file logger for KenshiCoop.
//
// Why a separate logger when KenshiLib already has DebugLog/ErrorLog?
//   * Those route into the engine's kenshi.log, which is overwritten each
//     launch and interleaved with heavy engine spam - awkward for an automated
//     test runner to evaluate.
//   * KenshiLib's UI/log helpers are NOT safe to call off the main thread,
//     whereas this logger guards a FILE* with a CRITICAL_SECTION so the net
//     thread can write too.
//
// Output is a dedicated, per-line-flushed file (so it survives a hard kill),
// with a timestamp + mode tag (HOST/JOIN) on every line. The high-level
// coopLog()/coopErr() wrappers in KenshiCoop.cpp call BOTH this and the
// KenshiLib helpers, so events still appear in kenshi.log as before.

#ifndef KENSHICOOP_COOPLOG_H
#define KENSHICOOP_COOPLOG_H

namespace coop {

// Open the log file at 'path' (truncating any previous run) and remember a
// short mode tag (e.g. "HOST"/"JOIN"). Safe to call once at plugin load.
void logInit(const char* path, const char* modeTag);

// Append one INFO/ERROR line (timestamped, tagged) and flush. Thread-safe.
void logLine(const char* msg);
void logErrLine(const char* msg);

// Flush and close the file. Called right before ExitProcess on test self-exit.
void logClose();

} // namespace coop

#endif // KENSHICOOP_COOPLOG_H
