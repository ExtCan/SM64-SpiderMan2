// Does the game's camera follow Mario up and down? Measured from what the game
// actually rendered: for every jump, how far Mario rose above his take-off
// height and how far the game's camera rose in the same time (plus a moment
// after landing, for its smoothing). Also how far the game moved Spider-Man
// away from Mario between two frames while Mario was in the air - the
// signature of a character controller snapping him back to the ground.
//
// Pure bookkeeping (no game access), so it is unit tested.
#pragma once

#include <string>
#include <vector>

namespace sm2m {

class FollowMonitor {
public:
    struct Jump {
        double marioRise = 0;  // metres above take-off at the highest point
        double cameraRise = 0; // metres the camera rose over the same span
    };

    // Seconds the camera gets after landing to catch up.
    static constexpr double kSettle = 0.6;
    // Jumps lower than this aren't counted (steps, small hops).
    static constexpr double kMinRise = 0.75;

    void Reset();
    // One rendered frame: heights along the game's up axis (metres).
    void Sample(double t, double marioUp, double cameraUp, bool airborne);
    // Where the game had moved Spider-Man since the last pin (before the next
    // one): vertical and horizontal distance from Mario, per frame.
    void SampleResidual(double vertical, double horizontal, bool airborne);

    // Jumps completed since the last call.
    std::vector<Jump> TakeJumps();

    int Jumps() const { return jumps_; }
    double MeanMarioRise() const { return jumps_ ? sumMario_ / jumps_ : 0.0; }
    double MeanCameraRise() const { return jumps_ ? sumCamera_ / jumps_ : 0.0; }
    // Camera rise / Mario rise over all jumps (1 = follows fully, 0 = not at all).
    double FollowRatio() const { return sumMario_ > 0 ? sumCamera_ / sumMario_ : 0.0; }
    int AirFrames() const { return airFrames_; }
    double MeanAirVertical() const { return airFrames_ ? sumAirV_ / airFrames_ : 0.0; }
    double MeanAirHorizontal() const { return airFrames_ ? sumAirH_ / airFrames_ : 0.0; }

    // One line for the log ("" when there is nothing to say yet).
    std::string Summary() const;

private:
    void Finish();

    bool inJump_ = false;
    bool settling_ = false;
    double settleUntil_ = 0;
    double m0_ = 0, c0_ = 0, mMax_ = 0, cMax_ = 0;
    bool haveGround_ = false;
    double groundM_ = 0, groundC_ = 0; // the last sample on the ground (the take-off point)
    std::vector<Jump> pending_;
    int jumps_ = 0;
    double sumMario_ = 0, sumCamera_ = 0;
    int airFrames_ = 0;
    double sumAirV_ = 0, sumAirH_ = 0;
};

} // namespace sm2m
