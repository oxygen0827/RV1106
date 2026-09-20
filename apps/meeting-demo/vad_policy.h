#pragma once

#include <algorithm>

struct VadDecision {
    int threshold;
    bool voice;
    bool calibrating;
};

// Learn the live microphone floor before admitting speech. This prevents the
// RV1106 codec startup transient from being uploaded as several seconds of
// speech on constrained Wi-Fi links.
class AdaptiveVadGate {
public:
    AdaptiveVadGate(int configured_threshold, int calibration_frames)
        : configured_threshold_(configured_threshold),
          calibration_remaining_(configured_threshold > 0 ? 0 : calibration_frames) {}

    VadDecision process(int rms) {
        if (configured_threshold_ > 0) {
            return VadDecision{configured_threshold_, rms >= configured_threshold_, false};
        }

        if (!initialized_) {
            noise_floor_ = std::max(1, rms);
            initialized_ = true;
        }
        if (calibration_remaining_ > 0) {
            noise_floor_ = 0.9 * noise_floor_ + 0.1 * rms;
            --calibration_remaining_;
            return VadDecision{adaptive_threshold(), false, true};
        }

        const bool loud = rms > noise_floor_ * 2.5 && rms > 140;
        if (!loud) noise_floor_ = 0.9 * noise_floor_ + 0.1 * rms;
        const int threshold = adaptive_threshold();
        return VadDecision{threshold, rms >= threshold, false};
    }

    bool calibrated() const { return calibration_remaining_ == 0; }
    double noise_floor() const { return noise_floor_; }

private:
    int adaptive_threshold() const {
        return static_cast<int>(std::max(140.0, 2.5 * noise_floor_));
    }

    int configured_threshold_ = 0;
    int calibration_remaining_ = 0;
    bool initialized_ = false;
    double noise_floor_ = 100.0;
};
