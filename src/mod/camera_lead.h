// Keeps the game camera with Mario when he jumps.
//
// The game's follow camera tracks its target's height through heavy
// smoothing - made for Spider-Man, who swings rather than hops. Mario jumps
// and falls all the time, and the camera trails him: 0.3's logs show it
// rising 60-90% of a jump's height, late.
//
// A camera that trails like that is, near enough, where its target was a
// moment ago - its lag. So while Mario is in the air the target is reported
// that far ahead along his vertical motion (his vertical speed times the
// lag: higher on the way up, lower on the way down), and the camera, a lag
// later, is where Mario is. The lag is learnt as he jumps, from how far the
// camera still trails for his speed (a little per frame), so it fits however
// this game's camera behaves - a heavy spring, a frame or two of delay.
// Nothing here feeds the camera's position straight back into the target, so
// the camera can't be made to overshoot and swing.
//
// "Trails" is measured from what the game rendered: the camera's height above
// Mario while he stands is its resting height (it changes when the player
// tilts the camera, so it is re-learnt on the ground); in the air the camera
// should stay that high above him.
//
// Pure arithmetic (no game access), unit tested.
#pragma once

namespace sm2m {

class CameraLead {
public:
    struct Params {
        float gain = 1.0f;        // 1 = make up for all of the camera's lag (0 = off, more = ahead of him)
        float maxLead = 3.0f;     // metres
        float restTau = 0.6f;     // s: how quickly the resting height follows camera tilts on the ground
        float smoothTau = 0.02f;  // s: smoothing of the lead itself
        float settle = 0.6f;      // s after landing before the resting height is learnt again
        float learnRate = 0.08f;  // per frame: how much of the lag measured in it is taken on
        float initialLag = 0.0f;  // s, until learnt (learning from nothing: no swing while it learns)
        float maxLag = 0.6f;      // s
        float minSpeed = 1.5f;    // m/s: slower frames don't teach the lag
    };

    void Reset();
    // The camera stopped following for a while (a pause): no lead until the
    // next frame, which starts afresh. What was learnt stays.
    void Interrupt();
    // One rendered frame: the height of the camera the game just rendered,
    // and Mario's in that frame - where he was when the game took the
    // camera's target from him (metres, along the up axis); his vertical speed
    // now (m/s), and whether he is in the air. Returns the lead to add to the
    // camera target's height from now on.
    float Update(double t, double marioUpRendered, double marioSpeedUp, double cameraUp, bool airborne,
                 const Params& p);
    float Lead() const { return lead_; }
    bool Learnt() const { return have_; }
    double RestHeight() const { return rest_; }
    double Lag() const { return lag_ < 0 ? 0.0 : lag_; }

private:
    bool have_ = false;       // the resting height is known
    double rest_ = 0;         // camera height above Mario at rest
    double lag_ = -1;         // the camera's lag (s), -1: not set yet
    bool wasLeading_ = false; // the last frame's target was led ...
    double ledSpeed_ = 0;     // ... for this speed
    double lastT_ = 0;
    bool started_ = false;
    bool wasAir_ = false;
    double landedAt_ = -1e30;
    float lead_ = 0;
};

} // namespace sm2m
