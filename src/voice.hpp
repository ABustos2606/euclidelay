// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include "pattern.hpp"
#include <cmath>
#include <cstddef>

// One voice (docs/design.md, "One voice"): a loop buffer y written every
// sample with the input plus the filtered loop,
//     y[t] = x[t] + fb * tone(y[t - L])
// and read at every hit,
//     v[t] = sum over hits k of y[t - D_k]   minus   x[t] if step 0 is a hit.
// Its Voice n switch fades it (docs/design.md, "Choosing voices"). Settings
// take effect at once, with no crossfade yet (milestone 3).
namespace euclidelay {

// The Voice n switch's fade, in seconds (tune).
inline constexpr double kSwitchSeconds = 0.020;

// The Tone filter's pole for Tone 0..1 at `rate`: 0 at Tone 1 (the loop
// untouched), about 1.5 kHz at Tone 0 (tune).
float tone_pole(float tone, double rate);

// Switching. A voice that is off is skipped: it reads and writes nothing.
// Coming on, it restarts empty: `fresh_` counts the samples written since,
// and any read further back than that is silence, which stands in for
// clearing a 6 MB buffer in run(). Its input fades in, so a note already
// sounding doesn't enter the loop with a click. Going off, its output fades
// out and it then stops. Coming on again during that fade, the old echoes
// (everything older than the restart) carry on fading out while the new
// input fades in; a third life during that leaves only the newest two.
class Voice {
public:
    Voice() = default;
    Voice(const Voice&) = delete;
    Voice& operator=(const Voice&) = delete;
    ~Voice();

    // Allocates the loop buffer for `seconds` of the longest loop at `rate`
    // and leaves the voice as clear() does. False if the memory isn't there.
    // Never called from run().
    bool allocate(double seconds, double rate);
    // Empties the loop and the filter and leaves the voice on, with no fade.
    void clear();
    // Off at once, with no fade (activate, for a voice that starts off).
    void stop() {
        on_ = false;
        out_ramp_ = old_ramp_ = 0;
    }
    // Everything in the loop reads as silence from now on, with no fade out;
    // the input fades in again. The non-finite guard.
    void empty() {
        fresh_ = 0;
        old_ramp_ = 0;
        in_ramp_ = 0;
        tone_ = 0.0f;
    }
    // The Voice n switch: fades in or out as above. Nothing if unchanged.
    void switch_to(bool on);
    // On, or still fading out: process() must be called every sample.
    bool active() const { return on_ || out_ramp_ > 0 || old_ramp_ > 0; }
    bool on() const { return on_; }

    std::size_t size() const { return size_; }
    const float* data() const { return buffer_; }

    // New settings, taking effect from the next sample. `taps.loop` and every
    // delay must be below size().
    void set(const Taps& taps, float feedback, float pole) {
        taps_ = taps;
        feedback_ = feedback;
        pole_ = pole;
    }
    const Taps& taps() const { return taps_; }

    // One sample of input in, one sample of the voice's echoes out. Only
    // while active().
    float process(float x) {
        const long age = fresh_;  // samples written in this life, before this one
        x *= gain(in_ramp_);
        const float back = taps_.loop <= age ? buffer_[wrap(write_ - taps_.loop)] : 0.0f;
        // One-pole low-pass: exactly `back` when the pole is 0. Kept out of
        // the denormals as the loop dies away.
        tone_ = back + pole_ * (tone_ - back);
        if (std::fabs(tone_) < 1e-20f) tone_ = 0.0f;
        const float looped = feedback_ * tone_;
        const float y = x + looped;
        buffer_[write_] = y;
        // Step 0 reads y[t] itself, x[t] + looped; without the note, that
        // is `looped`.
        float now = taps_.zero ? looped : 0.0f, before = 0.0f;
        for (int k = 0; k < taps_.count; ++k) {
            const long d = taps_.delay[k];
            const float s = buffer_[wrap(write_ - d)];
            if (d <= age) now += s;
            else if (d <= age + old_age_) before += s;  // the last life's echoes
        }
        float v = gain(out_ramp_) * now;
        if (old_ramp_ > 0) {
            // The last life's step 0 is its own loop read, filtered on.
            const long loop = taps_.loop;
            if (taps_.zero) {
                const float old_back = loop > age && loop <= age + old_age_ ? buffer_[wrap(write_ - loop)] : 0.0f;
                old_tone_ = old_back + pole_ * (old_tone_ - old_back);
                before += feedback_ * old_tone_;
            }
            v += gain(old_ramp_) * before;
        }
        // The non-finite guard (docs/design.md, "Host limits"): empty the
        // voice, silent until new input arrives.
        if (!std::isfinite(y) || !std::isfinite(v)) {
            buffer_[write_] = 0.0f;
            empty();
            v = 0.0f;
        }
        if (++write_ == static_cast<long>(size_)) write_ = 0;
        if (fresh_ < static_cast<long>(size_)) ++fresh_;
        if (in_ramp_ < fade_) ++in_ramp_;
        if (!on_ && out_ramp_ > 0) --out_ramp_;
        if (old_ramp_ > 0) --old_ramp_;
        return v;
    }

private:
    long wrap(long i) const { return i < 0 ? i + static_cast<long>(size_) : i; }
    float gain(int ramp) const { return ramp >= fade_ ? 1.0f : static_cast<float>(ramp) * inv_fade_; }

    float* buffer_ = nullptr;
    std::size_t size_ = 0;
    long write_ = 0;
    Taps taps_;
    float feedback_ = 0, pole_ = 0, tone_ = 0, old_tone_ = 0;
    bool on_ = true;
    long fresh_ = 0;    // samples written since it came on, up to size_
    long old_age_ = 0;  // the last life's fresh_ when it came on again
    int fade_ = 1;      // the switch fade in samples
    float inv_fade_ = 1;
    int in_ramp_ = 1, out_ramp_ = 1, old_ramp_ = 0;  // 0..fade_
};

} // namespace euclidelay
