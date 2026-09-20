#include "../vad_policy.h"

#include <cassert>
#include <cstdio>

int main() {
    AdaptiveVadGate adaptive(0, 50);

    // The board starts with a roughly 500-RMS codec transient, then settles.
    // Calibration must never publish that transient as speech.
    for (int i = 0; i < 30; ++i) {
        VadDecision d = adaptive.process(500);
        assert(d.calibrating);
        assert(!d.voice);
    }
    for (int i = 0; i < 20; ++i) {
        VadDecision d = adaptive.process(20);
        assert(d.calibrating);
        assert(!d.voice);
    }
    assert(adaptive.calibrated());

    VadDecision quiet = adaptive.process(20);
    assert(!quiet.calibrating);
    assert(!quiet.voice);
    assert(quiet.threshold < 500);

    VadDecision speech = adaptive.process(800);
    assert(speech.voice);

    AdaptiveVadGate fixed(700, 50);
    assert(!fixed.process(699).voice);
    assert(fixed.process(700).voice);

    std::puts("vad-policy-test: PASS");
    return 0;
}
