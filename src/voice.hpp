// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 Alejandro Bustos
#pragma once
#include "pattern.hpp"
#include <cmath>
#include <cstddef>
#include <cstdint>

// One voice (docs/design.md, "One voice"): a loop buffer y written every
// sample with the input plus the filtered loop,
//     y[t] = x[t] + fb * tone(y[t - L])
// and read at every hit,
//     v[t] = sum over hits k of y[t - D_k]   minus   x[t] if step 0 is a hit.
// Its Voice n switch fades it (docs/design.md, "Choosing voices"); a change
// of its taps crossfades from the old ones to the new (docs/design.md,
// "Changes while playing"; docs/implementation-plan.md, section 3.2).
namespace euclidelay {

// The Voice n switch's fade, in seconds (tune).
inline constexpr double kSwitchSeconds = 0.020;
// The crossfade between old and new taps, in seconds (tune).
inline constexpr double kFadeSeconds = 0.030;
// How long Level, Pan, Feedback, Tone, Dry and Echoes glide, in seconds (tune).
inline constexpr double kGlideSeconds = 0.020;
// The longest run() in samples.
inline constexpr int kChunk = 256;

// The Tone filter's pole for Tone 0..1 at `rate`: 0 at Tone 1 (the loop
// untouched), about 1.5 kHz at Tone 0 (tune).
float tone_pole(float tone, double rate);

// A control gliding to its newest value in a straight line, `length`
// samples from wherever it is. next() gives each sample's value; it reaches
// the target exactly.
struct Glide {
    float value = 0, target = 0, step = 0;
    int left = 0;
    void reset(float v) {
        value = target = v;
        step = 0;
        left = 0;
    }
    void set(float t, int length) {
        if (t == target) return;
        target = t;
        left = length;
        step = (t - value) / static_cast<float>(length);
    }
    float next() {
        if (left > 0) {
            if (--left == 0) value = target;
            else value += step;
        }
        return value;
    }
};

// Switching. A voice that is off is skipped: it reads and writes nothing.
// Coming on, it restarts empty: `fresh_` counts the samples written since,
// and any read further back than that is silence, which stands in for
// clearing a 6 MB buffer in run(). Its input fades in, so a note already
// sounding doesn't enter the loop with a click. Going off, its output fades
// out and it then stops. Coming on again during that fade, the old echoes
// (everything older than the restart) carry on fading out while the new
// input fades in; a third life during that leaves only the newest two.
//
// Changing taps. New taps fade in over kFadeSeconds while the old fade out:
// both loop reads and both sets of taps are read for that time. A change
// during a fade waits as `pending_` and fades in when the fade ends, so a
// knob dragged through many values costs at most two fades in a row.
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
    // On, or still fading out: run() must be called for every sample.
    bool active() const { return on_ || out_ramp_ > 0 || old_ramp_ > 0; }
    bool on() const { return on_; }

    std::size_t size() const { return size_; }
    const float* data() const { return buffer_; }

    // Settings taking effect at once, with no crossfade or glide: for a
    // voice coming on, or at activate. Every read (`taps.loop` and every
    // delay) must be below size() - kChunk.
    void start(const Taps& taps, float feedback);
    // New settings while playing: Feedback glides, new taps crossfade in.
    void set(const Taps& taps, float feedback);
    // The newest taps set, whether playing, fading in or pending.
    const Taps& taps() const { return pending_set_ ? pending_ : next_; }
    // Crossfades started since allocate (for the tests).
    uint64_t fades() const { return fades_; }

    // `n` samples (at most kChunk) of input `x` in, the voice's echoes out
    // into `v`; `pole` is the Tone filter's pole for each sample. Only while
    // active(); if the voice stops during the run, the rest of `v` is 0 and
    // nothing more is written.
    //
    // A run writes its samples of y first, one by one (each needs the loop
    // read before it), then adds up each tap over the whole run: every
    // tap's reads are in a row in the buffer, which keeps memory reads in
    // order and lets the sums vectorise. Taps read only samples already
    // written, since every D_k is above 0. The run is split where a
    // crossfade ends, so its taps are fixed for each part.
    void run(const float* x, const float* pole, float* v, int n);

private:
    long wrap(long i) const { return i < 0 ? i + static_cast<long>(size_) : i; }
    float gain(int ramp) const { return ramp >= fade_ ? 1.0f : static_cast<float>(ramp) * inv_fade_; }
    // Part of a run with fixed taps; returns false if the voice stopped.
    bool part(const float* x, const float* pole, float* v, int n);
    // Adds y[w0 + i - d] to sum[i] for i in [0, n).
    void add_run(float* sum, long w0, long d, int n) const;

    float* buffer_ = nullptr;
    std::size_t size_ = 0;
    long write_ = 0;
    float tone_ = 0, old_tone_ = 0;
    Glide feedback_;
    int glide_ = 1;  // the glide in samples

    // Taps: cur_ fading to next_ while fade_left_ > 0; equal otherwise.
    Taps cur_, next_, pending_;
    bool pending_set_ = false;
    int fade_left_ = 0, fade_len_ = 1;
    float inv_fade_len_ = 1;
    uint64_t fades_ = 0;

    bool on_ = true;
    long fresh_ = 0;    // samples written since it came on, up to size_
    long old_age_ = 0;  // the last life's fresh_ when it came on again
    int fade_ = 1;      // the switch fade in samples
    float inv_fade_ = 1;
    int in_ramp_ = 1, out_ramp_ = 1, old_ramp_ = 0;  // 0..fade_

    // Per sample of a part, from writing y to adding up the taps.
    long age_[kChunk];                  // fresh_ before the sample
    float a_[kChunk];                   // the crossfade, 0 old .. 1 new
    float out_gain_[kChunk], old_gain_[kChunk];
    float now_cur_[kChunk], now_next_[kChunk];  // this life, old and new taps
    float old_cur_[kChunk], old_next_[kChunk];  // the last life's
};

} // namespace euclidelay
