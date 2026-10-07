#include "camera_lead.h"

#include <algorithm>
#include <cmath>

namespace sm2m {

void CameraLead::Reset() { *this = CameraLead(); }

void CameraLead::Interrupt() {
    lead_ = 0;
    started_ = false;
    wasLeading_ = false;
}

float CameraLead::Update(double t, double marioUp, double speedUp, double cameraUp, bool airborne, const Params& p) {
    // (marioUp: Mario in the frame the camera was rendered for.)
    if (!std::isfinite(marioUp) || !std::isfinite(cameraUp) || !std::isfinite(t) || !std::isfinite(speedUp))
        return lead_;
    double dt = started_ ? t - lastT_ : 0.0;
    if (dt < 0 || dt > 0.5) dt = 0; // a pause or a hitch: no smoothing step across it
    started_ = true;
    lastT_ = t;
    if (wasAir_ && !airborne) landedAt_ = t;
    wasAir_ = airborne;
    const bool settling = !airborne && t - landedAt_ < double(p.settle);
    if (lag_ < 0) lag_ = std::max(0.0, std::min(double(p.maxLag), double(p.initialLag)));

    double target = 0;
    if (!airborne && !settling) {
        // Standing or running: learn how high the camera rests above him.
        const double h = cameraUp - marioUp;
        if (!have_) {
            rest_ = h;
            have_ = true;
        } else if (dt > 0) {
            const double a = 1.0 - std::exp(-dt / std::max(0.01, double(p.restTau)));
            rest_ += (h - rest_) * a;
        }
    } else if (airborne && have_ && p.gain > 0) {
        // How far the camera still trails for the speed its target was led
        // by (the target it shows was set a frame ago) is lag the lead
        // doesn't make up yet - or, ahead of him, makes up too much.
        if (wasLeading_ && std::fabs(ledSpeed_) > double(p.minSpeed) && dt > 0) {
            const double behind = (marioUp + rest_) - cameraUp;
            const double missing = std::max(-0.3, std::min(0.3, behind / (ledSpeed_ * double(p.gain))));
            lag_ = std::max(0.0, std::min(double(p.maxLag), lag_ + double(p.learnRate) * missing));
        }
        target = std::max(-double(p.maxLead), std::min(double(p.maxLead), double(p.gain) * lag_ * speedUp));
    }
    wasLeading_ = airborne && have_ && p.gain > 0;
    ledSpeed_ = speedUp;
    if (dt > 0) {
        const double a = 1.0 - std::exp(-dt / std::max(0.005, double(p.smoothTau)));
        lead_ = float(lead_ + (target - lead_) * a);
    } else if (!airborne && !settling) {
        lead_ = 0;
    }
    if (std::fabs(lead_) < 1e-4f) lead_ = 0;
    return lead_;
}

} // namespace sm2m
