// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include <cmath>
#include <cstdint>

// Euclidean patterns and the times they give, in samples (docs/design.md,
// "Patterns" and "Time"). Written from the spec's three rules; the same
// numbers give the same pattern as Peepo's Euclidean module, which
// tests/test_pattern.cpp checks against every entry Peepo draws.
namespace euclidelay {

inline constexpr int kMaxSteps = 16;

// The pattern for Steps, Hits and Shift as a mask: bit j set when step j is
// a hit, step 0 being the moment you play. Steps is clamped to 1..16, Hits
// to 0..Steps, and Shift is taken modulo Steps.
inline uint32_t pattern(int steps, int hits, int shift) {
    const int n = steps < 1 ? 1 : steps > kMaxSteps ? kMaxSteps : steps;
    const int k = hits < 0 ? 0 : hits > n ? n : hits;
    const int r = (shift % n + n) % n;
    uint32_t mask = 0;
    int bucket = 0;
    for (int i = 0; i < n; ++i) {
        bucket += k;
        if (bucket >= n) {
            bucket -= n;
            mask |= 1u << ((n - 1 - i + r) % n);
        }
    }
    return mask;
}

// The length of one step in samples: Step (an index into kStepBeats) in
// beats, at BPM, at the sample rate. A double, so long loops at odd tempos
// land on the right sample.
inline double step_samples(double beats, double bpm, double rate) { return beats * 60.0 / bpm * rate; }

// One voice's settings in samples.
struct Taps {
    int loop = 1;                // L = round(N * S)
    int count = 0;               // hits other than step 0
    int delay[kMaxSteps] = {};   // their D_k = round(k * S), ascending, all above 0
    bool zero = false;           // step 0 is a hit
    bool operator==(const Taps& o) const {
        if (loop != o.loop || count != o.count || zero != o.zero) return false;
        for (int i = 0; i < count; ++i)
            if (delay[i] != o.delay[i]) return false;
        return true;
    }
    bool operator!=(const Taps& o) const { return !(*this == o); }
};

// The taps for a pattern of `steps` steps, `step` samples apart. Each time is
// rounded on its own from k * S, so pass m is at most m / 2 samples off.
inline Taps make_taps(uint32_t mask, int steps, double step) {
    const int n = steps < 1 ? 1 : steps > kMaxSteps ? kMaxSteps : steps;
    Taps t;
    t.loop = static_cast<int>(std::llround(n * step));
    if (t.loop < 1) t.loop = 1;
    t.zero = mask & 1u;
    for (int k = 1; k < n; ++k)
        if (mask & (1u << k)) t.delay[t.count++] = static_cast<int>(std::llround(k * step));
    return t;
}

} // namespace euclidelay
