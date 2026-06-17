#define _CRT_SECURE_NO_WARNINGS 1

#include "Scenario.h"
#include "../CoopLog.h"
#include "../game/Engine.h"

#include <kenshi/GameWorld.h>
#include <kenshi/Character.h>

#include <cstdio>
#include <cmath>

namespace coop {
namespace {

// Log one "SCENARIO <kind> hand=i,s,t,c,cs pos=x,y,z" line for character 'c'.
// kind is "MEMBER" (authoritative) or "RECV" (observer). Returns false if the
// character's hand/pos couldn't be read.
bool logScenarioLine(const char* kind, Character* c) {
    if (!c) return false;
    unsigned int h[5];
    float x, y, z;
    if (!engine::readHand(c, h)) return false;
    if (!engine::readPos(c, &x, &y, &z)) return false;
    char b[160];
    _snprintf(b, sizeof(b) - 1,
              "SCENARIO %s hand=%u,%u,%u,%u,%u pos=%.2f,%.2f,%.2f",
              kind, h[0], h[1], h[2], h[3], h[4], x, y, z);
    b[sizeof(b) - 1] = '\0';
    coop::logLine(b);
    return true;
}

// leader_move (Stage 1): the HOST orders its squad leader to walk to a nearby
// destination and streams its transform; the JOIN drives its local copy of that
// same (shared-save) leader to the received transform. Host logs MEMBER, join
// logs RECV; the runner cross-checks them within tolerance.
class LeaderMoveScenario : public Scenario {
public:
    LeaderMoveScenario()
        : started_(false), passed_(false), recvCount_(0),
          lastLogMs_(0), haveStart_(false), sx_(0), sy_(0), sz_(0) {}

    virtual const char* name() const { return "leader_move"; }

    virtual void onStart(const ScenarioContext& ctx) {
        started_ = true;
        if (ctx.isHost) {
            Character* ld = engine::leader(ctx.gw);
            if (ld && engine::readPos(ld, &sx_, &sy_, &sz_)) {
                haveStart_ = true;
                engine::orderMoveTo(ld, sx_ + LEG, sy_, sz_ + LEG);
            }
        }
    }

    virtual bool onTick(const ScenarioContext& ctx) {
        // Emit a MEMBER/RECV line ~2 Hz so the runner has positions to compare
        // and an anchor to time its screenshot.
        if (ctx.elapsedMs - lastLogMs_ >= 500 || lastLogMs_ == 0) {
            lastLogMs_ = ctx.elapsedMs;
            Character* ld = engine::leader(ctx.gw);
            if (ctx.isHost) {
                // Oscillate between the start point and a far offset so the leader
                // keeps translating for the whole window (the later-loading join
                // then sees LIVE, sustained walking - the fair test for engine-
                // driven locomotion). Long legs + a long half-period keep straight
                // walking dominant and reversals rare (a reversal is a legitimate
                // stop/turn for engine locomotion, but we want them sparse). Then
                // SETTLE: return to the start and halt so the host streams a STILL
                // pose and the join converges for a fair cross-check.
                if (haveStart_ && ld) {
                    if (ctx.elapsedMs >= DURATION_MS - SETTLE_MS) {
                        engine::orderMoveTo(ld, sx_, sy_, sz_);
                    } else {
                        bool legB = ((ctx.elapsedMs / LEG_MS) % 2) != 0;
                        float tx = legB ? (sx_ + LEG) : sx_;
                        float tz = legB ? (sz_ + LEG) : sz_;
                        engine::orderMoveTo(ld, tx, sy_, tz);
                    }
                }
                logScenarioLine("MEMBER", ld);
            } else {
                if (logScenarioLine("RECV", ld)) ++recvCount_;
            }
        }

        if (ctx.elapsedMs >= DURATION_MS) {
            if (ctx.isHost) {
                // Authoritative side passes if its leader resolved (and, if we
                // had a start, ideally moved). Position match is the runner's job.
                passed_ = (engine::leader(ctx.gw) != 0);
            } else {
                passed_ = (recvCount_ >= 1); // observed + applied at least once
            }
            return true;
        }
        return false;
    }

    virtual bool passed() const { return passed_; }

private:
    static const unsigned long DURATION_MS = 24000; // long enough to overlap the join's load
    static const unsigned long SETTLE_MS   = 8000;  // final halt window (fair cross-check + converge)
    static const unsigned long LEG_MS      = 6000;  // oscillation half-period (sparse reversals)
    static const float         LEG;                 // straight-walk leg length (units)
    bool          started_;
    bool          passed_;
    unsigned int  recvCount_;
    unsigned long lastLogMs_;
    bool          haveStart_;
    float         sx_, sy_, sz_;
};

const float LeaderMoveScenario::LEG = 14.0f;

} // namespace

Scenario* makeScenario(const std::string& name) {
    if (name == "leader_move") return new LeaderMoveScenario();
    return 0;
}

} // namespace coop
