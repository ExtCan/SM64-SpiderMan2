#include "follow_monitor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sm2m {

void FollowMonitor::Reset() { *this = FollowMonitor(); }

void FollowMonitor::Finish() {
    const double rise = mMax_ - m0_;
    if (rise >= kMinRise) {
        Jump j;
        j.marioRise = rise;
        j.cameraRise = cMax_ - c0_;
        pending_.push_back(j);
        ++jumps_;
        sumMario_ += j.marioRise;
        sumCamera_ += j.cameraRise;
    }
    inJump_ = settling_ = false;
}

void FollowMonitor::Sample(double t, double marioUp, double cameraUp, bool airborne) {
    if (!std::isfinite(marioUp) || !std::isfinite(cameraUp)) return;
    if (settling_ && (airborne || t >= settleUntil_)) Finish();
    if (airborne && !inJump_) {
        inJump_ = true;
        m0_ = haveGround_ ? groundM_ : marioUp;
        c0_ = haveGround_ ? groundC_ : cameraUp;
        mMax_ = std::max(m0_, marioUp);
        cMax_ = std::max(c0_, cameraUp);
    }
    if (!airborne) {
        haveGround_ = true;
        groundM_ = marioUp;
        groundC_ = cameraUp;
    }
    if (!inJump_) return;
    mMax_ = std::max(mMax_, marioUp);
    cMax_ = std::max(cMax_, cameraUp);
    if (!airborne && !settling_) {
        settling_ = true;
        settleUntil_ = t + kSettle;
    }
}

void FollowMonitor::SampleResidual(double vertical, double horizontal, bool airborne) {
    if (!airborne || !std::isfinite(vertical) || !std::isfinite(horizontal)) return;
    ++airFrames_;
    sumAirV_ += std::fabs(vertical);
    sumAirH_ += std::fabs(horizontal);
}

std::vector<FollowMonitor::Jump> FollowMonitor::TakeJumps() {
    std::vector<Jump> out;
    out.swap(pending_);
    return out;
}

std::string FollowMonitor::Summary() const {
    char buf[320];
    std::string s;
    if (jumps_ > 0) {
        std::snprintf(buf, sizeof(buf), "%d jump(s): Mario rose %.2f m on average, the game camera %.2f m (%.0f%%)",
                      jumps_, MeanMarioRise(), MeanCameraRise(), 100.0 * FollowRatio());
        s = buf;
    }
    if (airFrames_ > 0) {
        std::snprintf(buf, sizeof(buf), "%swhile Mario was in the air the game moved Spider-Man %.2f m vertically and %.2f m "
                                        "horizontally away from him between frames (average of %d frames)",
                      s.empty() ? "" : "; ", MeanAirVertical(), MeanAirHorizontal(), airFrames_);
        s += buf;
    }
    return s;
}

} // namespace sm2m
