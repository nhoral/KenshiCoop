// Nametag.h - pure caption builder for the remote-player body nametag.
//
// The co-op debug-marker HUD (KENSHICOOP_DEBUG_MARKERS) pins a text label to
// every body the host stream DRIVES on this client - i.e. the OTHER player's
// characters. By default that label read "DRV <charName>" (the in-game name of
// the proxy), which does not tell you WHOSE unit it is. This helper turns that
// label into the remote player's Steam persona name so you can see at a glance
// which bodies your friend is controlling.
//
// The logic here is deliberately pure (no game / Steam / Win32 deps) so it can
// be unit-tested in prototest. The caller (Replicator::debugMark) resolves the
// Steam persona name elsewhere and passes it in; this function only decides the
// final caption text and applies the honest fallback chain:
//   1. a real persona name (peer is a Steam friend / info already resolved)
//   2. the fallback string (the old "DRV <charName>" form) when Steam can't
//      give us a name (peer not a friend yet, LAN/UDP transport, API down)
//   3. a generic "[Remote Player]" if even the fallback is empty.

#ifndef COOP_NAMETAG_H
#define COOP_NAMETAG_H

#include <string>

namespace coop {

// Max caption length we emit (the marker caption buffer is 64 bytes; leave room
// for the terminator).
const size_t COOP_NAMETAG_MAX = 63;

// Trim ASCII spaces/tabs/CR/LF from both ends of s. Pure helper.
inline std::string coopTrim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

// True when 'persona' is a usable Steam persona name: non-null, has non-blank
// content, and is not Steam's "[unknown]" placeholder (returned by
// GetFriendPersonaName before RequestUserInformation has resolved a non-friend).
inline bool coopPersonaValid(const char* persona) {
    if (!persona) return false;
    std::string t = coopTrim(persona);
    if (t.empty()) return false;
    if (t == "[unknown]") return false;
    return true;
}

// Build the final nametag caption from a (maybe-invalid) Steam persona name and
// a fallback caption. Applies the fallback chain described in the file header
// and caps the result to COOP_NAMETAG_MAX. Pure - safe to unit-test.
inline std::string remoteNametagCaption(const char* persona, const char* fallback) {
    std::string out;
    if (coopPersonaValid(persona)) {
        out = coopTrim(persona);
    } else if (fallback && fallback[0] != '\0') {
        out = fallback;
    } else {
        out = "[Remote Player]";
    }
    if (out.size() > COOP_NAMETAG_MAX) out.resize(COOP_NAMETAG_MAX);
    return out;
}

} // namespace coop

#endif // COOP_NAMETAG_H
