// OwnRanks.h - squad-tab ownership resolution (pure, zero game/Win32 deps).
//
// The ownership partition decides which squad tabs a peer controls locally and
// streams (its own) versus drives from the peer's stream. Host owns tab {0},
// join owns {1} by default; an explicit KENSHICOOP_OWN_SQUAD/OWN_RANK env
// override wins. This logic is shared by:
//   * Config.cpp   - initial resolution at load
//   * Plugin.cpp   - re-resolution when the F2 panel switches role mid-session
//   * prototest    - the no-game unit layer that guards the role-switch fix
//
// Bug this guards (2026-07-14): a session launched as HOST resolves ranks to
// {0}; switching to JOIN via the panel MUST re-resolve to {1}. Skipping that
// left the client claiming the host's rank-0 player squad, so that unit was
// treated as locally owned and never driven by the host's motion stream (it
// stood frozen while unowned NPCs replicated normally).

#ifndef COOP_OWN_RANKS_H
#define COOP_OWN_RANKS_H

#include <set>
#include <string>

namespace coop {

// Parse a CSV of unsigned ints ("0", "1", "1,2") into out. Tolerant of spaces
// or any non-digit separator. Returns true if at least one rank was parsed.
inline bool parseRankList(const std::string& csv, std::set<unsigned int>& out) {
    unsigned int v = 0; bool have = false; bool any = false;
    for (size_t i = 0; i < csv.size(); ++i) {
        char ch = csv[i];
        if (ch >= '0' && ch <= '9') { v = v * 10u + (unsigned int)(ch - '0'); have = true; }
        else if (have) { out.insert(v); any = true; v = 0; have = false; }
    }
    if (have) { out.insert(v); any = true; }
    return any;
}

// Resolve the ownership ranks a session should hold for a given role.
//   fromEnv == true : ranks came from an explicit env override - preserve them.
//   fromEnv == false: use the role default (host owns {0}, join owns {1}).
// Safe to call repeatedly; on a role switch the default is recomputed so the
// client can never keep the host's rank (see the header note above).
inline void resolveOwnRanks(std::set<unsigned int>& ranks, bool isHost, bool fromEnv) {
    if (fromEnv) return;
    ranks.clear();
    ranks.insert(isHost ? 0u : 1u);
}

} // namespace coop

#endif // COOP_OWN_RANKS_H
