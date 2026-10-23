// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include "pattern.hpp"
#include <cstddef>

// One voice (docs/design.md, "One voice"): a loop buffer y written every
// sample with the input plus the filtered loop,
//     y[t] = x[t] + fb * tone(y[t - L])
// and read at every hit,
//     v[t] = sum over hits k of y[t - D_k]   minus   x[t] if step 0 is a hit.
// Milestone 1: settings take effect at once, with no crossfade.
namespace euclidelay {

// The Tone filter's pole for Tone 0..1 at `rate`: 0 at Tone 1 (the loop
// untouched), about 1.5 kHz at Tone 0 (tune).
float tone_pole(float tone, double rate);

class Voice {
public:
    Voice() = default;
    Voice(const Voice&) = delete;
    Voice& operator=(const Voice&) = delete;
    ~Voice();

    // Allocates the loop buffer for `seconds` of the longest loop at `rate`.
    // False if the memory isn't there. Never called from run().
    bool allocate(double seconds, double rate);
    // Empties the loop and the filter.
    void clear();
    std::size_t size() const { return size_; }

    // New settings, taking effect from the next sample. `taps.loop` and every
    // delay must be below size().
    void set(const Taps& taps, float feedback, float pole) {
        taps_ = taps;
        feedback_ = feedback;
        pole_ = pole;
    }
    const Taps& taps() const { return taps_; }

    // One sample of input in, one sample of the voice's echoes out.
    float process(float x) {
        const float back = buffer_[wrap(write_ - taps_.loop)];
        // One-pole low-pass: exactly `back` when the pole is 0.
        tone_ = back + pole_ * (tone_ - back);
        const float looped = feedback_ * tone_;
        buffer_[write_] = x + looped;
        // Step 0 reads y[t] itself, x[t] + looped; without the note, that
        // is `looped`.
        float v = taps_.zero ? looped : 0.0f;
        for (int k = 0; k < taps_.count; ++k) v += buffer_[wrap(write_ - taps_.delay[k])];
        if (++write_ == static_cast<long>(size_)) write_ = 0;
        return v;
    }

private:
    long wrap(long i) const { return i < 0 ? i + static_cast<long>(size_) : i; }

    float* buffer_ = nullptr;
    std::size_t size_ = 0;
    long write_ = 0;
    Taps taps_;
    float feedback_ = 0, pole_ = 0, tone_ = 0;
};

} // namespace euclidelay
